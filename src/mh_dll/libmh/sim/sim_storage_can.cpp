//
// sim/sim_storage_can.cpp -- see sim_storage_can.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_can_{exit,enter,land}_*.asm), not from the exported Ghidra .c
// drafts -- every address cited below was independently walked against the raw
// IMUL/MOVZX/CMP/JC/JBE/JZ/JNZ/JL/JLE/JG/JGE opcodes per house rules.
//
#include "sim/sim_storage_can.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const storage_can_exit_calls &live_storage_can_exit_calls() {
    static const storage_can_exit_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_exit_tile_is_clear),
        MH_LIBMH_BIND(llm_strat_storage_resolve_exit_blockage),
    };
    return c;
}

const storage_can_enter_calls &live_storage_can_enter_calls() {
    static const storage_can_enter_calls c = {
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        MH_LIBMH_BIND(llm_prod_shuttle_slot_bind_default),
    };
    return c;
}

namespace {

// The storage occupancy cap all three functions in this file gate against (0x32 == 50) -- three
// independent CMPs against the same literal (can_exit does not use it; can_enter's two arms and
// can_land's one arm all do). Named once here rather than per-site.
constexpr int32_t STORAGE_OCCUPANCY_CAP = 0x32;

// units[player][unit_index].state == 0x24 -- the SAME value sim_unit_state_enter.h's
// ENTER_ARRIVAL_CHECK_STATE_ENTER_STORAGE_BEGIN/ENTER_WAIT_STATE_ENTER_STORAGE_BEGIN and
// sim_unit_state_group_marshal.cpp's GM_ORDER_ENTER_STORAGE_BEGIN already name from their own
// disassembly -- own local copy per this codebase's established per-TU-constant convention (no shared
// header binds it across TUs).
constexpr uint16_t UNIT_STATE_ENTER_STORAGE_BEGIN = 0x24;

// G_TEXT_PTRS indices can_enter's two reject-message sites read as their "reason" half -- derived from
// the raw fixed addresses (0x00584628-0x0058440c)/4 == 135 and (0x00585094-0x0058440c)/4 == 802. No
// backing Ghidra enum found (rule 17a fallback, same posture as sim_prod_shuttle_complete.h's
// TEXT_ID_PRODUCTION_COMPLETE) -- named descriptively from the CALL SITE's role, not from the string's
// actual English content (this translator has no ReVA/string-dump access); the conductor should
// confirm the wording via get-strings before trusting the name beyond "which message this is".
constexpr int32_t TEXT_ID_STORAGE_TYPE_REJECTED = 135; // 0x0048aa12/0x00584628: "wrong unit type" reason
constexpr int32_t TEXT_ID_STORAGE_CAPACITY_FULL = 802; // 0x0048ac00/0x00585094: "no room" reason

// The two format strings, supplied as our own literals rather than read out of the image (equivalent
// for a format string, and keeps a literal VA out of this TU -- same convention
// sim_order_dispatch_bldg.cpp's TEXT_FMT_NAME_REASON uses). Derived from the Ghidra auto-labels'
// escaped spelling (`u_%s_(%s)_0050149a` / `u_%s_-_(%s)_005014aa`).
constexpr const wchar_t *TEXT_FMT_TYPE_REJECTED = L"%s (%s)";
constexpr const wchar_t *TEXT_FMT_CAPACITY_FULL = L"%s - (%s)";

// can_enter's 0x0048ab7e-0x0048ab98 boundary check, transcribed as a function rather than collapsed
// into a single boolean expression because the FIRST branch is a genuine SHORT-CIRCUIT the assembly
// takes unconditionally before ever comparing `remaining` against `soldier_count` -- collapsing it
// into `remaining < soldier_count` gets the remaining==0-and-soldier_count==0 edge wrong (0>=0 would
// read as "capacity OK", but the original goes straight to the reject/print path regardless). Verified
// against every reachable (remaining, soldier_count) sign combination; see the .h banner's step 6.
//   0x0048ab7e/0x0048ab82/0x0048ab84/0x0048ab88: if (remaining <= 0 && soldier_count == 0) -> reject.
//   0x0048ab8a/0x0048ab90: else if (remaining >= soldier_count) -> accept (0x0048ac30).
//   0x0048ab92/0x0048ab96: else if (soldier_count > 0) -> reject (0x0048ab9d).
//   0x0048ab98 (fallthrough): else -> accept.
bool storage_full_should_print(int32_t remaining, int32_t soldier_count) {
    if (remaining <= 0 && soldier_count == 0) return true;
    if (remaining >= soldier_count) return false;
    if (soldier_count > 0) return true;
    return false;
}

} // namespace

