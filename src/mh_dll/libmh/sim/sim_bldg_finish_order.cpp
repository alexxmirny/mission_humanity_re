//
// sim/sim_bldg_finish_order.cpp -- see sim_bldg_finish_order.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_bldg_finish_current_order_00470cab.asm), which confirms the decompile's four-branch
// shape and callee order exactly (no floats, no hidden control flow).
//
#include "sim/sim_bldg_finish_order.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_finish_order_calls &live_bldg_finish_order_calls() {
    static const bldg_finish_order_calls c = {
        MH_LIBMH_BIND(llm_unit_apply_production_completion),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
        MH_LIBMH_BIND(llm_cfg_apply_project_resources),
        MH_LIBMH_BIND(llm_strat_bldg_grant_type_resources),
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_bldg_unassign_workers),
        MH_LIBMH_BIND(llm_strat_bldg_clear_staffed_flag),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace {
// llm_strat_bldg_state members this function reads, transcribed from the disassembly's CMP
// immediates (0x00470cd8/0x00470dce/0x00470e1d/0x00470f62) -- same bare-uint16_t-field reasoning as
// sim_order_dispatch_bldg.cpp's own BLDG_STATE_* block; not shared with it (anonymous-namespace,
// different TU) so redeclared here with the same values.
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP  = 0x6a;
inline constexpr uint16_t BLDG_STATE_PROD_WORKING = 0x6d;
inline constexpr uint16_t BLDG_STATE_UPGRADING    = 0x82;
inline constexpr uint16_t BLDG_STATE_RESEARCHING  = 0x89;

// `current_workers` is uint16_t compared SIGNED against a 32-bit builder_count in the disassembly
// (0x00470e9f/0x00470fb7: CMP EDX, dword ptr [...] after a widening load) -- same pattern
// sim_order_dispatch_bldg.cpp's workers_of() names for the identical comparison shape.
inline int32_t workers_of(const building &b) { return (int32_t)(uint32_t)b.current_workers; }

// UPGRADING (0x00470e53-0x00470f52) and CHARGE_STEP (0x00470f76-0x0047106a) re-staff identically:
// if the building doesn't use workers, drop them all; if it does and the surplus over the (possibly
// smaller, post-upgrade) cfg worker_count is positive, unassign the surplus; if current_workers has
// reached zero either way, clear the staffed flag. Two genuinely separate call sites in the original
// (not a shared subroutine -- the disassembly duplicates the whole sequence byte for byte at both
// addresses), reproduced here as one helper because C++ has no reason to duplicate what the source
// doesn't need to.
//
// THE FIELD IS worker_count, NOT builder_count -- a real bug caught by reimpl-verify (2026-08-10)
// before this was ever armed. Both immediates at 0x00470e9f/0x00470edb (and the CHARGE_STEP mirror
// at 0x00470fb7/0x00470ff3) are 0xd9eed8; cfg_buildings' base is 0x00d9ec80 (addr/mh_regions.gen.h),
// so 0xd9eed8 - 0xd9ec80 = 0x258, which addr/mh_structs.gen.h's own static_assert ties to
// worker_count, NOT builder_count (0x25c) -- confirmed independently by the same UPGRADING branch's
// separate upgrade_index read at 0x00470e49 (immediate 0xd9eee1, i.e. base + 0x261, matching that
// field's own asserted offset exactly), which rules out a wrong base/stride for the whole derivation.
void restaff_if_idle(const sim_view &v, sim_store &own, const bldg_finish_order_calls &c,
                     uint32_t player, uint32_t building_index) {
    building &b = own.building_at(player, (int32_t)building_index);
    if (c.bldg_uses_workers(player, (int32_t)building_index) == 0) {
        if (workers_of(b) != 0) {
            c.bldg_unassign_workers((uint16_t)player, building_index, (uint32_t)workers_of(b));
        }
    } else {
        const int32_t worker_count = v.cfg_buildings[b.building_id].worker_count;
        if (workers_of(b) > worker_count) {
            c.bldg_unassign_workers((uint16_t)player, building_index,
                                    (uint32_t)(workers_of(b) - worker_count));
        }
        if (b.current_workers == 0) { c.bldg_clear_staffed_flag((uint16_t)player, building_index); }
    }
}
} // namespace

namespace detail {

void bldg_finish_current_order(const sim_view &v, sim_store &own, const bldg_finish_order_calls &c,
                               uint32_t player, uint32_t building_index) {
    building &b = own.building_at(player, (int32_t)building_index);

    // ---- PROD_WORKING (0x00470cd8-0x00470dbe): finish the in-flight unit, credit its production
    // queue's running total (slot 0) AND its own per-type slot. ----
    if (b.state == BLDG_STATE_PROD_WORKING) {
        production   &p                = own.production_at(player, b.sub_id);
        const uint8_t active_unit_type = p.active_unit_type;
        c.unit_apply_production_completion(player, (int32_t)active_unit_type);
        c.ai_notify_unit_lifecycle((uint16_t)player, (uint16_t)active_unit_type, 0, 2);
        own.production_at(player, b.sub_id).queued_count[0] += 1;
        own.production_at(player, b.sub_id).queued_count[active_unit_type] += 1;
    }

    // ---- RESEARCHING (0x00470dce-0x00470e0d): apply the finished project's resource grant. ----
    if (b.state == BLDG_STATE_RESEARCHING) {
        c.cfg_apply_project_resources(player, (uint32_t)own.lab_at(player, b.sub_id).active_project_id);
    }

    // ---- UPGRADING (0x00470e1d-0x00470f52): grant the new type's resources, then re-staff. ----
    if (b.state == BLDG_STATE_UPGRADING) {
        c.bldg_grant_type_resources(player, v.cfg_buildings[b.building_id].upgrade_index);
        restaff_if_idle(v, own, c, player, building_index);
    }

    // ---- CHARGE_STEP (0x00470f62-0x0047106a): re-staff only -- no resource grant. ----
    if (b.state == BLDG_STATE_CHARGE_STEP) { restaff_if_idle(v, own, c, player, building_index); }

    // ---- Always: fall back to the building type's config-driven idle state, then refresh the UI.
    // 0x0047106a-0x004710b1. `state_transition_ids[1]` (RESOLVED, conductor 2026-08-22 -- named in
    // Ghidra, addr/mh_structs.gen.h; a THIRD independent reader agreeing with SIM1-G4's
    // land_activate/charge_gate on this same element's meaning). ----
    own.building_at(player, (int32_t)building_index).state =
        (uint16_t)v.cfg_buildings[b.building_id].state_transition_ids[1];
    c.bldg_notify_ui((uint16_t)player, building_index);
}

} // namespace detail

void bldg_finish_current_order(uint32_t player, uint32_t building_index) {
    sim_state st = state();
    detail::bldg_finish_current_order(st.read, st.own, live_bldg_finish_order_calls(), player,
                                      building_index);
}

} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
