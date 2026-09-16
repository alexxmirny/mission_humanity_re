//
// tact/tact_fx_splash_damage.h -- TACT1C: apply splash/explosion damage to every unit occupying a
// (2*radius+1)^2 tile square centered on (col,row), toroidally wrapped over the map extent.
//
//   llm_tact_fx_splash_damage @0x004314aa (0x1aa)
//   void __mh_watcall_ebx_volatile llm_tact_fx_splash_damage(int col, int row, int radius_tiles,
//                                                              int damage)
//
// SHAPE, transcribed literally from @0x004314cb-0x00431653:
//   for (rr = row - radius_tiles; rr <= row + radius_tiles; ++rr)      // @0x004314cb-0x004314ea
//     for (cc = col - radius_tiles; cc <= col + radius_tiles; ++cc) {  // @0x004314ec-0x00431505
//       // @0x0043150d-0x00431530: toroidal wrap (width/height are the SAME `width`/`height`
//       // globals as v.grid_width/v.grid_height), then the roster index off the tile's `.building`
//       // field -- the SAME field llm_tact_unit_destroy reads/clears (finding: unit_despawn's
//       // review caught a sibling function using `.unit[0..1]` at +4 instead of this word at +2;
//       // this translation uses `.building` from the start).
//       col_w = (width-1) & cc;  row_w = (height-1) & rr
//       target = tile_object_at(col_w, row_w).building
//       if (target == 0) continue;                                     // @0x00431533-0x00431537
//       if (units[target].anim_state == 2 || == 3) continue;            // @0x0043153d-0x0043155d
//       if (units[target].hp > damage) {                                // @0x00431562-0x00431573
//         // NOT lethal. @0x00431611/0x0043161a: subtract UNLESS (spare_nonzero_owner != 0 AND
//         // units[target].owner != 0). spare_nonzero_owner has NO WRITER anywhere in the binary
//         // (see tact_state.h's own field comment) -- the owner-gated skip is DEAD in every
//         // observed build, preserved as a real branch rather than folded to an unconditional
//         // subtract.
//         if (spare_nonzero_owner == 0 || units[target].owner == 0) units[target].hp -= damage;
//         unit_refresh_ui_slot(target);                                 // @0x0043163b
//       } else {
//         // LETHAL. @0x00431579-0x00431599: the "kill stamp" below runs only when BOTH
//         // anim_state <= 1 AND progress > 0 (two separate original tests, ANDed).
//         if (units[target].anim_state <= 1 && units[target].progress > 0) {  // @0x0043159b
//           facing_to_delta(units[target].facing_dir, &dx, &dy);
//           passable_at(col+dx, row+dy) = PASSABLE_DEFAULT;             // @0x004315b4-0x004315c5
//           passable_at(col, row)       = PASSABLE_BLOCKED;             // @0x004315cc-0x004315d5
//         }
//         units[target].hp = 0; units[target].progress = 0;             // @0x004315dc-0x004315f3
//         unit_set_anim_state(target, 0x1f);                            // @0x004315fa-0x00431602
//         unit_refresh_ui_slot(target);                                 // @0x00431607
//       }
//     }
//
// PROOF: OFFLINE. the measured write closure of llm_tact_fx_splash_damage -> 5 functions
// reachable, 3 regions: _G_LLM_TACT_UNITS + `passable` (own, depth 0) and
// _G_LLM_TILE_VIS_MAP_M5 (depth 2, written by llm_tact_unit_draw_hp_bar_slot, itself reached
// through the frontier llm_tact_unit_refresh_ui_slot -- an already-gated `pure` dispatcher per
// tools/data/tact_effect_classes.json, so the reach is safe, but the closure crosses into
// presentation-scratch territory the same way the sibling roster/UI functions in this batch do).
// REVIEW REQUIRED (writes a shared plane -- `passable`). Proof is an offline oracle over the wrap
// math, the roster lookup via `.building`, both damage arms, and the dead-owner-flag branch above.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

struct fx_splash_damage_calls {
    void (*facing_to_delta)(int32_t facing_dir, int32_t *out_dx,
                            int32_t *out_dy);                        // llm_tact_facing_to_delta @0x004311bf
    void (*unit_set_anim_state)(int32_t building_id, uint8_t state); // llm_tact_unit_set_anim_state @0x00430f03
    void (*unit_refresh_ui_slot)(int32_t building_id);               // llm_tact_unit_refresh_ui_slot @0x00434f98
};

const fx_splash_damage_calls &live_fx_splash_damage_calls();

namespace detail {

// llm_tact_fx_splash_damage @0x004314aa.
void fx_splash_damage(const tact_view &v, tact_store &own, const fx_splash_damage_calls &c,
                      int32_t col, int32_t row, int32_t radius_tiles, int32_t damage);

} // namespace detail

void fx_splash_damage(int32_t col, int32_t row, int32_t radius_tiles, int32_t damage);


} // namespace mh::tact
