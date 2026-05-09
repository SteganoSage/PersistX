#include "log_manager.hpp"
#include <stdexcept>
#include <iostream>

// ---------------------------------------------------------------------------
// Constructor: open (or create) the WAL file in binary read+write mode.
// Position the write cursor at the end so appends go after existing data.
// ---------------------------------------------------------------------------

LogManager::LogManager(const std::string& log_file_path)
    : log_file_path_(log_file_path)
{
    log_file_.open(log_file_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!log_file_.is_open()) {
        // File does not exist -- create it first, then reopen for read+write.
        std::ofstream creator(log_file_path, std::ios::binary);
        if (!creator.is_open()) {
            throw std::runtime_error("LogManager: failed to create WAL file: " + log_file_path);
        }
        creator.close();

        log_file_.open(log_file_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!log_file_.is_open()) {
            throw std::runtime_error("LogManager: failed to open WAL file: " + log_file_path);
        }
    }

    // Seek to end so future writes append.
    log_file_.seekp(0, std::ios::end);
}

// ---------------------------------------------------------------------------
// Destructor: flush any remaining buffered records, then close.
// ---------------------------------------------------------------------------

LogManager::~LogManager() {
    flush_all();
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

// ---------------------------------------------------------------------------
// append: assign an LSN, serialize the record, buffer it in memory.
// Does NOT write to disk -- that happens on flush().
// ---------------------------------------------------------------------------

lsn_t LogManager::append(LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Step 1: stamp the record with the next LSN
    lsn_t assigned_lsn = next_lsn_;
    record.set_lsn(assigned_lsn);
    next_lsn_++;

    // Step 2: serialize the record to bytes
    std::vector<uint8_t> bytes = record.serialize();

    // Step 3: append the serialized bytes to the in-memory buffer
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    return assigned_lsn;
}

// ---------------------------------------------------------------------------
// flush: write buffered records to the WAL file on disk, up to a given LSN.
// If the requested LSN is already on stable storage, return immediately.
// ---------------------------------------------------------------------------

void LogManager::flush(lsn_t up_to_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Already flushed past this point -- nothing to do.
    if (flushed_lsn_ != INVALID_LSN && flushed_lsn_ >= up_to_lsn) {
        return;
    }

    // Nothing in the buffer to flush.
    if (buffer_.empty()) {
        return;
    }

    // Write the entire buffer to the log file.
    log_file_.write(reinterpret_cast<const char*>(buffer_.data()),
                    static_cast<std::streamsize>(buffer_.size()));
    log_file_.flush();

    // Update flushed_lsn to the highest LSN we have seen so far.
    // next_lsn_ is always one past the last assigned, so the highest
    // flushed LSN is next_lsn_ - 1.
    if (next_lsn_ > 0) {
        flushed_lsn_ = next_lsn_ - 1;
    }

    // Clear the buffer -- these records are now on stable storage.
    buffer_.clear();
}

// ---------------------------------------------------------------------------
// flush_all: unconditionally flush everything in the buffer.
// ---------------------------------------------------------------------------

void LogManager::flush_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (buffer_.empty()) {
        return;
    }

    log_file_.write(reinterpret_cast<const char*>(buffer_.data()),
                    static_cast<std::streamsize>(buffer_.size()));
    log_file_.flush();

    if (next_lsn_ > 0) {
        flushed_lsn_ = next_lsn_ - 1;
    }

    buffer_.clear();
}

// ---------------------------------------------------------------------------
// read_log: scan the WAL file from the beginning and deserialize every
// record sequentially. Used by RecoveryManager on startup.
// ---------------------------------------------------------------------------

std::vector<LogRecord> LogManager::read_log() {
    std::vector<LogRecord> records;

    // Open a separate input stream so we do not disturb the write cursor.
    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open()) {
        return records;  // No WAL file -- nothing to recover.
    }

    while (true) {
        // Step 1: read the 4-byte total_size prefix.
        uint32_t total_size = 0;
        in.read(reinterpret_cast<char*>(&total_size), sizeof(total_size));
        if (in.eof() || in.gcount() < static_cast<std::streamsize>(sizeof(total_size))) {
            break;  // End of file or incomplete record.
        }

        // Step 2: allocate a buffer and copy the total_size we already read.
        std::vector<uint8_t> buf(total_size);
        std::memcpy(buf.data(), &total_size, sizeof(total_size));

        // Step 3: read the remaining bytes of this record.
        uint32_t remaining = total_size - static_cast<uint32_t>(sizeof(total_size));
        in.read(reinterpret_cast<char*>(buf.data() + sizeof(total_size)),
                static_cast<std::streamsize>(remaining));
        if (in.gcount() < static_cast<std::streamsize>(remaining)) {
            break;  // Truncated record -- stop here.
        }

        // Step 4: deserialize and store.
        records.push_back(LogRecord::deserialize(buf.data(), total_size));
    }

    return records;
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

lsn_t LogManager::get_flushed_lsn() {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushed_lsn_;
}

lsn_t LogManager::get_next_lsn() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_lsn_;
}