namespace detail {

int32_t storage_can_exit(const sim_view &v, const storage_can_exit_calls &c, int32_t player,
                         uint32_t unit_index, int32_t storage_slot) {
    // reimpl-verify (2026-08-21): the ORIGINAL reloads unit_index/storage_slot via a 16-bit MOVZX at
    // every one of their many uses (never a plain 32-bit reload) -- narrow both ONCE here, matching
    // `p` below, rather than forwarding the raw params (a caller value with garbage above bit 15
    // would otherwise index far outside units[]/storage[] where the original stays in-bounds).
    const uint16_t      p         = static_cast<uint16_t>(player);
    const int32_t       ui        = static_cast<int32_t>(static_cast<uint16_t>(unit_index));
    const int32_t       slot      = static_cast<int32_t>(static_cast<uint16_t>(storage_slot));
    const unit_storage &st        = storage_of(v, p, slot);              // 0x0048a567-0x0048a58b
    const building     &b         = building_of(v, p, st.b_index);       // 0x0048a587-0x0048a591
    const uint8_t       bldg_type = v.cfg_buildings[b.building_id].type; // 0x0048a593-0x0048a5a6
    const uint16_t      proto     = unit_of(v, p, ui).unit_proto_id;

    // 0x0048a5a9-0x0048a6b6: which of three gates applies -- re-derived from the CMP/JC/JBE/JZ chain,
    // not assumed as a clean switch (see the .h banner).
    bool gate1; // [EBP-0x10] in the original
    bool gate2; // [EBP-0x14] in the original -- only ever true for the shuttle case

    if (bldg_type == BUILDING_TYPE_A_PORT) {
        // 0x0048a66a-0x0048a6aa.
        gate1 = (b.online_state == 1);
        gate2 = false;
    } else if (bldg_type == BUILDING_TYPE_A_SHUTTLE || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        // 0x0048a5cf-0x0048a668: the shuttle housing-capacity gate.
        gate1 = true;
        const int32_t reserved =
            v.prod_shuttle_slots[p * PROD_SHUTTLE_SLOTS_PER_PLAYER + b.shuttle_slot].passengers_reserved;
        const int32_t housing_used = reserved + v.population[p].human;
        gate2                      = housing_used >= v.cfg_units[proto].human;
    } else {
        // 0x0048a6b6: every other building type -- always allowed past this gate.
        gate1 = true;
        gate2 = false;
    }

    // 0x0048a6be-0x0048a6f9: a blocked exit tile forces the blockage resolver, but ONLY for a
    // ground-class unit (cfg_units[proto].type <= UNIT_TYPE_A_GROUND, the same 0xe boundary
    // sim_unit_state_exit.h's CORRECTION note documents) -- an aircraft-class unit on a blocked tile
    // falls through to the ordinary gates below, same as a clear tile.
    const uint32_t tile_clear = c.storage_exit_tile_is_clear(p, slot);
    if (tile_clear == 0 && v.cfg_units[proto].type <= UNIT_TYPE_A_GROUND) {
        // 0x0048a7e9-0x0048a7f6: resolve the blockage as a SIDE EFFECT and return false
        // UNCONDITIONALLY -- the callee's own return value is discarded, only its effect matters here.
        c.storage_resolve_exit_blockage(static_cast<uint32_t>(player), slot);
        return 0;
    }

    // 0x0048a6ff-0x0048a7c5: door-mutex / housing-shortfall / built-flags gate chain.
    bool proceed;
    if (st.door_mutex_unit != 0) {
        proceed = false; // 0x0048a71c/0x0048a790
    } else if (v.population[p].human >= v.cfg_units[proto].human) {
        proceed = true; // 0x0048a758/0x0048a786
    } else if (v.cfg_units[proto].soldier_count > 0) {
        proceed = true; // 0x0048a784 (JLE not taken) -> 0x0048a786
    } else {
        proceed = gate2; // 0x0048a788
    }
    if (!proceed) return 0;                                 // 0x0048a790/0x0048a7c7/0x0048a7cf
    if (b.built_flags != BUILT_FLAGS_OPERATIONAL) return 0; // 0x0048a7be-0x0048a7c9
    return gate1 ? 1 : 0;                                   // 0x0048a7c9-0x0048a7e1
}

int32_t storage_can_enter(const sim_view &v, sim_store &own, const storage_can_enter_calls &c,
                          uint16_t player, uint32_t unit_index, uint32_t storage_slot) {
    // reimpl-verify (2026-08-21): the ORIGINAL reloads both via a 16-bit MOVZX at every one of ~29
    // uses (never a plain 32-bit reload) -- narrow both ONCE here, same reasoning as storage_can_exit.
    const int32_t slot = static_cast<int32_t>(static_cast<uint16_t>(storage_slot));
    const int32_t uidx = static_cast<int32_t>(static_cast<uint16_t>(unit_index));

    const unit_storage &st0       = storage_of(v, player, slot);          // 0x0048a827-0x0048a83d
    const building     &b0        = building_of(v, player, st0.b_index);  // 0x0048a847-0x0048a851
    const uint8_t       bldg_type = v.cfg_buildings[b0.building_id].type; // 0x0048a853-0x0048a860

    if (bldg_type == BUILDING_TYPE_A_SHUTTLE || bldg_type == BUILDING_TYPE_H_SHUTTLE) {
        // 0x0048a8af-0x0048ac96: the shuttle-only per-soldier-slot capacity gate. Reached ONLY for
        // these two building types -- every other bldg_type skips straight to the common final gate.
        const uint16_t  proto = unit_of(v, player, uidx).unit_proto_id;
        const cfg_unit &cu    = v.cfg_units[proto];

        // 0x0048a8db-0x0048a93a: the SAME crew_offset idiom sim_storage_type_accepts_unit.cpp already
        // documents for its own identical Building[].unit_capacity[100] lookup.
        const int32_t crew_offset   = (cu.soldier_count == 0) ? 0 : (cu.soldier_count - 1);
        const int32_t capacity_slot = static_cast<int32_t>(proto) - crew_offset; // 0x0048a999-0x0048a99c
        int32_t       remaining =
            v.cfg_buildings[b0.building_id].unit_capacity[capacity_slot]; // 0x0048a96f-0x0048a9a5, byte,
                                                                          // zero-extended, UNCHECKED index

        if (remaining == 0) {
            // 0x0048a9b2-0x0048aa36: reject -- optionally warn the local player.
            if (player == static_cast<uint16_t>(*v.player_side) &&
                unit_of(v, player, uidx).state == UNIT_STATE_ENTER_STORAGE_BEGIN) {
                // reimpl-verify (2026-08-21): cdecl PUSH order in the original (name, then reason,
                // then format, then dst) means the REAL call is w_sprintf(dst, format, reason, name)
                // -- reason is the FIRST %s, name the SECOND. Was swapped here.
                c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_TYPE_REJECTED,
                                 v.text_ptrs[TEXT_ID_STORAGE_TYPE_REJECTED], v.text_ptrs[cu.name]);
                c.game_ui_PrintTextMessage(own.text_scratch());
            }
            return 0;
        }

        // 0x0048aa42-0x0048ab7e: walk docked_units[], subtracting overlapping capacity usage.
        for (int32_t i = 0; i < st0.docked_count; ++i) {
            const int32_t docked_unit = st0.docked_units[i];
            if (cu.soldier_count != 0) {
                // 0x0048aa81-0x0048ab17: same soldier_type as the entering unit -> subtract the
                // docked unit's own soldier_count.
                const uint16_t docked_proto = unit_of(v, player, docked_unit).unit_proto_id;
                if (v.cfg_units[docked_proto].soldier_type == cu.soldier_type) {
                    remaining -= v.cfg_units[docked_proto].soldier_count;
                }
            } else {
                // 0x0048ab19-0x0048ab79: same proto as the entering unit -> subtract exactly one slot.
                const uint16_t docked_proto = unit_of(v, player, docked_unit).unit_proto_id;
                if (proto == docked_proto) {
                    remaining -= 1;
                }
            }
        }

        // 0x0048ab7e-0x0048ac24: reject if the remaining capacity does not cover this entry, with the
        // exact short-circuit priority storage_full_should_print's own derivation comment documents.
        if (storage_full_should_print(remaining, cu.soldier_count)) {
            if (player == static_cast<uint16_t>(*v.player_side) &&
                unit_of(v, player, uidx).state == UNIT_STATE_ENTER_STORAGE_BEGIN) {
                // 0x0048abe1-0x0048abe8: the ADJUSTED capacity_slot index, not the raw proto -- see the
                // .h banner's step 6. reimpl-verify (2026-08-21): same PUSH-order swap as the
                // type-rejected message above -- the real call is w_sprintf(dst, format, reason, name).
                c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_CAPACITY_FULL,
                                 v.text_ptrs[TEXT_ID_STORAGE_CAPACITY_FULL],
                                 v.text_ptrs[v.cfg_units[capacity_slot].name]);
                c.game_ui_PrintTextMessage(own.text_scratch());
            }
            return 0;
        }

