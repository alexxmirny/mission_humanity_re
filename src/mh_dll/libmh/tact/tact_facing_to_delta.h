//
// tact/tact_facing_to_delta.h -- TACT1B/A: dir24 facing -> octant (dx,dy) lookup.
//
//   llm_tact_facing_to_delta @0x004311bf (0x6a)
//
// A PURE OUT-POINTER WRITE: quantizes a dir24 heading (1..0x18) down to one of 8 octants
// ((facing_dir-1)/3, truncating toward zero -- matches C++ signed `/` exactly) and writes that
// octant's (dx,dy) straight out of the fixed 8-entry table. NO GLOBAL IS WRITTEN --
// `shadow_region_closure.py` reports 0 direct / 0 transitive write cells (the two out-pointers land
// in the CALLER's stack, invisible to a region diff) and the function is `void`, so
// `compare_return` has nothing to compare either -- the write-set preflight correctly flags this
// NOT SHADOWABLE. Proven OFFLINE instead (tact_facing_to_delta_selftest.cpp).
//
// @0x00431216-0x0043121e: a dead CMP pair -- `CMP [out_dx],0` (JNZ over one more CMP) then
// `CMP [out_dy],0` -- both land on the SAME label (LAB_00431221) regardless of the result; neither
// CMP is followed by a Jcc that reads it. Preserved as a no-op, not transcribed.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_facing_to_delta @0x004311bf.
//
// 1. @0x004311de-0x004311ee: idx = (facing_dir - 1) / 3, IDIV (truncates toward zero).
// 2. @0x004311f1-0x00431202: *out_dx = dir8_delta_table[idx].dx.
// 3. @0x00431202-0x00431213: *out_dy = dir8_delta_table[idx].dy.
void facing_to_delta(const tact_view &v, int32_t facing_dir, int32_t *out_dx, int32_t *out_dy);

} // namespace detail

void facing_to_delta(int32_t facing_dir, int32_t *out_dx, int32_t *out_dy);

} // namespace mh::tact
