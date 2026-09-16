//
// sim/sim_unit_soldier_screen_pos.cpp -- see sim_unit_soldier_screen_pos.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_soldier_get_sprite_screen_pos_00491086.asm), not from
// Ghidra's C draft: the draft's rendering of the divide-by-4 idiom
// (`(iVar4 + iVar5 * -4) - (uint)(iVar5 << 1 < 0)) >> 2`) is a correct comma-free reading of the raw
// SAR/SHL/SBB/SAR sequence and is kept value-for-value, but re-cast into the shift form the header
// hazard note calls out rather than trusted as an opaque expression.
//
#include "sim/sim_unit_soldier_screen_pos.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// unit::unit_above is `uint8_t[2]` in the generated header (map_t_unit_full_id rendered as a raw
// byte pair, not a scalar) -- this reassembles the little-endian word the original addresses with a
// single `MOVZX AX, word ptr [...]`. Same idiom as sim_unit_update_soldiers.cpp's / sim_unit_
// purge_unregistered.cpp's own local helpers of the same name; not shared across translation units
// per the "no new shared helpers" rule -- this one is a trivial byte-pair reassembly, not new logic.
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

} // namespace

const unit_soldier_get_sprite_screen_pos_calls &live_unit_soldier_get_sprite_screen_pos_calls() {
    static const unit_soldier_get_sprite_screen_pos_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_calc_interp_pixel_pos),
    };
    return c;
}

namespace detail {

void unit_soldier_get_sprite_screen_pos(const sim_view &v, const unit_soldier_get_sprite_screen_pos_calls &c,
                                        uint16_t player, int32_t unit_idx, int32_t soldier_hop_count,
                                        uint32_t *out_x, uint32_t *out_y) {
    const uint32_t p = (uint32_t)player;

    // ---- (1) walk the soldier chain EXACTLY soldier_hop_count hops (0x004910a7-0x004910fc) --------
    // Seed at the chain head (unit.unit_above), then follow next_soldier that many times. A plain
    // `for` loop -- soldier_hop_count <= 0 leaves soldier_idx at the head, untouched.
    uint32_t soldier_idx = unit_full_id_word(unit_of(v, p, unit_idx).unit_above);
    for (int32_t hop = 0; hop < soldier_hop_count; ++hop) {
        soldier_idx = v.soldiers[p * v.caps.soldiers + soldier_idx].next_soldier;
    }

    const soldier  &s     = v.soldiers[p * v.caps.soldiers + soldier_idx];
    const unit     &u     = unit_of(v, p, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // ---- (2) animated sprite-frame index (0x0049110f-0x004911a3) -----------------------------------
    // sprite_base = Unit[proto].sprite + ((soldier.sprite_frame % 24) / 3) * 8. Both divisions here
    // are genuine IDIVs in the assembly (0x0049113d/0x00491149), so plain C `%`/`/` reproduce them
    // exactly.
    const int32_t sprite_base = proto.sprite + (((int32_t)s.sprite_frame % 24) / 3) * 8;

    // signed-divide-by-4 idiom (SAR/SHL/SBB/SAR @ 0x00491186-0x00491190) -- see the header HAZARD
    // note. sign is 0 or -1; `- (sign << 2) - (sign != 0)` folds in the exact +3-when-negative bias
    // the asm's SHL+SBB pair computes, then the final `>> 2` is the arithmetic-shift floor.
    const int32_t anim_sum   = (int32_t)s.anim_change_count + u.move_microstep;
    const int32_t sign       = anim_sum >> 31;
    const int32_t div4       = (anim_sum - (sign << 2) - (sign != 0 ? 1 : 0)) >> 2;
    const int32_t sprite_idx = sprite_base + (div4 % 8);

    const sprite_meta_entry &meta = v.sprite_meta[sprite_idx];

    // ---- (3) X axis (0x004911a6-0x00491201) --------------------------------------------------------
    // The asm stores the raw call result into *param_4 first (0x004911ba), then re-loads it from
    // memory twice more while folding in the sprite-meta offsets, and stores the final masked value
    // over it (0x004911ff). Value-identical simplification: hold the raw result in a local
    // (`interp_x`) instead of round-tripping it through *out_x -- `out_x` isn't read by anything
    // else in between, and s/meta are still read live (by reference) at the point each is used below,
    // matching the asm's own after-the-call read order for cur_x/meta.
    const int32_t interp_x = c.calc_interp_pixel_pos(player, unit_idx, /*axis_is_x=*/1);
    *out_x                 = v.geom->bw_mask &
             (uint32_t)((int32_t)s.cur_x + (interp_x + (int32_t)meta.mount1_x - (int32_t)meta.origin_x));

    // ---- (4) Y axis (0x00491203-0x00491255) -- same shape/simplification as X above -----------------
    const int32_t interp_y = c.calc_interp_pixel_pos(player, unit_idx, /*axis_is_x=*/0);
    *out_y                 = v.geom->bh_mask &
             (uint32_t)((int32_t)s.cur_y + ((int32_t)meta.mount1_y + interp_y - (int32_t)meta.origin_y));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_soldier_get_sprite_screen_pos(uint16_t player, int32_t unit_idx, int32_t soldier_hop_count,
                                        uint32_t *out_x, uint32_t *out_y) {
    sim_state st = state();
    detail::unit_soldier_get_sprite_screen_pos(st.read, live_unit_soldier_get_sprite_screen_pos_calls(),
                                               player, unit_idx, soldier_hop_count, out_x, out_y);
}


} // namespace mh::sim
