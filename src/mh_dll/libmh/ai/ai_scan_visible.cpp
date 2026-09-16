//
// ai/ai_scan_visible.cpp -- see ai_scan_visible.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_target_list_scan_visible_enemies_004ec5ea.asm and
// tmp/decomp_a2/llm_strat_ai_scan_target_list_sort_004ec7bb.asm), not from Ghidra's C: the
// scan_visible_enemies draft reads an uninitialised `target_owner`/`extraout_ECX` and the sort
// draft's plate cites the WRONG comparator address (0x4ec292; the assembly loads 0x4ec792,
// matching `mh::addr::llm_strat_ai_scan_target_sort_cmp`). Neither artifact is reproduced here.
//
// ---- OUTSTANDING VIEW GAP (declared, not worked around) --------------------------------------
// target_list_scan_visible_enemies reads `Unit[unit_proto_id].type` (Ghidra global `Unit` @
// 0x00e4a098, `cfg::final::struct::Unit`, stride 0x23f, region RID_UNIT) to decide whether the
// packed target ref gets the 0x80 or the 0x20 owner-class bit. That table is NOT currently in
// `ai_view` (only cfg_buildings/cfg_weapons are), and `mh_structs.gen.h` has no
// `mh_cfg_final_struct_Unit` type yet -- `docs/structs.md` already documents the field, though:
// `cfg::final::struct::Unit` (size 0x23f) has `type` (`cfg::enum::E_UNIT_TYPE`) at `+0xde`, read
// here as a DWORD (matches the assembly's `CMP dword ptr`, and the next documented field,
// `build_time`, starts exactly at `+0xe2` = `0xde+4`, confirming the 4-byte width).
//
// This file therefore references `v.cfg_units` (a `const cfg_unit *`, `cfg_unit` aliasing a
// would-be `mh::game::mh_cfg_final_struct_Unit`) as if it already existed, per the translator
// brief's "stop and declare it -- do not work around it with an offset". The needed central
// addition, spelled out for whoever wires it in:
//   - mh_structs.gen.h: `struct mh_cfg_final_struct_Unit { ...; int32_t type; /* +0xde */ ...; };`
//     (only `type` is needed by this TU; the rest of the 0x23f-byte record can stay unmodelled
//     until another consumer needs it), base `mh::addr::Unit` (0x00e4a098), region `RID_UNIT`,
//     stride 0x23f, count 100.
//   - ai_state.h: `using cfg_unit = mh::game::mh_cfg_final_struct_Unit;` and
//     `const cfg_unit *cfg_units;` added to `ai_view` (read-only, same shape as cfg_buildings).
//   - ai_state.cpp's `state()`: bind it from the region registry like the other cfg tables.
// Until that lands, this TU will not compile on its own -- by design, so the gap is visible
// rather than silently patched over with a raw offset.
//
#include "ai/ai_scan_visible.h"


