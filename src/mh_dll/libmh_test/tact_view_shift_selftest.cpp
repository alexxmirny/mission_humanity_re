//
// tact_view_shift_selftest.cpp -- offline oracle for the llm_tact_view_shift_{col_inc,col_dec,
// row_inc,row_dec} quartet (TACT1E, 2026-08-27/28). See tact/tact_view_shift.h for the derivation.
//
// WHY OFFLINE: all four write through POINTER VARIABLES (gfx_framebuffer/tile_vis_map_ptr/
// los_cache_ptr loaded into a register first), so the static write-cell sweep sees only the pointer
// load, never the indirect store -- shadow_region_closure.py independently confirms 0 writable
// regions for all four (arm_ready:false). Proven here only.
//
// EVERY OFFSET BELOW WAS RE-DERIVED FROM THE RAW INSTRUCTION BYTES in
// tmp/decomp_tact/llm_tact_view_shift_{col_inc,col_dec,row_inc,row_dec}_*.asm (not copied from the
// .cpp under test), then cross-checked with an independent Python arithmetic pass -- the
// "cross-validate every derived number" rule. Each per-region write is fully disjoint from every
// other iteration's write for that region (destination stride always exceeds the copied length in
// every one of the four functions), so the expected byte at any destination offset can be computed
// directly from the ORIGINAL (pre-call) sentinel pattern at the corresponding source offset, without
// tracking intermediate mutation order -- except at the handful of offsets where a fill write lands
// on the same address a copy most recently read FROM; those are called out per case below (the read
// always happens before the fill in program order, so the fill value wins).
//
#include "tact/tact_view_shift.h"
#include "tact_test_support.h"

#include <cstdio>

namespace mh::tact::test {

namespace {

int              g_dirty_calls = 0;
void             mock_mark_view_tiles_dirty() { ++g_dirty_calls; }
view_shift_calls mock_calls() { return {mock_mark_view_tiles_dirty}; }

// Distinguishable, deterministic per-byte pattern -- NOT a constant, so a swapped/mis-offset copy is
// observable. MUST NOT be a simple `offset*odd_const & 0xFF` linear congruence: that has period 256,
// so it silently ALIASES (pattern(a) == pattern(b)) whenever a-b is a multiple of 256 -- which this
// file's own SRC_DELTA of 0x7800 (30720 = 120*256) for row_inc/row_dec IS. A first version of this
// oracle used exactly that linear form and a mutation test caught the alias: it missed a 4-byte
// framebuffer copy-length bug in row_inc because offset 960 and offset 960+0x7800 hashed identically.
// The Knuth multiplicative-hash top-byte below mixes every input bit into the result (via 32-bit
// carry propagation before the top-8-bit extraction), so it has no small-stride alias at any of this
// file's real deltas (64, 0x7800, 20, 15, 14) -- reverified after the fix caught the same mutation.
uint8_t pattern(size_t offset) { return (uint8_t)(((uint32_t)offset * 2654435761u) >> 24); }

void fill_pattern(std::vector<uint8_t> &buf) {
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = pattern(i);
}

// Byte-range check with a single ck() and a diagnostic scan on failure (avoids one ck_eq() per byte
// for ranges up to 964 bytes wide).
void ck_range_eq(const std::vector<uint8_t> &buf, size_t dst_lo, size_t src_lo, size_t len,
                 const char *what) {
    for (size_t k = 0; k < len; ++k) {
        if (buf[dst_lo + k] != pattern(src_lo + k)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s (first mismatch at k=%zu: got 0x%02x want 0x%02x)", what,
                     k, buf[dst_lo + k], pattern(src_lo + k));
            ck(false, msg);
            return;
        }
    }
    ck(true, what);
}

void ck_fill_eq(const std::vector<uint8_t> &buf, size_t lo, size_t len, uint8_t want,
                const char *what) {
    for (size_t k = 0; k < len; ++k) {
        if (buf[lo + k] != want) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s (first mismatch at k=%zu: got 0x%02x want 0x%02x)", what,
                     k, buf[lo + k], want);
            ck(false, msg);
            return;
        }
    }
    ck(true, what);
}

