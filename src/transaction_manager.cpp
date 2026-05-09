#include "transaction_manager.hpp"
#include <stdexcept>

// ─── constructor ─────────────────────────────────────────────────────────────

TransactionManager::TransactionManager(LogManager*    log_manager, BufferManager* buffer_manager)
    : log_manager_(log_manager), 
    buffer_manager_(buffer_manager)
{}

// ─── begin ───────────────────────────────────────────────────────────────────
// Creates a new Transaction, writes a BEGIN record to the WAL, and adds the
// transaction to the ATT. Returns a raw (non-owning) pointer.

Transaction* TransactionManager::begin() {
    std::lock_guard<std::mutex> lock(mutex_);

    txn_id_t txn_id = next_txn_id_++;
    auto txn = std::make_unique<Transaction>(txn_id);

    // BEGIN record: prev_lsn = INVALID_LSN (start of the chain for this txn).
    LogRecord begin_rec(txn_id, INVALID_LSN, LogRecordType::BEGIN);
    lsn_t begin_lsn = log_manager_->append(begin_rec);

    // The BEGIN record's LSN is the first link in the undo chain.
    txn->set_prev_lsn(begin_lsn);

    Transaction* raw_ptr = txn.get();
    att_[txn_id] = std::move(txn);
    return raw_ptr;
}

// ─── commit ──────────────────────────────────────────────────────────────────
// Durability guarantee: the COMMIT record must reach stable storage before
// this function returns. Nothing else is written to disk eagerly (No-Force
// policy) — dirty pages will reach disk later via eviction or checkpoint.

void TransactionManager::commit(Transaction* txn) {
    txn_id_t txn_id = txn->get_txn_id();

    // 1. Write COMMIT record (links onto the transaction's prev chain).
    LogRecord commit_rec(txn_id, txn->get_prev_lsn(), LogRecordType::COMMIT);
    lsn_t commit_lsn = log_manager_->append(commit_rec);
    txn->set_prev_lsn(commit_lsn);

    // 2. Force-flush the log up to (at least) the COMMIT record.
    //    This is the heart of durability: the transaction is NOT committed
    //    until its COMMIT record is on stable storage.
    log_manager_->flush(commit_lsn);

    txn->set_state(TxnState::COMMITTED);

    // 3. Write TXN_END — bookkeeping record for recovery (marks the transaction
    //    as fully complete; recovery skips it during the undo phase).
    //    TXN_END does not need to be flushed immediately.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lsn_t last_lsn = txn->get_prev_lsn();
        LogRecord end_rec(txn_id, last_lsn, LogRecordType::TXN_END);
        log_manager_->append(end_rec);
        att_.erase(txn_id);
        // txn is now destroyed — do not dereference it after this point.
    }
}

// ─── abort ───────────────────────────────────────────────────────────────────
// Undoes every modification made by txn in reverse chronological order.
//
// For each UPDATE record found in the undo chain:
//   1. Fetch the affected page.
//   2. Restore the before-image (overwrite slot with old_data).
//   3. Write a CLR (Compensation Log Record) that describes the undo step and
//      carries an undo_next_lsn pointing to the NEXT record to undo.
//      This makes abort idempotent across crashes: if recovery encounters a
//      CLR it knows that UPDATE has already been undone, and jumps directly
//      to undo_next_lsn instead of re-processing it.
//
// The undo chain terminates at the BEGIN record (which has no data to undo).

