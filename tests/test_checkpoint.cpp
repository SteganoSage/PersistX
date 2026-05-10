#include "disk_manager.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "transaction_manager.hpp"
#include "checkpoint_manager.hpp"
#include "page.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

static const std::string TEST_DB  = "storage/test_checkpoint.db";
static const std::string TEST_WAL = "storage/test_checkpoint.wal";

static void cleanup() {
    std::filesystem::remove(TEST_DB);
    std::filesystem::remove(TEST_WAL);
}

// ─── helper: deserialize ATT from old_data of CHECKPOINT_END ─────────────────
static std::vector<std::pair<txn_id_t, lsn_t>>
deserialize_att(const std::vector<uint8_t>& data) {
    std::vector<std::pair<txn_id_t, lsn_t>> att;
    uint32_t offset = 0;

    uint32_t count;
    std::memcpy(&count, &data[offset], sizeof(count));
    offset += sizeof(count);

    for (uint32_t i = 0; i < count; ++i) {
        txn_id_t tid;
        lsn_t    lsn;
        std::memcpy(&tid, &data[offset], sizeof(tid));
        offset += sizeof(tid);
        std::memcpy(&lsn, &data[offset], sizeof(lsn));
        offset += sizeof(lsn);
        att.emplace_back(tid, lsn);
    }
    return att;
}

// ─── helper: deserialize DPT from new_data of CHECKPOINT_END ─────────────────
static std::vector<std::pair<page_id_t, lsn_t>>
deserialize_dpt(const std::vector<uint8_t>& data) {
    std::vector<std::pair<page_id_t, lsn_t>> dpt;
    uint32_t offset = 0;

    uint32_t count;
    std::memcpy(&count, &data[offset], sizeof(count));
    offset += sizeof(count);

    for (uint32_t i = 0; i < count; ++i) {
        page_id_t pid;
        lsn_t     lsn;
        std::memcpy(&pid, &data[offset], sizeof(pid));
        offset += sizeof(pid);
        std::memcpy(&lsn, &data[offset], sizeof(lsn));
        offset += sizeof(lsn);
        dpt.emplace_back(pid, lsn);
    }
    return dpt;
}

