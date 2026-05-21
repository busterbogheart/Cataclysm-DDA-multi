#include "do_turn.h"
#include "mp_queue.h"
#include "mp_gamestate.h"
#include "mp_client_conn.h"

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <thread>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ratio>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "action.h"
#include "activity_type.h"
#include "avatar.h"
#include "bionics.h"
#include "cached_options.h"
#include "calendar.h"
#ifdef TILES
#include "cata_imgui.h"
#endif
#include "cata_variant.h"
#include "clzones.h"
#include "coordinates.h"
#include "debug.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "explosion.h"
#include "field.h"
#include "game.h"
#include "game_constants.h"
#include "gamemode.h"
#include "help.h"
#include "input.h"
#include "input_context.h"
#include "item_wakeup.h"
#include "magic_enchantment.h"
#include "map.h"
#include "map_iterator.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "memorial_logger.h"
#include "messages.h"
#include "mission.h"
#include "monster.h"
#include "mtype.h"
#include "music.h"
#include "sdlsound.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap_ui.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "player_activity.h"
#include "point.h"
#include "popup.h"
#include "rng.h"
#include "scent_map.h"
#include "sdlsound.h"
#include "simple_pathfinding.h"
#include "sounds.h"
#include "stats_tracker.h"
#include "string_formatter.h"
#include "timed_event.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"
#include "worldfactory.h"

static const activity_id ACT_AUTODRIVE( "ACT_AUTODRIVE" );
static const activity_id ACT_FIRSTAID( "ACT_FIRSTAID" );
static const activity_id ACT_MIGRATION_CANCEL( "ACT_MIGRATION_CANCEL" );
static const activity_id ACT_OPERATION( "ACT_OPERATION" );

static const bionic_id bio_alarm( "bio_alarm" );

static const efftype_id effect_controlled( "controlled" );
static const efftype_id effect_npc_suspend( "npc_suspend" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_sleep( "sleep" );

static const event_statistic_id event_statistic_last_words( "last_words" );

static const json_character_flag json_flag_NO_SCENT( "NO_SCENT" );

static const trait_id trait_HAS_NEMESIS( "HAS_NEMESIS" );

#if defined(__ANDROID__)
extern std::map<std::string, std::list<input_event>> quick_shortcuts_map;
extern bool add_best_key_for_action_to_quick_shortcuts( action_id action,
        const std::string &category, bool back );
#endif

#define dbg(x) DebugLog((x),D_GAME) << __FILE__ << ":" << __LINE__ << ": "

namespace turn_handler
{
bool cleanup_at_end()
{
    avatar &u = get_avatar();
    if( g->uquit == QUIT_DIED || g->uquit == QUIT_SUICIDE ) {
        // Put (non-hallucinations) into the overmap so they are not lost.
        for( monster &critter : g->all_monsters() ) {
            g->despawn_monster( critter );
        }
        // if player has "hunted" trait, remove their nemesis monster on death
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.remove_nemesis();
        }
        // Reset NPC factions and disposition
        g->reset_npc_dispositions();
        // Save the factions', missions and set the NPC's overmap coordinates
        // Npcs are saved in the overmap.
        g->save_factions_missions_npcs(); //missions need to be saved as they are global for all saves.

        // and the overmap, and the local map.
        g->save_maps(); //Omap also contains the npcs who need to be saved.

        //save achievements entry
        g->save_achievements();

        // Notify connected clients before the death screen takes focus so they
        // see "partner died" instead of a raw socket-drop spam.
        if( cata_mp::is_hosting() ) {
            cata_mp::notify_client_host_died();
        }
        g->death_screen();
        std::chrono::seconds time_since_load =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g->time_of_last_load );
        std::chrono::seconds total_time_played = g->time_played_at_last_load + time_since_load;
        get_event_bus().send<event_type::game_over>( total_time_played );
        // Struck the save_player_data here to forestall Weirdness
        g->move_save_to_graveyard();
        g->write_memorial_file( g->stats().value_of( event_statistic_last_words )
                                .get<cata_variant_type::string>() );
        get_memorial().clear();
        std::vector<std::string> characters = g->list_active_saves();
        // remove current player from the active characters list, as they are dead
        std::vector<std::string>::iterator curchar = std::find( characters.begin(),
                characters.end(), u.get_save_id() );
        if( curchar != characters.end() ) {
            characters.erase( curchar );
        }

        if( characters.empty() ) {
            bool queryDelete = false;
            bool queryReset = false;

            if( get_option<std::string>( "WORLD_END" ) == "query" ) {
                bool decided = false;
                std::string buffer = _( "Warning: NPC interactions and some other global flags "
                                        "will not all reset when starting a new character in an "
                                        "already-played world.  This can lead to some strange "
                                        "behavior.\n\n"
                                        "Are you sure you wish to keep this world?"
                                      );

                while( !decided ) {
                    uilist smenu;
                    smenu.allow_cancel = false;
                    smenu.addentry( 0, true, 'r', "%s", _( "Reset world" ) );
                    smenu.addentry( 1, true, 'd', "%s", _( "Delete world" ) );
                    smenu.addentry( 2, true, 'k', "%s", _( "Keep world" ) );
                    smenu.query();

                    switch( smenu.ret ) {
                        case 0:
                            queryReset = true;
                            decided = true;
                            break;
                        case 1:
                            queryDelete = true;
                            decided = true;
                            break;
                        case 2:
                            decided = query_yn( buffer );
                            break;
                    }
                }
            }

            if( queryDelete || get_option<std::string>( "WORLD_END" ) == "delete" ) {
                world_generator->delete_world( world_generator->active_world->world_name, true );

            } else if( queryReset || get_option<std::string>( "WORLD_END" ) == "reset" ) {
                world_generator->delete_world( world_generator->active_world->world_name, false );
            }
        } else if( get_option<std::string>( "WORLD_END" ) != "keep" ) {
            std::string tmpmessage;
            for( auto &character : characters ) {
                tmpmessage += "\n  ";
                tmpmessage += character;
            }
            popup( _( "World retained.  Characters remaining:%s" ), tmpmessage );
        }
        if( g->gamemode ) {
            g->gamemode = std::make_unique<special_game>(); // null gamemode or something..
        }
    }

    //Reset any offset due to driving
    g->set_driving_view_offset( point_rel_ms::zero );

    //clear all sound channels
    sfx::fade_audio_channel( sfx::channel::any, 300 );
    sfx::fade_audio_group( sfx::group::weather, 300 );
    sfx::fade_audio_group( sfx::group::time_of_day, 300 );
    sfx::fade_audio_group( sfx::group::context_themes, 300 );
    sfx::fade_audio_group( sfx::group::low_stamina, 300 );

    zone_manager::get_manager().clear();

    MAPBUFFER.clear();
    overmap_buffer.clear();

#if defined(__ANDROID__)
    quick_shortcuts_map.clear();
#endif
    return true;
}

} // namespace turn_handler

