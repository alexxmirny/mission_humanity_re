//
// sim/resid/sim_land_players_on_planet.cpp -- see sim_land_players_on_planet.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_game_land_players_on_planet_0045534e.asm), the Ghidra .c
// being a draft.
//
#include "sim/resid/sim_land_players_on_planet.h"

#include "addr/mh_calls.gen.h"   // typed callables for the frontier originals we still call OUT to
#include "sim/sim_event_codes.h" // SESSION_SP
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const land_players_on_planet_calls &live_land_players_on_planet_calls() {
    static const land_players_on_planet_calls c = {
        MH_LIBMH_BIND(llm_strat_claim_landing_spot),
        MH_LIBMH_BIND(llm_strat_unit_create),
        MH_LIBMH_BIND(llm_strat_spawn_ai_base),
        MH_LIBMH_BIND(llm_strat_landing_spots_reroll_out_of_bounds),
        MH_LIBMH_BIND(llm_strat_init_human_player_data),
        mh::state::evt::cam_set_col,
        mh::state::evt::cam_set_row,
        mh::state::evt::inv_viewport,
        MH_LIBMH_BIND(game_GetStartingUnit),
        MH_CRT(utils_sprintf__vssii),
        MH_CRT(w_sprintf__vii),
    };
    return c;
}

namespace {

// player_profile.status_flags bits (E_STRAT_PLAYER_STATUS) -- b0 slot enabled, b2 human-controlled.
// TU-local copy per the established project convention (sim_landing_spot.cpp / sim_step.cpp /
// sim_game_speed_recompute.cpp all keep their own copy rather than sharing one across headers --
// see sim_landing_spot.h's "CONDUCTOR FIX" comment: `inline constexpr` at HEADER scope collided
// C2374/C2086 the moment one aggregating TU included more than one sim/ header defining the same
// name, so these stay TU-local in the .cpp, not exported from this file's header).
constexpr uint32_t STRAT_PLAYER_STATUS_SLOT_ENABLED = 0x1u;
constexpr uint32_t STRAT_PLAYER_STATUS_HUMAN        = 0x4u;

// game_e_race member values, read directly off this function's own CMP immediates (0x004553ff /
// 0x004554e1 / 0x0045556b, all `CMP ...,0x2` gating the ALIEN arm). Same values as
// sim_game_get_starting_unit.h's RACE_ALIEN/RACE_HUMAN (that header's own derivation + cross-check
// citation); kept as a separate TU-local copy for the same ODR-safety reason as the status bits
// above -- this TU does not include that sibling header (game_GetStartingUnit is a frontier callee
// reached through mh::call::, not an intra-slice include).
constexpr uint32_t RACE_ALIEN = 2u;

// The System record's stride in int32 UNITS (0x8c bytes / 4 = 35). Same constant
// sim_player_presence_lost.cpp's SYSTEM_STRIDE_INTS names for the identical `System` region.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;

// system_define_index_base[system*35 + 4] -- 0x0045543e-0x0045544b reads dword ptr
// [CurrentSystem*0x8c + 0xbe1a40], and 0xbe1a40 - 0xbe1a30 (system_define_index_base's own bound
// base, per sim_state.h's comment) = 0x10 bytes = int32 index 4. NO committed field name exists for
// this element: mh_addrs.gen.h's own `System` comment documents that the separately-typed
// `cfg_final_struct_System` is anchored 8 bytes LATE and is a known, deliberately-unfixed gap, so the
// Ghidra .c draft's guess ("planets[1]") cannot be trusted as a semantic name -- only the byte offset
// is verified against the instruction (see uncertainties).
constexpr int32_t SYSTEM_FIELD_INDEX_4 = 4;

// Divide-by-64 truncating-toward-zero, the same `SAR ...,0x1f / SHL ...,0x6 / SBB / SAR ...,0x6`
// idiom sim_bldg_state_charge.cpp's fine_to_tile() documents for the /32 case (this function's two
// sites at 0x0045562e-0x00455636 and 0x0045566e-0x00455676 are the identical shape with n=6 instead
// of n=5) -- value-for-value C's truncating `/ 64`.
inline int32_t div_trunc_64(int32_t x) { return x / 64; }

} // namespace

