//
// sim/sim_bldg_link_to_network.cpp -- see sim_bldg_link_to_network.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_link_to_network_if_adjacent_0049232e.asm), not from the
// Ghidra .c draft (right about the overall shape, wrong about the FP comparison's NaN behaviour --
// see the header).
//
#include "sim/sim_bldg_link_to_network.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_link_to_network_calls &live_bldg_link_to_network_calls() {
    static const bldg_link_to_network_calls gc = {
        MH_LIBMH_BIND(llm_bldg_set_connected_flag),
        MH_LIBMH_BIND(llm_strat_bldg_propagate_network_connectivity),
    };
    return gc;
}

namespace {

// ---- cfg_enum_E_BUILDING members this function compares Building[].type against ------------------
// The generated header renders the field as its underlying scalar (`uint8_t type; //
// [cfg_enum_E_BUILDING]`, addr/mh_structs.gen.h), so the literals are named here, in an anonymous
// namespace (internal linkage -- no ODR collision with the same names other TUs in this module
// already define for their own sites). Values cross-checked against THREE existing bindings of the
// same enum rather than re-derived from the raw immediates alone: ai/ai_state.h's
// BLDG_TYPE_A_MOTHER/_H_MOTHER (6/26) and BLDG_TYPE_A_TURRET/_H_TURRET (5/25), and
// sim/sim_order_dispatch_bldg.cpp + sim/sim_order_enqueue.h's BLDG_TYPE_A_PORT/_H_PORT/
// _A_SHUTTLE/_H_SHUTTLE (0x0c/0x20/0x0d/0x21) -- all agree with the CMP immediates read directly off
// this function's own asm (0x0049236b, 0x00492394, 0x004923bf, 0x004923e8, 0x00492422, 0x0049244b,
// 0x00492694, 0x004926d1, 0x0049272b, 0x00492768, 0x004927c2, 0x004927ff).
inline constexpr uint8_t BLDG_TYPE_A_TURRET  = 0x05;
inline constexpr uint8_t BLDG_TYPE_A_MOTHER  = 0x06;
inline constexpr uint8_t BLDG_TYPE_A_PORT    = 0x0c;
inline constexpr uint8_t BLDG_TYPE_A_SHUTTLE = 0x0d;
inline constexpr uint8_t BLDG_TYPE_H_TURRET  = 0x19;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER  = 0x1a;
inline constexpr uint8_t BLDG_TYPE_H_PORT    = 0x20;
inline constexpr uint8_t BLDG_TYPE_H_SHUTTLE = 0x21;

// `FLDZ; FCOMP energy; FNSTSW AX; SAHF; JC <continue>` (0x0049261b-0x00492626) -- C0 (-> CF) is set
// for ST(0)<src AND for an unordered (NaN) compare, so the scan continues evaluating a candidate
// whose energy is NaN, not just one that is positive. `0.0 < energy` (what the .c draft renders, and
// what a plain transcription would write) drops that case; `!(energy <= 0.0)` is the form that
// reproduces JC's actual truth table -- same idiom sim_order_dispatch_bldg.cpp's own `energy_gate()`
// documents for its sibling dispatcher's 28 identical sites (this file defines its own copy rather
// than including that TU's anonymous-namespace helper, which has no external linkage to share).
inline bool energy_gate(double energy) { return !(energy <= 0.0); }

} // namespace

