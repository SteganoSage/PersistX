#pragma once

#include "common.hpp"
#include "log_record.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <mutex>

class LogManager {
public:
    explicit LogManager(const std::string& log_file_path);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // Assign an LSN to the record, serialize it, and buffer in memory.
    // Does NOT write to disk. Returns the assigned LSN.
    lsn_t append(LogRecord& record);

    // Flush buffered records up to and including up_to_lsn.
    // Returns immediately if already flushed past that point.
    void flush(lsn_t up_to_lsn);

    // Flush everything currently in the buffer unconditionally.
    void flush_all();

    // Read the entire WAL file from the beginning and return all records.
    // Used by RecoveryManager on startup.
    std::vector<LogRecord> read_log();

    // Find and return the single log record with the given LSN.
    // Flushes the in-memory buffer first so the record is guaranteed to be
    // on disk, then scans the file.
    // Returns an INVALID LogRecord (type == INVALID) if not found.
    LogRecord read_record_at_lsn(lsn_t target_lsn);

    // The highest LSN that has been written to stable storage.
    lsn_t get_flushed_lsn();

    // The next LSN that will be assigned (useful for testing).
    lsn_t get_next_lsn();

private:
    std::string  log_file_path_;
    std::fstream log_file_;
    std::vector<uint8_t> buffer_;

    lsn_t next_lsn_{0};
    lsn_t flushed_lsn_{INVALID_LSN};

    std::mutex mutex_;
};