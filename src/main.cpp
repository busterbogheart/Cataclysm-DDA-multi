/* Main Loop for cataclysm
 * Linux only I guess
 * But maybe not
 * Who knows
 */

// KG: Yes, the above is inaccurate now. It's also a poem, it stays.

// IWYU pragma: no_include <sys/signal.h>
#include <algorithm>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <exception>
#include <functional>
#include <iostream>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "mp_server.h"
#include "mp_gamestate.h"
#include "mp_client_conn.h"
#if defined(_WIN32)
#include "cata_allocator.h"
#include "platform_win.h"
#else
#include <csignal>
#endif

#include <flatbuffers/util.h>

#include "cached_options.h"
#include "cata_allocator.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "color.h"
#include "compatibility.h"
#include "crash.h"
#include "string_formatter.h"
#include "cursesdef.h"
#include "debug.h"
#include "do_turn.h"
#include "event.h"
#include "event_bus.h"
#include "filesystem.h"
#include "game.h"
#include "game_constants.h"
#include "game_ui.h"
#include "get_version.h"
#include "help.h"
#include "input.h"
#include "main_menu.h"
#include "worldfactory.h"
#include "mapsharing.h"
#include "memory_fast.h"
#include "options.h"
#include "ordered_static_globals.h"
#include "output.h"
#include "path_info.h"
#include "rng.h"
#include "system_locale.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "cata_imgui.h"
#if defined(MACOSX) || defined(__CYGWIN__)
#   include <unistd.h> // getpid()
#endif

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#if defined(PREFIX)
#   undef PREFIX
#   include "prefix.h"
#endif

class ui_adaptor;

#if defined(TILES) || defined(SDL_SOUND)
#   include "sdl_version_wrappers.h"
#endif

#if defined(__ANDROID__)
#include <SDL_filesystem.h>
#include <SDL_keyboard.h>
#include <SDL_system.h>
#include <android/log.h>
#include <unistd.h>

// Taken from: https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
// Force Android standard output to adb logcat output

static int pfd[2];
static pthread_t thr;
static const char *tag = "cdda";

static void *thread_func( void * )
{
    ssize_t rdsz;
    char buf[128];
    for( ;; ) {
        if( ( ( rdsz = read( pfd[0], buf, sizeof buf - 1 ) ) > 0 ) ) {
            if( buf[rdsz - 1] == '\n' ) {
                --rdsz;
            }
            buf[rdsz] = 0;  /* add null-terminator */
            __android_log_write( ANDROID_LOG_DEBUG, tag, buf );
        }
    }
    return 0;
}

int start_logger( const char *app_name )
{
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf( stdout, 0, _IOLBF, 0 );
    setvbuf( stderr, 0, _IONBF, 0 );

    /* create the pipe and redirect stdout and stderr */
    pipe( pfd );
    dup2( pfd[1], 1 );
    dup2( pfd[1], 2 );

    /* spawn the logging thread */
    if( pthread_create( &thr, 0, thread_func, 0 ) == -1 ) {
        return -1;
    }
    pthread_detach( thr );
    return 0;
}

#endif //__ANDROID__

namespace
{

#if defined(_WIN32) and defined(TILES)
// Used only if AttachConsole() works
FILE *CONOUT;
#endif
void exit_handler( int s )
{
    const int old_timeout = inp_mngr.get_timeout();
    inp_mngr.reset_timeout();
    if( s != 2 || test_mode || query_yn( _( "Really Quit?  All unsaved changes will be lost." ) ) ) {
        deinitDebug();

        int exit_status = 0;
        g.reset();

        catacurses::endwin();

#if defined(__ANDROID__)
        // Avoid capturing SIGABRT on exit on Android in crash report
        // Can be removed once the SIGABRT on exit problem is fixed
        signal( SIGABRT, SIG_DFL );
#endif

        imclient.reset();
        exit( exit_status );
    }
    inp_mngr.set_timeout( old_timeout );
    ui_manager::redraw_invalidated();
    catacurses::doupdate();
}

struct arg_handler {
    //! Handler function to be invoked when this argument is encountered. The handler will be
    //! called with the number of parameters after the flag was encountered, along with the array
    //! of following parameters. It must return an integer indicating how many parameters were
    //! consumed by the call or -1 to indicate that a required argument was missing.
    using handler_method = std::function<int ( int, const char ** )>;

