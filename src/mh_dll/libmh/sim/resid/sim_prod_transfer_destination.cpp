//
// sim/resid/sim_prod_transfer_destination.cpp -- see sim_prod_transfer_destination.h. Translated
// from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_prod_set_transfer_destination_0048efcc.asm), the Ghidra .c being a
// draft.
//
#include "sim/resid/sim_prod_transfer_destination.h"

#include "addr/mh_calls.gen.h"  // typed callables for the effectful/frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const prod_transfer_destination_calls &live_prod_transfer_destination_calls() {
    static const prod_transfer_destination_calls c = {
        MH_LIBMH_BIND(llm_strat_planet_distance),
        MH_LIBMH_BIND(llm_strat_prod_transfer_progress),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596, inlined -- see the header banner for why (MH_UNAVAILABLE, not
// reachable through mh::call::). FSTCW / RC=11,PC=11 / FLDCW / FRNDINT / FLDCW-restore / FISTP
// dword, the same shape ai_active_unit_tick.cpp's trunc_toward_zero already carries; private to this
// TU (no cross-TU sharing per translator-brief rule 4 -- this is the established per-TU inlining
// pattern, not a new shared helper). Called at 0x0048f0e6 and 0x0048f102, both FISTP-ing to a
// 32-bit (dword) stack slot, matching this exact shape.
int32_t trunc_toward_zero(double value) {
    return ::mh::fp::trunc_i32(value);
}

} // namespace

namespace detail {

// ---- llm_strat_prod_set_transfer_destination @0x0048efcc --------------------------------------
int32_t prod_set_transfer_destination(const sim_view &v, sim_store &own, const prod_transfer_destination_calls &c,
                                      uint32_t player_idx, int32_t prod_slot, int32_t dest_planet) {
    // 0x0048efeb/0x0048f00d/... (every player-index read in this body): a 16-bit MOVZX off the
    // stored dword parameter -- matches the .c's `player_idx & 0xffff`. Masked once, used everywhere
    // this function indexes the shuttle-slot roster.
    const uint32_t player = player_idx & 0xffffu;

    auto &slot = own.prod_shuttle_slot_at(player, prod_slot);

    double distance;
    if (slot.status == 0xc8) {
        // ---- 0x0048efeb-0x0048f1af: in transit -- recompute the interpolated position, then
        // re-lerp a leg from THAT position to the NEW destination parameter -------------------------
        int32_t origin_x, origin_y;
        if (slot.origin_planet == 0) {
            // 0x0048f052-0x0048f087: cached position lives in the NEXT slot's dead leading pad
            // (prev_slot_transit_x/_y) -- see the header's PRESERVE-BUG note for the exact
            // out-of-bounds hazard at player 7 / prod_slot 9. Translated literally, no guard.
            auto &next = own.prod_shuttle_slot_at(player, prod_slot + 1);
            origin_x   = next.prev_slot_transit_x;
            origin_y   = next.prev_slot_transit_y;
        } else {
            // 0x0048f030-0x0048f04d: Planets[origin_planet].coordinate_x/_y. FILD'd as signed int32
            // later (0x0048f0db/0x0048f0e1), so cast at the read here rather than at use.
            origin_x = static_cast<int32_t>(v.cfg_planets[slot.origin_planet].coordinate_x);
            origin_y = static_cast<int32_t>(v.cfg_planets[slot.origin_planet].coordinate_y);
        }
        // 0x0048f08a-0x0048f0c4: Planets[slot.dest_planet] -- the SLOT's current (OLD) destination,
        // still unchanged at this point in the body.
        const int32_t dest_x = static_cast<int32_t>(v.cfg_planets[slot.dest_planet].coordinate_x);
        const int32_t dest_y = static_cast<int32_t>(v.cfg_planets[slot.dest_planet].coordinate_y);

        // 0x0048f0c7-0x0048f0ca: progress is read against the RAW prod_slot parameter, never the
        // player-composite index.
        const double progress = c.prod_transfer_progress(prod_slot);

        // 0x0048f0d2-0x0048f0eb / 0x0048f0ee-0x0048f107: trunc(origin + (dest-origin)*progress), one
        // axis at a time. The subtraction is done on the (already int32_t) coordinates, matching the
        // .c's `(int)(destN - originN)` -- bit-identical to the asm's plain 32-bit SUB.
        const int32_t new_x = trunc_toward_zero(static_cast<double>(origin_x) +
                                                static_cast<double>(dest_x - origin_x) * progress);
        const int32_t new_y = trunc_toward_zero(static_cast<double>(origin_y) +
                                                static_cast<double>(dest_y - origin_y) * progress);

        // 0x0048f10a-0x0048f13c: write the freshly-interpolated position back into the SAME dead pad
        // it was read from (see the header's PRESERVE-BUG note -- same OOB hazard applies here too).
        auto &next               = own.prod_shuttle_slot_at(player, prod_slot + 1);
        next.prev_slot_transit_x = new_x;
        next.prev_slot_transit_y = new_y;

        // 0x0048f142-0x0048f155: clear origin_planet -- the leg now starts from the cached position,
        // not a named planet.
        slot.origin_planet = 0;

        // 0x0048f15e-0x0048f1aa: distance from the freshly-interpolated position to the NEW
        // destination PARAMETER (not slot.dest_planet, which still holds the old value here).
        distance = c.planet_distance(static_cast<uint32_t>(new_x), static_cast<uint32_t>(new_y),
                                     v.cfg_planets[dest_planet].coordinate_x,
                                     v.cfg_planets[dest_planet].coordinate_y);
    } else if (slot.status == 0xc9) {
        // ---- 0x0048f1b7-0x0048f22b: arrived -- leg starts from origin_planet, straight to the NEW
        // destination parameter -------------------------------------------------------------------
        distance = c.planet_distance(v.cfg_planets[slot.origin_planet].coordinate_x,
                                     v.cfg_planets[slot.origin_planet].coordinate_y,
                                     v.cfg_planets[dest_planet].coordinate_x,
                                     v.cfg_planets[dest_planet].coordinate_y);
    } else {
        // 0x0048f1d3-0x0048f237: not an active transfer -- nothing to reroute, no writes.
        return -1;
    }

    // ---- 0x0048f23c-0x0048f2e6: common tail -- commit the new destination/duration/status --------
    // 0x0048f24f-0x0048f262: Unit[slot.type_ref_id].equivalent -- a Building[] index (per the
    // field's own dual-purpose comment on mh_llm_prod_shuttle_slot::type_ref_id).
    const int32_t equivalent = v.cfg_units[slot.type_ref_id].equivalent;

    // 0x0048f278-0x0048f27b: dest_planet is stored truncated to 16 bits, matching the field's
    // int16_t width (`MOV word ptr [...],AX`).
    slot.dest_planet = static_cast<int16_t>(dest_planet);

    // 0x0048f282-0x0048f2a5: travel_duration = Building[equivalent].velocity * distance.
    slot.travel_duration = v.cfg_buildings[equivalent].velocity * distance;

    // 0x0048f2ab-0x0048f2c4: travel_duration_copy is a RE-READ of the value just stored (FLD the
    // address just FSTP'd, then FSTP again to the copy field) -- not a second independent
    // computation. Transcribed as the round-trip the .asm performs.
    slot.travel_duration_copy = slot.travel_duration;

    // 0x0048f2ca-0x0048f2e6: status = 0xc8 (200) -- an active transfer again; return 1 (ok).
    slot.status = 0xc8;

    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_set_transfer_destination(uint32_t player_idx, int32_t prod_slot, int32_t dest_planet) {
    sim_state st = state();
    return detail::prod_set_transfer_destination(st.read, st.own, live_prod_transfer_destination_calls(),
                                                 player_idx, prod_slot, dest_planet);
}

} // namespace mh::sim
