#pragma once

#include <fstream>
#include <string>
#include "common.hpp"


/**
 * @brief Manages raw page-level I/O to disk.
 * 
 * Reads and writes fixed-size pages to a single file. The disk manager
 * knows nothing about page layout, records, or B+ trees — it only handles
 * bytes on disk.
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

    bool flush();

private:
    std::string file_path_;
    std::fstream file_stream_;
    page_id_t next_page_id_{0};
};
