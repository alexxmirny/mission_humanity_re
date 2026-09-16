//
// crt/crt_heap.h -- the vendored allocator (LIB-CRT, the HEAP class).
//
// WHAT THIS REPLACES. Four callees, 10 sites:
//
//     utils_malloc              @0x004d0155   2 sites
//     utils_free                @0x004d0244   5 sites
//     struct_array_malloc_impl  @0x004d013d   2 sites   calloc, in Watcom's spelling
//     utils_malloc_struct_array @0x0041100b   1 site    a stack-probe wrapper over the above
//
// ---- THIS IS THE ONE FAMILY WITH NO BIT-EQUIVALENCE CLAIM, AND THAT IS MEASURED -----------------
//
// The others in this directory reproduce the original's OUTPUT. This one cannot and does not: a
// standalone libmh's allocator hands out different ADDRESSES than the game's Watcom rover
// (@0x004d018e's free-list walk over the DGROUP arena, @0x004e05c2 __MemAllocator). The user approved
// using our own allocator (2026-09-08) on the strength of the following measurement, which is the
// only thing that makes an address difference harmless -- re-run it if the region table changes:
//
//     ACROSS THE WHOLE GENERATED REGION TABLE there are just EIGHT 4-byte MF_HASH regions, every one
//     of them a count or a cached dimension, and ZERO pointer-shaped 4-byte MF_SAVE regions. So no
//     allocated address reaches the lockstep hash or a savegame, and two peers running different
//     allocators still agree.
//
// What IS reproduced is the observable CONTRACT, because callers branch on it:
//
//   * size 0 returns NULL (@0x004d0165 `TEST EAX,EAX / JZ`) -- NOT a unique 1-byte block the way
//     `std::malloc(0)` is permitted to. `sim_lt_map_region_pool` tests the result.
//   * size > 0xFFFFFFD4 returns NULL (@0x004d0169 `CMP EAX,-0x2c / JBE`), the overflow guard for the
//     block header. Reproduced so a `count * struct_size` that wrapped near the top of the address
//     space still fails the same way rather than reaching std::malloc with an absurd request.
//   * free(NULL) is a no-op (@0x004d024b).
//   * struct_array_malloc_impl ZEROES the block (@0x004d014c `XOR EDX,EDX` then utils_fill_data), and
//     multiplies count * struct_size in WRAPPING 32-bit arithmetic (@0x004d013e `IMUL EAX,EDX`) with
//     no overflow check. The zero fill uses the WRAPPED size, so an overflowing request that still
//     passes the guard above allocates and clears the wrapped amount. That is the original's
//     behaviour and reproducing it is the point -- a "safer" checked multiply would return NULL where
//     the original returns a block, which is a behaviour change, not a bug fix.
//
// Alignment: the original rounds the request up to 4 with a 12-byte floor (@0x004d017a, @0x004d0190).
// `std::malloc` gives at least 8 on Win32, so every alignment the game's own blocks satisfied is
// still satisfied; the size rounding is invisible to a caller and is not reproduced.
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_vendor_selftest.cpp), which asserts
// the contract above rather than an address.
//
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mh::crt {

// The largest request the original accepts: `CMP EAX,-0x2c / JBE` @0x004d0169 lets 0xffffffd4
// through and rejects everything above it.
inline constexpr uint32_t MALLOC_MAX = 0xffffffd4u;

// ---- utils_malloc @0x004d0155 --------------------------------------------------------------------
inline void *utils_malloc(uint32_t size) {
    if (size == 0u || size > MALLOC_MAX) return nullptr;
    return std::malloc(size);
}

// ---- utils_free @0x004d0244 ----------------------------------------------------------------------
inline void utils_free(void *param_1) {
    if (param_1 == nullptr) return; // @0x004d024b -- the original returns without touching the heap
    std::free(param_1);
}

// ---- struct_array_malloc_impl @0x004d013d --------------------------------------------------------
//
// calloc, with the wrapping multiply described in the banner. Note the original returns the pointer
// from utils_fill_data (@0x004d014e), which returns its own first argument -- so the value returned
// is the block, not the end of it.
inline void *struct_array_malloc_impl(uint32_t count, uint32_t struct_size) {
    const uint32_t bytes = count * struct_size; // deliberately wrapping, as `IMUL EAX,EDX` is
    void          *p     = utils_malloc(bytes);
    if (p != nullptr) std::memset(p, 0, bytes);
    return p;
}

// ---- utils_malloc_struct_array @0x0041100b -------------------------------------------------------
//
// A pure forwarder. Its whole body is the inert `PUSH 0x24 / CALL utils_assert_stack_capacity`
// prologue plus a tail call to struct_array_malloc_impl (@0x0041102e) -- the prologue touches no
// tracked state, so it is omitted under translator-brief rule 6.
inline void *utils_malloc_struct_array(uint32_t array_size, uint32_t struct_size) {
    return struct_array_malloc_impl(array_size, struct_size);
}

} // namespace mh::crt
