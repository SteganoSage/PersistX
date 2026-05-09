#pragma once

#include "common.hpp"
#include <vector>
#include <cstring>

class LogRecord{

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

    // --- Getters ---

    lsn_t         get_lsn()        const { return lsn_; }
    txn_id_t      get_txn_id()     const { return txn_id_; }
    lsn_t         get_prev_lsn()   const { return prev_lsn_; }
    LogRecordType get_type()       const { return type_; }
    page_id_t     get_page_id()    const { return page_id_; }
    slot_id_t     get_slot_id()    const { return slot_id_; }
    lsn_t         get_undo_next_lsn() const { return undo_next_lsn_; }

    const std::vector<uint8_t>& get_old_tuple_data() const { return old_tuple_data_; }
    const std::vector<uint8_t>& get_new_tuple_data() const { return new_tuple_data_; }

    // --- Setter for LSN (assigned by LogManager at append time) ---

    void set_lsn(lsn_t lsn) { lsn_ = lsn; }

    // --- Serialization ---
    // Converts this LogRecord into a flat byte buffer for writing to the WAL file.
    // Layout:
    //   [4] total_size  [8] lsn  [8] txn_id  [8] prev_lsn  [1] type   (29 byte header)
    //   UPDATE adds: [4] page_id  [2] slot_id  [4] old_size  [N] old_data  [4] new_size  [M] new_data
    //   CLR adds:    same as UPDATE  +  [8] undo_next_lsn

    std::vector<uint8_t> serialize() const {
        // Step 1: compute total size
        const uint32_t HEADER_SIZE = 4 + 8 + 8 + 8 + 1;  // total_size + lsn + txn_id + prev_lsn + type
        uint32_t total_size = HEADER_SIZE;

        if (type_ == LogRecordType::UPDATE || type_ == LogRecordType::CLR) {
            total_size += 4;  // page_id
            total_size += 2;  // slot_id
            total_size += 4;  // old_data length
            total_size += static_cast<uint32_t>(old_tuple_data_.size());
            total_size += 4;  // new_data length
            total_size += static_cast<uint32_t>(new_tuple_data_.size());
        }
        if (type_ == LogRecordType::CLR) {
            total_size += 8;  // undo_next_lsn
        }

        // Step 2: allocate the buffer
        std::vector<uint8_t> buf(total_size);
        uint32_t offset = 0;

        // Step 3: write the header
        std::memcpy(&buf[offset], &total_size, sizeof(total_size));
        offset += sizeof(total_size);

        std::memcpy(&buf[offset], &lsn_, sizeof(lsn_));
        offset += sizeof(lsn_);

        std::memcpy(&buf[offset], &txn_id_, sizeof(txn_id_));
        offset += sizeof(txn_id_);

        std::memcpy(&buf[offset], &prev_lsn_, sizeof(prev_lsn_));
        offset += sizeof(prev_lsn_);

        uint8_t type_byte = static_cast<uint8_t>(type_);
        std::memcpy(&buf[offset], &type_byte, sizeof(type_byte));
        offset += sizeof(type_byte);

        // Step 4: write UPDATE / CLR payload
        if (type_ == LogRecordType::UPDATE || type_ == LogRecordType::CLR) {
            std::memcpy(&buf[offset], &page_id_, sizeof(page_id_));
            offset += sizeof(page_id_);

            std::memcpy(&buf[offset], &slot_id_, sizeof(slot_id_));
            offset += sizeof(slot_id_);

            uint32_t old_size = static_cast<uint32_t>(old_tuple_data_.size());
            std::memcpy(&buf[offset], &old_size, sizeof(old_size));
            offset += sizeof(old_size);

            if (old_size > 0) {
                std::memcpy(&buf[offset], old_tuple_data_.data(), old_size);
                offset += old_size;
            }

            uint32_t new_size = static_cast<uint32_t>(new_tuple_data_.size());
            std::memcpy(&buf[offset], &new_size, sizeof(new_size));
            offset += sizeof(new_size);

            if (new_size > 0) {
                std::memcpy(&buf[offset], new_tuple_data_.data(), new_size);
                offset += new_size;
            }
        }

        // Step 5: write CLR-specific field
        if (type_ == LogRecordType::CLR) {
            std::memcpy(&buf[offset], &undo_next_lsn_, sizeof(undo_next_lsn_));
            offset += sizeof(undo_next_lsn_);
        }

        return buf;
    }

    // --- Deserialization ---
    // Reconstructs a LogRecord from a raw byte buffer (read from the WAL file).
    // The caller passes a pointer to the start of the record and the total size.
    // Returns a fully populated LogRecord.

    static LogRecord deserialize(const uint8_t* data, uint32_t size) {
        LogRecord rec;
        uint32_t offset = 0;

        // Skip total_size (we already know it from the caller)
        uint32_t total_size;
        std::memcpy(&total_size, &data[offset], sizeof(total_size));
        offset += sizeof(total_size);

        // Read header
        std::memcpy(&rec.lsn_, &data[offset], sizeof(rec.lsn_));
        offset += sizeof(rec.lsn_);

        std::memcpy(&rec.txn_id_, &data[offset], sizeof(rec.txn_id_));
        offset += sizeof(rec.txn_id_);

        std::memcpy(&rec.prev_lsn_, &data[offset], sizeof(rec.prev_lsn_));
        offset += sizeof(rec.prev_lsn_);

        uint8_t type_byte;
        std::memcpy(&type_byte, &data[offset], sizeof(type_byte));
        rec.type_ = static_cast<LogRecordType>(type_byte);
        offset += sizeof(type_byte);

        // Read UPDATE / CLR payload
        if (rec.type_ == LogRecordType::UPDATE || rec.type_ == LogRecordType::CLR) {
            std::memcpy(&rec.page_id_, &data[offset], sizeof(rec.page_id_));
            offset += sizeof(rec.page_id_);

            std::memcpy(&rec.slot_id_, &data[offset], sizeof(rec.slot_id_));
            offset += sizeof(rec.slot_id_);

            uint32_t old_size;
            std::memcpy(&old_size, &data[offset], sizeof(old_size));
            offset += sizeof(old_size);

            rec.old_tuple_data_.resize(old_size);
            if (old_size > 0) {
                std::memcpy(rec.old_tuple_data_.data(), &data[offset], old_size);
                offset += old_size;
            }

            uint32_t new_size;
            std::memcpy(&new_size, &data[offset], sizeof(new_size));
            offset += sizeof(new_size);

            rec.new_tuple_data_.resize(new_size);
            if (new_size > 0) {
                std::memcpy(rec.new_tuple_data_.data(), &data[offset], new_size);
                offset += new_size;
            }
        }

        // Read CLR-specific field
        if (rec.type_ == LogRecordType::CLR) {
            std::memcpy(&rec.undo_next_lsn_, &data[offset], sizeof(rec.undo_next_lsn_));
            offset += sizeof(rec.undo_next_lsn_);
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
    std::vector<uint8_t> old_tuple_data_;
    std::vector<uint8_t> new_tuple_data_;

    lsn_t undo_next_lsn_ = INVALID_LSN;

};
