//
// sim/hostreach/sim_batch_h_promote.cpp -- see sim_batch_h_promote.h.
//
#include "sim/hostreach/sim_batch_h_promote.h"

#include "addr/mh_export.gen.h" // sig_<name>, MH_EXPORT_REPLACE, mh_export_install_<name>()

// LIB-REF-IN: the adapters forward through the INBOUND C ENTRY, not into the C++ wrapper. See
// the banner block in sim/sim_hostreach_promote.cpp for the mechanism and the ordering hazard.
// The two group-order rows go through libmh_issue_order with their own ids -- that entry
// DISPATCHES on an id rather than running a sequence, so a per-row seam reaches exactly its own
// body. FOUR of this module's six covered rows route; the other two are PRINTED EXCEPTIONS
// (libmh_import_world, a replay-spine entry LIB-REF implements).
#include "../../../libmh/include/libmh_host_in.h"
#include "ai/ai_state.h" // ai_say -- the trace sink every mh::sim promoted_arm TU logs through

#include "state/hook_api.h" // F4D-PRE: entry_owner_of -- the host's hook table, never hook/ directly

// The 9 headers that declare the 15 public live wrappers these adapters forward into.
#include "sim/hostreach/sim_h_bldg_frame_center_offset.h"
#include "sim/hostreach/sim_h_bldg_init_defaults.h"
#include "sim/hostreach/sim_h_diplomacy_restore_relations.h"
#include "sim/hostreach/sim_h_group_issue_orders.h"
#include "sim/hostreach/sim_h_map_build_regions.h"
#include "sim/hostreach/sim_h_map_region_flood_fill.h"
#include "sim/hostreach/sim_h_map_region_prep.h"
#include "sim/hostreach/sim_h_map_region_seed.h"
#include "sim/hostreach/sim_h_shuttle_slot_spawn_arrival.h"
#include "sim/hostreach/sim_h_switch_to_planet.h"


namespace mh::sim {

namespace promoted_arm {

// `mh::sim::promoted_arm` is shared with sim/sim_hostreach_promote.cpp, sim/resid/resid_promote.cpp,
// sim/libtrans/sim_lt_promote.cpp and sim/sim_order_dispatch.cpp -- namespaces merge across
// translation units, so both the flag and the first-call helper stay internal-linkage (anonymous
// sub-namespace), matching those files' convention, so no two TUs can collide at link time.
namespace {
bool g_bh_promote_installed = false;

// PER-ROW FIRST-CALL LIVENESS (see the header banner). `fired` is a static local owned by the
// calling adapter, so each of the 15 gets its own one-shot latch. The '(OURS is live)' suffix is
// exact -- tools/test_ui.py greps it -- do not reword it.
void bh_mark_first_call(bool &fired, const char *orig_name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [promote] sim_batch_h: %s call #1 (OURS is live)\n", orig_name);
}
} // namespace

// clang-format off

// ---- sim/hostreach/sim_h_switch_to_planet.h --------------------------------------------------
int32_t bh_SwitchToPlanet(int32_t planet_index) {
    static bool fired = false;
    bh_mark_first_call(fired, "SwitchToPlanet");
    return ::libmh_planet_switch(planet_index);
}

// ---- sim/hostreach/sim_h_diplomacy_restore_relations.h --------------------------------------------------
void bh_llm_diplomacy_restore_relations(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_diplomacy_restore_relations");
    ::mh::sim::diplomacy_restore_relations();
}

// ---- sim/hostreach/sim_h_map_build_regions.h --------------------------------------------------
void bh_llm_map_build_regions(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_build_regions");
    ::mh::sim::build_regions();
}

// ---- sim/hostreach/sim_h_map_build_regions.h --------------------------------------------------
void bh_llm_map_assign_remaining_tiles_to_regions(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_assign_remaining_tiles_to_regions");
    ::mh::sim::assign_remaining_tiles_to_regions();
}

// ---- sim/hostreach/sim_h_group_issue_orders.h --------------------------------------------------
void bh_llm_strat_group_issue_attack_order(uint32_t param_1, uint32_t player, uint16_t selector) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_strat_group_issue_attack_order");
    const int32_t argv[3] = {(int32_t)param_1, (int32_t)player, (int32_t)selector};
    ::libmh_issue_order(LIBMH_ORD_GROUP_ISSUE_ATTACK_ORDER, argv, 3u);
}

// ---- sim/hostreach/sim_h_group_issue_orders.h --------------------------------------------------
void bh_llm_strat_group_issue_enter_building_order(uint32_t param_1, uint32_t param_2, uint32_t param_3) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_strat_group_issue_enter_building_order");
    const int32_t argv[3] = {(int32_t)param_1, (int32_t)param_2, (int32_t)param_3};
    ::libmh_issue_order(LIBMH_ORD_GROUP_ISSUE_ENTER_BUILDING_ORDER, argv, 3u);
}