    std::string_view flag;  //!< The commandline parameter to handle (e.g., "--seed").
    std::string_view param_documentation;  //!< Human readable description of this arguments parameter.
    std::string_view documentation;  //!< Human readable documentation for this argument.
    std::string_view help_group; //!< Section of the help message in which to include this argument.
    int num_args; //!< How many further arguments are expected for this parameter (usually 0 or 1).
    handler_method handler;  //!< The callback to be invoked when this argument is encountered.
};

template<typename FirstPassArgs, typename SecondPassArgs>
void printHelpMessage( const FirstPassArgs &first_pass_arguments,
                       const SecondPassArgs &second_pass_arguments )
{
    // Group all arguments by help_group.
    std::multimap<std::string, const arg_handler *> help_map;
    for( const arg_handler &handler : first_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }
    for( const arg_handler &handler : second_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }

    std::cout << "Command line parameters:\n";
    std::string current_help_group;
    for( std::pair<const std::string, const arg_handler *> &help_entry : help_map ) {
        if( help_entry.first != current_help_group ) {
            current_help_group = help_entry.first;
            std::cout << "\n" << current_help_group << "\n";
        }

        const arg_handler *handler = help_entry.second;
        std::cout << handler->flag << " " << handler->param_documentation;
        if( !handler->documentation.empty() ) {
            std::cout << "\n    " << handler->documentation << "\n";
        }
    }
    std::cout << std::endl;
}

/**
 * Displays current application version and compile options values
 */
void printVersionMessage()
{
#if defined(TILES)
    const bool hasTiles = true;
#else
    const bool hasTiles = false;
#endif

#if defined(SDL_SOUND)
    const bool hasSound = true;
#else
    const bool hasSound = false;
#endif

    printf( "Cataclysm Dark Days Ahead: %s\n\n"
            "%ctiles, %csound\n\n"
            "data dir: %s\nuser dir: %s\n",
            getVersionString(),
            hasTiles ? '+' : '-',
            hasSound ? '+' : '-',
            PATH_INFO::datadir().c_str(),
            PATH_INFO::user_dir().c_str() );
}

void process_args( const char **argv, int argc, const std::vector<arg_handler> &arg_handlers )
{
    while( argc ) {
        bool arg_handled = false;
        for( const arg_handler &handler : arg_handlers ) {
            if( handler.flag == argv[0] ) {
                argc--;
                argv++;
                if( argc < handler.num_args ) {
                    std::cout << "Missing expected argument to command line parameter " << handler.flag << std::endl;
                    std::exit( 1 );
                }
                int args_consumed = handler.handler( argc, argv );
                if( args_consumed < 0 ) {
                    printf( "Failed parsing parameter '%s'\n", *( argv - 1 ) );
                    std::exit( 1 );
                }
                argc -= args_consumed;
                argv += args_consumed;
                arg_handled = true;
                break;
            }
        }
        // Skip other options.
        if( !arg_handled ) {
            --argc;
            ++argv;
        }
    }
}

struct cli_opts {
    int seed = time( nullptr );
    bool verifyexit = false;
    bool noverify = false;
    bool check_mods = false;
    std::vector<std::string> opts;
    std::string world; /** if set try to load first save in this world on startup */
    bool disable_ascii_art = false;
    // Multiplayer
    bool server_mode = false;
    bool host_mode = false;
    uint16_t server_port = 8080;
    std::string server_password;
    // Client mode
    bool client_mode = false;
    std::string client_host;
    uint16_t client_port = 8080;
    std::string client_name;
    std::string client_password;
    std::string char_name;  // --char: which save to load (character name)
};

cli_opts parse_commandline( int argc, const char **argv )
{
    cli_opts result;

    constexpr std::string_view section_default;
    constexpr std::string_view section_map_sharing = "Map sharing";
    constexpr std::string_view section_user_directory = "User directories";
    constexpr std::string_view section_accessibility = "Accessibility";
    const std::vector<arg_handler> first_pass_arguments = {{
            {
                "--seed", "<string of letters and or numbers>",
                "Sets the random number generator's seed value",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    const unsigned char *hash_input = reinterpret_cast<const unsigned char *>( params[0] );
                    result.seed = djb2_hash( hash_input );
                    return 1;
                }
            },
            {
                "--jsonverify", {},
                "Checks the CDDA json files and exits",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.verifyexit = true;
                    return 0;
                }
            },
            {
                "--check-mods", "[mod…]",
                "Checks the json files belonging to given CDDA mod and exits",
                section_default,
                1,
                [&result]( int n, const char **params ) -> int {
                    result.check_mods = true;
                    test_mode = true;
                    for( int i = 0; i < n; ++i )
                    {
                        result.opts.emplace_back( params[ i ] );
                    }
                    return 0;
                }
            },
            {
                "--noverify", {},
                "Skips JSON verification",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.noverify = true;
                    return 0;
                }
            },
            {
                "--world", "<name>",
                "Load world",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.world = params[0];
                    return 1;
                }
            },
            {
                "--basepath", "<path>",
                "Base path for all game data subdirectories",
                section_default,
                1,
                []( int, const char **params )
                {
                    PATH_INFO::init_base_path( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--shared", {},
                "Activates the map-sharing mode",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setSharing( true );
                    MAP_SHARING::setCompetitive( true );
                    MAP_SHARING::setWorldmenu( false );
                    return 0;
                }
            },
            {
                "--username", "<name>",
                "Instructs map-sharing code to use this name for your character.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::setUsername( params[0] );
                    return 1;
                }
            },
            {
                "--addadmin", "<username>",
                "Instructs map-sharing code to use this name for your character and give you "
                "access to the cheat functions.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addAdmin( params[0] );
                    return 1;
                }
            },
            {
                "--adddebugger", "<username>",
                "Informs map-sharing code that you're running inside a debugger",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addDebugger( params[0] );
                    return 1;
                }
            },
            {
                "--competitive", {},
                "Instructs map-sharing code to disable access to the in-game cheat functions",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setCompetitive( true );
                    return 0;
                }
            },
            {
                "--userdir", "<path>",
                // NOLINTNEXTLINE(cata-text-style): the dot is not a period
                "Base path for user-overrides to files from the ./data directory and named below",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::init_user_dir( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--disable-ascii-art", {},
                "Disable aesthetic ascii art in menus and descriptions.",
                section_accessibility,
                0,
                [&result]( int, const char ** ) -> int {
                    result.disable_ascii_art = true;
                    return 0;
                }
            },
            {
                "--server", {},
                "Run as a headless multiplayer server (no display required)",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.server_mode = true;
                    return 0;
                }
            },
            {
                "--host", {},
                "Host a multiplayer game — play normally while accepting one remote player",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.host_mode = true;
                    return 0;
                }
            },
            {
                "--port", "<number>",
                "Port for the multiplayer server to listen on (default: 8080)",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.server_port = static_cast<uint16_t>( std::stoi( params[0] ) );
                    return 1;
                }
            },
            {
                "--password", "<string>",
                "Password required for players to join the multiplayer server",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.server_password = params[0];
                    return 1;
                }
            },
            {
                "--client", "<host:port>",
                "Connect to a multiplayer server as a client (e.g. --client localhost:8080)",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.client_mode = true;
                    const std::string arg = params[0];
                    const auto colon = arg.rfind( ':' );
                    if( colon != std::string::npos ) {
                        result.client_host = arg.substr( 0, colon );
                        result.client_port = static_cast<uint16_t>( std::stoi( arg.substr( colon + 1 ) ) );
                    } else {
                        result.client_host = arg;
                    }
                    return 1;
                }
            },
            {
                "--client-name", "<name>",
                "Player name to use when connecting as a client",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.client_name = params[0];
                    return 1;
                }
            },
            {
                "--char", "<name>",
                "Character name to load (use with --world to skip the character selection menu)",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.char_name = params[0];
                    return 1;
                }
            }
        }
    };

    // The following arguments are dependent on one or more of the previous flags and are run
    // in a second pass.
    const std::vector<arg_handler> second_pass_arguments = {{
            {
                "--worldmenu", {},
                "Enables the world menu in the map-sharing code",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setWorldmenu( true );
                    return true;
                }
            },
            {
                "--datadir", "<directory name>",
                "Sub directory from which game data is loaded",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_datadir( params[0] );
                    return 1;
                }
            },
            {
                "--savedir", "<directory name>",
                "Subdirectory for game saves",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_savedir( params[0] );
                    return 1;
                }
            },
            {
                "--configdir", "<directory name>",
                "Subdirectory for game configuration",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_config_dir( params[0] );
                    return 1;
                }
            },
            {
                "--memorialdir", "<directory name>",
                "Subdirectory for memorials",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_memorialdir( params[0] );
                    return 1;
                }
            },
            {
                "--optionfile", "<filename>",
                "Name of the options file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_options( params[0] );
                    return 1;
                }
            },
            {
                "--keymapfile", "<filename>",
                "Name of the keymap file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_keymap( params[0] );
                    return 1;
                }
            },
            {
                "--autopickupfile", "<filename>",
                "Name of the autopickup options file within the configdir",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_autopickup( params[0] );
                    return 1;
                }
            },
            {
                "--motdfile", "<filename>",
                "Name of the message of the day file within the motd directory",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_motd( params[0] );
                    return 1;
                }
            },
        }
    };

    if( std::count( argv, argv + argc, std::string( "--help" ) ) ) {
        printHelpMessage( first_pass_arguments, second_pass_arguments );
        std::exit( 0 );
    }

    if( std::count( argv, argv + argc, std::string( "--version" ) ) ) {
        printVersionMessage();
        std::exit( 0 );
    }

    // skip program name
    --argc;
    ++argv;

    process_args( argv, argc, first_pass_arguments );
    process_args( argv, argc, second_pass_arguments );

    return result;
}

