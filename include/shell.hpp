#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// PersistX — Interactive Shell
// ═══════════════════════════════════════════════════════════════════════════════
//
// REPL interface that exposes every layer of the storage engine through
// user-friendly commands with beautiful terminal output.
//
// The shell owns the full storage stack (DiskManager → BufferManager →
// LogManager → TransactionManager → BTreeIndex) and wires them together
// on construction.
// ═══════════════════════════════════════════════════════════════════════════════

#include "common.hpp"
#include "disk_manager.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "transaction_manager.hpp"
#include "checkpoint_manager.hpp"
#include "recovery_manager.hpp"
#include "btree_index.hpp"
#include "page.hpp"

#include <string>
#include <vector>
#include <memory>
#include <chrono>

class Shell {
public:
    explicit Shell(const std::string& db_path);
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Main REPL loop — blocks until the user types "exit" or "quit".
    void run();

private:
    // ── Storage stack (owned) ────────────────────────────────────────────────
    std::string db_path_;

    DiskManager*        disk_mgr_  = nullptr;
    LogManager*         log_mgr_   = nullptr;
    BufferManager*      buf_mgr_   = nullptr;
    TransactionManager* txn_mgr_   = nullptr;
    CheckpointManager*  ckpt_mgr_  = nullptr;
    BTreeIndex*         index_     = nullptr;

    void init_storage();
    void shutdown_storage();

    // ── Shell state ──────────────────────────────────────────────────────────
    Transaction* active_txn_ = nullptr;
    bool running_            = true;
    page_id_t data_page_id_  = INVALID_PAGE_ID;   // current page for value storage

    // ── Stats ────────────────────────────────────────────────────────────────
    uint64_t total_puts_    = 0;
    uint64_t total_gets_    = 0;
    uint64_t total_deletes_ = 0;
    uint64_t total_scans_   = 0;
    uint64_t total_commits_ = 0;
    uint64_t total_aborts_  = 0;

    // ── Command handlers ─────────────────────────────────────────────────────

    void cmd_put(const std::vector<std::string>& args);
    void cmd_get(const std::vector<std::string>& args);
    void cmd_del(const std::vector<std::string>& args);
    void cmd_scan(const std::vector<std::string>& args);
    void cmd_update(const std::vector<std::string>& args);

    void cmd_begin();
    void cmd_commit();
    void cmd_abort();

    void cmd_status();
    void cmd_tree();
    void cmd_buffer();
    void cmd_wal(const std::vector<std::string>& args);
    void cmd_page_cmd(const std::vector<std::string>& args);

    void cmd_flush();
    void cmd_checkpoint();
    void cmd_crash();
    void cmd_recover();
    void cmd_fill(const std::vector<std::string>& args);
    void cmd_bench(const std::vector<std::string>& args);
    void cmd_help();
    void cmd_clear();

    // ── Helpers ──────────────────────────────────────────────────────────────

    std::vector<std::string> tokenize(const std::string& line);
    void dispatch(const std::vector<std::string>& tokens);

    // Store a value string on a data page and return its RID.
    RID store_value(const std::string& value);

    // Read a value string from a data page given its RID.
    std::string fetch_value(const RID& rid);

    // Helpers for log record type names
    static std::string log_type_name(LogRecordType type);

    // Build a WAL-compatible record: [8-byte key][value bytes]
    static std::vector<uint8_t> make_record(int64_t key, const std::string& value) {
        std::vector<uint8_t> rec(sizeof(int64_t) + value.size());
        std::memcpy(rec.data(), &key, sizeof(int64_t));
        std::memcpy(rec.data() + sizeof(int64_t), value.data(), value.size());
        return rec;
    }
};
