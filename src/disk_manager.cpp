#include "disk_manager.hpp"
#include "common.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

DiskManager::DiskManager(const std::string& file_path) {

    file_path_ = file_path;
    file_stream_.open(file_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_stream_.is_open()) {

        std::ofstream creator(file_path, std::ios::binary);
        if (!creator.is_open()) {
            throw std::runtime_error("Failed to create disk file: " + file_path);
        }
        creator.close();

        file_stream_.open(file_path, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_stream_.is_open()) {
            throw std::runtime_error("Failed to open disk file: " + file_path);
        }
    }

    file_stream_.seekg(0, std::ios::end);
    auto file_size = file_stream_.tellg();
    next_page_id_ = static_cast<page_id_t>(file_size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

bool DiskManager::read_page(page_id_t page_id, uint8_t* dest) {

    auto offset = static_cast<uint64_t>(page_id) * PAGE_SIZE;
    file_stream_.seekg(static_cast<std::streamoff>(offset));
    file_stream_.read(reinterpret_cast<char*>(dest), PAGE_SIZE);
    auto bytes_read = file_stream_.gcount();
    if (bytes_read < PAGE_SIZE){
        std::memset(dest+bytes_read,0,PAGE_SIZE-bytes_read);
        file_stream_.clear();
    }
    return true;
}

bool DiskManager::write_page(page_id_t page_id, const uint8_t* src) {
    
    auto offset = static_cast<uint64_t>(page_id) * PAGE_SIZE;
    file_stream_.seekp(static_cast<std::streamoff>(offset));
    file_stream_.write(reinterpret_cast<const char*>(src), PAGE_SIZE);
    if (file_stream_.fail()) {
        file_stream_.clear();
        return false;
    }
    return true;
}

page_id_t DiskManager::allocate_page(){
    page_id_t saved = next_page_id_;
    next_page_id_++;
    return saved;
}

size_t DiskManager::get_num_pages() const {
    return next_page_id_;
}

bool DiskManager::flush(){
    file_stream_.flush();
    return file_stream_.good();
}