bool assure_essential_dirs_exist()
{
    using namespace PATH_INFO;
    std::vector<std::string> essential_paths{
        config_dir(),
        savedir(),
        templatedir(),
        user_font(),
        user_sound().get_unrelative_path().u8string(),
        user_gfx().get_unrelative_path().u8string()
    };
    for( const std::string &path : essential_paths ) {
        if( !assure_dir_exist( path ) ) {
            popup( _( "Unable to make directory \"%s\".  Check permissions." ), path );
            return false;
        }
    }
    return true;
}

}  // namespace

#if defined(EMSCRIPTEN)
EM_ASYNC_JS( void, mount_idbfs, (), {
    console.log( "Mounting IDBFS for persistence..." );
    FS.mkdir( '/home/web_user/.cataclysm-dda' );
    FS.mount( IDBFS, {}, '/home/web_user/.cataclysm-dda' );
    await new Promise( function( resolve, reject )
    {
        FS.syncfs( true, function( err ) {
            if( err ) {
                reject( err );
            } else {
                console.log( "Successfully mounted IDBFS." );
                resolve();
            }
        } );
    } );

    let fsNeedsSync = false;
    window.setFsNeedsSync = function setFsNeedsSync()
    {
        if( !fsNeedsSync ) {
            requestAnimationFrame( syncFs );
        }
        fsNeedsSync = true;
    };

    function syncFs()
    {
        console.log( "Persisting to IDBFS..." );
        FS.syncfs( false, function( err ) {
            fsNeedsSync = false;
            if( err ) {
                console.error( err );
            } else {
                console.log( "Successfully persisted to IDBFS..." );
            }
        } );
    }
} );
#endif

