#pragma once
#ifndef CATA_SRC_MP_QUEUE_H
#define CATA_SRC_MP_QUEUE_H

#include <queue>
#include <string>
#include <mutex>
#include <condition_variable>

namespace cata_mp {

struct mp_event {
    enum class type {
        connect,     // player 2 connected — spawn their character
        disconnect,  // player 2 disconnected — remove their character
        action       // player 2 sent an action command
    };
    type evt_type;
    std::string session_id;
    std::string data;  // JSON action string for type::action
};

class event_queue {
    public:
        void push( mp_event e ) {
            std::lock_guard<std::mutex> lock( mutex_ );
            queue_.push( std::move( e ) );
        }

        bool pop( mp_event &out ) {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( queue_.empty() ) {
                return false;
            }
            out = std::move( queue_.front() );
            queue_.pop();
            return true;
        }

        bool empty() {
            std::lock_guard<std::mutex> lock( mutex_ );
            return queue_.empty();
        }

    private:
        std::queue<mp_event> queue_;
        std::mutex mutex_;
};

// Global queue — Asio thread writes, game thread reads
event_queue &get_mp_queue();

} // namespace cata_mp

#endif // CATA_SRC_MP_QUEUE_H
