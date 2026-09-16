//
// crt/crt_qsort.h -- the vendored qsort (LIB-CRT, the VENDOR-EQUIV class).
//
// WHAT THIS REPLACES. `qsort` @0x004de8a6, 2 sites (ai_state.cpp, sim_pathfind_route_leg_group_and_
// sort.cpp) driving FOUR comparators between them. It is 0x439 bytes of Watcom CRT and it is
// transcribed here instruction-region by instruction-region, with the source VA on every block.
//
// ---- WHY A TRANSCRIPTION AND NOT std::sort -------------------------------------------------------
//
// Because the PERMUTATION OF EQUAL ELEMENTS is observable, and the tree already says so at both call
// sites in prose written before this file existed:
//
//   sim_pathfind_route_leg_group_and_sort.cpp:87  "never a host sort or a reimplemented comparator:
//                                                  a host sort permutes equal-distance ties
//                                                  differently than Watcom's qsort, a real lockstep
//                                                  divergence for group movement"
//   ai_state.h:1157                               "any other sort would produce a different (still
//                                                  'sorted') array -- a divergence the oracle would
//                                                  report and a desync if promoted"
//
// Neither sorted buffer is itself MF_HASH or MF_SAVE, but the ORDER drives what happens next: the
// dist sort assigns wave ranks, and the wave-rank sort decides the sequence in which group movement
// commits. So "sorted" is not the contract -- "sorted the way 0x004de8a6 sorts" is. Any sort that is
// merely correct is wrong here, which is why this is a transcription rather than a reimplementation
// and why `crttest` sweeps it against the original assembly on duplicate-heavy inputs.
//
// ---- WHAT THE ALGORITHM ACTUALLY IS --------------------------------------------------------------
//
// Not the textbook quicksort. Three things a casual reading gets wrong:
//
//   1. SMALL RANGES (num < 16) GO TO A GAP-3-THEN-GAP-1 SHELLSORT (@0x004de906), not to an insertion
//      sort. The gap loop steps the cursor by the GAP, not by one element (@0x004de970 adds
//      [EBP-0x18], the gap), so the first pass sorts only the subsequence 0,3,6,9,... Copying the
//      "obvious" insertion sort here changes the tie order for every partition of 15 or fewer.
//   2. THE PIVOT IS STAGED TWO DIFFERENT WAYS depending on element width (@0x004dea79). For a 4-byte
//      aligned element the pivot VALUE is copied into a local dword and the array is not disturbed;
//      for anything else the pivot ELEMENT is swapped to `base` and compared in place. The two give
//      different permutations, so the width test is part of the contract, not an optimisation.
//   3. IT IS A THREE-WAY (Bentley-McIlroy) PARTITION with equal-element regions collected at both
//      ends and block-swapped to the middle afterwards (@0x004debde, @0x004dec29) -- which is
//      exactly the machinery that decides where ties land.
//
// PIVOT SELECTION scales with size, and the thresholds are 0x1d and 0x2a rather than round numbers:
// median-of-3 above 29 elements, and a NINTHER (median of three medians, one from each eighth) above
// 42 (@0x004de9d0, @0x004de9f6).
//
// The explicit stack is 32 entries with NO overflow check, which is safe because the loop always
// pushes the LARGER subarray and iterates on the SMALLER (@0x004dec6f) -- the iterated half at least
// halves each time, so depth cannot exceed log2(SIZE_MAX/width).
//
// ---- THE COMPARATORS ARE THE OTHER HALF OF THIS ITEM ---------------------------------------------
//
// LIB-CRT's acceptance clause says "every comparator passed to vendored qsort is a translated C++
// body, none a VA". All four were raw `(void *)mh::addr::...` literals -- invisible to the VA census,
// because a comparator is passed as DATA, not called through `mh::call::`. They are now translated
// in their own DOMAIN and selected by MH_CRT_CMP (crt/crt_select.h):
//
//     llm_strat_ai_scan_target_sort_cmp  @0x004ec792  ai/ai_scan_target_sort_cmp.cpp (already had a
//                                                     verified body -- it was simply never wired in)
//     spiral_offset_sort_cmp             @0x004dc546  ai/ai_spiral_table_init.cpp
//     group_move_scratch_cmp_dist        @0x004cbe0c  sim/sim_pathfind_route_leg_group_and_sort.cpp
//     group_move_scratch_cmp_wave_rank   @0x004cbe42  sim/sim_pathfind_route_leg_group_and_sort.cpp
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_vendor_selftest.cpp), whose reference
// arm is this function's original machine code re-assembled into a `__declspec(naked)` body.
//
#pragma once

