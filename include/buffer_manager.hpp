#pragma once

#include "disk_manager.hpp"
#include "log_manager.hpp"
#include "replacer.hpp"
#include "page.hpp"
#include <unordered_map>
#include <list>
#include <mutex>
#include <vector>

class BufferManager {
public:
    // ── Original constructor (no WAL) — keeps old tests working ──────────────
    BufferManager(DiskManager* disk_manager, size_t pool_size);

    // ── WAL-aware constructor (Phase 5) ───────────────────────────────────────
    // log_manager is used to enforce the Write-Ahead Logging rule before any
    // dirty page is written to disk.
    BufferManager(DiskManager* disk_manager, LogManager* log_manager, size_t pool_size);

    ~BufferManager();

    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    Page* fetch_page(page_id_t page_id);
    bool  unpin_page(page_id_t page_id, bool is_dirty);
    bool  flush_page(page_id_t page_id);
    void  flush_all_pages();
    Page* new_page(page_id_t& page_id);
    bool  delete_page(page_id_t page_id);

    std::vector<std::pair<page_id_t, lsn_t>> get_dirty_pages();

    // ── introspection (for CLI shell) ─────────────────────────────────────────

    struct FrameInfo {
        frame_id_t frame_id;
        page_id_t  page_id;      // INVALID_PAGE_ID if free
        int32_t    pin_count;
        bool       dirty;
        lsn_t      page_lsn;
    };

    std::vector<FrameInfo> get_frame_info() const;
    size_t get_pool_size() const { return pool_size_; }

private:
    // ── WAL helper ────────────────────────────────────────────────────────────
    // Before writing frame to disk, flush the log up to the page's LSN.
    // If log_manager_ is null (no-WAL mode) this is a no-op.
    void enforce_wal(frame_id_t frame);

    // ── shared init logic used by both constructors ───────────────────────────
    void init_pool();

    DiskManager* disk_manager_;
    LogManager*  log_manager_{nullptr};  // may be null in no-WAL mode
    size_t       pool_size_;

    bool*      dirty_;
    int32_t*   pin_count_;
    page_id_t* page_ids_;
    Page*      pages_;

    std::unordered_map<page_id_t, frame_id_t> page_table_;
    Replacer    replacer_;
    std::list<frame_id_t> free_list_;

    // Protects page_table_, free_list_, pin_count_, dirty_, page_ids_.
    std::mutex bm_mutex_;
};