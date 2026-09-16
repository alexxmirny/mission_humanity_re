//
// sim/sim_game_try_start_project.cpp -- see sim_game_try_start_project.h. Translated from the
// DISASSEMBLY (tmp/decomp/game_TryStartProject_00492cac.asm), corroborated by, but not sourced from,
// the Ghidra .c draft.
//
#include "sim/sim_game_try_start_project.h"

#include "addr/mh_calls.gen.h"  // mh::call::game_TryStartProject (export target) / game_SpendResource
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const try_start_project_calls &live_try_start_project_calls() {
    static const try_start_project_calls c = {
        MH_LIBMH_BIND(game_SpendResource),
    };
    return c;
}

namespace detail {

int32_t game_try_start_project(const sim_view &v, const try_start_project_calls &c, uint32_t player,
                               int32_t b_idx, uint32_t project_id) {
    // Truncated to the low 16 bits at EVERY use site in the original (0x00492cdb/0x00492d00/
    // 0x00492d80/0x00492e12) -- one local here, matching sim_bldg_pay_costs.cpp's own convention.
    const int32_t p = (int32_t)(player & 0xFFFFu);

    const cfg_project &proj = v.cfg_projects[project_id];

    // ---- gate 1 (0x00492ccb-0x00492cf7): the prerequisite invention must be researched. No resource
    // walk at all, no building check at all, if it is not.
    if (progress_of(v, p, proj.invention).available == 0) {
        return PROJECT_ERR_INVENTION_NOT_RESEARCHED;
    }

    // ---- gate 2 (0x00492cfc-0x00492d3d): the building's cfg project_type must match this project's
    // type. `buildings[player][b_idx].building_id` is the roster instance's cfg TYPE id.
    const uint16_t      building_id = building_of(v, (uint32_t)p, b_idx).building_id;
    const cfg_building &cb          = v.cfg_buildings[building_id];
    if (cb.project_type != proj.type) {
        return PROJECT_ERR_TYPE_MISMATCH;
    }

    // ---- pass 1 (0x00492d3d-0x00492dbf): scan all up to 7 resource slots, accumulating an error code
    // WITHOUT short-circuiting on the first shortage -- identical shape to sim_bldg_pay_costs.cpp's two
    // functions; see that header's banner for the accumulation rule and the id-read-before-bound-check
    // order (id read first, tested against UNDEFINED(0); `i < CFG_RESOURCE_SLOTS` tested second).
    int32_t error = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = proj.resource[i].id; // 0x00492d5a
        if (resource_id == 0) break;                      // 0x00492d63: UNDEFINED
        if (!(i < CFG_RESOURCE_SLOTS)) break;             // 0x00492d69: bound check, evaluated SECOND

        // 0x00492d8f-0x00492d9b: holdings < cost?
        if (player_resource_of(v, p, (int32_t)resource_id) < proj.resource[i].val) {
            // 0x00492d9d-0x00492db4: first shortage sets id+0x89; any later one collapses to the bare
            // sentinel 0x89, discarding which resource(s) were short.
            error = (error == 0) ? (int32_t)(resource_id + PROJECT_ERR_RESOURCE_SHORTAGE_BASE)
                                 : PROJECT_ERR_RESOURCE_SHORTAGE_BASE;
        }
    }
    if (error != 0) return error; // 0x00492dbf-0x00492dcb

    // ---- pass 2 (0x00492dcd-0x00492e23): everything affordable -- actually charge. Same walk shape as
    // pass 1, over the SAME resource array (re-read rather than cached, matching the original).
    for (int32_t i = 0;; ++i) {
        const uint32_t resource_id = proj.resource[i].id; // 0x00492de3
        if (resource_id == 0) break;
        if (!(i < CFG_RESOURCE_SLOTS)) break;

        // 0x00492e16: game_SpendResource(player, resource_id, val) -- an untranslated ORIGINAL sibling
        // (Law 4). Routed through this TU's `try_start_project_calls` struct (bound to
        // `mh::call::game_SpendResource` in live_try_start_project_calls()) so the offline oracle can
        // stub it; behaviour-identical in production -- the pointer targets the same live-image callee.
        c.spend_resource(p, (int32_t)resource_id, proj.resource[i].val);
    }
    return 0; // 0x00492e23
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t game_try_start_project(uint16_t player, int32_t b_idx, uint32_t project_id) {
    const sim_view v = state().read;
    return detail::game_try_start_project(v, live_try_start_project_calls(), player, b_idx, project_id);
}


} // namespace mh::sim
