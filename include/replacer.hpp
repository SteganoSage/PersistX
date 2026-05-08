#pragma once
#include "common.hpp"
#include <unordered_map>
#include <list>
#include <cstddef>

class Replacer{
public:
    explicit Replacer(size_t num_pages);

    bool evict(frame_id_t& frame_id);

    void record_access(frame_id_t frame_id);

    void set_evictable(frame_id_t frame_id,bool evictable);

    void remove_from_replacer(frame_id_t frame_id);

    size_t size() const;

private:
    size_t frame_count_;
    std::list<frame_id_t> frame_list_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> frame_map_;

};
