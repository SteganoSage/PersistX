#pragma once

#include "disk_manager.hpp"
#include "replacer.hpp"
#include "page.hpp"
#include <unordered_map>
#include <list>


class BufferManager {
public:
    BufferManager(DiskManager* disk_manager, size_t pool_size);
    ~BufferManager();

    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    Page* fetch_page(page_id_t page_id);

    bool unpin_page(page_id_t page_id, bool is_dirty);

    bool flush_page(page_id_t page_id);

    void flush_all_pages();

    Page* new_page(page_id_t& page_id);

    bool delete_page(page_id_t page_id);    

private:
    DiskManager* disk_manager_;
    size_t pool_size_;
    bool* dirty_;
    int32_t* pin_count_;
    page_id_t* page_ids_;
    Page* pages_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    Replacer replacer_;
    std::list<frame_id_t> free_list_;
};