        // 0x0048ac30-0x0048ac96: capacity OK -- if this shuttle building has no slot bound yet, bind
        // one; a failed bind (-1) rejects the whole entry attempt.
        if (b0.shuttle_slot == 0) {
            const int32_t bind_result = c.shuttle_slot_bind_default(player, st0.b_index);
            if (bind_result == -1) return 0;
        }
        // else: fall through to the common final gate below.
    }

    // 0x0048ac9b-0x0048ae85: the common final gate -- reached directly for every non-shuttle storage
    // type, and for a shuttle type only after the capacity/bind checks above pass.
    const uint16_t      proto2 = unit_of(v, player, uidx).unit_proto_id;
    const unit_storage &st2    = storage_of(v, player, slot);
    const building     &b2     = building_of(v, player, st2.b_index);

    if (v.cfg_units[proto2].soldier_count > 0) {
        // 0x0048acbe-0x0048ae85, soldier_count>0 arm: occupancy+human vs the cap, online_state==2,
        // built_flags==3.
        if (st2.door_mutex_unit != 0) return 0;                                // 0x0048ace8
        if (st2.occupancy + v.cfg_units[proto2].human > STORAGE_OCCUPANCY_CAP) // 0x0048ad2f
            return 0;
        if (b2.online_state != 2) return 0;                         // 0x0048ad6a
        return (b2.built_flags == BUILT_FLAGS_OPERATIONAL) ? 1 : 0; // 0x0048ada1
    }
    // 0x0048adc0-0x0048ae85, soldier_count<=0 arm: occupancy vs the cap (strict, no `.human` added),
    // same online_state/built_flags gate.
    if (st2.door_mutex_unit != 0) return 0;                     // 0x0048addd
    if (st2.occupancy >= STORAGE_OCCUPANCY_CAP) return 0;       // 0x0048adfc
    if (b2.online_state != 2) return 0;                         // 0x0048ae34
    return (b2.built_flags == BUILT_FLAGS_OPERATIONAL) ? 1 : 0; // 0x0048ae6b
}

