#pragma once

#include "common.hpp"
#include "log_record.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <mutex>

/**
 * @brief The Write-Ahead Log engine.
 *
 * Owns the WAL file on disk and an in-memory log buffer. Assigns globally
 * unique LSNs to every log record, buffers serialized records in memory,
 * and flushes them to stable storage on demand. Tracks flushedLSN so the
 * BufferManager can enforce the WAL protocol before writing dirty pages.
 *
 * Thread safety: a single mutex protects the buffer, next_lsn_, and
 * flushed_lsn_. All of append() and flush() run under this mutex.
 */
class LogManager {
public:

    explicit LogManager(const std::string& log_file_path);

    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // Assign an LSN to the record, serialize it, and append to the
    // in-memory buffer. Returns the assigned LSN.
    lsn_t append(LogRecord& record);

    // Flush all buffered log records to stable storage, up to and
    // including the record with the given LSN. If already flushed,
    // returns immediately.
    void flush(lsn_t up_to_lsn);

    // Flush everything currently in the buffer, regardless of LSN.
    void flush_all();

    // Read the entire WAL file from the beginning and return all
    // log records in order. Used by RecoveryManager on startup.
    std::vector<LogRecord> read_log();

    // The highest LSN that has been written to stable storage.
    lsn_t get_flushed_lsn();

    // The next LSN that will be assigned (useful for testing).
    lsn_t get_next_lsn();

private:
    std::string log_file_path_;
    std::fstream log_file_;

    // In-memory buffer: accumulates serialized log records between flushes.
    std::vector<uint8_t> buffer_;

    // The next LSN to assign. Starts at 0, increments by 1 per record.
    lsn_t next_lsn_{0};

    // The highest LSN that has been fsync'd to disk. Starts at INVALID_LSN
    // (meaning nothing has been flushed yet).
    lsn_t flushed_lsn_{INVALID_LSN};

    // Protects buffer_, next_lsn_, and flushed_lsn_.
    std::mutex mutex_;
};