void ck_untouched(const std::vector<uint8_t> &buf, size_t offset, const char *what) {
    ck_eq((uint32_t)buf[offset], (uint32_t)pattern(offset), what);
}

} // namespace

void run_view_shift_tests() {
    // ============================================================ col_inc ============================
    // Framebuffer: dst_i=[i*1280,i*1280+896), src_i=dst_i+64, i=0..479. Gap [896,1280) untouched.
    {
        tact_fixture fx;
        fill_pattern(fx.framebuffer);
        fill_pattern(fx.tile_vis_map_buf);
        fill_pattern(fx.los_cache_buf);
        g_dirty_calls = 0;

        tact_store own = fx.store();
        detail::view_shift_col_inc(own, mock_calls());

        ck_range_eq(fx.framebuffer, 0, 64, 896, "col_inc: framebuffer i=0, 0x0043c0e2-0x0043c0f6");
        ck_range_eq(fx.framebuffer, 613120, 613184, 896, "col_inc: framebuffer i=479 (last row)");
        ck_range_eq(fx.framebuffer, 307200, 307264, 896, "col_inc: framebuffer i=240 (middle row)");
        ck_untouched(fx.framebuffer, 896, "col_inc: framebuffer gap canary (tight, immediately past the 896 B copy) untouched");

        ck_range_eq(fx.tile_vis_map_buf, 0, 1, 14, "col_inc: tile_vis_map i=0 copy, 0x0043c108-0x0043c11c");
        ck_fill_eq(fx.tile_vis_map_buf, 14, 1, 2, "col_inc: tile_vis_map i=0 fill=0x2, 0x0043c11f");
        ck_range_eq(fx.tile_vis_map_buf, 380, 381, 14, "col_inc: tile_vis_map i=19 copy (last row)");
        ck_fill_eq(fx.tile_vis_map_buf, 394, 1, 2, "col_inc: tile_vis_map i=19 fill=0x2");
        ck_range_eq(fx.tile_vis_map_buf, 180, 181, 14, "col_inc: tile_vis_map i=9 copy (middle row)");
        ck_untouched(fx.tile_vis_map_buf, 17, "col_inc: tile_vis_map gap canary (stride 20, len 14+1) untouched");

        ck_range_eq(fx.los_cache_buf, 0, 1, 14, "col_inc: los_cache i=0 copy, 0x0043c12b-0x0043c13f");
        ck_fill_eq(fx.los_cache_buf, 14, 1, 0, "col_inc: los_cache i=0 fill=0x0 (NOT 0x2 -- distinct from tile_vis_map's fill), 0x0043c142");
        ck_range_eq(fx.los_cache_buf, 380, 381, 14, "col_inc: los_cache i=19 copy (last row)");
        ck_fill_eq(fx.los_cache_buf, 394, 1, 0, "col_inc: los_cache i=19 fill=0x0");

        ck_eq((uint32_t)g_dirty_calls, 1u, "col_inc: mark_view_tiles_dirty called exactly once, 0x0043c14e");
    }

    // ============================================================ col_dec ============================
    // Framebuffer: dst_i (backward) = [613184-i*1280, 614080-i*1280), i=0..479.
    {
        tact_fixture fx;
        fill_pattern(fx.framebuffer);
        fill_pattern(fx.tile_vis_map_buf);
        fill_pattern(fx.los_cache_buf);
        g_dirty_calls = 0;

        tact_store own = fx.store();
        detail::view_shift_col_dec(own, mock_calls());

        ck_range_eq(fx.framebuffer, 613184, 613120, 896, "col_dec: framebuffer i=0, 0x0043c156-0x0043c171");
        ck_range_eq(fx.framebuffer, 64, 0, 896, "col_dec: framebuffer i=479 (last row)");
        ck_range_eq(fx.framebuffer, 305984, 305920, 896, "col_dec: framebuffer i=240 (middle row)");
        ck_untouched(fx.framebuffer, 613183, "col_dec: framebuffer gap canary (tight, immediately below i=0's [613184,614080) range) untouched");

        // i=0: fill_pos(380) == src_low(380), the copy's own last-read source byte -- the copy READS
        // it (as part of src=[380,394)) before the fill WRITES it, so the final value is the fill
        // constant, not the shifted pattern. Check the copy range EXCLUDING that shared byte, plus the
        // fill separately.
        ck_range_eq(fx.tile_vis_map_buf, 382, 381, 13, "col_dec: tile_vis_map i=0 copy (offsets 382..394), 0x0043c183-0x0043c19d");
        ck_fill_eq(fx.tile_vis_map_buf, 380, 1, 2, "col_dec: tile_vis_map i=0 fill=0x2 wins over the byte the copy also read as src, 0x0043c1a0");
        ck_range_eq(fx.tile_vis_map_buf, 2, 1, 13, "col_dec: tile_vis_map i=19 copy (last row, offsets 2..14)");
        ck_fill_eq(fx.tile_vis_map_buf, 0, 1, 2, "col_dec: tile_vis_map i=19 fill=0x2 (final write to offset 0)");
        ck_range_eq(fx.tile_vis_map_buf, 201, 200, 14, "col_dec: tile_vis_map i=9 copy (middle row, no fill-overlap)");
        ck_untouched(fx.tile_vis_map_buf, 377, "col_dec: tile_vis_map gap canary (positions 375-379) untouched");

        ck_fill_eq(fx.los_cache_buf, 380, 1, 0, "col_dec: los_cache i=0 fill=0x0, 0x0043c1c9");
        ck_range_eq(fx.los_cache_buf, 382, 381, 13, "col_dec: los_cache i=0 copy (offsets 382..394)");
        ck_fill_eq(fx.los_cache_buf, 0, 1, 0, "col_dec: los_cache i=19 fill=0x0 (final write to offset 0)");

        ck_eq((uint32_t)g_dirty_calls, 1u, "col_dec: mark_view_tiles_dirty called exactly once, 0x0043c1d6");
    }

    // ============================================================ row_inc ============================
    // Framebuffer: dst_i=[i*1280,i*1280+960), src_i=dst_i+30720, i=0..455.
    {
        tact_fixture fx;
        fill_pattern(fx.framebuffer);
        fill_pattern(fx.tile_vis_map_buf);
        fill_pattern(fx.los_cache_buf);
        g_dirty_calls = 0;

        tact_store own = fx.store();
        detail::view_shift_row_inc(own, mock_calls());

        ck_range_eq(fx.framebuffer, 0, 30720, 960, "row_inc: framebuffer i=0, 0x0043c1de-0x0043c1f5");
        ck_range_eq(fx.framebuffer, 582400, 613120, 960, "row_inc: framebuffer i=455 (last block)");
        ck_range_eq(fx.framebuffer, 290560, 321280, 960, "row_inc: framebuffer i=227 (middle block)");
        ck_untouched(fx.framebuffer, 960, "row_inc: framebuffer gap canary (tight, immediately past the 960 B copy -- NOT col_inc's 896) untouched");

        // tile_vis_map/los_cache: NO per-row fill (unlike col_inc/col_dec) -- ONE trailing 15-byte
        // block fill at the post-loop position. Copy chunk is 0xf (15) bytes, not col's 0xe (14).
        ck_range_eq(fx.tile_vis_map_buf, 0, 20, 15, "row_inc: tile_vis_map i=0 copy (15 B, not 14), 0x0043c207-0x0043c21b");
        ck_range_eq(fx.tile_vis_map_buf, 360, 380, 15, "row_inc: tile_vis_map i=18 copy (last row of the loop)");
        ck_range_eq(fx.tile_vis_map_buf, 180, 200, 15, "row_inc: tile_vis_map i=9 copy (middle row)");
        ck_fill_eq(fx.tile_vis_map_buf, 380, 15, 2,
                   "row_inc: tile_vis_map ONE trailing 15-B fill=0x2 at the post-loop position "
                   "(overlaps i=18's own src range, which was already read before this fill runs), "
                   "0x0043c227-0x0043c231");
        ck_untouched(fx.tile_vis_map_buf, 17, "row_inc: tile_vis_map gap canary (stride 20, len 15) untouched");

        ck_range_eq(fx.los_cache_buf, 0, 20, 15, "row_inc: los_cache i=0 copy, 0x0043c233-0x0043c247");
        ck_fill_eq(fx.los_cache_buf, 380, 15, 0, "row_inc: los_cache trailing fill=0x0 (distinct from tile_vis_map's 0x2), 0x0043c253-0x0043c25d");

        ck_eq((uint32_t)g_dirty_calls, 1u, "row_inc: mark_view_tiles_dirty called exactly once, 0x0043c25f");
    }

    // ============================================================ row_dec ============================
    // Framebuffer: dst_i (backward) = [613120-i*1280, 614084-i*1280), i=0..455 -- 964 B/block, NOT
    // row_inc's 960 (the flagged asymmetry: 0xf1 dwords vs 0xf0). Same total 1280 stride either way.
    {
        tact_fixture fx;
        fill_pattern(fx.framebuffer);
        fill_pattern(fx.tile_vis_map_buf);
        fill_pattern(fx.los_cache_buf);
        g_dirty_calls = 0;

        tact_store own = fx.store();
        detail::view_shift_row_dec(own, mock_calls());

        ck_range_eq(fx.framebuffer, 613120, 582400, 964, "row_dec: framebuffer i=0 -- 964 B, NOT row_inc's 960, 0x0043c267-0x0043c285");
        ck_range_eq(fx.framebuffer, 30720, 0, 964, "row_dec: framebuffer i=455 (last block) -- reaches exactly offset 0, no underflow");
        ck_range_eq(fx.framebuffer, 322560, 291840, 964, "row_dec: framebuffer i=227 (middle block)");
        ck_untouched(fx.framebuffer, 613119, "row_dec: framebuffer gap canary (tight, immediately below i=0's [613120,614084) range -- NOT row_inc's 960 B boundary) untouched");

        // tile_vis_map/los_cache: backward, no per-row fill, ONE trailing 15-B fill at [0,15) (the
        // post-loop position, which is ALSO i=18's own src range -- same read-before-write ordering
        // as col_dec's per-row case, just once instead of per-iteration).
        ck_range_eq(fx.tile_vis_map_buf, 380, 360, 15, "row_dec: tile_vis_map i=0 copy, 0x0043c297-0x0043c2b1");
        ck_range_eq(fx.tile_vis_map_buf, 20, 0, 15, "row_dec: tile_vis_map i=18 copy (last row of the loop) -- note this READS [0,15), which the trailing fill below then overwrites");
        ck_fill_eq(fx.tile_vis_map_buf, 0, 15, 2,
                   "row_dec: tile_vis_map ONE trailing 15-B fill=0x2 at [0,15), overwriting what i=18 "
                   "just read as its own source, 0x0043c2bd-0x0043c2c7");
        ck_untouched(fx.tile_vis_map_buf, 377, "row_dec: tile_vis_map gap canary (positions 375-379) untouched");

        ck_range_eq(fx.los_cache_buf, 380, 360, 15, "row_dec: los_cache i=0 copy, 0x0043c2c9-0x0043c2e3");
        ck_fill_eq(fx.los_cache_buf, 0, 15, 0, "row_dec: los_cache trailing fill=0x0, 0x0043c2ef-0x0043c2f9");

        ck_eq((uint32_t)g_dirty_calls, 1u, "row_dec: mark_view_tiles_dirty called exactly once, 0x0043c2fc");
    }
}

} // namespace mh::tact::test
