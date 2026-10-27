//
// seams/gameplay_hooks.cpp -- PT-SET1: the apply hook of every F_HOSTSYNC row in ini_keys.def.
//
// ONE entry per synced row, in this shape (tools/gen_ini_registry.py --check parses it: exactly one entry
// per F_HOSTSYNC row and vice versa):
//
//   {"net", "foo_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { MH_Foo_SetRuntime(v.i); fx.foo_fix = v.i != 0; }},
//
// A hook sets BOTH the retail-stub runtime global (the byte-patch carrier reads it) and the libmh
// `reimpl_fixes` field (the promoted body reads it). A setter that REFUSES (its splice is MISMATCHED, so the byte-patch
// carrier is absent) keeps the fx field off too.
//
#include "caps/caps.h"
#include "config/gameplay.h"
#include "include/mh_net_export.h" // MH_Net_IsStarted (PT-CAP8: the MP warning)
#include "lockstep/turn_engine.h"
#include "seams/net_internal.h"
#include "sim/move/stream_heat.h" // mods:M1WL v3: sh_legal_cost / sh_params_clamped, the clamps of the tuning keys

namespace mh::gameplay {

// mods:M1 [net] improved_movement -- the ONE launcher switch over the movement prototypes. Every detail row is
// a dev key whose default -1 means "follow the switch": the switch's preset when on, retail when off. An
// explicit value (dev, [dev] unlock=1) wins either way. The switch's row precedes the detail rows in
// ini_keys.def, and dispatch runs in registry order, so g_move_master is set before any mv() reads it. The
// host's values (including the -1s) travel as they are, so every peer resolves the same numbers.
static bool g_move_master = false;
static int  mv(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_move_master ? preset : retail); }
// mods:M1WL v3..v5: the stream heat's tuning keys reach the retail splice together (sh_params_clamped is the one clamp, shared with the libmh twin).
static void push_heat(const mh::lockstep::reimpl_fixes &fx) {
    mh_set_move_stream_heat_params(fx.move_stream_heat_cost, fx.move_stream_heat_window, fx.move_stream_heat_min_mates, fx.move_stream_heat_scale, fx.move_stream_heat_retrace,
                                   fx.move_stream_heat_turn, fx.move_stream_heat_scale_cap, fx.move_stream_heat_margin);
}
// mods:M1 [net] group_movement -- the SEPARATE launcher switch over group movement (today: move_group_speed). Same
// -1 = follow-the-switch rule as mv(), with its own master; its row precedes move_group_speed in ini_keys.def.
static bool g_group_master = false;
static int  gm(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_group_master ? preset : retail); }
// mods:M1C [net] improved_combat -- the launcher switch over the combat prototypes (today: attack_range_footprint). Same
// -1 = follow-the-switch rule; its row precedes the rows that follow it in ini_keys.def.
static int  g_cor_resolved  = 0; // the resolved route_corridor_mode (route_flood_exact's default follows it)
static bool g_combat_master = false;
static int  cb(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_combat_master ? preset : retail); }

static bool g_raise_refused = false; // PT-CAP8 S5: the last [caps] raise apply asked for ON and set_limits(true) said no

