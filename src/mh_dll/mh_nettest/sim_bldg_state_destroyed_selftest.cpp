//
// sim_bldg_state_destroyed_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_destroyed
// (sim/sim_bldg_state_destroyed.h/.cpp), SIM1B building_tick machinery -- the "building destroyed"
// per-state tick handler, largest/most effectful function in this batch (22 outward calls).
//
// SCOPE (honest, not exhaustive -- see the report back to the conductor for the full branch list):
// this file covers the ORCHESTRATION SPINE (call order + args via a shared trace + per-callee
// recorders), the DIRECT STATE WRITES the function makes itself, the LOCAL-vs-OTHER-PLAYER split
// (message UI vs. mothership-loss), the MOTHER-vs-non-MOTHER type gate on both sides of that split,
// the SIM_ACTIVE-gated sound calls (explosion + race-dependent voice line), the rand_below-driven
// death-anim index, the debris-intensity x87 formula, the shuttle/production teardown gate, and the
// buildings-alive-reaches-zero presence-lost gate. It does NOT reproduce w_sprintf's actual text
// formatting (asserted as "fired", not by content -- the whole formatting path is presentation) and
// does NOT independently re-derive the tile-conversion/x87 arithmetic already hand-verified in the
// .cpp's own header banner -- this file trusts and cross-checks against that derivation rather than
// re-deriving it from raw bytes a second time.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_state_destroyed_00473194.asm), cross-checked against the .cpp/.h's own
// per-line address citations -- NOT read off the .cpp body alone:
//
//   Full call/write order (0x004731ac-0x00473698): get_coords -> fx_anim_spawn(anim[7], flag=1) ->
//   rand_below(4) -> fx_anim_spawn(death_anim_table[trace*4+roll], flag=0) -> apply_area_damage ->
//   [SIM_ACTIVE: snd_play_at(explosion, tile) -- the original offscreen_snd_volume+snd_play pair,
//    one record since the LIFT-NOTIFY offscreen conversion] -> LOCAL-PLAYER SPLIT:
//     local:  [SIM_ACTIVE: snd_play(voice, race-dependent id)] -> mother-type gate:
//               mother:     ui_print_queue_text_id(4), game_sp_outcome_announce
//               non-mother: w_sprintf__vss, game_ui_PrintTextMessage
//             -> cam_pan_target_col/row = tile(fine_x/y)                        [UNCONDITIONAL in this arm]
//     other:  mother-type gate (roster-derived building_id, a DIFFERENT expression than every other
//             building_id read in this function -- see the .h banner):
//               mother AND mothership_alive!=0: planet_mother_lost_time[planet]=game_clock,
//                                                invasion_chance_roll(0)
//               else: nothing (mothership_alive is not even CALLED for a non-mother)
//   -> fx_debris_burst(tile, cfg.energy) -- the original offscreen_fx_scale ->
//      trunc(fx_scale*cfg.energy/2000.0) -> spawn_debris_burst chain, one record whose
//      camera-dependent derivation lives in the hosted sink since the LIFT-NOTIFY offscreen conversion ->
//   [shuttle_slot!=0: [matches profile.prod_queue_slot[planet]: prod_unbind_planet] ->
//    prod_shuttle_slot_release] -> bldg_unmap_footprint -> cycle_progress=0.0 ->
//   buildings_lost_total[planet]+=1, buildings_alive[planet]-=1, set_event(14) ->
//   [buildings_alive[planet]==0: unit_purge_unregistered, player_presence_lost(player,0)] ->
//   cycle_progress=0.0 (2nd, real) -> state=RUBBLE_SIGHT_DECAY(4) -> anim[0]=cfg.sight ->
//   sight_add_circle(player, b.x, b.y, building_id, sight) -> tick_budget=0.0 -> bldg_notify_ui.
//
#include "sim/sim_bldg_state_destroyed.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER (0x06) / BUILDING_TYPE_H_MOTHER (0x1a)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 22 callees ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs (settable per case BEFORE seed_and_run) --------------------------------
int32_t g_get_coords_fine_x    = 0;
int32_t g_get_coords_fine_y    = 0;
int32_t g_rand_below_result    = 0;
int32_t g_mothership_alive_ret = 0;

