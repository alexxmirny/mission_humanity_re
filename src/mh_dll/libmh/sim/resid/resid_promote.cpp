//
// sim/resid/resid_promote.cpp -- see resid_promote.h.
//
#include "sim/resid/resid_promote.h"

#include "addr/mh_export.gen.h" // sig_<name> typedefs, MH_EXPORT_REPLACE, mh_export_install_<name>()

// C10, through the host's hook table (F4D-PRE). This used to be a GUARDED harness include -- the
// arrangement lint_libmh_layering.py permitted -- and the guard is gone with the include: a module
// TU may no longer reach a harness header at all, guarded or not (check_libmh_outbound.py).
#include "state/hook_api.h" // mh::hosthook::harness_rebind_land_players -- the C10 landing rebind
#include "ai/ai_state.h"    // ai_say -- the shared trace sink every mh::sim promoted_arm TU logs through

// LIB-REF-IN: the adapters forward through the INBOUND C ENTRY, not into the C++ wrapper. See the
// banner block in sim/sim_hostreach_promote.cpp for the mechanism and the ordering hazard. ELEVEN
// of this module's nineteen covered rows route; the other eight are PRINTED EXCEPTIONS that
// gen_libmh_inbound.py classifies and re-derives every run -- two are served by the replay spine
// (libmh_sim_step, which LIB-REF implements and which is a DRIVER, so a seam inside the sim step
// calling it would recurse), and six by SEQUENCE entries (libmh_strat_mode_init,
// libmh_session_globals_reset) that run several bodies because one original body sequences them --
// a per-row seam cannot call one without executing its siblings.
#include "../../../libmh/include/libmh_host_in.h"

#include "sim/resid/sim_advisor_tick.h"
#include "sim/resid/sim_bldg_network_critical.h"
#include "sim/resid/sim_bldg_try_begin_placement.h"
#include "sim/resid/sim_clock_resync.h"
#include "sim/resid/sim_invasion_alert_reset.h"
#include "sim/resid/sim_invasion_due_check.h"
#include "sim/resid/sim_land_players_on_planet.h"
#include "sim/resid/sim_landing_spots_reroll.h"
#include "sim/resid/sim_map_fill_defaults.h"
#include "sim/resid/sim_new_game_init.h"
#include "sim/resid/sim_pathfinder_init.h"
#include "sim/resid/sim_planet_map_session_init.h"
#include "sim/resid/sim_planet_session_begin.h"
#include "sim/resid/sim_planet_transition_finalize.h"
#include "sim/resid/sim_player_init.h"
#include "sim/resid/sim_prod_transfer_destination.h"
#include "sim/resid/sim_rng_seed_channel.h"
#include "sim/resid/sim_scenario_planet_clone.h"
#include "sim/resid/sim_session_begin_multi.h"
#include "sim/resid/sim_session_clear_presence_flag.h"
#include "sim/resid/sim_session_state_reset.h"
#include "sim/resid/sim_spawn_ai_base.h"
#include "sim/resid/sim_squad_status_gather.h"
#include "sim/resid/sim_start_tutorial.h"
#include "sim/resid/sim_table_resets.h"
#include "sim/resid/sim_time_resync.h"
#include "sim/resid/sim_tutorial_step_driver.h"

#include <cstdint>
#include <cstring> // strcmp -- the C10 row match in rebind_land_players

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