#include <cstdint>
#include <cstring>

namespace mh::crt {

// The comparator ABI on OUR side. The original's is __watcall (a in EAX, b in EDX); ours is the
// default __cdecl, which is why the comparator addresses at the call sites have to switch with the
// build rather than being passed through unchanged.
using qsort_cmp_t = int32_t (*)(void *, void *);

namespace detail {

// ---- CRT_004de828: swap `n` bytes between two elements -------------------------------------------
//
// Dwords first (n >> 2 of them), then the n & 3 remainder -- `MOVZX EDX,CL` takes the low byte of
// the count before `SHR ECX,2`, which is `n & 3` for every n. Spelled with byte copies here: the
// original's `XCHG dword ptr [ESI]` / `STOSD` pair is an UNALIGNED dword access whenever the caller
// selected this path, which is legal on x86 and undefined in C++.
inline void qsort_swap(uint8_t *a, uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t t = a[i];
        a[i]            = b[i];
        b[i]            = t;
    }
}

// The width == 4, aligned case (@0x004de94d and friends): a single dword exchange, inline rather
// than through the helper.
inline void qsort_swap4(uint8_t *a, uint8_t *b) {
    uint32_t ta, tb;
    std::memcpy(&ta, a, 4);
    std::memcpy(&tb, b, 4);
    std::memcpy(a, &tb, 4);
    std::memcpy(b, &ta, 4);
}

// ---- CRT_004de84e: median of three -----------------------------------------------------------
//
// Transcribed branch for branch, INCLUDING the number and order of comparator calls -- the
// comparators reachable here are pure, but a median that agrees on the result while calling the
// comparator a different number of times is a different function, and this file's whole claim is
// that it is not a different function.
inline uint8_t *qsort_med3(uint8_t *a, uint8_t *b, uint8_t *c, qsort_cmp_t cmp) {
    if (cmp(a, b) > 0) {                    // 0x004de860 / JLE 0x004de885
        if (cmp(a, c) > 0) {                // 0x004de86b / JLE 0x004de881
            return (cmp(b, c) > 0) ? b : c; // 0x004de876 / JG 0x004de89b else 0x004de87d
        }
        return a; // 0x004de881
    }
    if (cmp(a, c) >= 0) return a;   // 0x004de889 / JGE 0x004de881
    return (cmp(b, c) > 0) ? c : b; // 0x004de894 / JG 0x004de87d else 0x004de89b
}

} // namespace detail

