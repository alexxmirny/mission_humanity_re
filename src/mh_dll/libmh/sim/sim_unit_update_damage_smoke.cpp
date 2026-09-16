#include "sim/sim_unit_update_damage_smoke.h"

#include "ai/ai_state.h"   // ai_say / trace_budget -- the shared trace sink, not AI state
#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::sim {

namespace {

// utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) -- see the header
// banner. This function's ONE call site (0x0048718d) builds `((max_energy - cur_energy) /
// max_energy) * scale` entirely on the x87 stack (subtract, THEN divide, THEN multiply -- order
// matters for the 80-bit intermediate) before handing ST0 to the trunc sequence, matching
// sim_projectile_tick.cpp's trunc_scaled_int()/trunc_mul_div() precedent for this identical callee.
int32_t trunc_hp_loss_level(double max_energy, double cur_energy, double scale) {
    return ::mh::fp::trunc_hp_loss_level(max_energy, cur_energy, scale);
}

} // namespace

namespace detail {

void unit_update_damage_smoke(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_idx) {
    // Every reload of `player` inside the original body is a MOVZX of the WORD half only (see the
    // header banner); `unit_idx` is always reloaded as a full dword. Applied once here rather than
    // re-derived at each access -- see the header banner for why that is observationally identical.
    const uint16_t p = static_cast<uint16_t>(player);

    // ONE accessor for every read below AND the writes further down: the asm re-derives this exact
    // record's address (player*0x5b04 + unit_idx*0xe9) TEN separate times, and nothing writes any
    // OTHER record in between -- same "collapse a provably-identical re-derivation" reasoning
    // sim_bldg_staffed_flag.cpp's set_staffed_flag() gives for its own single accessor call.
    unit &mu = own.unit_at(p, unit_idx);

    // 0x00487108-0x0048711c: gate on the unit's cfg TYPE having the smoke effect enabled at all.
    const cfg_unit &cu = v.cfg_units[mu.unit_proto_id];
    if (cu.dmg_smoke_enabled == 0) return;

    // 0x00487155-0x00487192: trunc(((max_energy - energy) / max_energy) * DAT_0050147a). DECLARED
    // NEED: v.dmg_smoke_level_scale does not exist in sim_view yet -- see the header banner.
    const int32_t computed_level = trunc_hp_loss_level(cu.energy, mu.energy, *v.dmg_smoke_level_scale);

    // 0x004871ab/0x004871b1: no level change -> nothing else runs.
    if (computed_level == mu.dmg_smoke_level) return;

    mu.dmg_smoke_level = computed_level; // 0x004871cd

    // 0x004871e6/0x004871ed: re-reads the SAME field this store just wrote, so testing
    // `computed_level == 0` here is provably identical to the original's re-read -- no state changed
    // in between (same non-issue set_staffed_flag()'s banner documents for its own re-derivation).
    if (computed_level == 0) return;

    // 0x00487202-0x00487224: A_DYM_POJAZD[level-1] -- see sim_view::a_dym_pojazd's own comment
    // (sim_state.h) and this file's header banner for the POP_STATS-adjacency trap this address
    // arithmetic looks like but is not. Guarded by the `computed_level == 0` return above, so
    // `level - 1` never goes negative.
    mu.dmg_smoke_anim_id = v.a_dym_pojazd[mu.dmg_smoke_level - 1];

    // 0x0048723d-0x00487243: reset the animation timer to the current game clock.
    mu.dmg_smoke_anim_timer = *v.game_clock;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_update_damage_smoke(uint32_t player, int32_t unit_idx) {
    sim_state st = state();
    detail::unit_update_damage_smoke(st.read, st.own, player, unit_idx);
}


} // namespace mh::sim
