#include "buffer_manager.hpp"
#include <stdexcept>


BufferManager::BufferManager(DiskManager* disk_manager, size_t pool_size)
    : disk_manager_(disk_manager),
    pool_size_(pool_size),
    dirty_(new bool[pool_size]{}),
    pin_count_(new int32_t[pool_size]{}),
    page_ids_(new page_id_t[pool_size]{}),
    pages_(new Page[pool_size]),
    replacer_(pool_size){

    for (size_t i = 0; i < pool_size_; ++i) {
        page_ids_[i] = INVALID_PAGE_ID;
    }
    
    for(frame_id_t i = 0; i < pool_size_; ++i){
        free_list_.push_back(i);
    }

}

BufferManager::~BufferManager() {
    flush_all_pages();
    delete[] dirty_;
    delete[] pin_count_;
    delete[] page_ids_;
    delete[] pages_;
}

Page* BufferManager::fetch_page(page_id_t page_id) {
    
    auto it = page_table_.find(page_id);

    if(it != page_table_.end()){
        frame_id_t frame_id = it->second;
        pin_count_[frame_id]++;
        replacer_.set_evictable(frame_id, false);
        replacer_.record_access(frame_id);
        return &pages_[frame_id];
    }

    frame_id_t frame_id;
    if(!free_list_.empty()){
        frame_id = free_list_.front();
        free_list_.pop_front();
    }
    else{

        if(!replacer_.evict(frame_id)){
            return nullptr;
        }

        if(dirty_[frame_id]){
            disk_manager_->write_page(page_ids_[frame_id], pages_[frame_id].raw());
            dirty_[frame_id] = false;
        }
        
        page_table_.erase(page_ids_[frame_id]);

    }

    if(!disk_manager_->read_page(page_id, pages_[frame_id].raw())){
        return nullptr;
    }

    page_ids_[frame_id] = page_id;
    page_table_[page_id] = frame_id;
    pin_count_[frame_id] = 1;
    dirty_[frame_id] = false;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);
    return &pages_[frame_id];
}

// ─── unpin_page ──────────────────────────────────────────────────────────────

bool BufferManager::unpin_page(page_id_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    frame_id_t frame_id = it->second;
    if (pin_count_[frame_id] <= 0) return false;

    pin_count_[frame_id]--;

    // Dirty bit is sticky — once dirty, stays dirty until flushed.
    if (is_dirty) {
        dirty_[frame_id] = true;
    }

    // When nobody is using the page, it becomes evictable.
    if (pin_count_[frame_id] == 0) {
        replacer_.set_evictable(frame_id, true);
    }

    return true;
}

// ─── flush_page ──────────────────────────────────────────────────────────────

bool BufferManager::flush_page(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    frame_id_t frame_id = it->second;
    disk_manager_->write_page(page_id, pages_[frame_id].raw());
    dirty_[frame_id] = false;
    return true;
}

// ─── flush_all_pages ─────────────────────────────────────────────────────────

void BufferManager::flush_all_pages() {
    for (size_t i = 0; i < pool_size_; ++i) {
        if (page_ids_[i] != INVALID_PAGE_ID) {
            disk_manager_->write_page(page_ids_[i], pages_[i].raw());
            dirty_[i] = false;
        }
    }
}

// ─── new_page ────────────────────────────────────────────────────────────────

Page* BufferManager::new_page(page_id_t& page_id) {
    // Find a frame — same logic as fetch_page's cache-miss path.
    frame_id_t frame_id;

    if (!free_list_.empty()) {
        frame_id = free_list_.front();
        free_list_.pop_front();
    } else {
        if (!replacer_.evict(frame_id)) {
            return nullptr;  // pool full, everything pinned
        }

        if (dirty_[frame_id]) {
            disk_manager_->write_page(page_ids_[frame_id], pages_[frame_id].raw());
        }

        page_table_.erase(page_ids_[frame_id]);
    }

    // Allocate a new page_id from disk manager.
    page_id = disk_manager_->allocate_page();

    // Initialize a fresh page — don't read from disk.
    pages_[frame_id].init(page_id, PageType::DATA);

    page_ids_[frame_id] = page_id;
    page_table_[page_id] = frame_id;
    pin_count_[frame_id] = 1;
    dirty_[frame_id] = false;
    replacer_.record_access(frame_id);
    replacer_.set_evictable(frame_id, false);

    return &pages_[frame_id];
}

// ─── delete_page ─────────────────────────────────────────────────────────────

bool BufferManager::delete_page(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return true;  // not in pool — nothing to do

    frame_id_t frame_id = it->second;

    // Cannot delete a page that someone is still using.
    if (pin_count_[frame_id] > 0) return false;

    // Remove from all tracking structures.
    page_table_.erase(it);
    replacer_.remove_from_replacer(frame_id);

    // Reset frame metadata.
    page_ids_[frame_id] = INVALID_PAGE_ID;
    dirty_[frame_id] = false;
    pin_count_[frame_id] = 0;

    // Return frame to the free list for reuse.
    free_list_.push_back(frame_id);

    return true;
}