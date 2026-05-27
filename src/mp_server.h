#pragma once
#ifndef CATA_SRC_MP_SERVER_H
#define CATA_SRC_MP_SERVER_H

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <functional>

// Forward declarations to avoid pulling asio into every translation unit
namespace asio {
class io_context;
}

namespace cata_mp {

struct client_session;

class server {
    public:
        server( uint16_t port, std::string password, std::string version = "" );
        ~server();

        // Start listening. Blocks until stop() is called.
        void run();

        // Signal the server to stop accepting connections and shut down.
        void stop();

        // Broadcast a message to all connected clients.
        // Must be called from the Asio io_context thread (e.g. inside a callback).
        void broadcast( const std::string &msg );

        // Thread-safe broadcast: posts the send onto the io_context thread.
        // Use this when calling from the game loop thread.
        void post_broadcast( const std::string &msg );

        uint16_t port() const {
            return port_;
        }

    private:
        void do_accept();
        void on_client_connected( std::shared_ptr<client_session> session );
        void on_client_disconnected( std::shared_ptr<client_session> session );
        void on_message( std::shared_ptr<client_session> session, const std::string &msg );

        uint16_t port_;
        std::string password_;
        std::string version_;

        struct impl;
        std::unique_ptr<impl> impl_;

        std::vector<std::shared_ptr<client_session>> clients_;
        std::mutex clients_mutex_;
};

// Start the server on the given port. Called from main() when --server flag is set.
// Does not return until the server shuts down.
void run_server( uint16_t port, const std::string &password,
                 const std::string &version = "" );

// Returns the active server instance, or nullptr if not running.
// Thread-safe: broadcast() on the returned pointer is mutex-protected.
server *get_active_server();

} // namespace cata_mp

#endif // CATA_SRC_MP_SERVER_H