int32_t storage_can_land(const sim_view &v, int32_t player, uint32_t unit_index, int32_t storage_slot) {
    // unit_index is part of the committed prototype but genuinely unread in this function -- see the
    // .h banner. Named (not dropped) for prototype fidelity; explicitly discarded here.
    (void)unit_index;

    // reimpl-verify (2026-08-21): the ORIGINAL reloads storage_slot via a 16-bit MOVZX at every one
    // of its 5 uses (never a plain 32-bit reload) -- narrow ONCE here, same reasoning as `p` below.
    const uint16_t      p         = static_cast<uint16_t>(player);
    const int32_t       slot      = static_cast<int32_t>(static_cast<uint16_t>(storage_slot));
    const unit_storage &st        = storage_of(v, p, slot);              // 0x0048aeaf-0x0048aec5
    const building     &b         = building_of(v, p, st.b_index);       // 0x0048aecf-0x0048aed9
    const uint8_t       bldg_type = v.cfg_buildings[b.building_id].type; // 0x0048aedb-0x0048aeee

    bool gate1;
    if (bldg_type != BUILDING_TYPE_A_PORT) {
        gate1 = true; // 0x0048af79
    } else {
        // 0x0048aefb-0x0048af71: A_PORT -- the connected-flag word must be 1 (docking) OR 2 (loading).
        gate1 = (b.online_state == 1) || (b.online_state == 2);
    }

    if (st.door_mutex_unit != 0) return 0;                  // 0x0048af9a
    if (st.occupancy >= STORAGE_OCCUPANCY_CAP) return 0;    // 0x0048afb9
    if (!gate1) return 0;                                   // 0x0048afc1
    if (b.built_flags != BUILT_FLAGS_OPERATIONAL) return 0; // 0x0048aff8
    return 1;                                               // 0x0048affc
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t storage_can_exit(int32_t player, uint32_t unit_index, int32_t storage_slot) {
    const sim_view v = state().read;
    return detail::storage_can_exit(v, live_storage_can_exit_calls(), player, unit_index, storage_slot);
}

int32_t storage_can_enter(uint16_t player, uint32_t unit_index, uint32_t storage_slot) {
    sim_state st = state();
    return detail::storage_can_enter(st.read, st.own, live_storage_can_enter_calls(), player, unit_index,
                                     storage_slot);
}

int32_t storage_can_land(int32_t player, uint32_t unit_index, int32_t storage_slot) {
    const sim_view v = state().read;
    return detail::storage_can_land(v, player, unit_index, storage_slot);
}


} // namespace mh::sim
