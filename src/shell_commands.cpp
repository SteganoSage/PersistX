#include "shell.hpp"
#include "terminal.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdlib>

// ─── Diagnostics ─────────────────────────────────────────────────────────────

void Shell::cmd_status() {
    std::cout << "\n";
    auto stats = index_->get_stats();
    auto frames = buf_mgr_->get_frame_info();
    size_t used = 0, dirty_count = 0;
    for (auto& f : frames) {
        if (f.page_id != INVALID_PAGE_ID) ++used;
        if (f.dirty) ++dirty_count;
    }

    term::print_two_boxes(
        "Buffer Pool",
        {{"Pool size:", std::to_string(buf_mgr_->get_pool_size()) + " frames"},
         {"Used:",      std::to_string(used) + " / " + std::to_string(buf_mgr_->get_pool_size())},
         {"Dirty:",     std::to_string(dirty_count)},
         {"Disk pages:", std::to_string(disk_mgr_->get_num_pages())}},
        "WAL",
        {{"Flushed LSN:", std::to_string(log_mgr_->get_flushed_lsn())},
         {"Next LSN:",    std::to_string(log_mgr_->get_next_lsn())},
         {"Log file:",    db_path_ + ".log"}},
        34
    );

    term::print_two_boxes(
        "B+ Tree",
        {{"Root page:",   std::to_string(index_->get_root_page_id())},
         {"Total keys:",  std::to_string(stats.total_keys)},
         {"Tree height:", std::to_string(stats.height)},
         {"Leaf pages:",  std::to_string(stats.leaf_pages)},
         {"Internal:",    std::to_string(stats.internal_pages)}},
        "Session Stats",
        {{"Puts:",    std::to_string(total_puts_)},
         {"Gets:",    std::to_string(total_gets_)},
         {"Deletes:", std::to_string(total_deletes_)},
         {"Scans:",   std::to_string(total_scans_)},
         {"Commits:", std::to_string(total_commits_)},
         {"Aborts:",  std::to_string(total_aborts_)}},
        34
    );
    std::cout << "\n";
}