void handle_key_blocking_activity()
{
    if( test_mode ) {
        return;
    }
    avatar &u = get_avatar();
    const bool has_unfinished_activity = u.activity && (
            u.activity.id()->based_on() == based_on_type::NEITHER
            || u.activity.moves_left > 0 );
    // MP-locked host has no activity but should still be able to zoom, check
    // inventory, see messages, etc. while waiting for the client to act.
    if( has_unfinished_activity || u.has_destination()
        || cata_mp::is_host_waiting_for_client() ) {
        input_context ctxt = get_default_mode_input_context();
        const std::string action = ctxt.handle_input( 0 );
        if( cata_mp::is_hosting() && cata_mp::is_host_waiting_for_client() &&
            !action.empty() && action != "ANY_INPUT" && action != "TIMEOUT" ) {
            cata_mp::mp_log( "[cdda-mp] HOST-LOCKED-INPUT: action=\"" + action + "\"" );
        }
        bool refresh = true;
        if( action == "pause" ) {
            if( u.activity.is_interruptible_with_kb() ) {
                g->cancel_activity_query( _( "Confirm:" ) );
            }
        } else if( action == "zoom_in" ) {
            g->zoom_in();
            g->mark_main_ui_adaptor_resize();
        } else if( action == "zoom_out" ) {
            g->zoom_out();
            g->mark_main_ui_adaptor_resize();
        } else if( action == "player_data" ) {
            u.disp_info( true );
        } else if( action == "messages" ) {
            Messages::display_messages();
        } else if( action == "help" ) {
            get_help().display_help();
        } else if( action != "HELP_KEYBINDINGS" ) {
            refresh = false;
        }
        if( refresh ) {
            ui_manager::redraw();
            refresh_display();
        }
    } else {
        refresh_display();
        inp_mngr.pump_events();
    }
}