static const hook_row k_hooks[] = {
    {"net", "turret_reload_realtime_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_reload_realtime_fix = mh_set_turret_reload_realtime(v.i) != 0 && v.i != 0; }},
    {"net", "turret_fire_rate_mult", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { mh_set_turret_fire_rate_mult(v.f); fx.turret_fire_rate_mult = v.f; }},
    {"net", "turret_clip_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_clip_fix = mh_set_turret_clip_fix(v.i) != 0 && v.i != 0; }},
    {"net", "turret_fire_poll_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.turret_fire_poll_fix = mh_set_turret_fire_poll_fix(v.i) != 0 && v.i != 0; }},
    {"net", "mother_free_build", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.mother_free_build = mh_set_mother_free_build(v.i) != 0 && v.i != 0; }},
    {"net", "mother_free_build_ai", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) { fx.mother_free_build_ai = mh_set_mother_free_build_ai(v.i) != 0 && v.i != 0; }},
    {"net", "improved_movement", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { g_move_master = v.i != 0; }},
    {"net", "group_movement", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { g_group_master = v.i != 0; }},
    {"net", "improved_combat", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { g_combat_master = v.i != 0; }},
    {"net", "move_centroid_wrap_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on              = mv(v, 1, 0);
         fx.move_centroid_wrap_fix = on != 0;
         mh_set_move_centroid_wrap_fix(on);
     }},
    {"net", "move_detour_slot_guard", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on              = mv(v, 1, 0);
         fx.move_detour_slot_guard = on != 0;
         mh_set_move_detour_slot_guard(on);
     }},
    {"net", "move_flood_queue_guard", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on              = mv(v, 1, 0);
         fx.move_flood_queue_guard = on != 0;
         mh_set_move_flood_queue_guard(on);
     }},
    {"net", "route_cost_mode", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on       = mv(v, 1, 0);
         fx.route_cost_mode = on;
         mh_set_route_cost_mode(on);
     }},
    {"net", "route_cost_base", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on       = mv(v, 1, 1);
         fx.route_cost_base = on;
         mh_set_route_cost_base(on);
     }},
    {"net", "route_cost_ring4", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 4);
         fx.route_cost_ring4 = on;
         mh_set_route_cost_ring(4, on);
     }},
    {"net", "route_cost_ring8", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 2, 8);
         fx.route_cost_ring8 = on;
         mh_set_route_cost_ring(8, on);
     }},
    {"net", "route_cost_ring16", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on         = mv(v, 3, 16);
         fx.route_cost_ring16 = on;
         mh_set_route_cost_ring(16, on);
     }},
    {"net", "route_gap_wave_full", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on           = mv(v, 6, 0);
         fx.route_gap_wave_full = on;
         mh_set_route_gap_wave_full(on);
     }},
    {"net", "route_corridor_mode", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on           = mv(v, 1, 0);
         fx.route_corridor_mode = on;
         g_cor_resolved         = on;
         mh_set_route_corridor_mode(on);
     }},
    // mods:M1H route_flood_exact: -1 follows route_corridor_mode (ON exactly when the resolved mode is 2: the whole-map flood is
    // unbounded work with the retail FIFO), 0 forces the retail FIFO, 1 forces the exact flood. Row order: after route_corridor_mode.
    {"net", "route_flood_exact", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on         = v.i >= 0 ? (v.i != 0) : (g_cor_resolved == 2 ? 1 : 0);
         fx.route_flood_exact = on != 0;
         mh_set_route_flood_exact(on);
     }},
    {"net", "move_block_wait_ms", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on          = mv(v, 1500, 0);
         fx.move_block_wait_ms = on;
         mh_set_move_block_wait_ms(on);
     }},
    {"net", "move_keep_sidestep", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on          = mv(v, 1, 0);
         fx.move_keep_sidestep = on != 0;
         mh_set_move_keep_sidestep(on);
     }},
    {"net", "move_repath_cooldown_ms", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on               = mv(v, 500, 0);
         fx.move_repath_cooldown_ms = on;
         mh_set_move_repath_cooldown_ms(on);
     }},
    {"net", "move_distinct_goals", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on           = mv(v, 1, 0);
         fx.move_distinct_goals = on != 0;
         mh_set_move_distinct_goals(on);
     }},
    {"net", "move_near_enough", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 0);
         fx.move_near_enough = on;
         mh_set_move_near_enough(on);
     }},
    {"net", "move_group_speed", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = gm(v, 1, 0);
         fx.move_group_speed = on != 0;
         mh_set_move_group_speed(on);
     }},
    {"net", "move_nudge_idle", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on       = mv(v, 1, 0);
         fx.move_nudge_idle = on != 0;
         mh_set_move_nudge_idle(on);
     }},
    {"net", "move_claim_leader", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on         = mv(v, 0, 0);
         fx.move_claim_leader = on != 0;
         mh_set_move_claim_leader(on);
     }},
    {"net", "move_swap_headon", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 0);
         fx.move_swap_headon = on != 0;
         mh_set_move_swap_headon(on);
     }},
    {"net", "move_route_through_idle", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on               = mv(v, 1, 0);
         fx.move_route_through_idle = on != 0;
         mh_set_move_route_through_idle(on);
     }},
    {"net", "move_route_through_moving_cost", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int k                       = mv(v, 0, 0); // mods:M1DT: preset 0 (user playtest rc16: cost 0 moves as expected, cost 3 was worse)
         fx.move_route_through_moving_cost = k < 0 ? 0 : (k > 4 ? 4 : k);
         mh_set_move_route_through_moving_cost(k);
     }},
    {"net", "move_route_through_moving", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on                 = mv(v, 1, 0);
         fx.move_route_through_moving = on != 0;
         mh_set_move_route_through_moving(on);
     }},
    // mods:M1DT fix 2: own units in state 0x0a (re-planning) are open ground to the planner too (shares the moving cost level).
    {"net", "move_route_through_replanning", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on                     = mv(v, 1, 0);
         fx.move_route_through_replanning = on != 0;
         mh_set_move_route_through_replanning(on);
     }},
    // mods:M1DT fix 4a: the block-wait budget restarts when the unit in the way changes to a walking friend. NOT in the preset (0): on the 20-order
    // benchmark it adds four 53-89 step stalls behind walking friends and, with 2 + 4b on, no gain (tmp/m1dt/RESULTS.md); flip the 0 to 1 to ship it.
    {"net", "move_block_reset_moving", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on               = mv(v, 0, 0);
         fx.move_block_reset_moving = on != 0;
         mh_set_move_block_reset_moving(on);
     }},
    // mods:M1DT fix 4b: a block-wait replan longer than N percent of the straight distance is discarded (bounded) and retried later.
    {"net", "move_replan_detour_cap", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int pct             = mv(v, 150, 0);
         fx.move_replan_detour_cap = pct < 0 ? 0 : (pct > 1000 ? 1000 : pct);
         mh_set_move_replan_detour_cap(pct);
     }},
    // mods:M1G 4b: formation from the click (sim/move/formation.h). 0 off (retail); 1..2 = the keep rule (members already standing on the click's
    // target set stay, N - 1 rings of slack; the rest go to the remaining tiles by total squared distance); 3 = the formation moves as one
    // (the whole order is planned together: a compact group is shifted rigidly, a spread one is gathered into a block). Clamped 1..3.
    {"net", "move_formation_click", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int n             = mv(v, 3, 0);
         fx.move_formation_click = n <= 0 ? 0 : (n > 3 ? 3 : n);
         mh_set_move_formation_click(n);
     }},
    // mp:U75: the unit-queue shuffle (a storage exit that makes room) and the idle nudge take a diagonal step only past an open flank tile, as the
    // route planner does; the shuffle search cannot index past its arrays. Default: on with improved_movement; 0 = retail.
    {"net", "move_queue_flank_guard", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on              = mv(v, 1, 0);
         fx.move_queue_flank_guard = on != 0;
         mh_set_move_queue_flank_guard(on);
     }},
    // mods:M1WL v2: stream heat (sim/move/stream_heat.h). 0 retail; 1 = a lone unit of a stream prices the tiles its groupmates are about to walk.
    {"net", "move_stream_heat", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 0);
         fx.move_stream_heat = on != 0;
         mh_set_move_stream_heat(on);
     }},
    // mods:M1WL v3: the stream heat's tuning keys (the preset is the shipped rule: cost 2, window 120, 8 mates, scaled up to 16 mates, margin 50). One hook each; they all store
    // the clamped value into fx and push the four numbers to the retail splice together (sh_params_clamped is the one clamp, shared with the twin).
    {"net", "move_stream_heat_cost", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_cost = mh::sim::move::sh_legal_cost(mv(v, 2, 2));
         push_heat(fx);
     }},
    {"net", "move_stream_heat_window", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_window = mv(v, mh::sim::move::SH_WINDOW, mh::sim::move::SH_WINDOW);
         fx.move_stream_heat_window = mh::sim::move::sh_params_clamped(2, fx.move_stream_heat_window, 8, 0).window;
         push_heat(fx);
     }},
    {"net", "move_stream_heat_min_mates", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_min_mates = mv(v, 8, 8);
         fx.move_stream_heat_min_mates = mh::sim::move::sh_params_clamped(2, mh::sim::move::SH_WINDOW, fx.move_stream_heat_min_mates, 0).min_mates;
         push_heat(fx);
     }},
    {"net", "move_stream_heat_scale", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_scale = mv(v, 1, 1) != 0 ? 1 : 0;
         push_heat(fx);
     }},
    // mods:M1WL v4: re-trace a heated plan charging the tile stepped onto (sim/move/stream_heat.h sh_retrace). -1 = on when the heat is on, 0 = rc9 behaviour.
    {"net", "move_stream_heat_retrace", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_retrace = v.i >= 0 ? (v.i != 0 ? 1 : 0) : (fx.move_stream_heat ? 1 : 0);
         push_heat(fx);
     }},
    // mods:M1WL v4: the re-trace's penalty for a step that turns by more than 45 degrees (cost units). -1 = the shipped default.
    {"net", "move_stream_heat_turn", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_turn = mh::sim::move::sh_params_clamped(2, mh::sim::move::SH_WINDOW, 8, 0, 1, v.i >= 0 ? v.i : mh::sim::move::SH_TURN_DEFAULT).turn;
         push_heat(fx);
     }},
    // mods:M1WL v5: the mate count at which the scaled cost stops growing (-1 = 16: cost 2 below 16 mates on a tile, 32 from 16). 1 .. 255.
    {"net", "move_stream_heat_scale_cap", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_scale_cap = mh::sim::move::sh_params_clamped(2, mh::sim::move::SH_WINDOW, 8, 1, 0, 0, v.i >= 0 ? v.i : mh::sim::move::SH_SCALE_CAP).scale_cap;
         push_heat(fx);
     }},
    // mods:M1WL v5: the margin rule (sim/move/stream_heat.h sh_margin): percent; a heated route longer than (100 + margin)% of the unheated one is replaced
    // by it. -1 = 50, 0 = off.
    {"net", "move_stream_heat_margin", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         fx.move_stream_heat_margin = mh::sim::move::sh_params_clamped(2, mh::sim::move::SH_WINDOW, 8, 1, 0, 0, mh::sim::move::SH_SCALE_CAP, v.i >= 0 ? v.i : mh::sim::move::SH_MARGIN_DEFAULT).margin;
         push_heat(fx);
     }},
    {"net", "move_attack_heading_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on               = mv(v, 1, 0);
         fx.move_attack_heading_fix = on != 0;
         mh_set_move_attack_heading_fix(on);
     }},
    {"net", "move_squad_merge_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on            = mv(v, 1, 0);
         fx.move_squad_merge_fix = on != 0;
         mh_set_move_squad_merge_fix(on);
     }},
    {"net", "move_merge_goal_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on           = mv(v, 1, 0);
         fx.move_merge_goal_fix = on != 0;
         mh_set_move_merge_goal_fix(on);
     }},
    {"net", "move_reverse_now", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 0);
         fx.move_reverse_now = on != 0;
         mh_set_move_reverse_now(on);
     }},
    {"net", "move_turn_rate", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int k       = mv(v, 0, 0); // preset 0 = retail: the playtester sets it by hand (user, 2026-10-18)
         fx.move_turn_rate = k < 0 ? 0 : (k > 2 ? 2 : k);
         mh_set_move_turn_rate(k);
     }},
    // mods:M1BE: soldiers ordered into a storage queue at its door. move_enter_queue: ORDER 0x24 units ignore move_block_wait_ms /
    // move_keep_sidestep / move_repath_cooldown_ms and wait behind an entering friend; storage_door_hold: the open door stays open while an
    // entering friend is within 2 tiles of the door tile.
    {"net", "move_enter_queue", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on        = mv(v, 1, 0);
         fx.move_enter_queue = on != 0;
         mh_set_move_enter_queue(on);
     }},
    {"net", "storage_door_hold", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on         = mv(v, 1, 0);
         fx.storage_door_hold = on != 0;
         mh_set_storage_door_hold(on);
     }},
    {"net", "attack_range_footprint", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on              = cb(v, 1, 0);
         fx.attack_range_footprint = on != 0;
         mh_set_attack_range_footprint(on);
     }},
    {"net", "projectile_facing_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on             = cb(v, 1, 0);
         fx.projectile_facing_fix = on != 0;
         mh_set_projectile_facing_fix(on);
     }},
    // mp:DSHEAL: the shot origin must not read the render-only soldier anim_change_count. Default ON (not a follower of
    // improved_combat: a desync fix, not a balance prototype); the host's value travels like every other match rule.
    {"net", "soldier_origin_anim_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &fx) {
         const int on               = v.i != 0;
         fx.soldier_origin_anim_fix = on != 0;
         mh_set_soldier_origin_anim_fix(on);
     }},
    // patching:PT-CAP8 [caps] raise: the v0.2.2 roster limit layer (buildings 100 -> 500, storage/turrets/mines/labs 25|32 -> 127,
    // productions 8 -> 127, projectiles 1500 -> 4000). The ARRAYS are relocated at boot whatever this says (mh::caps); this only flips
    // the 51 limit sites, at the PT-SET1 apply points (boot LOCAL, host PUBLISHED at Start, client ADOPTED before begin_map_load) -- all
    // outside a running sim. set_limits() refuses (logs, stays as it was) while a world is stepping or when the arrays are not relocated.
    {"caps", "raise", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) {
         // PT-CAP8 S5: remember whether a requested raise was refused (relocation refused / not staged / world live), so the
         // client's adopt point (launch.cpp mp_client_adopt_start) can leave the lobby instead of playing unraised against a raised host.
         g_raise_refused = v.i != 0 && !mh::caps::set_limits(true, "gameplay settings");
         if (v.i == 0) mh::caps::set_limits(false, "gameplay settings");
         // O1c: the replicated order lane carries a 12-bit building index (seams/order_wide_index.cpp), so
         // raise=1 in multiplayer no longer mis-targets building orders past index 255.
     }},
    // mp:LSCEIL [net] lockstep_max_ms: the command-delay ceiling of the adaptive lookahead (400 / 600 / 800 / 1000 ms). Not sim-affecting
    // (each peer's controller keeps its own lookahead below it), but the host's choice is adopted so every peer in a match runs the same
    // ceiling; the apply points are all before the lockstep starts. The setter snaps to the allowed set and scales the restart margin.
    {"net", "lockstep_max_ms", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_lockstep_max_ms(v.i); }},
    {nullptr, nullptr, nullptr}, // sentinel: keeps the array non-empty if every synced row is ever removed
};

bool raise_refused() { return g_raise_refused; }

const hook_row *hooks(int *count) {
    *count = (int)(sizeof(k_hooks) / sizeof(k_hooks[0])) - 1;
    return k_hooks;
}

} // namespace mh::gameplay