// ---- sim/hostreach/sim_h_shuttle_slot_spawn_arrival.h --------------------------------------------------
void bh_llm_strat_prod_shuttle_slot_spawn_arrival(uint32_t slot_index) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_strat_prod_shuttle_slot_spawn_arrival");
    ::libmh_prod_shuttle_slot_spawn_arrival(slot_index);
}

// ---- sim/hostreach/sim_h_bldg_init_defaults.h --------------------------------------------------
void bh_llm_strat_bldg_init_defaults(uint32_t building_id) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_strat_bldg_init_defaults");
    ::mh::sim::bldg_init_defaults(building_id);
}

// ---- sim/hostreach/sim_h_map_region_flood_fill.h --------------------------------------------------
int32_t bh_llm_map_region_flood_fill(uint32_t x, uint32_t y, mh::game::mh_llm_map_region *block) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_region_flood_fill");
    return ::mh::sim::region_flood_fill(x, y, block);
}

// ---- sim/hostreach/sim_h_map_region_seed.h --------------------------------------------------
void bh_llm_map_try_seed_region_at(uint32_t x, uint32_t y, uint32_t max_d) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_try_seed_region_at");
    ::mh::sim::try_seed_region_at(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(max_d));
}

// ---- sim/hostreach/sim_h_map_region_seed.h --------------------------------------------------
void bh_llm_map_seed_regions_multires(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_seed_regions_multires");
    ::mh::sim::seed_regions_multires();
}

// ---- sim/hostreach/sim_h_map_region_prep.h --------------------------------------------------
void bh_llm_map_compute_obstacle_proximity_flags(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_compute_obstacle_proximity_flags");
    ::mh::sim::compute_obstacle_proximity_flags();
}

// ---- sim/hostreach/sim_h_map_region_prep.h --------------------------------------------------
void bh_llm_map_init_region_route_step_deltas(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_init_region_route_step_deltas");
    ::mh::sim::init_region_route_step_deltas();
}

// ---- sim/hostreach/sim_h_map_region_prep.h --------------------------------------------------
void bh_llm_map_compute_region_merge_threshold(void) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_map_compute_region_merge_threshold");
    ::mh::sim::compute_region_merge_threshold();
}

// ---- sim/hostreach/sim_h_bldg_frame_center_offset.h --------------------------------------------------
void bh_llm_gfx_bldg_frame_center_offset(uint16_t building_id, int32_t *out_dx, int32_t *out_dy) {
    static bool fired = false;
    bh_mark_first_call(fired, "llm_gfx_bldg_frame_center_offset");
    ::mh::sim::gfx_bldg_frame_center_offset(building_id, out_dx, out_dy);
}

} // namespace promoted_arm