// ---- per-callee recorders (22, one per bldg_state_destroyed_calls member) ----------------------
struct CoordsCall {
    uint16_t player;
    int32_t  index;
};
std::vector<CoordsCall> g_coords;
void                    rec_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("get_coords");
    g_coords.push_back({player, index});
    *out_x = g_get_coords_fine_x;
    *out_y = g_get_coords_fine_y;
}

struct AnimSpawnCall {
    uint32_t x, y, frame;
    double   elapsed;
    uint32_t flag;
};
std::vector<AnimSpawnCall> g_anim_spawns;
uint32_t                   rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t frame, double elapsed, uint32_t flag) {
    tr("fx_anim_spawn");
    g_anim_spawns.push_back({x, y, frame, elapsed, flag});
    return 0;
}

std::vector<int32_t> g_rand_calls; // upper_bound arg
int32_t              rec_rand_below(int32_t upper_bound) {
    tr("rand_below");
    g_rand_calls.push_back(upper_bound);
    return g_rand_below_result;
}

struct AreaDamageCall {
    int32_t  x, y, kind;
    double   dmg;
    int32_t  ring;
    uint32_t owner;
    uint32_t killer_info;
    int32_t  killer_idx;
};
std::vector<AreaDamageCall> g_area_damage;
void                        rec_apply_area_damage(int32_t x, int32_t y, int32_t kind, double dmg, int32_t ring, uint32_t owner,
                                                  uint32_t killer_info, int32_t killer_idx) {
    tr("apply_area_damage");
    g_area_damage.push_back({x, y, kind, dmg, ring, owner, killer_info, killer_idx});
}

struct SndAtCall {
    int32_t id, col, row;
};
std::vector<SndAtCall> g_snd_play_at;
void                   rec_snd_play_at(int32_t id, int32_t tile_col, int32_t tile_row) {
    tr("snd_play_at");
    g_snd_play_at.push_back({id, tile_col, tile_row});
}

struct SndCall {
    int32_t id, volume;
};
std::vector<SndCall> g_snd_play;
void                 rec_snd_play(int32_t id, int32_t volume) {
    tr("snd_play");
    g_snd_play.push_back({id, volume});
}

std::vector<int32_t> g_print_queue_text_ids;
void                 rec_ui_print_queue_text_id(int32_t id) {
    tr("ui_print_queue_text_id");
    g_print_queue_text_ids.push_back(id);
}

int  g_outcome_announce_count = 0;
void rec_outcome_announce() {
    tr("outcome_announce");
    ++g_outcome_announce_count;
}

int     g_sprintf_count = 0;
int32_t rec_sprintf(void *, const wchar_t *, const wchar_t *, const wchar_t *) {
    tr("sprintf");
    ++g_sprintf_count;
    return 0;
}

int      g_print_text_message_count = 0;
uint32_t rec_print_text_message(void *) {
    tr("print_text_message");
    ++g_print_text_message_count;
    return 0;
}

int     g_mothership_alive_calls = 0;
int32_t rec_mothership_alive() {
    tr("mothership_alive");
    ++g_mothership_alive_calls;
    return g_mothership_alive_ret;
}

std::vector<int32_t> g_invasion_roll_calls; // building_completed arg
int32_t              rec_invasion_chance_roll(int32_t completed) {
    tr("invasion_chance_roll");
    g_invasion_roll_calls.push_back(completed);
    return 0;
}

struct DebrisCall {
    int32_t col, row, energy;
};
std::vector<DebrisCall> g_debris_calls;
void                    rec_fx_debris_burst(int32_t tile_col, int32_t tile_row, int32_t energy_max) {
    tr("fx_debris_burst");
    g_debris_calls.push_back({tile_col, tile_row, energy_max});
}

