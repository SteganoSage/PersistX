#include "checkpoint_manager.hpp"
#include <cstring>

// ─── helper: serialize ATT snapshot into a byte vector ───────────────────────
// Format: [4 bytes: count] then for each entry [8 bytes: txn_id][8 bytes: lsn]

static std::vector<uint8_t> serialize_att(
    const std::vector<std::pair<txn_id_t, lsn_t>>& att)
{
    uint32_t count = static_cast<uint32_t>(att.size());
    // 4 bytes for count + 16 bytes per entry (8 txn_id + 8 lsn)
    std::vector<uint8_t> buf(4 + count * 16);
    uint32_t offset = 0;

    std::memcpy(&buf[offset], &count, sizeof(count));
    offset += sizeof(count);

    for (auto& [txn_id, lsn] : att) {
        std::memcpy(&buf[offset], &txn_id, sizeof(txn_id));
        offset += sizeof(txn_id);
        std::memcpy(&buf[offset], &lsn, sizeof(lsn));
        offset += sizeof(lsn);
    }
    return buf;
}

// ─── helper: serialize DPT snapshot into a byte vector ───────────────────────
// Format: [4 bytes: count] then for each entry [4 bytes: page_id][8 bytes: lsn]

static std::vector<uint8_t> serialize_dpt(
    const std::vector<std::pair<page_id_t, lsn_t>>& dpt)
{
    uint32_t count = static_cast<uint32_t>(dpt.size());
    // 4 bytes for count + 12 bytes per entry (4 page_id + 8 lsn)
    std::vector<uint8_t> buf(4 + count * 12);
    uint32_t offset = 0;

    std::memcpy(&buf[offset], &count, sizeof(count));
    offset += sizeof(count);

    for (auto& [page_id, lsn] : dpt) {
        std::memcpy(&buf[offset], &page_id, sizeof(page_id));
        offset += sizeof(page_id);
        std::memcpy(&buf[offset], &lsn, sizeof(lsn));
        offset += sizeof(lsn);
    }
    return buf;
}

// ─── checkpoint ──────────────────────────────────────────────────────────────
// Fuzzy checkpoint protocol:
//   1. Write CHECKPOINT_BEGIN  (marks the start — recovery scans for this)
//   2. Snapshot ATT and DPT    (captured under their respective locks)
//   3. Write CHECKPOINT_END    (carries serialised ATT + DPT in payload)
//   4. Flush dirty pages       (push modified data to disk)
//   5. Flush the log           (guarantee the checkpoint records are durable)

void CheckpointManager::checkpoint() {
    // Step 1: CHECKPOINT_BEGIN — a simple marker record.
    LogRecord begin_rec(INVALID_TXN_ID, INVALID_LSN, LogRecordType::CHECKPOINT_BEGIN);
    log_manager_->append(begin_rec);

    // Step 2: Snapshot the ATT and DPT.
    // Each snapshot grabs its own mutex internally, so this is thread-safe.
    auto att = transaction_manager_->get_att_snapshot();
    auto dpt = buffer_manager_->get_dirty_pages();

    // Step 3: Serialize ATT/DPT and write CHECKPOINT_END.
    // ATT goes into old_data, DPT goes into new_data.
    std::vector<uint8_t> att_bytes = serialize_att(att);
    std::vector<uint8_t> dpt_bytes = serialize_dpt(dpt);

    LogRecord end_rec(LogRecordType::CHECKPOINT_END, att_bytes, dpt_bytes);
    lsn_t end_lsn = log_manager_->append(end_rec);

    // Step 4: Flush all dirty pages to disk.
    buffer_manager_->flush_all_pages();

    // Step 5: Flush the log so the checkpoint records are on stable storage.
    log_manager_->flush(end_lsn);
}
