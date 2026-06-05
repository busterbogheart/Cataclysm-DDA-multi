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

// Defined in mp_gamestate.cpp. Forward-declared (mp_client_conn.cpp doesn't
// include the gamestate header) so we can trace connection lifecycle here.
void mp_log( const std::string &msg );

static bool client_mode_ = false;

bool is_client_mode()
{
    return client_mode_;
}

void set_client_mode( bool enabled )
{
    client_mode_ = enabled;
    // Trace client_mode flips: a stale "true" left over from a failed prior
    // join makes mp_menu_join_session() skip the IP prompt entirely.
    mp_log( "[cdda-mp] set_client_mode -> " + std::string( enabled ? "true" : "false" ) );
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

// Join message / welcome storage.
static std::string g_pending_join;
static bool g_join_sent = false;
static std::string g_connect_error;

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
                     const std::string &name, const std::string &password,
                     const std::string &version )
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

    // Disable Nagle's algorithm. The lockstep grant/wait/ack messages are tiny;
    // Nagle batching plus the peer's delayed-ACK add hundreds of ms per
    // round-trip on a high-latency link, wedging the turn cycle. Invisible on
    // LAN, essential for internet play.
    {
        asio::error_code nd_ec;
        g_client->sock.set_option( tcp::no_delay( true ), nd_ec );
        mp_log( "[cdda-mp] client TCP_NODELAY ec=" + nd_ec.message() );
    }

    g_client->start_read();
    g_client->io_thread = std::thread( []() {
        g_client->io_ctx.run();
    } );

    // Send the join immediately so the server can validate version/password
    // before the client enters character creation.  The server replies with
    // {"type":"welcome"} or {"type":"error"} from its network thread — no
    // host game loop needed.  Errors are surfaced here so the caller can show
    // a clean popup before any world/char UI appears.
    std::string join_msg = "{\"type\":\"join\",\"name\":\"" + name + "\"";
    if( !password.empty() ) {
        join_msg += ",\"password\":\"" + password + "\"";
    }
    if( !version.empty() ) {
        join_msg += ",\"version\":\"" + version + "\"";
    }
    join_msg += "}\n";

    asio::error_code wec;
    asio::write( g_client->sock, asio::buffer( join_msg ), wec );
    if( wec ) {
        std::cerr << "[cdda-mp] Failed to send join: " << wec.message() << std::endl;
        g_client.reset();
        return false;
    }

    // Wait up to 5 s for welcome or error.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( std::chrono::steady_clock::now() < deadline ) {
        std::string msg;
        if( g_recv_queue.pop( msg ) ) {
            if( msg.find( "\"type\":\"error\"" ) != std::string::npos ) {
                // Extract human-readable error for the caller.
                g_connect_error = "Server rejected connection.";
                const auto mpos = msg.find( "\"message\":\"" );
                if( mpos != std::string::npos ) {
                    const size_t start = mpos + 11;
                    const size_t end = msg.find( '"', start );
                    if( end != std::string::npos ) {
                        g_connect_error = msg.substr( start, end - start );
                    }
                }
                g_join_sent = false;
                g_client.reset();
                return false;
            }
            if( msg.find( "\"type\":\"welcome\"" ) != std::string::npos ) {
                // Store welcome so the game-loop handler can adopt the seed.
                g_pending_join = msg;
                g_join_sent = true;
                std::cout << "[cdda-mp] Connected to " << host << ":" << port
                          << " as '" << name << "' — version accepted." << std::endl;
                return true;
            }
            // Any other packet (e.g. hello) — keep waiting.
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    }

    g_connect_error = "Timed out waiting for server response.";
    std::cerr << "[cdda-mp] Timed out waiting for welcome from server." << std::endl;
    g_client.reset();
    return false;
}

std::string client_connect_error()
{
    return g_connect_error;
}

void client_send_join()
{
    // Join was already sent during client_connect() and welcome was received.
    // On the first do_turn call, replay the stored welcome through the normal
    // incoming-packet handler so the game loop applies the seed and world name.
    if( g_join_sent && !g_pending_join.empty() &&
        g_pending_join.find( "\"type\":\"welcome\"" ) != std::string::npos ) {
        g_recv_queue.push( g_pending_join );
        g_pending_join.clear();
        std::cout << "[cdda-mp] Replayed welcome into recv queue." << std::endl;
        return;
    }
    if( g_join_sent || !g_client || g_pending_join.empty() ) {
        return;
    }
    // Legacy path (should not be reached with the new eager-join flow).
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