namespace mh::sim {

namespace {

// PER-ROW FIRST-CALL LIVENESS. All 32 rows are T2 (offline-oracle-only -- none of them is armed, so
// this is the FIRST time any of them runs for real): the acceptance run's done_when needs, per row,
// proof that the seam was actually reached in the live process, not just that install_export_ok took
// the entry-byte guard. `fired` is a static local owned by the calling adapter, so each of the 32
// adapters gets its own one-shot latch. The "(OURS is live)" suffix is exact -- tools/test_ui.py greps
// it -- do not reword it. Logs through mh::ai::ai_say, the shared trace sink every other
// mh::sim::promoted_arm TU in this domain already uses (see the header banner) -- no module-local
// say()/set_logger() pair to wire.
void mark_first_call(bool &fired, const char *orig_name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [promote] sim_resid: %s call #1 (OURS is live)\n", orig_name);
}

} // namespace

// ---- PROMOTION (SIM-RESID-P): install this domain's 32 verified rows as the LIVE implementation ----
//
// One named adapter per row, matching its `::mh::exp::sig_<orig>` function-pointer type exactly (the
// generated naked entry thunk assigns straight into a `static sig_<orig>` -- a mismatched signature is
// a compile error, not a runtime one). Each adapter forwards into this same domain's already-verified
// `mh::sim::` public wrapper, with an explicit cast wherever the original's parameter/return width
// differs from the wrapper's (the wrapper's own header banner documents which width is the ORIGINAL's,
// e.g. sim_session_state_reset.h's `uint32_t param_1` -> `int32_t reset_flag`).
namespace promoted_arm {

// `mh::sim::promoted_arm` is ALSO used by sim/sim_order_dispatch.cpp (the dispatch entry's own
// promoted arm) -- namespaces merge across translation units, so this flag is kept internal-linkage
// (an anonymous sub-namespace, matching that TU's own `g_installed` convention for this exact
// namespace) rather than a bare namespace-scope global, to avoid a second TU ever colliding on the
// name at link time. Qualified access (`promoted_arm::g_any_installed`, used below by
// resid_promotion_active()) still resolves within this TU -- the using-directive an anonymous
// namespace injects into its enclosing namespace applies to qualified lookup too.
namespace {
bool g_any_installed = false;
} // namespace

// clang-format off
int32_t bldg_try_begin_placement(uint16_t player_idx, int32_t building_idx) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_bldg_try_begin_placement");
    return ::libmh_bldg_begin_placement(player_idx, building_idx);
}

void time_resync_and_tick() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_time_resync_and_tick");
    ::libmh_clock_resync_and_tick();
}

void planet_transition_finalize() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_planet_transition_finalize");
    ::libmh_planet_transition_finalize();
}

void try_enter_tactical_mission(uint32_t player, uint32_t bldg_idx, uint32_t param_3) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_try_enter_tactical_mission");
    ::libmh_enter_tactical_mission(player, bldg_idx, param_3);
}

int32_t bldg_gather_nearby_squad_status(int32_t scan_player, int32_t bldg_owner, int32_t bldg_idx) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_bldg_gather_nearby_squad_status");
    return ::mh::sim::bldg_gather_nearby_squad_status(scan_player, bldg_owner, bldg_idx);
}

void session_state_reset(uint32_t param_1) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_session_state_reset");
    ::mh::sim::session_state_reset(static_cast<int32_t>(param_1));
}

void planet_session_begin(int32_t race, int32_t reset_flag) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_planet_session_begin");
    ::libmh_planet_session_begin(race, reset_flag);
}

int32_t session_begin_multi(void *cfg_blob) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_session_begin_multi");
    return ::libmh_planet_session_begin_multi(cfg_blob);
}

void player_profile_init(uint32_t player, uint32_t controller_flags, uint32_t race, double game_clock,
                         uint32_t color_index, char *name_str, int32_t side_id) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_player_profile_init");
    ::mh::sim::player_profile_init(static_cast<int32_t>(player), controller_flags, race, game_clock,
                                   color_index, name_str, side_id);
}

void landing_spots_reroll_out_of_bounds() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_landing_spots_reroll_out_of_bounds");
    ::mh::sim::landing_spots_reroll_out_of_bounds();
}

void land_players_on_planet(uint32_t planet_index) {
    static bool fired = false;
    mark_first_call(fired, "llm_game_land_players_on_planet");
    ::mh::sim::land_players_on_planet(planet_index);
}

