//
// sim/sim_fog_of_war.cpp -- see sim_fog_of_war.h. Translated from the DISASSEMBLY, not the Ghidra .c
// drafts (tmp/decomp/map_fow_ConvertSightToArea_004a6792.asm and its six siblings) -- every drift from
// the .c is called out per-function below.
//
// reimpl-verify (2026-08-16) also raised, then closed as a non-issue: the per-run tile loops below
// (update_fow_plus_impl and remove_sight_apply) translate the original's do-while (body-first,
// DEC ECX/JNZ) as a pre-test `for (i < len)`, which would diverge only if a SIGHT_AREA_N table row
// ever had len==0. Read all 312 bytes of SIGHT_AREA_1..10 via read-memory and parsed every {x,y,len}
// triple (skipping the ten x==0x80 sentinels): zero rows found with len==0. Confirmed unreachable,
// not merely unlikely -- the for-loop translation is safe.
#include "sim/sim_fog_of_war.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const map_fow_update_fow_plus_calls &live_map_fow_update_fow_plus_calls() {
    static const map_fow_update_fow_plus_calls c = {
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus_impl),
    };
    return c;
}

const fow_remove_sight_calls &live_fow_remove_sight_calls() {
    static const fow_remove_sight_calls c = {
        MH_LIBMH_BIND(llm_strat_fow_remove_sight_apply),
    };
    return c;
}

const sight_add_circle_calls &live_sight_add_circle_calls() {
    static const sight_add_circle_calls c = {
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
    };
    return c;
}

const sight_remove_circle_calls &live_sight_remove_circle_calls() {
    static const sight_remove_circle_calls c = {
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
    };
    return c;
}