namespace
{
void monmove()
{
    g->cleanup_dead();
    map &m = get_map();
    avatar &u = get_avatar();

    int mon_count = 0;
    std::string mon_slow_log;
    for( monster &critter : g->all_monsters() ) {
        if( !m.inbounds( critter.pos_abs() ) ) {
            continue;
        }
        ++mon_count;
        const auto mon_t0 = std::chrono::steady_clock::now();
        const tripoint_bub_ms critter_pos = critter.pos_bub( m );

        // Critters in impassable tiles get pushed away, unless it's not impassable for them
        if( !critter.is_dead() && ( m.impassable( critter_pos ) &&
                                    !m.get_impassable_field_at( critter_pos ).has_value() ) &&
            !critter.can_move_to( critter_pos ) ) {
            dbg( D_ERROR ) << "game:monmove: " << critter.name()
                           << " can't move to its location!  (" << critter_pos.x()
                           << ":" << critter_pos.y() << ":" << critter_pos.z() << "), "
                           << m.tername( critter_pos );
            add_msg_debug( debugmode::DF_MONSTER, "%s can't move to its location!  (%d,%d,%d), %s",
                           critter.name(),
                           critter_pos.x(), critter_pos.y(), critter_pos.z(), m.tername( critter_pos ) );
            bool okay = false;
            for( const tripoint_bub_ms &dest : m.points_in_radius( critter_pos, 3 ) ) {
                if( critter.can_move_to( dest ) && g->is_empty( dest ) ) {
                    critter.setpos( m, dest );
                    okay = true;
                    break;
                }
            }
            if( !okay ) {
                // die of "natural" cause (overpopulation is natural)
                critter.die( &m, nullptr );
            }
        }

        if( !critter.is_dead() ) {
            critter.process_turn();
        }

        m.creature_in_field( critter );
        if( calendar::once_every( 1_days ) ) {
            if( critter.has_flag( mon_flag_MILKABLE ) ) {
                critter.refill_udders();
            }
            critter.try_biosignature();
            critter.try_reproduce();
        }
        while( critter.get_moves() > 0 && !critter.is_dead() && !critter.has_effect( effect_ridden ) ) {
            critter.made_footstep = false;
            // Controlled critters don't make their own plans
            if( !critter.has_effect( effect_controlled ) ) {
                // Formulate a path to follow
                critter.plan();
            } else {
                critter.set_moves( 0 );
                break;
            }
            critter.move(); // Move one square, possibly hit u
            critter.process_triggers();
            m.creature_in_field( critter );
        }

        if( !critter.is_dead() && !critter.is_hallucination() &&
            rl_dist( u.pos_abs(), critter.pos_abs() ) < u.enchantment_cache->modify_value(
                enchant_vals::mod::MOTION_ALARM, 0 ) ) {
            if( u.has_active_bionic( bio_alarm ) ) {
                u.mod_power_level( -bio_alarm->power_trigger );
                add_msg( m_warning, _( "Your motion alarm goes off!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your motion alarm goes off!" ) );
            } else {
                add_msg( m_warning, _( "You suddenly feel alerted!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your instincts warn you for danger!" ) );
            }
            if( u.has_effect( effect_sleep ) ) {
                u.wake_up();
            }
        }

        if( cata_mp::is_hosting() ) {
            const int mon_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - mon_t0 ).count() );
            if( mon_ms >= 10 ) {
                mon_slow_log += " " + critter.type->id.str() + "=" + std::to_string( mon_ms ) + "ms";
            }
        }
    }

    if( cata_mp::is_hosting() && !mon_slow_log.empty() ) {
        cata_mp::mp_log( "[cdda-mp] monmove slow monsters (count=" +
                         std::to_string( mon_count ) + "):" + mon_slow_log );
    }

    g->cleanup_dead();

    // The remaining monsters are all alive, but may be outside of the reality bubble.
    // If so, despawn them. This is not the same as dying, they will be stored for later and the
    // monster::die function is not called.
    g->despawn_nonlocal_monsters();

    // Now, do active NPCs.
    for( npc &guy : g->all_npcs() ) {
        // Remote player NPCs are driven by network input, not AI.
        if( cata_mp::is_remote_player( guy.getID() ) ) {
            continue;
        }
        const auto npc_t0 = std::chrono::steady_clock::now();
        int turns = 0;
        int real_count = 0;
        const int count_limit = std::max( 10, guy.get_moves() / 64 );
        if( guy.is_mounted() ) {
            guy.check_mount_is_spooked();
        }
        m.creature_in_field( guy );
        if( !guy.has_effect( effect_npc_suspend ) ) {
            guy.process_turn();
        }
        while( !guy.is_dead() && ( !guy.in_sleep_state() ||
                                   guy.activity.id() == ACT_OPERATION || guy.activity.id() == ACT_MIGRATION_CANCEL ) &&
               guy.get_moves() > 0 && turns < 10 ) {
            const int moves = guy.get_moves();
            const bool has_destination = guy.has_destination_activity();
            guy.move();
            if( moves == guy.get_moves() ) {
                // Count every time we exit npc::move() without spending any moves.
                real_count++;
                if( has_destination == guy.has_destination_activity() || real_count > count_limit ) {
                    turns++;
                }
            }
            // Turn on debug mode when in infinite loop
            // It has to be done before the last turn, otherwise
            // there will be no meaningful debug output.
            if( turns == 9 ) {
                debugmsg( "NPC '%s' entered infinite loop, npc activity id: '%s'",
                          guy.get_name(), guy.activity.id().str() );
            }
        }

        // If we spun too long trying to decide what to do (without spending moves),
        // Invoke cognitive suspension to prevent an infinite loop.
        if( turns == 10 ) {
            add_msg( _( "%s faints!" ), guy.get_name() );
            guy.reboot();
        }

        if( !guy.is_dead() ) {
            guy.npc_update_body();
        }

        if( cata_mp::is_hosting() ) {
            const int npc_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - npc_t0 ).count() );
            if( npc_ms >= 10 ) {
                cata_mp::mp_log( "[cdda-mp] monmove slow NPC: " + guy.get_name() +
                                 " activity=" + guy.activity.id().str() +
                                 " " + std::to_string( npc_ms ) + "ms" );
            }
        }
    }
    g->cleanup_dead();
}

void overmap_npc_move()
{
    avatar &u = get_avatar();
    std::vector<npc *> travelling_npcs;
    static constexpr int move_search_radius = 600;
    for( auto &elem : overmap_buffer.get_npcs_near_player( move_search_radius ) ) {
        if( !elem ) {
            continue;
        }
        npc *npc_to_add = elem.get();
        if( ( !npc_to_add->is_active() || rl_dist( u.pos_bub(), npc_to_add->pos_bub() ) > SEEX * 2 ) &&
            npc_to_add->mission == NPC_MISSION_TRAVELLING ) {
            travelling_npcs.push_back( npc_to_add );
        }
    }
    bool npcs_need_reload = false;
    for( npc *&elem : travelling_npcs ) {
        if( elem->has_omt_destination() ) {
            if( !elem->omt_path.empty() ) {
                if( rl_dist( elem->omt_path.back(), elem->pos_abs_omt() ) > 2 ) {
                    // recalculate path, we got distracted doing something else probably
                    elem->omt_path.clear();
                } else if( elem->omt_path.back() == elem->pos_abs_omt() ) {
                    elem->omt_path.pop_back();
                }
            }
            if( elem->omt_path.empty() ) {
                elem->omt_path = overmap_buffer.get_travel_path( elem->pos_abs_omt(), elem->goal,
                                 overmap_path_params::for_npc() ).points;
                if( elem->omt_path.empty() ) { // goal is unreachable, or already reached goal, reset it
                    elem->goal = npc::no_goal_point;
                }
            } else {
                elem->travel_overmap( elem->omt_path.back() );
                npcs_need_reload = true;
            }
        }
        if( !elem->has_omt_destination() && calendar::once_every( 1_hours ) && one_in( 3 ) ) {
            // travelling destination is reached/not set, try different one
            elem->set_omt_destination();
        }
    }
    if( npcs_need_reload ) {
        g->reload_npcs();
    }
}

} // namespace

