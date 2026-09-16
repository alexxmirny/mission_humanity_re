//
// sim_bldg_state_mine_check_deposits_selftest.cpp -- `simtest` oracle for
// llm_strat_bldg_state_mine_check_deposits @0x004743e1 (sim/sim_bldg_state_mine.h/.cpp, RI-SIM /
// SIM1-G4).
//
// EXPECTED BEHAVIOUR from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_mine_check_deposits_004743e1.asm) and the header's own
// derivation (sim/sim_bldg_state_mine.h):
//   0x0047440d-0x0047441b: cur_building->cycle_progress = 0.0, UNCONDITIONAL -- written before the
//     sum is even read, on both branches.
//   0x00474429-0x0047445b: sum the four mines[cur_player][sub_id].deposit_slot[i].extract_rate (raw
//     dword at slot-relative offset 9, 13-byte stride) into a local accumulator.
//   0x0047445d/0x00474461: sum > 0 -> state = MINE_EXTRACTING (0x74), no further effect
//     (0x00474463-0x0047446e), straight to the tail.
//   sum <= 0 -> state = MINE_DEPLETED (0x75) (0x00474473-0x0047447e). Then, ONLY for
//     cur_player == PlayerSide (0x0047447e-0x0047448b):
//       SIM_ACTIVE != 0 (0x00474491-0x00474498) -> race-dependent voice line via snd_play
//         (sound_id = (player_race==2 ? 0x12 : 0) + 3, volume 100, 0x0047449a-0x004744c3).
//         SIM_ACTIVE == 0 SKIPS ONLY THIS CALL (JZ lands directly at LAB_004744c8) -- get_coords/
//         w_sprintf/PrintTextMessage below are NOT gated by SIM_ACTIVE, only by
//         cur_player==PlayerSide.
//       bldg_get_coords(cur_player, cur_index, &cam_pan_target_col, &cam_pan_target_row)
//         (0x004744c8-0x004744e5), then the SAME truncating fine-to-tile idiom applied to both
//         (0x004744e5-0x00474516).
//       "%s: %s" message (name=Building[building_id].id text, reason=G_TEXT_PTRS[14]) via
//         w_sprintf__vss + game_ui_PrintTextMessage (0x0047451b-0x00474558).
//   0x0047455d-0x00474570: bldg_notify_ui(cur_player, cur_index), UNCONDITIONAL tail -- fires on
//     EVERY case above (both branches, both local and non-local player).
//
#include "sim/sim_bldg_state_mine.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorders --------------------------------------------------------------------------------

struct coords_call {
    uint16_t player;
    int32_t  building_index;
};
std::vector<coords_call> g_coords_calls;
int32_t                  g_coords_out_x = 0; // the raw fine value the recorder writes into *out_x
int32_t                  g_coords_out_y = 0; // the raw fine value the recorder writes into *out_y
void                     rec_bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y) {
    g_coords_calls.push_back({player, building_index});
    *out_x = g_coords_out_x;
    *out_y = g_coords_out_y;
}

struct snd_call {
    int32_t sound_id;
    int32_t volume;
};
std::vector<snd_call> g_snd_calls;
void                  rec_snd_play(int32_t sound_id, int32_t volume) { g_snd_calls.push_back({sound_id, volume}); }

struct sprintf_call {
    void          *dst;
    const wchar_t *fmt;
    const wchar_t *a0;
    const wchar_t *a1;
};
std::vector<sprintf_call> g_sprintf_calls;
int32_t                   rec_w_sprintf__vss(void *dst, const wchar_t *fmt, const wchar_t *a0, const wchar_t *a1) {
    g_sprintf_calls.push_back({dst, fmt, a0, a1});
    return 0;
}

std::vector<void *> g_print_calls;
uint32_t            rec_game_ui_PrintTextMessage(void *text) {
    g_print_calls.push_back(text);
    return 0;
}

