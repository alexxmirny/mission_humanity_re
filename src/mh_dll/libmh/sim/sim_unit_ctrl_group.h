//
// sim/sim_unit_ctrl_group.h -- the two Ctrl+digit CONTROL-GROUP functions from the strategic sim's
// SIM1A slice, grouped into one TU because both act on the same _G_LLM_STRAT_CTRL_GROUPS region and
// neither is large enough to earn its own file.
//
//   * llm_strat_ctrl_group_contains_unit  @0x00445f27 (0x6f bytes) -- pure predicate.
//   * llm_strat_unit_ctrl_group_assign    @0x0044aa18 (0xad bytes) -- roster-scratch writer, via two
//                                          ORIGINAL helper calls (not reimplemented here).
//
// ---- DECLARED NEED: sim_view/sim_store have no member for _G_LLM_STRAT_CTRL_GROUPS -----------------
//
// Both functions index _G_LLM_STRAT_CTRL_GROUPS (RID_STRAT_CTRL_GROUPS, base 0x00b63be0, 4040 bytes =
// 10 entries x 0x194). Ghidra already has a typed record for one entry -- `llm_strat_ctrl_group`
// (category /Manual/map, size 0x194): `+0x00 count int; +0x04 unit_ids ushort[200]` (see
// docs/structs.md) -- but that type is NOT yet emitted into addr/mh_structs.gen.h, and sim_state.h has
// no view/store member for the region at all. Per the translator brief ("if the global or struct you
// need is missing from the manifest, stop and declare it -- do not work around it with an offset"),
// this TU references the members below AS IF they already existed, and will not compile standalone
// until the conductor adds them -- same move as sim_unit_passive_engage.cpp's engage-scratch-count
// gap. Needed central additions:
//
//   addr/mh_structs.gen.h: emit the existing Ghidra type `llm_strat_ctrl_group` as
//     struct mh_llm_strat_ctrl_group { int32_t count; uint16_t unit_ids[200]; };  // size 0x194
//   (naming matches the established mh_llm_strat_<X> convention other sim_state.h aliases use, e.g.
//   mh_llm_strat_map_geom, mh_llm_strat_player_profile.)
//
//   sim_state.h, in the "record types" section:
//     using ctrl_group = mh::game::mh_llm_strat_ctrl_group;
//
//   sim_state.h, on sim_view (read access for ctrl_group_contains_unit):
//     const ctrl_group *ctrl_groups; // _G_LLM_STRAT_CTRL_GROUPS[10] @ RID_STRAT_CTRL_GROUPS
//
//   sim_state.h, on sim_store (unit_ctrl_group_assign needs the ADDRESS of one group's `count` field
//   to hand to the two original ctrlgroup_add_member/remove_member helpers as their count_ptr
//   out-param -- the exact same "an address escapes because it is an OUT-PARAM to an original callee"
//   exception sim_store's text_scratch() member already documents and is built for):
//     ctrl_group &ctrl_group_at(int32_t group_index) { return ctrl_groups_[group_index]; }
//   backed by a private `ctrl_group *ctrl_groups_;` constructor parameter, bound in sim_state.cpp's
//   state() via ptr<ctrl_group>(RID_STRAT_CTRL_GROUPS) -- the SAME RID the view member above binds,
//   which is fine per sim_state.h's own precedent (the order-queue: "two modules resolving the same
//   RID through mh::state::ptr is fine; only a binding that does NOT come from the registry is the
//   hazard").
//
// Until these land, this TU will not compile on its own -- by design, so the gap stays visible rather
// than silently patched over with a raw offset/reinterpret_cast.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls (llm_strat_unit_ctrl_group_assign only) --------------------------------
//
// Indirected for the same reason as sim_unit_passive_engage.h's table: a direct mh::call:: inside a
// detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest. Both are ORIGINAL functions outside this batch that mutate
// _G_LLM_STRAT_CTRL_GROUPS in place via the count_ptr out-param -- see sim_state.h's needed
// ctrl_group_at() accessor above for how that address is obtained without an escaping raw pointer.
struct unit_ctrl_group_calls {
    void (*ctrlgroup_add_member)(int32_t unit_id, int32_t *count_ptr,
                                 int32_t group_id); // llm_strat_unit_ctrlgroup_add_member (committed
                                                    // pointee int32_t *, TACT1-P C6, 2026-09-04)
    void (*ctrlgroup_remove_member)(uint32_t unit_id, int32_t *count_ptr,
                                    int32_t group_id); // llm_strat_unit_ctrlgroup_remove_member (ditto)
};

const unit_ctrl_group_calls &live_unit_ctrl_group_calls();

namespace detail {

// llm_strat_ctrl_group_contains_unit @0x00445f27.
//
// Linear membership test: is `unit_id` present in _G_LLM_STRAT_CTRL_GROUPS[group_index].unit_ids
// [0..count)? Pure predicate -- no writes, no outward calls beyond the inert prologue (brief rule 6).
//
// PARAMETER ORDER, confirmed against the .asm header (not assumed positionally): EAX=unit_id (uint),
// EDX=count (int), EBX=group_index (int) -- i.e. group_index is genuinely the THIRD parameter here,
// matching the exported .c draft's declared order; there is no EAX/EDX/EBX-vs-declaration-order
// mismatch to correct in this function (unlike some other __watcall bodies in this tree where EBX
// arrives out of positional order).
int32_t ctrl_group_contains_unit(const sim_view &v, uint32_t unit_id, int32_t count, int32_t group_index);

// llm_strat_unit_ctrl_group_assign @0x0044aa18.
//
// Assigns `unit_id` to control group `new_group_id`: if the unit already belongs to a DIFFERENT group
// (ctrl_group_id != 0 -- note the original does not compare that group to new_group_id, only to 0; a
// unit already sitting in `new_group_id` still runs the remove step, which the ORIGINAL
// ctrlgroup_remove_member call absorbs as a no-op-ish swap-removal, not something reproduced by
// short-circuiting here), remove it from its current group first, then unconditionally add it to
// new_group_id. Both mutations happen inside the two ORIGINAL callees, addressed by the group's
// `count` field (see the DECLARED NEED above) -- this function itself never writes a roster region.
//
// AMBIENT PLAYER: the group being consulted is units[PlayerSide][unit_id], where PlayerSide is the
// AMBIENT global (the local human player's slot) -- NOT a parameter. Read via sim_view::player_side,
// the same idiom sim_order_dispatch_admin.cpp's PLAYER_SET_AI_HUMAN arm already uses for the same
// global. Signature stays __watcall(unit_id, new_group_id) -- no player parameter added.
void unit_ctrl_group_assign(const sim_view &v, sim_store &own, const unit_ctrl_group_calls &c,
                            int32_t unit_id, int32_t new_group_id);

} // namespace detail

// Live wrappers: the logic applied to state() (and live_unit_ctrl_group_calls() for the second).
// Match the originals' committed __watcall shapes (mh_export.gen.h's sig_ typedefs).
int32_t ctrl_group_contains_unit(uint32_t unit_id, int32_t count, int32_t group_index);
void    unit_ctrl_group_assign(int32_t unit_id, int32_t new_group_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
