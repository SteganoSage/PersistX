#include "shell.hpp"
#include "terminal.hpp"
#include "recovery_manager.hpp"
#include <iostream>
#include <sstream>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

static constexpr size_t POOL_SIZE = 64;

Shell::Shell(const std::string& db_path) : db_path_(db_path) {
    init_storage();
}

Shell::~Shell() { shutdown_storage(); }

void Shell::init_storage() {
    disk_mgr_ = new DiskManager(db_path_ + ".db");
    log_mgr_  = new LogManager(db_path_ + ".log");
    buf_mgr_  = new BufferManager(disk_mgr_, log_mgr_, POOL_SIZE);

    // index_ must be allocated BEFORE txn_mgr_ so we can pass the pointer in.
    // Note: index_->create() or index_->load() is called below after the WAL
    // check — the BTreeIndex object itself is valid as soon as it's constructed.
    index_    = new BTreeIndex(buf_mgr_);
    txn_mgr_  = new TransactionManager(log_mgr_, buf_mgr_, index_);
    ckpt_mgr_ = new CheckpointManager(log_mgr_, buf_mgr_, txn_mgr_);

    // Check if WAL has records — if so, this is a restart (clean or crash)
    auto wal_records = log_mgr_->read_log();
    bool needs_recovery = !wal_records.empty();

    if (!needs_recovery) {
        // Fresh database — create tree root and first data page
        index_->create();
        page_id_t dpid;
        Page* dp = buf_mgr_->new_page(dpid);
        dp->init(dpid, PageType::DATA);
        buf_mgr_->unpin_page(dpid, true);
        data_page_id_ = dpid;
    } else {
        // Existing database — always run ARIES recovery on startup.
        // Real databases (PostgreSQL, MySQL) do this on every boot.
        // If the last shutdown was clean, recovery is a no-op.
        std::cout << "  " << term::bold_cyan("\xf0\x9f\x94\x84 Running ARIES recovery...") << "\n";
        auto start = std::chrono::high_resolution_clock::now();

        RecoveryManager rm(log_mgr_, buf_mgr_);
        rm.recover();

        auto t_aries = std::chrono::high_resolution_clock::now();
        double aries_ms = std::chrono::duration<double, std::milli>(t_aries - start).count();
        std::cout << "  " << term::bold_green("\xe2\x9c\x93 ARIES complete")
                  << term::dim(" (" + std::to_string(aries_ms).substr(0, 5) + "ms)") << "\n";

        // Rebuild B+ tree index from recovered data pages.
        // We use the WAL records to find which pages have data,
        // since disk_mgr may not know about pages that only lived in buffer pool.

        // Sync disk_mgr page counter so new allocations don't collide
        // with pages that ARIES loaded into the buffer pool.
        page_id_t max_pid = 0;
        std::set<page_id_t> data_pages;
        for (auto& rec : wal_records) {
            if (rec.get_type() == LogRecordType::UPDATE || rec.get_type() == LogRecordType::CLR) {
                data_pages.insert(rec.get_page_id());
                if (rec.get_page_id() >= max_pid)
                    max_pid = rec.get_page_id() + 1;
            }
        }
        disk_mgr_->set_num_pages(max_pid);

        std::cout << "  " << term::bold_cyan("\xf0\x9f\x94\xa7 Rebuilding B+ tree index...") << "\n";
        index_->create();  // fresh tree — allocates AFTER existing pages

        int keys_recovered = 0;
        for (page_id_t pid : data_pages) {
            Page* page = buf_mgr_->fetch_page(pid);
            if (!page) continue;

            uint16_t slot_count = page->get_slot_count();
            for (uint16_t s = 0; s < slot_count; ++s) {
                std::vector<uint8_t> data;
                if (!page->read_record(s, data)) continue;
                if (data.size() < sizeof(int64_t)) continue;

                int64_t key;
                std::memcpy(&key, data.data(), sizeof(int64_t));
                index_->insert(key, RID{pid, s});
                ++keys_recovered;
            }

            buf_mgr_->unpin_page(pid, false);
            data_page_id_ = pid;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "  " << term::bold_green("\xe2\x9c\x93 Ready")
                  << term::dim(" (" + std::to_string(keys_recovered) + " keys, "
                     + std::to_string(total_ms).substr(0, 6) + "ms)") << "\n\n";
    }
}

void Shell::shutdown_storage() {
    if (active_txn_) { txn_mgr_->abort(active_txn_); active_txn_ = nullptr; }
    if (buf_mgr_) buf_mgr_->flush_all_pages();
    delete index_;    index_    = nullptr;
    delete ckpt_mgr_; ckpt_mgr_ = nullptr;
    delete txn_mgr_;  txn_mgr_  = nullptr;
    delete buf_mgr_;  buf_mgr_  = nullptr;
    delete log_mgr_;  log_mgr_  = nullptr;
    delete disk_mgr_; disk_mgr_ = nullptr;
}

void Shell::run() {
    term::enable_ansi();
    term::print_banner();
    std::string line;
    while (running_) {
        term::print_prompt(active_txn_ != nullptr,
                          active_txn_ ? active_txn_->get_txn_id() : 0);
        if (!std::getline(std::cin, line)) break;
        auto tokens = tokenize(line);
        if (!tokens.empty()) dispatch(tokens);
    }
}

std::vector<std::string> Shell::tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    // Handle quoted strings
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            ++i;
            std::string quoted;
            while (i < line.size() && line[i] != '"') quoted += line[i++];
            if (i < line.size()) ++i; // skip closing quote
            tokens.push_back(quoted);
        } else {
            std::string word;
            while (i < line.size() && line[i] != ' ') word += line[i++];
            tokens.push_back(word);
        }
    }
    return tokens;
}

