#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest.
struct pathfinder_init_calls {
    void *(*malloc_struct_array)(uint32_t array_size, uint32_t struct_size); // utils_malloc_struct_array @0x0041100b
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t value);             // utils_fill_data @0x004d1780
    void (*abort)(int32_t status);                                           // utils_abort @0x004da944
};

const pathfinder_init_calls &live_pathfinder_init_calls();

namespace detail {

// llm_strat_pathfinder_init @0x004614f9. Reads only `v.passable` (for the params block's `passable`
// field); writes the job-result table (`own`), the pathfinder workbuf/params pointers (`own`, see
// the declared_needs accessors above), and the params block's three fields through the freshly
// allocated pointer itself (heap memory, not a registered region -- only reachable through the
// pointer this function itself just stored). Reaches every callee (all frontier) through `c`. void
// return, matching the original.
void pathfinder_init(const sim_view &v, sim_store &own, const pathfinder_init_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_pathfinder_init_calls().
void pathfinder_init();

} // namespace mh::sim
