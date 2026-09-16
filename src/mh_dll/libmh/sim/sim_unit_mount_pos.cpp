//
// sim/sim_unit_mount_pos.cpp -- see sim_unit_mount_pos.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_calc_mount_fine_pos_00490841.asm,
// tmp/decomp/llm_strat_unit_calc_mount_render_pos_00490b82.asm), not from Ghidra's .c: both drafts'
// overall shape (base-frame index, mount_idx three-way branch, independent-turret sub-sprite sum) is
// correct, but the two per-twin differences (Y-base helper, final mask) were re-walked branch-by-
// branch against the raw CMP/JZ/AND instructions -- see the header's two HAZARD notes.
//
#include "sim/sim_unit_mount_pos.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const mount_pos_calls &live_mount_pos_calls() {
    static const mount_pos_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_calc_fine_axis_pos),
        MH_LIBMH_BIND(llm_strat_unit_calc_render_fine_y),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_calc_mount_fine_pos @0x00490841 -------------------------------------------
//
// Accumulates in uint32_t throughout (matching the x86 ADD's modulo-2^32 wraparound on the signed
// per-field deltas) and masks the finished accumulator with general.bw_mask (X) / general.bh_mask
// (Y) at the very end -- HAZARD (b) in the header. v.geom is mh_llm_strat_map_geom, which already
// carries bw_mask/bh_mask as real fields (offsets 0x0/0x4) distinct from the tile-space width_mask/
// height_mask sim_state.h's own map_width_mask()/map_height_mask() helpers read (offsets 0x8/0x20) --
// no declared need for those two, only for _G_LLM_SPRITE_META (see the header banner).
uint32_t calc_mount_fine_pos(const sim_view &v, const mount_pos_calls &c, uint16_t player,
                             int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    // Base sprite frame index (iVar2 in the draft .c), computed ONCE (0x00490862-0x004908a3),
    // shared by whichever axis branch below runs. Base index uses facing_TARGET.
    const int32_t base_idx = static_cast<int32_t>(u.facing_target) + proto.sprite - 1;

    uint32_t result;
    if (axis_is_x == 1) {
        // X-axis base: llm_strat_unit_calc_fine_axis_pos(..., axis_is_x=1) -- SAME call the render
        // twin makes for X (HAZARD (a) is a Y-only difference).
        result = static_cast<uint32_t>(c.fine_axis_pos(player, unit_idx, 1));

        if (proto.independent == 0) {
            if (mount_idx != 0) {
                if (mount_idx < 2) { // == 1 (mount_idx is unsigned)
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount1_x) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_x));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount2_x) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_x));
                }
                // mount_idx > 2: nothing added, matching the .asm's JMP-to-common-tail else arm.
            }
        } else {
            // Sub-sprite index (iVar3): facing_CURRENT (not facing_target) + sprite + 0x17.
            // Re-derived fresh here, not shared with the Y-branch's own copy below -- see the
            // header's shared HAZARD note.
            const int32_t sub_idx = static_cast<int32_t>(u.facing_current) + proto.sprite + 0x17;
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_x) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_x)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount1_x) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_x));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_x) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_x)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount2_x) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_x));
                }
            }
        }
        result &= v.geom->bw_mask;
    } else {
        // Y-axis base: llm_strat_unit_calc_fine_axis_pos(..., axis_is_x=0) -- the TWIN'S OWN base
        // (calc_mount_render_pos's Y base is a DIFFERENT helper -- see the header HAZARD (a)).
        result = static_cast<uint32_t>(c.fine_axis_pos(player, unit_idx, 0));

        if (proto.independent == 0) {
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount1_y) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_y));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount2_y) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_y));
                }
            }
        } else {
            const int32_t sub_idx = static_cast<int32_t>(u.facing_current) + proto.sprite + 0x17;
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_y) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_y)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount1_y) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_y));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_y) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_y)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount2_y) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_y));
                }
            }
        }
        result &= v.geom->bh_mask;
    }
    return result;
}

// ---- llm_strat_unit_calc_mount_render_pos @0x00490b82 -----------------------------------------
//
// Same shape as calc_mount_fine_pos above, except (HAZARD (a)) the Y-axis base is the altitude-aware
// llm_strat_unit_calc_render_fine_y, NOT calc_fine_axis_pos(...,0), and (HAZARD (b)) the result is
// NOT masked at all -- confirmed by the .asm falling straight from the last offset-add block
// (LAB_00490ea1) to the epilogue with no AND instruction anywhere in the function, unlike its twin.
int32_t calc_mount_render_pos(const sim_view &v, const mount_pos_calls &c, uint16_t player,
                              int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    const int32_t base_idx = static_cast<int32_t>(u.facing_target) + proto.sprite - 1;

    uint32_t result;
    if (axis_is_x == 1) {
        // X-axis base: IDENTICAL call to the fine_pos twin's X base.
        result = static_cast<uint32_t>(c.fine_axis_pos(player, unit_idx, 1));

        if (proto.independent == 0) {
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount1_x) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_x));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount2_x) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_x));
                }
            }
        } else {
            const int32_t sub_idx = static_cast<int32_t>(u.facing_current) + proto.sprite + 0x17;
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_x) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_x)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount1_x) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_x));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_x) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_x)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount2_x) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_x));
                }
            }
        }
        // NO mask here -- HAZARD (b).
    } else {
        // Y-axis base: the ALTITUDE-AWARE helper -- HAZARD (a). NOT calc_fine_axis_pos(...,0).
        result = c.render_fine_y(player, unit_idx);

        if (proto.independent == 0) {
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount1_y) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_y));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        static_cast<int32_t>(v.sprite_meta[base_idx].mount2_y) -
                        static_cast<int32_t>(v.sprite_meta[base_idx].origin_y));
                }
            }
        } else {
            const int32_t sub_idx = static_cast<int32_t>(u.facing_current) + proto.sprite + 0x17;
            if (mount_idx != 0) {
                if (mount_idx < 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_y) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_y)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount1_y) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_y));
                } else if (mount_idx == 2) {
                    result += static_cast<uint32_t>(
                        (static_cast<int32_t>(v.sprite_meta[base_idx].submount_y) -
                         static_cast<int32_t>(v.sprite_meta[base_idx].origin_y)) +
                        static_cast<int32_t>(v.sprite_meta[sub_idx].mount2_y) -
                        static_cast<int32_t>(v.sprite_meta[sub_idx].origin_y));
                }
            }
        }
        // NO mask here -- HAZARD (b).
    }
    // The .asm returns the raw accumulator bit pattern with no sign-extension instruction (a plain
    // `MOV EAX, local_14`), so this is a reinterpreting cast, not a value conversion.
    return static_cast<int32_t>(result);
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

uint32_t calc_mount_fine_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    sim_state st = state();
    return detail::calc_mount_fine_pos(st.read, live_mount_pos_calls(), player, unit_idx, mount_idx,
                                       axis_is_x);
}

int32_t calc_mount_render_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    sim_state st = state();
    return detail::calc_mount_render_pos(st.read, live_mount_pos_calls(), player, unit_idx, mount_idx,
                                         axis_is_x);
}


} // namespace mh::sim