namespace detail {

// ---- map_fow_ConvertSightToArea @0x004a6792 --------------------------------------------------------
//
// A literal 10-way cascade (see the header's [EBP-RETURN] banner): sight==1..9 select SIGHT_AREA_1..9
// in order; every other byte value, INCLUDING 0, falls through to SIGHT_AREA_10 (0x004a6810's
// `MOV EBP,SIGHT_AREA_10` has no guarding CMP -- it is the unconditional last rung of the ladder).
// Reproduced as a switch, not arithmetic over one array.
const map_t_tile_coord *convert_sight_to_area(const sim_view &v, sim_store &own) {
    switch (static_cast<uint8_t>(own.g_tmp_sight())) {
        case 1: return v.sight_area[0];
        case 2: return v.sight_area[1];
        case 3: return v.sight_area[2];
        case 4: return v.sight_area[3];
        case 5: return v.sight_area[4];
        case 6: return v.sight_area[5];
        case 7: return v.sight_area[6];
        case 8: return v.sight_area[7];
        case 9: return v.sight_area[8];
        default: return v.sight_area[9]; // 0 and >=10
    }
}

// ---- map_fow_UpdateFoWPlus @0x0049681a --------------------------------------------------------------
//
// Pure staging: four global writes then a tail call to _impl. Order matches the assembly
// (player, x, y, sight) though nothing here depends on it (independent globals, no callee runs in
// between).
void update_fow_plus(sim_store &own, const map_fow_update_fow_plus_calls &c, uint32_t player,
                     uint32_t x, uint32_t y, uint8_t sight) {
    own.g_tmp_player() = static_cast<int32_t>(player);
    own.g_tmp_x()      = static_cast<int32_t>(x);
    own.g_tmp_y()      = static_cast<int32_t>(y);
    own.g_tmp_sight()  = static_cast<int32_t>(sight); // single zero-extended 4-byte store, see header
    c.update_fow_plus_impl();
}

// ---- map_fow_UpdateFoWPlus_impl @0x004a6817 ----------------------------------------------------------
//
// ---- THE PACKED torus-wrap ACCUMULATION (0x004a6861-0x004a68b8) -----------------------------------
// AL/AH packing is REAL and confirmed by the disassembly's own address comments, not inferred:
//   `MOV AL,[G_TMP_y]` then `MOV AH,[G_TMP_x]` -- so AL is the y-component, AH is the x-component,
//   and the combined AX = (x<<8)|y matches sim_state.h's OWN tile_object_at()/tile_at() indexing
//   convention exactly (confirms this is the right x/y assignment, not a guess).
// Per table entry: `ADD AL,entry.y; ADD AH,entry.x; AND AX,(width-1)<<8|(height-1)` sets the ROW-START
// position (both axes move once). Then INSIDE the per-tile run (len iterations): `INC AH` -- ONLY the
// x-component (AH) increments per iteration; y (AL) is held fixed for the whole run and wraps with the
// row-start AND.
//
// UNCERTAINTY (see uncertainties[]): the task brief describing this function said "the Y-ish component
// increments... the X-ish component sets the row start" -- the OPPOSITE of what 0x004a68b6's `INC AH`
// (x, per the address-comment-confirmed AL=y/AH=x assignment) shows. Followed the assembly per house
// rule 1 (asm is the spec); flagged rather than silently reconciled.
//
// width_mask/height_mask here are NOT sim_view::geom->width_mask/height_mask (a DIFFERENT pair of
// globals, general+0x8/general+0x20) and NOT width_m/height_m either -- the assembly reads the PLAIN
// `width`/`height` scalars (sim_view::map_width/map_height, same addresses map_fow_reveal_full.cpp
// already reads) and truncates to a byte on the fly (`DEC EAX; MOV BH/BL,AL`). Reproduced that way, not
// through map_width_mask(v)/map_height_mask(v) -- see uncertainties[].
//
// ---- THE is_human SPLIT (0x004a6837-0x004a68d1) ----------------------------------------------------
// TWO WHOLE SEPARATE COPIES of the walk, not one walk with an inner branch: `TEST is_human,1<<player;
// JZ` -- if the bit is CLEAR (this player is NOT human-controlled) execution jumps to a walk that
// touches ONLY fog (visible_by_count + discovered); if the bit is SET it falls into a walk that ALSO
// writes tile_objects (flags[1], visibility). Both increment/discover identically; only the
// tile_objects half is gated. Confirmed by the second copy's `MOV EDI,tile_objects_ptr` being loaded
// but NEVER referenced inside that copy's loop body (a dead load, consistent with it being a mechanical
// duplicate of the human copy with the tile_objects writes stripped, not independently written).
//
// ---- THE pmVar9(-=+=)G_TMP_PLAYER ARTIFACT (0x004a6892/0x004a689b) ---------------------------------
// `ADD ESI,G_TMP_PLAYER ... INC byte[ESI+EAX*8] ... SUB ESI,G_TMP_PLAYER` -- confirmed from the
// listing: ESI is bumped by G_TMP_PLAYER only to compute the per-player fog byte address for the ONE
// instruction that needs it, then immediately reverted. pmVar9/fow's own base is never advanced by
// anything else in the loop (only ever indexed via EAX, the tile index) -- the Ghidra .c draft's
// `pmVar9 = (map_struct_fog_of_war*)((int)pmVar9 + (gVar4 - G_TMP_PLAYER))` (always +0, since gVar4 was
// JUST set to G_TMP_PLAYER) is exactly this transient add/sub re-derived as fake pointer arithmetic --
// confirmed artifact, not reproduced; see the fog_visible_by_count_at() accessor for the real
// index math (tile-major stride 8, see the header's binding note (C)).
//
void update_fow_plus_impl(const sim_view &v, sim_store &own) {
    if (static_cast<uint8_t>(own.g_tmp_sight()) == 0) {
        return;
    }
    const map_t_tile_coord *table = convert_sight_to_area(v, own);

    const uint32_t player      = static_cast<uint32_t>(own.g_tmp_player());
    const uint8_t  width_mask  = static_cast<uint8_t>(*v.map_width - 1);
    const uint8_t  height_mask = static_cast<uint8_t>(*v.map_height - 1);
    uint8_t        cur_x       = static_cast<uint8_t>(own.g_tmp_x());
    uint8_t        cur_y       = static_cast<uint8_t>(own.g_tmp_y());

    const bool human = (*v.is_human & (1u << (player & 0x1fu))) != 0;

    while (table->x != 0x80) {
        cur_x       = static_cast<uint8_t>((cur_x + table->x) & width_mask);
        cur_y       = static_cast<uint8_t>((cur_y + table->y) & height_mask);
        int32_t len = table->len;
        ++table;
        const uint8_t bit = static_cast<uint8_t>(1u << (player & 0x1fu));
        for (int32_t i = 0; i < len; ++i) {
            own.fog_visible_by_count_at(cur_x, cur_y, player) += 1; // 0x004a6898/0x004a6929
            if (human) {                                            // only the human-branch copy
                tile_object &t = own.tile_object_at(cur_x, cur_y);
                t.flags[1]     = static_cast<uint8_t>((t.flags[1] & 0xbf) | 0x80); // 0x004a68a1/68a6
                t.visibility   = static_cast<uint8_t>(t.visibility | bit);         // 0x004a68ab
            }
            own.fog_discovered_at(cur_x, cur_y) |= bit;             // 0x004a68af/6932, BOTH branches
            cur_x = static_cast<uint8_t>((cur_x + 1) & width_mask); // 0x004a68b6/6939: INC AH, x only
        }
    }
}

// ---- llm_strat_fow_remove_sight @0x00496868 ---------------------------------------------------------
void remove_sight(sim_store &own, const fow_remove_sight_calls &c, uint32_t player, int32_t x,
                  int32_t y, uint8_t radius) {
    own.g_tmp_player() = static_cast<int32_t>(player);
    own.g_tmp_x()      = x;
    own.g_tmp_y()      = y;
    own.g_tmp_sight()  = static_cast<int32_t>(radius);
    c.remove_sight_apply();
}

// ---- llm_strat_fow_remove_sight_apply @0x004a6946 ----------------------------------------------------
//
// ONE walk (unlike _impl's two whole copies) -- the is_human check is INSIDE the per-tile body, gating
// only the final "mark explored" store, exactly as the asm shows (`TEST byte[is_human],DH; JZ skip`
// sits inside the loop, not around it). No `discovered` write anywhere in this function (confirmed:
// the only stores in the loop touch visible_by_count and, conditionally, tile_objects.visibility/
// flags[1] -- 0x004a69be-0x004a69d1, no 0x80000-offset OR anywhere in this listing, unlike _impl's).
//
// G_OTHER_PLAYERS_MASK (0x004a6965) is written as a REAL side effect here -- reproduced via
// own.other_players_mask() even though this function's own logic only ever reads the value back
// locally, per the task's standing instruction that a differential shadow arm must reproduce writes
// the original makes to shared scratch, not just the writes its own logic strictly needs.
void remove_sight_apply(const sim_view &v, sim_store &own) {
    if (static_cast<uint8_t>(own.g_tmp_sight()) == 0) {
        return;
    }
    const map_t_tile_coord *table = convert_sight_to_area(v, own);

    const uint32_t player     = static_cast<uint32_t>(own.g_tmp_player());
    const uint8_t  other_mask = static_cast<uint8_t>(~(1u << (player & 0x1fu))); // 0x004a695f-6965
    own.other_players_mask()  = other_mask;

    const uint8_t width_mask  = static_cast<uint8_t>(*v.map_width - 1);
    const uint8_t height_mask = static_cast<uint8_t>(*v.map_height - 1);
    uint8_t       cur_x       = static_cast<uint8_t>(own.g_tmp_x());
    uint8_t       cur_y       = static_cast<uint8_t>(own.g_tmp_y());

    while (table->x != 0x80) {
        cur_x       = static_cast<uint8_t>((cur_x + table->x) & width_mask);
        cur_y       = static_cast<uint8_t>((cur_y + table->y) & height_mask);
        int32_t len = table->len;
        ++table;
        for (int32_t i = 0; i < len; ++i) {
            uint8_t &count = own.fog_visible_by_count_at(cur_x, cur_y, player); // 0x004a69be
            count -= 1;
            if (count == 0) {
                tile_object &t = own.tile_object_at(cur_x, cur_y);
                t.visibility   = static_cast<uint8_t>(t.visibility & other_mask); // 0x004a69c3
                // 0x004a69c9: TEST byte ptr[is_human],DH -- BYTE-width here, unlike the sibling
                // map_fow_UpdateFoWPlus_impl's dword-width `TEST dword ptr[is_human],EAX`
                // (0x004a6837). reimpl-verify caught this (2026-08-16, two independent reviewers,
                // confirmed by both): DH is built from an 8-bit SHL (0x004a695f), so only bits 0-7
                // of is_human are ever reachable here, while a naive dword-width port would also see
                // bits 8-31. Masked to 8 bits to match literally -- provably inert while
                // MAX_PLAYERS==8 holds (player is never >7 anywhere in this closure), but the two
                // sibling functions are NOT interchangeable here, so it is not written as if they
                // were. Read fresh per tile in the assembly; hoisting is safe (is_human has no writer
                // anywhere in this closure, and no callee runs mid-loop that could change it), but
                // re-read to keep the translation literal.
                const bool i_am_human =
                    (static_cast<uint8_t>(*v.is_human) & static_cast<uint8_t>(1u << (player & 0x1fu))) != 0;
                if (t.visibility == 0 && i_am_human) {
                    t.flags[1] = static_cast<uint8_t>(t.flags[1] | 0xc0); // 0x004a69d1
                }
            }
            cur_x = static_cast<uint8_t>((cur_x + 1) & width_mask); // 0x004a69d6: INC AH, x only
        }
    }
}

// ---- llm_strat_sight_add_circle @0x004968b6 ----------------------------------------------------------
//
// Building[building_id].area is uint8_t[10][10]; the outer loop var pairs with origin_x/width_mask,
// the inner with origin_y/height_mask -- confirmed by the asm's PUSH/CALL argument order
// (EDX=param_2+outer&width_mask is the 2nd call arg "x"; EBX=a2+inner&height_mask is the 3rd call arg
// "y"; area index is `outer*10+inner`, i.e. area[outer(x)][inner(y)]). width_mask/height_mask HERE
// really are the geom ones (`general.width_mask`/`general.height_mask`, 0x00e15398/0x00e153b0) --
// confirmed by the exact addresses in the listing, matching sim_state.h's existing
// map_width_mask(v)/map_height_mask(v) helpers exactly (unlike _impl above, which uses the OTHER
// width/height pair -- see that function's note; the two siblings genuinely use different globals).
//
// RETURN VALUE: the committed export prototype returns uint32_t/game_t_Player, but nothing in the
// 0x99-byte body ever writes a return-carrying value with intent -- EAX's last INTENTIONAL write in
// the loop epilogue (0x004968e6: `MOV EAX,[outer counter]`) is a DEAD read (the very next instruction
// increments the counter in memory, not in EAX; EAX is never stored anywhere or used again before the
// function falls through to POP/RET), and it happens to survive in EAX until RET, which is exactly
// what the decompiler's "gVar1 = local_14" is reconstructing. There is no early-return path -- on any
// normal exit outer has just been incremented past 9, so this dead value is always 9. BOTH callers
// (sim_bldg_state_destroyed.cpp's `c.sight_add_circle(...)` and, for the sibling function,
// sim_bldg_unmap_footprint.h's explicit "Return value discarded by the original" comment) confirm the
// value is never consumed. Translated as void; see uncertainties[].
void sight_add_circle(const sim_view &v, const sight_add_circle_calls &c, uint32_t player,
                      int32_t origin_x, int32_t origin_y, int32_t building_id, uint8_t sight) {
    const cfg_building &b = v.cfg_buildings[building_id];
    for (int32_t dx = 0; dx < 10; ++dx) {
        for (int32_t dy = 0; dy < 10; ++dy) {
            if (b.area[dx][dy] != 0) {
                const uint32_t x =
                    static_cast<uint32_t>(origin_x + dx) & map_width_mask(v);
                const uint32_t y =
                    static_cast<uint32_t>(origin_y + dy) & map_height_mask(v);
                c.update_fow_plus(player, x, y, sight);
            }
        }
    }
}

// ---- llm_strat_sight_remove_circle @0x0049694f --------------------------------------------------------
// Identical shape to sight_add_circle (same footprint walk, same width_mask/height_mask == the geom
// ones, same dead-return-value posture -- see that function's banner). Calls llm_strat_fow_remove_sight
// per nonzero footprint cell instead of map_fow_UpdateFoWPlus.
void sight_remove_circle(const sim_view &v, const sight_remove_circle_calls &c, uint32_t player,
                         int32_t origin_x, int32_t origin_y, int32_t building_id, uint8_t radius) {
    const cfg_building &b = v.cfg_buildings[building_id];
    for (int32_t dx = 0; dx < 10; ++dx) {
        for (int32_t dy = 0; dy < 10; ++dy) {
            if (b.area[dx][dy] != 0) {
                const int32_t x =
                    static_cast<int32_t>(static_cast<uint32_t>(origin_x + dx) & map_width_mask(v));
                const int32_t y =
                    static_cast<int32_t>(static_cast<uint32_t>(origin_y + dy) & map_height_mask(v));
                c.remove_sight(player, x, y, radius);
            }
        }
    }
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void update_fow_plus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    sim_state st = state();
    detail::update_fow_plus(st.own, live_map_fow_update_fow_plus_calls(), player, x, y, sight);
}

void update_fow_plus_impl() {
    sim_state st = state();
    detail::update_fow_plus_impl(st.read, st.own);
}

void remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    sim_state st = state();
    detail::remove_sight(st.own, live_fow_remove_sight_calls(), player, x, y, radius);
}

