#pragma once

#include <fstream>
#include <string>
#include <mutex>
#include "common.hpp"


/**
 * @brief Manages raw page-level I/O to disk.
 * 
 * Reads and writes fixed-size pages to a single file. The disk manager
 * knows nothing about page layout, records, or B+ trees — it only handles
 * bytes on disk.
 *
 * Thread safety: all public methods are protected by dm_mutex_.
 */
class DiskManager {
public:

    explicit DiskManager(const std::string& file_path);

    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    bool read_page(page_id_t page_id, uint8_t* dest);

    bool write_page(page_id_t page_id, const uint8_t* src);

    page_id_t allocate_page();

    size_t get_num_pages() const;

    void set_num_pages(page_id_t n);

    bool flush();

private:
    std::string file_path_;
    std::fstream file_stream_;
    page_id_t next_page_id_{0};
    mutable std::mutex dm_mutex_;
};
