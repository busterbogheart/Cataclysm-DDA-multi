// Networking only — no CDDA game headers allowed here (asio conflicts with PCH).
// Follow the same pattern as mp_server.cpp.
#include "mp_client_conn.h"
#include "mp_queue.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

#include <zstd/zstd.h>
#include "catacharset.h"   // base64_decode — only pulls std headers, asio-safe

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
void mp_set_client_host_world_name( const std::string &name );
void mp_set_client_host_player_name( const std::string &name );
void mp_store_pending_welcome( const std::string &msg );

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

// Reverse of the host's mp_compress_frame (mp_server.cpp): a line of the form
// {"z":"<base64 zstd>"} is a compressed state broadcast — base64-decode then
// zstd-decompress back to the original JSON line.  Any other line is returned
// unchanged (grants/acks/control messages stay plaintext).  Both ends are the
// same build (version handshake), so the format is guaranteed to match.
static std::string mp_decompress_frame( std::string line )
{
    static const std::string prefix = "{\"z\":\"";
    if( line.size() <= prefix.size() + 2 ||
        line.compare( 0, prefix.size(), prefix ) != 0 ||
        line.compare( line.size() - 2, 2, "\"}" ) != 0 ) {
        return line;
    }
    const std::string b64 = line.substr( prefix.size(),
                                          line.size() - prefix.size() - 2 );
    const std::string comp = base64_decode( b64 );
    const unsigned long long orig =
        ZSTD_getFrameContentSize( comp.data(), comp.size() );
    if( orig == ZSTD_CONTENTSIZE_UNKNOWN || orig == ZSTD_CONTENTSIZE_ERROR ) {
        mp_log( "[cdda-mp] decompress: bad zstd frame; dropping" );
        return std::string();
    }
    std::string out;
    out.resize( static_cast<size_t>( orig ) );
    const size_t n = ZSTD_decompress( &out[0], out.size(), comp.data(), comp.size() );
    if( ZSTD_isError( n ) ) {
        mp_log( std::string( "[cdda-mp] decompress error: " ) + ZSTD_getErrorName( n ) );
        return std::string();
    }
    out.resize( n );
    return out;
}

// --- Auto-reconnect state ------------------------------------------------
// Declared before client_impl because client_impl::start_read() reads these on
// an ungraceful drop.  Last successful connect params, remembered so a dropped
// link can re-dial the same host without going back through menu/char-creation.
// g_rc_enabled gates the feature (cleared on intentional teardown / goodbye).
static std::string g_rc_host;
static uint16_t g_rc_port = 0;
static std::string g_rc_name;
static std::string g_rc_password;
static std::string g_rc_version;
static std::atomic<bool> g_rc_enabled{ false };
static std::atomic<bool> g_rc_in_progress{ false };
static void reconnect_worker();

// Heartbeat + stall detection (Increment 1.5).  The read-error path only fires
// when there's outstanding SENT data for TCP's RTO to time out on; a link that
// dies while the client is idle/locked WAITING for a grant is invisible to it
// (no traffic -> no RTO -> no error).  So we send a periodic heartbeat (keeps
// liveness traffic on the wire) and, if we hear nothing back for MP_STALL_MS,
// declare the link dead and start the reconnect sweep ourselves.  Runs on the
// io thread (a steady_timer) so it's immune to the game thread blocking in
// handle_action (observed blocking up to 37s waiting for input).
static constexpr int64_t MP_HB_INTERVAL_MS = 1500;  // heartbeat cadence
static constexpr int64_t MP_STALL_MS = 8000;        // peer silence -> drop (~5 missed beats)
static std::atomic<int64_t> g_last_recv_ms{ 0 };    // last time ANY host msg arrived

