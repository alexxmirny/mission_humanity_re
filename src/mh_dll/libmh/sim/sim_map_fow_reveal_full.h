//
// sim/sim_map_fow_reveal_full.h -- llm_map_fow_reveal_full, the "reveal the whole map's
// fog-of-war for one player" order handler (RI-SIM SIM1D / SIM1E opening,
// tmp/decomp/llm_map_fow_reveal_full_0049ab26.asm).
//
// llm_map_fow_reveal_full @0x0049ab26 (0x70 B) -- llm_strat_order_queue_dispatch order-code table
//   case 20/0x14 (order_code 0xf9, high/special command range), single arg (player). Strides a
//   6-tile grid over the WHOLE map, calling map::fow::UpdateFoWPlus(player, x, y, 10) once per grid
//   point (0x0049ab48-0x0049ab8c):
//     for (x = 0; x < width;  x += 6)
//       for (y = 0; y < height; y += 6)
//         map_fow_UpdateFoWPlus(player, x, y, 10);
//   `width`/`height` here are the plain map-DIMENSION globals (0x00825084 / 0x00825064,
//   `mh::addr::width` / `mh::addr::height`, already bound as sim_view::map_width/map_height @
//   RID_WIDTH/RID_HEIGHT) -- NOT the torus wrap-mask fields (`general.width_mask`/`height_mask`,
//   sim_state.h's map_width_mask()/map_height_mask()) other sim functions read; the two are
//   different quantities at different addresses and this function's asm reads the dimension pair
//   directly by absolute address, confirmed against mh_addrs.gen.h's own width/height entries.
//
// ---- LOOP SHAPE: OUTER = X (bounded by width), INNER = Y (bounded by height) -----------------------
// The outer counter ([EBP-0x1c]) is compared against `width` (0x00825084) and is loaded into EDX at
// the call site; the inner counter ([EBP-0x18]) is compared against `height` (0x00825064) and is
// loaded into EBX. The call is `map_fow_UpdateFoWPlus(EAX=player, EDX=x, EBX=y, ECX=0xa)`, matching
// the committed signature `(uint32_t player, uint32_t x, uint32_t y, uint8_t sight)` in
// addr/mh_calls.gen.h exactly -- so the outer/X-bounded-by-width, inner/Y-bounded-by-height mapping
// is direct from the register assignment at the call site, not inferred from variable names (the
// Ghidra .c draft's `local_20`/`local_1c` naming agrees with this reading here, unlike the
// get_coords-style mismatch other sim TUs have had to correct for).
//
// Sight radius is the literal constant 10 (`MOV ECX,0xa`) every call -- a "reveal fully" radius, no
// backing Ghidra enum found (a single freestanding literal, not a discriminated domain per rule 17a).
//
// No sim state is WRITTEN by this function itself -- the only effect is the outward call to the
// ORIGINAL map_fow_UpdateFoWPlus (@0x0049681a, already committed in addr/mh_calls.gen.h), which does
// its own writes (map::fow bits, G_TMP_PLAYER/G_TMP_SIGHT scratch -- see mh_addrs.gen.h) outside this
// translation's own region-write closure. Same "outward call, no local write" posture as
// sim_bldg_reset_construction_anim's callee, just with zero local state at all -- there is no `own`
// (sim_store) parameter here because there is nothing in this closure to write through it.
//
// The opening `utils_assert_stack_capacity` call is the standard inert prologue (translator-brief
// rule 6) and is omitted.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability (same reason as
// every other module here: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest).
struct map_fow_reveal_full_calls {
    // map_fow_UpdateFoWPlus @0x0049681a. Already committed in addr/mh_calls.gen.h
    // (`void(uint32_t player, uint32_t x, uint32_t y, uint8_t sight)`).
    void (*fow_update_fow_plus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);
};

const map_fow_reveal_full_calls &live_map_fow_reveal_full_calls();

namespace detail {

// llm_map_fow_reveal_full @0x0049ab26. See the header banner above for the full derivation; the
// .cpp carries the per-line address citation. No sim_store parameter -- this closure writes no sim
// state of its own.
void map_fow_reveal_full(const sim_view &v, const map_fow_reveal_full_calls &c, uint32_t player);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter type matches the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint32_t player).
void map_fow_reveal_full(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
