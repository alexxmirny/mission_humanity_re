//
// sim/sim_unit_load_into_shuttle_cargo.cpp -- see sim_unit_load_into_shuttle_cargo.h. Translated
// from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_load_into_shuttle_cargo_0048e7c5.asm), not from the Ghidra .c draft.
//
#include "sim/sim_unit_load_into_shuttle_cargo.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_load_into_shuttle_cargo_calls &live_unit_load_into_shuttle_cargo_calls() {
    static const unit_load_into_shuttle_cargo_calls c = {
        MH_LIBMH_BIND(llm_unit_state_is_boarding),
        MH_LIBMH_BIND(llm_strat_storage_remove_docked_unit),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
    };
    return c;
}

namespace {

// ---- the cargo-manifest entry accessors -------------------------------------------------------
//
// mh_llm_prod_shuttle_slot::cargo_manifest_raw is a flat `uint8_t[696]` (Ghidra has not typed the
// per-entry layout) -- same memcpy-through-a-flattened-byte-array shape
// sim_bldg_reset_construction_anim.cpp's frame_at()/set_frame_at() establish for cfg_t_frame_index[12],
// narrowly scoped to this TU per that file's own precedent.
//
// UNCHECKED BY DESIGN, matching the original instruction sequence exactly: the header banner's
// out-of-bounds-hazard note documents that entry 49's `experience` field write reaches 4 bytes past
// cargo_manifest_raw's declared 696-byte extent whenever `entry == CARGO_MANIFEST_SLOTS - 1`. This
// function does not special-case that entry -- the original doesn't either -- so the pointer
// arithmetic here is deliberately a plain, unconditional `base + entry*stride + field_offset` with no
// bounds check, exactly like the asm's own address computation. See the header banner for the full
// derivation and the declared need this leaves for the conductor (confirm the offline fixture's
// RID_PROD_SHUTTLE_SLOTS buffer has >=4 bytes of trailing slack before exercising this corner).
uint8_t *cargo_entry_ptr(prod_shuttle_slot &slot, int32_t entry, int32_t field_offset) {
    return slot.cargo_manifest_raw + entry * CARGO_ENTRY_STRIDE + field_offset;
}

uint16_t cargo_entry_header(prod_shuttle_slot &slot, int32_t entry) {
    uint16_t value;
    std::memcpy(&value, cargo_entry_ptr(slot, entry, 0x0), sizeof(value));
    return value;
}
void set_cargo_entry_header(prod_shuttle_slot &slot, int32_t entry, uint16_t value) {
    std::memcpy(cargo_entry_ptr(slot, entry, 0x0), &value, sizeof(value));
}
void set_cargo_entry_energy(prod_shuttle_slot &slot, int32_t entry, double value) {
    std::memcpy(cargo_entry_ptr(slot, entry, 0x2), &value, sizeof(value));
}
void set_cargo_entry_experience(prod_shuttle_slot &slot, int32_t entry, int32_t value) {
    std::memcpy(cargo_entry_ptr(slot, entry, 0xa), &value, sizeof(value));
}

} // namespace

namespace detail {

int32_t unit_load_into_shuttle_cargo(const sim_view &v, sim_store &own,
                                     const unit_load_into_shuttle_cargo_calls &c, uint16_t player,
                                     int32_t building_idx, uint16_t unit_idx) {
    // 0x0048e7e4-0x0048e7fe: shuttle_slot = buildings[player][building_idx].shuttle_slot (byte, no
    // "unbound" gate here -- unlike sim_bldg_shuttle_slot_is_free.cpp, this function indexes whatever
    // value the field holds, including 0).
    const uint32_t shuttle_slot =
        building_of(v, static_cast<uint32_t>(player), building_idx).shuttle_slot;

    // 0x0048e801-0x0048e81e: proto_id = units[player][unit_idx].unit_proto_id (word), read ONCE here
    // and reused below for both the manifest header write and the cfg Unit[] human/soldier_count
    // lookup -- the asm re-reads the SAME field a second time (0x0048e8aa-0x0048e8c0) with no write in
    // between, collapsed to this one local per the header banner's note.
    const uint16_t proto_id =
        unit_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(unit_idx)).unit_proto_id;

