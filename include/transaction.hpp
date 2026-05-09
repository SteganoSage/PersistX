#pragma once


#include "common.hpp"
#include <vector>
#include <atomic>


class Transaction{

    public:
        explicit Transaction(txn_id_t txn_id) : txn_id_(txn_id), state_(TxnState::GROWING), prev_lsn_(INVALID_LSN){}


        void set_prev_lsn(lsn_t lsn){
            prev_lsn_ = lsn;
        }

        txn_id_t get_txn_id()const{
            return txn_id_;
        }

        TxnState get_state()const{
            return state_;
        }

        lsn_t get_prev_lsn()const{
            return prev_lsn_;
        }

        void set_state(TxnState state){
            state_ = state;
        }

    private:
        txn_id_t txn_id_;
        TxnState state_;
        lsn_t prev_lsn_;
    

};
