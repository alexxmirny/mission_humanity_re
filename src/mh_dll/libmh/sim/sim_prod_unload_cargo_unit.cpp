#include "sim/sim_prod_unload_cargo_unit.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_unload_cargo_unit_calls &live_prod_unload_cargo_unit_calls() {
    static const prod_unload_cargo_unit_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_spawn_docked),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
        MH_LIBMH_BIND(llm_strat_unit_ctrlgroup_add_member),
        MH_LIBMH_BIND(game_SetEvent),
    };
    return c;
}

namespace {

// units[player][0].order per-player HEADER-ROW increment (0x0048ead0, `INC word ptr`) / decrement
// (0x0048ec38, `DEC word ptr`) -- see the header's own derivation. Named locally rather than shared
// or folded into sim_order_enqueue.h's UNIT_STATE_STOP_TO_DEFAULT, matching every sibling occurrence's
// caution that the numeric coincidence (both are 1) is not proof of shared enum semantics.
inline constexpr uint16_t kUnitZeroOrderHeaderIncrement = 1;

// One 14-byte cargo-manifest entry's field offsets within cargo_manifest_raw -- see the header's
// MANIFEST ENTRY LAYOUT note for the derivation of each.
inline constexpr uint32_t kCargoEntryStride           = 0xe;
inline constexpr uint32_t kCargoEntryProtoIdOffset    = 0x0; // uint16_t, bits 12-15 = ctrl group
inline constexpr uint32_t kCargoEntryEnergyOffset     = 0x2; // double
inline constexpr uint32_t kCargoEntryExperienceOffset = 0xa; // int32_t

} // namespace