    // 0x0048e821-0x0048e871: descending scan for the LOWEST free manifest slot -- see header banner.
    // ONE fetch of the slot record, reused for the scan and every write below (task brief hazard note:
    // never re-resolve a mutable accessor mid-function).
    prod_shuttle_slot &slot       = own.prod_shuttle_slot_at(static_cast<uint32_t>(player),
                                                             static_cast<int32_t>(shuttle_slot));
    int32_t            found_slot = -1;
    for (int32_t i = CARGO_MANIFEST_SLOTS - 1; i >= 0; --i) {
        if (cargo_entry_header(slot, i) == 0) found_slot = i;
    }

    // 0x0048e871-0x0048e87d / 0x0048e8a1-0x0048e8a5: no free slot -> no-op. (The asm's second half of
    // this gate, `[EBP-0x34] > 0`, is a dead tautology -- see header banner; omitted here.)
    if (found_slot < 0) return 0;

    // 0x0048e87f-0x0048e8a3: the unit must be in a boarding-related state, or this is a no-op too.
    const unit &u = unit_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(unit_idx));
    if (c.unit_state_is_boarding(static_cast<int32_t>(u.state)) == 0) return 0;

    // 0x0048e8aa-0x0048e8f2: result = (Unit[proto_id].soldier_count < 1) ? Unit[proto_id].human : 0.
    const cfg_unit &cu     = v.cfg_units[proto_id];
    const int32_t   result = (cu.soldier_count < 1) ? cu.human : 0;

    // 0x0048e8f2-0x0048e938: manifest header = proto_id | (ctrl_group_id << 12), truncated to 16 bits
    // by the store width (see header banner on why the OR's garbage upper bits are harmless here).
    const uint16_t header =
        static_cast<uint16_t>(proto_id | (static_cast<uint32_t>(u.ctrl_group_id) << 12));
    set_cargo_entry_header(slot, found_slot, header);

    // 0x0048e961-0x0048e96d: manifest energy = units[player][unit_idx].energy (plain double copy).
    set_cargo_entry_energy(slot, found_slot, u.energy);

    // 0x0048e99c-0x0048e9a8: manifest experience = units[player][unit_idx].experience (int32 copy).
    // This is the write that can land 4 bytes past cargo_manifest_raw's declared bound when
    // found_slot == CARGO_MANIFEST_SLOTS - 1 -- see header banner.
    set_cargo_entry_experience(slot, found_slot, u.experience);

    // 0x0048e9c4-0x0048e9d3: remove the unit from its home storage slot. `u.home_storage_slot` is read
    // BEFORE either callee runs (no write to `u` has happened yet at this point).
    c.storage_remove_docked_unit(player, static_cast<int32_t>(unit_idx),
                                 static_cast<int32_t>(u.home_storage_slot));

    // 0x0048e9d8-0x0048e9e0: tear the unit off the map.
    c.unit_teardown(static_cast<uint32_t>(player), unit_idx);

    // 0x0048e9e5-0x0048ea07: activity_clock += shuttle_board_activity_bump. The asm RE-COMPUTES the
    // unit's address here, after both callees -- re-fetched via own.unit_at() (not the earlier `u`
    // read view reference) so this reads/writes whatever either callee may have left in the record,
    // exactly matching that re-fetch.
    own.unit_at(static_cast<uint32_t>(player), static_cast<int32_t>(unit_idx)).activity_clock +=
        *v.shuttle_board_activity_bump;

    return result;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_load_into_shuttle_cargo(uint16_t player, int32_t building_idx, uint16_t unit_idx) {
    sim_state st = state();
    return detail::unit_load_into_shuttle_cargo(st.read, st.own, live_unit_load_into_shuttle_cargo_calls(),
                                                player, building_idx, unit_idx);
}


} // namespace mh::sim