MH_EXPORT_REPLACE(SwitchToPlanet, mh::sim::promoted_arm::bh_SwitchToPlanet)
MH_EXPORT_REPLACE(llm_diplomacy_restore_relations, mh::sim::promoted_arm::bh_llm_diplomacy_restore_relations)
MH_EXPORT_REPLACE(llm_map_build_regions, mh::sim::promoted_arm::bh_llm_map_build_regions)
MH_EXPORT_REPLACE(llm_map_assign_remaining_tiles_to_regions, mh::sim::promoted_arm::bh_llm_map_assign_remaining_tiles_to_regions)
MH_EXPORT_REPLACE(llm_strat_group_issue_attack_order, mh::sim::promoted_arm::bh_llm_strat_group_issue_attack_order)
MH_EXPORT_REPLACE(llm_strat_group_issue_enter_building_order, mh::sim::promoted_arm::bh_llm_strat_group_issue_enter_building_order)
MH_EXPORT_REPLACE(llm_strat_prod_shuttle_slot_spawn_arrival, mh::sim::promoted_arm::bh_llm_strat_prod_shuttle_slot_spawn_arrival)
MH_EXPORT_REPLACE(llm_strat_bldg_init_defaults, mh::sim::promoted_arm::bh_llm_strat_bldg_init_defaults)
MH_EXPORT_REPLACE(llm_map_region_flood_fill, mh::sim::promoted_arm::bh_llm_map_region_flood_fill)
MH_EXPORT_REPLACE(llm_map_try_seed_region_at, mh::sim::promoted_arm::bh_llm_map_try_seed_region_at)
MH_EXPORT_REPLACE(llm_map_seed_regions_multires, mh::sim::promoted_arm::bh_llm_map_seed_regions_multires)
MH_EXPORT_REPLACE(llm_map_compute_obstacle_proximity_flags, mh::sim::promoted_arm::bh_llm_map_compute_obstacle_proximity_flags)
MH_EXPORT_REPLACE(llm_map_init_region_route_step_deltas, mh::sim::promoted_arm::bh_llm_map_init_region_route_step_deltas)
MH_EXPORT_REPLACE(llm_map_compute_region_merge_threshold, mh::sim::promoted_arm::bh_llm_map_compute_region_merge_threshold)
MH_EXPORT_REPLACE(llm_gfx_bldg_frame_center_offset, mh::sim::promoted_arm::bh_llm_gfx_bldg_frame_center_offset)
// clang-format on

bool batch_h_promotion_active() { return promoted_arm::g_bh_promote_installed; }

int install_promotion_batch_h(int default_on) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: PROMOTION IS INJECTION, so the whole installer is hosted. Its `seams[]` table
    // pairs each callee's ORIGINAL ENTRY ADDRESS with the generated installer, and standalone every
    // one of those installers already refuses (addr/mh_export.gen.h's standalone arm) -- but the
    // table is built on the stack before any of them is called, so the addresses reached the object
    // anyway: 76 distinct original VAs across the three installers that still carry one, measured
    // 2026-09-11 after the macro-level arms had taken everything else.
    //
    // 0, not a partial attempt: the existing contract is "0 = not promoted, treat this run as
    // unpromoted", and a standalone build is exactly a run where nothing is promoted because there
    // is nothing to promote OVER. The ini read below would also be meaningless -- there is no ini.
    (void)default_on;
    return 0;
