//
// sim/sim_unit_state_squad_merge.cpp -- see sim_unit_state_squad_merge.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_squad_merge_0047ea62.asm) -- see the header banner
// for the field-overload / preserved-ordering / declared-need derivations.
//
#include "sim/sim_unit_state_squad_merge.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT (shared there)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

const unit_state_squad_merge_calls &live_unit_state_squad_merge_calls() {
    static const unit_state_squad_merge_calls c = {
        MH_LIBMH_BIND(llm_map_wrap_delta_x),
        MH_LIBMH_BIND(llm_map_wrap_delta_y),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_PROMOTED_ROW(llm_strat_facing_step_apply) /* renamed from llm_ui_cursor_apply_anim_frame_offset 2026-09-02 */,
        MH_LIBMH_BIND(llm_strat_squad_pick_free_formation_anchor),
        MH_LIBMH_BIND(llm_strat_unit_teardown_mapped),
        MH_LIBMH_BIND(llm_strat_unit_change_proto_and_energy),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
    };
    return c;
}

namespace detail {

// `unit.unit_above` is `uint8_t[2]` (`map_t_unit_full_id`), and this function's own use of it is the
// OVERLOADED "soldier-crew chain head" meaning (see the header banner) -- reassembles the
// little-endian word a single `MOVZX reg, word ptr [...]` reads/writes in the original. Local copy per
// this TU's own convention (see sim_unit_transport_unload.cpp / sim_combat_kill_credit.cpp / others,
// each carrying their own copy rather than a shared helper).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0]) | (static_cast<uint16_t>(packed[1]) << 8);
}