// ---- qsort @0x004de8a6 ---------------------------------------------------------------------------
inline void qsort(void *base_, uint32_t num, uint32_t width, void *compare) {
    const qsort_cmp_t cmp  = reinterpret_cast<qsort_cmp_t>(compare);
    uint8_t          *base = static_cast<uint8_t *>(base_);

    // @0x004de8b4-0x004de8cd. `OR EAX,EBX` over (base | width), then `TEST AL,3`:
    //   2 -- base or width is not 4-aligned: swap byte-wise through the helper
    //   1 -- aligned but wider than 4 bytes: still the helper
    //   0 -- exactly the aligned 4-byte case: inline dword exchanges AND a by-value pivot
    // Only the 0-vs-nonzero distinction is ever tested afterwards (`CMP [EBP-0x1c],0`), so the 1/2
    // split carries no behaviour; it is kept because the reference arm computes it.
    const uint32_t base_or_width = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)) | width;
    const int      swap_kind     = ((base_or_width & 3u) != 0u) ? 2 : ((width > 4u) ? 1 : 0);

    const uint32_t w3 = width * 3u; // @0x004de8d0  [EBP-0x3c]
    const uint32_t w2 = width * 2u; // @0x004de8da  [EBP-0x38]

    // The explicit stack. 32 entries, no bound check -- see the banner.
    uint8_t *stk_base[32];
    uint32_t stk_num[32];
    uint32_t sp = 0u; // @0x004de8e2  [EBP-0x14]

    for (;;) {
        // ---- @0x004de8ec: the dispatch ------------------------------------------------------
        if (num <= 1u) {
            // fall through to the pop
        } else if (num < 16u) {
            // ---- @0x004de906: gap-3-then-gap-1 shellsort ------------------------------------
            uint8_t *const end = base + static_cast<size_t>(num) * width; // @0x004de90c [EBP-0x34]
            for (int32_t gap = static_cast<int32_t>(w3); gap > 0;
                 gap -= static_cast<int32_t>(w2)) {                      // @0x004de984 / 0x004de97e
                for (uint8_t *cur = base + gap; cur < end; cur += gap) { // @0x004de926 / 0x004de976
                    uint8_t *p = cur;                                    // @0x004de934
                    while (p > base) {                                   // @0x004de958
                        uint8_t *const q = p - gap;                      // @0x004de960
                        if (cmp(q, p) <= 0) break;                       // @0x004de969 / JG
                        if (swap_kind != 0) {
                            detail::qsort_swap(q, p, width); // @0x004de946
                        } else {
                            detail::qsort_swap4(q, p); // @0x004de94d
                        }
                        p = q; // @0x004de955
                    }
                }
            }
        } else {
            // ---- @0x004de9b9: pivot selection ----------------------------------------------
            uint8_t *mid = base + static_cast<size_t>(num >> 1) * width;
            if (num > 0x1du) {      // @0x004de9d0
                uint8_t *lo = base; // [EBP-0x30]
                uint8_t *hi = base + static_cast<size_t>(num - 1u) * width;
                if (num > 0x2au) {                                                              // @0x004de9f6 -- the ninther
                    const size_t off  = static_cast<size_t>(num >> 3) * width;                  // @0x004dea0b
                    const size_t off2 = off * 2u;                                               // @0x004dea1a
                    lo                = detail::qsort_med3(base, base + off, base + off2, cmp); // @0x004dea34
                    mid               = detail::qsort_med3(mid - off, mid, mid + off, cmp);     // @0x004dea4b
                    hi                = detail::qsort_med3(hi - off2, hi - off, hi, cmp);       // @0x004dea61
                }
                mid = detail::qsort_med3(lo, mid, hi, cmp); // @0x004dea72
            }

            // ---- @0x004dea79: stage the pivot ----------------------------------------------
            //
            // The two arms are NOT interchangeable. The wide arm moves the pivot ELEMENT to `base`
            // and compares against the array; the dword arm copies the pivot VALUE out and leaves
            // the array alone -- so the element that was at `base` stays there and takes part in the
            // partition. Different permutations, same sortedness.
            uint32_t pivot_value = 0u; // @0x004deaa9  [EBP-0x44]
            uint8_t *pivot_ptr;        // @0x004dea7f  [EBP-0x28]
            if (swap_kind != 0) {
                pivot_ptr = base;
                detail::qsort_swap(mid, base, width); // @0x004dea92
            } else {
                std::memcpy(&pivot_value, mid, 4); // @0x004deaaf
                pivot_ptr = reinterpret_cast<uint8_t *>(&pivot_value);
            }

            // ---- @0x004deab4: the three-way partition --------------------------------------
            uint8_t *lo_eq = base;                                         // [EBP-0xc]
            uint8_t *hi_eq = base + static_cast<size_t>(num - 1u) * width; // [EBP-0x8]
            uint8_t *r     = hi_eq;                                        // [EBP-0x4]
            uint8_t *l     = base;                                         // EBX

            for (;;) {
                while (l <= r) { // @0x004dead4
                    const int32_t t = cmp(l, pivot_ptr);
                    if (t > 0) break;                                            // @0x004deae3
                    if (t == 0) {                                                // @0x004deae5
                        if (swap_kind != 0) detail::qsort_swap(l, lo_eq, width); // @0x004deaf8
                        else detail::qsort_swap4(l, lo_eq);                      // @0x004deaff
                        lo_eq += width;                                          // @0x004deb0d
                    }
                    l += width; // @0x004deb16
                }
                while (l <= r) { // @0x004deb1e
                    const int32_t t = cmp(r, pivot_ptr);
                    if (t < 0) break;                                            // @0x004deb2e
                    if (t == 0) {                                                // @0x004deb30
                        if (swap_kind != 0) detail::qsort_swap(r, hi_eq, width); // @0x004deb44
                        else detail::qsort_swap4(r, hi_eq);                      // @0x004deb4b
                        hi_eq -= width;                                          // @0x004deb5f
                    }
                    r -= width; // @0x004deb68
                }
                if (l > r) break;                                    // @0x004deb76
                if (swap_kind != 0) detail::qsort_swap(l, r, width); // @0x004deb89
                else detail::qsort_swap4(l, r);                      // @0x004deb90
                l += width;                                          // @0x004deba4
                r -= width;                                          // @0x004debaa
            }

            // ---- @0x004debb2: fold the two equal blocks into the middle --------------------
            uint8_t *const end = base + static_cast<size_t>(num) * width; // [EBP-0x2c]

            uint32_t n_left = static_cast<uint32_t>(l - lo_eq);
            {
                const uint32_t eq_left = static_cast<uint32_t>(lo_eq - base);
                if (static_cast<int32_t>(eq_left) < static_cast<int32_t>(n_left))
                    n_left = eq_left;                             // @0x004debd8  signed CMP/JGE
                if (n_left != 0u)                                 // @0x004debde  TEST/JBE
                    detail::qsort_swap(l - n_left, base, n_left); // @0x004debf7
            }

            uint32_t n_right = static_cast<uint32_t>(hi_eq - r);
            {
                const uint32_t tail = static_cast<uint32_t>(end - hi_eq) - width; // @0x004dec11
                if (n_right >= tail) n_right = tail;                              // @0x004dec23  unsigned CMP/JC
                if (n_right != 0u)                                                // @0x004dec29
                    detail::qsort_swap(end - n_right, l, n_right);                // @0x004dec3f
            }

            // ---- @0x004dec59: push the LARGER half, iterate on the smaller -----------------
            const uint32_t less_bytes = static_cast<uint32_t>(l - lo_eq); // EDI
            const uint32_t more_bytes = static_cast<uint32_t>(hi_eq - r); // ESI
            uint8_t *const more_base  = end - more_bytes;                 // ECX

            if (more_bytes >= less_bytes) {        // @0x004dec6f  JC to the other arm
                stk_num[sp]  = more_bytes / width; // @0x004dec7f
                stk_base[sp] = more_base;          // @0x004dec8e
                num          = less_bytes / width; // @0x004decd1
                ++sp;                              // @0x004decd7
                continue;                          // base unchanged
            }
            // @0x004dec97. Both halves are trivial when the larger one is at most one element --
            // and it must be, since more_bytes < less_bytes <= width.
            if (less_bytes <= width) {
                // fall through to the pop
            } else {
                stk_base[sp] = base;               // @0x004decab
                stk_num[sp]  = less_bytes / width; // @0x004decbc
                base         = more_base;          // @0x004deccb
                num          = more_bytes / width; // @0x004decd1
                ++sp;
                continue;
            }
        }

        // ---- @0x004de98a: pop, or return when the stack is empty --------------------------
        if (sp == 0u) return; // @0x004de98e -- jumps to the epilogue Watcom shares with med3
        --sp;
        base = stk_base[sp];
        num  = stk_num[sp];
    }
}

} // namespace mh::crt
