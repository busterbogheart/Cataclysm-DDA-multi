#include "mp_server.h"
#include "mp_queue.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

#include <zstd/zstd.h>
#include "catacharset.h"   // base64_encode — only pulls std headers, asio-safe

// Standalone Asio — no Boost dependency
#define ASIO_STANDALONE
#include <asio.hpp>

// Local-interface enumeration for the host "hosting at:" hint (mp_local_ipv4s).
// MUST follow asio.hpp on Windows (it pulls in winsock2.h; iphlpapi.h has to come
// after that, not before, or the legacy winsock1 ordering trips a redefinition).
#ifdef _WIN32
#include <iphlpapi.h>   // GetAdaptersAddresses — links -liphlpapi (see Makefile)
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

using asio::ip::tcp;

namespace cata_mp {

// Forward-declared (not #included) on purpose: pulling mp_gamestate.h into this
// TU drags in CDDA's enum_traits.h, whose generic operator++ collides with
// asio's std::atomic<long> increment in scheduler.hpp. We only need these two.
void mp_log( const std::string &msg );
unsigned int mp_host_world_seed();
std::string mp_get_host_world_name();
std::string mp_get_host_player_name();
std::string mp_host_omt_welcome_field();
std::string mp_host_active_mods_field();

// Heartbeat-based RTT: the client measures the true network round-trip (its
// heartbeat -> our immediate io-thread echo -> back) and mirrors that number to
// us in each heartbeat's "rtt" field, so the host's co-op panel shows the same
// latency.  Set on the io thread (do_read), read on the game thread.
static std::atomic<int> g_host_partner_rtt_ms{ -1 };
int mp_host_partner_rtt_ms();
int mp_host_partner_rtt_ms()
{
    return g_host_partner_rtt_ms.load();
}

// Escape a string for embedding in a JSON double-quoted value (host names can
// contain quotes/backslashes). Minimal — covers the chars that break parsing.
static std::string mp_json_escape( const std::string &s )
{
    std::string out;
    out.reserve( s.size() + 2 );
    for( const char c : s ) {
        if( c == '"' || c == '\\' ) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// Normalize a build-version string to its commit identity for the join
// handshake. getVersionString() is the git commit hash, but the Makefile
// appends build noise that differs between separately-built binaries of the
// SAME commit: "-dirty.<HHMMSS>" (a dirty tree, stamped with the build time)
// and "+SDL3" (rendering backend). The MP wire protocol is fixed by the
// commit, not by build time or render backend, so the handshake must compare
// only the commit token — otherwise no two machines' builds ever match.
static std::string mp_version_commit_id( const std::string &v )
{
    std::string s = v;
    const std::string::size_type d = s.find( "-dirty" );
    if( d != std::string::npos ) {
        s.erase( d );
    }
    const std::string::size_type p = s.find( '+' );
    if( p != std::string::npos ) {
        s.erase( p );
    }
    return s;
}

// True if two version strings name the same commit, tolerant of different hash
// abbreviation LENGTHS. A full clone and a shallow clone of the SAME commit
// abbreviate the short hash differently (e.g. host "648cb8a6f9" vs client
// "648cb8a"), so a plain != on the stripped ids falsely rejects them (the
// 2026-06-28 M4↔Linux-VM mismatch). git abbreviations are unambiguous within
// each repo, so comparing the shorter id as a prefix of the longer is safe.
static bool mp_version_commit_match( const std::string &a, const std::string &b )
{
    const std::string ca = mp_version_commit_id( a );
    const std::string cb = mp_version_commit_id( b );
    if( ca.empty() || cb.empty() ) {
        return ca == cb;
    }
    const std::string::size_type n = std::min( ca.size(), cb.size() );
    return ca.compare( 0, n, cb, 0, n ) == 0;
}

// Host→client wire compression.  The per-turn state broadcast (monster/map
// snapshot) is large, repetitive JSON; zstd shrinks it ~5-10×.  We keep the
// newline-delimited framing by wrapping the compressed bytes in a tiny JSON
// envelope {"z":"<base64 zstd>"} — base64 contains no JSON-special or newline
// chars, so it rides the existing reader AND the write-queue coalescing
// unchanged.  The client (mp_client_conn.cpp mp_decompress_frame) recognizes the
// {"z": prefix and reverses this.  Messages below the threshold (grants, acks)
// stay plain — the envelope overhead isn't worth it and tiny payloads don't
// compress.  The version handshake guarantees both ends run this same build, so
// there is no cross-version compatibility risk.
static constexpr size_t MP_COMPRESS_THRESHOLD = 512;

static std::string mp_compress_frame( const std::string &msg )
{
    if( msg.size() < MP_COMPRESS_THRESHOLD ) {
        return msg;
    }
    // Compress the payload without its trailing newline; the envelope adds its own.
    size_t body_len = msg.size();
    if( body_len > 0 && msg.back() == '\n' ) {
        --body_len;
    }
    const size_t bound = ZSTD_compressBound( body_len );
    std::string comp;
    comp.resize( bound );
    const size_t n = ZSTD_compress( &comp[0], bound, msg.data(), body_len, 3 );
    if( ZSTD_isError( n ) || n >= body_len ) {
        // Compression failed or didn't help — send the original plaintext.
        return msg;
    }
    return "{\"z\":\"" + base64_encode( std::string_view( comp.data(), n ) ) + "\"}\n";
}

// ---------------------------------------------------------------------------
// client_session — owns one TCP connection
// ---------------------------------------------------------------------------

struct client_session : public std::enable_shared_from_this<client_session> {
    tcp::socket socket;
    asio::streambuf read_buf;
    std::string name;
    bool authenticated = false;

    std::function<void( std::shared_ptr<client_session>, const std::string & )> on_message;
    std::function<void( std::shared_ptr<client_session> )> on_disconnect;

    // Outgoing write queue — only one async_write may be in flight at a time.
    // All send() / do_write() calls happen on the single Asio thread so no mutex needed.
    std::deque<std::string> write_queue_;
    bool writing_ = false;

    explicit client_session( tcp::socket sock )
        : socket( std::move( sock ) ) {}

    void start() {
        // Disable Nagle — see client_connect(). The host's grant packets are
        // tiny and must not be batched on a high-latency link or the lockstep
        // turn cycle wedges (works on LAN, hangs over the internet).
        std::error_code nd_ec;
        socket.set_option( tcp::no_delay( true ), nd_ec );
        // SO_KEEPALIVE backstop (see client_connect) — reaps a dead idle peer
        // socket even if the app-level heartbeat/stall path misses it.
        std::error_code ka_ec;
        socket.set_option( asio::socket_base::keep_alive( true ), ka_ec );
        send( "{\"type\":\"hello\",\"protocol\":\"cdda-mp\",\"version\":\"0.1\"}\n" );
        do_read();
    }

    void send( const std::string &msg ) {
        write_queue_.push_back( mp_compress_frame( msg ) );
        if( !writing_ ) {
            do_write();
        }
    }

    void do_write() {
        if( write_queue_.empty() ) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        // Coalesce EVERY queued message into one buffer so N messages enqueued in
        // a single turn become one write() / one batch of back-to-back segments,
        // instead of N separate writes (with Nagle off, N tiny packets — the
        // small-packet storm).  Each queued message already ends in '\n', so the
        // client's newline-framed reader still splits them apart correctly, and an
        // older non-coalescing client is unaffected (wire-compatible).  Messages
        // posted during this async_write land back in write_queue_ and coalesce on
        // the next pass.
        auto buf = std::make_shared<std::string>();
        for( const std::string &m : write_queue_ ) {
            buf->append( m );
        }
        const size_t buf_bytes = buf->size();
        write_queue_.clear();
        // Diagnostic for the 2026-07-03 "host crafting -> server times out" report:
        // a heartbeat enqueued behind a huge FF-broadcast buffer can't reach the
        // wire until this async_write completes.  Only log large/slow flushes
        // (skip the steady-state tiny-grant traffic) so this doesn't flood the log.
        const bool track_flush = buf_bytes > 65536;
        const auto flush_start = track_flush ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
        if( track_flush ) {
            mp_log( "[cdda-mp] HOST-WRITE-BEGIN: bytes=" + std::to_string( buf_bytes ) );
        }
        asio::async_write( socket, asio::buffer( *buf ),
        [self, buf, buf_bytes, track_flush, flush_start]( std::error_code ec, std::size_t ) {
            if( track_flush ) {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - flush_start ).count();
                mp_log( "[cdda-mp] HOST-WRITE-DONE: bytes=" + std::to_string( buf_bytes ) +
                        " elapsed_ms=" + std::to_string( elapsed_ms ) +
                        " ec=" + ec.message() +
                        " queued_after=" + std::to_string( self->write_queue_.size() ) );
            }
            if( ec ) {
                self->disconnect();
                return;
            }
            self->do_write();
        } );
    }

    void disconnect() {
        std::error_code ec;
        socket.close( ec );
        if( on_disconnect ) {
            on_disconnect( shared_from_this() );
        }
    }

    private:
        void do_read() {
            auto self = shared_from_this();
            asio::async_read_until( socket, read_buf, '\n',
            [self]( std::error_code ec, std::size_t ) {
                if( ec ) {
                    self->disconnect();
                    return;
                }
                std::istream stream( &self->read_buf );
                std::string line;
                std::getline( stream, line );
                if( !line.empty() ) {
                    // Heartbeat RTT (io thread): echo the client's ping stamp
                    // IMMEDIATELY so the measured round-trip is pure network
                    // latency, not game-loop cadence; and adopt the client's
                    // measured RTT for the host's own co-op panel.  Client
                    // heartbeats are sent uncompressed, so parse straight off
                    // the raw line.
                    if( line.find( "\"type\":\"heartbeat\"" ) != std::string::npos ) {
                        const size_t cp = line.find( "\"cp\":" );
                        if( cp != std::string::npos ) {
                            const long long stamp = std::strtoll( line.c_str() + cp + 5, nullptr, 10 );
                            self->send( "{\"type\":\"heartbeat\",\"pong\":" +
                                        std::to_string( stamp ) + "}\n" );
                        }
                        const size_t rp = line.find( "\"rtt\":" );
                        if( rp != std::string::npos ) {
                            g_host_partner_rtt_ms.store(
                                static_cast<int>( std::strtol( line.c_str() + rp + 6, nullptr, 10 ) ) );
                        }
                    }
                    if( self->on_message ) {
                        self->on_message( self, line );
                    }
                }
                self->do_read();
            } );
        }
};

// ---------------------------------------------------------------------------
// server::impl — holds the Asio io_context and acceptor
// ---------------------------------------------------------------------------

struct server::impl {
    asio::io_context io_ctx;
    tcp::acceptor acceptor;
    asio::steady_timer hb_timer{ io_ctx };   // host->client heartbeat cadence

    impl( uint16_t port )
        : acceptor( io_ctx, tcp::endpoint( tcp::v4(), port ) ) {}
};

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

server::server( uint16_t port, std::string password, std::string version )
    : port_( port )
    , password_( std::move( password ) )
    , version_( std::move( version ) )
    , impl_( std::make_unique<impl>( port ) ) {}

server::~server() = default;

void server::arm_heartbeat() {
    impl_->hb_timer.expires_after( std::chrono::milliseconds( 1500 ) );
    impl_->hb_timer.async_wait( [this]( const std::error_code & ec ) {
        if( ec ) {
            return;   // cancelled (server stopping)
        }
        // Beat to every authenticated client.  On the io thread, so it fires even
        // when the host's game thread is parked in a modal.  Clients drop it after
        // stamping their liveness clock; pre-JOIN sessions aren't sent one.
        {
            std::lock_guard<std::mutex> lock( clients_mutex_ );
            for( auto &c : clients_ ) {
                if( c->authenticated ) {
                    c->send( "{\"type\":\"heartbeat\"}\n" );
                }
            }
        }
        arm_heartbeat();
    } );
}

void server::run() {
    std::cout << "[cdda-mp] Server listening on port " << port_ << std::endl;
    do_accept();
    arm_heartbeat();
    impl_->io_ctx.run();
}

void server::stop() {
    impl_->io_ctx.stop();
}

void server::broadcast( const std::string &msg ) {
    std::lock_guard<std::mutex> lock( clients_mutex_ );
    for( auto &c : clients_ ) {
        c->send( msg );
    }
}

void server::post_broadcast( const std::string &msg ) {
    asio::post( impl_->io_ctx, [this, msg]() { broadcast( msg ); } );
}

void server::do_accept() {
    impl_->acceptor.async_accept(
    [this]( std::error_code ec, tcp::socket socket ) {
        if( !ec ) {
            auto session = std::make_shared<client_session>( std::move( socket ) );

            session->on_message = [this]( auto sess, auto msg ) {
                on_message( sess, msg );
            };
            session->on_disconnect = [this]( auto sess ) {
                on_client_disconnected( sess );
            };

            on_client_connected( session );
            session->start();
        }
        do_accept();
    } );
}

void server::on_client_connected( std::shared_ptr<client_session> session ) {
    {
        std::lock_guard<std::mutex> lock( clients_mutex_ );
        clients_.push_back( session );
    }
    std::cout << "[cdda-mp] Client connected. Total: " << clients_.size() << std::endl;

    if( clients_.size() > 2 ) {
        session->send( "{\"type\":\"error\",\"message\":\"Server is full (max 2 players)\"}\n" );
        session->disconnect();
    }
}

void server::on_client_disconnected( std::shared_ptr<client_session> session ) {
    {
        std::lock_guard<std::mutex> lock( clients_mutex_ );
        clients_.erase(
            std::remove( clients_.begin(), clients_.end(), session ),
            clients_.end()
        );
    }
    std::string name = session->name.empty() ? "unknown" : session->name;
    std::cout << "[cdda-mp] Client '" << name << "' disconnected. Total: " <<
              clients_.size() << std::endl;

    if( session->authenticated ) {
        // Only the CURRENT active session ending is a real disconnect.  If this is
        // a superseded/stale session (an old socket dying after the client already
        // reconnected on a new one), do NOT push a disconnect — that would evict
        // the reconnected player.  Order-independent: the active session is whoever
        // JOINed most recently.
        if( active_session_.lock() == session ) {
            active_session_.reset();
            broadcast( "{\"type\":\"player_left\",\"name\":\"" + name + "\"}\n" );
            get_mp_queue().push( { cata_mp::mp_event::type::disconnect, name, "" } );
        } else {
            mp_log( "[cdda-mp] disconnect: superseded/stale session for '" + name +
                    "' closed — not ending the co-op session (reconnect in progress)" );
        }
    }
}

// Extract a string value from a simple JSON message, tolerating optional spaces
// around the colon (handles both "key":"val" and "key": "val").
static std::string json_get_str( const std::string &json, const std::string &key )
{
    for( const std::string &sep : { std::string( "\":\"" ), std::string( "\": \"" ) } ) {
        std::string needle = "\"" + key + sep;
        auto pos = json.find( needle );
        if( pos != std::string::npos ) {
            pos += needle.size();
            auto end = json.find( '"', pos );
            if( end != std::string::npos ) {
                return json.substr( pos, end - pos );
            }
        }
    }
    return "";
}

void server::on_message( std::shared_ptr<client_session> session, const std::string &msg ) {
    std::cout << "[cdda-mp] recv: " << msg << std::endl;

    const std::string type = json_get_str( msg, "type" );

    // Lightweight pre-join probe: validate version + password immediately
    // without spawning the proxy NPC or queuing a connect event.  Lets the
    // client surface a version mismatch before the character creation UI.
    if( type == "version_probe" ) {
        const std::string probe_ver = json_get_str( msg, "version" );
        mp_log( "[cdda-mp] PROBE recv: client_ver='" +
                ( probe_ver.empty() ? std::string( "(none)" ) : probe_ver ) +
                "' host_ver='" + version_ + "' -> client_commit=" +
                mp_version_commit_id( probe_ver ) + " host_commit=" +
                mp_version_commit_id( version_ ) );
        if( !version_.empty() ) {
            const std::string client_ver = json_get_str( msg, "version" );
            if( !mp_version_commit_match( client_ver, version_ ) ) {
                const std::string errmsg = "Version mismatch. Host: " + version_ +
                                           " Client: " + ( client_ver.empty() ? "(unknown)" : client_ver );
                session->send( "{\"type\":\"error\",\"message\":\"" + errmsg + "\"}\n" );
                mp_log( "[cdda-mp] PROBE REJECTED — version mismatch. " + errmsg +
                        " (host and client are on different builds; both must run the same release)" );
                session->disconnect();
                return;
            }
        }
        if( !password_.empty() ) {
            const std::string provided = json_get_str( msg, "password" );
            if( provided != password_ ) {
                session->send( "{\"type\":\"error\",\"message\":\"Wrong password\"}\n" );
                mp_log( "[cdda-mp] PROBE REJECTED — wrong password" );
                session->disconnect();
                return;
            }
        }
        // Probe accepted — send world name + seed + host OMT so the client can
        // display "Joining <world>" AND adopt the seed + spawn location before
        // character creation runs (client start_game() needs both before it
        // builds the host-area overmap; the post-JOIN welcome arrives too late —
        // it lands on the first do_turn, after start_game has already generated
        // the overmap with its own rng_bits() seed → ocean-spawn / "different
        // overmap" co-op join regression, 2026-06-21).
        mp_log( "[cdda-mp] PROBE accepted — version OK; sending welcome (world='" +
                mp_get_host_world_name() + "')" );
        session->send( "{\"type\":\"welcome\",\"player_id\":\"probe\""
                       ",\"world\":\"" + mp_get_host_world_name() + "\""
                       ",\"host_name\":\"" + mp_json_escape( mp_get_host_player_name() ) + "\""
                       ",\"current_turn\":0,\"seed\":" +
                       std::to_string( mp_host_world_seed() ) +
                       mp_host_omt_welcome_field() +
                       mp_host_active_mods_field() + "}\n" );
        return;
    }

    if( type == "join" ) {
        // Extract name
        std::string name = json_get_str( msg, "name" );
        if( name.empty() ) {
            name = "player";
        }

        // Check version compatibility — reject mismatched binaries. Compare
        // commit identity only (mp_version_commit_id), so two builds of the
        // same commit connect even if one tree was dirty / built at a
        // different time / uses a different render backend.
        if( !version_.empty() ) {
            const std::string client_ver = json_get_str( msg, "version" );
            if( !mp_version_commit_match( client_ver, version_ ) ) {
                const std::string errmsg = "Version mismatch. Host: " + version_ +
                                           " Client: " + ( client_ver.empty() ? "(unknown)" : client_ver );
                session->send( "{\"type\":\"error\",\"message\":\"" + errmsg + "\"}\n" );
                mp_log( "[cdda-mp] JOIN REJECTED — version mismatch. " + errmsg +
                        " client_commit=" + mp_version_commit_id( client_ver ) +
                        " host_commit=" + mp_version_commit_id( version_ ) );
                session->disconnect();
                return;
            }
        }

        // Check password
        if( !password_.empty() ) {
            const std::string provided = json_get_str( msg, "password" );
            if( provided != password_ ) {
                session->send( "{\"type\":\"error\",\"message\":\"Wrong password\"}\n" );
                mp_log( "[cdda-mp] JOIN REJECTED — wrong password (name='" + name + "')" );
                session->disconnect();
                return;
            }
        }

        session->name = name;
        session->authenticated = true;
        // This session is now the active one.  A prior session for the same player
        // (a reconnect's old socket) is hereby superseded — its later close won't
        // end the co-op session (see on_client_disconnected).
        active_session_ = session;

        // Include the host's worldgen seed so the client adopts it before
        // generating the host-area overmap — otherwise it renders its own
        // randomly-seeded terrain outside the tile-synced bubble.
        const std::string wname = mp_get_host_world_name();
        session->send( "{\"type\":\"welcome\",\"player_id\":\"" + name +
                       "\",\"world\":\"" + wname +
                       "\",\"host_name\":\"" + mp_json_escape( mp_get_host_player_name() ) +
                       "\",\"current_turn\":0,\"seed\":" +
                       std::to_string( mp_host_world_seed() ) +
                       mp_host_omt_welcome_field() +
                       mp_host_active_mods_field() + "}\n" );
        mp_log( "[cdda-mp] SEED: welcome sent host seed " +
                std::to_string( mp_host_world_seed() ) + " to '" + name + "'" );

        broadcast( "{\"type\":\"player_joined\",\"name\":\"" + name + "\"}\n" );
        mp_log( "[cdda-mp] JOIN accepted — player '" + name + "' authenticated and connected" );

        // Notify game loop to spawn this player's character
        get_mp_queue().push( { cata_mp::mp_event::type::connect, name, "" } );

    } else if( type == "quit" ) {
        session->send( "{\"type\":\"goodbye\"}\n" );
        session->disconnect();

    } else if( session->authenticated ) {
        // Route action to game loop
        get_mp_queue().push( { cata_mp::mp_event::type::action, session->name, msg } );

    } else {
        // Unauthenticated session sent something that isn't version_probe / join /
        // quit — e.g. a heartbeat, or a straggler (chat/state/action) still in the
        // client's send queue when it reconnects mid-game.  IGNORE it (do NOT
        // close): the version_probe handshake already validated the build, so an
        // unexpected type here is a buffered/interleaved message on a reconnect,
        // not an incompatible client.  Closing here dropped the reconnecting
        // socket the instant a buffered 'chat' arrived, forcing the client to
        // sweep again — the reconnect-flapping half of the 2026-07-01 bug.  (An
        // older client that truly predates version_probe simply never gets a
        // welcome and times out on its own end; we don't need to force-close it.)
        mp_log( "[cdda-mp] HANDSHAKE: ignoring stray pre-auth message type='" +
                ( type.empty() ? std::string( "(none)" ) : type ) +
                "' (pre-JOIN) — waiting for join." );
    }
}

// ---------------------------------------------------------------------------
// Entry point called from main()
// ---------------------------------------------------------------------------

static server *active_server_ = nullptr;
static std::atomic<bool> server_thread_running_{ false };

server *get_active_server()
{
    return active_server_;
}

bool is_server_thread_running()
{
    return server_thread_running_.load();
}

// True for address ranges commonly handed out by mesh/VPN software — Tailscale
// (100.64.0.0/10 CGNAT), Radmin (26/8), Hamachi (25/8).  Surfaced FIRST in the
// host hint because that's usually the address a remote partner actually uses;
// the physical-LAN address only helps a same-network partner.  Best-effort sort
// key, not exhaustive (ZeroTier etc. vary) — we never DROP an address, only order.
static bool mp_ip_is_vpn_like( const std::string &ip )
{
    int a = 0, b = 0;
    if( std::sscanf( ip.c_str(), "%d.%d", &a, &b ) < 2 ) {
        return false;
    }
    if( a == 26 || a == 25 ) {
        return true;                          // Radmin / Hamachi
    }
    if( a == 100 && b >= 64 && b <= 127 ) {
        return true;                          // Tailscale CGNAT 100.64.0.0/10
    }
    return false;
}

std::vector<std::string> mp_local_ipv4s()
{
    // Cache: interfaces don't change within a session and the HUD asks ~10x/sec.
    static std::vector<std::string> cached;
    static bool resolved = false;
    if( resolved ) {
        return cached;
    }
    resolved = true;

    auto add = [&]( const std::string & ip ) {
        if( ip.empty() || ip == "0.0.0.0" ) {
            return;
        }
        if( ip.rfind( "127.", 0 ) == 0 || ip.rfind( "169.254.", 0 ) == 0 ) {
            return;                            // loopback / link-local APIPA — useless to share
        }
        if( std::find( cached.begin(), cached.end(), ip ) == cached.end() ) {
            cached.push_back( ip );
        }
    };

#ifdef _WIN32
    ULONG buf_len = 16384;
    std::vector<char> buf( buf_len );
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>( buf.data() );
    if( GetAdaptersAddresses( AF_INET,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, addrs, &buf_len ) == NO_ERROR ) {
        for( auto *a = addrs; a; a = a->Next ) {
            if( a->OperStatus != IfOperStatusUp ) {
                continue;
            }
            for( auto *u = a->FirstUnicastAddress; u; u = u->Next ) {
                auto *sa = reinterpret_cast<sockaddr_in *>( u->Address.lpSockaddr );
                char ipbuf[INET_ADDRSTRLEN] = { 0 };
                inet_ntop( AF_INET, &sa->sin_addr, ipbuf, sizeof( ipbuf ) );
                add( ipbuf );
            }
        }
    }
#else
    ifaddrs *ifs = nullptr;
    if( getifaddrs( &ifs ) == 0 ) {
        for( ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next ) {
            if( !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ) {
                continue;
            }
            if( !( ifa->ifa_flags & IFF_UP ) || ( ifa->ifa_flags & IFF_LOOPBACK ) ) {
                continue;
            }
            auto *sa = reinterpret_cast<sockaddr_in *>( ifa->ifa_addr );
            char ipbuf[INET_ADDRSTRLEN] = { 0 };
            inet_ntop( AF_INET, &sa->sin_addr, ipbuf, sizeof( ipbuf ) );
            add( ipbuf );
        }
        freeifaddrs( ifs );
    }
#endif

    // VPN-like addresses first (most likely the share target); stable otherwise.
    std::stable_sort( cached.begin(), cached.end(),
    []( const std::string & x, const std::string & y ) {
        return mp_ip_is_vpn_like( x ) && !mp_ip_is_vpn_like( y );
    } );
    return cached;
}

void run_server( uint16_t port, const std::string &password, const std::string &version ) {
    server_thread_running_.store( true );
    try {
        // The server ctor binds the listen socket and THROWS if the port is
        // still held (e.g. a prior session's socket not yet released). Surface
        // it via mp_log — std::cerr alone is invisible in the in-game log and
        // hid this failure: a thrown ctor leaves active_server_ null, so
        // is_hosting() stays false and the whole host turn body is skipped.
        server srv( port, password, version );
        active_server_ = &srv;
        srv.run();
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] SERVER-ERROR: " ) + e.what() );
        std::cerr << "[cdda-mp] Server error: " << e.what() << std::endl;
        // Never call exit() from a background thread — it runs SDL atexit handlers
        // on the wrong thread, which crashes on macOS (EXC_BREAKPOINT in Cocoa).
    }
    active_server_ = nullptr;
    // Cleared last: the server object above has now destructed and released the
    // listen socket, so a waiter on this flag knows the port is free.
    server_thread_running_.store( false );
}

} // namespace cata_mp