void Shell::dispatch(const std::vector<std::string>& tokens) {
    const auto& cmd = tokens[0];
    if      (cmd == "put")        cmd_put(tokens);
    else if (cmd == "get")        cmd_get(tokens);
    else if (cmd == "del")        cmd_del(tokens);
    else if (cmd == "scan")       cmd_scan(tokens);
    else if (cmd == "update")     cmd_update(tokens);
    else if (cmd == "begin")      cmd_begin();
    else if (cmd == "commit")     cmd_commit();
    else if (cmd == "abort")      cmd_abort();
    else if (cmd == "status")     cmd_status();
    else if (cmd == "tree")       cmd_tree();
    else if (cmd == "buffer")     cmd_buffer();
    else if (cmd == "wal")        cmd_wal(tokens);
    else if (cmd == "page")       cmd_page_cmd(tokens);
    else if (cmd == "flush")      cmd_flush();
    else if (cmd == "checkpoint") cmd_checkpoint();
    else if (cmd == "crash")      cmd_crash();
    else if (cmd == "recover")    cmd_recover();
    else if (cmd == "fill")       cmd_fill(tokens);
    else if (cmd == "bench")      cmd_bench(tokens);
    else if (cmd == "help")       cmd_help();
    else if (cmd == "clear")      cmd_clear();
    else if (cmd == "exit" || cmd == "quit") {
        std::cout << "\n  " << term::dim("Shutting down...") << "\n\n";
        running_ = false;
    }
    else {
        std::cout << "  " << term::red("Unknown command: ") << cmd
                  << term::dim("  (type 'help' for commands)") << "\n";
    }
}

RID Shell::store_value(const std::string& value) {
    // Record format: [8 bytes: int64_t key][N bytes: value]
    // Key is prepended by cmd_put before calling this; here we just store raw bytes.
    Page* page = buf_mgr_->fetch_page(data_page_id_);
    assert(page);
    slot_id_t slot = page->insert_record(
        reinterpret_cast<const uint8_t*>(value.data()),
        static_cast<uint16_t>(value.size()));

    if (slot == INVALID_SLOT_ID) {
        buf_mgr_->unpin_page(data_page_id_, false);
        page_id_t new_pid;
        page = buf_mgr_->new_page(new_pid);
        assert(page);
        page->init(new_pid, PageType::DATA);
        data_page_id_ = new_pid;
        slot = page->insert_record(
            reinterpret_cast<const uint8_t*>(value.data()),
            static_cast<uint16_t>(value.size()));
    }
    RID rid{data_page_id_, slot};
    buf_mgr_->unpin_page(data_page_id_, true);
    return rid;
}

std::string Shell::fetch_value(const RID& rid) {
    Page* page = buf_mgr_->fetch_page(rid.page_id);
    if (!page) return "<error: page not found>";
    std::vector<uint8_t> data;
    if (!page->read_record(rid.slot_id, data)) {
        buf_mgr_->unpin_page(rid.page_id, false);
        return "<error: record not found>";
    }
    buf_mgr_->unpin_page(rid.page_id, false);
    // Record format: [8 bytes key][value bytes]
    if (data.size() <= sizeof(int64_t)) return "";
    return std::string(data.begin() + sizeof(int64_t), data.end());
}

// Extract key from a raw record
static int64_t record_key(const std::vector<uint8_t>& data) {
    int64_t key = 0;
    if (data.size() >= sizeof(int64_t))
        std::memcpy(&key, data.data(), sizeof(int64_t));
    return key;
}