void TransactionManager::abort(Transaction* txn) {
    txn_id_t txn_id = txn->get_txn_id();

    // 1. Write ABORT record (the chain link from abort back to the last UPDATE).
    LogRecord abort_rec(txn_id, txn->get_prev_lsn(), LogRecordType::ABORT);
    lsn_t abort_lsn = log_manager_->append(abort_rec);
    txn->set_prev_lsn(abort_lsn);
    txn->set_state(TxnState::ABORTED);

    // 2. Walk the undo chain.
    //    undo_lsn starts at the ABORT record. The loop follows prev_lsn pointers
    //    until it reaches a BEGIN (stops) or INVALID_LSN (safety stop).
    lsn_t undo_lsn = txn->get_prev_lsn();  // = abort_lsn

    while (undo_lsn != INVALID_LSN) {
        // read_record_at_lsn flushes the in-memory buffer first so the record
        // is guaranteed to be on disk before we try to read it.
        LogRecord rec = log_manager_->read_record_at_lsn(undo_lsn);

        if (rec.get_type() == LogRecordType::INVALID) {
            // Should never happen — bail out safely.
            break;
        }

        if (rec.get_type() == LogRecordType::BEGIN) {
            // Reached the start of this transaction — nothing left to undo.
            break;
        }

        if (rec.get_type() == LogRecordType::UPDATE) {
            // ── undo one data modification ───────────────────────────────────

            Page* page = buffer_manager_->fetch_page(rec.get_page_id());
            if (page != nullptr) {
                const std::vector<uint8_t>& old_data = rec.get_old_tuple_data();

                // Restore the before-image in-place.
                page->update_record(rec.get_slot_id(),
                                    old_data.data(),
                                    static_cast<uint16_t>(old_data.size()),
                                    0 /* lsn — will stamp via set_page_lsn below */);

                // Write a CLR to record this undo step.
                // CLR layout:
                //   old_data = {} (empty — CLRs are never themselves undone)
                //   new_data = old_data of the UPDATE (the value we just restored)
                //   undo_next_lsn = rec.get_prev_lsn()  → next record to process
                LogRecord clr(txn_id, txn->get_prev_lsn(),
                              rec.get_page_id(), rec.get_slot_id(),
                              /*old_data=*/{},
                              /*new_data=*/old_data,
                              /*undo_next_lsn=*/rec.get_prev_lsn());
                lsn_t clr_lsn = log_manager_->append(clr);
                txn->set_prev_lsn(clr_lsn);

                // Stamp the page with the CLR's LSN so recovery knows this
                // page was modified at this point in the log.
                page->set_page_lsn(clr_lsn);
                buffer_manager_->unpin_page(rec.get_page_id(), /*is_dirty=*/true);
            }

            // Follow the UPDATE's prev chain to the next record to undo.
            undo_lsn = rec.get_prev_lsn();

        } else if (rec.get_type() == LogRecordType::CLR) {
            // This record was already written during a previous (interrupted)
            // abort attempt. Jump to undo_next_lsn to skip the already-undone
            // UPDATE — never undo the same step twice.
            undo_lsn = rec.get_undo_next_lsn();

        } else {
            // ABORT, COMMIT, or other bookkeeping records — not data
            // modifications, nothing to undo. Just follow the chain.
            undo_lsn = rec.get_prev_lsn();
        }
    }

    // 3. Write TXN_END and remove from ATT.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lsn_t last_lsn = txn->get_prev_lsn();
        LogRecord end_rec(txn_id, last_lsn, LogRecordType::TXN_END);
        log_manager_->append(end_rec);
        att_.erase(txn_id);
        // txn is destroyed here — do not dereference after this point.
    }
}

// ─── log_update ──────────────────────────────────────────────────────────────
// Records a data modification in the WAL and advances the transaction's
// prevLSN to the new record. Does NOT modify the page — the caller does that.
//
// Typical caller sequence:
//   1. read_record(slot, old_data)      — capture before-image
//   2. update_record(slot, new_data)    — modify the page
//   3. lsn = tm.log_update(...)         — record the change in the WAL
//   4. page->set_page_lsn(lsn)          — stamp the page

lsn_t TransactionManager::log_update(Transaction* txn, page_id_t page_id, slot_id_t slot_id, 
    const std::vector<uint8_t>& old_data, const std::vector<uint8_t>& new_data) {
    LogRecord rec(txn->get_txn_id(), txn->get_prev_lsn(),
                  page_id, slot_id,
                  old_data, new_data);
    lsn_t lsn = log_manager_->append(rec);
    txn->set_prev_lsn(lsn);
    return lsn;
}