static int64_t mp_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// Kick off a reconnect sweep iff enabled and not already running.  Shared by the
// read-error path and the heartbeat stall path so only one sweep ever runs.
static void start_reconnect_sweep( const char *why )
{
    if( g_rc_enabled.load() && !g_rc_in_progress.exchange( true ) ) {
        mp_log( std::string( "[cdda-mp] RECONNECT: " ) + why + " — starting reconnect sweep" );
        g_recv_queue.push( "{\"type\":\"state\",\"reconnecting\":true}" );
        std::thread( reconnect_worker ).detach();
    }
}

// ---------------------------------------------------------------------------
// Connection impl
// ---------------------------------------------------------------------------

struct client_impl {
    asio::io_context io_ctx;
    tcp::socket sock{ io_ctx };
    asio::streambuf read_buf;
    std::thread io_thread;
    asio::steady_timer hb_timer{ io_ctx };

    ~client_impl() {
        io_ctx.stop();
        if( io_thread.joinable() ) {
            io_thread.join();
        }
    }

    // Periodic heartbeat + stall watchdog, on the io thread.  Sends a tiny
    // heartbeat every MP_HB_INTERVAL_MS so the link carries traffic even when
    // the client is idle/locked, and triggers a reconnect if the host has gone
    // silent for MP_STALL_MS.
    void arm_heartbeat() {
        hb_timer.expires_after( std::chrono::milliseconds( MP_HB_INTERVAL_MS ) );
        hb_timer.async_wait( [this]( const asio::error_code & ec ) {
            if( ec ) {
                return;   // cancelled — client_impl is being torn down
            }
            static const std::string hb = "{\"type\":\"heartbeat\"}\n";
            asio::error_code wec;
            asio::write( sock, asio::buffer( hb ), wec );   // io-thread write (same as client_send)
            const int64_t last = g_last_recv_ms.load();
            if( last > 0 && mp_now_ms() - last > MP_STALL_MS ) {
                mp_log( "[cdda-mp] HEARTBEAT: host silent >" + std::to_string( MP_STALL_MS ) +
                        "ms — treating link as dropped" );
                start_reconnect_sweep( "stall (no host msg)" );
                return;   // sweep rebuilds g_client; don't re-arm on the dying one
            }
            arm_heartbeat();
        } );
    }