// Extract value from a raw record
static std::string record_value(const std::vector<uint8_t>& data) {
    if (data.size() <= sizeof(int64_t)) return "";
    return std::string(data.begin() + sizeof(int64_t), data.end());
}

std::string Shell::log_type_name(LogRecordType type) {
    switch (type) {
        case LogRecordType::BEGIN:            return "BEGIN";
        case LogRecordType::UPDATE:           return "UPDATE";
        case LogRecordType::COMMIT:           return "COMMIT";
        case LogRecordType::ABORT:            return "ABORT";
        case LogRecordType::TXN_END:          return "TXN_END";
        case LogRecordType::CLR:              return "CLR";
        case LogRecordType::CHECKPOINT_BEGIN: return "CKPT_BGN";
        case LogRecordType::CHECKPOINT_END:   return "CKPT_END";
        default:                              return "INVALID";
    }
}

// ─── Data commands ───────────────────────────────────────────────────────────

void Shell::cmd_put(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::cout << "  " << term::red("Usage: put <key> <value>") << "\n"; return;
    }
    int64_t key;
    try { key = std::stoll(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid key (must be integer)") << "\n"; return;
    }
    std::string value;
    for (size_t i = 2; i < args.size(); ++i) {
        if (i > 2) value += " ";
        value += args[i];
    }

    // Auto-commit: if no explicit transaction, wrap in one
    bool auto_txn = (active_txn_ == nullptr);
    if (auto_txn) active_txn_ = txn_mgr_->begin();

    // Step 1: Insert key+value record into data page
    auto record = make_record(key, value);
    RID rid = store_value(std::string(record.begin(), record.end()));

    // Step 2: Insert key→RID into B+ tree
    if (!index_->insert(key, rid)) {
        std::cout << "  " << term::yellow("Key " + std::to_string(key) + " already exists") << "\n";
        if (auto_txn) { txn_mgr_->abort(active_txn_); active_txn_ = nullptr; }
        return;
    }

    // Step 3: WAL — log the data page modification.
    // Pass index_key so abort() can remove the ghost B+ tree entry if this
    // transaction is rolled back (fixes Bug #6 — ghost index entries).
    std::vector<uint8_t> old_data;  // empty — this was an insert
    lsn_t lsn = txn_mgr_->log_update(active_txn_, rid.page_id, rid.slot_id,
                                       old_data, record, key);
    // Flush WAL to disk immediately — write-ahead guarantee.
    log_mgr_->flush(lsn);

    // Step 4: Stamp the page with the LSN
    Page* page = buf_mgr_->fetch_page(rid.page_id);
    if (page) {
        page->set_page_lsn(lsn);
        buf_mgr_->unpin_page(rid.page_id, true);
    }

    // Step 5: Auto-commit if this was an implicit transaction
    if (auto_txn) {
        txn_mgr_->commit(active_txn_);
        active_txn_ = nullptr;
        ++total_commits_;
    }

    ++total_puts_;
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 INSERT")
              << "  key=" << term::bold(std::to_string(key))
              << "  \xe2\x86\x92  RID(" << term::magenta(std::to_string(rid.page_id)
                 + "," + std::to_string(rid.slot_id)) << ")"
              << term::dim("  [LSN=" + std::to_string(lsn) + "]") << "\n";
}

void Shell::cmd_get(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "  " << term::red("Usage: get <key>") << "\n"; return;
    }
    int64_t key;
    try { key = std::stoll(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid key") << "\n"; return;
    }
    RID rid = index_->search(key);
    if (!rid.is_valid()) {
        std::cout << "  " << term::yellow("Key " + std::to_string(key) + " not found") << "\n";
        return;
    }
    ++total_gets_;
    std::string val = fetch_value(rid);
    std::cout << "  " << term::bold_cyan(std::to_string(key))
              << " \xe2\x86\x92 " << term::bold(val) << "\n";
}

