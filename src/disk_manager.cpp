#include "disk_manager.hpp"
#include "common.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

#ifdef _WIN32
#include <io.h>        // _commit, _close, _sopen_s
#include <fcntl.h>     // _O_RDWR, _O_BINARY
#include <share.h>     // _SH_DENYNO
#else
#include <unistd.h>    // fdatasync, close
#include <fcntl.h>     // open, O_RDWR
#endif

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
    std::lock_guard<std::mutex> lock(dm_mutex_);

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
    std::lock_guard<std::mutex> lock(dm_mutex_);

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
    std::lock_guard<std::mutex> lock(dm_mutex_);
    page_id_t saved = next_page_id_;
    next_page_id_++;
    return saved;
}

size_t DiskManager::get_num_pages() const {
    std::lock_guard<std::mutex> lock(dm_mutex_);
    return next_page_id_;
}

bool DiskManager::flush(){
    std::lock_guard<std::mutex> lock(dm_mutex_);
    file_stream_.flush();

    // fsync — push past the OS page cache to stable storage.
#ifdef _WIN32
    {
        int fd = -1;
        if (_sopen_s(&fd, file_path_.c_str(),
                     _O_RDWR | _O_BINARY, _SH_DENYNO, 0) == 0 && fd >= 0) {
            _commit(fd);
            _close(fd);
        }
    }
#else
    {
        int fd = ::open(file_path_.c_str(), O_RDWR);
        if (fd >= 0) {
            ::fdatasync(fd);
            ::close(fd);
        }
    }
#endif

    return file_stream_.good();
}
