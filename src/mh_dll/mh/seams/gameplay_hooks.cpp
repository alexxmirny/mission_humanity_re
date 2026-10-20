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
#include "config/gameplay.h"
#include "lockstep/turn_engine.h"
#include "seams/net_internal.h"

namespace mh::gameplay {

// mods:M1 [net] improved_movement -- the ONE launcher switch over the movement prototypes. Every detail row is
// a dev key whose default -1 means "follow the switch": the switch's preset when on, retail when off. An
// explicit value (dev, [dev] unlock=1) wins either way. The switch's row precedes the detail rows in
// ini_keys.def, and dispatch runs in registry order, so g_move_master is set before any mv() reads it. The
// host's values (including the -1s) travel as they are, so every peer resolves the same numbers.
static bool g_move_master = false;
static int  mv(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_move_master ? preset : retail); }
// mods:M1 [net] group_movement -- the SEPARATE launcher switch over group movement (today: move_group_speed). Same
// -1 = follow-the-switch rule as mv(), with its own master; its row precedes move_group_speed in ini_keys.def.
static bool g_group_master = false;
static int  gm(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_group_master ? preset : retail); }
// mods:M1C [net] improved_combat -- the launcher switch over the combat prototypes (today: attack_range_footprint). Same
// -1 = follow-the-switch rule; its row precedes the rows that follow it in ini_keys.def.
static int  g_cor_resolved  = 0; // the resolved route_corridor_mode (route_flood_exact's default follows it)
static bool g_combat_master = false;
static int  cb(const mh_gameplay::value &v, int preset, int retail) { return v.i >= 0 ? v.i : (g_combat_master ? preset : retail); }

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
    {nullptr, nullptr, nullptr}, // sentinel: keeps the array non-empty if every synced row is ever removed
};

const hook_row *hooks(int *count) {
    *count = (int)(sizeof(k_hooks) / sizeof(k_hooks[0])) - 1;
    return k_hooks;
}

} // namespace mh::gameplay
