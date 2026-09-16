//
// sim/sim_unit_bldg_apply_scaled_damage.cpp -- see sim_unit_bldg_apply_scaled_damage.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_unit_bldg_apply_scaled_damage_0049a9aa.asm), not from the
// Ghidra .c draft (whose shape and corrected plate both check out, but is cited only as
// corroboration -- see the header for the re-derivation).
//
#include "sim/sim_unit_bldg_apply_scaled_damage.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// DAT_00501740 -- read-only image `double` constant at 0x00501740, the UNIT arm's MULTIPLIER
// (`Unit[proto].energy * DAT_00501740`). RESOLVED by the conductor via ReVA read-memory: raw bytes
// `00 00 00 00 00 00 E0 3F` = IEEE754 double 0.5 exactly.
inline constexpr double UNIT_DAMAGE_SCALE = 0.5; // DAT_00501740

// DAT_00501748 -- read-only image `double` constant at 0x00501748, the BUILDING arm's DIVISOR
// (`Building[bid].energy / DAT_00501748`). A DIFFERENT constant from DAT_00501740 above, used with a
// DIFFERENT operator (FDIV, not FMUL) -- see the header's CRITICAL ASYMMETRY note. RESOLVED by the
// conductor via ReVA read-memory: raw bytes `00 00 00 00 00 00 08 40` = IEEE754 double 3.0 exactly --
// confirms the asymmetry (unit damage = half max energy; building damage = a third of max energy).
inline constexpr double BLDG_DAMAGE_SCALE = 3.0; // DAT_00501748

} // namespace

namespace detail {

void unit_bldg_apply_scaled_damage(const sim_view &v, sim_store &own, uint32_t target_selector,
                                   int32_t target_index) {
    // 0x0049a9c7-0x0049a9cd: player = target_selector & 0xf, computed ONCE and reused, masked, in
    // BOTH arms below -- neither arm ever indexes with the raw target_selector value.
    const uint32_t player = target_selector & 0xfu;

    // 0x0049a9d0-0x0049a9d7: TEST target_selector,0x80; JZ building_label. Bit 0x80 CLEAR -> building
    // arm; bit 0x80 SET -> unit arm (falls straight through from entry).
    if ((target_selector & 0x80u) != 0u) {
        // ---- unit arm (0x0049a9d9-0x0049aa1e) ---------------------------------------------------
        unit &u = own.unit_at(player, target_index);

        // 0x0049a9f9-0x0049a9ff: units[player][target_index].unit_proto_id selects the cfg row;
        // 0x0049aa06-0x0049aa18: ONE FLD/FMUL/FADD/FSTP chain, no intermediate 64-bit store --
        // ordinary C++ double arithmetic reproduces it exactly under this TU's /arch:IA32 /fp:precise
        // build (no trunc/rounding call is sandwiched into this chain, unlike the debris-intensity/
        // pip-count/refund siblings, so no inline __asm is needed here).
        u.pending_damage =
            v.cfg_units[u.unit_proto_id].energy * UNIT_DAMAGE_SCALE + u.pending_damage;
    } else {
        // ---- building arm (0x0049aa20-0x0049aa65) -----------------------------------------------
        building &b = own.building_at(player, target_index);

        // 0x0049aa40-0x0049aa46: buildings[player][target_index].building_id selects the cfg row;
        // 0x0049aa4d-0x0049aa5f: ONE FLD/FDIV/FADD/FSTP chain, same "no intermediate store" reasoning
        // as the unit arm -- DIVISION, not multiplication, and a DIFFERENT constant (see the header's
        // CRITICAL ASYMMETRY note).
        b.pending_damage =
            v.cfg_buildings[b.building_id].energy / BLDG_DAMAGE_SCALE + b.pending_damage;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_bldg_apply_scaled_damage(uint32_t target_selector, int32_t target_index) {
    sim_state st = state();
    detail::unit_bldg_apply_scaled_damage(st.read, st.own, target_selector, target_index);
}


} // namespace mh::sim
