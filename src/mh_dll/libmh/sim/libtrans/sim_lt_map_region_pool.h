#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls both allocator halves make, indirected (like every sim/ TU) so detail::
// stays drivable under libtranstest without reaching into the loaded game image. get_next_block
// uses only `malloc_`; pool_reset uses only `free_` (and forwards it to region_free's sibling call,
// which itself makes no outward call).
struct lt_map_region_pool_calls {
    void *(*malloc_)(uint32_t size); // utils_malloc @0x004d0155
    void (*free_)(void *p);          // utils_free   @0x004d0244
};

const lt_map_region_pool_calls &live_lt_map_region_pool_calls();

namespace detail {

// map_block_8_GetNextBlock @0x004222cf. Pop a node off the free list, or malloc a fresh 0x420-byte
// one if the free list is empty; either way register it in BY_INDEX, reset cell_count/
// neighbor_count/x/y/prev, push it onto the active list, and bump the live-node count. See the
// header banner for the full field-by-field derivation.
llm_map_region *get_next_block(sim_store &own, const lt_map_region_pool_calls &c);

// llm_map_region_free @0x0042239a. Walk the active list looking for `node`; if found, splice it out
// (head case rewrites LIST_HEAD, mid-list case rewrites the predecessor's `next`). Then --
// UNCONDITIONALLY, whether `node` was found or not -- clear its BY_INDEX slot, push it onto the
// free list, and decrement the live-node count. `node` is the typed pointer; the public wrapper
// does the uint32_t<->pointer cast (Ghidra gap G1, see header banner).
void region_free(sim_store &own, llm_map_region *node);

// llm_map_region_pool_reset @0x004234b8. Drain the whole active list through region_free (a plain
// in-TU sibling call, per translator-brief rule 3c -- the original's own CALL at 0x004234de targets
// llm_map_region_free directly), then utils_free every node left on the (now-swollen) free list,
// then reset the live-node count to 0 and the index allocator to 1 (NOT 0 -- index 0 is never
// allocated).
void pool_reset(sim_store &own, const lt_map_region_pool_calls &c);

} // namespace detail

// Live wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly --
// region_free's parameter is `uint32_t`, not `llm_map_region *` (Ghidra gap G1).
llm_map_region *get_next_block();
void            region_free(uint32_t region_ptr);
void            pool_reset();

} // namespace mh::sim
