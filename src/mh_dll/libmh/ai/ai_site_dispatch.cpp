//
// ai/ai_site_dispatch.cpp -- see ai_site_dispatch.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_bldg_production_type_dispatch_004e7cec.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h and addr/mh_addrs.gen.h rather than
// transcribed:
//   0x00fe5b54 = _G_LLM_STRAT_AI_SITE_CANDIDATE_COUNT   (a standalone global, bound in ai_store)
//   0xe7e380   = player_data + 0x104c0 = is_alien_race  (player stride 166140)
//   0xd9ec88   = Building    + 0x8     = Building[id].type   (IMUL 0x842 = the Building stride)
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_site_dispatch.h"


namespace mh::ai {
namespace detail {

void bldg_production_type_dispatch(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t building_id) {
    // MOV dword ptr [0x00fe5b54],0x0 @0x004e7cfa -- FIRST, before any dispatch, and the only reset
    // in the image. See the header.
    *own.site_candidate_count = 0;

    // The type is re-read from the cfg table at each test in the original (three separate
    // IMUL/MOVZX pairs); one read here, because nothing can change it in between -- there is no
    // call until after the last test.
    const uint8_t bldg_type = v.cfg_buildings[building_id].type;

    // is_alien_race is likewise re-read at each test (0x004e7d16 / 0x004e7d5d / 0x004e7d98). Kept
    // as three reads so the transcription matches the listing an auditor is holding; the value is
    // const through the whole body.
    if (bldg_type == race_mine_type(v.players[player_id].is_alien_race)) {
        gc.bldg_scan_resource_site_candidates(player_id, building_id); // 0x004e7d3f
    } else if (bldg_type == race_garage_type(v.players[player_id].is_alien_race) ||
               bldg_type == race_barracks_type(v.players[player_id].is_alien_race)) {
        // Both branches converge on LAB_004e7dbf; the garage test short-circuits the barracks one
        // exactly as `||` does (JZ 0x004e7dbf @0x004e7d82).
        gc.scan_build_site_candidates((int32_t)player_id, building_id); // 0x004e7dc1
    } else {
        gc.bldg_scan_grid_candidates(player_id, building_id); // 0x004e7dca
    }

    gc.sort_site_candidates_by_dist(); // CALL @0x004e7dcf -- ordinary, not a tail call
}

} // namespace detail

void bldg_production_type_dispatch(uint32_t player_id, int32_t building_id) {
    const ai_state st = state();
    detail::bldg_production_type_dispatch(st.read, st.own, live_calls(), player_id, building_id);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// NOTHING IS STUBBED, and the site declares its callees' regions. All four calls write only the
// site-candidate array and its count -- the scanners append, the sort permutes in place -- both of
// which this site declares via extra_regions, so the restore between the arms undoes them. Stubbing
// them would instead GUARANTEE a divergence, because the original arm would have filled the list and
// ours would not.
//
// This is a DELEGATING function: the state matrix measures only the one direct write (the count),
// so without extra_regions the arm's candidate writes would be neither restored nor compared and
// would pile on top of the original's.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. Three of the four exits are branch-selected, and on a base
// that only ever queues one class of building a run can take one of them exclusively. The arm
// therefore counts each branch separately: a `grid=N mine=0 build=0` line means the dispatch itself
// was never exercised, only its default.

} // namespace mh::ai
