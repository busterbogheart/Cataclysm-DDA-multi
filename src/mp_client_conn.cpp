// Networking only — no CDDA game headers allowed here (asio conflicts with PCH).
// Follow the same pattern as mp_server.cpp.
#include "mp_client_conn.h"
#include "mp_queue.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#define ASIO_STANDALONE
#include <asio.hpp>

using asio::ip::tcp;

namespace cata_mp {

// ---------------------------------------------------------------------------
// Client mode flag
// ---------------------------------------------------------------------------

static bool client_mode_ = false;

bool is_client_mode()
{
    return client_mode_;
}

void set_client_mode( bool enabled )
{
    client_mode_ = enabled;
}

// ---------------------------------------------------------------------------
// Thread-safe queue for incoming state strings (io thread → game thread)
// ---------------------------------------------------------------------------

class string_queue
{
    public:
        void push( std::string s ) {
            std::lock_guard<std::mutex> lk( mtx_ );
            q_.push( std::move( s ) );
        }
        bool pop( std::string &out ) {
            std::lock_guard<std::mutex> lk( mtx_ );
            if( q_.empty() ) {
                return false;
            }
            out = std::move( q_.front() );
            q_.pop();
            return true;
        }
    private:
        std::queue<std::string> q_;
        std::mutex mtx_;
};

static string_queue g_recv_queue;

// ---------------------------------------------------------------------------
// Connection impl
// ---------------------------------------------------------------------------

struct client_impl {
    asio::io_context io_ctx;
    tcp::socket sock{ io_ctx };
    asio::streambuf read_buf;
    std::thread io_thread;

    ~client_impl() {
        io_ctx.stop();
        if( io_thread.joinable() ) {
            io_thread.join();
        }
    }

    void start_read() {
        asio::async_read_until( sock, read_buf, '\n',
        [this]( const asio::error_code & ec, size_t /*n*/ ) {
            if( ec ) {
                std::cerr << "[cdda-mp] Server disconnected: " << ec.message() << std::endl;
                // Synthetic disconnect message — game thread will clean up the host NPC.
                g_recv_queue.push( "{\"type\":\"state\",\"connected\":false}" );
                return;
            }
            std::istream is( &read_buf );
            std::string line;
            std::getline( is, line );
            if( !line.empty() ) {
                g_recv_queue.push( std::move( line ) );
            }
            start_read();
        } );
    }
};

static std::unique_ptr<client_impl> g_client;

// Join message built at connect time but sent only after the game is loaded.
static std::string g_pending_join;
static bool g_join_sent = false;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool tcp_probe( const std::string &host, uint16_t port, int timeout_ms )
{
    try {
        asio::io_context io;
        tcp::resolver resolver( io );
        asio::error_code resolve_ec;
        auto endpoints = resolver.resolve( host, std::to_string( port ), resolve_ec );
        if( resolve_ec ) {
            return false;
        }
        tcp::socket sock( io );
        bool connected = false;
        bool finished = false;
        asio::async_connect( sock, endpoints,
        [&]( const asio::error_code & ec, const tcp::endpoint & ) {
            finished = true;
            connected = !ec;
        } );
        asio::steady_timer deadline( io );
        deadline.expires_after( std::chrono::milliseconds( timeout_ms ) );
        deadline.async_wait( [&]( const asio::error_code & ) {
            if( !finished ) {
                // Cancels the pending async_connect, which then fires its
                // completion handler with operation_aborted.
                asio::error_code ignore;
                sock.close( ignore );
            }
        } );
        io.run();
        return connected;
    } catch( const std::exception & ) {
        return false;
    }
}

bool client_connect( const std::string &host, uint16_t port,
                     const std::string &name, const std::string &password )
{
    g_client = std::make_unique<client_impl>();

    asio::error_code ec;
    tcp::resolver resolver( g_client->io_ctx );
    auto endpoints = resolver.resolve( host, std::to_string( port ), ec );
    if( ec ) {
        std::cerr << "[cdda-mp] Could not resolve '" << host << "': " << ec.message() << std::endl;
        g_client.reset();
        return false;
    }

    asio::connect( g_client->sock, endpoints, ec );
    if( ec ) {
        std::cerr << "[cdda-mp] Could not connect to " << host << ":" << port
                  << " — " << ec.message() << std::endl;
        g_client.reset();
        return false;
    }

    g_client->start_read();
    g_client->io_thread = std::thread( []() {
        g_client->io_ctx.run();
    } );

    // Build the join message but don't send yet — deferred until the save is loaded.
    g_pending_join = "{\"type\":\"join\",\"name\":\"" + name + "\"";
    if( !password.empty() ) {
        g_pending_join += ",\"password\":\"" + password + "\"";
    }
    g_pending_join += "}\n";
    g_join_sent = false;

    std::cout << "[cdda-mp] Connected to " << host << ":" << port
              << " as '" << name << "' (join deferred until save loaded)" << std::endl;
    return true;
}

void client_send_join()
{
    if( g_join_sent || !g_client || g_pending_join.empty() ) {
        return;
    }
    asio::error_code ec;
    asio::write( g_client->sock, asio::buffer( g_pending_join ), ec );
    if( ec ) {
        std::cerr << "[cdda-mp] Failed to send join: " << ec.message() << std::endl;
        return;
    }
    g_join_sent = true;
    std::cout << "[cdda-mp] Join sent — now in-game." << std::endl;
}

bool client_join_is_sent()
{
    return g_join_sent;
}

bool client_recv_pop( std::string &out )
{
    return g_recv_queue.pop( out );
}

void client_send( const std::string &json )
{
    if( !g_client ) {
        return;
    }
    const std::string msg = json + "\n";
    asio::post( g_client->io_ctx, [msg]() {
        if( !g_client ) {
            return;
        }
        asio::error_code ec;
        asio::write( g_client->sock, asio::buffer( msg ), ec );
        if( ec ) {
            std::cerr << "[cdda-mp] Send error: " << ec.message() << std::endl;
        }
    } );
}

} // namespace cata_mp
