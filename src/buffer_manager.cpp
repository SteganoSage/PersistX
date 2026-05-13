#include "buffer_manager.hpp"
#include <stdexcept>

// ─── private helpers ─────────────────────────────────────────────────────────

void BufferManager::init_pool() {
    for (size_t i = 0; i < pool_size_; ++i) {
        dirty_[i]    = false;
        pin_count_[i] = 0;
        page_ids_[i] = INVALID_PAGE_ID;
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

// WAL rule: before any dirty frame hits disk, every log record that modified
// that page must already be on stable storage.
// If log_manager_ is null (no-WAL mode) this is a no-op.
void BufferManager::enforce_wal(frame_id_t frame) {
    if (log_manager_ == nullptr) return;
    // flush() is safe to call unconditionally — it short-circuits if already
    // flushed past the page's LSN, and returns immediately if buffer is empty.
    log_manager_->flush(pages_[frame].get_page_lsn());
}

// ─── constructors ────────────────────────────────────────────────────────────

BufferManager::BufferManager(DiskManager* disk_manager, size_t pool_size)
    : disk_manager_(disk_manager),
      log_manager_(nullptr),
      pool_size_(pool_size),
      dirty_(new bool[pool_size]{}),
      pin_count_(new int32_t[pool_size]{}),
      page_ids_(new page_id_t[pool_size]{}),
      pages_(new Page[pool_size]),
      replacer_(pool_size) {
    init_pool();
}

BufferManager::BufferManager(DiskManager* disk_manager, LogManager* log_manager,
                             size_t pool_size)
    : disk_manager_(disk_manager),
      log_manager_(log_manager),
      pool_size_(pool_size),
      dirty_(new bool[pool_size]{}),
      pin_count_(new int32_t[pool_size]{}),
      page_ids_(new page_id_t[pool_size]{}),
      pages_(new Page[pool_size]),
      replacer_(pool_size) {
    init_pool();
}

BufferManager::~BufferManager() {
    flush_all_pages();
    delete[] dirty_;
    delete[] pin_count_;
    delete[] page_ids_;
    delete[] pages_;
}

// ─── fetch_page ──────────────────────────────────────────────────────────────

Page* BufferManager::fetch_page(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    // ── cache hit ────────────────────────────────────────────────────────────
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        pin_count_[frame_id]++;
        replacer_.set_evictable(frame_id, false);
        replacer_.record_access(frame_id);
        return &pages_[frame_id];
    }

    // ── cache miss: find a frame ──────────────────────────────────────────────
    frame_id_t frame_id;
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_.evict(frame_id)) return nullptr;

        if (dirty_[frame_id]) {
            // WAL rule: flush log before writing the dirty page to disk.
            enforce_wal(frame_id);
            if (!disk_manager_->write_page(page_ids_[frame_id], pages_[frame_id].raw())) {
                // Write failed — cannot evict this frame safely.
                return nullptr;
            }
            dirty_[frame_id] = false;
        }
        page_table_.erase(page_ids_[frame_id]);
    }

    if (!disk_manager_->read_page(page_id, pages_[frame_id].raw())) return nullptr;

    page_ids_[frame_id]    = page_id;
    page_table_[page_id]   = frame_id;
    pin_count_[frame_id]   = 1;
    dirty_[frame_id]       = false;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);
    return &pages_[frame_id];
}

// ─── unpin_page ──────────────────────────────────────────────────────────────

bool BufferManager::unpin_page(page_id_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    frame_id_t frame_id = it->second;
    if (pin_count_[frame_id] <= 0) return false;

    pin_count_[frame_id]--;
    if (is_dirty) dirty_[frame_id] = true;  // sticky — only cleared on flush

    if (pin_count_[frame_id] == 0) {
        replacer_.set_evictable(frame_id, true);
    }
    return true;
}

// ─── flush_page ──────────────────────────────────────────────────────────────

bool BufferManager::flush_page(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    frame_id_t frame_id = it->second;
    // WAL rule: log must be ahead of the page on disk.
    enforce_wal(frame_id);
    if (!disk_manager_->write_page(page_id, pages_[frame_id].raw())) return false;
    dirty_[frame_id] = false;
    return true;
}

// ─── flush_all_pages ─────────────────────────────────────────────────────────

void BufferManager::flush_all_pages() {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    for (size_t i = 0; i < pool_size_; ++i) {
        if (page_ids_[i] != INVALID_PAGE_ID && dirty_[i]) {
            enforce_wal(static_cast<frame_id_t>(i));
            disk_manager_->write_page(page_ids_[i], pages_[i].raw());
            dirty_[i] = false;
        }
    }
}

// ─── new_page ────────────────────────────────────────────────────────────────

Page* BufferManager::new_page(page_id_t& page_id) {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    frame_id_t frame_id;
    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_.evict(frame_id)) return nullptr;

        if (dirty_[frame_id]) {
            // WAL rule before evicting a dirty frame.
            enforce_wal(frame_id);
            if (!disk_manager_->write_page(page_ids_[frame_id], pages_[frame_id].raw())) {
                return nullptr;
            }
            dirty_[frame_id] = false;
        }
        page_table_.erase(page_ids_[frame_id]);
    }

    page_id = disk_manager_->allocate_page();
    pages_[frame_id].init(page_id, PageType::DATA);

    page_ids_[frame_id]    = page_id;
    page_table_[page_id]   = frame_id;
    pin_count_[frame_id]   = 1;
    dirty_[frame_id]       = false;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);
    return &pages_[frame_id];
}

// ─── delete_page ─────────────────────────────────────────────────────────────

bool BufferManager::delete_page(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(bm_mutex_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return true;  // not in pool — nothing to do

    frame_id_t frame_id = it->second;
    if (pin_count_[frame_id] > 0) return false;  // still in use

    page_table_.erase(it);
    replacer_.remove_from_replacer(frame_id);
    page_ids_[frame_id]   = INVALID_PAGE_ID;
    dirty_[frame_id]      = false;
    pin_count_[frame_id]  = 0;
    free_list_.push_back(frame_id);
    return true;
}

std::vector<std::pair<page_id_t, lsn_t>> BufferManager::get_dirty_pages() {
    std::lock_guard<std::mutex> lock(bm_mutex_);
    std::vector<std::pair<page_id_t, lsn_t>> dirty_pages;
    for (size_t i = 0; i < pool_size_; ++i) {
        if (dirty_[i] && page_ids_[i] != INVALID_PAGE_ID) {
            dirty_pages.emplace_back(page_ids_[i], pages_[i].get_page_lsn());
        }
    }
    return dirty_pages;
}