void Shell::cmd_del(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "  " << term::red("Usage: del <key>") << "\n"; return;
    }
    int64_t key;
    try { key = std::stoll(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid key") << "\n"; return;
    }

    // Look up the RID first (we need it for WAL logging)
    RID rid = index_->search(key);
    if (!rid.is_valid()) {
        std::cout << "  " << term::yellow("Key " + std::to_string(key) + " not found") << "\n";
        return;
    }

    bool auto_txn = (active_txn_ == nullptr);
    if (auto_txn) active_txn_ = txn_mgr_->begin();

    // Step 1: Capture before-image
    std::vector<uint8_t> old_data;
    Page* page = buf_mgr_->fetch_page(rid.page_id);
    if (page) page->read_record(rid.slot_id, old_data);

    // Step 2: Delete from data page
    if (page) {
        page->delete_record(rid.slot_id);
        buf_mgr_->unpin_page(rid.page_id, true);
    }

    // Step 3: Remove from B+ tree
    index_->remove(key);

    // Step 4: WAL log
    std::vector<uint8_t> new_data;  // empty — this was a delete
    lsn_t lsn = txn_mgr_->log_update(active_txn_, rid.page_id, rid.slot_id, old_data, new_data,key);
    log_mgr_->flush(lsn);

    // Step 5: Stamp page LSN
    page = buf_mgr_->fetch_page(rid.page_id);
    if (page) {
        page->set_page_lsn(lsn);
        buf_mgr_->unpin_page(rid.page_id, true);
    }

    if (auto_txn) {
        txn_mgr_->commit(active_txn_);
        active_txn_ = nullptr;
        ++total_commits_;
    }

    ++total_deletes_;
    std::cout << "  " << term::bold_red("\xe2\x9c\x97 DELETE")
              << "  key=" << term::bold(std::to_string(key))
              << term::dim("  [LSN=" + std::to_string(lsn) + "]") << "\n";
}

void Shell::cmd_scan(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::cout << "  " << term::red("Usage: scan <begin> <end>") << "\n"; return;
    }
    int64_t begin_key, end_key;
    try { begin_key = std::stoll(args[1]); end_key = std::stoll(args[2]); }
    catch (...) { std::cout << "  " << term::red("Invalid keys") << "\n"; return; }

    auto rids = index_->range_scan(begin_key, end_key);
    ++total_scans_;

    if (rids.empty()) {
        std::cout << "  " << term::yellow("No keys in range [" + std::to_string(begin_key)
                  + ", " + std::to_string(end_key) + "]") << "\n";
        return;
    }

    std::vector<std::string> headers = {"Key", "Value", "RID"};
    std::vector<std::vector<std::string>> rows;

    for (auto& rid : rids) {
        // Read raw record so we can extract both key and value in one fetch.
        // Previously the key column was hardcoded to "-" (Bug #3) because
        // fetch_value() only returns the value portion of the record.
        Page* page = buf_mgr_->fetch_page(rid.page_id);
        if (!page) continue;

        std::vector<uint8_t> data;
        if (!page->read_record(rid.slot_id, data)) {
            buf_mgr_->unpin_page(rid.page_id, false);
            continue;
        }
        buf_mgr_->unpin_page(rid.page_id, false);

        std::string key_str = std::to_string(record_key(data));
        std::string val     = record_value(data);
        std::string rid_str = std::to_string(rid.page_id) + ":" + std::to_string(rid.slot_id);
        rows.push_back({key_str, val, rid_str});
    }

    std::cout << "\n";
    term::print_table(headers, rows);
    std::cout << "  " << term::dim(std::to_string(rows.size()) + " rows") << "\n\n";
}

void Shell::cmd_update(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::cout << "  " << term::red("Usage: update <key> <value>") << "\n"; return;
    }
    int64_t key;
    try { key = std::stoll(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid key") << "\n"; return;
    }
    RID rid = index_->search(key);
    if (!rid.is_valid()) {
        std::cout << "  " << term::yellow("Key " + std::to_string(key) + " not found") << "\n";
        return;
    }
    std::string value;
    for (size_t i = 2; i < args.size(); ++i) {
        if (i > 2) value += " ";
        value += args[i];
    }

    bool auto_txn = (active_txn_ == nullptr);
    if (auto_txn) active_txn_ = txn_mgr_->begin();

    // Step 1: Capture before-image, then delete + reinsert (size may change)
    std::vector<uint8_t> old_data;
    Page* page = buf_mgr_->fetch_page(rid.page_id);
    if (page) {
        page->read_record(rid.slot_id, old_data);
        page->delete_record(rid.slot_id);
        buf_mgr_->unpin_page(rid.page_id, true);
    }

    // Step 2: Insert new record (key+value) — may go to same or different slot
    auto new_record = make_record(key, value);
    RID new_rid = store_value(std::string(new_record.begin(), new_record.end()));

    // Step 3: Update index if RID changed
    if (new_rid.page_id != rid.page_id || new_rid.slot_id != rid.slot_id) {
        index_->remove(key);
        index_->insert(key, new_rid);
    }

    // Step 4: WAL log (using new RID since record was reinserted)
    auto wal_new_data = make_record(key, value);
    lsn_t lsn = txn_mgr_->log_update(active_txn_, new_rid.page_id, new_rid.slot_id,
                                       old_data, wal_new_data);
    log_mgr_->flush(lsn);

    // Step 5: Stamp page LSN
    page = buf_mgr_->fetch_page(new_rid.page_id);
    if (page) {
        page->set_page_lsn(lsn);
        buf_mgr_->unpin_page(new_rid.page_id, true);
    }

    if (auto_txn) {
        txn_mgr_->commit(active_txn_);
        active_txn_ = nullptr;
        ++total_commits_;
    }

    std::cout << "  " << term::bold_yellow("\xe2\x9c\x93 UPDATE")
              << "  key=" << term::bold(std::to_string(key))
              << term::dim("  [LSN=" + std::to_string(lsn) + "]") << "\n";
}