namespace detail {

uint32_t prod_unload_cargo_unit(const sim_view &v, sim_store &own,
                                const prod_unload_cargo_unit_calls &c, uint16_t player,
                                int32_t building_index, uint32_t cargo_index) {
    // 0x0048ea46-0x0048ea7d: shuttle_slot/sub_id, both read off the BUILDING record.
    const building &b            = building_of(v, player, building_index);
    const uint32_t  shuttle_slot = b.shuttle_slot;
    const uint32_t  sub_id       = b.sub_id;

    // cargo_index (`a2`) is used ONLY through its low 16 bits throughout the original (every
    // reference is a `MOVZX ..,word ptr` off the stack slot) -- see the header's note.
    const uint32_t entry_off = (cargo_index & 0xffffu) * kCargoEntryStride;

    prod_shuttle_slot &slot = own.prod_shuttle_slot_at(player, (int32_t)shuttle_slot);

    // 0x0048ea9c-0x0048eaa6: ctrl_group_index = the packed word's high nibble (bits 12-15).
    uint16_t proto_id_raw;
    std::memcpy(&proto_id_raw, &slot.cargo_manifest_raw[entry_off + kCargoEntryProtoIdOffset],
                sizeof(proto_id_raw));
    const int32_t ctrl_group_index = (int32_t)(proto_id_raw >> 12);

    // 0x0048eac5: mask the control-group nibble OUT of the STORED word (a single `AND byte,0xf` on
    // the word's high byte) -- leaves the bare proto id (bits 0-11) behind for the spawn call below.
    slot.cargo_manifest_raw[entry_off + kCargoEntryProtoIdOffset + 1] &= 0xfu;

    // 0x0048ead0-0x0048ead6: the roster header-row increment, UNCONDITIONAL, BEFORE the spawn
    // attempt -- targets units[player][0], NOT units[player][unit_id]. See the header hazard note.
    unit &roster_header = own.unit_at(player, 0);
    roster_header.order = (uint16_t)(roster_header.order + kUnitZeroOrderHeaderIncrement);

    // 0x0048eb00: re-read the (now-masked) proto id for the spawn call -- the original re-reads
    // memory here rather than reusing a register, so a second memcpy off the just-masked bytes is
    // behaviourally identical.
    uint16_t masked_proto_id;
    std::memcpy(&masked_proto_id, &slot.cargo_manifest_raw[entry_off + kCargoEntryProtoIdOffset],
                sizeof(masked_proto_id));

    // 0x0048eb07: llm_strat_unit_spawn_docked(proto_id, player, sub_id) (ORIGINAL, via `c`).
    const int32_t unit_id = c.unit_spawn_docked(masked_proto_id, player, sub_id);

    if (unit_id == 0) {
        // 0x0048ec2e-0x0048ec3f: spawn failed -- undo the header increment (same record; no rebase
        // can occur between the two accessor calls within this function), return 1 (failure).
        roster_header.order = (uint16_t)(roster_header.order - kUnitZeroOrderHeaderIncrement);
        return 1;
    }

    // 0x0048eb19-0x0048eb48: llm_strat_ai_notify_unit_lifecycle(player, unit_type, unit_id, kind=4)
    // (ORIGINAL, via `c`). unit_type is the masked proto id, re-read off the manifest entry again
    // (same value; the original performs a fresh memory load here too).
    uint16_t unit_type;
    std::memcpy(&unit_type, &slot.cargo_manifest_raw[entry_off + kCargoEntryProtoIdOffset],
                sizeof(unit_type));
    c.ai_notify_unit_lifecycle(player, unit_type, (uint32_t)unit_id, 4u);

    // 0x0048eb7c-0x0048eb82: restore energy from entry+0x2 (double) -- a raw 8-byte copy, not an FP
    // computation (see the header's note on why the x87/SSE2 landmine does not apply here).
    double energy;
    std::memcpy(&energy, &slot.cargo_manifest_raw[entry_off + kCargoEntryEnergyOffset],
                sizeof(energy));
    own.unit_at(player, unit_id).energy = energy;

    // 0x0048ebb7-0x0048ebbd: restore experience from entry+0xa (int32_t) -- CORRECTED entry-layout
    // field, confirmed independently via struct-offset arithmetic (see the header's MANIFEST ENTRY
    // LAYOUT note).
    int32_t experience;
    std::memcpy(&experience, &slot.cargo_manifest_raw[entry_off + kCargoEntryExperienceOffset],
                sizeof(experience));
    own.unit_at(player, unit_id).experience = experience;

    // 0x0048ebdf: clear the manifest entry's leading word to 0 -- ONE `MOV word ptr ..,0` in the
    // asm (0x66 operand-size prefix), i.e. a single 2-byte store, not two independent byte writes.
    const uint16_t zero16 = 0;
    std::memcpy(&slot.cargo_manifest_raw[entry_off + kCargoEntryProtoIdOffset], &zero16,
                sizeof(zero16));

    if (ctrl_group_index != 0) {
        // 0x0048ebee-0x0048ec02: llm_strat_unit_ctrlgroup_add_member(unit_id,
        // &_G_LLM_STRAT_CTRL_GROUPS[ctrl_group_index].count, ctrl_group_index) (ORIGINAL, via `c`) --
        // address escape to the group record's leading `.count` field, same shape sim_state.h's own
        // comment on ctrl_group_at() documents for llm_strat_unit_ctrl_group_assign's identical use.
        c.unit_ctrlgroup_add_member((int32_t)unit_id, &own.ctrl_group_at(ctrl_group_index).count,
                                    ctrl_group_index);
    }

    // 0x0048ec07-0x0048ec20: three UI-refresh event pushes, unconditional on the success path.
    c.set_event(PROD_UNLOAD_CARGO_UNIT_BUILD_PROJECTS_REFRESH);
    c.set_event(PROD_UNLOAD_CARGO_UNIT_MAP_OBJECTS_REFRESH);
    c.set_event(EVENT_INFO_REFRESH);

    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t prod_unload_cargo_unit(uint16_t player, int32_t building_index, uint32_t cargo_index) {
    sim_state st = state();
    return detail::prod_unload_cargo_unit(st.read, st.own, live_prod_unload_cargo_unit_calls(), player,
                                          building_index, cargo_index);
}


} // namespace mh::sim
