//
// sim_new_game_init_selftest.cpp -- `simtest` offline oracle for llm_strat_new_game_init
// @0x00455df0 (sim/resid/sim_new_game_init.h/.cpp, RI-SIM sim_resid batch).
//
// NO SHADOW SITE -- session-entry-only writer, proof:OFFLINE (batch rule 1). This file is the only
// verification. Six outward calls, all firing UNCONDITIONALLY (no branch in the whole body besides
// the four loop bounds), so every mock must be reachable and checked on a single run -- there is no
// "ready gate" here to hide a naively-gated mock behind.
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_new_game_init_00455df0.asm)
// -- the .asm is the spec, never the .c beside it (the .c has silently lied elsewhere in this project):
//   0x00455e08: game::ClearAvailableProjects(), unconditional, first call.
//   0x00455e0d-0x00455ffc: PER-PLAYER loop, player=0..7 (MAX_PLAYERS, CMP ..,0x8 @0x00455e14 /
//     JL @0x00455e18). Per player, IN ORDER:
//     * 0x00455e24-0x00455e39: llm_strat_player_profile_init(player, controller_flags=0, race=0,
//       game_clock=0.0, color_index=0, name_str=&0x0050109d, side_id=-1).
//     * 0x00455e3e-0x00455e97: progress[player][row] = {available=0, acquired=0, f3=0} for
//       row=0..299 (PROGRESS_ROW_COUNT, CMP ..,0x12c @0x00455e45 / JL @0x00455e4c) -- three separate
//       byte stores, not a whole-record memset.
//     * 0x00455e99-0x00455f5a: Weapon[w].{range_min,range_max,missing,power,fire_range}[player] =
//       Weapon[w].{...}[8] (the baseline column) for w=0..31 (MAX_WEAPONS, CMP ..,0x20 @0x00455ea0 /
//       JL @0x00455ea4) -- five separate field copies.
//     * 0x00455f5f-0x00455fd9: Unit[u].{step_speed,turn_speed,armor_prob}[player] = Unit[u].{...}[8]
//       for u=0..99 (MAX_UNITS, CMP ..,0x64 @0x00455f66 / JL @0x00455f6a) -- three separate field
//       copies.
//     * 0x00455fdb-0x00455ffc: llm_strat_prod_shuttle_slot_release(player, slot) for slot=0..9
//       (SHUTTLE_SLOTS_PER_PLAYER, CMP ..,0xa @0x00455fe2 / JL @0x00455fe6).
//   0x00456001: llm_strat_invasion_alert_reset_all(), unconditional, once, right after the loop.
//   0x00456006-0x00456012: utils_fill_data(&fog_of_war, 0x90000 [589824], 0) -- a WHOLE-REGION clear
//     via the own.fog_of_war_base() address escape, not a field-level write.
//   0x00456017: save_misc_dword = 0xf.
//   0x0045602f: llm_map_set_zoom_scale(1.0, 1.0), unconditional, last call.
//
#include "sim/resid/sim_new_game_init.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// One recorded call, tagged by which of the six outward callees fired -- same "single tagged-event
// vector shared by every mocked callee" shape as sim_bldg_scrap_stored_units_selftest.cpp's
// `call_log`, widened to six kinds so both COUNT and ORDER across every callee are visible from one
// log. Only the fields relevant to `kind` are meaningful; the rest keep their default.
struct calls_log {
    enum kind_t { CLEAR_PROJECTS,
                  PROFILE_INIT,
                  SHUTTLE_RELEASE,
                  INVASION_RESET,
                  FILL_DATA,
                  ZOOM_SCALE };
    struct ev {
        kind_t      kind             = CLEAR_PROJECTS;
        uint32_t    player           = 0xffffffffu;
        uint32_t    controller_flags = 0;
        uint32_t    race             = 0;
        uint32_t    color_index      = 0;
        double      game_clock       = 0.0;
        const char *name_str         = nullptr;
        int32_t     side_id          = 0;
        int32_t     slot             = -1;
        void       *ptr              = nullptr;
        uint32_t    size             = 0;
        uint8_t     default_         = 0;
        double      zoom_x           = 0.0;
        double      zoom_y           = 0.0;
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
calls_log g_log;

// Bounds-checked event fetch -- a missing event fails its own `ck` rather than reading past the
// vector (a mutation that drops calls must fail loudly, not read garbage that happens to compare
// equal).
const calls_log::ev &ev_at(size_t n, const char *what) {
    static const calls_log::ev empty{};
    if (n >= g_log.events.size()) {
        ck(false, what);
        return empty;
    }
    return g_log.events[n];
}

const new_game_init_calls &recording_calls() {
    static const new_game_init_calls c = {
        // clear_available_projects() -- 0x00455e08, unconditional, fires before the per-player loop.
        []() -> void {
            calls_log::ev e;
            e.kind = calls_log::CLEAR_PROJECTS;
            g_log.events.push_back(e);
        },
        // player_profile_init(player, controller_flags, race, game_clock, color_index, name_str,
        // side_id) -- 0x00455e24-0x00455e39.
        [](uint32_t player, uint32_t controller_flags, uint32_t race, double game_clock,
           uint32_t color_index, char *name_str, int32_t side_id) -> void {
            calls_log::ev e;
            e.kind             = calls_log::PROFILE_INIT;
            e.player           = player;
            e.controller_flags = controller_flags;
            e.race             = race;
            e.game_clock       = game_clock;
            e.color_index      = color_index;
            e.name_str         = name_str;
            e.side_id          = side_id;
            g_log.events.push_back(e);
        },
        // prod_shuttle_slot_release(player, slot) -- 0x00455fef-0x00455ff5.
        [](int32_t player, int32_t slot) -> void {
            calls_log::ev e;
            e.kind   = calls_log::SHUTTLE_RELEASE;
            e.player = (uint32_t)player;
            e.slot   = slot;
            g_log.events.push_back(e);
        },
        // invasion_alert_reset_all() -- 0x00456001, unconditional, fires once right after the loop.
        []() -> void {
            calls_log::ev e;
            e.kind = calls_log::INVASION_RESET;
            g_log.events.push_back(e);
        },
        // fill_data(ptr, size, default_) -- 0x00456006-0x00456012. This mock PERFORMS the fill (not
        // just records it): an address escape that writes nothing logs identically to one that
        // works, so the test proves the write by reading the region back afterward, exactly as a
        // real utils_fill_data would leave it.
        [](void *ptr, uint32_t size, uint8_t default_) -> void * {
            calls_log::ev e;
            e.kind     = calls_log::FILL_DATA;
            e.ptr      = ptr;
            e.size     = size;
            e.default_ = default_;
            g_log.events.push_back(e);
            memset(ptr, default_, size);
            return ptr;
        },
        // map_set_zoom_scale(zoom_x, zoom_y) -- 0x0045602f, unconditional, fires last.
        [](double zoom_x, double zoom_y) -> void {
            calls_log::ev e;
            e.kind   = calls_log::ZOOM_SCALE;
            e.zoom_x = zoom_x;
            e.zoom_y = zoom_y;
            g_log.events.push_back(e);
        },
    };
    return c;
}

} // namespace

void run_new_game_init_tests() {
    sim_fixture fx;
    fx.reset();

    // ---- seed the baseline (column 8) Weapon/Unit stat columns this closure reads FROM --------------
    // w=0 (first) and w=31 (MAX_WEAPONS-1, last) -- FIVE DISTINCT values per weapon so a field-swap
    // (e.g. power landing in fire_range's slot) disagrees with the fixture.
    fx.cfg_weapons[0].range_min[8]   = 101;
    fx.cfg_weapons[0].range_max[8]   = 102;
    fx.cfg_weapons[0].missing[8]     = 103;
    fx.cfg_weapons[0].power[8]       = 104.5;
    fx.cfg_weapons[0].fire_range[8]  = 105.5;
    fx.cfg_weapons[31].range_min[8]  = 201;
    fx.cfg_weapons[31].range_max[8]  = 202;
    fx.cfg_weapons[31].missing[8]    = 203;
    fx.cfg_weapons[31].power[8]      = 204.5;
    fx.cfg_weapons[31].fire_range[8] = 205.5;

    // u=0 (first) and u=99 (MAX_UNITS-1, last) -- THREE DISTINCT values per unit.
    fx.cfg_units[0].step_speed[8]  = 11.5;
    fx.cfg_units[0].turn_speed[8]  = 12.5;
    fx.cfg_units[0].armor_prob[8]  = 13;
    fx.cfg_units[99].step_speed[8] = 21.5;
    fx.cfg_units[99].turn_speed[8] = 22.5;
    fx.cfg_units[99].armor_prob[8] = 23;

    // Poison the per-player columns (0..7) of those same records with a sentinel that is NEITHER the
    // baseline value NOR zero, so "never copied" and "read from column [player] instead of [8]" are
    // both directly observable.
    for (int32_t w : {0, 31}) {
        for (int32_t p = 0; p < 8; ++p) {
            fx.cfg_weapons[(size_t)w].range_min[p]  = -1;
            fx.cfg_weapons[(size_t)w].range_max[p]  = -1;
            fx.cfg_weapons[(size_t)w].missing[p]    = -1;
            fx.cfg_weapons[(size_t)w].power[p]      = -1.0;
            fx.cfg_weapons[(size_t)w].fire_range[p] = -1.0;
        }
    }
    for (int32_t u : {0, 99}) {
        for (int32_t p = 0; p < 8; ++p) {
            fx.cfg_units[(size_t)u].step_speed[p] = -1.0;
            fx.cfg_units[(size_t)u].turn_speed[p] = -1.0;
            fx.cfg_units[(size_t)u].armor_prob[p] = -1;
        }
    }

    // Poison EVERY progress record (all 8 players * 300 rows) with a nonzero sentinel, so the
    // per-field byte zero (0x00455e64/0x00455e7a/0x00455e90) is directly observable at every slot the
    // loop must reach.
    for (auto &rec : fx.progress) {
        rec.available = 0x11;
        rec.acquired  = 0x22;
        rec.f3        = 0x33;
    }

    // The fog-of-war region: poison the FIRST, LAST, and one INTERIOR byte with a sentinel that is
    // NOT the fill value (0) -- fx.reset() already zeroed the whole buffer, and starting from zero
    // would let a no-op escape (writes nothing) pass silently.
    fx.fog_of_war_bytes[0]      = 0xAA;
    fx.fog_of_war_bytes[300000] = 0xAA; // interior
    fx.fog_of_war_bytes[589823] = 0xAA; // last byte of the 589824-byte (0x90000) region

    g_log.reset();
    sim_store own = fx.store();
    detail::new_game_init(fx.view(), own, recording_calls());

    // =================================================================================================
    // T1 -- the overall call sequence: 1 (clear_available_projects) + 8*(1 profile_init + 10
    // shuttle_release) + 1 (invasion_alert_reset_all) + 1 (fill_data) + 1 (map_set_zoom_scale) = 92
    // events, in that exact order. A wrong MAX_PLAYERS/SHUTTLE_SLOTS_PER_PLAYER, or a callee
    // dropped/reordered/duplicated, moves this count or the indices every case below checks.
    // =================================================================================================
    ck_eq((uint32_t)g_log.events.size(), 92u,
          "T1: 1 clear + 8*(1 profile_init + 10 shuttle_release) + invasion + fill + zoom = 92 calls, "
          "0x00455e08-0x0045602f");

    // ---- T2: clear_available_projects is the VERY FIRST call, unconditional, 0x00455e08. -----------
    {
        const calls_log::ev &e = ev_at(0, "T2: event[0] exists");
        ck(e.kind == calls_log::CLEAR_PROJECTS,
           "T2: event[0] is clear_available_projects(), fires before the per-player loop, 0x00455e08");
    }

    // ---- T3: player_profile_init, player=0 (first loop iteration) -- full register/stack split, ----
    // 0x00455e24-0x00455e39.
    {
        const calls_log::ev &e = ev_at(1, "T3: event[1] exists");
        ck(e.kind == calls_log::PROFILE_INIT,
           "T3: event[1] is player_profile_init for player 0, 0x00455e39");
        ck_eq(e.player, 0u, "T3: player_profile_init player=0 (EAX), 0x00455e36/0x00455e39");
        ck_eq(e.controller_flags, 0u, "T3: controller_flags=0 (EDX, XOR EDX,EDX), 0x00455e34");
        ck_eq(e.race, 0u, "T3: race=0 (EBX, XOR EBX,EBX), 0x00455e32");
        ck_eq_d(e.game_clock, 0.0, "T3: game_clock=0.0 (two PUSH 0x0), 0x00455e2e/0x00455e30");
        ck_eq(e.color_index, 0u, "T3: color_index=0 (PUSH 0x0), 0x00455e2c");
        ck(e.name_str == &fx.empty_name_str,
           "T3: name_str is the ADDRESS of the blank-name byte (0x0050109d / v.empty_name_str), "
           "0x00455e26/0x00455e2b");
        ck_eq((uint32_t)(uint8_t)*e.name_str, 0u,
              "T3: the blank-name byte reads as an empty C string, 0x0050109d");
        ck_eq((uint32_t)e.side_id, (uint32_t)-1, "T3: side_id=-1 (PUSH -0x1), 0x00455e24");
    }

    // ---- T4: player_profile_init, player=7 (MAX_PLAYERS-1, LAST loop iteration) -- proves the -------
    // player-loop bound is 8 (CMP ..,0x8 @0x00455e14 / JL @0x00455e18) and that every player, not just
    // player 0, gets the same blank profile.
    {
        const size_t         idx = 1 + 7 * 11; // 78
        const calls_log::ev &e   = ev_at(idx, "T4: player=7's profile_init event exists");
        ck(e.kind == calls_log::PROFILE_INIT,
           "T4: event[78] is player_profile_init for player 7, player-loop bound 0x00455e14/0x00455e18");
        ck_eq(e.player, 7u, "T4: player_profile_init player=7 (last iteration)");
        ck(e.name_str == &fx.empty_name_str, "T4: player 7 gets the SAME blank-name address as player 0");
        ck_eq((uint32_t)e.side_id, (uint32_t)-1, "T4: side_id=-1 for player 7 too");
    }

    // ---- T5: progress zero-out -- player=0/row=0 (first record), player=7/row=299 (last record of ---
    // the last player, row-loop bound CMP ..,0x12c @0x00455e45 / JL @0x00455e4c), and an interior
    // record. All were poisoned to 0x11/0x22/0x33 above.
    {
        const player_progress &p0 = fx.progress[(size_t)0 * PROGRESS_ROW_COUNT + 0];
        ck_eq(p0.available, 0u, "T5a: progress[player=0][row=0].available=0, 0x00455e64");
        ck_eq(p0.acquired, 0u, "T5a: progress[player=0][row=0].acquired=0, 0x00455e7a");
        ck_eq(p0.f3, 0u, "T5a: progress[player=0][row=0].f3=0, 0x00455e90");

        const player_progress &pLast =
            fx.progress[(size_t)7 * PROGRESS_ROW_COUNT + (PROGRESS_ROW_COUNT - 1)];
        ck_eq(pLast.available, 0u,
              "T5b: progress[player=7][row=299].available=0 -- last row of the last player, "
              "row-loop bound 0x00455e45/0x00455e4c");
        ck_eq(pLast.acquired, 0u, "T5b: progress[player=7][row=299].acquired=0");
        ck_eq(pLast.f3, 0u, "T5b: progress[player=7][row=299].f3=0");

        const player_progress &pMid = fx.progress[(size_t)3 * PROGRESS_ROW_COUNT + 150];
        ck_eq(pMid.available, 0u, "T5c: progress[player=3][row=150] (interior) .available=0");
        ck_eq(pMid.acquired, 0u, "T5c: progress[player=3][row=150] (interior) .acquired=0");
        ck_eq(pMid.f3, 0u, "T5c: progress[player=3][row=150] (interior) .f3=0");
    }

    // ---- T6: Weapon baseline copy -- w=0 (first) and w=31 (MAX_WEAPONS-1, last), player=0 and -------
    // player=7 -- FIVE fields, each a DISTINCT value, so consuming the wrong field disagrees. Column
    // 8 (the baseline itself) must stay untouched by the per-player writes.
    {
        ck_eq((uint32_t)fx.cfg_weapons[0].range_min[0], 101u,
              "T6a: weapon[0].range_min[player=0] = baseline[8]=101, 0x00455ec6/0x00455ecc");
        ck_eq((uint32_t)fx.cfg_weapons[0].range_max[0], 102u,
              "T6a: weapon[0].range_max[player=0] = baseline[8]=102, 0x00455ee8/0x00455eee");
        ck_eq((uint32_t)fx.cfg_weapons[0].missing[0], 103u,
              "T6a: weapon[0].missing[player=0] = baseline[8]=103, 0x00455f0a/0x00455f10");
        ck_eq_d(fx.cfg_weapons[0].power[0], 104.5,
                "T6a: weapon[0].power[player=0] = baseline[8]=104.5 (x87 FLD/FSTP), 0x00455f2c/0x00455f32");
        ck_eq_d(fx.cfg_weapons[0].fire_range[0], 105.5,
                "T6a: weapon[0].fire_range[player=0] = baseline[8]=105.5 (x87), 0x00455f4e/0x00455f54");

        ck_eq((uint32_t)fx.cfg_weapons[31].range_min[7], 201u,
              "T6b: weapon[31].range_min[player=7] = baseline[8]=201 -- last weapon, last player, "
              "weapon-loop bound 0x00455ea0/0x00455ea4");
        ck_eq((uint32_t)fx.cfg_weapons[31].range_max[7], 202u, "T6b: weapon[31].range_max[player=7]=202");
        ck_eq((uint32_t)fx.cfg_weapons[31].missing[7], 203u, "T6b: weapon[31].missing[player=7]=203");
        ck_eq_d(fx.cfg_weapons[31].power[7], 204.5, "T6b: weapon[31].power[player=7]=204.5 (x87)");
        ck_eq_d(fx.cfg_weapons[31].fire_range[7], 205.5, "T6b: weapon[31].fire_range[player=7]=205.5 (x87)");

        ck_eq((uint32_t)fx.cfg_weapons[0].range_min[8], 101u,
              "T6c: baseline column [8] is UNTOUCHED (source, never destination), weapon[0].range_min[8]");
        ck_eq_d(fx.cfg_weapons[31].fire_range[8], 205.5,
                "T6c: baseline column [8] is UNTOUCHED, weapon[31].fire_range[8]");
    }

    // ---- T7: Unit baseline copy -- u=0 (first) player=0, and u=99 (MAX_UNITS-1, last) player=7 -- ----
    // THREE fields, each DISTINCT, plus the baseline column [8] left untouched.
    {
        ck_eq_d(fx.cfg_units[0].step_speed[0], 11.5,
                "T7a: unit[0].step_speed[player=0] = baseline[8]=11.5 (x87), 0x00455f89/0x00455f8f");
        ck_eq_d(fx.cfg_units[0].turn_speed[0], 12.5,
                "T7a: unit[0].turn_speed[player=0] = baseline[8]=12.5 (x87), 0x00455fab/0x00455fb1");
        ck_eq((uint32_t)fx.cfg_units[0].armor_prob[0], 13u,
              "T7a: unit[0].armor_prob[player=0] = baseline[8]=13, 0x00455fcd/0x00455fd3");

        ck_eq_d(fx.cfg_units[99].step_speed[7], 21.5,
                "T7b: unit[99].step_speed[player=7] = baseline[8]=21.5 -- last unit, last player, "
                "unit-loop bound 0x00455f66/0x00455f6a");
        ck_eq_d(fx.cfg_units[99].turn_speed[7], 22.5, "T7b: unit[99].turn_speed[player=7]=22.5");
        ck_eq((uint32_t)fx.cfg_units[99].armor_prob[7], 23u, "T7b: unit[99].armor_prob[player=7]=23");

        ck_eq_d(fx.cfg_units[0].step_speed[8], 11.5,
                "T7c: baseline column [8] is UNTOUCHED, unit[0].step_speed[8]");
        ck_eq((uint32_t)fx.cfg_units[99].armor_prob[8], 23u,
              "T7c: baseline column [8] is UNTOUCHED, unit[99].armor_prob[8]");
    }

    // ---- T8: shuttle slot release -- FIRST call (player=0, slot=0) and LAST call (player=7, slot=9, ---
    // slot-loop bound CMP ..,0xa @0x00455fe2 / JL @0x00455fe6).
    {
        const calls_log::ev &e0 = ev_at(2, "T8a: player=0,slot=0 shuttle_release event exists");
        ck(e0.kind == calls_log::SHUTTLE_RELEASE,
           "T8a: event[2] is prod_shuttle_slot_release(player=0, slot=0), first of the inner loop, "
           "0x00455fef/0x00455ff5");
        ck_eq(e0.player, 0u, "T8a: shuttle_release player=0 (EAX)");
        ck_eq((uint32_t)e0.slot, 0u, "T8a: shuttle_release slot=0 (EDX), first iteration");

        const size_t         idxLast = 2 + 7 * 11 + 9; // 88
        const calls_log::ev &eLast   = ev_at(idxLast, "T8b: player=7,slot=9 shuttle_release event exists");
        ck(eLast.kind == calls_log::SHUTTLE_RELEASE,
           "T8b: event[88] is prod_shuttle_slot_release(player=7, slot=9), the LAST shuttle-release "
           "call, immediately before invasion_alert_reset_all, slot-loop bound 0x00455fe2/0x00455fe6");
        ck_eq(eLast.player, 7u, "T8b: shuttle_release player=7");
        ck_eq((uint32_t)eLast.slot, 9u, "T8b: shuttle_release slot=9 (SHUTTLE_SLOTS_PER_PLAYER-1)");
    }

    // ---- T9: invasion_alert_reset_all -- event[89], right after the per-player loop, 0x00456001. -----
    {
        const calls_log::ev &e = ev_at(89, "T9: invasion_alert_reset_all event exists");
        ck(e.kind == calls_log::INVASION_RESET,
           "T9: event[89] is invasion_alert_reset_all(), fires once immediately after the per-player "
           "loop, 0x00456001");
    }

    // ---- T10: fill_data -- event[90], the WHOLE-REGION fog-of-war clear (0x00456006-0x00456012). -----
    // The mock actually performs the fill; the byte checks prove the escape address really is the
    // region base and really covers all 589824 (0x90000) bytes.
    {
        const calls_log::ev &e = ev_at(90, "T10: fill_data event exists");
        ck(e.kind == calls_log::FILL_DATA,
           "T10: event[90] is utils_fill_data(), fires right after invasion_alert_reset_all, "
           "0x00456012");
        ck(e.ptr == fx.fog_of_war_bytes.data(),
           "T10a: fill_data ptr == own.fog_of_war_base(), the WHOLE RID_FOG_OF_WAR escape, "
           "0x0045600d/0x00456012");
        ck_eq(e.size, 0x90000u,
              "T10b: fill_data size = 589824 (0x90000) bytes -- the WHOLE region, not one field, "
              "0x00456006");
        ck_eq((uint32_t)e.default_, 0u, "T10c: fill_data default_=0, 0x0045600b");

        ck_eq((uint32_t)fx.fog_of_war_bytes[0], 0u,
              "T10d: fog_of_war byte[0] (FIRST byte of the region) actually cleared -- an escape that "
              "writes nothing would still leave the recorded args looking correct");
        ck_eq((uint32_t)fx.fog_of_war_bytes[300000], 0u,
              "T10e: fog_of_war byte[300000] (INTERIOR of the region) actually cleared");
        ck_eq((uint32_t)fx.fog_of_war_bytes[589823], 0u,
              "T10f: fog_of_war byte[589823] (LAST byte, 589824-byte / 0x90000 extent) actually cleared");
    }

    // ---- T11: save_misc_dword = 0xf, 0x00456017. -------------------------------------------------
    { ck_eq((uint32_t)fx.save_misc_dword, 0xfu, "T11: save_misc_dword = 0xf, 0x00456017"); }

    // ---- T12: map_set_zoom_scale -- event[91], the LAST call the function makes, 0x0045602f. --------
    {
        const calls_log::ev &e = ev_at(91, "T12: map_set_zoom_scale event exists");
        ck(e.kind == calls_log::ZOOM_SCALE,
           "T12: event[91] is map_set_zoom_scale(), the LAST call the function makes, 0x0045602f");
        ck_eq_d(e.zoom_x, 1.0, "T12: zoom_x=1.0 (0x3ff00000), 0x00456021/0x00456026");
        ck_eq_d(e.zoom_y, 1.0, "T12: zoom_y=1.0 (0x3ff00000), 0x00456028/0x0045602d");
    }
}

} // namespace mh::sim::test