namespace mh::ai {
namespace detail {

int32_t target_list_scan_visible_enemies(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         uint32_t player) {
    (void)own; // read-only function; every effect is inside gc.scan_target_list_add
    int32_t added = 0;

    // Outer loop: every active player (UNSIGNED compare against *v.active_player_count, matching
    // the original's `CMP ESI,[active_player_count] / JC`).
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        // Hostility test, read straight off `player_data[player].ai_player_relation[p]` -- `player`
        // (the ticking player, this function's argument) is the STRUCT the relation is read from,
        // `p` (the scanned/other player) is the INDEX into it. `> -1` skips (the original's
        // `CMP ..,-1 / JG skip`), i.e. anything that isn't exactly -1/hostile is skipped, not just
        // +1/friendly -- reproduced exactly rather than narrowed to `== -1`.
        if (v.players[player].ai_player_relation[p] > -1) continue;
        // A second, separate self-check follows the hostility test in the original even though
        // ai_player_relation[self] is always +1 (so this branch is normally unreachable) -- kept
        // as its own statement rather than folded into the check above.
        if (p == player) continue;

        // COUNT-DRIVEN roster walk, NOT index-bounded -- same idiom as turret_threat_rescan's
        // building walk, but over units[p][]. `remaining` seeds from the raw 2 bytes at
        // units[p][0]'s offset 0 (the `unit_above` field, `map::t::unit_full_id`) reinterpreted as
        // a little-endian ushort; `unit_index` starts at 1 and increments every iteration;
        // `remaining` decrements only when the slot is occupied (unit_proto_id != 0).
        const unit &u0         = unit_of(v, p, 0);
        uint32_t    remaining  = uint32_t(u0.unit_above[0]) | (uint32_t(u0.unit_above[1]) << 8);
        int32_t     unit_index = 1;
        while (remaining != 0) {
            const unit &u = unit_of(v, p, unit_index);
            if (u.unit_proto_id != 0) {
                if (u.energy > 0.0) {
                    if (gc.unit_is_idle_or_parked((int32_t)p, unit_index) == 0) {
                        // `player`'s OWN tile-flag grid (not p's), indexed (x<<8)|y over the fixed
                        // 256x256 plane -- matches ai_tile_flags_grid's documented indexing, same
                        // as tile_at()/fog_visible_count_at(). The [1,4] magnitude test is NOT the
                        // documented 0x40 turret-threat bit; it reads some other, uncatalogued use
                        // of the same per-tile byte (see uncertainties).
                        const uint32_t tile_idx = (uint32_t(u.x) << 8) | uint32_t(u.y);
                        const uint8_t  flag     = v.players[player].ai_tile_flags_grid[tile_idx];
                        if (flag >= 1 && flag <= 4) {
                            // Packed target ref: p (owner, low bits) | a class bit chosen from the
                            // unit's cfg TYPE. `< 0xf` (the original's `CMP ..,0xe / JLE`) -> 0x80,
                            // else -> 0x20 -- both are components of ai_state.h's REF_UNIT_BITS
                            // (0xa0 = 0x80|0x20), so this always produces a UNIT-class ref, never a
                            // building one. See the file-top note: v.cfg_units is a declared need.
                            uint32_t aggressor_ref = p;
                            if ((int32_t)v.cfg_units[u.unit_proto_id].type < 0xf)
                                aggressor_ref |= 0x80u;
                            else
                                aggressor_ref |= 0x20u;
                            // gc.scan_target_list_add's COMMITTED parameter names are
                            // (player, target_id, target_owner), but THIS call site's register
                            // values (EAX=player, EDX=aggressor_ref, EBX=unit_index) land aggressor_ref
                            // in the `target_id` slot and unit_index in the `target_owner` slot --
                            // passed positionally as the assembly has them, not reordered to match
                            // the names (see uncertainties: the callee's own disassembly, which
                            // would confirm this, was not part of this batch).
                            gc.scan_target_list_add((int32_t)player, (int32_t)aggressor_ref,
                                                    unit_index);
                            ++added;
                        }
                    }
                }
                --remaining;
            }
            ++unit_index;
        }
    }
    return added;
}

void scan_target_list_sort(const ai_view &v, const ai_store &own, const ai_calls &gc, void *base,
                           uint32_t count) {
    (void)v;
    (void)own; // no AI state read or written; base/count fully determine the effect
    // Fixed element width (0x16) and a fixed comparator address -- both read straight off the
    // assembly (`MOV EBX,0x16` / `MOV ECX,0x4ec792`), not derived. Calls the GAME's qsort with the
    // GAME's comparator; see ai_calls::qsort's own comment for why this must not become a host
    // sort.
    gc.qsort(base, count, 0x16u, gc.scan_target_sort_cmp);
}

} // namespace detail

int32_t target_list_scan_visible_enemies(uint32_t player) {
    const ai_state st = state();
    return detail::target_list_scan_visible_enemies(st.read, st.own, live_calls(), player);
}

void scan_target_list_sort(void *base, uint32_t count) {
    const ai_state st = state();
    detail::scan_target_list_sort(st.read, st.own, live_calls(), base, count);
}

// ---- the differential-oracle arms --------------------------------------------------------------

} // namespace mh::ai