void player_param_defaults_init() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_player_param_defaults_init");
    ::mh::sim::player_param_defaults_init();
}

void tech_tables_reset() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_tech_tables_reset");
    ::mh::sim::tech_tables_reset();
}

void new_game_init() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_new_game_init");
    ::mh::sim::new_game_init();
}

void map_fill_defaults() {
    static bool fired = false;
    mark_first_call(fired, "map_FillDefaults");
    ::mh::sim::map_fill_defaults();
}

void scenario_planet_clone(void *cfg_blob) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_scenario_planet_clone");
    ::mh::sim::scenario_planet_clone(cfg_blob);
}

void pathfinder_init() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_pathfinder_init");
    ::mh::sim::pathfinder_init();
}

int32_t prod_set_transfer_destination(uint32_t player_idx, int32_t prod_slot, int32_t dest_planet) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_prod_set_transfer_destination");
    return ::libmh_prod_set_transfer_destination(player_idx, prod_slot, dest_planet);
}

void prod_reset_system() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_prod_reset_system");
    ::mh::sim::prod_reset_system();
}

int32_t bldg_is_network_critical(int32_t player, int32_t b_index) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_bldg_is_network_critical");
    return ::libmh_bldg_is_network_critical(player, b_index);
}

void session_clear_system_presence_flag() {
    static bool fired = false;
    mark_first_call(fired, "llm_game_session_clear_system_presence_flag");
    ::mh::sim::session_clear_system_presence_flag();
}

int32_t invasion_due_check() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_invasion_due_check");
    return ::mh::sim::invasion_due_check();
}

void clock_resync_units_and_buildings(double new_time) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_clock_resync_units_and_buildings");
    ::libmh_clock_rebase(new_time);
}

void invasion_alert_reset_all() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_invasion_alert_reset_all");
    ::libmh_invasion_alerts_reset();
}

void advisor_tick(double now) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_advisor_tick");
    ::mh::sim::advisor_tick(now);
}

void rng_seed_channel(int32_t ch, uint32_t value) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_rng_seed_channel");
    ::mh::sim::rng_seed_channel(ch, value);
}

int32_t tutorial_step_driver() {
    static bool fired = false;
    mark_first_call(fired, "llm_tutorial_step_driver");
    return ::mh::sim::tutorial_step_driver();
}

int32_t start_tutorial() {
    static bool fired = false;
    mark_first_call(fired, "llm_game_start_tutorial");
    return ::mh::sim::start_tutorial();
}

void sort_sites_by_dist(int32_t player_id) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_sort_sites_by_dist");
    ::mh::sim::sort_sites_by_dist(player_id);
}

void planet_map_session_init() {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_planet_map_session_init");
    ::libmh_planet_map_session_init();
}

void spawn_ai_base(int32_t player, int32_t is_alien, int32_t x, int32_t y) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_spawn_ai_base");
    ::mh::sim::spawn_ai_base(player, is_alien, x, y);
}

void init_human_player_data(uint32_t player_idx, int32_t is_alien_race) {
    static bool fired = false;
    mark_first_call(fired, "llm_strat_init_human_player_data");
    ::mh::sim::init_human_player_data(player_idx, is_alien_race);
}
// clang-format on

} // namespace promoted_arm

} // namespace mh::sim

