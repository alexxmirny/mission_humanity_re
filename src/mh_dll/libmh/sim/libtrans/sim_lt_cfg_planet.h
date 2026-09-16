#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three real outward calls this function makes -- see the header banner's CALLEES section.
struct cfg_planet_construct_calls {
    uint32_t (*read_map_file)(mh::game::mh_cfg_struct_map_header *map_header); // cfg_ReadMapFile @0x004a3b9d
    uint8_t (*get_tlo_index)(char *file_name);                                 // cfg_GetTloIndex @0x004b9582
    // cfg_final_planet_FillBankData @0x0045e306 IS GONE (LIFT-TABLE S3, 2026-09-09) -- it left with
    // the rest of the graphics tail onto LIBMH_EVK_INV_PLANET_GFX_SETUP, and with it the host-API
    // entry: the tail's eight calls were its only libmh caller.
};

const cfg_planet_construct_calls &live_cfg_planet_construct_calls();

namespace detail {

// cfg_final_planet_Construct @0x0045b69c. See the header banner for the full derivation; the .cpp
// carries the per-write address citation. The by-value 60-byte planet_data blob is marshalled as a
// pointer by the export thunk, and it is `const` here: LIB-CONSTSIG (2026-09-04) established that a
// by-value blob has TWO committed spellings for two DIRECTIONS -- addr/mh_calls.gen.h's CALL
// direction (`const T *`, the caller lends its buffer and the thunk copies it) and
// addr/mh_export.gen.h::sig_<fn>'s ENTRY direction (`T *`, our replacement body owns the pushed
// copy). This body never writes through the pointer (~25 reads, no store; the apparent
// `planet_data.info_txt += 2` is register-only -- `MOV ESI,[EBP+0x1c]` once, then `ADD ESI,0x2`,
// never stored back), so it takes the CALL spelling --
// which is what lets the standalone binder rebind the calls-struct member at
// sim/resid/sim_scenario_planet_clone.h onto it (LIB-REBIND R2/R4). The promoted arm in
// sim_lt_promote.cpp still matches sig_<fn> exactly and passes its non-const pointer in. Takes no
// `sim_view` -- this function reads nothing through the read view; every input arrives via a
// parameter or a callee return.
void cfg_final_planet_Construct(sim_store &own, const cfg_planet_construct_calls &c,
                                int32_t define_index, int32_t invention_index, char *map_name,
                                const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                char                                     *path_unc);

} // namespace detail

// Live wrapper: the logic applied to state().own, with the real outward calls bound live.
void cfg_final_planet_Construct(int32_t define_index, int32_t invention_index, char *map_name,
                                const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                char                                     *path_unc);

} // namespace mh::sim
