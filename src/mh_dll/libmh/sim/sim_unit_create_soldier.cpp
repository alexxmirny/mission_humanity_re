//
// sim/sim_unit_create_soldier.cpp -- see sim_unit_create_soldier.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_unit_create_soldier_00463ac6.asm), not from Ghidra's C: the draft's overall shape
// (population-cap guard -> direct-placement-or-outward-scan -> per-slot record init -> tile
// occupancy -> population bookkeeping -> notify) is structurally faithful and was used as a reading
// aid, but every field write, every re-derived index, and the exact scan-loop bound below were
// re-walked against the raw CMP/JZ/JNZ/JL/JG targets rather than trusted from the .c.
//
// ---- DECLARED NEEDS: see the header for the full list (soldiers/passable view members, mutable
// tile_object/passable/pop_stats accessors, and the missing shadow-site codegen). This TU will not
// compile until they land -- by design, matching sim_unit_passive_engage.cpp's precedent.
//
#include "sim/sim_unit_create_soldier.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder (SIM1-P clause 2)
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT -- see the anon-namespace note below
#include "state/rebind_targets.gen.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::sim {

namespace {

// The NEWLY PLACED unit's own order/state fields (0x00463c51-0x00463c80) are set to the real
// `llm_strat_unit_state::STOP_TO_DEFAULT` enum member (Ghidra enum, value 1) -- unambiguously the
// same usage sim_order_enqueue.h's UNIT_STATE_STOP_TO_DEFAULT already names (a unit's own order/state
// pair), so that constant is reused verbatim via the include above per the naming brief (17a), not
// re-declared here.
//
// SEPARATELY: the `*plVar1 = *plVar1 + STOP_TO_DEFAULT;` idiom on units[player][0].order (0x00463c2d,
// a bare `INC word ptr`) targets slot 0 -- the roster's per-player HEADER row, not a live unit (see
// sim_unit_passive_engage.cpp's derivation of that same slot's `unit_above` bytes as a roster-count
// field). Its numeric value is provably 1 either way (the INC proves that much on its own), but
// whether reusing the ENUM MEMBER's meaning there -- as opposed to it merely sharing the same bit
// pattern by coincidence -- is correct is UNVERIFIED, so it is named separately here rather than
// silently assumed identical to UNIT_STATE_STOP_TO_DEFAULT's real order/state usage above (see
// uncertainties[] in the translation report).
inline constexpr uint16_t kUnitZeroOrderHeaderIncrement = 1;

} // namespace

