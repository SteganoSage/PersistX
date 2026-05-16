#pragma once

#include "common.hpp"
#include <vector>
#include <cstring>

class LogRecord
{

public:
    // Constructor 1: Default -- creates an INVALID record (used by deserialization)
    LogRecord() = default;

    // Constructor 2: Simple records (BEGIN, COMMIT, ABORT, TXN_END)
    // These carry no page/slot/tuple data.
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn, LogRecordType type)
        : txn_id_(txn_id), prev_lsn_(prev_lsn), type_(type) {}

    // Constructor 3: UPDATE -- records a data modification with before/after images
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn,
              page_id_t page_id, slot_id_t slot_id,
              std::vector<uint8_t> old_data, std::vector<uint8_t> new_data)
        : txn_id_(txn_id), prev_lsn_(prev_lsn), type_(LogRecordType::UPDATE),
          page_id_(page_id), slot_id_(slot_id),
          old_tuple_data_(std::move(old_data)),
          new_tuple_data_(std::move(new_data)) {}

    // Constructor 3b: UPDATE with index key (used for INSERT/DELETE ops)
    // Constructor 3b: UPDATE with index key -- used for INSERT/DELETE ops so
    // that abort() can remove the ghost B+ tree entry without rescanning the heap.
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn,
              page_id_t page_id, slot_id_t slot_id,
              std::vector<uint8_t> old_data, std::vector<uint8_t> new_data,
              int64_t index_key)
        : txn_id_(txn_id), prev_lsn_(prev_lsn), type_(LogRecordType::UPDATE),
          page_id_(page_id), slot_id_(slot_id),
          index_key_(index_key), // ← moved before old/new_tuple_data_
          old_tuple_data_(std::move(old_data)),
          new_tuple_data_(std::move(new_data))
    {
    }

    // Constructor 4: CLR -- same as UPDATE but with undo_next_lsn for recovery
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn,
              page_id_t page_id, slot_id_t slot_id,
              std::vector<uint8_t> old_data, std::vector<uint8_t> new_data,
              lsn_t undo_next_lsn)
        : txn_id_(txn_id), prev_lsn_(prev_lsn), type_(LogRecordType::CLR),
          page_id_(page_id), slot_id_(slot_id),
          old_tuple_data_(std::move(old_data)),
          new_tuple_data_(std::move(new_data)),
          undo_next_lsn_(undo_next_lsn) {}

    // Constructor 5: CHECKPOINT_END -- carries ATT (old_data) and DPT (new_data)
    // page_id and slot_id are unused (set to INVALID) but included in the
    // serialisation format so the existing UPDATE codec handles it unchanged.
    LogRecord(LogRecordType type,
              std::vector<uint8_t> att_data, std::vector<uint8_t> dpt_data)
        : txn_id_(INVALID_TXN_ID), prev_lsn_(INVALID_LSN), type_(type),
          page_id_(INVALID_PAGE_ID), slot_id_(INVALID_SLOT_ID),
          old_tuple_data_(std::move(att_data)),
          new_tuple_data_(std::move(dpt_data)) {}

    // --- Getters ---

    lsn_t get_lsn() const { return lsn_; }
    txn_id_t get_txn_id() const { return txn_id_; }
    lsn_t get_prev_lsn() const { return prev_lsn_; }
    LogRecordType get_type() const { return type_; }
    page_id_t get_page_id() const { return page_id_; }
    slot_id_t get_slot_id() const { return slot_id_; }
    int64_t get_index_key() const { return index_key_; }
    lsn_t get_undo_next_lsn() const { return undo_next_lsn_; }

    const std::vector<uint8_t> &get_old_tuple_data() const { return old_tuple_data_; }
    const std::vector<uint8_t> &get_new_tuple_data() const { return new_tuple_data_; }

    // --- Setter for LSN (assigned by LogManager at append time) ---

    void set_lsn(lsn_t lsn) { lsn_ = lsn; }

    // --- Serialization ---
    // Converts this LogRecord into a flat byte buffer for writing to the WAL file.
    // Layout:
    //   [4] total_size  [8] lsn  [8] txn_id  [8] prev_lsn  [1] type   (29 byte header)
    //   UPDATE adds: [4] page_id  [2] slot_id  [4] old_size  [N] old_data  [4] new_size  [M] new_data
    //   CLR adds:    same as UPDATE  +  [8] undo_next_lsn

    std::vector<uint8_t> serialize() const
    {
        const uint32_t HEADER_SIZE = 4 + 8 + 8 + 8 + 1;
        uint32_t total_size = HEADER_SIZE;

        if (type_ == LogRecordType::UPDATE || type_ == LogRecordType::CLR || type_ == LogRecordType::CHECKPOINT_END)
        {
            total_size += 4 + 2; // page_id, slot_id
            total_size += 4 + static_cast<uint32_t>(old_tuple_data_.size());
            total_size += 4 + static_cast<uint32_t>(new_tuple_data_.size());
        }
        if (type_ == LogRecordType::UPDATE)
        {
            total_size += 8; // index_key  ← ADD THIS
        }
        if (type_ == LogRecordType::CLR)
        {
            total_size += 8; // undo_next_lsn
        }

        std::vector<uint8_t> buf(total_size);
        uint32_t offset = 0;

        // header (unchanged)
        std::memcpy(&buf[offset], &total_size, 4);
        offset += 4;
        std::memcpy(&buf[offset], &lsn_, 8);
        offset += 8;
        std::memcpy(&buf[offset], &txn_id_, 8);
        offset += 8;
        std::memcpy(&buf[offset], &prev_lsn_, 8);
        offset += 8;
        uint8_t tb = static_cast<uint8_t>(type_);
        std::memcpy(&buf[offset], &tb, 1);
        offset += 1;

        if (type_ == LogRecordType::UPDATE || type_ == LogRecordType::CLR || type_ == LogRecordType::CHECKPOINT_END)
        {
            std::memcpy(&buf[offset], &page_id_, 4);
            offset += 4;
            std::memcpy(&buf[offset], &slot_id_, 2);
            offset += 2;

            uint32_t old_sz = static_cast<uint32_t>(old_tuple_data_.size());
            std::memcpy(&buf[offset], &old_sz, 4);
            offset += 4;
            if (old_sz)
            {
                std::memcpy(&buf[offset], old_tuple_data_.data(), old_sz);
                offset += old_sz;
            }

            uint32_t new_sz = static_cast<uint32_t>(new_tuple_data_.size());
            std::memcpy(&buf[offset], &new_sz, 4);
            offset += 4;
            if (new_sz)
            {
                std::memcpy(&buf[offset], new_tuple_data_.data(), new_sz);
                offset += new_sz;
            }
        }
        if (type_ == LogRecordType::UPDATE)
        { // ← ADD THIS BLOCK
            std::memcpy(&buf[offset], &index_key_, 8);
            offset += 8;
        }
        if (type_ == LogRecordType::CLR)
        {
            std::memcpy(&buf[offset], &undo_next_lsn_, 8);
            offset += 8;
        }

        return buf;
    }

    static LogRecord deserialize(const uint8_t *data, uint32_t size)
    {
        LogRecord rec;
        uint32_t offset = 0;

        offset += 4; // skip total_size
        std::memcpy(&rec.lsn_, &data[offset], 8);
        offset += 8;
        std::memcpy(&rec.txn_id_, &data[offset], 8);
        offset += 8;
        std::memcpy(&rec.prev_lsn_, &data[offset], 8);
        offset += 8;
        uint8_t tb;
        std::memcpy(&tb, &data[offset], 1);
        rec.type_ = static_cast<LogRecordType>(tb);
        offset += 1;

        if (rec.type_ == LogRecordType::UPDATE || rec.type_ == LogRecordType::CLR || rec.type_ == LogRecordType::CHECKPOINT_END)
        {
            std::memcpy(&rec.page_id_, &data[offset], 4);
            offset += 4;
            std::memcpy(&rec.slot_id_, &data[offset], 2);
            offset += 2;

            uint32_t old_sz;
            std::memcpy(&old_sz, &data[offset], 4);
            offset += 4;
            rec.old_tuple_data_.resize(old_sz);
            if (old_sz)
            {
                std::memcpy(rec.old_tuple_data_.data(), &data[offset], old_sz);
                offset += old_sz;
            }

            uint32_t new_sz;
            std::memcpy(&new_sz, &data[offset], 4);
            offset += 4;
            rec.new_tuple_data_.resize(new_sz);
            if (new_sz)
            {
                std::memcpy(rec.new_tuple_data_.data(), &data[offset], new_sz);
                offset += new_sz;
            }
        }
        if (rec.type_ == LogRecordType::UPDATE)
        { // ← ADD THIS BLOCK
            std::memcpy(&rec.index_key_, &data[offset], 8);
            offset += 8;
        }
        if (rec.type_ == LogRecordType::CLR)
        {
            std::memcpy(&rec.undo_next_lsn_, &data[offset], 8);
            offset += 8;
        }

        return rec;
    }

private:
    lsn_t lsn_ = INVALID_LSN;
    txn_id_t txn_id_ = INVALID_TXN_ID;
    lsn_t prev_lsn_ = INVALID_LSN;
    LogRecordType type_ = LogRecordType::INVALID;

    page_id_t page_id_ = INVALID_PAGE_ID;
    slot_id_t slot_id_ = INVALID_SLOT_ID;
    int64_t index_key_ = 0; // B+ tree key — stored for INSERT undo
    std::vector<uint8_t> old_tuple_data_;
    std::vector<uint8_t> new_tuple_data_;

    lsn_t undo_next_lsn_ = INVALID_LSN;
};
