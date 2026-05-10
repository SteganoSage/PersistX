#pragma once

#include "common.hpp"
#include "transaction.hpp"
#include "log_manager.hpp"
#include "log_record.hpp"
#include "buffer_manager.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════════
// TransactionManager
// ═══════════════════════════════════════════════════════════════════════════════
//
// Manages the full lifecycle of transactions: begin, commit, abort.
// Maintains the Active Transaction Table (ATT): the set of transactions
// that are currently in-flight.
//
// Atomicity guarantee:
//   - commit() forces the COMMIT record to stable storage before returning.
//     A transaction is durable the moment commit() returns.
//   - abort() walks the undo chain (prevLSN pointers) backward, restoring
//     every before-image and writing a CLR (Compensation Log Record) for each
//     undo step. This makes abort crash-safe: if the system crashes mid-abort,
//     the CLRs tell recovery which steps were already done.
//
// Thread safety:
//   - A mutex protects the ATT and next_txn_id_.
//   - Individual Transaction objects are NOT thread-safe — they must be
//     accessed only by their owning thread.
//   - The mutex is released before calling into BufferManager or LogManager,
//     respecting the latch ordering:
//         TransactionManager mutex → BufferManager mutex → LogManager mutex
// ═══════════════════════════════════════════════════════════════════════════════

class TransactionManager {
public:
    TransactionManager(LogManager* log_manager, BufferManager* buffer_manager);

    ~TransactionManager() = default;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // ── lifecycle ────────────────────────────────────────────────────────────

    // Allocate a new transaction ID, write a BEGIN record, return a pointer
    // to the Transaction (owned by the ATT — do not delete it).
    Transaction* begin();

    // Write COMMIT record, flush the log (durability), write TXN_END, remove
    // from ATT. The pointer is invalid after this call.
    void commit(Transaction* txn);

    // Write ABORT record, undo all modifications via the prevLSN chain
    // (writing CLRs for each undo step), write TXN_END, remove from ATT.
    // The pointer is invalid after this call.
    void abort(Transaction* txn);

    std::vector<std::pair<txn_id_t, lsn_t>> get_att_snapshot();

    // ── WAL helpers ──────────────────────────────────────────────────────────

    // Log a data modification on behalf of txn.
    // The caller is responsible for:
    //   1. Capturing old_data (before-image) before modifying the page.
    //   2. Modifying the page (e.g. via page->update_record()).
    //   3. Calling log_update() to record the change.
    //   4. Setting page->set_page_lsn(returned_lsn) to stamp the page.
    // Returns the LSN of the UPDATE record.
    lsn_t log_update(Transaction*               txn,
                     page_id_t                  page_id,
                     slot_id_t                  slot_id,
                     const std::vector<uint8_t>& old_data,
                     const std::vector<uint8_t>& new_data);

private:
    LogManager*    log_manager_;
    BufferManager* buffer_manager_;

    // Active Transaction Table: txn_id → Transaction object (owned here).
    std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> att_;

    txn_id_t   next_txn_id_{0};
    std::mutex mutex_;  // protects att_ and next_txn_id_
};