void Shell::cmd_tree() {
    auto layout = index_->get_tree_layout();
    if (layout.empty()) {
        std::cout << "  " << term::yellow("Tree is empty") << "\n"; return;
    }
    std::cout << "\n";
    for (size_t lvl = 0; lvl < layout.size(); ++lvl) {
        std::string label = (lvl == 0) ? "root" : (lvl == layout.size()-1 ? "leaf" : "L" + std::to_string(lvl));
        std::cout << "  " << term::bold_cyan("Level " + std::to_string(lvl))
                  << term::dim(" (" + label + ")") << "  ";
        for (auto& node : layout[lvl]) {
            std::cout << term::cyan("[");
            for (size_t k = 0; k < node.keys.size(); ++k) {
                if (k > 0) std::cout << term::dim(",");
                // Limit display to first 8 keys per node
                if (k >= 8) {
                    std::cout << term::dim("...+" + std::to_string(node.keys.size() - 8));
                    break;
                }
                std::cout << term::bold(std::to_string(node.keys[k]));
            }
            std::cout << term::cyan("]");
            std::cout << term::dim("p" + std::to_string(node.page_id)) << " ";
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

void Shell::cmd_buffer() {
    auto frames = buf_mgr_->get_frame_info();
    std::vector<std::string> headers = {"Frame", "Page ID", "Pin", "Dirty", "LSN"};
    std::vector<std::vector<std::string>> rows;
    size_t empty = 0;
    for (auto& f : frames) {
        if (f.page_id == INVALID_PAGE_ID) {
            ++empty;
            continue;  // skip empty frames
        }
        rows.push_back({
            std::to_string(f.frame_id),
            std::to_string(f.page_id),
            std::to_string(f.pin_count),
            f.dirty ? "\xe2\x9c\x93" : "\xe2\x9c\x97",
            f.page_lsn == INVALID_LSN ? "-" : std::to_string(f.page_lsn)
        });
    }
    std::cout << "\n";
    if (rows.empty()) {
        std::cout << "  " << term::yellow("Buffer pool is empty") << "\n";
    } else {
        term::print_table(headers, rows);
    }
    std::cout << "  " << term::dim(std::to_string(rows.size()) + " used, "
              + std::to_string(empty) + " free of "
              + std::to_string(frames.size()) + " frames") << "\n\n";
}

void Shell::cmd_wal(const std::vector<std::string>& args) {
    int n = 10;
    if (args.size() >= 2) {
        try { n = std::stoi(args[1]); } catch (...) { n = 10; }
    }
    auto records = log_mgr_->read_log();
    if (records.empty()) {
        std::cout << "  " << term::yellow("WAL is empty") << "\n"; return;
    }

    // Show last N records
    size_t start = records.size() > static_cast<size_t>(n) ? records.size() - static_cast<size_t>(n) : 0;
    std::vector<std::string> headers = {"LSN", "Txn", "Type", "Page:Slot", "PrevLSN"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = start; i < records.size(); ++i) {
        auto& r = records[i];
        std::string ps = "-";
        if (r.get_type() == LogRecordType::UPDATE || r.get_type() == LogRecordType::CLR)
            ps = std::to_string(r.get_page_id()) + ":" + std::to_string(r.get_slot_id());
        std::string prev = r.get_prev_lsn() == INVALID_LSN ? "-" : std::to_string(r.get_prev_lsn());
        std::string txn = r.get_txn_id() == INVALID_TXN_ID ? "-" : std::to_string(r.get_txn_id());
        rows.push_back({std::to_string(r.get_lsn()), txn, log_type_name(r.get_type()), ps, prev});
    }
    std::cout << "\n";
    term::print_table(headers, rows);
    std::cout << "  " << term::dim("Showing " + std::to_string(rows.size()) + " of "
              + std::to_string(records.size()) + " records") << "\n\n";
}

void Shell::cmd_page_cmd(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "  " << term::red("Usage: page <page_id>") << "\n"; return;
    }
    page_id_t pid;
    try { pid = static_cast<page_id_t>(std::stoul(args[1])); } catch (...) {
        std::cout << "  " << term::red("Invalid page ID") << "\n"; return;
    }
    Page* page = buf_mgr_->fetch_page(pid);
    if (!page) {
        std::cout << "  " << term::red("Page " + std::to_string(pid) + " not found") << "\n"; return;
    }

    // Check if it's a B+ tree page by reading node_type byte
    uint8_t node_type;
    std::memcpy(&node_type, page->raw() + 4, sizeof(node_type));

    std::cout << "\n";
    if (node_type == 0 || node_type == 1) {
        // B+ tree page
        uint16_t num_keys;
        std::memcpy(&num_keys, page->raw() + 5, sizeof(num_keys));
        page_id_t parent;
        std::memcpy(&parent, page->raw() + 7, sizeof(parent));
        std::string type_str = node_type == 1 ? "LEAF" : "INTERNAL";

        term::print_box("Page " + std::to_string(pid) + " (" + type_str + ")",
            {{"Node type:", type_str},
             {"Num keys:",  std::to_string(num_keys)},
             {"Parent:",    parent == INVALID_PAGE_ID ? "none" : std::to_string(parent)}}, 34);
    } else {
        // Data page — use slotted page header
        term::print_box("Page " + std::to_string(pid) + " (DATA)",
            {{"Page ID:",    std::to_string(page->get_page_id())},
             {"Slots:",      std::to_string(page->get_slot_count())},
             {"Tombstones:", std::to_string(page->get_tombstone_count())},
             {"Free space:", std::to_string(page->free_space()) + " bytes"},
             {"Page LSN:",   std::to_string(page->get_page_lsn())}}, 34);
    }
    buf_mgr_->unpin_page(pid, false);
    std::cout << "\n";
}

// ─── System commands ─────────────────────────────────────────────────────────

void Shell::cmd_flush() {
    buf_mgr_->flush_all_pages();
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 Flushed") << " all dirty pages to disk\n";
}

void Shell::cmd_checkpoint() {
    ckpt_mgr_->checkpoint();
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 Checkpoint") << " complete\n";
}

void Shell::cmd_crash() {
    std::cout << "\n  " << term::bold_red("\xf0\x9f\x92\xa5 CRASH")
              << term::dim(" — killing process without flushing!") << "\n";
    std::cout << "  " << term::dim("Restart and run 'recover' to test ARIES.") << "\n\n";
    std::cout.flush();
    _Exit(1);  // Immediate termination — no destructors, no flush
}

void Shell::cmd_recover() {
    std::cout << "  " << term::bold_cyan("\xf0\x9f\x94\x84 Running ARIES recovery...") << "\n";
    auto start = std::chrono::high_resolution_clock::now();

    RecoveryManager rm(log_mgr_, buf_mgr_);
    rm.recover();

    auto t_aries = std::chrono::high_resolution_clock::now();
    double aries_ms = std::chrono::duration<double, std::milli>(t_aries - start).count();
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 ARIES complete")
              << term::dim(" (" + std::to_string(aries_ms).substr(0, 5) + "ms)") << "\n";

    // Phase 2: Rebuild B+ tree index from data pages
    std::cout << "  " << term::bold_cyan("\xf0\x9f\x94\xa7 Rebuilding B+ tree index...") << "\n";

    // Destroy old tree and create fresh one
    delete index_;
    index_ = new BTreeIndex(buf_mgr_);
    index_->create();

    // Scan all pages looking for data pages with records
    uint32_t num_pages = disk_mgr_->get_num_pages();
    int keys_recovered = 0;

    for (uint32_t pid = 0; pid < num_pages; ++pid) {
        Page* page = buf_mgr_->fetch_page(pid);
        if (!page) continue;

        // Check if this is a data page by looking at page type
        // Data pages have PageType::DATA (0x02) at offset 2 in the header
        PageType ptype = page->get_page_type();
        if (ptype != PageType::DATA) {
            buf_mgr_->unpin_page(pid, false);
            continue;
        }

        // Scan all slots in this data page
        uint16_t slot_count = page->get_slot_count();
        for (uint16_t s = 0; s < slot_count; ++s) {
            std::vector<uint8_t> data;
            if (!page->read_record(s, data)) continue;  // tombstone or empty
            if (data.size() < sizeof(int64_t)) continue; // too small

            // Extract key from record: [8 bytes key][value]
            int64_t key;
            std::memcpy(&key, data.data(), sizeof(int64_t));

            RID rid{pid, s};
            index_->insert(key, rid);
            ++keys_recovered;
        }

        buf_mgr_->unpin_page(pid, false);
        data_page_id_ = pid;  // track last data page for future inserts
    }

    auto end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "  " << term::bold_green("\xe2\x9c\x93 Index rebuilt")
              << term::dim(" (" + std::to_string(keys_recovered) + " keys recovered)")
              << "\n";
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 Recovery complete")
              << term::dim(" (" + std::to_string(total_ms).substr(0, 6) + "ms total)") << "\n";
}

void Shell::cmd_fill(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "  " << term::red("Usage: fill <count>") << "\n"; return;
    }
    int n;
    try { n = std::stoi(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid count") << "\n"; return;
    }
    std::cout << "  " << term::dim("Inserting " + std::to_string(n) + " keys...") << "\n";

    auto start = std::chrono::high_resolution_clock::now();
    int inserted = 0;

    // Single transaction for the whole batch (group commit)
    auto* txn = txn_mgr_->begin();

    for (int i = 0; i < n; ++i) {
        int64_t key = static_cast<int64_t>(i);
        std::string val = "val_" + std::to_string(i);
        auto record = make_record(key, val);
        RID rid = store_value(std::string(record.begin(), record.end()));

        if (!index_->insert(key, rid)) continue;  // duplicate key

        // WAL log (no per-key flush — we flush once at commit)
        std::vector<uint8_t> old_data;
        lsn_t lsn = txn_mgr_->log_update(txn, rid.page_id, rid.slot_id,
                                           old_data, record);
        Page* page = buf_mgr_->fetch_page(rid.page_id);
        if (page) { page->set_page_lsn(lsn); buf_mgr_->unpin_page(rid.page_id, true); }
        ++inserted;
    }

    // Single WAL flush + commit
    txn_mgr_->commit(txn);
    ++total_commits_;

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    total_puts_ += static_cast<uint64_t>(inserted);
    double ops = (ms > 0) ? (inserted / ms * 1000.0) : 0;
    std::cout << "  " << term::bold_green("\xe2\x9c\x93 Inserted " + std::to_string(inserted) + " keys")
              << term::dim(" in " + std::to_string(ms).substr(0, 6) + "ms")
              << term::dim(" (" + std::to_string(static_cast<int>(ops)) + " ops/sec)") << "\n";
}

void Shell::cmd_bench(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "  " << term::red("Usage: bench <count>") << "\n"; return;
    }
    int n;
    try { n = std::stoi(args[1]); } catch (...) {
        std::cout << "  " << term::red("Invalid count") << "\n"; return;
    }

    int base = static_cast<int>(total_puts_) + 100000;  // avoid key conflicts

    // Phase 1: Inserts (single transaction — group commit)
    auto* txn = txn_mgr_->begin();

    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < n; ++i) {
        int64_t key = static_cast<int64_t>(base + i);
        auto record = make_record(key, "bench_" + std::to_string(i));
        RID rid = store_value(std::string(record.begin(), record.end()));
        index_->insert(key, rid);

        std::vector<uint8_t> old_data;
        lsn_t lsn = txn_mgr_->log_update(txn, rid.page_id, rid.slot_id,
                                           old_data, record);
        Page* page = buf_mgr_->fetch_page(rid.page_id);
        if (page) { page->set_page_lsn(lsn); buf_mgr_->unpin_page(rid.page_id, true); }
    }
    auto t2 = std::chrono::high_resolution_clock::now();

    txn_mgr_->commit(txn);
    ++total_commits_;

    double insert_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // Phase 2: Lookups
    auto t3 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < n; ++i) {
        int64_t key = static_cast<int64_t>(base + i);
        index_->search(key);
    }
    auto t4 = std::chrono::high_resolution_clock::now();
    double lookup_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();

    auto stats = index_->get_stats();
    double ins_ops = (insert_ms > 0) ? (n / insert_ms * 1000.0) : 0;
    double lkp_ops = (lookup_ms > 0) ? (n / lookup_ms * 1000.0) : 0;

    std::cout << "\n";
    term::print_box("Benchmark Results",
        {{"Inserts:",     std::to_string(n) + " in " + std::to_string(insert_ms).substr(0,6) + "ms"},
         {"Insert ops/s:", std::to_string(static_cast<int>(ins_ops))},
         {"Lookups:",     std::to_string(n) + " in " + std::to_string(lookup_ms).substr(0,6) + "ms"},
         {"Lookup ops/s:", std::to_string(static_cast<int>(lkp_ops))},
         {"Tree height:", std::to_string(stats.height)},
         {"Leaf pages:",  std::to_string(stats.leaf_pages)}}, 42);
    std::cout << "\n";
    total_puts_ += static_cast<uint64_t>(n);
    total_gets_ += static_cast<uint64_t>(n);
}

void Shell::cmd_clear() { term::clear_screen(); }
