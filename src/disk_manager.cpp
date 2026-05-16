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

    // FIX: tellg() returns std::streamoff which is signed — it returns -1 on
    // error (e.g. the stream is in a bad state right after creation on some
    // platforms). Guard against negative values before casting to unsigned
    // page_id_t, otherwise next_page_id_ wraps to a huge number and every
    // subsequent allocate_page() produces garbage IDs.
    std::streamoff file_size = file_stream_.tellg();
    if (file_size < 0) file_size = 0;
    next_page_id_ = static_cast<page_id_t>(
        static_cast<uint64_t>(file_size) / PAGE_SIZE);
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

    // FIX: gcount() returns std::streamsize (signed). The old code compared
    // it directly to PAGE_SIZE (size_t, unsigned) — if the stream is in a
    // bad state and gcount() returns -1, the unsigned comparison passes and
    // we'd call memset with a massive length, corrupting the buffer.
    // Guard: treat negative gcount as 0 bytes read.
    std::streamsize bytes_read = file_stream_.gcount();
    if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE)) {
        size_t got = (bytes_read > 0) ? static_cast<size_t>(bytes_read) : 0;
        std::memset(dest + got, 0, PAGE_SIZE - got);
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

void DiskManager::set_num_pages(page_id_t n) {
    std::lock_guard<std::mutex> lock(dm_mutex_);
    if (n > next_page_id_) next_page_id_ = n;
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