#else
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        uintptr_t   addr;
        bool (*install)();
    };
    // clang-format off
    const entry seams[] = {
        {"SwitchToPlanet",                             mh::exp::addr_SwitchToPlanet,                             mh_export_install_SwitchToPlanet},
        {"llm_diplomacy_restore_relations",            mh::exp::addr_llm_diplomacy_restore_relations,            mh_export_install_llm_diplomacy_restore_relations},
        {"llm_map_build_regions",                      mh::exp::addr_llm_map_build_regions,                      mh_export_install_llm_map_build_regions},
        {"llm_map_assign_remaining_tiles_to_regions",  mh::exp::addr_llm_map_assign_remaining_tiles_to_regions,  mh_export_install_llm_map_assign_remaining_tiles_to_regions},
        {"llm_strat_group_issue_attack_order",         mh::exp::addr_llm_strat_group_issue_attack_order,         mh_export_install_llm_strat_group_issue_attack_order},
        {"llm_strat_group_issue_enter_building_order", mh::exp::addr_llm_strat_group_issue_enter_building_order, mh_export_install_llm_strat_group_issue_enter_building_order},
        {"llm_strat_prod_shuttle_slot_spawn_arrival",  mh::exp::addr_llm_strat_prod_shuttle_slot_spawn_arrival,  mh_export_install_llm_strat_prod_shuttle_slot_spawn_arrival},
        {"llm_strat_bldg_init_defaults",               mh::exp::addr_llm_strat_bldg_init_defaults,               mh_export_install_llm_strat_bldg_init_defaults},
        {"llm_map_region_flood_fill",                  mh::exp::addr_llm_map_region_flood_fill,                  mh_export_install_llm_map_region_flood_fill},
        {"llm_map_try_seed_region_at",                 mh::exp::addr_llm_map_try_seed_region_at,                 mh_export_install_llm_map_try_seed_region_at},
        {"llm_map_seed_regions_multires",              mh::exp::addr_llm_map_seed_regions_multires,              mh_export_install_llm_map_seed_regions_multires},
        {"llm_map_compute_obstacle_proximity_flags",   mh::exp::addr_llm_map_compute_obstacle_proximity_flags,   mh_export_install_llm_map_compute_obstacle_proximity_flags},
        {"llm_map_init_region_route_step_deltas",      mh::exp::addr_llm_map_init_region_route_step_deltas,      mh_export_install_llm_map_init_region_route_step_deltas},
        {"llm_map_compute_region_merge_threshold",     mh::exp::addr_llm_map_compute_region_merge_threshold,     mh_export_install_llm_map_compute_region_merge_threshold},
        {"llm_gfx_bldg_frame_center_offset",           mh::exp::addr_llm_gfx_bldg_frame_center_offset,           mh_export_install_llm_gfx_bldg_frame_center_offset},
    };
    // clang-format on

    int ok = 0, attempted = 0;
    for (const entry &e : seams) {
        // The per-row ladder. READ THE HEADER BANNER before using it as a bisect: for a LEAF it is
        // exactly 'this row stays original'; for a PIPELINE member it rolls back the ENTRY only,
        // because our promoted siblings call our body directly in C++ and no ini reaches that edge.
        ++attempted;
        if (e.install()) {
            ++ok;
            mh::ai::ai_say("; [promote] sim_batch_h: + %s\n", e.name);
        } else {
            // Distinguish 'another detour already owns this entry' (name it -- the fix is a rebind,
            // not a rebuild) from 'the entry bytes do not match this build' (wrong image).
            if (const char *owner = mh::hosthook::entry_owner_of(e.addr))
                mh::ai::ai_say(
                    "; [promote] sim_batch_h: seam %s REFUSED -- the entry is held by %s. NOT "
                    "necessarily a build problem: an owned entry means another detour got there "
                    "first (rebind it, or accept this row stays original)\n",
                    e.name, owner);
            else
                mh::ai::ai_say("; [promote] sim_batch_h: seam %s REFUSED -- entry guard "
                               "mismatch, NOT promoted\n",
                               e.name);
        }
    }

    if (ok != attempted) {
        mh::ai::ai_say("; [promote] sim_batch_h: %d/%d seams installed -- PARTIAL, treat this run "
                       "as invalid\n",
                       ok, attempted);
    } else {
        mh::ai::ai_say("; [promote] sim_batch_h: ALL %d seams installed -- batch H is LIVE\n", ok);
    }

    promoted_arm::g_bh_promote_installed = ok > 0;
    return ok;
#endif // MH_LIBMH_BUILD
}

} // namespace mh::sim