struct notify_call {
    uint16_t player;
    uint32_t b_index;
};
std::vector<notify_call> g_notify_calls;
void                     rec_bldg_notify_ui(uint16_t player, uint32_t b_index) {
    g_notify_calls.push_back({player, b_index});
}

const bldg_state_mine_check_deposits_calls g_calls = {
    &rec_bldg_get_coords,
    &rec_snd_play,
    &rec_w_sprintf__vss,
    &rec_game_ui_PrintTextMessage,
    &rec_bldg_notify_ui,
};

void reset_recorders() {
    g_coords_calls.clear();
    g_coords_out_x = 0;
    g_coords_out_y = 0;
    g_snd_calls.clear();
    g_sprintf_calls.clear();
    g_print_calls.clear();
    g_notify_calls.clear();
}

constexpr uint16_t PLAYER_LOCAL = 7; // == sim_fixture::reset()'s player_side default (7)
constexpr uint16_t PLAYER_OTHER = 2; // != player_side

constexpr int32_t BUILDING_SLOT = 9;      // cur_building's OWN roster index (b()'s second arg)
constexpr int32_t CUR_INDEX     = 6;      // _G_LLM_STRAT_CUR_INDEX -- deliberately DIFFERENT from
                                          // BUILDING_SLOT/SUB_ID so a confused translation is caught
constexpr int32_t SUB_ID = 3;             // building.sub_id -- selects the mines[] row, distinct from
                                          // CUR_INDEX/BUILDING_SLOT/BUILDING_TYPE_ID
constexpr int32_t BUILDING_TYPE_ID = 5;   // building.building_id -- indexes cfg_buildings[]
constexpr int32_t NAME_TEXT_ID     = 200; // cfg_buildings[BUILDING_TYPE_ID].id -- indexes text_ptrs[]
constexpr int32_t REASON_TEXT_ID   = 14;  // TEXT_ID_MINE_DEPOSITS_DEPLETED, per the header derivation

const wchar_t NAME_TEXT[]   = L"MINE_NAME_SENTINEL";
const wchar_t REASON_TEXT[] = L"MINE_REASON_SENTINEL";

// mh_map_object_mine::deposit_slot is now mh_llm_mine_deposit_slot[4] (retyped 2026-08-22, SIM1-G4)
// -- real field access replaces the former raw-byte-offset workaround.
void set_slot_extract_rate(mine &m, int32_t slot, int32_t value) {
    m.deposit_slot[slot].extract_rate = static_cast<uint32_t>(value);
}

// Common fixture wiring every case shares: cur_building points at a real roster slot, sub_id/
// building_id/cfg-text-id chain wired so the message-path asserts have real pointers to compare
// against, cur_player/cur_index/player_side set so PLAYER_LOCAL is "the local player" by default. A
// case that needs the non-local path repoints fx.view_cur_player itself afterward (same convention
// the exemplar's make_ready_unit uses for cur_unit_ptr).
building &make_check_deposits_building(sim_fixture &fx) {
    building &b   = fx.b(PLAYER_LOCAL, BUILDING_SLOT);
    b.sub_id      = static_cast<uint8_t>(SUB_ID);
    b.building_id = static_cast<uint16_t>(BUILDING_TYPE_ID);
    b.state       = 0;

    fx.cfg_buildings[BUILDING_TYPE_ID].id = NAME_TEXT_ID;
    fx.text_ptrs[NAME_TEXT_ID]            = NAME_TEXT;
    fx.text_ptrs[REASON_TEXT_ID]          = REASON_TEXT;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = PLAYER_LOCAL;
    fx.view_cur_index   = static_cast<uint16_t>(CUR_INDEX);
    fx.player_side      = static_cast<int16_t>(PLAYER_LOCAL);
    return b;
}

} // namespace

