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
        // Periodic host->client heartbeat, driven by the io thread (NOT the game
        // thread) so it keeps beating even while the host is blocked in a modal
        // (inventory/crafting/…) — a game-thread heartbeat would stall there and
        // trip the client's stall watchdog into a spurious reconnect.
        void arm_heartbeat();
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

        // The currently-active authenticated session (Increment 2).  Set on JOIN;
        // only THIS session's disconnect ends the co-op session.  A superseded
        // session (the old socket lingering after a reconnect) dying must NOT
        // push a disconnect event, or it would evict the just-reconnected player.
        std::weak_ptr<client_session> active_session_;
};

// Start the server on the given port. Called from main() when --server flag is set.
// Does not return until the server shuts down.
void run_server( uint16_t port, const std::string &password,
                 const std::string &version = "" );

// Returns the active server instance, or nullptr if not running.
// Thread-safe: broadcast() on the returned pointer is mutex-protected.
server *get_active_server();

// All of this machine's non-loopback / non-link-local IPv4 addresses, for the
// host HUD's "hosting at:" hint — enumerates every up interface (physical LAN +
// every VPN: Tailscale 100.x, Radmin 26.x, ZeroTier, …).  VPN-range addresses
// are ordered FIRST since that's usually the address a remote partner uses.  We
// can't reliably pick THE right one (depends how the partner connects), so we
// list them all and let the host read off the matching one.  Cached after the
// first call.  Still can't know the host's public IP / DDNS (external config).
std::vector<std::string> mp_local_ipv4s();

// True from the moment the (detached) listen thread enters run_server() until
// it fully returns — i.e. until the server object has destructed and its
// listen socket (the port) is released. Session-end waits on this going false
// before allowing a re-host to re-bind the same port, so a second host session
// in one launch can't race the first session's socket teardown.
bool is_server_thread_running();

} // namespace cata_mp

#endif // CATA_SRC_MP_SERVER_H