#if defined(USE_WINMAIN)
int APIENTRY WinMain( _In_ HINSTANCE /* hInstance */, _In_opt_ HINSTANCE /* hPrevInstance */,
                      _In_ LPSTR /* lpCmdLine */, _In_ int /* nCmdShow */ )
{
    int argc = __argc;
    char **argv = __argv;
#elif defined(__ANDROID__)
extern "C" int SDL_main( int argc, char **argv ) {
#else
int main( int argc, const char *argv[] )
{
#endif

    cata::init_allocator();

    ordered_static_globals();
    init_crash_handlers();
    reset_floating_point_mode();
#if defined(FLATBUFFERS_LOCALE_INDEPENDENT) && (FLATBUFFERS_LOCALE_INDEPENDENT > 0)
    flatbuffers::ClassicLocale::Get();
#endif

#if defined(EMSCRIPTEN)
    mount_idbfs();
#endif

    on_out_of_scope json_member_reporting_guard{ [] {
            // Disable reporting unvisited members if stack unwinding leaves main early.
            Json::globally_report_unvisited_members( false );
        } };

#if defined(_WIN32) and defined(TILES)
    const HANDLE std_output { GetStdHandle( STD_OUTPUT_HANDLE ) }, std_error { GetStdHandle( STD_ERROR_HANDLE ) };
    if( std_output != INVALID_HANDLE_VALUE and std_error != INVALID_HANDLE_VALUE ) {
        if( AttachConsole( ATTACH_PARENT_PROCESS ) ) {
            if( std_output == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stdout );
            }
            if( std_error == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stderr );
            }
        }
    }
#endif
#if defined(__ANDROID__)
    // Start the standard output logging redirector
    start_logger( "cdda" );

    // On Android first launch, we copy all data files from the APK into the app's writeable folder so std::io stuff works.
    // Use the external storage so it's publicly modifiable data (so users can mess with installed data, save games etc.)
    std::string external_storage_path( SDL_AndroidGetExternalStoragePath() );

    PATH_INFO::init_base_path( external_storage_path );
#else
    // Set default file paths
#if defined(PREFIX)
    PATH_INFO::init_base_path( std::string( PREFIX ) );
#else
    PATH_INFO::init_base_path( "" );
#endif
#endif

#if defined(__ANDROID__)
    PATH_INFO::init_user_dir( external_storage_path );
#else
#   if defined(USE_HOME_DIR) || defined(USE_XDG_DIR) || defined(EMSCRIPTEN)
    PATH_INFO::init_user_dir( "" );
#   else
    PATH_INFO::init_user_dir( "." );
#   endif
#endif
    PATH_INFO::set_standard_filenames();

    MAP_SHARING::setDefaults();

    cli_opts cli = parse_commandline( argc, const_cast<const char **>( argv ) );

    // Server mode: flag early so catacurses::init_interface() and display
    // calls throughout the rest of init are suppressed via test_mode guards.
    if( cli.server_mode ) {
        test_mode = true;
        cata_mp::set_server_mode( true );
        // Tiles build loads options and colors inside catacurses::init_interface(),
        // which we skip in test_mode.  Do only the display-independent parts here:
        // options must be loaded before set_language_from_options(), and init_colors()
        // must be called before load_core_data() parses color names from JSON.
#if defined(TILES)
        get_options().init();
        get_options().load();
#endif
        init_colors();
    }

    if( !dir_exist( PATH_INFO::datadir() ) ) {
        printf( "Fatal: Can't find data directory \"%s\"\nPlease ensure the current working directory is correct or specify data directory with --datadir.  Perhaps you meant to start \"cataclysm-launcher\"?\n",
                PATH_INFO::datadir().c_str() );
        exit( 1 );
    }

    if( !assure_dir_exist( PATH_INFO::user_dir() ) ) {
        printf( "Can't open or create %s. Check permissions.\n",
                PATH_INFO::user_dir().c_str() );
        exit( 1 );
    }

#if defined(EMSCRIPTEN)
    setupDebug( DebugOutput::std_err );
#else
    setupDebug( DebugOutput::file );
#endif
    // NOLINTNEXTLINE(cata-tests-must-restore-global-state)
    json_error_output_colors = json_error_output_colors_t::color_tags;

    /**
     * OS X does not populate locale env vars correctly (they usually default to
     * "C") so don't bother trying to set the locale based on them.
     */
#if !defined(MACOSX)
    if( setlocale( LC_ALL, "" ) == nullptr ) {
        DebugLog( D_WARNING, D_MAIN ) << "Error while setlocale(LC_ALL, '').";
    } else {
#endif
        try {
            std::locale::global( std::locale( "" ) );
        } catch( const std::exception & ) {
            // if user default locale retrieval isn't implemented by system
            try {
                // default to basic C locale
                std::locale::global( std::locale::classic() );
            } catch( const std::exception &err ) {
                debugmsg( "%s", err.what() );
                exit_handler( -999 );
            }
        }
#if !defined(MACOSX)
    }
#endif

    DebugLog( D_INFO, DC_ALL ) << "[main] C locale set to " << setlocale( LC_ALL, nullptr );
    DebugLog( D_INFO, DC_ALL ) << "[main] C++ locale set to " << std::locale().name();

#if defined(TILES) || defined(SDL_SOUND)
    {
        const SDLVersionInfo compiled = GetCompiledSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during compile is "
                                   << compiled.major << "."
                                   << compiled.minor << "."
                                   << compiled.patch;

        const SDLVersionInfo linked = GetLinkedSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during linking and in runtime is "
                                   << linked.major << "."
                                   << linked.minor << "."
                                   << linked.patch;
    }
#endif

#if !defined(TILES)
    get_options().init();
    get_options().load();
#endif

    // in test mode don't initialize curses to avoid escape sequences being inserted into output stream
    if( !test_mode ) {
        try {
            // set minimum FULL_SCREEN sizes
            FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
            FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
            catacurses::init_interface();
        } catch( const std::exception &err ) {
            // can't use any curses function as it has not been initialized
            std::cerr << "Error while initializing the interface: " << err.what() << std::endl;
            DebugLog( D_ERROR, DC_ALL ) << "Error while initializing the interface: " << err.what() << "\n";
            return 1;
        }
    } else if( cli.check_mods ) {
        get_options().init();
        get_options().load();
    }

    set_language_from_options();

    rng_set_engine_seed( cli.seed );

    if( !cata_mp::is_server_mode() ) {
        game_ui::init_ui();
    }

    g = std::make_unique<game>();

    // First load and initialize everything that does not
    // depend on the mods.
    try {
        g->load_static_data();
        if( cli.verifyexit ) {
            exit_handler( 0 );
        }
        if( cli.check_mods ) {
            init_colors();
            const std::vector<mod_id> mods( cli.opts.begin(), cli.opts.end() );
            exit( g->check_mod_data( mods ) && !debug_has_error_been_observed() ? 0 : 1 );
        }
    } catch( const std::exception &err ) {
        debugmsg( "%s", err.what() );
        exit_handler( -999 );
    }

    if( !cata_mp::is_server_mode() ) {
        // Load the colors of ImGui to match the colors set by the user.
        cataimgui::init_colors();

        // set decimal point for float input widgets
        // uses system locale, because that's what imgui uses to parse and display floats
        ImGui::GetPlatformIO().Platform_LocaleDecimalPoint =
            static_cast<unsigned char>( *localeconv()->decimal_point );
    }

    // Override existing settings from cli  options
    if( cli.disable_ascii_art ) {
        get_options().get_option( "ENABLE_ASCII_ART" ).setValue( "false" );
        get_options().get_option( "ENABLE_ASCII_TITLE" ).setValue( "false" );
    }

    if( cli.noverify ) {
        get_options().get_option( "SKIP_VERIFICATION" ).setValue( "true" );
    }

    // Now we do the actual game.

#if defined(DEBUG_CURSES_CURSOR)
    catacurses::curs_set( 2 );
#else
    // I have no clue what this comment is on about
    // Any value works well enough for debugging at least
    catacurses::curs_set( 0 ); // Invisible cursor here, because MAPBUFFER.load() is crash-prone
#endif

#if !defined(_WIN32)
    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = exit_handler;
    sigemptyset( &sigIntHandler.sa_mask );
    sigIntHandler.sa_flags = 0;
    sigaction( SIGINT, &sigIntHandler, nullptr );
#endif

    if( !assure_essential_dirs_exist() ) {
        exit_handler( -999 );
        return 0;
    }

#if defined(LOCALIZE)
    // imclient is null in server mode (SDL/ImGui not initialized), skip this.
    if( !cata_mp::is_server_mode() &&
        get_option<std::string>( "USE_LANG" ).empty() && !SystemLocale::Language().has_value() ) {
        imclient->new_frame(); // we have to prime the pump, because of reasons
        imclient->end_frame();
        const std::string lang = select_language();
        get_options().get_option( "USE_LANG" ).setValue( lang );
        set_language_from_options();
    }
#endif
    replay_buffered_debugmsg_prompts();

    // Server mode: load world headlessly, start TCP server on background thread,
    // then run the game loop on this thread.  Avatar moves are zeroed each turn
    // in do_turn() so we never block on handle_action().
    if( cli.server_mode ) {
        const std::string worldname = cli.world;
        if( worldname.empty() ) {
            fprintf( stderr, "[cdda-mp] --server requires --world <worldname>\n" );
            fprintf( stderr, "[cdda-mp] Usage: ./cataclysm-tiles --server --world <name> --port 8081\n" );
            return 1;
        }

        // Scan available worlds so we can print a useful error if the name is wrong.
        world_generator->init();
        const WORLD *wptr = world_generator->get_world( worldname );
        if( !wptr ) {
            fprintf( stderr, "[cdda-mp] World '%s' not found. Available worlds:\n",
                     worldname.c_str() );
            for( const std::string &wn : world_generator->all_worldnames() ) {
                const WORLD *w = world_generator->get_world( wn );
                fprintf( stderr, "  %s  (%zu save(s))\n", wn.c_str(),
                         w ? w->world_saves.size() : 0 );
            }
            fprintf( stderr, "[cdda-mp] Create a character in the game first, then run --server.\n" );
            return 1;
        }
        if( wptr->world_saves.empty() ) {
            fprintf( stderr,
                     "[cdda-mp] World '%s' has no character saves. "
                     "Start the game, create a character, and save before running --server.\n",
                     worldname.c_str() );
            return 1;
        }

        if( !g->load( worldname ) ) {
            fprintf( stderr, "[cdda-mp] Failed to load world '%s' — check debug.log for details.\n",
                     worldname.c_str() );
            return 1;
        }

        const uint16_t port = cli.server_port;
        const std::string password = cli.server_password;
        const std::string ver = getVersionString();
        std::thread server_thread( [port, password, ver]() {
            cata_mp::run_server( port, password, ver );
        } );
        server_thread.detach();
        printf( "[cdda-mp] Headless server running on port %d (world: %s)\n",
                port, worldname.c_str() );

        get_event_bus().send<event_type::game_begin>( getVersionString() );

        // Pace the server to real-time (1 game turn = 1 real second).
        // Between game ticks, poll MP events at 10 Hz so player input is
        // processed in ~100 ms rather than waiting up to a full second.
        using clock = std::chrono::steady_clock;
        constexpr auto TICK_INTERVAL = std::chrono::seconds( 1 );
        constexpr auto POLL_INTERVAL = std::chrono::milliseconds( 100 );
        while( !g->do_turn() ) {
            auto next_tick = clock::now() + TICK_INTERVAL;
            while( clock::now() < next_tick ) {
                std::this_thread::sleep_for( POLL_INTERVAL );
                cata_mp::process_mp_events();
            }
        }

        exit_handler( -999 );
        return 0;
    }

    // Client mode: connect to server before entering the game loop.
    // The game still loads and displays normally; player actions are forwarded
    // to the server instead of executing locally.
    if( cli.client_mode ) {
        if( cli.client_host.empty() ) {
            fprintf( stderr, "[cdda-mp] --client requires a host, e.g. --client localhost:8080\n" );
            return 1;
        }
        cata_mp::set_client_mode( true );
        // Name: --client-name > --char > "player2"
        const std::string name = !cli.client_name.empty() ? cli.client_name
                                 : !cli.char_name.empty()  ? cli.char_name
                                 : "player2";
        if( !cata_mp::client_connect( cli.client_host, cli.client_port,
                                      name, cli.client_password ) ) {
            fprintf( stderr, "[cdda-mp] Failed to connect to %s:%d\n",
                     cli.client_host.c_str(), cli.client_port );
            return 1;
        }
        printf( "[cdda-mp] Client mode active — connected to %s:%d\n",
                cli.client_host.c_str(), cli.client_port );
    }

    main_menu::queued_world_to_load = std::move( cli.world );
    if( !cli.char_name.empty() ) {
        main_menu::queued_save_id_to_load = cli.char_name;
    }

    // Host mode: start listen server in background thread before entering game loop
    std::thread host_thread;
    if( cli.host_mode ) {
        cata_mp::set_host_mode( true );
        const uint16_t port = cli.server_port;
        const std::string password = cli.server_password;
        const std::string ver2 = getVersionString();
        host_thread = std::thread( [port, password, ver2]() {
            cata_mp::run_server( port, password, ver2 );
        } );
        host_thread.detach();
        printf( "[cdda-mp] Hosting on port %d — waiting for player 2...\n", port );
    }

    // Refresh window title now that host/client mode flags are set.
    // set_language_from_options() was called during SDL init, before the flags existed.
    if( !test_mode ) {
        set_language_from_options();
    }

    // MP build tag — uses the binary's on-disk mtime (not __DATE__/__TIME__,
    // which only reflect main.cpp's last compile and stay stale when other
    // translation units rebuild).  Helpful when iterating on MP features.
    if( !test_mode ) {
        std::string role;
        if( cli.host_mode ) {
            role = "HOST";
        } else if( cli.client_mode ) {
            role = "CLIENT";
        } else {
            role = "SP";
        }
        std::string stamp = "?";
        struct stat st {};
        if( argc > 0 && argv[0] && stat( argv[0], &st ) == 0 ) {
            char buf[32];
            const time_t mtime = st.st_mtime;
            const std::tm *tm_local = std::localtime( &mtime );
            if( tm_local && std::strftime( buf, sizeof( buf ), "%b %d %H:%M:%S", tm_local ) > 0 ) {
                stamp = buf;
            }
        }
        // Cache the stamp so later title-set calls (mp_update_window_title
        // on mode-change) can re-include it instead of overwriting back to
        // a stamp-less title.
        cata_mp::g_mp_build_stamp = stamp;
        set_title( string_format( "CDDA co-op — %s — %s — build %s",
                                  role, getVersionString(), stamp ) );
    }

    while( true ) {
        main_menu menu;
        if( !menu.opening_screen() ) {
            break;
        }

        // Client: block until the server sends our initial position so the
        // first rendered frame already shows the correct location, not the
        // scenario start tile the character was saved at.
        if( cata_mp::is_client_mode() ) {
            cata_mp::client_wait_for_initial_position();
        }

        shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();
        get_event_bus().send<event_type::game_begin>( getVersionString() );
        // DIAG (temp #2/#3 timing): wall-time of each client do_turn (redraw is
        // inside do_turn). Pair with since_grant + host SRV-WAIT to split a turn
        // into network vs client-render vs other. Only log slow turns to limit spam.
        while( true ) {
            const auto dt0 = std::chrono::steady_clock::now();
            const bool done = g->do_turn();
            if( cata_mp::is_client_mode() ) {
                const int dt_ms = static_cast<int>(
                                      std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - dt0 ).count() );
                if( dt_ms > 30 ) {
                    cata_mp::mp_log( "[cdda-mp] CLI-DOTURN: " + std::to_string( dt_ms ) + "ms" );
                }
            }
            if( done ) {
                break;
            }
        }
        // World unloaded (quit-to-menu). Reset per-world MP state so re-entering
        // a world re-runs the stale-proxy sweep instead of skipping it.
        cata_mp::mp_on_world_exit();
    }

    exit_handler( -999 );
    return 0;
}
