//
// sim/sim_bldg_reset_construction_anim.cpp -- see sim_bldg_reset_construction_anim.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_bldg_reset_construction_anim_00474ae0.asm), not from the
// Ghidra .c draft (which reads close but does not show the operand widths cited below).
//
#include "sim/sim_bldg_reset_construction_anim.h"

#include <cstring>

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_reset_construction_anim_calls &live_bldg_reset_construction_anim_calls() {
    static const bldg_reset_construction_anim_calls gc = {
        MH_LIBMH_BIND(llm_cfg_anim_frame_at_progress),
    };
    return gc;
}

namespace {
// mh_map_object_building::anim and mh_cfg_final_struct_Building::anim are both documented
// `cfg_t_frame_index[12]` (int32_t) but flattened by Ghidra to `uint8_t anim[48]`
// (addr/mh_structs.gen.h) -- same flattening sim_state.h's cfg_unit_weapon_id()/
// cfg_unit_weapon_enabled() comment documents for cfg_unit::weapons. These two helpers are the
// narrowly-scoped, file-local equivalent for THIS field shape (see the header's declared-need
// note) -- not a new shared cross-cutting utility, just enough to read/write one already-typed
// struct field correctly.
int32_t frame_at(const uint8_t (&anim)[48], int32_t slot) {
    int32_t value;
    std::memcpy(&value, &anim[slot * 4], sizeof(value));
    return value;
}
void set_frame_at(uint8_t (&anim)[48], int32_t slot, int32_t value) {
    std::memcpy(&anim[slot * 4], &value, sizeof(value));
}
} // namespace

namespace detail {

void bldg_reset_construction_anim(const sim_view &v, sim_store &own,
                                  const bldg_reset_construction_anim_calls &gc, uint32_t player,
                                  int32_t building_index) {
    building &b = own.building_at(player, building_index);

    // building_id, and the cfg record it selects, are read ONCE here rather than re-derived at
    // every asm reference -- 0x00474b04/0x00474b37/0x00474b5b/0x00474b78/0x00474b9a/0x00474bda all
    // separately recompute the SAME player*0x6aa4+building_index*0x111(+building_id*0x842)
    // arithmetic from scratch, which is Watcom's usual unoptimized codegen rather than a sign that
    // the value could change mid-call: nothing this function calls (llm_cfg_anim_frame_at_progress
    // is a PURE function of its two arguments) can write building_id, buildings[], or the cfg
    // table, so caching is behaviourally identical to the asm's redundant re-fetch -- same
    // reasoning sim_bldg_placement_preview.cpp's own `const cfg_building &cb = ...` hoist and
    // sim_bldg_finish_order.cpp's `building &b = own.building_at(...)` (obtained once, reused for
    // every field access below) both rely on.
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // ---- zero the anim frames, 0x00474b04-0x00474b59 -------------------------------------------
    // Loop bound is sprite_quantity (uint8_t, cfg_final_struct_Building) -- the asm has NO
    // array-length check beyond that field itself (CMP EAX,local_18 / JG, nothing else). See the
    // header's declared-need note / uncertainties below on what a value above 12 would do.
    for (int32_t i = 0; i < (int32_t)cb.sprite_quantity; ++i) { set_frame_at(b.anim, i, 0); }

    // ---- recompute anim[0], 0x00474b5b-0x00474bd4 -----------------------------------------------
    // READ-MODIFY-WRITE ORDER: the cycle_progress/build_time_2 division (FDIV @0x00474b8e) and the
    // callee (CALL @0x00474bbd) both happen BEFORE online_state is cleared -- that store is the
    // LAST thing the function does, at 0x00474bea. Statement order below matches the asm exactly.
    const double  progress_fraction = b.cycle_progress / cb.build_time_2;
    const int32_t start_frame       = frame_at(cb.anim, 0);
    const int32_t new_frame         = gc.cfg_anim_frame_at_progress(start_frame, progress_fraction);
    set_frame_at(b.anim, 0, new_frame);

    // ---- clear online_state, 0x00474bda-0x00474bea ------------------------------------------------
    // `66 C7 80 ... 0000` -- a 16-BIT store (0x66 operand-size prefix), matching online_state's
    // int16_t storage exactly; no widening.
    b.online_state = 0;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_reset_construction_anim(uint32_t player, int32_t building_index) {
    sim_state st = state();
    detail::bldg_reset_construction_anim(st.read, st.own, live_bldg_reset_construction_anim_calls(),
                                         player, building_index);
}


} // namespace mh::sim
