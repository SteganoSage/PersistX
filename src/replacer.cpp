#include "persistx/replacer.hpp"

Replacer::Replacer(size_t num_pages){
    frame_count_ = num_pages;
}

bool Replacer::evict(frame_id_t& frame_id){
    if (frame_list_.empty()){
        return false;
    }
    frame_id = frame_list_.back();
    frame_list_.pop_back();
    frame_map_.erase(frame_id);
    return true;    
}

void Replacer::record_access(frame_id_t frame_id){
    if (frame_map_.find(frame_id) != frame_map_.end()){
        frame_list_.erase(frame_map_[frame_id]);
        frame_list_.push_front(frame_id);
        frame_map_[frame_id] = frame_list_.begin();
    }
}

void Replacer::remove_from_replacer(frame_id_t frame_id){
    if (frame_map_.find(frame_id) != frame_map_.end()){
        frame_list_.erase(frame_map_[frame_id]);
        frame_map_.erase(frame_id);
    }
}

void Replacer::set_evictable(frame_id_t frame_id, bool evictable){

    if (evictable && frame_map_.find(frame_id) == frame_map_.end() ){
        frame_list_.push_front(frame_id);
        frame_map_[frame_id] = frame_list_.begin();
    }

    if(!evictable && frame_map_.find(frame_id) != frame_map_.end()){
        frame_list_.erase(frame_map_[frame_id]);
        frame_map_.erase(frame_id);
    }
}

size_t Replacer::size() const {
    return frame_list_.size();
}


