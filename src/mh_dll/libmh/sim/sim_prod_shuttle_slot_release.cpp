//
// sim/sim_prod_shuttle_slot_release.cpp -- see sim_prod_shuttle_slot_release.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_prod_shuttle_slot_release_0046318d.asm), not from the Ghidra
// .c draft (used here as CONFIRMED, per the header banner).
//
#include "sim/sim_prod_shuttle_slot_release.h"

#include <cstring>

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// The manifest entry's stride, matching sim_prod_unload_cargo_unit.cpp's identically-named local
// constant for the SAME region (own copy per this TU's "write only your own new files" rule -- not
// shared, per translator-brief rule 4).
inline constexpr uint32_t kCargoEntryStride = 0xe;

} // namespace

namespace detail {

void prod_shuttle_slot_release(sim_store &own, int32_t player, int32_t slot) {
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(static_cast<uint32_t>(player), slot);

    // 0x004631ba-0x0046326b: the eight scalar identity/state fields, all unconditional word/dword
    // zero stores, in the assembly's own store order (see the header banner for the address/offset
    // citation of each).
    s.type_ref_id            = 0;
    s.src_building_type      = 0;
    s.src_building_index     = 0;
    s.status                 = 0;
    s.origin_planet          = 0;
    s.is_planet_bound        = 0;
    s.is_heli_mother_pending = 0;
    s.dest_planet            = 0;

    // 0x00463284/0x0046328e: travel_duration's two dword halves, zeroed as one bit-pattern-zero
    // double assignment -- see the header banner's note on why this is not an x87/SSE2-sensitive FP
    // operation (no FLD/FST/FCOM/FADD anywhere in the body; both original stores are plain integer
    // MOVs). travel_duration_copy (offset 0x28) is DELIBERATELY left untouched -- see the header
    // banner.
    s.travel_duration = 0.0;

    // 0x004632a8: passengers_reserved.
    s.passengers_reserved = 0;

    // 0x004632b9-0x004632ea: resources_reserved[0..9], stride 4 -- matches the field's own
    // int32_t[10] declaration exactly.
    for (int32_t i = 0; i < 10; ++i) s.resources_reserved[i] = 0;

    // 0x004632ea-0x0046331f: the leading uint16_t of each of the first 50 conceptual 14-byte
    // cargo_manifest_raw entries, one 2-byte MOV per iteration in the assembly (see the header
    // banner's note on the Ghidra .c draft's equivalent-but-differently-shaped two-byte rendering).
    // A memcpy of a zero uint16_t reproduces the single word store exactly, matching
    // sim_prod_unload_cargo_unit.cpp's own zero-entry idiom for the same field.
    const uint16_t zero16 = 0;
    for (int32_t i = 0; i < 0x32; ++i) {
        std::memcpy(&s.cargo_manifest_raw[static_cast<uint32_t>(i) * kCargoEntryStride], &zero16,
                    sizeof(zero16));
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void prod_shuttle_slot_release(int32_t player, int32_t slot) {
    sim_state st = state();
    detail::prod_shuttle_slot_release(st.own, player, slot);
}


} // namespace mh::sim