struct PP {
    int32_t player, v;
};
std::vector<PP> g_prod_unbind_calls;
void            rec_prod_unbind_planet(int32_t player, int32_t planet) {
    tr("prod_unbind_planet");
    g_prod_unbind_calls.push_back({player, planet});
}
std::vector<PP> g_prod_shuttle_release_calls; // .v holds the slot
void            rec_prod_shuttle_slot_release(int32_t player, int32_t slot) {
    tr("prod_shuttle_slot_release");
    g_prod_shuttle_release_calls.push_back({player, slot});
}

struct UnmapCall {
    uint16_t player;
    int32_t  index;
};
std::vector<UnmapCall> g_unmap_calls;
void                   rec_bldg_unmap_footprint(uint16_t player, int32_t index) {
    tr("bldg_unmap_footprint");
    g_unmap_calls.push_back({player, index});
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_set_event(uint32_t type) {
    tr("set_event");
    g_set_event_calls.push_back(type);
    return 0;
}

std::vector<uint32_t> g_purge_calls; // player arg
void                  rec_unit_purge_unregistered(uint32_t player) {
    tr("unit_purge_unregistered");
    g_purge_calls.push_back(player);
}

struct PresenceCall {
    uint32_t player, mode;
};
std::vector<PresenceCall> g_presence_calls;
uint32_t                  rec_player_presence_lost(uint32_t player, uint32_t mode) {
    tr("player_presence_lost");
    g_presence_calls.push_back({player, mode});
    return 0;
}

struct SightCall {
    uint32_t player;
    int32_t  x, y, bid;
    uint8_t  sight;
};
std::vector<SightCall> g_sight_calls;
uint32_t               rec_sight_add_circle(uint32_t player, int32_t x, int32_t y, int32_t bid,
                                            uint8_t sight) {
    tr("sight_add_circle");
    g_sight_calls.push_back({player, x, y, bid, sight});
    return 0;
}

struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_calls.push_back({player, index});
}