void unit_state_squad_merge(const sim_view &v, sim_store &own, const unit_state_squad_merge_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    // ---- step 0 (0x0047ea7a-0x0047eaeb): snapshot self proto / cfg family base / own tile coords ----
    const int32_t self_proto = u.unit_proto_id;
    const int32_t cfgA       = v.cfg_units[self_proto].soldier_count;
    // +0x84/+0x85/+0x86/+0x87 -- x, y, goal_x, goal_y (see the header banner's correction vs the batch
    // context note).
    const int32_t x      = u.x;
    const int32_t y      = u.y;
    const int32_t goal_x = u.goal_x;
    const int32_t goal_y = u.goal_y;

    const int32_t  family_base = self_proto - cfgA + 1;
    const uint32_t cfgB        = v.cfg_units[family_base].soldier_type;

    // ---- step 1 (0x0047eaee-0x0047eb32): family-size scan ----
    // FIXED 2026-09-07 (SPCAMP A/B/C, the first oracle to reach this body with real input): `expected`
    // is INCREMENTED BEFORE THE COMPARE, not after it. The original INCs [EBP-0x50] at 0x0047eb05 and
    // the CMP at 0x0047eb1b reads that INCREMENTED slot, so the first iteration tests
    // cfg_units[family_base+1].soldier_count against 2 -- which is what a contiguous 1,2,3,4,5 family
    // requires. The header banner's own step-1 derivation says exactly this (`++family_count;
    // ++expected; ++idx; if (...soldier_count != expected) break;`); the C++ drifted from it into a
    // `for (expected = 1;; ++expected)` whose increment runs at the END of the body, so it compared
    // against 1 and broke on iteration 1 EVERY TIME. `family_count` was therefore always 1, and step
    // 3c (`target_count + cfgA > family_count`) then bailed out of every real merge.
    //
    // Written as an explicit pre-increment rather than `for (expected = 2;;)` so it reads the way the
    // banner and the asm do -- the seeded-start form is equivalent but re-hides the thing that was
    // wrong.
    int32_t family_count = 0;
    int32_t idx          = family_base;
    int32_t expected     = 1;
    for (;;) {
        ++family_count;
        ++expected;
        ++idx;
        if (v.cfg_units[idx].soldier_count != expected) break;
        if (v.cfg_units[idx].soldier_type != cfgB) break;
    }

    // ---- step 2 (0x0047eb32-0x0047eb87): goal-tile ownership/class check ----
    const tile_object &goal_tile = tile_at(v, goal_x, goal_y);
    if ((goal_tile.class_owner & 0xfu) != player) {
        // 0x0047eb54 (JNZ eb6d): not our tile -- bail.
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }
    if ((goal_tile.class_owner & 0x80u) == 0) {
        // 0x0047eb6b (JNZ not taken): not a unit standing there -- bail.
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }
    // `.building` (offset 0x2), not `.unit` (offset 0x4) -- see the header banner's FIELD OVERLOAD note.
    const int32_t target_idx = static_cast<int32_t>(goal_tile.building);

    // ---- step 3a (0x0047eb6f-0x0047ebc7): target must itself be a "family" unit type ----
    const int32_t target_proto = own.unit_at(player, target_idx).unit_proto_id;
    if (v.cfg_units[target_proto].soldier_count <= 0) {
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- step 3b (0x0047ebcb-0x0047ebe9): target must be idle ----
    if (own.unit_at(player, target_idx).state != UNIT_STATE_STOP_TO_DEFAULT) {
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- step 3c (0x0047ebed-0x0047ec1c): combined crew must fit the family's largest size ----
    if (v.cfg_units[target_proto].soldier_count + cfgA > family_count) {
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- step 3d (0x0047ec20-0x0047ec5e): same family (soldier_type) ----
    if (v.cfg_units[target_proto].soldier_type != v.cfg_units[self_proto].soldier_type) {
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- step 3e (0x0047ec62-0x0047ecb8): adjacency ----
    // Both wrap-delta calls happen unconditionally, then whichever axis produced the LARGER delta is
    // recomputed by a SECOND call rather than reusing the first result -- reproduced literally.
    // FIXED (2026-08-21, offline-oracle authoring): an earlier draft had this backwards (recomputed the
    // SMALLER axis, giving near=min(dx,dy)) -- confirmed via direct opcode decode at 0x0047ec86/ec88
    // (`CMP ESI,EAX; JLE`, ESI=dx/EAX=dy, jumps to the wrap_delta_y-recompute block when dx<=dy) that the
    // original recomputes the axis that is NOT the smaller one, i.e. near=max(dx,dy).
    const int32_t dx   = c.wrap_delta_x(x, y, goal_x);
    const int32_t dy   = c.wrap_delta_y(x, y, goal_x, goal_y);
    const int32_t near = (dx <= dy) ? c.wrap_delta_y(x, y, goal_x, goal_y) : c.wrap_delta_x(x, y, goal_x);
    if (near > 1) {
        c.unit_set_state(UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- step 4 (0x0047ecc9-0x0047ed11): facing + target chain-head snapshot ----
    // CORRECTED (reimpl-verify, 2026-08-21): the asm writes and reads back +0x2c, which
    // mh_structs.gen.h's static_assert proves is facing_target, NOT facing_current (+0x2d) -- the
    // first draft mismapped this field. facing_current is the actual/current facing that the
    // rotation-chase machinery steps toward facing_target one tick at a time; this write commits
    // the NEW target, not a snap of the current facing.
    const int32_t facing_needed = c.dir_from_to(x, y, goal_x, goal_y);
    u.facing_target             = static_cast<uint8_t>(facing_needed);
    const uint8_t own_facing    = u.facing_target;

    uint16_t chain = unit_full_id_word(own.unit_at(player, target_idx).unit_above);

    // ---- step 5 (0x0047ed14-0x0047ed8d): loop 1 -- snapshot the target's existing crew into the
    // shared anchor scratch. DO-WHILE: runs at least once even when `chain == 0` (soldier slot [0]'s
    // sentinel record gets read as if it were real data on that path -- a preserved original quirk).
    int32_t anchor_count = 0;
    int32_t tail         = 0;
    do {
        tail                                        = chain;
        own.squad_anchor_scratch_at(anchor_count).x = own.soldier_at(player, chain).end_x;
        own.squad_anchor_scratch_at(anchor_count).y = own.soldier_at(player, chain).end_y;
        ++anchor_count;
        chain = own.soldier_at(player, chain).next_soldier;
    } while (chain != 0);

    // ---- step 6 (0x0047ed8f-0x0047edce): graft this unit's crew onto the target's tail ----
    own.soldier_at(player, tail).next_soldier = unit_full_id_word(u.unit_above);
    // Snapshotted HERE, before unit_teardown_mapped() below -- see the header banner's ordering note
    // (step 8's `u.experience` read is deliberately NOT snapshotted this early).
    const double own_energy = u.energy;

    // ---- step 7 (0x0047edd5-0x0047ef36): loop 2 -- assign each of THIS unit's own (now-grafted)
    // soldiers a fresh walk-in start position and a picked formation end position. Same DO-WHILE /
    // runs-at-least-once shape as loop 1.
    chain                = unit_full_id_word(u.unit_above);
    int32_t merged_count = 0;
    do {
        c.cursor_apply_anim_frame_offset(
            reinterpret_cast<char *>(&own.soldier_at(player, chain).start_x),
            reinterpret_cast<char *>(&own.soldier_at(player, chain).start_y),
            static_cast<int32_t>(own_facing));
        c.squad_pick_free_formation_anchor(anchor_count,
                                           reinterpret_cast<char *>(&own.soldier_at(player, chain).end_x),
                                           reinterpret_cast<char *>(&own.soldier_at(player, chain).end_y));
        own.squad_anchor_scratch_at(anchor_count).x = own.soldier_at(player, chain).end_x;
        own.squad_anchor_scratch_at(anchor_count).y = own.soldier_at(player, chain).end_y;
        // ADDED 2026-09-07 (same oracle): 0x0047eecb/0x0047eed1 and 0x0047eefd/0x0047ef03 copy
        // start_x -> cur_x (+0x7 -> +0x4) and start_y -> cur_y (+0x8 -> +0x5), unconditionally, once
        // per soldier. The whole loop 2 body was translated from the header banner's step-7
        // derivation, and THAT derivation omits these two -- so the defect is in the banner as well
        // as here and both are corrected. They snap each newly-grafted soldier's CURRENT position to
        // the walk-in start cursor_apply_anim_frame_offset just wrote, which is what makes the merged
        // crew appear at the formation's edge and walk in; without them the soldiers keep whatever
        // cur_x/cur_y they held under their old unit and the interpolation runs from the wrong origin.
        own.soldier_at(player, chain).cur_x = own.soldier_at(player, chain).start_x;
        own.soldier_at(player, chain).cur_y = own.soldier_at(player, chain).start_y;
        ++anchor_count;
        ++merged_count;
        chain = own.soldier_at(player, chain).next_soldier;
    } while (chain != 0);

    // ---- step 8 (0x0047ef3c-0x0047efbc): finalize ----
    c.unit_teardown_mapped(static_cast<uint32_t>(player), static_cast<uint32_t>(unit_index));
    // Read LIVE (after teardown_mapped), NOT snapshotted -- see the header banner's ordering note.
    own.unit_at(player, target_idx).experience += u.experience;
    // The 4th arg (`unused`) is a leftover pointer-arithmetic value in the original's ECX at this call
    // site (from the LAST `MOV ECX,0xe0e5a8` inside loop 2 above, never re-set before this call) --
    // mh_calls.gen.h's own committed signature already names it `unused`, so its exact stale value is
    // provably inert; 0 is passed here (see uncertainties in the translation report).
    c.unit_change_proto_and_energy(player, target_idx, static_cast<int16_t>(merged_count), 0, own_energy);
    // Only on THIS success path -- unlike sim_unit_state_attack_building.cpp's unconditional entry
    // drain, none of the seven bail-outs above touch tick_budget.
    own.tick_budget() = 0.0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_squad_merge() {
    sim_state st = state();
    detail::unit_state_squad_merge(st.read, st.own, live_unit_state_squad_merge_calls());
}


} // namespace mh::sim