int main() {
    cleanup();

    // ── Test 1: Checkpoint with no active txns, no dirty pages ───────────────
    // Should produce CHECKPOINT_BEGIN + CHECKPOINT_END with empty ATT/DPT.
    {
        cleanup();
        DiskManager  dm(TEST_DB);
        LogManager   lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);
        CheckpointManager  cm(&lm, &bm, &tm);

        cm.checkpoint();

        auto records = lm.read_log();
        assert(records.size() == 2);
        assert(records[0].get_type() == LogRecordType::CHECKPOINT_BEGIN);
        assert(records[1].get_type() == LogRecordType::CHECKPOINT_END);

        // ATT should be empty (no in-flight transactions)
        auto att = deserialize_att(records[1].get_old_tuple_data());
        assert(att.empty());

        // DPT should be empty (no dirty pages)
        auto dpt = deserialize_dpt(records[1].get_new_tuple_data());
        assert(dpt.empty());

        std::cout << "Test 1 PASSED: checkpoint with empty ATT and DPT\n";
    }

    // ── Test 2: Checkpoint captures active transactions in ATT ───────────────
    // Start two transactions, checkpoint, verify both appear in the ATT.
    {
        cleanup();
        DiskManager  dm(TEST_DB);
        LogManager   lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);
        CheckpointManager  cm(&lm, &bm, &tm);

        Transaction* txn1 = tm.begin();
        Transaction* txn2 = tm.begin();
        txn_id_t id1 = txn1->get_txn_id();
        txn_id_t id2 = txn2->get_txn_id();

        cm.checkpoint();

        auto records = lm.read_log();
        // 2 BEGINs + CHECKPOINT_BEGIN + CHECKPOINT_END = 4 records
        assert(records.size() == 4);
        assert(records[2].get_type() == LogRecordType::CHECKPOINT_BEGIN);
        assert(records[3].get_type() == LogRecordType::CHECKPOINT_END);

        auto att = deserialize_att(records[3].get_old_tuple_data());
        assert(att.size() == 2);

        // Verify both transaction IDs are in the snapshot
        bool found1 = false, found2 = false;
        for (auto& [tid, lsn] : att) {
            if (tid == id1) found1 = true;
            if (tid == id2) found2 = true;
        }
        assert(found1 && found2);

        // Clean up transactions
        tm.abort(txn1);
        tm.abort(txn2);

        std::cout << "Test 2 PASSED: checkpoint captures active transactions in ATT\n";
    }

    // ── Test 3: Checkpoint captures dirty pages in DPT ───────────────────────
    // Create a page, dirty it, checkpoint, verify it appears in the DPT.
    {
        cleanup();
        DiskManager  dm(TEST_DB);
        LogManager   lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);
        CheckpointManager  cm(&lm, &bm, &tm);

        Transaction* txn = tm.begin();

        page_id_t pid;
        Page* page = bm.new_page(pid);
        assert(page != nullptr);

        uint8_t payload[] = {0xAA, 0xBB};
        slot_id_t sid = page->insert_record(payload, 2, 0);
        assert(sid != INVALID_SLOT_ID);

        std::vector<uint8_t> old_d = {}, new_d = {0xAA, 0xBB};
        lsn_t upd_lsn = tm.log_update(txn, pid, sid, old_d, new_d);
        page->set_page_lsn(upd_lsn);
        bm.unpin_page(pid, /*is_dirty=*/true);

        cm.checkpoint();

        auto records = lm.read_log();
        // Find the CHECKPOINT_END record (last one)
        LogRecord& ckpt_end = records.back();
        assert(ckpt_end.get_type() == LogRecordType::CHECKPOINT_END);

        auto dpt = deserialize_dpt(ckpt_end.get_new_tuple_data());
        assert(dpt.size() == 1);
        assert(dpt[0].first == pid);

        tm.commit(txn);

        std::cout << "Test 3 PASSED: checkpoint captures dirty pages in DPT\n";
    }

    // ── Test 4: Checkpoint flushes log to stable storage ─────────────────────
    // After checkpoint(), the log must be on disk (flushed_lsn >= CHECKPOINT_END LSN).
    {
        cleanup();
        DiskManager  dm(TEST_DB);
        LogManager   lm(TEST_WAL);
        BufferManager bm(&dm, &lm, 10);
        TransactionManager tm(&lm, &bm);
        CheckpointManager  cm(&lm, &bm, &tm);

        // Before checkpoint, nothing flushed.
        assert(lm.get_flushed_lsn() == INVALID_LSN);

        cm.checkpoint();

        // After checkpoint, log must be flushed.
        assert(lm.get_flushed_lsn() != INVALID_LSN);

        std::cout << "Test 4 PASSED: checkpoint flushes log to stable storage\n";
    }

    // ── Test 5: Checkpoint records survive restart ───────────────────────────
    // Write a checkpoint, destroy everything, reopen, verify records are there.
    {
        cleanup();
        {
            DiskManager  dm(TEST_DB);
            LogManager   lm(TEST_WAL);
            BufferManager bm(&dm, &lm, 10);
            TransactionManager tm(&lm, &bm);
            CheckpointManager  cm(&lm, &bm, &tm);

            Transaction* txn = tm.begin();
            (void)txn;  // just so ATT is non-empty

            cm.checkpoint();
            tm.abort(txn);
        }

        // Reopen — simulates restart.
        {
            LogManager lm2(TEST_WAL);
            auto records = lm2.read_log();

            // Should have: BEGIN, CHECKPOINT_BEGIN, CHECKPOINT_END, ABORT, TXN_END
            bool found_begin = false, found_end = false;
            for (auto& r : records) {
                if (r.get_type() == LogRecordType::CHECKPOINT_BEGIN) found_begin = true;
                if (r.get_type() == LogRecordType::CHECKPOINT_END)   found_end   = true;
            }
            assert(found_begin);
            assert(found_end);
        }

        std::cout << "Test 5 PASSED: checkpoint records survive restart\n";
    }

    cleanup();
    std::cout << "\nAll CheckpointManager tests passed.\n";
    return 0;
}