const bldg_state_destroyed_calls g_calls = {
    &rec_get_coords,
    &rec_fx_anim_spawn,
    &rec_rand_below,
    &rec_apply_area_damage,
    &rec_snd_play_at,
    &rec_snd_play,
    &rec_ui_print_queue_text_id,
    &rec_outcome_announce,
    &rec_sprintf,
    &rec_print_text_message,
    &rec_mothership_alive,
    &rec_invasion_chance_roll,
    &rec_fx_debris_burst,
    &rec_prod_unbind_planet,
    &rec_prod_shuttle_slot_release,
    &rec_bldg_unmap_footprint,
    &rec_set_event,
    &rec_unit_purge_unregistered,
    &rec_player_presence_lost,
    &rec_sight_add_circle,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_coords.clear();
    g_anim_spawns.clear();
    g_rand_calls.clear();
    g_area_damage.clear();
    g_snd_play_at.clear();
    g_snd_play.clear();
    g_print_queue_text_ids.clear();
    g_outcome_announce_count   = 0;
    g_sprintf_count            = 0;
    g_print_text_message_count = 0;
    g_mothership_alive_calls   = 0;
    g_invasion_roll_calls.clear();
    g_debris_calls.clear();
    g_prod_unbind_calls.clear();
    g_prod_shuttle_release_calls.clear();
    g_unmap_calls.clear();
    g_set_event_calls.clear();
    g_purge_calls.clear();
    g_presence_calls.clear();
    g_sight_calls.clear();
    g_notify_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player       = 0;
    int32_t  index        = 1;
    uint16_t cfg_row      = 10;
    uint8_t  type         = 1; // non-mother by default
    int32_t  sound_explo  = 777;
    int32_t  anim7_frame  = 0x3001; // primary fx_anim_spawn frame, distinct sentinel
    int32_t  trace_row    = 2;
    double   cfg_energy   = 20.0;
    uint8_t  sight        = 6;
    uint8_t  shuttle_slot = 0;
    uint8_t  bx = 11, by = 13; // building's OWN tile position (distinct from the fine coords below)

    int32_t buildings_alive_seed = 5;
    int32_t buildings_lost_seed  = 3;
    int32_t prod_queue_slot_seed = -1; // mismatches any uint8_t shuttle_slot by default

    int32_t planet      = 3;
    int16_t player_side = 0; // == player -> "local"; != player -> "other"
    int32_t player_race = 0;
    int32_t sim_active  = 0;

    int32_t mothership_alive_result = 0;
    int32_t rand_below_result       = 0;

    double game_clock  = 100.0;
    double tick_budget = 10.0;

    int32_t fine_x = 1600; // -> tile 50 (1600 / 32, exact)
    int32_t fine_y = 1760; // -> tile 55 (1760 / 32, exact)

    int32_t cam_pan_col_seed = -777;
    int32_t cam_pan_row_seed = -888;

    double planet_mother_lost_time_seed = -1.0;

    int32_t death_anim_entry = 0x5A5A; // death_anim_table[trace_row*4 + rand_below_result]
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.shuttle_slot   = s.shuttle_slot;
    b.cycle_progress = 42.5; // nonzero sentinel -- the function must zero it
    b.x              = s.bx;
    b.y              = s.by;
    b.state          = (uint16_t)0xBEEF; // sentinel, distinct from RUBBLE_SIGHT_DECAY(4)
    std::memset(b.anim, 0, sizeof(b.anim));

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.type          = s.type;
    cb.sound_explo   = s.sound_explo;
    std::memcpy(&cb.anim[7 * 4], &s.anim7_frame, sizeof(int32_t)); // anim[7] = cfg_t_frame_index slot 7
    cb.trace  = s.trace_row;
    cb.energy = s.cfg_energy;
    cb.sight  = s.sight;

    fx.profiles[s.player].buildings_alive[s.planet]      = s.buildings_alive_seed;
    fx.profiles[s.player].buildings_lost_total[s.planet] = s.buildings_lost_seed;
    fx.profiles[s.player].prod_queue_slot[s.planet]      = s.prod_queue_slot_seed;

    fx.planet_index = s.planet;
    fx.player_side  = s.player_side;
    fx.player_race  = s.player_race;
    fx.sim_active   = s.sim_active;
    fx.game_clock   = s.game_clock;
    fx.tick_budget  = s.tick_budget;

    fx.cam_pan_target_col                                      = s.cam_pan_col_seed;
    fx.cam_pan_target_row                                      = s.cam_pan_row_seed;
    fx.planet_mother_lost_time[s.planet]                       = s.planet_mother_lost_time_seed;
    fx.death_anim_table[s.trace_row * 4 + s.rand_below_result] = s.death_anim_entry;

    g_get_coords_fine_x    = s.fine_x;
    g_get_coords_fine_y    = s.fine_y;
    g_rand_below_result    = s.rand_below_result;
    g_mothership_alive_ret = s.mothership_alive_result;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_destroyed(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_destroyed_tests() {
    sim_fixture fx;

    // =================================================================================================
    // D1 -- THE SPINE: non-mother, LOCAL player, SIM_ACTIVE=0, no shuttle, buildings_alive stays > 0.
    // Exercises the full order-of-calls minus the sim_active-gated sounds and the mother/teardown/
    // presence-lost extras (isolated in later cases), plus every direct state write.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        seed_and_run(fx, s);

        ck(trace_eq({"get_coords", "fx_anim_spawn", "rand_below", "fx_anim_spawn", "apply_area_damage",
                     "sprintf", "print_text_message", "fx_debris_burst",
                     "bldg_unmap_footprint", "set_event", "sight_add_circle", "notify_ui"}),
           "D1: exact call order for the local/non-mother/sim-inactive/no-shuttle/no-teardown spine");

        ck(g_coords.size() == 1 && g_coords[0].player == 0 && g_coords[0].index == 1,
           "D1: get_coords(cur_player, cur_index)");

        ck(g_anim_spawns.size() == 2, "D1: exactly 2 fx_anim_spawn calls");
        if (g_anim_spawns.size() == 2) {
            const auto &a0 = g_anim_spawns[0];
            ck(a0.x == (uint32_t)s.fine_x && a0.y == (uint32_t)s.fine_y &&
                   a0.frame == (uint32_t)s.anim7_frame && a0.flag == 1,
               "D1: fx_anim_spawn #1 -- Building.anim[7], flag=1 (PRIMARY)");
            ck_eq_d(a0.elapsed, s.game_clock - s.tick_budget, "D1: fx_anim_spawn #1 elapsed = game_clock-tick_budget");
            const auto &a1 = g_anim_spawns[1];
            ck(a1.x == (uint32_t)s.fine_x && a1.y == (uint32_t)s.fine_y && a1.flag == 0,
               "D1: fx_anim_spawn #2 -- same coords, flag=0 (SECONDARY)");
            ck_eq(a1.frame, (uint32_t)s.death_anim_entry,
                  "D1: fx_anim_spawn #2 frame = death_anim_table[trace*4 + rand_below(4)]");
        }
        ck(g_rand_calls.size() == 1 && g_rand_calls[0] == 4, "D1: rand_below(4)");

        ck(g_area_damage.size() == 1, "D1: apply_area_damage called once");
        if (g_area_damage.size() == 1) {
            const auto &d = g_area_damage[0];
            ck(d.x == 50 && d.y == 55 && d.kind == DEATH_BLAST_TARGET_KIND && d.ring == DEATH_BLAST_RING_COUNT &&
                   d.owner == 0 && d.killer_info == 0 && d.killer_idx == 0,
               "D1: apply_area_damage(tile 50,55, kind=2, ring=2, owner=0, killer_info=0, killer_idx=0)");
            ck_eq_d(d.dmg, DEATH_BLAST_DAMAGE, "D1: apply_area_damage damage = 10.0");
        }

        ck(g_snd_play_at.empty() && g_snd_play.empty(), "D1: SIM_ACTIVE=0 -- no explosion/voice sound");
        ck(g_sprintf_count == 1 && g_print_text_message_count == 1,
           "D1: non-mother local outcome -- w_sprintf + PrintTextMessage fire once each");
        ck(g_print_queue_text_ids.empty() && g_outcome_announce_count == 0,
           "D1: non-mother local outcome -- mothership-destroyed UI does NOT fire");
        ck(g_mothership_alive_calls == 0 && g_invasion_roll_calls.empty(),
           "D1: local-player branch never calls mothership-alive/invasion-roll");

        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "D1: cam_pan_target_col = tile(fine_x)");
        ck_eq((uint32_t)fx.cam_pan_target_row, 55u, "D1: cam_pan_target_row = tile(fine_y)");

        ck(g_debris_calls.size() == 1 && g_debris_calls[0].col == 50 && g_debris_calls[0].row == 55 &&
               g_debris_calls[0].energy == 20,
           "D1: fx_debris_burst(tile 50,55, cfg.energy=20) -- the fx_scale->intensity derivation "
           "lives in the hosted sink now, so the TU's record carries the raw inputs");

        ck(g_prod_unbind_calls.empty() && g_prod_shuttle_release_calls.empty(),
           "D1: shuttle_slot==0 -- no production teardown calls");
        ck(g_unmap_calls.size() == 1 && g_unmap_calls[0].player == 0 && g_unmap_calls[0].index == 1,
           "D1: bldg_unmap_footprint(player, index)");

        ck_eq_d(fx.b(0, 1).cycle_progress, 0.0, "D1: cycle_progress zeroed");
        ck_eq((uint32_t)fx.profiles[0].buildings_lost_total[s.planet], (uint32_t)(s.buildings_lost_seed + 1),
              "D1: buildings_lost_total[planet] += 1");
        ck_eq((uint32_t)fx.profiles[0].buildings_alive[s.planet], (uint32_t)(s.buildings_alive_seed - 1),
              "D1: buildings_alive[planet] -= 1");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 14,
           "D1: set_event(14) -- MAP_OBJECTS_REFRESH");
        ck(g_purge_calls.empty() && g_presence_calls.empty(),
           "D1: buildings_alive stayed nonzero (5-1=4) -- no purge/presence-lost");

        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)BLDG_STATE_RUBBLE_SIGHT_DECAY, "D1: state = RUBBLE_SIGHT_DECAY(4)");
        {
            int32_t anim0 = -1;
            std::memcpy(&anim0, &fx.b(0, 1).anim[0], sizeof(anim0));
            ck_eq((uint32_t)anim0, (uint32_t)s.sight, "D1: anim[0] = cfg.sight (zero-extended)");
        }
        ck(g_sight_calls.size() == 1, "D1: sight_add_circle called once");
        if (g_sight_calls.size() == 1) {
            const auto &sc = g_sight_calls[0];
            ck(sc.player == 0 && sc.x == s.bx && sc.y == s.by && sc.bid == s.cfg_row && sc.sight == s.sight,
               "D1: sight_add_circle(player, b.x, b.y, building_id, sight)");
        }
        ck_eq_d(fx.tick_budget, 0.0, "D1: tick_budget zeroed (shared with the unit-tick driver)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 0 && g_notify_calls[0].index == 1,
           "D1: bldg_notify_ui(player, index)");
    }

    // =================================================================================================
    // D2 -- LOCAL player, MOTHER type: the ui_print_queue_text_id/outcome_announce arm fires INSTEAD of
    // w_sprintf/PrintTextMessage; cam_pan_target is still written (unconditional in the local arm).
    // =================================================================================================
    {
        Seed s;
        s.player      = 1;
        s.index       = 2;
        s.player_side = 1; // == player -> LOCAL branch (default player_side=0 would NOT match player=1)
        s.type        = BUILDING_TYPE_A_MOTHER;
        seed_and_run(fx, s);

        ck(g_print_queue_text_ids.size() == 1 && g_print_queue_text_ids[0] == TEXT_ID_MOTHERSHIP_DESTROYED,
           "D2: mother local outcome -- ui_print_queue_text_id(4)");
        ck(g_outcome_announce_count == 1, "D2: mother local outcome -- game_sp_outcome_announce fires");
        ck(g_sprintf_count == 0 && g_print_text_message_count == 0,
           "D2: mother local outcome -- generic w_sprintf/PrintTextMessage do NOT fire");
        ck_eq((uint32_t)fx.cam_pan_target_col, 50u, "D2: cam_pan still written on the mother arm (unconditional)");
        ck(g_mothership_alive_calls == 0, "D2: local branch never calls mothership-alive even for a mother");

        // Same gate for H_MOTHER (0x1a), the other of the two mother type ids.
        s.type = BUILDING_TYPE_H_MOTHER;
        seed_and_run(fx, s);
        ck(g_print_queue_text_ids.size() == 1 && g_outcome_announce_count == 1,
           "D2b: H_MOTHER (0x1a) takes the same mother-outcome arm as A_MOTHER (0x06)");
    }

    // =================================================================================================
    // D3 -- SIM_ACTIVE=1 sound gate: explosion (unconditional on sim_active, before the player split)
    // THEN the local-only race-dependent voice line, in that order. race==2 adds the 0x12 offset.
    // =================================================================================================
    {
        Seed s;
        s.player      = 0;
        s.index       = 1;
        s.sim_active  = 1;
        s.player_race = 2;
        seed_and_run(fx, s);

        ck(g_snd_play_at.size() == 1 && g_snd_play_at[0].id == s.sound_explo &&
               g_snd_play_at[0].col == 50 && g_snd_play_at[0].row == 55,
           "D3: snd_play_at(cfg.sound_explo, tile 50,55) fires once (explosion, the fused pair)");
        ck(g_snd_play.size() == 1, "D3: exactly 1 fixed-volume snd_play call (the voice line)");
        if (g_snd_play.size() == 1) {
            ck(g_snd_play[0].id == VOICE_LINE_RACE2_OFFSET + VOICE_LINE_BASE_SOUND_ID &&
                   g_snd_play[0].volume == VOICE_LINE_VOLUME,
               "D3: snd_play = voice line, race==2 -> id = 0x12+5 = 0x17, volume 100");
        }

        // race != 2 -> no offset.
        s.player_race = 0;
        seed_and_run(fx, s);
        ck(g_snd_play.size() == 1 && g_snd_play[0].id == VOICE_LINE_BASE_SOUND_ID,
           "D3b: race!=2 -> voice line id = base (5), no 0x12 offset");

        // SIM_ACTIVE=0 with the same otherwise-identical fixture -- neither sound fires.
        s.sim_active = 0;
        seed_and_run(fx, s);
        ck(g_snd_play_at.empty() && g_snd_play.empty(),
           "D3c: SIM_ACTIVE=0 -- neither explosion nor voice line fires");
    }

    // =================================================================================================
    // D4 -- OTHER-player branch, MOTHER type, mothership_alive returns NONZERO: the loss-bookkeeping
    // arm fires (planet_mother_lost_time write + invasion_chance_roll(0)); cam_pan_target is NOT
    // touched (that write lives only in the local-player arm).
    // =================================================================================================
    {
        Seed s;
        s.player                  = 0;
        s.index                   = 1;
        s.player_side             = 5; // != player -> "other"
        s.type                    = BUILDING_TYPE_A_MOTHER;
        s.mothership_alive_result = 1;
        seed_and_run(fx, s);

        ck(g_mothership_alive_calls == 1, "D4: game_check_players_mothership_alive called once");
        ck_eq_d(fx.planet_mother_lost_time[s.planet], s.game_clock,
                "D4: planet_mother_lost_time[planet] = game_clock");
        ck(g_invasion_roll_calls.size() == 1 && g_invasion_roll_calls[0] == 0,
           "D4: invasion_chance_roll(0) fires");
        ck_eq((uint32_t)fx.cam_pan_target_col, (uint32_t)s.cam_pan_col_seed,
              "D4: cam_pan_target_col UNCHANGED -- only the local-player arm writes it");
        ck_eq((uint32_t)fx.cam_pan_target_row, (uint32_t)s.cam_pan_row_seed, "D4: cam_pan_target_row UNCHANGED");
        ck(g_print_queue_text_ids.empty() && g_outcome_announce_count == 0 && g_sprintf_count == 0 &&
               g_print_text_message_count == 0,
           "D4: none of the local-only message calls fire on the other-player branch");
    }

    // =================================================================================================
    // D5 -- OTHER-player, MOTHER type, mothership_alive returns ZERO: check IS made, but neither the
    // lost-time write nor invasion_chance_roll fires.
    // =================================================================================================
    {
        Seed s;
        s.player                  = 0;
        s.index                   = 1;
        s.player_side             = 5;
        s.type                    = BUILDING_TYPE_H_MOTHER;
        s.mothership_alive_result = 0;
        seed_and_run(fx, s);

        ck(g_mothership_alive_calls == 1, "D5: mothership-alive check still fires for a mother");
        ck_eq_d(fx.planet_mother_lost_time[s.planet], s.planet_mother_lost_time_seed,
                "D5: planet_mother_lost_time UNCHANGED (mothership_alive returned 0)");
        ck(g_invasion_roll_calls.empty(), "D5: invasion_chance_roll does NOT fire");
    }

    // =================================================================================================
    // D6 -- OTHER-player, NON-mother type: the mothership-alive check is not even CALLED (the type gate
    // fails before that call site is reached), so neither side effect can fire.
    // =================================================================================================
    {
        Seed s;
        s.player      = 0;
        s.index       = 1;
        s.player_side = 5;
        s.type        = 1; // not a mother type
        seed_and_run(fx, s);

        ck(g_mothership_alive_calls == 0, "D6: non-mother other-player -- mothership-alive NEVER called");
        ck(g_invasion_roll_calls.empty(), "D6: invasion_chance_roll does not fire");
        ck_eq_d(fx.planet_mother_lost_time[s.planet], s.planet_mother_lost_time_seed,
                "D6: planet_mother_lost_time UNCHANGED");
    }

    // =================================================================================================
    // D7 -- shuttle/production teardown gate (three sub-cases, same fixture shape otherwise).
    // =================================================================================================
    {
        // D7a: shuttle_slot == 0 -- neither call fires.
        Seed s;
        s.player       = 0;
        s.index        = 1;
        s.shuttle_slot = 0;
        seed_and_run(fx, s);
        ck(g_prod_unbind_calls.empty() && g_prod_shuttle_release_calls.empty(),
           "D7a: shuttle_slot==0 -- no teardown calls at all");

        // D7b: shuttle_slot matches profile.prod_queue_slot[planet] -- BOTH fire, unbind first.
        s.shuttle_slot         = 5;
        s.prod_queue_slot_seed = 5;
        seed_and_run(fx, s);
        ck(trace_eq({"get_coords", "fx_anim_spawn", "rand_below", "fx_anim_spawn", "apply_area_damage",
                     "sprintf", "print_text_message", "fx_debris_burst",
                     "prod_unbind_planet", "prod_shuttle_slot_release", "bldg_unmap_footprint", "set_event",
                     "sight_add_circle", "notify_ui"}),
           "D7b: matched slot -- prod_unbind_planet fires BEFORE prod_shuttle_slot_release, in the spine");
        ck(g_prod_unbind_calls.size() == 1 && g_prod_unbind_calls[0].player == 0 &&
               g_prod_unbind_calls[0].v == s.planet,
           "D7b: prod_unbind_planet(player, planet)");
        ck(g_prod_shuttle_release_calls.size() == 1 && g_prod_shuttle_release_calls[0].player == 0 &&
               g_prod_shuttle_release_calls[0].v == 5,
           "D7b: prod_shuttle_slot_release(player, shuttle_slot)");

        // D7c: shuttle_slot set but MISMATCHED against prod_queue_slot[planet] -- only release fires.
        s.shuttle_slot         = 5;
        s.prod_queue_slot_seed = 9; // mismatch
        seed_and_run(fx, s);
        ck(g_prod_unbind_calls.empty(), "D7c: mismatched slot -- prod_unbind_planet does NOT fire");
        ck(g_prod_shuttle_release_calls.size() == 1 && g_prod_shuttle_release_calls[0].v == 5,
           "D7c: prod_shuttle_slot_release STILL fires regardless of the match");
    }

    // =================================================================================================
    // D8 -- buildings_alive[planet] reaching exactly ZERO after the decrement: unit_purge_unregistered
    // then player_presence_lost(player, 0) fire, both AFTER set_event in the trace.
    // =================================================================================================
    {
        Seed s;
        s.player               = 0;
        s.index                = 1;
        s.buildings_alive_seed = 1; // -> 0 after the decrement
        seed_and_run(fx, s);

        ck(trace_eq({"get_coords", "fx_anim_spawn", "rand_below", "fx_anim_spawn", "apply_area_damage",
                     "sprintf", "print_text_message", "fx_debris_burst",
                     "bldg_unmap_footprint", "set_event", "unit_purge_unregistered", "player_presence_lost",
                     "sight_add_circle", "notify_ui"}),
           "D8: buildings_alive hits 0 -- purge+presence-lost fire, ordered right after set_event");
        ck_eq((uint32_t)fx.profiles[0].buildings_alive[s.planet], 0u, "D8: buildings_alive[planet] == 0");
        ck(g_purge_calls.size() == 1 && g_purge_calls[0] == 0, "D8: unit_purge_unregistered(player)");
        ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == 0 && g_presence_calls[0].mode == 0,
           "D8: player_presence_lost(player, mode=0)");
    }

    // =================================================================================================
    // D9 -- the fx_debris_burst record's energy payload truncates cfg.energy (a double) to int32.
    // The intensity formula itself (fx_scale * energy / 2000.0, x87-truncated) moved into the hosted
    // sink with the LIFT-NOTIFY offscreen conversion -- what the TU owns now is passing the RAW energy through, and a
    // fractional seed pins the cast (77.9 -> 77, not 78).
    // =================================================================================================
    {
        Seed s;
        s.player     = 0;
        s.index      = 1;
        s.cfg_energy = 77.9;
        seed_and_run(fx, s);
        ck(g_debris_calls.size() == 1 && g_debris_calls[0].energy == 77,
           "D9: fx_debris_burst energy = (int32)cfg.energy -- 77.9 truncates to 77");
    }
}

} // namespace mh::sim::test