// The 32 verified sim_resid seams (tools/data/sim_resid_migration.json, "state": "verified"). Every
// row is T2 (offline-oracle-only -- none of these is shadow-armed, per the domain's own no-shadow-site
// rule; see each header's own banner), so this promotion is the first time any of them runs against
// the real game image. MH_EXPORT_REPLACE expands at file scope (its generated `mh_export_install_*`
// is a TU-local `static` function, so it cannot live inside a namespace block) -- same placement as
// mh::orders's own P0-EXPORT block in orders/order_queue.cpp.
MH_EXPORT_REPLACE(llm_strat_bldg_try_begin_placement, mh::sim::promoted_arm::bldg_try_begin_placement)
MH_EXPORT_REPLACE(llm_strat_time_resync_and_tick, mh::sim::promoted_arm::time_resync_and_tick)
MH_EXPORT_REPLACE(llm_strat_planet_transition_finalize, mh::sim::promoted_arm::planet_transition_finalize)
MH_EXPORT_REPLACE(llm_strat_try_enter_tactical_mission, mh::sim::promoted_arm::try_enter_tactical_mission)
MH_EXPORT_REPLACE(llm_strat_bldg_gather_nearby_squad_status, mh::sim::promoted_arm::bldg_gather_nearby_squad_status)
MH_EXPORT_REPLACE(llm_strat_session_state_reset, mh::sim::promoted_arm::session_state_reset)
MH_EXPORT_REPLACE(llm_strat_planet_session_begin, mh::sim::promoted_arm::planet_session_begin)
MH_EXPORT_REPLACE(llm_strat_session_begin_multi, mh::sim::promoted_arm::session_begin_multi)
MH_EXPORT_REPLACE(llm_strat_player_profile_init, mh::sim::promoted_arm::player_profile_init)
MH_EXPORT_REPLACE(llm_strat_landing_spots_reroll_out_of_bounds, mh::sim::promoted_arm::landing_spots_reroll_out_of_bounds)
MH_EXPORT_REPLACE(llm_game_land_players_on_planet, mh::sim::promoted_arm::land_players_on_planet)
MH_EXPORT_REPLACE(llm_strat_player_param_defaults_init, mh::sim::promoted_arm::player_param_defaults_init)

namespace {
// C10: hand the harness OUR entry thunk for llm_game_land_players_on_planet, so its landing-conversion
// detour can fall through to us instead of the stolen prologue. Returns 1 only when a detour is
// actually armed this run ([test] all_ai=1); 0 otherwise, and then the ordinary entry install below is
// the correct route. Lives here rather than in reimpl_probe because the thunk is a static of THIS TU --
// the same reason mh::sim::order_queue_dispatch_entry_thunk() exists.
int rebind_land_players(const char *name) {
#ifndef MH_LIBMH_BUILD
    if (std::strcmp(name, "llm_game_land_players_on_planet") != 0) return 0;
    return mh::hosthook::harness_rebind_land_players(
        reinterpret_cast<void *>(mh_export_thunk_llm_game_land_players_on_planet));
#else
    // The guard that REMAINS is about the THUNK, not about the harness: MH_EXPORT_REPLACE generates
    // no entry thunk standalone (addr/mh_export.gen.h's standalone arm), so there is no address to
    // hand over. The harness question itself now needs no guard -- mh::hosthook answers 0 unbound.
    (void)name;
    return 0;
#endif
}
} // namespace
MH_EXPORT_REPLACE(llm_strat_tech_tables_reset, mh::sim::promoted_arm::tech_tables_reset)
MH_EXPORT_REPLACE(llm_strat_new_game_init, mh::sim::promoted_arm::new_game_init)
MH_EXPORT_REPLACE(map_FillDefaults, mh::sim::promoted_arm::map_fill_defaults)
MH_EXPORT_REPLACE(llm_strat_scenario_planet_clone, mh::sim::promoted_arm::scenario_planet_clone)
MH_EXPORT_REPLACE(llm_strat_pathfinder_init, mh::sim::promoted_arm::pathfinder_init)
MH_EXPORT_REPLACE(llm_strat_prod_set_transfer_destination, mh::sim::promoted_arm::prod_set_transfer_destination)
MH_EXPORT_REPLACE(llm_strat_prod_reset_system, mh::sim::promoted_arm::prod_reset_system)
MH_EXPORT_REPLACE(llm_strat_bldg_is_network_critical, mh::sim::promoted_arm::bldg_is_network_critical)
MH_EXPORT_REPLACE(llm_game_session_clear_system_presence_flag, mh::sim::promoted_arm::session_clear_system_presence_flag)
MH_EXPORT_REPLACE(llm_strat_invasion_due_check, mh::sim::promoted_arm::invasion_due_check)
MH_EXPORT_REPLACE(llm_strat_clock_resync_units_and_buildings, mh::sim::promoted_arm::clock_resync_units_and_buildings)
MH_EXPORT_REPLACE(llm_strat_invasion_alert_reset_all, mh::sim::promoted_arm::invasion_alert_reset_all)
MH_EXPORT_REPLACE(llm_strat_advisor_tick, mh::sim::promoted_arm::advisor_tick)
MH_EXPORT_REPLACE(llm_strat_rng_seed_channel, mh::sim::promoted_arm::rng_seed_channel)
MH_EXPORT_REPLACE(llm_tutorial_step_driver, mh::sim::promoted_arm::tutorial_step_driver)
MH_EXPORT_REPLACE(llm_game_start_tutorial, mh::sim::promoted_arm::start_tutorial)
MH_EXPORT_REPLACE(llm_strat_sort_sites_by_dist, mh::sim::promoted_arm::sort_sites_by_dist)
MH_EXPORT_REPLACE(llm_strat_planet_map_session_init, mh::sim::promoted_arm::planet_map_session_init)
MH_EXPORT_REPLACE(llm_strat_spawn_ai_base, mh::sim::promoted_arm::spawn_ai_base)
MH_EXPORT_REPLACE(llm_strat_init_human_player_data, mh::sim::promoted_arm::init_human_player_data)

