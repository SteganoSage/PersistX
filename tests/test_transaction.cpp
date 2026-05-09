#include "disk_manager.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "transaction_manager.hpp"
#include "page.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

static const std::string TEST_DB = "storage/test_transaction.db";
static const std::string TEST_WAL = "storage/test_transaction.wal";

static void cleanup()
{
    std::filesystem::remove(TEST_DB);
    std::filesystem::remove(TEST_WAL);
}

int main()
{
    cleanup();

    // ── Test 1: begin → commit ────────────────────────────────────────────────
    // Insert a record inside a transaction, commit, then verify:
    //   - The record is still readable.
    //   - flushed_lsn >= the UPDATE record's LSN (commit forced a flush).
    {
        cleanup();
        DiskManager dm(TEST_DB);
        LogManager lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);

        Transaction *txn = tm.begin();
        assert(txn->get_state() == TxnState::GROWING);
        assert(txn->get_txn_id() != INVALID_TXN_ID);

        page_id_t pid;
        Page *page = bm.new_page(pid);
        assert(page != nullptr);

        uint8_t payload[] = {10, 20, 30};
        std::vector<uint8_t> old_data = {};
        std::vector<uint8_t> new_data(payload, payload + 3);

        slot_id_t sid = page->insert_record(payload, 3, 0);
        assert(sid != INVALID_SLOT_ID);

        lsn_t upd_lsn = tm.log_update(txn, pid, sid, old_data, new_data);
        page->set_page_lsn(upd_lsn);
        bm.unpin_page(pid, /*is_dirty=*/true);

        tm.commit(txn);

        // Record must still be readable after commit.
        Page *p2 = bm.fetch_page(pid);
        assert(p2 != nullptr);
        std::vector<uint8_t> out;
        assert(p2->read_record(sid, out));
        assert(out == new_data);
        bm.unpin_page(pid, false);

        // Commit must have forced the log to disk.
        assert(lm.get_flushed_lsn() != INVALID_LSN);
        assert(lm.get_flushed_lsn() >= upd_lsn);

        std::cout << "Test 1 PASSED: begin + commit, record readable, log flushed\n";
    }

    // ── Test 2: begin → abort (before-image restored) ────────────────────────
    // 1. Insert {0xAA, 0xBB} outside any transaction.
    // 2. Begin txn, overwrite with {0xFF, 0xEE}, log the change.
    // 3. Abort — slot must revert to {0xAA, 0xBB}.
    {
        cleanup();
        DiskManager dm(TEST_DB);
        LogManager lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);

        page_id_t pid;
        Page *page = bm.new_page(pid);
        assert(page != nullptr);

        uint8_t initial[] = {0xAA, 0xBB};
        slot_id_t sid = page->insert_record(initial, 2, 0);
        assert(sid != INVALID_SLOT_ID);
        bm.unpin_page(pid, /*is_dirty=*/true);

        Transaction *txn = tm.begin();

        Page *p2 = bm.fetch_page(pid);
        assert(p2 != nullptr);

        std::vector<uint8_t> before_img;
        assert(p2->read_record(sid, before_img));
        assert(before_img == std::vector<uint8_t>({0xAA, 0xBB}));

        uint8_t after_raw[] = {0xFF, 0xEE};
        std::vector<uint8_t> after_img(after_raw, after_raw + 2);
        assert(p2->update_record(sid, after_raw, 2, 0));

        lsn_t upd_lsn = tm.log_update(txn, pid, sid, before_img, after_img);
        p2->set_page_lsn(upd_lsn);
        bm.unpin_page(pid, /*is_dirty=*/true);

        tm.abort(txn);

        Page *p3 = bm.fetch_page(pid);
        assert(p3 != nullptr);
        std::vector<uint8_t> after_abort;
        assert(p3->read_record(sid, after_abort));
        assert(after_abort == before_img); // abort restored the before-image
        bm.unpin_page(pid, false);

        std::cout << "Test 2 PASSED: abort restores before-image\n";
    }

    // ── Test 3: abort with multiple updates (full undo chain walk) ────────────
    // Ensures abort walks the entire prevLSN chain, not just one step.
    {
        cleanup();
        DiskManager dm(TEST_DB);
        LogManager lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);

        page_id_t pid;
        Page *p0 = bm.new_page(pid);
        uint8_t v0[] = {1}, v1[] = {2}, v2[] = {3};
        slot_id_t s0 = p0->insert_record(v0, 1, 0);
        slot_id_t s1 = p0->insert_record(v1, 1, 0);
        slot_id_t s2 = p0->insert_record(v2, 1, 0);
        bm.unpin_page(pid, true);

        Transaction *txn = tm.begin();

        auto do_update = [&](slot_id_t slot, uint8_t new_val)
        {
            Page *pg = bm.fetch_page(pid);
            std::vector<uint8_t> old_d;
            pg->read_record(slot, old_d);
            std::vector<uint8_t> new_d = {new_val};
            pg->update_record(slot, &new_val, 1, 0);
            lsn_t lsn = tm.log_update(txn, pid, slot, old_d, new_d);
            pg->set_page_lsn(lsn);
            bm.unpin_page(pid, true);
        };

        do_update(s0, 10);
        do_update(s1, 20);
        do_update(s2, 30);

        tm.abort(txn);

        // All three slots must be back to their original values.
        Page *pg = bm.fetch_page(pid);
        std::vector<uint8_t> out;

        pg->read_record(s0, out);
        assert(out == std::vector<uint8_t>{1});
        pg->read_record(s1, out);
        assert(out == std::vector<uint8_t>{2});
        pg->read_record(s2, out);
        assert(out == std::vector<uint8_t>{3});

        bm.unpin_page(pid, false);

        std::cout << "Test 3 PASSED: abort walks full undo chain (3 updates)\n";
    }

    // ── Test 4: WAL enforcement on eviction ───────────────────────────────────
    // 1-frame pool forces eviction on every new_page().
    // BufferManager must flush the log before writing the dirty frame to disk.
    {
        cleanup();
        DiskManager dm(TEST_DB);
        LogManager lm(TEST_WAL);
        BufferManager bm(&dm, &lm, /*pool_size=*/1); // single frame
        TransactionManager tm(&lm, &bm);

        Transaction *txn = tm.begin();

        page_id_t pa;
        Page *pageA = bm.new_page(pa);
        assert(pageA != nullptr);

        uint8_t raw[] = {42};
        slot_id_t sa = pageA->insert_record(raw, 1, 0);
        assert(sa != INVALID_SLOT_ID);

        std::vector<uint8_t> old_d = {}, new_d = {42};
        lsn_t upd_lsn = tm.log_update(txn, pa, sa, old_d, new_d);
        pageA->set_page_lsn(upd_lsn);
        bm.unpin_page(pa, /*is_dirty=*/true);

        // Nothing flushed yet — log is purely in memory.
        assert(lm.get_flushed_lsn() == INVALID_LSN);

        // Allocating pageB evicts pageA; enforce_wal() must flush before write.
        page_id_t pb;
        Page *pageB = bm.new_page(pb);
        assert(pageB != nullptr);
        bm.unpin_page(pb, false);

        // WAL must now be on disk, at least up to the page's LSN.
        assert(lm.get_flushed_lsn() != INVALID_LSN);
        assert(lm.get_flushed_lsn() >= upd_lsn);

        tm.abort(txn);

        std::cout << "Test 4 PASSED: WAL flushed before dirty page evicted\n";
    }

    // ── Test 5: log_update returns correct LSN and threads prevLSN chain ──────
    {
        cleanup();
        DiskManager dm(TEST_DB);
        LogManager lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);

        Transaction *txn = tm.begin();
        lsn_t begin_lsn = txn->get_prev_lsn();

        std::vector<uint8_t> od, nd = {1};
        lsn_t lsn1 = tm.log_update(txn, 0, 0, od, nd);
        assert(txn->get_prev_lsn() == lsn1);
        assert(lsn1 > begin_lsn);

        lsn_t lsn2 = tm.log_update(txn, 0, 1, od, nd);
        assert(txn->get_prev_lsn() == lsn2);
        assert(lsn2 > lsn1);

        tm.commit(txn);

        std::cout << "Test 5 PASSED: log_update returns increasing LSNs, threads prevLSN\n";
    }

    cleanup();
    std::cout << "\nAll TransactionManager tests passed.\n";
    return 0;
}