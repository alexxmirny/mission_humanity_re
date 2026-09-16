//
// tact/tact_camera_center_on_tile.h -- TACT1E: centers the tactical camera on a given tile.
//
//   llm_tact_camera_center_on_tile @0x0042e889 (0xce)
//   void __watcall llm_tact_camera_center_on_tile(int target_col, int target_row)
//   param target_col   storage=EAX:4
//   param target_row   storage=EDX:4
//
// Re-derived from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_camera_center_on_tile_0042e889.asm), not from Ghidra's .c: the .c's
// literal transcription of the SBB-with-borrow idiom as a `(uint)(... < 0)` subtraction is
// correct but opaque, and it collapses the two-step `/32` then `/2` into a single `/64` (only
// equal to the two-step form because both divisors are positive -- see below), which hides the
// actual instruction shape.
//
// DERIVATION:
//
//  0. @0x0042e889-0x0042e8a6: prologue (assert_stack_capacity(0x20) is inert per
//     translator-brief.md #6 -- omitted); target_col/target_row spilled to locals.
//
//  1. @0x0042e8a6-0x0042e8c7: EAX = (G_WIN_W / 32) / 2, BOTH divisions truncating toward zero.
//     `G_WIN_W / 32` is the SAR/SHL/SBB/SAR shift-based signed-divide-by-32 idiom -- the IDENTICAL
//     sequence tact_cam_follow_selection_tick.h already verified by hand for both signs and
//     documented as exact for plain C++ `/`. The following MOV/MOV/SAR/SUB/SAR block is the same
//     compiler's bias-then-shift idiom for `/2`. Composing two truncating divisions by positive
//     integers a,b as `(x/a)/b` equals `x/(a*b)` (a standard integer-division identity for
//     nonnegative dividends, extended by sign here through the trunc-toward-zero bias each step
//     already applies) -- so `(win_w/32)/2` is exact AND happens to equal `win_w/64`, but the code
//     below spells it as the two literal steps the assembly performs.
//
//  2. @0x0042e8c7-0x0042e8cc: own.map_cam_col() = target_col - EAX (step 1's result).
//
//  3. @0x0042e8d2-0x0042e8f0: EAX = (WindowHeight / 24) / 2. `WindowHeight / 24` is a literal
//     `IDIV EBX` (EBX=0x18=24) -- real hardware division, exact via C++ `/`. The following block is
//     the same `/2` bias-then-shift idiom as step 1's second half.
//
//  4. @0x0042e8f2-0x0042e8f7: own.map_cam_row() = target_row - EAX (step 3's result).
//
//  5. @0x0042e8fd-0x0042e923: clamp map_cam_col() and map_cam_row() up to >= 0 (independent
//     per-axis checks, asm order col-then-row).
//
//  6. @0x0042e923-0x0042e949: clamp map_cam_col() down to <= 0x6f (111) and map_cam_row() down to
//     <= 0x6a (106). These two bounds have no existing named constant anywhere in mh/tact (grepped)
//     -- kept as local named constants in the .cpp rather than invented shared ones; see
//     declared_needs in the translation report if a shared viewport-bound name should exist.
//
//  7. @0x0042e949: llm_tact_vis_map_fill_default() called UNCONDITIONALLY on every path (the
//     function never early-returns) -- routed through the calls-struct per Law 3b since it is a
//     shared/effectful frontier callee (same callee tact_cam_follow_selection_tick.h already mocks
//     the same way).
//
// PROOF PATH: same shape as tact_cam_follow_selection_tick.h -- the write closure reaches the
// frontier `llm_tact_vis_map_fill_default`, a shared/ungated `state`-class callee, so this site is
// proven via net_selftest.exe tacttest through the mockable `camera_center_on_tile_calls` seam, not
// rig-armed.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable callee seam -- `llm_tact_vis_map_fill_default` is a shared/effectful frontier callee, so
// the offline oracle must mock it rather than let the translated body call `mh::call::` directly
// (translator-brief.md 3b). Same shape as `cam_follow_selection_tick_calls`.
struct camera_center_on_tile_calls {
    void (*vis_map_fill_default)();
};

const camera_center_on_tile_calls &live_camera_center_on_tile_calls();

namespace detail {

// llm_tact_camera_center_on_tile @0x0042e889. See the header banner for the full derivation.
// Takes `v` only for `win_w` (G_WIN_W); `own` for WindowHeight and the two camera-position fields.
void camera_center_on_tile(const tact_view &v, tact_store &own,
                           const camera_center_on_tile_calls &c, int32_t target_col,
                           int32_t target_row);

} // namespace detail

void camera_center_on_tile(int32_t target_col, int32_t target_row);

// Declared here per the module convention; DEFINED in tact_camera_center_on_tile.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