namespace detail {

// ---- llm_game_land_players_on_planet @0x0045534e ---------------------------------------------------
void land_players_on_planet(const sim_view &v, sim_store &own,
                            const land_players_on_planet_calls &c, int32_t planet_index,
                            const planet_map_session_init_calls &c_pmsi) {
    // C10: the landing observer, BEFORE any of the body -- matching where the harness trampoline used
    // to run (a run-before detour that jumped to the stolen prologue afterwards). It must precede the
    // body because what it does is convert player slots HUMAN->AI, and the landing loop below reads
    // those very flags to decide each slot's arm. Null in every run that does not ask for it, which
    // includes every offline oracle. See the header for why this is not a detour on our own entry.
    fire_land_players_observer();

    // 0x00455369: intra-slice DIRECT call (sim_resid rule 2), not mh::call::.
    mh::sim::detail::planet_map_session_init(v, own, c_pmsi);

    // 0x0045536e-0x00455391: outer-planet-landed latch for planets 7..0x1e (exclusive of 0x1f, which
    // gets its own latch at the tail).
    if (planet_index > 6 && planet_index < 0x1f && own.outer_planet_landed_flag() == 0) {
        own.outer_planet_landed_flag() = -1;
    }

    // 0x00455391: frontier reroll helper, no args, no return used.
    c.landing_spots_reroll_out_of_bounds();

    // 0x00455396-0x004555bf: the per-player loop.
    for (int32_t player = 0; player < MAX_PLAYERS; ++player) {
        // 0x004553b4/0x004553bb: disabled slots are skipped entirely -- not even the
        // primary_mother_unit mark at the loop tail runs for them.
        if ((v.profiles[player].status_flags & STRAT_PLAYER_STATUS_SLOT_ENABLED) == 0) continue;

        // ---- THE DOMAIN-FACT BRANCH: TEST byte [player*0x740+0xcff060],0x4 @0x004553c8, JZ
        // @0x004553cf. HUMAN bit CLEAR -> AI arm (0x0045550a); HUMAN bit SET -> HUMAN arm
        // (0x004553d5, falls through). ONLY the AI arm reaches c.spawn_ai_base -- see the header
        // banner's domain-fact note.
        if ((v.profiles[player].status_flags & STRAT_PLAYER_STATUS_HUMAN) == 0) {
            // ==== 0x0045550a: AI ARM =========================================================
            // 0x0045550a-0x0045552e: eligibility gate. Skip claim+spawn (but still fall through to
            // the primary_mother_unit mark below) when session_mode==SP AND player>=2 AND
            // player > cfg_planets[G_PLANET_INDEX].enemy. NOTE: G_PLANET_INDEX (*v.planet_index) is
            // the UNRELATED "current planet" global, NOT this function's own `planet_index`
            // parameter -- see the header banner and uncertainties.
            const bool eligible =
                *v.session_mode != SESSION_SP || player < 2 ||
                player <= static_cast<int32_t>(v.cfg_planets[*v.planet_index].enemy);
            if (eligible) {
                // 0x00455530: claim, then re-read the just-claimed landing_x/landing_y (0x00455536-
                // 0x00455568) before computing the race flag (0x0045556b-0x00455584) and calling
                // spawn_ai_base (0x00455597; register map EAX=player, EDX=is_alien, EBX=x, ECX=y).
                c.claim_landing_spot(static_cast<uint32_t>(player), static_cast<uint32_t>(planet_index));
                const int32_t x        = v.profiles[player].landing_x[planet_index];
                const int32_t y        = v.profiles[player].landing_y[planet_index];
                const int32_t is_alien = (v.profiles[player].race == RACE_ALIEN) ? 1 : 0;
                c.spawn_ai_base(player, is_alien, x, y);
            }
        } else {
            // ==== 0x004553d5: HUMAN ARM ======================================================
            c.claim_landing_spot(static_cast<uint32_t>(player), static_cast<uint32_t>(planet_index));

            // 0x004553ef-0x0045542d: build "init\{A,H}_<planet_index><landing_spot_index>.DMP" into
            // the scratch buffer at 0x00e58146. Race letter is the literal ONE-CHARACTER string
            // (see the header banner's race-letter note) -- NOT the word "ALIEN"/"HUMAN".
            const char *race_letter = (v.profiles[player].race == RACE_ALIEN) ? "A" : "H";
            c.land_dmp_sprintf(own.land_dmp_scratch(), "%s%s_%02d%02d.DMP", "init\\", race_letter,
                               planet_index, v.profiles[player].landing_spot_index[planet_index]);

            // 0x00455435-0x00455451: only attempt the starting-unit spawn when session_mode != SP,
            // OR (session_mode == SP AND this function's OWN planet_index equals
            // system_define_index_base[CurrentSystem*35+4]) -- see SYSTEM_FIELD_INDEX_4's comment.
            const bool try_starting_unit =
                *v.session_mode != SESSION_SP ||
                planet_index ==
                    v.system_define_index_base[*v.current_system * SYSTEM_STRIDE_INTS +
                                               SYSTEM_FIELD_INDEX_4];
            if (try_starting_unit) {
                const int32_t starting_unit = c.get_starting_unit(v.profiles[player].race);
                if (starting_unit == 0) {
                    own.game_land_no_start_unit_flag() = 1;
                } else {
                    const int32_t x = v.profiles[player].landing_x[planet_index];
                    const int32_t y = v.profiles[player].landing_y[planet_index];
                    // 0x004554aa: register map EAX=x, EDX=y, EBX=unit, ECX=player, stack=is_ship.
                    // is_ship is the LITERAL 2 the assembly pushes (0x00455472/0x00455477), not a
                    // boolean -- see the header banner's note.
                    c.unit_create(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                  static_cast<uint16_t>(starting_unit), static_cast<uint16_t>(player),
                                  2);
                    // 0x004554af-0x004554c5: if this is the local player's own unit, seed control
                    // group 0 with unit id 1.
                    if (static_cast<int32_t>(static_cast<uint16_t>(*v.player_side)) == player) {
                        own.ctrl_group_at(0).count       = 1;
                        own.ctrl_group_at(0).unit_ids[0] = 1;
                    }
                }
            }

            // 0x004554da-0x00455500: is_alien flag for init_human_player_data (same race==ALIEN
            // test, re-derived independently by the assembly rather than reusing the earlier one).
            const int32_t is_alien = (v.profiles[player].race == RACE_ALIEN) ? 1 : 0;
            c.init_human_player_data(static_cast<uint32_t>(player), is_alien);
        }

        // 0x0045559c: both arms converge here -- mark this player's primary mother-unit slot for
        // the landed planet. (0x004555b5-0x004555bf: the assembly re-reads PlayerSide and CMPs it
        // against `player`, but nothing consumes the resulting flags before the unconditional JMP to
        // the loop increment -- a provably inert compare, not translated.)
        own.profile_at(player).primary_mother_unit[planet_index] = 1;
    }

    // 0x004555c4-0x0045560b: "[x,y]" debug text for the LOCAL_PLAYER_SLOT's landing spot (distinct
    // from PlayerSide, used below) into G_TEXT_TMP. Unconditional, independent of the loop above.
    {
        const int32_t local_slot = *v.local_player_slot;
        const int32_t x          = v.profiles[local_slot].landing_x[planet_index];
        const int32_t y          = v.profiles[local_slot].landing_y[planet_index];
        c.coord_msg_sprintf(own.text_scratch(), L"[%d,%d]", x, y);
    }

    // 0x0045560e-0x00455689: center the camera on PlayerSide's landing spot, torus-wrapped.
    {
        const int32_t side = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
        const int32_t col =
            (v.profiles[side].landing_x[planet_index] - div_trunc_64(*v.win_w)) &
            static_cast<int32_t>(map_width_mask(v));
        c.cam_set_col(col);
        const int32_t row =
            (v.profiles[side].landing_y[planet_index] - div_trunc_64(*v.win_h)) &
            static_cast<int32_t>(map_height_mask(v));
        c.cam_set_row(row);

        // 0x0045568e-0x004556c9: UI base-marker slot 0 gets the same PlayerSide landing coords
        // (unwrapped, unlike the camera pan target above).
        own.ui_base_marker_coords_at(0).cam_col = v.profiles[side].landing_x[planet_index];
        own.ui_base_marker_coords_at(0).cam_row = v.profiles[side].landing_y[planet_index];
    }

    // 0x004556ce-0x004556e9: outer-planet-land-state latch, planet 0x1f only.
    if (planet_index == 0x1f && own.outer_planet_land_state() == 0) {
        own.outer_planet_land_state() = -1;
    }

    // 0x004556e9: frontier tail call.
    c.cam_mark_viewport_dirty();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void land_players_on_planet(uint32_t planet_index) {
    sim_state st = state();
    detail::land_players_on_planet(st.read, st.own, live_land_players_on_planet_calls(),
                                   static_cast<int32_t>(planet_index));
}

// ---- the landing observer (C10) -- see the header for why it is a seam and not an entry detour -----
namespace {
void (*g_land_observer)() = nullptr;
} // namespace

void set_land_players_observer(void (*fn)()) { g_land_observer = fn; }
void (*land_players_observer())() { return g_land_observer; }
void fire_land_players_observer() {
    if (g_land_observer) g_land_observer();
}

} // namespace mh::sim
