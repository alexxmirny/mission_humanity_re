//
// tact/tact_view_shift.cpp -- see tact_view_shift.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_view_shift_{col_inc,col_dec,row_inc,row_dec}_*.asm), not from Ghidra's
// .c.
//
// Every REP MOVS[BD]/REP STOSB block below is reproduced as a real C++ loop using
// std::memmove/std::memset over the exact byte ranges the CPU touches -- not a literal
// PUSHAD/REP-prefix transcription. std::memmove is used (not memcpy) throughout because every
// source/destination pair here overlaps (the whole point of a scroll shift): memmove's contract
// guarantees the correct result regardless of which physical direction the original ran the copy
// in, provided the (destination, source, length) triple names the same byte range the original
// touched -- which is what the per-function derivation in each loop below establishes.
//
// THE *_DEC FUNCTIONS RUN BACKWARD (STD): the CPU's EDI/ESI register value at the top of each
// outer-loop iteration is the HIGHEST address touched that iteration, not the lowest -- REP MOVS
// with the direction flag set writes/reads at the current pointer and then DECREMENTS. To get the
// (low-address, length) pair std::memmove needs, each backward loop below converts the recorded
// "top" pointer with `top - (chunk_bytes - elem_size)`, i.e. `top - chunk_bytes + elem_size`: the
// lowest byte address is elem_size below the address the last element's chunk_bytes-sized retreat
// would otherwise reach. This conversion is algebra, not a literal read -- see this file's
// uncertainties.
//
#include "tact/tact_view_shift.h"

#include <cstring>

#include "state/host_events.h" // LIFT-EVQ: the pilot event-channel emit

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites

namespace mh::tact {

const view_shift_calls &live_view_shift_calls() {
    // LIFT-EVQ pilot conversion (2026-09-03): the fine host-callback entry became an
    // invalidate-channel record. Hosted, mh.dll's sink routes the record back onto the
    // original thunk SYNCHRONOUSLY at emit, so behavior is bit-identical; a poll host drains
    // it at frame edge; the suites' own bindings stay their stubs.
    static const view_shift_calls c = {
        [] { mh::state::emit_invalidate(LIBMH_EVK_INV_TACT_VIEW_TILES); }};
    return c;
}

namespace detail {

void view_shift_col_inc(tact_store &own, const view_shift_calls &c) {
    // @0x0043c0e2-0x0043c106: framebuffer, forward -- dst/src start at the lowest address each
    // iteration touches, so no top->low conversion is needed here.
    {
        uint8_t *dst = own.gfx_framebuffer();
        uint8_t *src = dst + 0x40;
        for (int32_t row = 0; row < 0x1e0; ++row) {
            std::memmove(dst, src, 0x380);
            dst += 0x380 + 0x180; // the copy's own +0x380 advance, plus the explicit ADD 0x180
            src += 0x380 + 0x180;
        }
    }

    // @0x0043c108-0x0043c129: tile_vis_map, forward. The fill happens EVERY iteration, at the byte
    // immediately past the 14-byte copy (matching the .asm's own MOV-after-REP-MOVSB order).
    {
        uint8_t *dst = own.tile_vis_map_ptr();
        uint8_t *src = dst + 1;
        for (int32_t row = 0; row < 0x14; ++row) {
            std::memmove(dst, src, 0xe);
            dst += 0xe;
            src += 0xe;
            *dst = 0x2;
            dst += 6;
            src += 6;
        }
    }

    // @0x0043c12b-0x0043c14c: LOS cache, forward -- identical shape, fill value 0x0.
    {
        uint8_t *dst = own.los_cache_ptr();
        uint8_t *src = dst + 1;
        for (int32_t row = 0; row < 0x14; ++row) {
            std::memmove(dst, src, 0xe);
            dst += 0xe;
            src += 0xe;
            *dst = 0x0;
            dst += 6;
            src += 6;
        }
    }

    // @0x0043c14e.
    c.mark_view_tiles_dirty();
}

void view_shift_col_dec(tact_store &own, const view_shift_calls &c) {
    // @0x0043c156-0x0043c181: framebuffer, backward. `top` is EDI's value at loop entry each
    // iteration (the highest byte of that iteration's 0x380-byte range); the range's low address
    // is `top - 0x380 + 4` (elem_size 4, REP MOVSD).
    {
        uint8_t *dst_top = own.gfx_framebuffer() + 0x95ebc;
        uint8_t *src_top = dst_top - 0x40;
        for (int32_t row = 0; row < 0x1e0; ++row) {
            std::memmove(dst_top - 0x380 + 4, src_top - 0x380 + 4, 0x380);
            dst_top -= 0x500; // the copy's own -0x380 retreat, plus the explicit SUB 0x180
            src_top -= 0x500;
        }
    }

    // @0x0043c183-0x0043c1aa: tile_vis_map, backward (REP MOVSB, elem_size 1: low = top-14+1 =
    // top-0xd). The fill happens every iteration at the post-copy register position (top-0xe),
    // i.e. the byte immediately BELOW the copied span -- mirroring col_inc's "immediately above".
    {
        uint8_t *dst_top = own.tile_vis_map_ptr() + 0x18a;
        uint8_t *src_top = dst_top - 1;
        for (int32_t row = 0; row < 0x14; ++row) {
            std::memmove(dst_top - 0xd, src_top - 0xd, 0xe);
            dst_top -= 0xe;
            src_top -= 0xe;
            *dst_top = 0x2;
            dst_top -= 6;
            src_top -= 6;
        }
    }

    // @0x0043c1ac-0x0043c1d3: LOS cache, backward -- identical shape, fill value 0x0.
    {
        uint8_t *dst_top = own.los_cache_ptr() + 0x18a;
        uint8_t *src_top = dst_top - 1;
        for (int32_t row = 0; row < 0x14; ++row) {
            std::memmove(dst_top - 0xd, src_top - 0xd, 0xe);
            dst_top -= 0xe;
            src_top -= 0xe;
            *dst_top = 0x0;
            dst_top -= 6;
            src_top -= 6;
        }
    }

    // @0x0043c1d6 (CLD @0x0043c1d5 has no C++ equivalent -- it only resets the CPU direction flag).
    c.mark_view_tiles_dirty();
}

void view_shift_row_inc(tact_store &own, const view_shift_calls &c) {
    // @0x0043c1de-0x0043c205: framebuffer, forward.
    {
        uint8_t *dst = own.gfx_framebuffer();
        uint8_t *src = dst + 0x7800;
        for (int32_t blk = 0; blk < 0x1c8; ++blk) {
            std::memmove(dst, src, 0x3c0);
            dst += 0x3c0 + 0x140;
            src += 0x3c0 + 0x140;
        }
    }

    // @0x0043c207-0x0043c225: tile_vis_map, forward -- NO per-row fill (unlike col_inc); the fill
    // is a single trailing block, step 3 below.
    uint8_t *tv_dst = own.tile_vis_map_ptr();
    {
        uint8_t *src = tv_dst + 0x14;
        for (int32_t row = 0; row < 0x13; ++row) {
            std::memmove(tv_dst, src, 0xf);
            tv_dst += 0xf + 5;
            src += 0xf + 5;
        }
    }
    // @0x0043c227-0x0043c231: one 15-byte block-fill of 0x2 at the position the loop above ended on.
    std::memset(tv_dst, 0x2, 0xf);

    // @0x0043c233-0x0043c251: LOS cache, forward -- identical shape.
    uint8_t *los_dst = own.los_cache_ptr();
    {
        uint8_t *src = los_dst + 0x14;
        for (int32_t row = 0; row < 0x13; ++row) {
            std::memmove(los_dst, src, 0xf);
            los_dst += 0xf + 5;
            src += 0xf + 5;
        }
    }
    // @0x0043c253-0x0043c25d: one 15-byte block-fill of 0x0.
    std::memset(los_dst, 0x0, 0xf);

    // @0x0043c25f.
    c.mark_view_tiles_dirty();
}

void view_shift_row_dec(tact_store &own, const view_shift_calls &c) {
    // @0x0043c267-0x0043c295: framebuffer, backward. NOTE the chunk here is 0xf1 dwords (0x3c4
    // bytes), NOT row_inc's 0xf0/0x3c0 -- preserved literally, see this TU's header banner. low =
    // top - 0x3c4 + 4 = top - 0x3c0.
    {
        uint8_t *dst_top = own.gfx_framebuffer() + 0x95ec0;
        uint8_t *src_top = dst_top - 0x7800;
        for (int32_t blk = 0; blk < 0x1c8; ++blk) {
            std::memmove(dst_top - 0x3c0, src_top - 0x3c0, 0x3c4);
            dst_top -= 0x500; // the copy's own -0x3c4 retreat, plus the explicit SUB 0x13c (0x3c4+0x13c=0x500)
            src_top -= 0x500;
        }
    }

    // @0x0043c297-0x0043c2bb: tile_vis_map, backward -- no per-row fill (mirrors row_inc's shape).
    // low = top - 0xf + 1 = top - 0xe.
    uint8_t *tv_top = own.tile_vis_map_ptr() + 0x18a;
    {
        uint8_t *src_top = tv_top - 0x14;
        for (int32_t row = 0; row < 0x13; ++row) {
            std::memmove(tv_top - 0xe, src_top - 0xe, 0xf);
            tv_top -= 0xf + 5;
            src_top -= 0xf + 5;
        }
    }
    // @0x0043c2bd-0x0043c2c7: one 15-byte block-fill of 0x2. REP STOSB runs backward here too (DF
    // still set), but a uniform fill value makes the direction unobservable -- the touched range is
    // [tv_top-0xe, tv_top] either way.
    std::memset(tv_top - 0xe, 0x2, 0xf);

    // @0x0043c2c9-0x0043c2ed: LOS cache, backward -- identical shape.
    uint8_t *los_top = own.los_cache_ptr() + 0x18a;
    {
        uint8_t *src_top = los_top - 0x14;
        for (int32_t row = 0; row < 0x13; ++row) {
            std::memmove(los_top - 0xe, src_top - 0xe, 0xf);
            los_top -= 0xf + 5;
            src_top -= 0xf + 5;
        }
    }
    // @0x0043c2ef-0x0043c2f9: one 15-byte block-fill of 0x0.
    std::memset(los_top - 0xe, 0x0, 0xf);

    // @0x0043c2fc (CLD @0x0043c2fb has no C++ equivalent).
    c.mark_view_tiles_dirty();
}

} // namespace detail

void view_shift_col_inc() {
    tact_state st = state();
    detail::view_shift_col_inc(st.own, live_view_shift_calls());
}
void view_shift_col_dec() {
    tact_state st = state();
    detail::view_shift_col_dec(st.own, live_view_shift_calls());
}
void view_shift_row_inc() {
    tact_state st = state();
    detail::view_shift_row_inc(st.own, live_view_shift_calls());
}
void view_shift_row_dec() {
    tact_state st = state();
    detail::view_shift_row_dec(st.own, live_view_shift_calls());
}


// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// This TU used to carry counter-wrapped `promoted_arm::` adapters and an MH_EXPORT_REPLACE install
// for each, so our bodies could take the game's entry points. The fork's config selector has two
// hosted answers, `original` and `brokered`, and TACTICAL MODE IS ORIGINAL IN BOTH: the reimplemented
// spine the fork ships is the strategic one. So the install surface has no configuration left to be
// armed in, and an installer nothing can arm is not a dormant feature, it is a claim about what runs
// that is false in every run.
//
// THE BODIES ABOVE ARE UNTOUCHED and stay reachable two ways: the offline oracle (net_selftest
// tacttest) drives them directly, and their rebind rows survive (fork ruling Q2 -- the BIND survives,
// only the per-row runtime gate died), so a standalone host binds them unconditionally. What is gone
// is only the route that overwrote the game's own entry inside a hosted process.

} // namespace mh::tact