namespace detail {

uint32_t create_soldier(const sim_view &v, sim_store &own, uint32_t tile_x, uint32_t tile_y,
                        uint16_t unit_proto_id, uint16_t player, char is_special_flag) {
    // ---- population-cap guard (0x00463ad3-0x00463b37) --------------------------------------------
    // `is_special_flag == 1` narrows the slot search below by 9 -- the "9" is a bare literal in the
    // asm (`MOV ..., 0x9`), not a Ghidra-named constant anywhere in the image.
    const int32_t slot_search_reduction = (is_special_flag == 1) ? 9 : 0;

    const cfg_unit &proto = v.cfg_units[unit_proto_id];
    if (proto.soldier_count != 0) {
        // ONLY computed/checked when soldier_count != 0 -- soldier_count == 0 skips this whole guard
        // and always falls through to the placement check (asm: JZ straight past the CMP EAX,0x64).
        // ZERO-extend owner_unit (asm: MOVZX EAX,word ptr [...]) -- reimpl-verify 2026-08-10 caught a
        // sign-extending `(int32_t)` cast here, which flips the guard for any owner_unit with the
        // high bit set (0x8000 reads as -32768, letting a should-be-refused creation through). Same
        // field/idiom sim_unit_recruit.cpp already gets right.
        const int32_t existing_plus_new =
            (int32_t)(uint32_t)(uint16_t)v.soldiers[player * v.caps.soldiers + 0].owner_unit +
            proto.soldier_count + 1;
        if (existing_plus_new >= 100) return 0;
    }

    // ---- is the target tile directly placeable? (0x00463b3c-0x00463b68) ---------------------------
    if (tile_at(v, (int32_t)tile_x, (int32_t)tile_y).class_owner == 0 &&
        v.passable[(tile_x << 8) | tile_y] != 0) {
        // ---- direct placement: find an empty unit slot (0x00463be4-0x00463eb0) --------------------
        for (int32_t slot = 1; slot < 100 - slot_search_reduction; ++slot) {
            if (unit_of(v, (uint32_t)player, slot).unit_proto_id != 0) continue; // occupied

            // The `*plVar1 = *plVar1 + STOP_TO_DEFAULT;` idiom, verbatim -- see the header/anon-
            // namespace comment. `units[player][0]`, NOT `units[player][slot]`.
            unit &roster_header = own.unit_at((uint32_t)player, 0);
            roster_header.order = (uint16_t)(roster_header.order + kUnitZeroOrderHeaderIncrement);

            MH_LIBMH_BIND(llm_strat_unit_housing_count_add)((uint32_t)player, (uint32_t)unit_proto_id);
            MH_LIBMH_BIND(llm_strat_unit_init_record)(slot, (uint32_t)unit_proto_id, (uint32_t)player);

            // Taken AFTER init_record so `.unit_proto_id` below reflects what init_record just wrote
            // (the original re-reads units[player][slot].unit_proto_id from memory at each of its 4
            // later uses rather than reusing the `a2`/unit_proto_id register -- see the header note;
            // nothing between here and those uses writes unit_proto_id again, so reading it once here
            // is behaviourally identical to the original's repeated re-reads).
            unit &u = own.unit_at((uint32_t)player, slot);

            u.order                    = UNIT_STATE_STOP_TO_DEFAULT;
            u.state                    = UNIT_STATE_STOP_TO_DEFAULT;
            u.x                        = (uint8_t)tile_x;
            u.y                        = (uint8_t)tile_y;
            u.origin_tile_was_passable = v.passable[(tile_x << 8) | tile_y];
            u.move_step_speed_scale    = (double)u.origin_tile_was_passable;

            own.tile_object_at((int32_t)tile_x, (int32_t)tile_y).building = (uint16_t)slot;
            own.tile_object_at((int32_t)tile_x, (int32_t)tile_y).class_owner =
                (uint8_t)((uint8_t)player | 0x80u);
            own.passable_at((int32_t)tile_x, (int32_t)tile_y) = 0;

            // ---- population bookkeeping (0x00463d5b-0x00463e41): re-derives the cfg row from the
            // JUST-WRITTEN unit_proto_id (u.unit_proto_id, not the `unit_proto_id` parameter) ----
            const cfg_unit &placed_proto = v.cfg_units[u.unit_proto_id];
            if (placed_proto.soldier_count == 0 && placed_proto.human != 0) {
                MH_LIBMH_BIND(llm_strat_population_add)(player, placed_proto.human);
                own.population_at(player).human -= placed_proto.human;
                own.population_at(player).human_in_field += placed_proto.human;
            }

            // ---- shared tail (0x00463e47-0x00463eae), always runs regardless of the population
            // branch above ----
            MH_LIBMH_BIND(map_fow_UpdateFoWPlus)(player, tile_x, tile_y, (uint8_t)placed_proto.sight);
            MH_PROMOTED_ROW(llm_strat_ai_notify_unit_lifecycle)(player, u.unit_proto_id, (uint32_t)slot, 4u);

            return (uint32_t)slot;
        }
        // Slot search exhausted (all `100 - slot_search_reduction` slots occupied) -- falls to the
        // final `return 0;` below, matching LAB_00463eb5.
    } else {
        // ---- not directly placeable: scan outward along the row (0x00463b69-0x00463bdf) -----------
        // Faithful transliteration of the do-while's exact bottom-tested bound, NOT a "for (attempt =
        // 0; attempt < width; ...)" -- the asm compares the PRE-increment attempt counter against
        // `width`, so this runs while `attempt(before increment) < width`, i.e. ONE MORE scan attempt
        // than `width` naively suggests (attempt values 0..width inclusive, `width + 1` total probes)
        // -- reproduced exactly, not "fixed" to a rounder bound.
        uint32_t scan_x  = tile_x;
        int32_t  attempt = 0;
        for (;;) {
            scan_x = map_width_mask(v) & (scan_x + 1);
            if (tile_at(v, (int32_t)scan_x, (int32_t)tile_y).class_owner == 0 &&
                v.passable[(scan_x << 8) | tile_y] != 0) {
                // Genuine local recursive call -- the outer and recursive invocations are the exact
                // same shadow-armed body (brief instruction; NOT routed through mh::call::).
                return create_soldier(v, own, scan_x, tile_y, unit_proto_id, player, is_special_flag);
            }
            const int32_t attempt_before_inc = attempt;
            ++attempt;
            if (!(attempt_before_inc < *v.map_width)) break;
        }
        // Scan exhausted -- falls to the final `return 0;` below, matching LAB_00463bd8.
    }

    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t unit_create_soldier(uint32_t tile_x, uint32_t tile_y, uint16_t unit_proto_id, uint16_t player,
                             char is_special_flag) {
    sim_state st = state();
    return detail::create_soldier(st.read, st.own, tile_x, tile_y, unit_proto_id, player,
                                  is_special_flag);
}


} // namespace mh::sim
