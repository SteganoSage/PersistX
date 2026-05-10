#pragma once

#include "common.hpp"
#include "log_manager.hpp"
#include "buffer_manager.hpp"
#include "transaction_manager.hpp"
#include <vector>
#include <atomic>
#include <thread>


class CheckpointManager{

    public:

        explicit CheckpointManager(LogManager* log_manager,
                                BufferManager* buffer_manager,
                                TransactionManager* transaction_manager)
            : log_manager_(log_manager),
            buffer_manager_(buffer_manager),
            transaction_manager_(transaction_manager) {}

        void checkpoint();


    private:
        LogManager* log_manager_;
        BufferManager* buffer_manager_;
        TransactionManager* transaction_manager_;

};
