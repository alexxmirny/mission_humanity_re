//
// sim/hostreach/sim_h_bldg_init_defaults.h -- llm_strat_bldg_init_defaults @0x0045a068, the boot-time
// cfg::final::data::Building[] DEFAULTS SEEDER (RI-SIM / SIM-HOSTREACH batch H, 5531 B / 0x159b).
//
// Runs ONCE per building type, after the .cfg parse and before any tick (called from
// llm_strat_bldg_init_all), filling per-type fields the .cfg file itself does not carry:
// state_transition_ids[0..3] (+ builder_count for A/H_MOTHER only), door_approach_route/
// door_exit_route, dock_lift_offset_x/y, park_offset_x/y, shuttle_pad_offset_x/y, sprite_offset_x/y,
// selection_marker_offset_x/y, pip_slot_count + the mount/submount/pip anchor table
// (undef_block_tail), and 128 bytes of a still-anonymous pad region (_pad_0x796) that this function
// turns out to be the SOLE writer of -- see the DECLARED NEED in the .cpp.
//
// own.cfg_building_at(building_id) (RID_BUILDING, the write half of sim_view::cfg_buildings) is
// the ONLY writer of this region in the whole 538-row sim set -- confirmed by
// check_state_bindings + report_promotion_reconciliation.py (SwitchToPlanet's own two Building[]
// stores sit in a block that is dead by construction, per sim_h_switch_to_planet.h).
//
// THIS UNIT IS PROMOTED-ONLY, NO SHADOW SITE (SIM-HOSTREACH batch H, rule 1): the function mutates
// the shared cfg table itself (a boot-time-only, once-per-type write), so a shadow snapshot/restore
// would corrupt the very table every other sim function reads for the rest of the run -- same
// DO-NOT-ARM class as sim_player_presence_lost.h, which this file copies the shape of (a `_calls`
// struct, a `detail::` body over explicit state, a thin public wrapper, no shadow section at all).
//
// ---- Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_init_defaults_0045a068.asm); the .c draft's switch-case ENUM
// GROUPING (which type values share a store body) was cross-checked against the mechanically-
// recovered jump table and trusted for that one STRUCTURAL fact -- Ghidra's jump-table-to-case
// recovery is direct disassembly, not the "phantom content" failure mode the translator brief warns
// about. Every STORED VALUE was independently re-derived from the raw instructions; see the .cpp's
// per-case citations.
//
// ---- THE cfg_enum_E_BUILDING VALUES used as switch case labels in the .cpp come from
// sim/sim_order_enqueue.h's BUILDING_TYPE_* (a genuine Ghidra enum dump, 2026-08-08/2026-08-12,
// comment: "Ghidra enum dump 2026-08-12; A_BIURO/H_BYURO are real members with no case ... values are
// the enum's, not renumbered"), independently cross-confirmed by ai/ai_state.h's BLDG_TYPE_* and half
// a dozen other sim/ TUs (sim_order_dispatch_bldg.cpp, sim_bldg_unmap_footprint.cpp,
// sim_bldg_register_online.cpp, sim_bldg_completion_dispatch.cpp, sim_bldg_link_to_network.cpp,
// sim_combat_kill_credit.cpp) that each independently declare the same constants with the same
// values. This TU keeps its OWN local copy per the convention every one of those files already
// establishes (libmh/sim/ does not include a sibling TU's anonymous-namespace constants). An EARLIER
// (wrong) reading of this same function, before this cross-check, mis-assigned A_LAB/A_PORT/
// A_SHUTTLE positionally (0xb/0xc/0xd assumed to be PORT/SHUTTLE/LAB in .c listing order); the real
// values are A_LAB=0xb, A_PORT=0xc, A_SHUTTLE=0xd -- .c switch-case LISTING ORDER IS NOT VALUE ORDER,
// confirmed the hard way. Do not re-derive building-type values from .c positional order again.
//
// ---- THE PARAMETER: `building_id` arrives in EAX (uint, per the committed prototype) and the WHOLE
// body reads it back from a SINGLE 4-byte stack slot ([EBP-0x1c]) via a 16-bit sub-read (`MOVZX EAX,
// word ptr [EBP-0x1c]`) at every one of the function's ~150 use sites -- both dispatches and the
// final common tail -- confirmed by direct instruction inspection at every site, not by trusting the
// Ghidra .c's separate-looking `local_20`/`local_1c` locals (a DECOMPILER ARTIFACT: Ghidra re-slices
// the SAME stack dword differently at different points and renders it as two distinct-looking
// variables of different apparent widths). There is exactly ONE relevant value anywhere in this
// function: `(uint16_t)building_id`, used identically at every site.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one FRONTIER outward call this function makes (translator-brief rule 3b):
// llm_gfx_bldg_frame_center_offset, called once at the very end of every path through this function
// (0x0045b643). llm_strat_bldg_sprite_anchor_offset (9 call sites total in this function) is an
// ALREADY-TRANSLATED sibling reached DIRECTLY via mh::sim::detail::sprite_anchor_offset (translator-
// brief rule 3c) -- see sim/sim_bldg_sprite_anchor.h; it takes no `_calls` of its own, so nothing to
// thread through this struct.
struct bldg_init_defaults_calls {
    void (*gfx_bldg_frame_center_offset)(uint16_t building_id, int32_t *out_dx,
                                         int32_t *out_dy); // llm_gfx_bldg_frame_center_offset @0x00450b40
};

const bldg_init_defaults_calls &live_bldg_init_defaults_calls();

namespace detail {

// llm_strat_bldg_init_defaults @0x0045a068. Writes own.cfg_building_at(building_id)'s whole record
// and nothing else (no roster/session state touched). Returns void (matches the committed prototype).
void bldg_init_defaults(const sim_view &v, sim_store &own, const bldg_init_defaults_calls &c,
                        uint32_t building_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_init_defaults_calls(). PROMOTION SEAM --
// signature matches sig_llm_strat_bldg_init_defaults exactly: void __watcall(uint building_id).
void bldg_init_defaults(uint32_t building_id);

} // namespace mh::sim
