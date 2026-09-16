#include "sim/libtrans/sim_lt_deploy_squad.h"

#include "addr/mh_calls.gen.h" // typed callables for the still-original callees this TU binds directly
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const deploy_squad_calls &live_deploy_squad_calls() {
    static const deploy_squad_calls c = {
        MH_LIBMH_BIND(game_GetStartingUnit),
        mh::state::evt::cam_set_col,
        mh::state::evt::cam_set_row,
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(llm_strat_unit_order_scatter_from_spawn),
    };
    return c;
}

namespace {

// 0x00465f49-0x00465f5c / 0x00465f74-0x00465f87: `SAR EDX,0x1f; SHL EDX,6; SBB EAX,EDX; SAR EAX,6`
// -- the compensated arithmetic-shift idiom Watcom emits for a SIGNED divide by 64, reproduced here
// as shifts/mask/add (translator-brief rule 8: reproduce the shift form, not `/ 64`), even though
// the result is numerically identical to C++'s `/ 64` in this case (both truncate toward zero;
// hand-verified for w in {-128, -65, -64, -63, 127}). `(w >> 31) & 0x3f` reproduces the SAR-then-
// SHL pair's bit pattern (0 when w >= 0, 0x3f when w < 0 -- the SBB's borrow is already folded into
// that mask), and the final `>> 6` is the trailing SAR.
int32_t sdiv64_shift_form(int32_t w) {
    const int32_t bias = (w >> 31) & 0x3f;
    return (w + bias) >> 6;
}

} // namespace

namespace detail {

int32_t deploy_starting_squad(const sim_view &v, sim_store &own, const deploy_squad_calls &c,
                              const debris_burst_calls &c_debris) {
    const uint32_t width_mask  = map_width_mask(v);
    const uint32_t height_mask = map_height_mask(v);
    // 0x00465d3e: PlayerSide read via MOVZX (16-bit unsigned widen) -- widen through uint16_t, not
    // the view's own signed int16_t.
    const uint16_t player_side = static_cast<uint16_t>(*v.player_side);

    // 0x00465d3e-0x00465d57: starting unit type = game_GetStartingUnit(profile.race) + 1, truncated
    // to 16 bits at the create_soldier call site (header hazard 4/5) -- the +1 is real.
    const int32_t  starting_unit_plus_one = c.get_starting_unit(v.profiles[player_side].race) + 1;
    const uint16_t unit_type              = static_cast<uint16_t>(starting_unit_plus_one);

    // 0x00465d61-0x00465fcb: outer scan col 0..width-1, inner row 0..height-1 (hazard 2) -- returns
    // on the FIRST clear 7x7 area found; scan order is behaviour, not style.
    for (int32_t col = 0; col < *v.map_width; ++col) {
        for (int32_t row = 0; row < *v.map_height; ++row) {
            // ---- the clear test (0x00465d92-0x00465e26, hazard 3) --------------------------------
            // stencil_row perturbs the map COLUMN and stencil_col perturbs the map ROW -- that is
            // what the register trace shows (EBP-0x2c is added to `col`, EBP-0x30 to `row`), not a
            // naming slip. The loop carries an early-out flag tested in the loop CONDITION; kept for
            // shape fidelity even though the boolean result is identical either way.
            bool clear = true;
            for (int32_t stencil_row = 0; stencil_row < 7 && clear; ++stencil_row) {
                for (int32_t stencil_col = 0; stencil_col < 7 && clear; ++stencil_col) {
                    const uint32_t wc = static_cast<uint32_t>(col + stencil_row) & width_mask;
                    const uint32_t wr = static_cast<uint32_t>(row + stencil_col) & height_mask;
                    if (tile_at(v, static_cast<int32_t>(wc), static_cast<int32_t>(wr)).class_owner != 0 ||
                        v.passable[(wc << 8) | wr] == 0) {
                        clear = false;
                    }
                }
            }
            if (!clear) continue;

            // ---- place the formation (0x00465e30-0x00465f3f) --------------------------------------
            // One llm_unit_create_soldier per set stencil cell (in-tree sibling, rule 3c -- see the
            // header banner), with the four independent scatter nudges on the formation's edges.
            // THE ASYMMETRY IS IN THE BINARY (hazard 6, candidate preserve_bug): the two "+1"
            // directions are NOT torus-wrapped, the two "-1" directions ARE. All four ifs are
            // independent, not else-if, matching the asm exactly.
            for (int32_t stencil_row = 0; stencil_row < 7; ++stencil_row) {
                for (int32_t stencil_col = 0; stencil_col < 7; ++stencil_col) {
                    const uint32_t wc = static_cast<uint32_t>(col + stencil_row) & width_mask;
                    const uint32_t wr = static_cast<uint32_t>(row + stencil_col) & height_mask;
                    if (v.deploy_formation_stencil[stencil_row * 7 + stencil_col] == 0) continue;

                    const uint32_t slot = detail::create_soldier(v, own, wc, wr, unit_type, player_side,
                                                                 static_cast<char>(2));
                    if (slot == 0) continue;

                    const int32_t unit_idx = static_cast<int32_t>(slot);
                    if (stencil_row < 2) {
                        c.scatter_from_spawn(player_side, unit_idx, wc + 1, wr);
                    }
                    if (stencil_col < 2) {
                        c.scatter_from_spawn(player_side, unit_idx, wc, wr + 1);
                    }
                    if (stencil_row > 4) {
                        c.scatter_from_spawn(player_side, unit_idx, (wc - 1) & width_mask, wr);
                    }
                    if (stencil_col > 4) {
                        c.scatter_from_spawn(player_side, unit_idx, wc, (wr - 1) & height_mask);
                    }
                }
            }

            // ---- camera + effect tail (0x00465f49-0x00465fb3, hazard 7/8) -------------------------
            const uint32_t cam_col =
                static_cast<uint32_t>(col + 3 - sdiv64_shift_form(*v.win_w)) & width_mask;
            const uint32_t cam_row =
                static_cast<uint32_t>(row + 3 - sdiv64_shift_form(*v.win_h)) & height_mask;
            c.cam_set_col(static_cast<int32_t>(cam_col));
            c.cam_set_row(static_cast<int32_t>(cam_row));
            // In-tree sibling (rule 3c) -- NOT mh::call::, and not in _outward_effects.target_rows
            // (a selector blind spot for the classification data, moot for this direct C++ call).
            detail::spawn_debris_burst(100, v, own, c_debris);
            c.snd_play(0xa5, 100);
            return 1;
        }
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t deploy_starting_squad() {
    sim_state st = state();
    return detail::deploy_starting_squad(st.read, st.own, live_deploy_squad_calls());
}

} // namespace mh::sim