namespace mh::sim {

bool resid_promotion_active() { return promoted_arm::g_any_installed; }

// C8-f-shaped: `default_on` is the SHIPPING DEFAULT for `[promote] sim_resid`, PASSED IN by the seams
// layer rather than read from a header here -- this module must not include a seams header (the
// layering lint keeps binary-bound seam code out of the reimplementation), so the one call site is
// where "what ships" is answerable, same discipline as mh::orders::install_promotion.
int install_promotion_resid(int default_on) {
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        bool (*install)();
    };
    // clang-format off
    const entry seams[] = {
        {"llm_strat_bldg_try_begin_placement",              mh_export_install_llm_strat_bldg_try_begin_placement},
        {"llm_strat_time_resync_and_tick",                  mh_export_install_llm_strat_time_resync_and_tick},
        {"llm_strat_planet_transition_finalize",            mh_export_install_llm_strat_planet_transition_finalize},
        {"llm_strat_try_enter_tactical_mission",             mh_export_install_llm_strat_try_enter_tactical_mission},
        {"llm_strat_bldg_gather_nearby_squad_status",       mh_export_install_llm_strat_bldg_gather_nearby_squad_status},
        {"llm_strat_session_state_reset",                   mh_export_install_llm_strat_session_state_reset},
        {"llm_strat_planet_session_begin",                  mh_export_install_llm_strat_planet_session_begin},
        {"llm_strat_session_begin_multi",                   mh_export_install_llm_strat_session_begin_multi},
        {"llm_strat_player_profile_init",                   mh_export_install_llm_strat_player_profile_init},
        {"llm_strat_landing_spots_reroll_out_of_bounds",    mh_export_install_llm_strat_landing_spots_reroll_out_of_bounds},
        {"llm_game_land_players_on_planet",                 mh_export_install_llm_game_land_players_on_planet},
        {"llm_strat_player_param_defaults_init",            mh_export_install_llm_strat_player_param_defaults_init},
        {"llm_strat_tech_tables_reset",                     mh_export_install_llm_strat_tech_tables_reset},
        {"llm_strat_new_game_init",                         mh_export_install_llm_strat_new_game_init},
        {"map_FillDefaults",                                mh_export_install_map_FillDefaults},
        {"llm_strat_scenario_planet_clone",                 mh_export_install_llm_strat_scenario_planet_clone},
        {"llm_strat_pathfinder_init",                       mh_export_install_llm_strat_pathfinder_init},
        {"llm_strat_prod_set_transfer_destination",         mh_export_install_llm_strat_prod_set_transfer_destination},
        {"llm_strat_prod_reset_system",                     mh_export_install_llm_strat_prod_reset_system},
        {"llm_strat_bldg_is_network_critical",               mh_export_install_llm_strat_bldg_is_network_critical},
        {"llm_game_session_clear_system_presence_flag",     mh_export_install_llm_game_session_clear_system_presence_flag},
        {"llm_strat_invasion_due_check",                    mh_export_install_llm_strat_invasion_due_check},
        {"llm_strat_clock_resync_units_and_buildings",       mh_export_install_llm_strat_clock_resync_units_and_buildings},
        {"llm_strat_invasion_alert_reset_all",              mh_export_install_llm_strat_invasion_alert_reset_all},
        {"llm_strat_advisor_tick",                          mh_export_install_llm_strat_advisor_tick},
        {"llm_strat_rng_seed_channel",                      mh_export_install_llm_strat_rng_seed_channel},
        {"llm_tutorial_step_driver",                        mh_export_install_llm_tutorial_step_driver},
        {"llm_game_start_tutorial",                         mh_export_install_llm_game_start_tutorial},
        {"llm_strat_sort_sites_by_dist",                    mh_export_install_llm_strat_sort_sites_by_dist},
        {"llm_strat_planet_map_session_init",               mh_export_install_llm_strat_planet_map_session_init},
        {"llm_strat_spawn_ai_base",                         mh_export_install_llm_strat_spawn_ai_base},
        {"llm_strat_init_human_player_data",                mh_export_install_llm_strat_init_human_player_data},
    };
    // clang-format on

