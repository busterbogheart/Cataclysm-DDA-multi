#include "mp_server.h"
#include "mp_queue.h"

#include <iostream>
#include <algorithm>

// Standalone Asio — no Boost dependency
#define ASIO_STANDALONE
#include <asio.hpp>

using asio::ip::tcp;

namespace cata_mp {

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
        send( "{\"type\":\"hello\",\"protocol\":\"cdda-mp\",\"version\":\"0.1\"}\n" );
        do_read();
    }

    void send( const std::string &msg ) {
        write_queue_.push_back( msg );
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
        // Move the front message into a shared_ptr so it stays alive across the async op.
        auto buf = std::make_shared<std::string>( std::move( write_queue_.front() ) );
        write_queue_.pop_front();
        asio::async_write( socket, asio::buffer( *buf ),
        [self, buf]( std::error_code ec, std::size_t ) {
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
                if( !line.empty() && self->on_message ) {
                    self->on_message( self, line );
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

    impl( uint16_t port )
        : acceptor( io_ctx, tcp::endpoint( tcp::v4(), port ) ) {}
};

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

server::server( uint16_t port, std::string password )
    : port_( port )
    , password_( std::move( password ) )
    , impl_( std::make_unique<impl>( port ) ) {}

server::~server() = default;

void server::run() {
    std::cout << "[cdda-mp] Server listening on port " << port_ << std::endl;
    do_accept();
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
        broadcast( "{\"type\":\"player_left\",\"name\":\"" + name + "\"}\n" );
        get_mp_queue().push( { cata_mp::mp_event::type::disconnect, name, "" } );
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

    if( type == "join" ) {
        // Extract name
        std::string name = json_get_str( msg, "name" );
        if( name.empty() ) {
            name = "player";
        }

        // Check password
        if( !password_.empty() ) {
            const std::string provided = json_get_str( msg, "password" );
            if( provided != password_ ) {
                session->send( "{\"type\":\"error\",\"message\":\"Wrong password\"}\n" );
                session->disconnect();
                return;
            }
        }

        session->name = name;
        session->authenticated = true;

        session->send( "{\"type\":\"welcome\",\"player_id\":\"" + name +
                       "\",\"world\":\"default\",\"current_turn\":0}\n" );

        broadcast( "{\"type\":\"player_joined\",\"name\":\"" + name + "\"}\n" );
        std::cout << "[cdda-mp] Player '" << name << "' joined." << std::endl;

        // Notify game loop to spawn this player's character
        get_mp_queue().push( { cata_mp::mp_event::type::connect, name, "" } );

    } else if( type == "quit" ) {
        session->send( "{\"type\":\"goodbye\"}\n" );
        session->disconnect();

    } else if( session->authenticated ) {
        // Route action to game loop
        get_mp_queue().push( { cata_mp::mp_event::type::action, session->name, msg } );
    }
}

// ---------------------------------------------------------------------------
// Entry point called from main()
// ---------------------------------------------------------------------------

static server *active_server_ = nullptr;

server *get_active_server()
{
    return active_server_;
}

void run_server( uint16_t port, const std::string &password ) {
    try {
        server srv( port, password );
        active_server_ = &srv;
        srv.run();
    } catch( const std::exception &e ) {
        std::cerr << "[cdda-mp] Server error: " << e.what() << std::endl;
        // Never call exit() from a background thread — it runs SDL atexit handlers
        // on the wrong thread, which crashes on macOS (EXC_BREAKPOINT in Cocoa).
    }
    active_server_ = nullptr;
}

} // namespace cata_mp