// ─── Transaction commands ────────────────────────────────────────────────────

void Shell::cmd_begin() {
    if (active_txn_) {
        std::cout << "  " << term::yellow("Transaction already active (txn#"
                  + std::to_string(active_txn_->get_txn_id()) + ")") << "\n";
        return;
    }
    active_txn_ = txn_mgr_->begin();
    std::cout << "  " << term::bold_green("\xe2\x94\x82 BEGIN")
              << "  txn#" << term::bold_magenta(std::to_string(active_txn_->get_txn_id())) << "\n";
}

void Shell::cmd_commit() {
    if (!active_txn_) {
        std::cout << "  " << term::yellow("No active transaction") << "\n"; return;
    }
    txn_id_t id = active_txn_->get_txn_id();
    txn_mgr_->commit(active_txn_);
    active_txn_ = nullptr;
    ++total_commits_;
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 COMMIT")
              << "  txn#" << term::bold_magenta(std::to_string(id)) << "\n";
}

void Shell::cmd_abort() {
    if (!active_txn_) {
        std::cout << "  " << term::yellow("No active transaction") << "\n"; return;
    }
    txn_id_t id = active_txn_->get_txn_id();
    txn_mgr_->abort(active_txn_);
    active_txn_ = nullptr;
    ++total_aborts_;
    std::cout << "  " << term::bold_red("\xe2\x9c\x97 ABORT")
              << "  txn#" << term::bold_magenta(std::to_string(id)) << "\n";
}

// ─── Help ────────────────────────────────────────────────────────────────────

void Shell::cmd_help() {
    std::cout << "\n";
    std::cout << "  " << term::bold_cyan("Data Operations:") << "\n";
    std::cout << "    " << term::bold("put") << " <key> <value>    Insert a key-value pair\n";
    std::cout << "    " << term::bold("get") << " <key>            Look up a key\n";
    std::cout << "    " << term::bold("del") << " <key>            Delete a key\n";
    std::cout << "    " << term::bold("update") << " <key> <val>   Update value for key\n";
    std::cout << "    " << term::bold("scan") << " <begin> <end>   Range scan\n";
    std::cout << "\n";
    std::cout << "  " << term::bold_cyan("Transactions:") << "\n";
    std::cout << "    " << term::bold("begin") << "                Start transaction\n";
    std::cout << "    " << term::bold("commit") << "               Commit transaction\n";
    std::cout << "    " << term::bold("abort") << "                Abort transaction\n";
    std::cout << "\n";
    std::cout << "  " << term::bold_cyan("Diagnostics:") << "\n";
    std::cout << "    " << term::bold("status") << "               System overview\n";
    std::cout << "    " << term::bold("tree") << "                 B+ tree visualization\n";
    std::cout << "    " << term::bold("buffer") << "               Buffer pool frames\n";
    std::cout << "    " << term::bold("wal") << " [n]              Last N WAL entries\n";
    std::cout << "    " << term::bold("page") << " <id>            Inspect a page\n";
    std::cout << "\n";
    std::cout << "  " << term::bold_cyan("System:") << "\n";
    std::cout << "    " << term::bold("flush") << "                Flush dirty pages\n";
    std::cout << "    " << term::bold("checkpoint") << "           Trigger checkpoint\n";
    std::cout << "    " << term::bold("fill") << " <n>             Bulk insert N keys\n";
    std::cout << "    " << term::bold("bench") << " <n>            Benchmark N ops\n";
    std::cout << "    " << term::bold("crash") << "                Simulate crash\n";
    std::cout << "    " << term::bold("recover") << "              Run ARIES recovery\n";
    std::cout << "    " << term::bold("clear") << "                Clear screen\n";
    std::cout << "    " << term::bold("exit") << "                 Quit\n";
    std::cout << "\n";
}