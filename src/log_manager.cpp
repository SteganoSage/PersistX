#include "log_manager.hpp"
#include <stdexcept>
#include <cstring>

// ─── constructor ─────────────────────────────────────────────────────────────

LogManager::LogManager(const std::string& log_file_path)
    : log_file_path_(log_file_path)
{
    log_file_.open(log_file_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!log_file_.is_open()) {
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
    log_file_.seekp(0, std::ios::end);
}

// ─── destructor ──────────────────────────────────────────────────────────────

LogManager::~LogManager() {
    flush_all();
    if (log_file_.is_open()) log_file_.close();
}

// ─── append ──────────────────────────────────────────────────────────────────
// Stamps the record with the next LSN, serializes it, and adds it to the
// in-memory buffer. Does NOT touch the disk.

lsn_t LogManager::append(LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    lsn_t assigned_lsn = next_lsn_;
    record.set_lsn(assigned_lsn);
    next_lsn_++;

    std::vector<uint8_t> bytes = record.serialize();
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    return assigned_lsn;
}

// ─── flush ───────────────────────────────────────────────────────────────────
// Writes the in-memory buffer to the WAL file up to (at least) up_to_lsn.
// The entire buffer is always written in one shot — partial flushes would
// require tracking byte offsets per LSN, which adds complexity for no gain.

void LogManager::flush(lsn_t up_to_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Already on stable storage — nothing to do.
    if (flushed_lsn_ != INVALID_LSN && flushed_lsn_ >= up_to_lsn) return;

    flush_all_unlocked();
}

// ─── flush_all_unlocked ──────────────────────────────────────────────────────
// Private helper — caller MUST hold mutex_.

void LogManager::flush_all_unlocked() {
    if (buffer_.empty()) return;

    log_file_.write(reinterpret_cast<const char*>(buffer_.data()),
                    static_cast<std::streamsize>(buffer_.size()));
    log_file_.flush();

    if (next_lsn_ > 0) flushed_lsn_ = next_lsn_ - 1;

    buffer_.clear();
}

// ─── flush_all (public) ──────────────────────────────────────────────────────

void LogManager::flush_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_all_unlocked();
}

// ─── read_log ────────────────────────────────────────────────────────────────
// Opens a fresh ifstream so the write cursor is never disturbed.
// Reads records sequentially until EOF or a truncated record.

std::vector<LogRecord> LogManager::read_log() {
    std::vector<LogRecord> records;

    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in.is_open()) return records;

    while (true) {
        // Step 1: read the 4-byte total_size prefix.
        uint32_t total_size = 0;
        in.read(reinterpret_cast<char*>(&total_size), sizeof(total_size));
        if (in.eof() || in.gcount() < static_cast<std::streamsize>(sizeof(total_size))) break;

        // Step 2: allocate buffer and copy the size prefix we already read.
        std::vector<uint8_t> buf(total_size);
        std::memcpy(buf.data(), &total_size, sizeof(total_size));

        // Step 3: read the rest of the record.
        uint32_t remaining = total_size - static_cast<uint32_t>(sizeof(total_size));
        in.read(reinterpret_cast<char*>(buf.data() + sizeof(total_size)),
                static_cast<std::streamsize>(remaining));
        if (in.gcount() < static_cast<std::streamsize>(remaining)) break;

        records.push_back(LogRecord::deserialize(buf.data(), total_size));
    }

    return records;
}

// ─── read_record_at_lsn ──────────────────────────────────────────────────────
// Flushes the in-memory buffer first (so any recently appended records are
// on disk), then scans the WAL file looking for the target LSN.
// Used by TransactionManager::abort() to walk the undo chain.
// Returns a default (INVALID) LogRecord if the LSN is not found.

LogRecord LogManager::read_record_at_lsn(lsn_t target_lsn) {
    // Ensure the record is on disk before we try to read it.
    flush_all();

    auto all_records = read_log();
    for (auto& r : all_records) {
        if (r.get_lsn() == target_lsn) return r;
    }
    return LogRecord{};  // INVALID — caller should check get_type() != INVALID
}

// ─── getters ─────────────────────────────────────────────────────────────────

lsn_t LogManager::get_flushed_lsn() {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushed_lsn_;
}

lsn_t LogManager::get_next_lsn() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_lsn_;
}