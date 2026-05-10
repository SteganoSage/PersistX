#pragma once

#include "common.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "log_record.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

class RecoveryManager {
    public:

        RecoveryManager(LogManager* log_manager, BufferManager* buffer_manager);

        void recover();
      

    private:

        void analyze();
        void redo();
        void undo();

        LogManager* log_manager_;
        BufferManager* buffer_manager_;
        std::unordered_map<txn_id_t, lsn_t> att_;
        std::unordered_map<page_id_t, lsn_t> dpt_;

};