    int attempted = 0;
    int ok        = 0;
    for (const entry &e : seams) {
        ++attempted;
        // C10: TRY THE REBIND FIRST for the one row a harness detour is entitled to own. The all-AI
        // soak's landing conversion arms in MH_Harness_Init, which runs before every promotion, so
        // this entry is ALREADY taken whenever [test] all_ai=1 -- and the plain install below was
        // therefore refused on every such run, reporting 30/31 PARTIAL. Same shape as
        // install_promotion_dispatch's D18 branch: the detour keeps the entry so the conversion still
        // runs, and only its fall-through moves to ours. Returns 0 when no detour is armed (the
        // ordinary case), and then the install below is the right route.
        if (rebind_land_players(e.name)) {
            ++ok;
            mh::ai::ai_say("; [promote] sim_resid: + %s REBOUND onto the ALLAI landing detour -- ours "
                           "IS the function, and the conversion still runs first (C10)\n",
                           e.name);
            continue;
        }
        if (e.install()) {
            ++ok;
            // The done_when's "promoted set named + size reported" evidence -- one line per
            // installed row, not just a count.
            mh::ai::ai_say("; [promote] sim_resid: + %s\n", e.name);
        } else {
            // install_export_ok already logged WHY (entry-byte guard mismatch = the DLL was built
            // against a different image). Refusing loudly beats a half-promoted domain.
            mh::ai::ai_say("; [promote] sim_resid: seam %s REFUSED -- container is NOT promoted\n",
                           e.name);
        }
    }
    if (ok != attempted) {
        mh::ai::ai_say(
            "; [promote] sim_resid: %d/%d seams installed -- PARTIAL, treat this run as invalid\n", ok,
            attempted);
    } else {
        mh::ai::ai_say("; [promote] sim_resid: ALL %d seams installed -- the resid domain is LIVE\n",
                       ok);
    }
    promoted_arm::g_any_installed = ok > 0;
    return ok;
}

} // namespace mh::sim