// MAIN GAME LOOP
// Returns true if game is over (death, saved, quit, etc)
bool do_turn()
{
    if( g->is_game_over() ) {
        return turn_handler::cleanup_at_end();
    }

    weather_manager &weather = get_weather();
    // Actual stuff
    if( g->new_game ) {
        g->new_game = false;
        if( get_option<std::string>( "ETERNAL_WEATHER" ) != "normal" ) {
            weather.weather_override = static_cast<weather_type_id>
                                       ( get_option<std::string>( "ETERNAL_WEATHER" ) );
            weather.set_nextweather( calendar::turn );
        } else {
            weather.weather_override = WEATHER_NULL;
            weather.set_nextweather( calendar::turn );
        }
    } else {
        g->gamemode->per_turn();
        // Client: only advance the calendar when moves were just granted (i.e. an
        // actual game turn is happening).  Without this guard the clock races
        // forward at ~10 turns/sec of real time while the client is locked, creating
        // the divergence shown in the debug HUD.
        if( !cata_mp::is_client_mode() || get_avatar().get_moves() > 0 ) {
            calendar::turn += 1_turns;
        }
    }
    //used for dimension swapping
    if( g->swapping_dimensions ) {
        g->swapping_dimensions = false;
    }
    play_music( music::get_music_id_string() );

    // starting a new turn, clear out temperature cache
    weather.temperature_cache.clear();

    if( g->npcs_dirty ) {
        g->load_npcs();
    }

    if( cata_mp::is_hosting() ) {
        cata_mp::mp_log( "[cdda-mp] HOST-DO-TURN-ENTRY: avatar_moves=" +
                         std::to_string( get_avatar().get_moves() ) +
                         " av_act=" + ( get_avatar().activity ? get_avatar().activity.id().str() : "none" ) );
    }
    // Process multiplayer events from network thread
    cata_mp::process_mp_events();
    // Apply any server state updates received since the last turn (client mode)
    cata_mp::client_process_incoming();
    if( cata_mp::is_client_mode() ) {
        avatar &u_dbg = get_avatar();
        cata_mp::mp_log( "[cdda-mp] post-incoming moves=" + std::to_string( u_dbg.get_moves() ) +
                         " ack=" + std::to_string( cata_mp::is_client_waiting_for_ack() ) );
    }
    if( cata_mp::is_hosting() ) {
        cata_mp::mp_log( "[cdda-mp] HOST-POST-MP-EVENTS: avatar_moves=" +
                         std::to_string( get_avatar().get_moves() ) );
    }
    // Lockstep: grant the client their turn at the start of each game turn.
    if( cata_mp::is_hosting() ) {
        cata_mp::grant_client_turn();
    }
    // Keep the MP debug HUD alive whenever multiplayer is active
    if( cata_mp::is_client_mode() || cata_mp::is_hosting() ) {
        cata_mp::ensure_mp_hud();
    }

    timed_event_manager &timed_events = get_timed_events();
    timed_events.process();
    get_item_wakeups().process( calendar::turn );
    mission::process_all();
    avatar &u = get_avatar();
    map &m = get_map();
    // If controlling a vehicle that is owned by someone else
    if( u.in_vehicle && u.controlling_vehicle ) {
        vehicle *veh = veh_pointer_or_null( m.veh_at( u.pos_bub() ) );
        if( veh && !veh->handle_potential_theft( u, true ) ) {
            veh->handle_potential_theft( u, false, false );
        }
    }

    // If you're inside a wall or something and haven't been telefragged, let's get you out.
    // In client MP mode the server is authoritative for position; staggering against
    // the server's position causes a stumble loop and flicker, so skip it.
    if( !cata_mp::is_client_mode() &&
        ( m.impassable( u.pos_bub() ) && !m.impassable_field_at( u.pos_bub() ) ) &&
        !m.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, u.pos_bub() ) ) {
        u.stagger();
    }

    // If riding a horse - chance to spook
    if( u.is_mounted() ) {
        u.check_mount_is_spooked();
    }
    if( calendar::once_every( 1_days ) ) {
        overmap_buffer.process_mongroups();
    }

    // Move hordes every turn, move_hordes has its own rate limiting
    overmap_buffer.move_hordes();
    if( calendar::once_every( time_duration::from_minutes( 2.5 ) ) ) {
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.move_nemesis();
        }
    }

    g->debug_hour_timer.print_time();

    u.update_body();

    // Auto-save if autosave is enabled (suppressed in client mode — server owns saves)
    if( !cata_mp::is_client_mode() &&
        get_option<bool>( "AUTOSAVE" ) &&
        calendar::once_every( 1_turns * get_option<int>( "AUTOSAVE_TURNS" ) ) &&
        !u.is_dead_state() ) {
        g->autosave();
    }

    weather.update_weather();
    g->reset_light_level();
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
        m.set_lightmap_cache_dirty( z );
    }

    g->perhaps_add_random_npc( /* ignore_spawn_timers_and_rates = */ false );
    // Snapshot moves and activity ID before the loop.  The ID is needed because
    // ACT_WAIT_STAMINA (and other perpetual wait activities) consume moves and then
    // call finish() inside the same do_turn() call when their condition is met
    // (e.g. stamina fully recovered after server state is applied).  After finish()
    // the activity pointer is null, so client_dispatch_wait_for_activity() needs the
    // pre-loop ID as a fallback to know it should still send a "wait" to unblock the server.
    const int pre_activity_moves = u.get_moves();
    // Default-constructed activity_id is empty-but-not-null, so it evaluates
    // truthy in bool context.  Use NULL_ID so downstream `if( pre_activity_id )`
    // checks correctly distinguish "had an activity" from "no activity".
    const activity_id pre_activity_id = u.activity ? u.activity.id() : activity_id::NULL_ID();
    if( cata_mp::is_client_mode() ) {
        cata_mp::mp_log( "[cdda-mp] pre-act-loop: moves=" + std::to_string( pre_activity_moves ) +
                         " act=" + ( pre_activity_id ? pre_activity_id.str() : "none" ) );
        // Snapshot this turn's activity id for the wire so enrich uses it even
        // if av.activity is cleared mid-turn by the activity finishing.
        cata_mp::set_client_turn_activity( pre_activity_id ? pre_activity_id.str()
                                           : std::string() );
        // Client busy-loop fix: when we have an activity but no moves to tick
        // it, do_turn just spins doing nothing — the input loop is skipped
        // (moves <= 0) and the activity loop is skipped (moves <= 0).  The
        // main loop calls do_turn again immediately, eating CPU at ~100kHz and
        // starving the ASIO network thread so host grants never get processed
        // through client_process_incoming.  Sleep briefly to yield to the
        // network thread; when its broadcast arrives, our moves regen and the
        // next do_turn iteration ticks the activity normally.
        if( pre_activity_id && pre_activity_moves <= 0 ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
    }
    while( u.get_moves() > 0 && u.activity ) {
        u.activity.do_turn( u );
    }
    // Client: if a wait activity consumed the server-granted moves this turn,
    // dispatch "wait" so the server advances its timeline in sync.
    bool mp_wait_dispatched = false;
    if( cata_mp::is_client_mode() && pre_activity_moves > 0 && u.get_moves() <= 0 ) {
        cata_mp::mp_log( "[cdda-mp] pre-loop dispatch: pre_moves=" + std::to_string( pre_activity_moves ) +
                         " act=" + ( pre_activity_id ? pre_activity_id.str() : "none" ) );
        cata_mp::client_dispatch_wait_for_activity( pre_activity_id );
        mp_wait_dispatched = true;
    } else if( cata_mp::is_client_mode() ) {
        cata_mp::mp_log( "[cdda-mp] pre-loop dispatch SKIP: pre_moves=" + std::to_string( pre_activity_moves ) +
                         " cur_moves=" + std::to_string( u.get_moves() ) +
                         " act=" + ( pre_activity_id ? pre_activity_id.str() : "none" ) );
    }

    // Process NPC sound events before they move or they hear themselves talking
    for( npc &guy : g->all_npcs() ) {
        if( rl_dist( guy.pos_bub(), u.pos_bub() ) < MAX_VIEW_DISTANCE ) {
            sounds::process_sound_markers( &guy );
        }
    }

    music::deactivate_music_id( music::music_id::sound );

    // Process sound events into sound markers for display to the player.
    sounds::process_sound_markers( &u );

    if( u.is_deaf() ) {
        sfx::do_hearing_loss();
    }

    // In server mode the avatar is a simulation host, not a controllable player.
    // Zero its moves so we skip the input-blocking loop, and keep survival
    // needs at safe levels so it never dies and crashes the server.
    if( cata_mp::is_server_mode() ) {
        u.set_moves( 0 );
        u.set_hunger( 0 );
        u.set_thirst( 0 );
        u.set_sleep_deprivation( 0 );
        u.set_stamina( u.get_stamina_max() );
        u.healall( 100 );
    }

    if( cata_mp::is_hosting() ) {
        cata_mp::mp_log( "[cdda-mp] HOST-INPUT-GATE: avatar_moves=" + std::to_string( u.get_moves() ) +
                         " has_act=" + ( u.activity ? u.activity.id().str() : "none" ) +
                         " sleep=" + std::to_string( u.has_effect( effect_sleep ) ) +
                         " enter_loop=" + std::to_string( u.get_moves() > 0 || g->uquit == QUIT_WATCH ) );
    }
    if( !u.has_effect( effect_sleep ) || g->uquit == QUIT_WATCH ) {
        if( u.get_moves() > 0 || g->uquit == QUIT_WATCH ) {
            if( cata_mp::is_client_mode() ) {
                cata_mp::mp_log( "[cdda-mp] input-loop enter: moves=" +
                                 std::to_string( u.get_moves() ) +
                                 " ms_grant=" + std::to_string( cata_mp::ms_since_last_grant() ) +
                                 " ack=" + std::to_string( cata_mp::is_client_waiting_for_ack() ) );
            }
            if( cata_mp::is_hosting() ) {
                cata_mp::mp_log( "[cdda-mp] HOST-INPUT-LOOP enter: moves=" + std::to_string( u.get_moves() ) );
            }
            while( u.get_moves() > 0 || g->uquit == QUIT_WATCH ) {
                m.process_falling();
                g->cleanup_dead();
                g->mon_info_update();
                // Process any new sounds the player caused during their turn.
                for( npc &guy : g->all_npcs() ) {
                    if( rl_dist( guy.pos_bub(), u.pos_bub() ) < MAX_VIEW_DISTANCE ) {
                        sounds::process_sound_markers( &guy );
                    }
                }
                explosion_handler::process_explosions();
                sounds::process_sound_markers( &u );
                if( !u.activity && g->uquit != QUIT_WATCH
                    && ( !u.has_distant_destination() || calendar::once_every( 10_seconds ) ) ) {
                    g->wait_popup_reset();
                    ui_manager::redraw();
                }

                if( g->queue_screenshot ) {
                    g->take_screenshot();
                    g->queue_screenshot = false;
                }

                {
                    const size_t pre_msg = cata_mp::is_hosting() ? Messages::size() : 0;
                    if( cata_mp::is_hosting() ) {
                        cata_mp::mp_log( "[cdda-mp] HOST-HANDLE-ACTION: calling, pre_moves=" +
                                         std::to_string( u.get_moves() ) );
                    }
                    const bool acted = g->handle_action();
                    if( cata_mp::is_hosting() ) {
                        cata_mp::mp_log( "[cdda-mp] HOST-HANDLE-ACTION: returned acted=" +
                                         std::to_string( acted ) +
                                         " post_moves=" + std::to_string( u.get_moves() ) );
                    }
                    if( acted ) {
                        ++g->moves_since_last_save;
                        u.action_taken();
                        cata_mp::host_capture_avatar_msgs( pre_msg );
                    }
                }

                // Pure lockstep: no idle auto-wait.  The host blocks on this
                // client's action each turn — if the user doesn't press anything,
                // game time doesn't advance.  In-activity auto-ack still fires
                // (via CLI-GRANT-ACT-ACK) so wait/craft/read progress turn-by-turn
                // without intervention.

                // Pump MP events after each host action so the remote player's
                // queued actions are processed immediately, not deferred until the
                // next full do_turn() call.  Also re-invalidate the HUD so the
                // updated move count is visible on the very next redraw.
                if( cata_mp::is_hosting() ) {
                    cata_mp::process_mp_events();
                    cata_mp::ensure_mp_hud();
                }

                if( g->is_game_over() ) {
                    return turn_handler::cleanup_at_end();
                }

                if( g->uquit == QUIT_WATCH ) {
                    break;
                }
                const activity_id iter_pre_act = u.activity ? u.activity.id()
                                                 : activity_id::NULL_ID();
                while( u.get_moves() > 0 && u.activity ) {
                    u.activity.do_turn( u );
                }
                // Client: if a multi-turn activity just ended mid-input-loop
                // (e.g. a 1-tick ACT_DROP that finished with moves to spare),
                // emit the end signal immediately so the host's lockstep
                // bypass closes without waiting for the outer post-loop
                // dispatch.  Then drop the remaining moves to ack the host's
                // current grant — without this, the input loop loops back to
                // handle_action waiting on a user keypress while the host
                // sits in lockstep waiting for an ack that won't come until
                // the user presses a key.  Mirrors how SP "spends the turn"
                // on a drop: the post-activity moves are forfeit.
                if( cata_mp::is_client_mode() && iter_pre_act && !u.activity ) {
                    cata_mp::set_client_turn_activity( std::string() );
                    cata_mp::client_send_activity_end( iter_pre_act.str() );
                    if( !cata_mp::is_client_waiting_for_ack() ) {
                        cata_mp::mp_log( "[cdda-mp] in-loop activity-end: burning "
                                         + std::to_string( u.get_moves() )
                                         + " moves to ack host" );
                        u.set_moves( 0 );
                        cata_mp::client_dispatch_wait_for_activity(
                            activity_id(), /*force_idle=*/true );
                    }
                }
            }
            // Client: catch activities that started inside the input loop (e.g. ACT_WAIT_STAMINA
            // triggered by burn_move_stamina inside handle_action).  The pre-loop dispatch above
            // ran before the loop and couldn't see them — dispatch "wait" now if we haven't yet.
            // Guards:
            //  pre_activity_moves > 0  — only fire when a real server grant arrived this iteration;
            //                            prevents a "wait storm" when do_turn re-enters with 0 moves
            //                            and no grant (each extra "wait" drives g_remote_moves negative
            //                            and the positive grant never clears the ack guard).
            //  !is_client_waiting_for_ack() — don't stack a "wait" on top of a pending movement ack;
            //                            the pre-loop dispatch will handle it on the next grant.
            // Send a wait if either (a) moves were consumed this turn or (b) an
            // activity that was running has just ended.  Case (b) catches the
            // "drop completes mid-turn with moves left over" path that would
            // otherwise leave the client blocked in handle_action and the host
            // waiting through its 30s DISCONNECT-TIMEOUT.  Force the wait
            // through even when no activity is current, so a short activity
            // (drop_activity_actor finishing in one tick) still acks the host.
            const bool activity_just_ended = pre_activity_id && !u.activity;
            const bool moves_consumed = pre_activity_moves > 0 && u.get_moves() <= 0;
            if( cata_mp::is_client_mode() && activity_just_ended ) {
                // Explicit end-of-activity signal — closes the host's lockstep
                // bypass immediately so the next host turn re-enters lockstep.
                // Also clear the per-turn snapshot so the next enrich sends
                // client_activity="" instead of the stale id.
                cata_mp::set_client_turn_activity( std::string() );
                cata_mp::client_send_activity_end( pre_activity_id.str() );
            }
            if( cata_mp::is_client_mode() && !mp_wait_dispatched &&
                ( moves_consumed || activity_just_ended ) &&
                !cata_mp::is_client_waiting_for_ack() ) {
                const activity_id post_id = u.activity ? u.activity.id() : activity_id::NULL_ID();
                cata_mp::mp_log( "[cdda-mp] post-loop dispatch: pre_moves=" + std::to_string( pre_activity_moves ) +
                                 " cur_moves=" + std::to_string( u.get_moves() ) +
                                 " act=" + ( post_id ? post_id.str() : "none" ) +
                                 " ended=" + std::to_string( activity_just_ended ) +
                                 " force_idle=1" );
                cata_mp::client_dispatch_wait_for_activity( post_id, /*force_idle=*/true );
            } else if( cata_mp::is_client_mode() && pre_activity_moves > 0 && u.get_moves() <= 0 ) {
                cata_mp::mp_log( "[cdda-mp] post-loop SKIPPED: dispatched=" + std::to_string( mp_wait_dispatched ) +
                                 " ack=" + std::to_string( cata_mp::is_client_waiting_for_ack() ) +
                                 " act=" + ( u.activity ? u.activity.id().str() : "none" ) );
            }
            // Reset displayed sound markers now that the turn is over.
            // We only want this to happen if the player had a chance to examine the sounds.
            sounds::reset_markers();
        } else {
            // Rate limit key polling to 10 times a second.
            // Skip in host mode: the blocking poll would gate grant_client_turn()
            // to one grant per keypress.  UI keys for the host are handled inside
            // wait_for_client_action() via pump_events() instead.
            // Client mode: enabled — handle_input(0) is non-blocking unless the
            // user actually presses an interrupt key (e.g. `5`), in which case
            // the cancel-confirmation popup blocking briefly is the correct UX.
            {
                // Run for both host and client.  Earlier we gated on !is_hosting
                // because mp_poll_input() in wait_for_client_action handled host
                // UI keys.  That call was removed (it blocked on handle_action),
                // leaving the host with no way to cancel its own |-wait/craft/etc.
                // handle_input(0) is non-blocking unless the user actually presses
                // a cancel key, in which case the cancel-confirmation popup is the
                // correct UX — even on host.
                static auto start = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() );
                const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() );
                if( ( now - start ).count() > 100 ) {
                    handle_key_blocking_activity();
                    start = now;
                }
            }

            if( cata_mp::is_client_mode() ) {
                cata_mp::ensure_mp_hud();
                // Skip the blocking handle_action() when an activity is running.
                // Otherwise the wait_popup logic below never gets a chance to draw
                // (handle_action blocks for a keypress, and ACTION_PAUSE just sends
                // a wait dispatch — it never cancels the activity).  When activity
                // is set, handle_key_blocking_activity() (called above at the 100ms
                // poll) handles cancel-via-5 properly, and the wait_popup logic
                // displays progress.
                if( !u.activity ) {
                    cata_mp::mp_log( "[cdda-mp] LOCKED-HA: enter handle_action, moves=" +
                                     std::to_string( u.get_moves() ) );
                    g->handle_action();
                    cata_mp::mp_log( "[cdda-mp] LOCKED-HA: exit handle_action" );
                    ui_manager::redraw();
                }
            }

            g->mon_info_update();

            // If player is performing a task, a monster is dangerously close,
            // and monster can reach to the player or it has some sort of a ranged attack,
            // warn them regardless of previous safemode warnings
            if( u.activity ) {
                for( std::pair<const distraction_type, std::string> &dist : u.activity.get_distractions() ) {
                    if( g->cancel_activity_or_ignore_query( dist.first, dist.second ) ) {
                        break;
                    }
                }
            }
        }
    }

    if( g->driving_view_offset.x() != 0 || g->driving_view_offset.y() != 0 ) {
        // Still have a view offset, but might not be driving anymore,
        // or the option has been deactivated,
        // might also happen when someone dives from a moving car.
        // or when using the handbrake.
        vehicle *veh = veh_pointer_or_null( m.veh_at( u.pos_bub() ) );
        g->calc_driving_offset( veh );
    }

    scent_map &scent = get_scent();
    // No-scent debug mutation has to be processed here or else it takes time to start working
    if( !u.has_flag( json_flag_NO_SCENT ) ) {
        scent.set( u.pos_bub(), u.scent, u.get_type_of_scent() );
        overmap_buffer.set_scent( u.pos_abs_omt(),  u.scent );
    }
    scent.update( u.pos_bub(), m );

    // We need floor cache before checking falling 'n stuff
    m.build_floor_caches();

    m.process_falling();
    if( !cata_mp::is_client_mode() ) {
        const size_t pre_veh = cata_mp::is_hosting() ? Messages::size() : 0;
        m.vehmove();
        cata_mp::host_capture_vehmove_msgs( pre_veh );
    }
    if( !cata_mp::is_client_mode() ) {
        m.process_fields();
    }
    m.process_items();
    explosion_handler::process_explosions();
    m.creature_in_field( u );

    // Apply sounds from previous turn to monster and NPC AI.
    sounds::process_sounds();
    const int levz = m.get_abs_sub().z();
    // Update vision caches for monsters. If this turns out to be expensive,
    // consider a stripped down cache just for monsters.
    m.build_map_cache( levz, true );
    // Lockstep: wait for the client to act before advancing the world.
    if( cata_mp::is_hosting() ) {
        cata_mp::wait_for_client_action();
    }
    if( !cata_mp::is_client_mode() ) {
        if( cata_mp::is_hosting() ) {
            const auto t0 = std::chrono::steady_clock::now();
                monmove();
                const auto ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0 ).count() );
                cata_mp::set_last_monmove_ms( ms );
                if( ms > 50 ) {
                    cata_mp::mp_log( "[cdda-mp] monmove slow: " + std::to_string( ms ) + "ms" );
                }
        } else {
            monmove();
        }
        if( calendar::once_every( time_between_npc_OM_moves ) ) {
            overmap_npc_move();
        }
    }
    if( calendar::once_every( 10_seconds ) ) {
        for( const tripoint_bub_ms &elem : m.get_furn_field_locations() ) {
            const furn_t &furn = *m.furn( elem );
            for( const emit_id &e : furn.emissions ) {
                m.emit_field( elem, e );
            }
        }
        for( const tripoint_bub_ms &elem : m.get_ter_field_locations() ) {
            const ter_t &ter = *m.ter( elem );
            for( const emit_id &e : ter.emissions ) {
                m.emit_field( elem, e );
            }
        }
    }
    g->mon_info_update();
    // Client: process_turn() unconditionally adds get_speed() moves.  The
    // client's move allowance comes only from server grant packets, so we
    // need to discard process_turn's regen — but NOT any grant moves the
    // client already has.  Snapshot before, restore after.
    //
    // The naive "if moves > 0 set_moves(0)" wedged: when a grant arrives
    // during a locked-HA input poll (between turns), moves=92 entering
    // this block.  process_turn adds ~100 → 192.  Naive zero clobbers
    // the grant, autofire never fires, both players freeze forever.
    const int pre_process_turn_moves = u.get_moves();
    u.process_turn();
    if( cata_mp::is_client_mode() ) {
        u.set_moves( pre_process_turn_moves );
    }
    if( u.get_moves() < 0 && get_option<bool>( "FORCE_REDRAW" ) ) {
        ui_manager::redraw();
        refresh_display();
    }

    if( levz >= 0 && !u.is_underwater() ) {
        handle_weather_effects( weather.weather_id );
    }

    const bool player_is_sleeping = u.has_effect( effect_sleep );
    bool wait_redraw = false;
    std::string wait_message;
    time_duration wait_refresh_rate;
    if( player_is_sleeping ) {
        wait_redraw = true;
        wait_message = _( "Wait till you wake up…" );
        wait_refresh_rate = 30_minutes;
    } else if( const std::optional<std::string> progress = u.activity.get_progress_message( u ) ) {
        wait_redraw = true;
        wait_message = *progress;
        if( u.activity.is_interruptible() && u.activity.interruptable_with_kb ) {
            wait_message += string_format( _( "\n%s to interrupt" ), press_x( ACTION_PAUSE ) );
        }
        if( u.activity.id() == ACT_AUTODRIVE ) {
            wait_refresh_rate = 1_turns;
        } else if( u.activity.id() == ACT_FIRSTAID ) {
            wait_refresh_rate = 5_turns;
        } else {
            wait_refresh_rate = 5_minutes;
        }
    }
    if( wait_redraw ) {
        if( g->first_redraw_since_waiting_started ||
            calendar::once_every( std::min( 1_minutes, wait_refresh_rate ) ) ) {
            if( g->first_redraw_since_waiting_started || calendar::once_every( wait_refresh_rate ) ) {
                ui_manager::redraw();
            }

            // Avoid redrawing the main UI every time due to invalidation
#ifdef TILES
            // If an ImGui window just closed and cleared the buffer, do a full
            // redraw now before blocking UIs below.
            if( cataimgui::clear_pending() ) {
                ui_manager::redraw();
            }
#endif
            ui_adaptor dummy( ui_adaptor::disable_uis_below {} );
            if( !g->wait_popup ) {
                g->wait_popup = std::make_unique<static_popup>();
            }
            g->wait_popup->on_top( true ).wait_message( "%s", wait_message );
            ui_manager::redraw();
            refresh_display();
            g->first_redraw_since_waiting_started = false;
        }
    } else {
        // Nothing to wait for now
        g->wait_popup_reset();
        g->first_redraw_since_waiting_started = true;
    }

    m.invalidate_visibility_cache();

    u.update_bodytemp();
    u.update_body_wetness( *weather.weather_precise );
    u.apply_wetness_morale( weather.temperature );

    if( calendar::once_every( 1_minutes ) ) {
        u.update_morale();
        for( npc &guy : g->all_npcs() ) {
            if( cata_mp::is_remote_player( guy.getID() ) ) {
                continue;
            }
            guy.update_morale();
            guy.check_and_recover_morale();
        }
    }

    if( calendar::once_every( 9_turns ) ) {
        u.check_and_recover_morale();
    }

    if( !u.is_deaf() ) {
        sfx::remove_hearing_loss();
    }
    sfx::do_ambient();
    sfx::do_danger_music();
    sfx::do_vehicle_engine_sfx();
    sfx::do_vehicle_exterior_engine_sfx();
    sfx::do_low_stamina_sfx();

    // reset player noise
    u.volume = 0;

    // Calculate bionic power balance
    u.power_balance = u.get_power_level() - u.power_prev_turn;
    u.power_prev_turn = u.get_power_level();

#if defined(EMSCRIPTEN)
    // This will cause a prompt to be shown if the window is closed, until the
    // game is saved.
    EM_ASM( window.game_unsaved = true; );
#endif

    return false;
}
