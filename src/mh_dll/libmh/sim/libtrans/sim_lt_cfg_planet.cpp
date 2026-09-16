//
// sim/libtrans/sim_lt_cfg_planet.cpp -- see sim_lt_cfg_planet.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/cfg_final_planet_Construct_0045b69c.asm), not from the Ghidra `.c` draft.
//
#include "sim/libtrans/sim_lt_cfg_planet.h"

#include <cstring> // std::strcpy / std::memcpy -- see the header banner's string-copy derivation

#include "addr/mh_calls.gen.h"   // mh::host().cfg_ReadMapFile
#include "state/boot_snapshot.h" // LIB-BOOT: the tlo lookup over the IMPORTED registry
#include "state/host_api.h"
#include "state/host_events.h" // LIFT-TABLE S3: the graphics tail crosses as ONE invalidate record

namespace mh::sim {

const cfg_planet_construct_calls &live_cfg_planet_construct_calls() {
    static const cfg_planet_construct_calls gc = {
        // Table param is void* (C facade); the member keeps the typed signature -- adapt.
        +[](mh::game::mh_cfg_struct_map_header *hdr) { return mh::host().cfg_ReadMapFile(hdr); },
        // LIB-BOOT (2026-09-10): was `mh::call::cfg_GetTloIndex`, and it was the last VA call in
        // this file. cfg_GetTloIndex @0x004b9582 is 116 bytes of case-insensitive lookup over the
        // 8-entry _G_LLM_TLO_REGISTRY -- a region with MEASURED ZERO WRITERS, i.e. baked image data
        // rather than parser output. So the site was never really a call we needed; it was a table
        // we did not have. The snapshot carries that table in its DERIVED form (the `char *` names
        // materialised inline, because standalone there is no image for them to point into) and the
        // lookup is ours. Hosted, boot::tlo_index_for reads the live registry and returns exactly
        // what the original returned; the lib_trans selftest pins that equivalence.
        +[](char *file_name) { return mh::state::boot::tlo_index_for(file_name); },
    };
    return gc;
}

namespace detail {

void cfg_final_planet_Construct(sim_store &own, const cfg_planet_construct_calls &c,
                                int32_t define_index, int32_t invention_index, char *map_name,
                                const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                char                                     *path_unc) {
    // 0x0045b6bd-0x0045b6d1: SIGNED `> 0` guard (CMP/JLE) -- row 0 of Progress is never touched by
    // this function. `.index` is uint16_t (AX write); planet_data->index is truncated to 16 bits.
    if (invention_index > 0) {
        own.cfg_invention_at(invention_index).index = static_cast<uint16_t>(planet_data->index);
    }

    // Every remaining write targets Planets[planet_data->index] -- the record slot is the
    // pre-struct's LAST field (`index`, [cfg_t_planet_index]), re-read from the by-value blob via
    // `IMUL ...,0x427` at every single write site in the original (not cached in a register); a
    // single C++ reference is behaviourally identical since nothing here mutates planet_data->index.
    cfg_planet &planet = own.cfg_planet_at(planet_data->index);

    // 0x0045b6fc-0x0045b70f: the two identity scalars. `name`'s own Ghidra field comment tags it
    // `[cfg_t_define_index]` -- exactly this parameter's type, confirming the mapping.
    planet.invention_index = invention_index; // 0x0045b6ff
    planet.name            = define_index;    // 0x0045b70f

    // 0x0045b71c-0x0045b76f: five scalars copied straight through from the by-value blob.
    planet.icon_index   = planet_data->icon_index;   // 0x0045b71f
    planet.coordinate_x = planet_data->coordinate_x; // 0x0045b72f
    planet.coordinate_y = planet_data->coordinate_y; // 0x0045b73f
    planet.asteriods    = planet_data->asteroids;    // 0x0045b74f (Ghidra's own misspelling, kept)
    planet.turn_speed   = planet_data->turn_speed;   // 0x0045b75f
    planet.enemy        = planet_data->enemy;        // 0x0045b76f

    // 0x0045b775-0x0045b835: four NUL-terminated string copies, Watcom's byte-pair strcpy shape --
    // see the header banner's byte-exact derivation of why `std::strcpy` reproduces it precisely.
    std::strcpy(planet.map_name, map_name);                                         // 0x0045b78d, dest +0x10f
    std::strcpy(planet.path_unc, path_unc);                                         // 0x0045b7bb, dest +0x10
    std::strcpy(planet.info_txt, static_cast<const char *>(planet_data->info_txt)); // 0x0045b7ec, dest +0x30d
    std::strcpy(planet.info_flc, static_cast<const char *>(planet_data->info_flc)); // 0x0045b81d, dest +0x34d

    // 0x0045b836-0x0045b88d: two RAW 16-byte copies (source_mul/source_add are `void *` fields
    // pointing at 4-int32 arrays, not strings -- see the header banner's memcpy derivation).
    std::memcpy(planet.source_mul, planet_data->source_mul, sizeof(planet.source_mul)); // 0x0045b852
    std::memcpy(planet.source_add, planet_data->source_add, sizeof(planet.source_add)); // 0x0045b87e

    // 0x0045b88e-0x0045b8f2: READ BACK path_unc/map_name from the Planets record just filled above
    // into the local map_header (same byte-pair strcpy shape) -- a genuine round-trip through the
    // table, not a mistake. Only path_unc/map_name are filled before the call; every other field
    // (tlo_name included, read only AFTER the call, once cfg_ReadMapFile has filled it) is left at
    // whatever the original's uninitialised stack held. Value-initialising here is a DECLARED
    // divergence -- see uncertainties[] -- not an attempt to match the original's garbage.
    // THE HEADER LOCAL CARRIES THE ORIGINAL FRAME'S HEADROOM, NOT THE STRUCT'S sizeof -- found the
    // hard way at LIB-TRANS-P (2026-09-02, the domain's first promoted boot): cfg_ReadMapFile
    // writes PAST mh_cfg_struct_map_header's 0x17c for maps whose .MP carries the mission tail --
    // MEASURED high-water 0x189 (+0xD) on 204_META.MP (metal256.tlo) and one ruins256 map; every
    // plain map stops exactly at 0x17c. The ORIGINAL survives because its map_header local sits at
    // [EBP-0x254] with nothing live below [EBP-0xC] -- ~0x248 bytes of dead-frame slack absorb the
    // tail -- while a sizeof-sized MSVC local put the /GS cookie right above the buffer: instant
    // 0xC0000409 fast-fail at return, the silent boot kill the -P bisect chased. So the local
    // mirrors the CONTRACT the original actually provides (0x248 bytes of writable backing), and
    // the struct's undersize is filed as a Ghidra lead (map_header footer tail; read
    // cfg_ReadMapFile's mission-tail writer to size it properly) rather than guessed at here.
    alignas(8) unsigned char            map_header_raw[0x248] = {};
    mh::game::mh_cfg_struct_map_header &map_header =
        *reinterpret_cast<mh::game::mh_cfg_struct_map_header *>(map_header_raw);
    std::strcpy(map_header.path_unc, planet.path_unc); // 0x0045b8a6
    std::strcpy(map_header.map_name, planet.map_name); // 0x0045b8da

    (void)c.read_map_file(&map_header); // 0x0045b8f9 -- return value discarded, matching the original

    // ---- THE GRAPHICS TAIL LEAVES (LIFT-TABLE S3, 2026-09-09) ------------------------------------
    //
    // 0x0045b8fe to the end of the body is one CONTIGUOUS graphics tail with nothing sim beneath it
    // (field census 2026-09-09): tlo_index -> the 8-way switch -> `SHL AL,6` ->
    // soldier_sprite_bank_offset -> the `index == 0x1f` guard -> 8x FillBankData. Its outputs are
    // read by renderers and bank loaders only, and S2 has just taken every one of those bytes out
    // of the determinism hash -- which is what makes a host-owned writer legal rather than a
    // notify:1 entry writing hashed state. It is now ONE record, and the switch, the shift, the two
    // Planets[] byte stores and all eight FillBankData calls live in the hosted sink
    // (seams/host_event_sink.cpp, LIBMH_EVK_INV_PLANET_GFX_SETUP).
    //
    // 0x0045b904/0x0045b912: cfg_GetTloIndex reads map_header.tlo_name (just filled by
    // cfg_ReadMapFile) and returns the tileset's index. THE LOOKUP STAYS HERE and the resolved
    // index crosses in the record, because tlo_name is a stack local of THIS frame: handing the
    // host a pointer to it works under mh.dll, whose sink dispatches synchronously at emit, and
    // dangles under a NULL-callback host, which drains at the frame edge. An identity crosses; a
    // pointer into a dead frame does not (R4, and R7's two consumptions are why it matters).
    //
    // The store into planet.tlo_index is GONE from libmh with the rest of the tail -- the host
    // writes it. Standalone, with no sink bound, the two graphics bytes stay 0: unhashed since S2,
    // and no translated body reads either of them (the R3b gate measures this, it is not asserted).
    const uint8_t tlo_index = c.get_tlo_index(map_header.tlo_name);
    mh::state::evt::inv_planet_gfx_setup(static_cast<int32_t>(planet_data->index),
                                         static_cast<int32_t>(tlo_index));
}

} // namespace detail

// ---- the public wrapper ----------------------------------------------------------------------

void cfg_final_planet_Construct(int32_t define_index, int32_t invention_index, char *map_name,
                                const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                char                                     *path_unc) {
    sim_state st = state();
    detail::cfg_final_planet_Construct(st.own, live_cfg_planet_construct_calls(), define_index,
                                       invention_index, map_name, planet_data, path_unc);
}

} // namespace mh::sim
