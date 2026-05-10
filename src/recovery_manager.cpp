#include "recovery_manager.hpp"
#include <set>
#include <cassert>
#include <iostream>
#include <algorithm>

// ─── ATT/DPT deserialization helpers ─────────────────────────────────────────
// Same binary format that CheckpointManager writes into CHECKPOINT_END records.

static std::vector<std::pair<txn_id_t, lsn_t>>
deserialize_att(const std::vector<uint8_t>& data) {
    std::vector<std::pair<txn_id_t, lsn_t>> att;
    if (data.empty()) return att;

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

static std::vector<std::pair<page_id_t, lsn_t>>
deserialize_dpt(const std::vector<uint8_t>& data) {
    std::vector<std::pair<page_id_t, lsn_t>> dpt;
    if (data.empty()) return dpt;

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

// ─── constructor ─────────────────────────────────────────────────────────────

RecoveryManager::RecoveryManager(LogManager* log_manager,
                                 BufferManager* buffer_manager)
    : log_manager_(log_manager),
      buffer_manager_(buffer_manager) {}

// ─── recover ─────────────────────────────────────────────────────────────────
// Entry point: runs the three ARIES passes in order.

void RecoveryManager::recover() {
    att_.clear();
    dpt_.clear();
    analyze();
    redo();
    undo();
}

// ─── Pass 1: Analysis ────────────────────────────────────────────────────────
// Scan the log to reconstruct the ATT and DPT.
//
// 1. Find the last CHECKPOINT_END.  If found, seed att_ and dpt_ from it.
// 2. Scan forward from the record after the checkpoint (or from the start if
//    no checkpoint exists).
// 3. For each record:
//    - BEGIN:       add txn to ATT with lastLSN = record LSN
//    - COMMIT/TXN_END: remove txn from ATT
//    - UPDATE/CLR:  update ATT[txn].lastLSN;
//                   if page not in DPT, add with rec_lsn = record LSN
//    - ABORT:       update ATT[txn].lastLSN (txn stays — it's a loser)
//
// After analysis, att_ contains only LOSER transactions (never committed).

void RecoveryManager::analyze() {
    auto records = log_manager_->read_log();
    if (records.empty()) return;

    // Step 1: Find the last CHECKPOINT_END and seed ATT/DPT from it.
    size_t start_idx = 0;
    for (size_t i = records.size(); i > 0; --i) {
        if (records[i - 1].get_type() == LogRecordType::CHECKPOINT_END) {
            auto& ckpt = records[i - 1];

            // Seed ATT from checkpoint
            auto ckpt_att = deserialize_att(ckpt.get_old_tuple_data());
            for (auto& [tid, lsn] : ckpt_att) {
                att_[tid] = lsn;
            }

            // Seed DPT from checkpoint
            auto ckpt_dpt = deserialize_dpt(ckpt.get_new_tuple_data());
            for (auto& [pid, lsn] : ckpt_dpt) {
                dpt_[pid] = lsn;
            }

            start_idx = i;  // start scanning AFTER the checkpoint
            break;
        }
    }

    // Step 2: Scan forward from start_idx, updating ATT and DPT.
    for (size_t i = start_idx; i < records.size(); ++i) {
        auto& rec = records[i];
        lsn_t rec_lsn = rec.get_lsn();
        txn_id_t tid = rec.get_txn_id();

        switch (rec.get_type()) {
            case LogRecordType::BEGIN:
                att_[tid] = rec_lsn;
                break;

            case LogRecordType::COMMIT:
            case LogRecordType::TXN_END:
                att_.erase(tid);
                break;

            case LogRecordType::UPDATE:
            case LogRecordType::CLR: {
                // Update the transaction's lastLSN in the ATT.
                att_[tid] = rec_lsn;

                // If this page is not in the DPT, add it.
                // rec_lsn is the earliest dirty LSN for this page.
                page_id_t pid = rec.get_page_id();
                if (dpt_.find(pid) == dpt_.end()) {
                    dpt_[pid] = rec_lsn;
                }
                break;
            }

            case LogRecordType::ABORT:
                att_[tid] = rec_lsn;
                break;

            default:
                break;
        }
    }
}

// ─── Pass 2: Redo ────────────────────────────────────────────────────────────
// Repeat history: replay every UPDATE and CLR from the earliest dirty page's
// rec_lsn forward, skipping records that are already reflected on disk.
//
// For each UPDATE/CLR record at LSN L, for page P:
//   1. P not in DPT?            → skip (page was clean at checkpoint)
//   2. L < DPT[P].rec_lsn?     → skip (page was flushed after this change)
//   3. page.pageLSN >= L?       → skip (change already on disk)
//   4. Otherwise                → apply new_data to the page

void RecoveryManager::redo() {
    if (dpt_.empty()) return;

    // Find the smallest rec_lsn across all dirty pages — redo starts here.
    lsn_t start_lsn = INVALID_LSN;
    for (auto& [pid, rlsn] : dpt_) {
        if (start_lsn == INVALID_LSN || rlsn < start_lsn)
            start_lsn = rlsn;
    }
    if (start_lsn == INVALID_LSN) return;

    auto records = log_manager_->read_log();

    for (auto& rec : records) {
        if (rec.get_lsn() < start_lsn) continue;

        if (rec.get_type() != LogRecordType::UPDATE &&
            rec.get_type() != LogRecordType::CLR) continue;

        page_id_t pid = rec.get_page_id();

        // Check 1: page not in DPT → skip
        if (dpt_.find(pid) == dpt_.end()) continue;

        // Check 2: record LSN < page's rec_lsn → skip
        if (rec.get_lsn() < dpt_[pid]) continue;

        // Check 3: fetch page and check pageLSN
        Page* page = buffer_manager_->fetch_page(pid);
        if (page == nullptr) continue;

        if (page->get_page_lsn() >= rec.get_lsn()) {
            buffer_manager_->unpin_page(pid, false);
            continue;
        }

        // Apply the change.
        const auto& new_data = rec.get_new_tuple_data();
        const auto& old_data = rec.get_old_tuple_data();

        if (rec.get_type() == LogRecordType::UPDATE) {
            if (old_data.empty()) {
                // Original operation was an INSERT → redo by inserting.
                page->insert_record(new_data.data(),
                                    static_cast<uint16_t>(new_data.size()), 0);
            } else {
                // Original operation was an UPDATE → redo by overwriting.
                page->update_record(rec.get_slot_id(),
                                    new_data.data(),
                                    static_cast<uint16_t>(new_data.size()), 0);
            }
        } else {
            // CLR — redo whatever the compensation did.
            if (new_data.empty()) {
                // CLR undid an insert → redo the delete.
                page->delete_record(rec.get_slot_id(), 0);
            } else {
                // CLR undid an update → redo restoring the before-image.
                page->update_record(rec.get_slot_id(),
                                    new_data.data(),
                                    static_cast<uint16_t>(new_data.size()), 0);
            }
        }

        page->set_page_lsn(rec.get_lsn());
        buffer_manager_->unpin_page(pid, /*is_dirty=*/true);
    }
}

// ─── Pass 3: Undo ────────────────────────────────────────────────────────────
// Roll back all loser transactions (those still in att_ after analysis).
//
// Algorithm:
//   1. Collect the lastLSN of every loser into a max-set (largest first).
//   2. Pop the largest LSN, read its record:
//      - UPDATE → undo (restore before-image or delete), write CLR, add prev to set
//      - CLR    → skip (already undone), add undo_next_lsn to set
//      - BEGIN  → done with this txn, write TXN_END
//      - ABORT  → follow prev_lsn (continue undoing)
//   3. Repeat until the set is empty.

void RecoveryManager::undo() {
    if (att_.empty()) return;

    // Collect lastLSNs of all losers into a max-ordered set.
    std::set<lsn_t, std::greater<lsn_t>> to_undo;
    for (auto& [tid, last_lsn] : att_) {
        if (last_lsn != INVALID_LSN) {
            to_undo.insert(last_lsn);
        }
    }

    while (!to_undo.empty()) {
        lsn_t lsn = *to_undo.begin();
        to_undo.erase(to_undo.begin());

        LogRecord rec = log_manager_->read_record_at_lsn(lsn);
        if (rec.get_type() == LogRecordType::INVALID) break;

        txn_id_t tid = rec.get_txn_id();

        if (rec.get_type() == LogRecordType::UPDATE) {
            // ── Undo one data modification ──
            Page* page = buffer_manager_->fetch_page(rec.get_page_id());
            if (page != nullptr) {
                const auto& old_data = rec.get_old_tuple_data();

                // Restore before-image (or delete if it was an insert).
                if (old_data.empty()) {
                    page->delete_record(rec.get_slot_id(), 0);
                } else {
                    bool ok = page->update_record(rec.get_slot_id(),
                                        old_data.data(),
                                        static_cast<uint16_t>(old_data.size()), 0);
                    assert(ok && "recovery undo: update_record failed");
                }

                // Write a CLR for crash-safety.
                LogRecord clr(tid, att_[tid],
                              rec.get_page_id(), rec.get_slot_id(),
                              /*old_data=*/{},
                              /*new_data=*/old_data,
                              /*undo_next_lsn=*/rec.get_prev_lsn());
                lsn_t clr_lsn = log_manager_->append(clr);
                att_[tid] = clr_lsn;   // update lastLSN in ATT

                page->set_page_lsn(clr_lsn);
                buffer_manager_->unpin_page(rec.get_page_id(), /*is_dirty=*/true);
            }

            // Continue undo from the previous record.
            if (rec.get_prev_lsn() != INVALID_LSN) {
                to_undo.insert(rec.get_prev_lsn());
            }

        } else if (rec.get_type() == LogRecordType::CLR) {
            // CLR — this undo step was already done.  Jump to undo_next_lsn.
            if (rec.get_undo_next_lsn() != INVALID_LSN) {
                to_undo.insert(rec.get_undo_next_lsn());
            } else {
                // undo_next_lsn == INVALID → BEGIN was reached, write TXN_END.
                LogRecord end_rec(tid, att_[tid], LogRecordType::TXN_END);
                log_manager_->append(end_rec);
                att_.erase(tid);
            }

        } else if (rec.get_type() == LogRecordType::BEGIN) {
            // Reached the start of this transaction — fully undone.
            LogRecord end_rec(tid, att_[tid], LogRecordType::TXN_END);
            log_manager_->append(end_rec);
            att_.erase(tid);

        } else {
            // ABORT or other records — follow the prev chain.
            if (rec.get_prev_lsn() != INVALID_LSN) {
                to_undo.insert(rec.get_prev_lsn());
            }
        }
    }
}
