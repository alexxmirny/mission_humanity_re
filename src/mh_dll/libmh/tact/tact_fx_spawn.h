//
// tact/tact_fx_spawn.h -- TACT1C: allocate a free _G_LLM_TACT_FX_POOL slot for a projectile or pure
// animation effect, stamp its spawn state, and play the associated sound with view-relative pan/
// volume (or skip the sound entirely when the spawn point is far outside the camera view).
//
//   llm_tact_fx_spawn @0x0042bdce (0x4a6)
//   int __mh_watcall_ecx_ebx_volatile llm_tact_fx_spawn(int fx_type, byte owner, int x, int y,
//                                                        int x2, int y2, byte altitude)
//   returns 1 if the pool is full (no slot allocated -- record NOT written), 0 on success.
//
// SHAPE, transcribed literally from @0x0042bdfd-0x0042c271:
//   1. @0x0042bdfd-0x0042be34: linear scan slots 1..0x3ff (index 0 is never allocated) for the
//      first fx_type==0 (free). No free slot -> return 1, nothing written.
//   2. @0x0042be39-0x0042bee3: stamp fx_type, owner, pos_x/y = (double)(x,y), travel_dx/dy = 0.0,
//      tile_col = x/32 (shift-trick signed div, matches C `/`), tile_row = y/24 (real IDIV,
//      matches C `/`), altitude, move_clock = time_GetCurrentTime().
//   3. @0x0042bef2-0x0042c009: view-relative sound gate. center_col/row = camera + half the view
//      extent; dist_col/row = abs(tile_col/row(x,y) - center_col/row). If BOTH dist_col <= half_w
//      AND dist_row <= half_h ("fully in view"): play at fixed volume 100 with a signed pan
//      (@0x0042bf8e-0x0042bfff). Otherwise, if dist_col > VIEW_TILES_W or dist_row > VIEW_TILES_H
//      ("too far even for a distant cue"): skip the sound call entirely (falls straight to step 4).
//   4. @0x0042c024-0x0042c0ec: the "distant but audible" volume falloff, reached only when the
//      in-view test failed but neither far-skip test tripped. `vol = 100/(dist_col-half_w)` when
//      dist_col > dist_row; `vol = 100/(dist_row-half_h)` when dist_col < dist_row. **When
//      dist_col == dist_row EXACTLY, the original reads vol off an UNINITIALISED stack slot** --
//      neither branch's CMP/JGE pair (@0x0042c02a, @0x0042c05b) executes the assignment in that
//      one case (verified by tracing both jump targets against both fallthroughs). C++ cannot
//      reproduce a raw stack-garbage read as defined behaviour; this translation substitutes a
//      documented `vol = 0` for the tie case (see the .cpp) rather than invoking UB. Then plays at
//      the same pan formula as step 3's in-view branch (@0x0042c086-0x0042c0f5).
//   5. @0x0042c0fa-0x0042c136: dir24 = 1 if (x,y)==(x2,y2), else calc_dir24(x, y, x2, y2). Stamps
//      target_x/y = (double)(x2,y2).
//   6. @0x0042c136-0x0042c1fa: dist_sq computed as INT arithmetic ((x2-x)^2+(y2-y)^2, may overflow
//      exactly as the original's 32-bit IMUL/ADD does -- preserved, not widened), then converted to
//      double. dist_sq > 0.0: vel_x/y = (double)(x2-x, y2-y) / sqrt((double)dist_sq). Else (spawn
//      == target): vel_x = vel_y = 0.0.
//   7. @0x0042c1fa-0x0042c25b: frame_counter = 0 (unconditional, before the branch below).
//      fx_type_table[fx_type].direct != 0: sprite_frame = (dir24-1)*fx_type_table[fx_type].frames +
//      frame_counter (reads the field just set to 0 -- kept as a field read, not folded to the
//      literal, since that is what the instruction stream does). Else: sprite_frame = frame_counter.
//   8. @0x0042c25b: ++_G_LLM_TACT_FX_LIVE_COUNT. Return 0.
//
// PROOF: OFFLINE. the measured write closure of llm_tact_fx_spawn -> 8 functions reachable, 4
// regions: _G_LLM_TACT_FX_LIVE_COUNT / _G_LLM_TACT_FX_POOL (own, depth 0) plus
// _G_LLM_SND_CHANNEL_NEXT_RETRIGGER_TIME / a DirectSound scratch dword (both depth 1-3, INSIDE the
// gated llm_tact_fx_play_sound's own suppressed-in-`ours` body -- declaring them as shadow regions
// would compare a real original-arm write against a deliberately-suppressed ours-arm no-write,
// which is the gate's OWN correctness property, not a translation bug; the sibling
// llm_tact_fx_update_projectile.h (same fx_play_sound reach) reached the identical conclusion and
// also went OFFLINE). Proof is an offline oracle over the pool-slot record, the pan/volume/skip
// gate (mocking fx_play_sound through the calls struct), and the tie-case substitution above.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

struct fx_spawn_calls {
    double (*time_now)();                                                    // time_GetCurrentTime @0x00427616
    void (*fx_play_sound)(int32_t fx_type, int32_t volume_pct, int32_t pan); // llm_tact_fx_play_sound @0x0042f060
    int32_t (*calc_dir24)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);   // llm_tact_calc_dir24 @0x0042e0e2
    double (*sqrt_fn)(double);                                               // llm_sqrt @0x004da9c0
};

const fx_spawn_calls &live_fx_spawn_calls();

namespace detail {

// llm_tact_fx_spawn @0x0042bdce.
int32_t fx_spawn(const tact_view &tv, tact_store &own, const fx_spawn_calls &c, int32_t fx_type,
                 uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2, uint8_t altitude);

} // namespace detail

int32_t fx_spawn(int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
                 uint8_t altitude);


} // namespace mh::tact
