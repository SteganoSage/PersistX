#include "disk_manager.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "transaction_manager.hpp"
#include "checkpoint_manager.hpp"
#include "recovery_manager.hpp"
#include "page.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

static const std::string TEST_DB  = "storage/test_recovery.db";
static const std::string TEST_WAL = "storage/test_recovery.wal";

static void cleanup() {
    std::filesystem::remove(TEST_DB);
    std::filesystem::remove(TEST_WAL);
}

int main() {

    // ── Test 1: Recovery on empty log — should be a no-op ────────────────────
    {
        cleanup();
        DiskManager  dm(TEST_DB);
        LogManager   lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        RecoveryManager rm(&lm, &bm);

        rm.recover();  // nothing to do — should not crash

        std::cout << "Test 1 PASSED: recovery on empty log is a no-op\n";
    }

    // ── Test 2: Committed transaction data survives crash (redo) ─────────────
    // Write data, commit, simulate crash (destroy everything), recover, verify.
    {
        cleanup();
        page_id_t pid;
        slot_id_t sid;

        // --- Phase A: normal operation + commit ---
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            TransactionManager tm(&lm, &bm);

            Transaction* txn = tm.begin();

            Page* page = bm.new_page(pid);
            assert(page != nullptr);

            uint8_t payload[] = {0xDE, 0xAD};
            sid = page->insert_record(payload, 2, 0);
            assert(sid != INVALID_SLOT_ID);

            std::vector<uint8_t> old_d = {}, new_d = {0xDE, 0xAD};
            lsn_t lsn = tm.log_update(txn, pid, sid, old_d, new_d);
            page->set_page_lsn(lsn);
            bm.unpin_page(pid, true);

            tm.commit(txn);

            // Do NOT flush pages — simulate crash where committed data
            // is only in the WAL, not on the data page on disk.
            // (The log was flushed by commit, but the page wasn't.)
        }
        // --- everything destroyed = simulated crash ---

        // --- Phase B: recovery ---
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            RecoveryManager rm(&lm, &bm);

            rm.recover();

            // The committed data should be on the page now (redo applied it).
            Page* page = bm.fetch_page(pid);
            assert(page != nullptr);

            std::vector<uint8_t> out;
            bool ok = page->read_record(sid, out);
            assert(ok);
            assert(out.size() == 2);
            assert(out[0] == 0xDE && out[1] == 0xAD);

            bm.unpin_page(pid, false);
        }

        std::cout << "Test 2 PASSED: committed data survives crash (redo)\n";
    }

    // ── Test 3: Uncommitted transaction is rolled back (undo) ────────────────
    // Write data WITHOUT committing, simulate crash, recover, verify rollback.
    {
        cleanup();
        page_id_t pid;
        slot_id_t sid;

        // --- Phase A: write data but don't commit ---
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            TransactionManager tm(&lm, &bm);

            Transaction* txn = tm.begin();

            Page* page = bm.new_page(pid);
            assert(page != nullptr);

            uint8_t payload[] = {0xBE, 0xEF};
            sid = page->insert_record(payload, 2, 0);

            std::vector<uint8_t> old_d = {}, new_d = {0xBE, 0xEF};
            lsn_t lsn = tm.log_update(txn, pid, sid, old_d, new_d);
            page->set_page_lsn(lsn);
            bm.unpin_page(pid, true);

            // Flush the page to disk (steal policy — dirty page on disk
            // from uncommitted txn). Flush the log too so records are durable.
            bm.flush_all_pages();
            lm.flush_all();

            // DO NOT commit — simulate crash with dirty data on disk.
        }

        // --- Phase B: recovery should undo the uncommitted data ---
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            RecoveryManager rm(&lm, &bm);

            rm.recover();

            // The uncommitted insert should be rolled back (slot is a tombstone).
            Page* page = bm.fetch_page(pid);
            assert(page != nullptr);

            std::vector<uint8_t> out;
            bool ok = page->read_record(sid, out);
            assert(!ok);  // should fail — record was undone (deleted)

            bm.unpin_page(pid, false);
        }

        std::cout << "Test 3 PASSED: uncommitted data rolled back (undo)\n";
    }

    // ── Test 4: Recovery with checkpoint ─────────────────────────────────────
    // Commit T1, start T2 (don't commit), checkpoint, crash, recover.
    // T1's data should survive, T2's data should be rolled back.
    {
        cleanup();
        page_id_t pid1, pid2;
        slot_id_t sid1, sid2;

        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            TransactionManager tm(&lm, &bm);
            CheckpointManager  cm(&lm, &bm, &tm);

            // T1: committed
            Transaction* txn1 = tm.begin();
            Page* p1 = bm.new_page(pid1);
            uint8_t d1[] = {0x11, 0x22};
            sid1 = p1->insert_record(d1, 2, 0);
            lsn_t lsn1 = tm.log_update(txn1, pid1, sid1, {}, {0x11, 0x22});
            p1->set_page_lsn(lsn1);
            bm.unpin_page(pid1, true);
            tm.commit(txn1);

            // T2: NOT committed
            Transaction* txn2 = tm.begin();
            Page* p2 = bm.new_page(pid2);
            uint8_t d2[] = {0x33, 0x44};
            sid2 = p2->insert_record(d2, 2, 0);
            lsn_t lsn2 = tm.log_update(txn2, pid2, sid2, {}, {0x33, 0x44});
            p2->set_page_lsn(lsn2);
            bm.unpin_page(pid2, true);

            // Checkpoint captures T2 as active.
            cm.checkpoint();
            lm.flush_all();

            // Crash — T2 never committed.
        }

        // Recovery
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            RecoveryManager rm(&lm, &bm);

            rm.recover();

            // T1's data should be there (committed before checkpoint).
            Page* p1 = bm.fetch_page(pid1);
            assert(p1 != nullptr);
            std::vector<uint8_t> out1;
            assert(p1->read_record(sid1, out1));
            assert(out1[0] == 0x11 && out1[1] == 0x22);
            bm.unpin_page(pid1, false);

            // T2's data should be gone (uncommitted → undone).
            Page* p2 = bm.fetch_page(pid2);
            assert(p2 != nullptr);
            std::vector<uint8_t> out2;
            assert(!p2->read_record(sid2, out2));
            bm.unpin_page(pid2, false);
        }

        std::cout << "Test 4 PASSED: recovery with checkpoint (redo T1, undo T2)\n";
    }

    // ── Test 5: Recovery after partial abort ─────────────────────────────────
    // T1 makes 2 updates, abort starts undoing but only undoes 1, then crash.
    // Recovery should finish the undo (the CLR from the partial abort tells
    // recovery to skip the already-undone update).
    {
        cleanup();
        page_id_t pid;
        slot_id_t sid;

        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            TransactionManager tm(&lm, &bm);

            Transaction* txn = tm.begin();

            // Insert initial data.
            Page* page = bm.new_page(pid);
            uint8_t init[] = {0xAA};
            sid = page->insert_record(init, 1, 0);
            lsn_t lsn0 = tm.log_update(txn, pid, sid, {}, {0xAA});
            page->set_page_lsn(lsn0);

            // Update #1: 0xAA → 0xBB
            std::vector<uint8_t> old1 = {0xAA}, new1 = {0xBB};
            page->update_record(sid, new1.data(), 1, 0);
            lsn_t lsn1 = tm.log_update(txn, pid, sid, old1, new1);
            page->set_page_lsn(lsn1);

            // Update #2: 0xBB → 0xCC
            std::vector<uint8_t> old2 = {0xBB}, new2 = {0xCC};
            page->update_record(sid, new2.data(), 1, 0);
            lsn_t lsn2 = tm.log_update(txn, pid, sid, old2, new2);
            page->set_page_lsn(lsn2);

            bm.unpin_page(pid, true);

            // Manually simulate partial abort:
            // Write ABORT record, undo Update#2 (write CLR), then crash.
            lsn_t undo_start = txn->get_prev_lsn();
            LogRecord abort_rec(txn->get_txn_id(), undo_start, LogRecordType::ABORT);
            lsn_t abort_lsn = lm.append(abort_rec);

            // Undo Update#2: read it, restore 0xBB, write CLR.
            LogRecord rec2 = lm.read_record_at_lsn(lsn2);
            Page* pg = bm.fetch_page(pid);
            uint8_t restore[] = {0xBB};
            pg->update_record(sid, restore, 1, 0);

            // CLR for Update#2 — undo_next points to Update#1 (lsn1)
            LogRecord clr2(txn->get_txn_id(), abort_lsn,
                           pid, sid, {}, {0xBB}, lsn1);
            lsn_t clr2_lsn = lm.append(clr2);
            pg->set_page_lsn(clr2_lsn);
            bm.unpin_page(pid, true);

            // Flush everything to disk, then "crash."
            bm.flush_all_pages();
            lm.flush_all();
            // Update#1 and the original insert are NOT yet undone.
        }

        // Recovery should finish the undo: undo Update#1, undo insert.
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            RecoveryManager rm(&lm, &bm);

            rm.recover();

            // All of T1's data should be fully rolled back.
            Page* page = bm.fetch_page(pid);
            assert(page != nullptr);

            std::vector<uint8_t> out;
            // The slot should be tombstoned (insert was undone).
            assert(!page->read_record(sid, out));

            bm.unpin_page(pid, false);
        }

        std::cout << "Test 5 PASSED: recovery completes partial abort\n";
    }

    cleanup();
    std::cout << "\nAll RecoveryManager tests passed.\n";
    return 0;
}
