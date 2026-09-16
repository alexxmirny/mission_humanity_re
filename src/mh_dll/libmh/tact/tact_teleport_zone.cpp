//
// tact/tact_teleport_zone.cpp -- see tact_teleport_zone.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_teleport_zone.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_tact_unit_teleport, llm_tact_unit_despawn, time_GetCurrentTime
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const teleport_zone_scan_tick_calls &live_teleport_zone_scan_tick_calls() {
    static const teleport_zone_scan_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_teleport),
        MH_LIBMH_BIND(llm_tact_unit_despawn),
        MH_LIBMH_BIND(time_GetCurrentTime),
    };
    return c;
}

namespace detail {

void teleport_zone_scan_tick(const tact_view &tv, tact_store &own,
                             const teleport_zone_scan_tick_calls &c) {
    // ---- Half 1 (@0x00432bb9-0x00432cf3): the teleport-zone scan --------------------------------
    //
    // Slot 0 is never touched -- the same off-by-one every other teleport_zone_at loop in this
    // domain preserves.
    for (int32_t i = 1; i < 0x40; ++i) {
        teleport_zone &tz = own.teleport_zone_at(i);

        // @0x00432bd3-0x00432bde: inactive zones (id == 0) are skipped entirely -- no corner probe,
        // no field_28 reset.
        if (tz.id == 0) continue;

        const int32_t col = tz.start_col;
        const int32_t row = tz.start_row;

        // @0x00432be4-0x00432c1f: (col, row). A CASCADE -- the first occupied corner wins and the
        // zone's other three corners are never probed this tick.
        int32_t occupant = tile_at(tv, col, row).building;
        if (occupant > 0) {
            c.unit_teleport(i, occupant);
            continue;
        }

        // @0x00432c21-0x00432c5d: (col + 1, row).
        occupant = tile_at(tv, col + 1, row).building;
        if (occupant > 0) {
            c.unit_teleport(i, occupant);
            continue;
        }

        // @0x00432c62-0x00432c9d: (col, row + 1) -- read via the row-adjacent element's `.building`
        // field, not `.unit[1]`; see the header's derivation note.
        occupant = tile_at(tv, col, row + 1).building;
        if (occupant > 0) {
            c.unit_teleport(i, occupant);
            continue;
        }

        // @0x00432ca2-0x00432cde: (col + 1, row + 1).
        occupant = tile_at(tv, col + 1, row + 1).building;
        if (occupant > 0) {
            c.unit_teleport(i, occupant);
            continue;
        }

        // @0x00432ce3: no unit stands on any of the four corners this frame -- clear the
        // "consumed/occupied" latch tact_unit_teleport.cpp's LINKED-zone success path stamps.
        tz.field_28 = 0;
    }

    // ---- Half 2 (@0x00432cf3-0x00432de6): UNRELATED to teleport zones -- the quit-tile despawn ---
    //
    // Reads (never writes) _G_LLM_TACT_QUIT_TILE_COL/_ROW through the store's existing accessors --
    // tact_view has no binding for them yet (mission_load is their only writer so far).
    const int32_t qcol = own.quit_tile_col();
    const int32_t qrow = own.quit_tile_row();

    // @0x00432cf3-0x00432d12: gate -- both must hold, or the whole despawn block is skipped.
    if (qcol + qrow > 0 && c.time_now() < *tv.mine_blast_time_end) {
        // @0x00432d17-0x00432d4a: (qcol, qrow). Each of these four checks is INDEPENDENT -- a hit at
        // one corner does not skip the other three (unlike Half 1's cascade).
        const int32_t v0 = tile_at(tv, qcol, qrow).building;
        if (v0 > 0 && v0 < 0x20) c.unit_despawn(v0);

        // @0x00432d4a-0x00432d78: (qcol, qrow + 1) -- same row-adjacent-element `.building` read as
        // Half 1's third corner.
        const int32_t v1 = tile_at(tv, qcol, qrow + 1).building;
        if (v1 > 0 && v1 < 0x20) c.unit_despawn(v1);

        // @0x00432d7d-0x00432dac: (qcol + 1, qrow).
        const int32_t v2 = tile_at(tv, qcol + 1, qrow).building;
        if (v2 > 0 && v2 < 0x20) c.unit_despawn(v2);

        // @0x00432db1-0x00432de1: (qcol + 1, qrow + 1).
        const int32_t v3 = tile_at(tv, qcol + 1, qrow + 1).building;
        if (v3 > 0 && v3 < 0x20) c.unit_despawn(v3);
    }
}

} // namespace detail

void teleport_zone_scan_tick() {
    tact_state st = state();
    detail::teleport_zone_scan_tick(st.read, st.own, live_teleport_zone_scan_tick_calls());
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