namespace detail {

void bldg_link_to_network_if_adjacent(const sim_view &v, const bldg_link_to_network_calls &gc,
                                      uint16_t player, uint32_t index) {
    // 0x0049232e-0x0049236b: buildings[player][index] and its cfg record. Read ONCE here rather than
    // re-derived at every asm reference (the asm recomputes player*0x6aa4+index*0x111(+building_id*
    // 0x842) SIX separate times across the classification below, Watcom's usual unoptimized codegen)
    // -- safe because nothing before this function's two terminal calls writes `buildings` or the cfg
    // table, and both terminal calls are followed immediately by `return`.
    const building     &b    = building_of(v, (uint32_t)player, (int32_t)index);
    const cfg_building &cb   = v.cfg_buildings[b.building_id];
    const uint8_t       type = cb.type;

    // ---- classification 1/3 (0x0049236b-0x004923f1): shuttles/ports link unconditionally ----------
    if (type == BLDG_TYPE_H_SHUTTLE || type == BLDG_TYPE_A_SHUTTLE || type == BLDG_TYPE_H_PORT ||
        type == BLDG_TYPE_A_PORT) {
        gc.bldg_set_connected_flag(player, (int32_t)index);
        return;
    }

    // ---- classification 2/3 (0x00492402-0x00492460): mothers recurse straight into the flood-fill --
    if (type == BLDG_TYPE_H_MOTHER || type == BLDG_TYPE_A_MOTHER) {
        gc.bldg_propagate_network_connectivity(player, (int32_t)index);
        return;
    }

    // ---- classification 3/3 (0x00492465-0x00492830): scan the 31x31 footprint+radius window --------
    const uint32_t width_mask  = map_width_mask(v);
    const uint32_t height_mask = map_height_mask(v);

    // Column seed (0x004924c5-0x00492537): MASK the raw sum first, THEN subtract 15 and mask AGAIN.
    // Not a redundant no-op in the asm's own shape -- see the header's derivation -- transcribed as
    // the literal two-stage form per translator brief rule 8 (reproduce the shape the assembly
    // computes, not an arithmetically-equivalent rewrite), even though the two stages are provably
    // equal as integers here.
    const uint32_t col_seed  = width_mask & ((uint32_t)(cb.width / 2) + (uint32_t)b.x);
    const uint32_t col_start = width_mask & (col_seed - 15u);

    // Row seed (0x004924d2-0x00492578 / re-seeded at 0x00492567 each outer pass in the original).
    // Nothing in the loop below can change buildings[player][index].y or Building[bid].height before
    // the function's one terminal call+return, so this is computed ONCE here rather than re-derived
    // every outer iteration -- same caching reasoning sim_bldg_reset_construction_anim.cpp's header
    // documents for its own redundant-recompute case.
    const uint32_t row_seed  = height_mask & ((uint32_t)(cb.height / 2) + (uint32_t)b.y);
    const uint32_t row_start = height_mask & (row_seed - 15u);

    // Both loops run exactly 31 iterations (Watcom's doubled-compare codegen for `for (i=0; i<0x1f;
    // ++i)`, re-derived from the raw CMP/JGE at 0x00492541-0x00492549 / 0x0049257f-0x00492587 rather
    // than trusted from the .c's cleaner rendering).
    uint32_t tile_col = col_start;
    for (int32_t ix = 0; ix < 31; ++ix) {
        uint32_t tile_row = row_start;
        for (int32_t iy = 0; iy < 31; ++iy) {
            // 0x004925a5-0x004925c0: class_owner == (player|0x40) -- hi nibble 0x40 = "building here",
            // lo nibble = owner (mh_map_tile_object_data's field comment). player is always < 0x10 in
            // this game (MAX_PLAYERS=8), so the byte-vs-word width of the OR doesn't matter.
            const tile_object &t = tile_at(v, (int32_t)tile_col, (int32_t)tile_row);
            if (t.class_owner == (uint8_t)((player & 0xffu) | 0x40u)) {
                // The CANDIDATE adjacent building, a different record from the subject `b` above.
                const building &cand = building_of(v, (uint32_t)player, (int32_t)t.building);

                // 0x004925e9-0x00492659: built_flags&1, then energy (FP, see energy_gate() above),
                // then online_state != 0. Order matches the asm exactly.
                if ((cand.built_flags & 0x1u) != 0 && energy_gate(cand.energy) &&
                    cand.online_state != 0) {
                    // 0x00492660-0x0049281a: three sequential exclusion checks (turret, shuttle,
                    // port), each short-circuiting the whole chain to "skip this tile" on any match.
                    const uint8_t cand_type = v.cfg_buildings[cand.building_id].type;
                    const bool    is_turret =
                        (cand_type == BLDG_TYPE_H_TURRET || cand_type == BLDG_TYPE_A_TURRET);
                    const bool is_shuttle =
                        (cand_type == BLDG_TYPE_H_SHUTTLE || cand_type == BLDG_TYPE_A_SHUTTLE);
                    const bool is_port =
                        (cand_type == BLDG_TYPE_H_PORT || cand_type == BLDG_TYPE_A_PORT);

                    if (!is_turret && !is_shuttle && !is_port) {
                        // 0x00492822-0x0049282e: first qualifying adjacent building wins -- call and
                        // return immediately, the scan never continues after a hit.
                        gc.bldg_propagate_network_connectivity(player, (int32_t)index);
                        return;
                    }
                }
            }
            tile_row = height_mask & (tile_row + 1u); // 0x0049258e-0x0049259a
        }
        tile_col = width_mask & (tile_col + 1u); // 0x00492550-0x0049255c
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_link_to_network_if_adjacent(uint16_t player, uint32_t index) {
    sim_state st = state();
    detail::bldg_link_to_network_if_adjacent(st.read, live_bldg_link_to_network_calls(), player, index);
}


} // namespace mh::sim
