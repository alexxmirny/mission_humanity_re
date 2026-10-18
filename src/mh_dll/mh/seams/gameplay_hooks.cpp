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
    {"net", "move_centroid_wrap_fix", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_centroid_wrap_fix(mv(v, 1, 0)); }},
    {"net", "move_detour_slot_guard", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_detour_slot_guard(mv(v, 1, 0)); }},
    {"net", "move_flood_queue_guard", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_flood_queue_guard(mv(v, 1, 0)); }},
    {"net", "route_cost_mode", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_cost_mode(mv(v, 1, 0)); }},
    {"net", "route_cost_base", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_cost_base(mv(v, 1, 1)); }},
    {"net", "route_cost_ring4", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_cost_ring(4, mv(v, 1, 4)); }},
    {"net", "route_cost_ring8", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_cost_ring(8, mv(v, 2, 8)); }},
    {"net", "route_cost_ring16", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_cost_ring(16, mv(v, 3, 16)); }},
    {"net", "route_gap_wave_full", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_gap_wave_full(mv(v, 6, 0)); }},
    {"net", "route_corridor_mode", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_route_corridor_mode(mv(v, 1, 0)); }},
    {"net", "move_block_wait_ms", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_block_wait_ms(mv(v, 1500, 0)); }},
    {"net", "move_keep_sidestep", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_keep_sidestep(mv(v, 1, 0)); }},
    {"net", "move_repath_cooldown_ms", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_repath_cooldown_ms(mv(v, 500, 0)); }},
    {"net", "move_distinct_goals", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_distinct_goals(mv(v, 1, 0)); }},
    {"net", "move_near_enough", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_near_enough(mv(v, 1, 0)); }},
    {"net", "move_group_speed", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_group_speed(gm(v, 1, 0)); }},
    {"net", "move_nudge_idle", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_nudge_idle(mv(v, 1, 0)); }},
    {"net", "move_claim_leader", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_claim_leader(mv(v, 0, 0)); }},
    {"net", "move_swap_headon", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_swap_headon(mv(v, 1, 0)); }},
    {"net", "move_route_through_idle", [](const mh_gameplay::value &v, mh::lockstep::reimpl_fixes &) { mh_set_move_route_through_idle(mv(v, 1, 0)); }},
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
