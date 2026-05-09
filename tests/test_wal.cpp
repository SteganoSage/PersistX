#include "log_record.hpp"
#include "log_manager.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

static const std::string TEST_WAL = "storage/test_wal.wal";

static void cleanup() {
    std::filesystem::remove(TEST_WAL);
}

int main() {
    cleanup();

    // ── Test 1: BEGIN record serialize → deserialize roundtrip ───────────────
    {
        LogRecord rec(/*txn_id=*/42, /*prev_lsn=*/INVALID_LSN, LogRecordType::BEGIN);
        rec.set_lsn(7);

        auto bytes = rec.serialize();
        LogRecord got = LogRecord::deserialize(bytes.data(),
                                               static_cast<uint32_t>(bytes.size()));

        assert(got.get_lsn()              == 7);
        assert(got.get_txn_id()           == 42);
        assert(got.get_prev_lsn()         == INVALID_LSN);
        assert(got.get_type()             == LogRecordType::BEGIN);
        assert(got.get_old_tuple_data().empty());
        assert(got.get_new_tuple_data().empty());

        std::cout << "Test 1 PASSED: BEGIN record serialize/deserialize roundtrip\n";
    }

    // ── Test 2: UPDATE record roundtrip (with before/after images) ───────────
    {
        std::vector<uint8_t> old_data = {0xAA, 0xBB, 0xCC};
        std::vector<uint8_t> new_data = {0x11, 0x22, 0x33};

        LogRecord rec(/*txn_id=*/10, /*prev_lsn=*/5,
                      /*page_id=*/99, /*slot_id=*/3,
                      old_data, new_data);
        rec.set_lsn(20);

        auto bytes = rec.serialize();
        LogRecord got = LogRecord::deserialize(bytes.data(),
                                               static_cast<uint32_t>(bytes.size()));

        assert(got.get_lsn()              == 20);
        assert(got.get_txn_id()           == 10);
        assert(got.get_prev_lsn()         == 5);
        assert(got.get_type()             == LogRecordType::UPDATE);
        assert(got.get_page_id()          == 99);
        assert(got.get_slot_id()          == 3);
        assert(got.get_old_tuple_data()   == old_data);
        assert(got.get_new_tuple_data()   == new_data);
        assert(got.get_undo_next_lsn()    == INVALID_LSN);

        std::cout << "Test 2 PASSED: UPDATE record serialize/deserialize roundtrip\n";
    }

    // ── Test 3: CLR record roundtrip (UPDATE + undo_next_lsn) ────────────────
    {
        std::vector<uint8_t> old_data = {};
        std::vector<uint8_t> new_data = {0xDE, 0xAD};

        LogRecord rec(/*txn_id=*/5, /*prev_lsn=*/10,
                      /*page_id=*/7, /*slot_id=*/1,
                      old_data, new_data,
                      /*undo_next_lsn=*/3);
        rec.set_lsn(15);

        auto bytes = rec.serialize();
        LogRecord got = LogRecord::deserialize(bytes.data(),
                                               static_cast<uint32_t>(bytes.size()));

        assert(got.get_lsn()              == 15);
        assert(got.get_type()             == LogRecordType::CLR);
        assert(got.get_page_id()          == 7);
        assert(got.get_slot_id()          == 1);
        assert(got.get_new_tuple_data()   == new_data);
        assert(got.get_undo_next_lsn()    == 3);

        std::cout << "Test 3 PASSED: CLR record serialize/deserialize roundtrip\n";
    }

    // ── Test 4: COMMIT, ABORT, TXN_END simple record roundtrips ─────────────
    {
        for (auto type : {LogRecordType::COMMIT, LogRecordType::ABORT,
                          LogRecordType::TXN_END}) {
            LogRecord rec(/*txn_id=*/1, /*prev_lsn=*/0, type);
            rec.set_lsn(100);
            auto bytes = rec.serialize();
            LogRecord got = LogRecord::deserialize(bytes.data(),
                                                   static_cast<uint32_t>(bytes.size()));
            assert(got.get_lsn()              == 100);
            assert(got.get_type()             == type);
            assert(got.get_old_tuple_data().empty());
            assert(got.get_new_tuple_data().empty());
        }

        std::cout << "Test 4 PASSED: COMMIT / ABORT / TXN_END roundtrips\n";
    }

    // ── Test 5: append increments next_lsn; flushed_lsn stays INVALID ────────
    {
        cleanup();
        LogManager lm(TEST_WAL);

        assert(lm.get_flushed_lsn() == INVALID_LSN);
        assert(lm.get_next_lsn()    == 0);

        LogRecord r1(1, INVALID_LSN, LogRecordType::BEGIN);
        LogRecord r2(2, INVALID_LSN, LogRecordType::BEGIN);

        lsn_t lsn1 = lm.append(r1);
        lsn_t lsn2 = lm.append(r2);

        assert(lsn1 == 0);
        assert(lsn2 == 1);
        assert(lm.get_next_lsn()    == 2);
        assert(lm.get_flushed_lsn() == INVALID_LSN); // still not flushed

        std::cout << "Test 5 PASSED: append increments next_lsn, no implicit flush\n";
    }

    // ── Test 6: flush(lsn) updates flushed_lsn; redundant flush is a no-op ───
    {
        cleanup();
        LogManager lm(TEST_WAL);

        LogRecord r1(1, INVALID_LSN, LogRecordType::BEGIN);
        LogRecord r2(1, 0,           LogRecordType::COMMIT);
        lsn_t lsn1 = lm.append(r1); // 0
        lsn_t lsn2 = lm.append(r2); // 1

        lm.flush(lsn2);
        assert(lm.get_flushed_lsn() == lsn2);

        // Flushing an already-flushed LSN must not roll flushed_lsn backward.
        lm.flush(lsn1);
        assert(lm.get_flushed_lsn() == lsn2);

        std::cout << "Test 6 PASSED: flush updates flushed_lsn, no-op on redundant flush\n";
    }

    // ── Test 7: read_log recovers records written to disk ────────────────────
    {
        cleanup();
        LogManager lm(TEST_WAL);

        LogRecord r1(10, INVALID_LSN, LogRecordType::BEGIN);
        LogRecord r2(10, 0,           LogRecordType::COMMIT);
        lsn_t lsn1 = lm.append(r1); // 0
        lsn_t lsn2 = lm.append(r2); // 1
        lm.flush(lsn2);

        auto records = lm.read_log();
        assert(records.size()          == 2);
        assert(records[0].get_lsn()    == lsn1);
        assert(records[0].get_txn_id() == 10);
        assert(records[0].get_type()   == LogRecordType::BEGIN);
        assert(records[1].get_lsn()    == lsn2);
        assert(records[1].get_type()   == LogRecordType::COMMIT);

        std::cout << "Test 7 PASSED: read_log recovers flushed records\n";
    }

    // ── Test 8: read_record_at_lsn finds a specific record ───────────────────
    {
        cleanup();
        LogManager lm(TEST_WAL);

        LogRecord r0(1, INVALID_LSN, LogRecordType::BEGIN);
        LogRecord r1(1, 0,           LogRecordType::COMMIT);
        LogRecord r2(2, INVALID_LSN, LogRecordType::BEGIN);

        lsn_t lsn0 = lm.append(r0); // 0
        lsn_t lsn1 = lm.append(r1); // 1
        lsn_t lsn2 = lm.append(r2); // 2
        (void)lsn0;

        // read_record_at_lsn flushes internally — no manual flush needed.
        LogRecord found = lm.read_record_at_lsn(lsn1);
        assert(found.get_lsn()    == lsn1);
        assert(found.get_type()   == LogRecordType::COMMIT);
        assert(found.get_txn_id() == 1);

        LogRecord found2 = lm.read_record_at_lsn(lsn2);
        assert(found2.get_lsn()  == lsn2);
        assert(found2.get_type() == LogRecordType::BEGIN);

        // Non-existent LSN must return an INVALID record.
        LogRecord notfound = lm.read_record_at_lsn(999);
        assert(notfound.get_type() == LogRecordType::INVALID);

        std::cout << "Test 8 PASSED: read_record_at_lsn finds specific record\n";
    }

    // ── Test 9: WAL survives LogManager restart (persistence check) ───────────
    {
        cleanup();
        lsn_t lsn0, lsn1;

        {
            LogManager lm(TEST_WAL);
            LogRecord r0(5, INVALID_LSN, LogRecordType::BEGIN);
            LogRecord r1(5, 0,           LogRecordType::COMMIT);
            lsn0 = lm.append(r0);
            lsn1 = lm.append(r1);
            lm.flush(lsn1);
            // destructor flushes and closes
        }

        // Reopen — simulates a crash/restart.
        {
            LogManager lm2(TEST_WAL);
            auto records = lm2.read_log();
            assert(records.size()          == 2);
            assert(records[0].get_lsn()    == lsn0);
            assert(records[1].get_lsn()    == lsn1);
        }

        std::cout << "Test 9 PASSED: WAL survives LogManager restart\n";
    }

    cleanup();
    std::cout << "\nAll WAL tests passed.\n";
    return 0;
}