void remove_sight_apply() {
    sim_state st = state();
    detail::remove_sight_apply(st.read, st.own);
}

void sight_add_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                      uint8_t sight) {
    const sim_view v = state().read;
    detail::sight_add_circle(v, live_sight_add_circle_calls(), player, origin_x, origin_y,
                             building_id, sight);
}

void sight_remove_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                         uint8_t radius) {
    const sim_view v = state().read;
    detail::sight_remove_circle(v, live_sight_remove_circle_calls(), player, origin_x, origin_y,
                                building_id, radius);
}

// ---- the rebind ABI shims ------------------------------------------------------------------------
//
// The committed prototypes are non-void (uint32_t / int32_t) while the translations above are
// correctly void. Header finding (F) carries why: the original's return value is dead
// register-reuse noise, 9 the always-observed value, confirmed against BOTH real callers by
// reading the disassembly. That is an RE fact about the original, so it never depended on the
// differential oracle and does not change now the oracle is gone. `sight_remove_circle` also
// takes `player` signed to match its committed prototype, so the shim casts.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {

uint32_t sight_add_circle(uint32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                          uint8_t sight) {
    mh::sim::sight_add_circle(player, origin_x, origin_y, building_id, sight);
    return 9;
}

int32_t sight_remove_circle(int32_t player, int32_t origin_x, int32_t origin_y, int32_t building_id,
                            uint8_t radius) {
    mh::sim::sight_remove_circle(static_cast<uint32_t>(player), origin_x, origin_y, building_id,
                                 radius);
    return 9;
}

} // namespace rebind_arm

} // namespace mh::sim