    void start_read() {
        asio::async_read_until( sock, read_buf, '\n',
        [this]( const asio::error_code & ec, size_t /*n*/ ) {
            if( ec ) {
                std::cerr << "[cdda-mp] Server disconnected: " << ec.message() << std::endl;
                mp_log( "[cdda-mp] DISCONNECT: server closed connection (" + ec.message() +
                        "). If this fired during join, the host rejected the handshake "
                        "(version mismatch / wrong password) or is on a different build — "
                        "check the host's cdda-mp-server.log for a PROBE/JOIN REJECTED line." );
                // Ungraceful drop (tunnel blip / RTO timeout): try to silently
                // re-dial the same host instead of ending the session.  (The
                // stall watchdog above is the primary trigger when idle; this
                // catches the mid-send RTO case.)
                if( g_rc_enabled.load() ) {
                    start_reconnect_sweep( "link dropped (read error)" );
                } else {
                    // Reconnect disabled (intentional teardown / host goodbye): the
                    // game thread cleans up the host NPC via the synthetic message.
                    g_recv_queue.push( "{\"type\":\"state\",\"connected\":false}" );
                }
                return;
            }
            // Liveness: any inbound byte means the host is alive — feeds the stall
            // watchdog so a real silence is distinguishable from normal idle.
            g_last_recv_ms.store( mp_now_ms() );
            std::istream is( &read_buf );
            std::string line;
            std::getline( is, line );
            if( !line.empty() ) {
                std::string msg = mp_decompress_frame( std::move( line ) );
                if( !msg.empty() ) {
                    // An intentional end from the host (goodbye / session_ending)
                    // means DON'T auto-reconnect when the socket closes next.
                    if( msg.find( "\"type\":\"goodbye\"" ) != std::string::npos ||
                        msg.find( "\"type\":\"session_ending\"" ) != std::string::npos ) {
                        g_rc_enabled.store( false );
                        mp_log( "[cdda-mp] RECONNECT: disabled (host goodbye/session_ending)" );
                    }
                    g_recv_queue.push( std::move( msg ) );
                }
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
        // SO_KEEPALIVE backstop: lets the OS eventually reap a dead idle socket
        // even if the app-level heartbeat/stall path somehow misses it.
        asio::error_code ka_ec;
        g_client->sock.set_option( asio::socket_base::keep_alive( true ), ka_ec );
    }

    // Reset the liveness clock so the stall watchdog doesn't fire on a fresh/
    // reconnected socket before the first inbound message.
    g_last_recv_ms.store( mp_now_ms() );
    g_client->start_read();
    g_client->arm_heartbeat();
    g_client->io_thread = std::thread( []() {
        g_client->io_ctx.run();
    } );

    // Send a lightweight version_probe — validates version + password without
    // spawning the proxy NPC on the host.  The real join (which triggers proxy
    // spawn) is deferred until client_send_join() fires after char creation.
    std::string join_msg = "{\"type\":\"version_probe\"";
    if( !password.empty() ) {
        join_msg += ",\"password\":\"" + password + "\"";
    }
    if( !version.empty() ) {
        join_msg += ",\"version\":\"" + version + "\"";
    }
    join_msg += "}\n";

    mp_log( "[cdda-mp] HANDSHAKE: sent version_probe to " + host + ":" +
            std::to_string( port ) + " (our version='" +
            ( version.empty() ? std::string( "(none)" ) : version ) + "'" +
            ( password.empty() ? "" : ", password set" ) + ")" );
    asio::error_code wec;
    asio::write( g_client->sock, asio::buffer( join_msg ), wec );
    if( wec ) {
        std::cerr << "[cdda-mp] Failed to send join: " << wec.message() << std::endl;
        mp_log( "[cdda-mp] HANDSHAKE: failed to send version_probe — " + wec.message() );
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
                mp_log( "[cdda-mp] HANDSHAKE REJECTED by host: " + g_connect_error +
                        " (raw=" + msg + ")" );
                g_join_sent = false;
                g_client.reset();
                return false;
            }
            if( msg.find( "\"type\":\"welcome\"" ) != std::string::npos ) {
                // Probe accepted — version + password OK.  Extract the world
                // name immediately so the join dialog can show "Joining <World>"
                // before the game loop gets to process the welcome packet.
                const auto wpos = msg.find( "\"world\":\"" );
                if( wpos != std::string::npos ) {
                    const size_t ws = wpos + 9;
                    const size_t we = msg.find( '"', ws );
                    if( we != std::string::npos ) {
                        const std::string wn = msg.substr( ws, we - ws );
                        if( !wn.empty() && wn != "default" ) {
                            mp_set_client_host_world_name( wn );
                        }
                    }
                }
                // Host's character name — so the join dialog can show whose game
                // it is ("Joining <World> — <Host>'s game") before char select.
                const auto hpos = msg.find( "\"host_name\":\"" );
                if( hpos != std::string::npos ) {
                    const size_t hs = hpos + 12;
                    const size_t he = msg.find( '"', hs );
                    if( he != std::string::npos ) {
                        const std::string hn = msg.substr( hs, he - hs );
                        if( !hn.empty() ) {
                            mp_set_client_host_player_name( hn );
                        }
                    }
                }
                // Stash + pre-parse the welcome NOW (connect time, before char
                // creation) so start_game() can adopt the host seed + spawn-omt
                // before it builds the host-area overmap. The game-loop replay
                // below still fires on the first do_turn (idempotent).
                mp_store_pending_welcome( msg );
                // Store the welcome so the game-loop handler can adopt the seed.
                g_pending_join = "{\"type\":\"join\",\"name\":\"" + name + "\"";
                if( !password.empty() ) {
                    g_pending_join += ",\"password\":\"" + password + "\"";
                }
                if( !version.empty() ) {
                    g_pending_join += ",\"version\":\"" + version + "\"";
                }
                g_pending_join += "}\n";
                g_join_sent = false;
                // Replay the probe welcome so seed/world-name adoption fires
                // from the normal incoming-packet path on first do_turn.
                g_recv_queue.push( msg );
                std::cout << "[cdda-mp] Connected to " << host << ":" << port
                          << " as '" << name << "' — version accepted." << std::endl;
                mp_log( "[cdda-mp] HANDSHAKE: host accepted our version — connected to " +
                        host + ":" + std::to_string( port ) + " as '" + name + "'" );
                // Remember params + (re)enable auto-reconnect for this session so
                // a later ungraceful drop can re-dial the same host silently.
                g_rc_host = host;
                g_rc_port = port;
                g_rc_name = name;
                g_rc_password = password;
                g_rc_version = version;
                g_rc_enabled.store( true );
                return true;
            }
            // Any other packet (e.g. hello) — keep waiting.
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    }

    g_connect_error = "Timed out waiting for server response.";
    std::cerr << "[cdda-mp] Timed out waiting for welcome from server." << std::endl;
    mp_log( "[cdda-mp] HANDSHAKE: TIMED OUT after 5s waiting for host welcome/error. "
            "The host accepted the TCP connection but never answered the version_probe — "
            "host may be on an older build (no probe support) or not yet loaded into a world." );
    g_client.reset();
    return false;
}

std::string client_connect_error()
{
    return g_connect_error;
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

// Runs on its own detached thread (NOT the io thread — client_connect() tears
// down and re-creates g_client, which joins the old io thread).  Re-dials the
// last host with backoff; on success re-sends the stored join (no char
// creation — the avatar/world are still loaded) and the host's spawn path does
// the full resync.  Surfaces the real disconnect only after exhausting tries.
static void reconnect_worker()
{
    // Backoff schedule (ms) — ~15s of trying before giving up.  Generous enough
    // to ride out a several-second tunnel blip, short enough not to strand the
    // player if the host is truly gone.
    static const int delays_ms[] = { 500, 1000, 2000, 3000, 4000, 5000 };
    const int attempts = static_cast<int>( sizeof( delays_ms ) / sizeof( delays_ms[0] ) );
    for( int i = 0; i < attempts && g_rc_enabled.load(); ++i ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( delays_ms[i] ) );
        if( !g_rc_enabled.load() ) {
            break;
        }
        mp_log( "[cdda-mp] RECONNECT: attempt " + std::to_string( i + 1 ) + "/" +
                std::to_string( attempts ) + " -> " + g_rc_host + ":" +
                std::to_string( g_rc_port ) );
        // client_connect() rebuilds g_client + re-PROBEs; on success it leaves
        // g_pending_join armed with g_join_sent=false.
        if( client_connect( g_rc_host, g_rc_port, g_rc_name, g_rc_password, g_rc_version ) ) {
            client_send_join();   // resume in-game; host re-spawns + full-resyncs us
            g_recv_queue.push( "{\"type\":\"state\",\"reconnected\":true}" );
            mp_log( "[cdda-mp] RECONNECT: success on attempt " + std::to_string( i + 1 ) );
            g_rc_in_progress.store( false );
            return;
        }
        mp_log( "[cdda-mp] RECONNECT: attempt " + std::to_string( i + 1 ) +
                " failed (" + client_connect_error() + ")" );
    }
    mp_log( "[cdda-mp] RECONNECT: giving up — surfacing disconnect" );
    g_recv_queue.push( "{\"type\":\"state\",\"connected\":false}" );
    g_rc_in_progress.store( false );
}

bool client_is_reconnecting()
{
    return g_rc_in_progress.load();
}

void client_disable_reconnect()
{
    g_rc_enabled.store( false );
}

} // namespace cata_mp