void run_bldg_state_mine_check_deposits_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- deposits remain (sum(5,-2,10,1)=14 > 0, four DISTINCT nonzero slot values, wrong
    // stride/offset would sum different bytes): state -> MINE_EXTRACTING (0x74), cycle_progress
    // zeroed regardless, NONE of the depleted-arm calls fire even though cur_player == PlayerSide
    // here, notify_ui still fires unconditionally.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b      = make_check_deposits_building(fx);
        b.cycle_progress = 42.0; // must be zeroed regardless of branch, 0x0047440d-0x0047441b
        fx.sim_active    = 1;
        mine &m          = fx.mines[PLAYER_LOCAL * MINES_PER_PLAYER + SUB_ID];
        set_slot_extract_rate(m, 0, 5);
        set_slot_extract_rate(m, 1, -2);
        set_slot_extract_rate(m, 2, 10);
        set_slot_extract_rate(m, 3, 1); // sum = 14 > 0

        sim_store own = fx.store();
        detail::bldg_state_mine_check_deposits(fx.view(), own, g_calls);

        ck_eq_d(b.cycle_progress, 0.0, "T1: cycle_progress zeroed unconditionally, 0x0047440d-0x0047441b");
        ck_eq((uint32_t)b.state, 0x74u, "T1: sum(14)>0 -> state=MINE_EXTRACTING(0x74), @0x00474461/0x00474468");
        ck_eq((uint32_t)g_snd_calls.size(), 0u, "T1: EXTRACTING arm -- snd_play never called, @0x0047446e (skips to tail)");
        ck_eq((uint32_t)g_coords_calls.size(), 0u, "T1: EXTRACTING arm -- bldg_get_coords never called");
        ck_eq((uint32_t)g_sprintf_calls.size(), 0u, "T1: EXTRACTING arm -- w_sprintf__vss never called");
        ck_eq((uint32_t)g_print_calls.size(), 0u, "T1: EXTRACTING arm -- game_ui_PrintTextMessage never called");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == PLAYER_LOCAL &&
               g_notify_calls[0].b_index == (uint32_t)CUR_INDEX,
           "T1: bldg_notify_ui(cur_player, cur_index) fires unconditionally, 0x0047455d-0x00474570");
    }

    // =================================================================================================
    // T2 -- no deposits (sum(40,-25,-10,-5)=0, four DISTINCT nonzero slot values summing to exactly
    // zero -- a wrong stride/offset would sum different bytes and very likely not land on 0): state ->
    // MINE_DEPLETED (0x75). LOCAL player (cur_player==PlayerSide), SIM_ACTIVE!=0, race=0 -> voice line
    // fires with sound_id=3 (race!=2 branch), volume=100. get_coords/fine-to-tile/message/notify all
    // fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b      = make_check_deposits_building(fx);
        b.cycle_progress = 7.0;
        fx.sim_active    = 1;
        fx.player_race   = 0;
        mine &m          = fx.mines[PLAYER_LOCAL * MINES_PER_PLAYER + SUB_ID];
        set_slot_extract_rate(m, 0, 40);
        set_slot_extract_rate(m, 1, -25);
        set_slot_extract_rate(m, 2, -10);
        set_slot_extract_rate(m, 3, -5); // sum = 0 <= 0

        g_coords_out_x = 100; // fine_to_tile(100) = 100/32 = 3
        g_coords_out_y = -65; // fine_to_tile(-65) = -65/32 = -2 (truncation TOWARD ZERO, not floor)

        sim_store own = fx.store();
        detail::bldg_state_mine_check_deposits(fx.view(), own, g_calls);

        ck_eq_d(b.cycle_progress, 0.0, "T2: cycle_progress zeroed unconditionally, 0x0047440d-0x0047441b");
        ck_eq((uint32_t)b.state, 0x75u, "T2: sum(0)<=0 -> state=MINE_DEPLETED(0x75), @0x0047445d/0x00474478");
        ck(g_snd_calls.size() == 1 && g_snd_calls[0].sound_id == 3 && g_snd_calls[0].volume == 100,
           "T2: local player, SIM_ACTIVE, race!=2 -> snd_play(3, 100), 0x004744aa-0x004744c3");
        ck(g_coords_calls.size() == 1 && g_coords_calls[0].player == PLAYER_LOCAL &&
               g_coords_calls[0].building_index == CUR_INDEX,
           "T2: bldg_get_coords(cur_player, cur_index, ...), 0x004744d2-0x004744e0");
        ck_eq((uint32_t)fx.cam_pan_target_col, 3u, "T2: cam_pan_target_col = fine_to_tile(100) = 3, 0x004744e5-0x004744fb");
        ck_eq((int32_t)fx.cam_pan_target_row, -2, "T2: cam_pan_target_row = fine_to_tile(-65) = -2 (truncate toward zero, not floor), 0x00474500-0x00474516");
        ck(g_sprintf_calls.size() == 1 && g_sprintf_calls[0].dst == fx.text_scratch.data() &&
               g_sprintf_calls[0].a0 == NAME_TEXT && g_sprintf_calls[0].a1 == REASON_TEXT,
           "T2: w_sprintf__vss(text_scratch, \"%s: %s\", Building[bid].id text, G_TEXT_PTRS[14]), 0x0047451b-0x0047454b");
        ck(g_print_calls.size() == 1 && g_print_calls[0] == (void *)fx.text_scratch.data(),
           "T2: game_ui_PrintTextMessage(text_scratch), 0x00474553-0x00474558");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == PLAYER_LOCAL &&
               g_notify_calls[0].b_index == (uint32_t)CUR_INDEX,
           "T2: bldg_notify_ui(cur_player, cur_index) fires unconditionally, 0x0047455d-0x00474570");
    }

    // =================================================================================================
    // T3 -- negative sum(-50,10,5,3)=-32, another DISTINCT-nonzero-slot set: still MINE_DEPLETED
    // (0x75) -- the gate is `sum > 0`, not `sum == 0`. LOCAL player, SIM_ACTIVE!=0, race==2 -> voice
    // line sound_id = 0x12+3 = 0x15 (the race==2 branch), pinning BOTH race values against T2's race=0.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b    = make_check_deposits_building(fx);
        fx.sim_active  = 1;
        fx.player_race = 2;
        mine &m        = fx.mines[PLAYER_LOCAL * MINES_PER_PLAYER + SUB_ID];
        set_slot_extract_rate(m, 0, -50);
        set_slot_extract_rate(m, 1, 10);
        set_slot_extract_rate(m, 2, 5);
        set_slot_extract_rate(m, 3, 3); // sum = -32 < 0

        sim_store own = fx.store();
        detail::bldg_state_mine_check_deposits(fx.view(), own, g_calls);

        ck_eq((uint32_t)b.state, 0x75u, "T3: negative sum(-32)<=0 -> state=MINE_DEPLETED(0x75) too, @0x00474461");
        ck(g_snd_calls.size() == 1 && g_snd_calls[0].sound_id == 0x15 && g_snd_calls[0].volume == 100,
           "T3: local player, SIM_ACTIVE, race==2 -> snd_play(0x12+3=0x15, 100), 0x004744a1-0x004744c3");
        ck_eq((uint32_t)g_coords_calls.size(), 1u, "T3: get_coords still fires on the negative-sum depleted arm");
        ck_eq((uint32_t)g_sprintf_calls.size(), 1u, "T3: w_sprintf__vss still fires on the negative-sum depleted arm");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == PLAYER_LOCAL,
           "T3: bldg_notify_ui fires unconditionally on the negative-sum arm too, 0x0047455d-0x00474570");
    }

    // =================================================================================================
    // T4 -- DEPLETED, local player, but SIM_ACTIVE==0: this SKIPS ONLY snd_play (the JZ at 0x00474498
    // lands directly at LAB_004744c8, which is the get_coords sequence) -- get_coords/w_sprintf/
    // PrintTextMessage are gated by cur_player==PlayerSide ONLY, NOT by SIM_ACTIVE. The branch most
    // likely to be mistranslated as "SIM_ACTIVE gates the whole local-player block".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b   = make_check_deposits_building(fx);
        fx.sim_active = 0; // the gate under test
        mine &m       = fx.mines[PLAYER_LOCAL * MINES_PER_PLAYER + SUB_ID];
        set_slot_extract_rate(m, 0, 0);
        set_slot_extract_rate(m, 1, 0);
        set_slot_extract_rate(m, 2, 0);
        set_slot_extract_rate(m, 3, 0); // sum = 0 <= 0

        sim_store own = fx.store();
        detail::bldg_state_mine_check_deposits(fx.view(), own, g_calls);

        ck_eq((uint32_t)b.state, 0x75u, "T4: sum(0)<=0 -> state=MINE_DEPLETED(0x75)");
        ck_eq((uint32_t)g_snd_calls.size(), 0u, "T4: SIM_ACTIVE==0 -> snd_play SKIPPED, JZ @0x00474498");
        ck_eq((uint32_t)g_coords_calls.size(), 1u,
              "T4: SIM_ACTIVE==0 does NOT skip bldg_get_coords -- only gated by cur_player==PlayerSide, LAB_004744c8");
        ck_eq((uint32_t)g_sprintf_calls.size(), 1u, "T4: SIM_ACTIVE==0 does NOT skip w_sprintf__vss either");
        ck_eq((uint32_t)g_print_calls.size(), 1u, "T4: SIM_ACTIVE==0 does NOT skip game_ui_PrintTextMessage either");
        ck(g_notify_calls.size() == 1, "T4: bldg_notify_ui still fires unconditionally");
    }

    // =================================================================================================
    // T5 -- DEPLETED, NON-local player (cur_player != PlayerSide): NONE of snd_play/get_coords/
    // w_sprintf/PrintTextMessage fire (the whole block is gated by cur_player==PlayerSide) -- only
    // bldg_notify_ui fires, with the NON-local (cur_player, cur_index). The branch the brief flags as
    // most likely to be mistranslated.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b        = make_check_deposits_building(fx);        // sets player_side=PLAYER_LOCAL, view_cur_player=PLAYER_LOCAL
        fx.view_cur_player = PLAYER_OTHER;                            // repoint to the NON-local player, @0x00474484
        fx.sim_active      = 1;                                       // SIM_ACTIVE on -- proves the gate is the player compare,
                                                                      // not SIM_ACTIVE, since even with it on nothing local fires
        mine &m = fx.mines[PLAYER_OTHER * MINES_PER_PLAYER + SUB_ID]; // indexed by cur_player, NOT player_side
        set_slot_extract_rate(m, 0, -1);
        set_slot_extract_rate(m, 1, -1);
        set_slot_extract_rate(m, 2, -1);
        set_slot_extract_rate(m, 3, -1); // sum = -4 <= 0

        sim_store own = fx.store();
        detail::bldg_state_mine_check_deposits(fx.view(), own, g_calls);

        ck_eq((uint32_t)b.state, 0x75u, "T5: sum(-4)<=0 -> state=MINE_DEPLETED(0x75)");
        ck_eq((uint32_t)g_snd_calls.size(), 0u, "T5: non-local player -- snd_play never called, JNZ @0x0047448b");
        ck_eq((uint32_t)g_coords_calls.size(), 0u, "T5: non-local player -- bldg_get_coords never called");
        ck_eq((uint32_t)g_sprintf_calls.size(), 0u, "T5: non-local player -- w_sprintf__vss never called");
        ck_eq((uint32_t)g_print_calls.size(), 0u, "T5: non-local player -- game_ui_PrintTextMessage never called");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == PLAYER_OTHER &&
               g_notify_calls[0].b_index == (uint32_t)CUR_INDEX,
           "T5: bldg_notify_ui(cur_player, cur_index) still fires unconditionally with the NON-local player, 0x0047455d-0x00474570");
    }
}

} // namespace mh::sim::test
