//
// sim_unit_state_die_explode_selftest.cpp -- `simtest` cases for llm_strat_unit_state_die_explode
// @0x004854f3 (sim/sim_unit_state_die_explode.h/.cpp). PERMANENT DO-NOT-ARM per the translator brief
// (every path makes confirmed effectful/UI outward calls -- llm_snd_play, llm_strat_fx_anim_spawn x3,
// llm_strat_unit_on_destroyed, llm_game_sp_outcome_announce, game_SetEvent(MAP_OBJECTS_REFRESH),
// llm_strat_unit_notify_ui -- a per-call shadow arm cannot un-play a sound or un-spawn an animation,
// so arming would double-fire all of that for real while the state comparison still reads green).
// This file is the ONLY execution evidence this function will ever get.
//
// SCOPE (honest, not exhaustive): the SIGNED small-vs-mothership dispatch (CMP+JG @0x0048559d/
// 0x004855a4, incl. the high-bit-set case that separates the fixed signed cast from the unsigned
// naive translation it replaced), the SIM_ACTIVE-gated death sound (both sides), the fine_x/fine_y
// tile-conversion round-toward-zero divide (incl. a negative-coordinate boundary), the small-arm
// explosion + optional debris-burst FX (full argument tuples, rand_below draw ORDER, the
// debris_anim_row*4+roll index formula over two distinct rows, the torus-wrap AND masks), the
// big-arm explosion FX (fine_y REPLACED by unit_calc_render_fine_y, fine_x untouched) +
// unit_on_destroyed + the local-player/mother-type outcome-announce gate (all four combinations),
// the common-tail target_ref/target2_ref release pair (independent ifs, order, field clears), the
// path_slot_id gate, the per-planet units_lost/units_alive counters + presence-lost gate, the
// state-commit + move_microstep-as-saved-sight overload + FoW refresh + notify-ui tail, full
// CALL/WRITE ORDER via one shared trace, and neighbouring-slot non-corruption. It does NOT
// independently re-derive the SAR/SHL/SBB/SAR signed-divide-by-32 idiom's bit-level equivalence to
// C++ `/32` a second time (the .cpp's own header banner already cites this as the same idiom
// sim_projectile_tick.cpp documents) -- it DOES, however, add a negative-input case to pin the
// round-toward-zero behaviour that a `>>5` "simplification" would silently break.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_die_explode_004854f3.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_die_explode.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 18 callees ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value / behaviour knobs (settable per case BEFORE seed_and_run) --------------------
int32_t              g_get_coords_fine_x = 0;
int32_t              g_get_coords_fine_y = 0;
std::vector<int32_t> g_rand_below_queue; // popped FIFO; 0 once exhausted
uint32_t             g_calc_render_fine_y_ret = 0;
// The unit_proto_id staleness pin (T10): when non-null/non-negative, rec_unit_on_destroyed mutates
// *g_mutate_unit_ptr's unit_proto_id to g_mutate_proto_to -- something the REAL llm_strat_unit_on_
// destroyed does not do (checked separately and refuted), used here only to prove empirically which
// value the LATER reads (the outcome-announce type gate @0x00485705-0x00485767, the
// unit_housing_count_remove proto-id arg @0x004857f8-0x00485801) actually consume.
unit   *g_mutate_unit_ptr = nullptr;
int32_t g_mutate_proto_to = -1;

// ---- per-callee recorders (18, one per unit_state_die_explode_calls member) --------------------
struct CoordsCall {
    uint16_t player;
    int32_t  index;
};
std::vector<CoordsCall> g_coords;
void                    rec_unit_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    tr("unit_get_coords");
    g_coords.push_back({player, index});
    *out_x = g_get_coords_fine_x;
    *out_y = g_get_coords_fine_y;
}

struct SndAtCall {
    int32_t id, col, row;
};
std::vector<SndAtCall> g_snd_play_at;
void                   rec_snd_play_at(int32_t id, int32_t tile_col, int32_t tile_row) {
    tr("snd_play_at");
    g_snd_play_at.push_back({id, tile_col, tile_row});
}

struct AnimSpawnCall {
    uint32_t x, y, anim_id;
    double   elapsed;
    uint32_t owner_or_flag;
};
std::vector<AnimSpawnCall> g_anim_spawns;
uint32_t                   rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t anim_id, double elapsed, uint32_t owner_or_flag) {
    tr("fx_anim_spawn");
    g_anim_spawns.push_back({x, y, anim_id, elapsed, owner_or_flag});
    return 0;
}

std::vector<int32_t> g_rand_calls; // upper_bound arg, in call order
int32_t              rec_rand_below(int32_t upper_bound) {
    tr("rand_below");
    g_rand_calls.push_back(upper_bound);
    int32_t v = 0;
    if (!g_rand_below_queue.empty()) {
        v = g_rand_below_queue.front();
        g_rand_below_queue.erase(g_rand_below_queue.begin());
    }
    return v;
}

struct RemoveCall {
    uint16_t player;
    uint32_t idx;
};
std::vector<RemoveCall> g_remove_calls;
void                    rec_unit_remove_from_map(uint16_t player, uint32_t unit_idx) {
    tr("unit_remove_from_map");
    g_remove_calls.push_back({player, unit_idx});
}

struct CalcRenderCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<CalcRenderCall> g_calc_render_calls;
uint32_t                    rec_unit_calc_render_fine_y(uint16_t player, int32_t unit_idx) {
    tr("unit_calc_render_fine_y");
    g_calc_render_calls.push_back({player, unit_idx});
    return g_calc_render_fine_y_ret;
}

struct OnDestroyedCall {
    uint16_t player;
    uint32_t idx;
};
std::vector<OnDestroyedCall> g_on_destroyed_calls;
void                         rec_unit_on_destroyed(uint16_t player, uint32_t unit_idx) {
    tr("unit_on_destroyed");
    g_on_destroyed_calls.push_back({player, unit_idx});
    if (g_mutate_unit_ptr != nullptr && g_mutate_proto_to >= 0)
        g_mutate_unit_ptr->unit_proto_id = static_cast<uint16_t>(g_mutate_proto_to);
}

int  g_outcome_announce_count = 0;
void rec_game_sp_outcome_announce() {
    tr("game_sp_outcome_announce");
    ++g_outcome_announce_count;
}

struct TargetReleaseCall {
    uint32_t player;
    int32_t  idx;
    uint32_t mode;
};
std::vector<TargetReleaseCall> g_target_release_calls;
void                           rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_target_release_calls.push_back({player_idx, unit_idx, mode});
}

struct HousingCall {
    int32_t player, proto_id;
};
std::vector<HousingCall> g_housing_calls;
void                     rec_unit_housing_count_remove(int32_t player, int32_t unit_proto_id) {
    tr("unit_housing_count_remove");
    g_housing_calls.push_back({player, unit_proto_id});
}

struct DoorCall {
    int32_t player, idx;
};
std::vector<DoorCall> g_door_calls;
int32_t               rec_storage_release_door_held_by_unit(int32_t player, int32_t unit_idx) {
    tr("storage_release_door_held_by_unit");
    g_door_calls.push_back({player, unit_idx});
    return 0;
}

struct PathFreeCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<PathFreeCall> g_path_free_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_path_free_calls.push_back({player, unit_index});
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

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct FowCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<FowCall> g_fow_calls;
void                 rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_calls.push_back({player, x, y, sight});
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_game_SetEvent(uint32_t type) {
    tr("game_SetEvent");
    g_set_event_calls.push_back(type);
    return 0;
}

struct NotifyCall {
    uint32_t side, idx;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_unit_notify_ui(uint32_t side, uint32_t unit_index) {
    tr("unit_notify_ui");
    g_notify_calls.push_back({side, unit_index});
}

const unit_state_die_explode_calls g_calls = {
    &rec_unit_get_coords,
    &rec_snd_play_at,
    &rec_fx_anim_spawn,
    &rec_rand_below,
    &rec_unit_remove_from_map,
    &rec_unit_calc_render_fine_y,
    &rec_unit_on_destroyed,
    &rec_game_sp_outcome_announce,
    &rec_target_release_ref,
    &rec_unit_housing_count_remove,
    &rec_storage_release_door_held_by_unit,
    &rec_path_free_slot,
    &rec_player_presence_lost,
    &rec_unit_set_state,
    &rec_map_fow_UpdateFoWPlus,
    &rec_game_SetEvent,
    &rec_unit_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_coords.clear();
    g_snd_play_at.clear();
    g_anim_spawns.clear();
    g_rand_calls.clear();
    g_rand_below_queue.clear();
    g_remove_calls.clear();
    g_calc_render_calls.clear();
    g_on_destroyed_calls.clear();
    g_outcome_announce_count = 0;
    g_target_release_calls.clear();
    g_housing_calls.clear();
    g_door_calls.clear();
    g_path_free_calls.clear();
    g_presence_calls.clear();
    g_set_state_calls.clear();
    g_fow_calls.clear();
    g_set_event_calls.clear();
    g_notify_calls.clear();
    g_mutate_unit_ptr = nullptr;
    g_mutate_proto_to = -1;
}

// Fixed "guard" slots no test's own (player,index) ever touches -- seeded with sentinel nonzero
// data each run so a wrong-index write lands somewhere observable. Kept well outside every test's
// own player (0..3) / index (0..4) / planet (0..5) ranges below.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;
constexpr int32_t  GUARD_PLANET = 17;

void seed_guard_slot(sim_fixture &fx) {
    unit &g                                                  = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id                                          = 88;
    g.x                                                      = 111;
    g.y                                                      = 122;
    g.target_ref                                             = 5;
    g.target_index                                           = 6;
    g.target2_ref                                            = 7;
    g.target2_index                                          = 8;
    g.path_slot_id                                           = 44;
    g.move_microstep                                         = 999;
    fx.profiles[GUARD_PLAYER].units_alive[GUARD_PLANET]      = 555;
    fx.profiles[GUARD_PLAYER].units_lost_total[GUARD_PLANET] = 333;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    uint32_t type            = 1;      // small/non-mother by default (< UNIT_TYPE_A_HELI==0xf)
    int32_t  anim_explo      = 0x4001; // distinct sentinel frame id
    int32_t  sound_explo     = 909;
    uint8_t  sight           = 17;
    int32_t  debris_anim_row = 0; // debris-burst gate OFF by default

    int32_t sim_active  = 0;
    int16_t player_side = 7; // matches sim_fixture::reset()'s own default -- "not this player" unless overridden

    double game_clock  = 100.0;
    double tick_budget = 10.0;

    int32_t fine_x = 1600; // 1600 / 32 == 50 exactly
    int32_t fine_y = 1760; // 1760 / 32 == 55 exactly

    uint32_t calc_render_fine_y_ret = 2400; // distinct from fine_x/fine_y

    int16_t target_ref    = 0;
    int16_t target_index  = 0;
    int16_t target2_ref   = 0;
    int16_t target2_index = 0;
    uint8_t path_slot_id  = 0xff; // 0xff == none

    uint8_t ux = 37, uy = 91; // the unit's OWN tile x/y (distinct from fine_x/fine_y)

    int32_t planet           = 4;
    int32_t units_alive_seed = 9;
    int32_t units_lost_seed  = 2;

    uint32_t bw_mask = 0x1ff; // distinct from geom.width_mask(0xff)/height_mask(0x3f)
    uint32_t bh_mask = 0x0ff;

    std::vector<int32_t> rand_queue; // consumed FIFO by rand_below

    int32_t mutate_proto_via_on_destroyed_to = -1; // T10 knob; -1 == no mutation
    int32_t mutate_target_cfg_type           = -1; // T10 knob: cfg_units[mutate_...to].type to seed
                                                   // (applied AFTER fx.reset(), same reason as the
                                                   // death_anim_table_* knobs below)

    // T4 knob: pre-seed one death_anim_table[row*4+roll] entry BEFORE the run (seed_and_run's own
    // fx.reset() zeroes the whole table first, so this must be applied AFTER reset, not by the
    // caller poking fx directly between two seed_and_run calls). -1 == do not pre-seed.
    int32_t death_anim_table_row   = -1;
    int32_t death_anim_table_roll  = -1;
    int32_t death_anim_table_value = 0;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.target_ref     = s.target_ref;
    u.target_index   = s.target_index;
    u.target2_ref    = s.target2_ref;
    u.target2_index  = s.target2_index;
    u.path_slot_id   = s.path_slot_id;
    u.x              = s.ux;
    u.y              = s.uy;
    u.move_microstep = -777; // sentinel, distinct from any proto.sight -- must be overwritten

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    cfg_unit &cu       = fx.cfg_units[s.cfg_row];
    cu.type            = s.type;
    cu.anim_explo      = s.anim_explo;
    cu.sound_explo     = s.sound_explo;
    cu.sight           = s.sight;
    cu.debris_anim_row = s.debris_anim_row;

    fx.sim_active   = s.sim_active;
    fx.player_side  = s.player_side;
    fx.game_clock   = s.game_clock;
    fx.tick_budget  = s.tick_budget;
    fx.planet_index = s.planet;

    fx.profiles[s.player].units_alive[s.planet]      = s.units_alive_seed;
    fx.profiles[s.player].units_lost_total[s.planet] = s.units_lost_seed;

    fx.geom.bw_mask = s.bw_mask;
    fx.geom.bh_mask = s.bh_mask;

    if (s.death_anim_table_row >= 0 && s.death_anim_table_roll >= 0)
        fx.death_anim_table[s.death_anim_table_row * 4 + s.death_anim_table_roll] = s.death_anim_table_value;

    if (s.mutate_proto_via_on_destroyed_to >= 0 && s.mutate_target_cfg_type >= 0)
        fx.cfg_units[(size_t)s.mutate_proto_via_on_destroyed_to].type = (uint32_t)s.mutate_target_cfg_type;

    g_get_coords_fine_x      = s.fine_x;
    g_get_coords_fine_y      = s.fine_y;
    g_calc_render_fine_y_ret = s.calc_render_fine_y_ret;

    reset_observations();
    g_rand_below_queue = s.rand_queue;
    if (s.mutate_proto_via_on_destroyed_to >= 0) {
        g_mutate_unit_ptr = &u;
        g_mutate_proto_to = s.mutate_proto_via_on_destroyed_to;
    }

    sim_store own = fx.store();
    detail::unit_state_die_explode(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_die_explode_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- small-vs-mothership dispatch: SIGNED comparison @0x0048559d(CMP)/0x004855a4(JG). Exact
    // boundary (0xe -> small, 0xf -> big) plus the HIGH-BIT-SET case that separates the fixed signed
    // cast (`static_cast<int32_t>(proto.type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)`) from the
    // naive unsigned `<` translation it replaced: for type=0x80000000u the two disagree (unsigned:
    // huge, NOT < 15 -> big arm; signed: INT_MIN, IS < 15 -> small arm). JG is a SIGNED jump (0f 8f),
    // confirmed by the opcode itself, not inferred.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        s.type   = 0xe; // just below UNIT_TYPE_A_HELI(0xf) -- must take SMALL arm
        seed_and_run(fx, s);
        ck(g_remove_calls.size() == 1, "T1a: type=0xe (boundary-1) takes the SMALL arm (unit_remove_from_map fires)");
        ck(g_calc_render_calls.empty() && g_on_destroyed_calls.empty(),
           "T1a: type=0xe does NOT take the BIG arm");

        s.type = 0xf; // exactly UNIT_TYPE_A_HELI -- must take BIG arm
        seed_and_run(fx, s);
        ck(g_calc_render_calls.size() == 1 && g_on_destroyed_calls.size() == 1,
           "T1b: type=0xf (boundary) takes the BIG arm (0x004855a4 JG taken)");
        ck(g_remove_calls.empty(), "T1b: type=0xf does NOT take the SMALL arm");

        s.type = 0x80000000u; // high bit set: signed vs unsigned comparison disagree
        seed_and_run(fx, s);
        ck(g_remove_calls.size() == 1,
           "T1c: type=0x80000000 (high bit set) takes the SMALL arm under the SIGNED cast fix "
           "(0x0048559d CMP / 0x004855a4 JG is a SIGNED compare -- INT_MIN < 0xe) -- an unsigned "
           "`<` translation would wrongly take the BIG arm here");
        ck(g_calc_render_calls.empty() && g_on_destroyed_calls.empty(),
           "T1c: type=0x80000000 must NOT reach the BIG-arm callees");
    }

    // =================================================================================================
    // T2 -- SIM_ACTIVE-gated death sound (0x00485524 CMP / 0x0048552b JZ), both sides, plus the
    // fine->tile conversion's round-toward-zero divide (a `>>5` "simplification" would floor instead
    // of truncate and diverge on this exact negative case).
    // =================================================================================================
    {
        Seed s;
        s.player     = 0;
        s.index      = 1;
        s.sim_active = 1;
        s.fine_x     = 1600; // -> tile_col 50
        s.fine_y     = 1760; // -> tile_row 55
        seed_and_run(fx, s);
        ck(g_snd_play_at.size() == 1 && g_snd_play_at[0].id == s.sound_explo &&
               g_snd_play_at[0].col == 50 && g_snd_play_at[0].row == 55,
           "T2a: SIM_ACTIVE!=0 -- snd_play_at(proto.sound_explo, tile_col=fine_x/32, "
           "tile_row=fine_y/32) (0x0048552d-0x0048556f)");
        ck(trace_eq({"unit_get_coords", "snd_play_at", "fx_anim_spawn",
                     "unit_remove_from_map", "unit_housing_count_remove", "storage_release_door_held_by_unit",
                     "unit_set_state", "map_fow_UpdateFoWPlus", "game_SetEvent", "unit_notify_ui"}),
           "T2a: death sound fires BEFORE the type dispatch, in this exact order");

        s.sim_active = 0;
        seed_and_run(fx, s);
        ck(g_snd_play_at.empty(), "T2b: SIM_ACTIVE==0 -- the sound record does not fire (0x0048552b JZ taken)");

        // Negative fine coordinate: round TOWARD ZERO, not floor. -1/32 == 0 in C++ (and in the
        // asm's SAR/SHL/SBB/SAR idiom, matching the .cpp header's own cross-reference to that idiom) --
        // a `>>5` translation would give -1 (arithmetic-shift floor) instead.
        s.sim_active = 1;
        s.fine_x     = -1;
        s.fine_y     = -1;
        seed_and_run(fx, s);
        ck(g_snd_play_at.size() == 1 && g_snd_play_at[0].col == 0 && g_snd_play_at[0].row == 0,
           "T2c: fine=-1 -> tile=0 (truncate toward zero, NOT -1) (0x0048552d-0x0048554e signed-divide idiom)");
    }

    // =================================================================================================
    // T3 -- SMALL arm, no debris burst (proto.debris_anim_row==0, gate @0x004855f1/0x004855f8): full
    // call/write order + explosion FX argument tuple (owner_or_flag=1, per 0x004855aa MOV EAX,0x1) +
    // the common tail (no target refs, path_slot_id==0xff, alive stays >0).
    // =================================================================================================
    {
        Seed s;
        s.player          = 0;
        s.index           = 1;
        s.debris_anim_row = 0;
        seed_and_run(fx, s);

        ck(trace_eq({"unit_get_coords", "fx_anim_spawn", "unit_remove_from_map", "unit_housing_count_remove",
                     "storage_release_door_held_by_unit", "unit_set_state", "map_fow_UpdateFoWPlus",
                     "game_SetEvent", "unit_notify_ui"}),
           "T3: exact call order, small/no-sound/no-debris/no-target/no-path/no-presence spine");

        ck(g_coords.size() == 1 && g_coords[0].player == 0 && g_coords[0].index == 1,
           "T3: unit_get_coords(cur_player, cur_index) (0x0048551f)");

        ck(g_anim_spawns.size() == 1, "T3: exactly one fx_anim_spawn (debris gated off)");
        if (g_anim_spawns.size() == 1) {
            const auto &a = g_anim_spawns[0];
            ck(a.x == (uint32_t)s.fine_x && a.y == (uint32_t)s.fine_y && a.anim_id == (uint32_t)s.anim_explo &&
                   a.owner_or_flag == 1u,
               "T3: fx_anim_spawn(fine_x, fine_y, proto.anim_explo, elapsed, owner_or_flag=1) (0x004855aa-0x004855dd)");
            ck_eq_d(a.elapsed, s.game_clock - s.tick_budget, "T3: fx_anim_spawn elapsed = game_clock - tick_budget (0x004855b0-0x004855bf)");
        }
        ck(g_rand_calls.empty(), "T3: debris_anim_row==0 -- no rand_below draws at all (0x004855f8 JZ taken)");

        ck(g_remove_calls.size() == 1 && g_remove_calls[0].player == 0 && g_remove_calls[0].idx == 1,
           "T3: unit_remove_from_map(cur_player, cur_index) (0x0048568b)");

        ck(g_target_release_calls.empty(), "T3: target_ref==target2_ref==0 -- no release calls");
        ck(g_housing_calls.size() == 1 && g_housing_calls[0].player == 0 && g_housing_calls[0].proto_id == (int32_t)s.cfg_row,
           "T3: unit_housing_count_remove(cur_player, unit_proto_id) (0x00485808)");
        ck(g_door_calls.size() == 1 && g_door_calls[0].player == 0 && g_door_calls[0].idx == 1,
           "T3: storage_release_door_held_by_unit(cur_player, cur_index) (0x0048581b)");
        ck(g_path_free_calls.empty(), "T3: path_slot_id==0xff -- path_free_slot does NOT fire (0x0048582c JZ taken)");

        ck_eq((uint32_t)fx.profiles[0].units_lost_total[s.planet], (uint32_t)(s.units_lost_seed + 1),
              "T3: units_lost_total[planet] += 1 (0x00485859)");
        ck_eq((uint32_t)fx.profiles[0].units_alive[s.planet], (uint32_t)(s.units_alive_seed - 1),
              "T3: units_alive[planet] -= 1 (0x00485877)");
        ck(g_presence_calls.empty(), "T3: units_alive stayed nonzero (9-1=8) -- player_presence_lost does NOT fire");

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_DIE_EXPLODE_CORPSE_FOW_DECAY,
           "T3: unit_set_state(CORPSE_FOW_DECAY=4) (0x004858ab-0x004858b0)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, (uint32_t)s.sight,
              "T3: move_microstep = proto.sight (saved-FoW-decay overload) (0x004858c4-0x004858d6)");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].player == 0 && g_fow_calls[0].x == s.ux &&
               g_fow_calls[0].y == s.uy && g_fow_calls[0].sight == s.sight,
           "T3: map_fow_UpdateFoWPlus(cur_player, unit.x, unit.y, move_microstep-as-byte) (0x00485901)");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == UNIT_STATE_DIE_EXPLODE_MAP_OBJECTS_REFRESH,
           "T3: game_SetEvent(14 == MAP_OBJECTS_REFRESH) (0x0048590b)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].side == 0 && g_notify_calls[0].idx == 1,
           "T3: unit_notify_ui(cur_player, cur_index) (0x0048591e)");
    }

    // =================================================================================================
    // T4 -- SMALL arm, debris burst ON (two distinct debris_anim_row values), pinning the rand_below
    // DRAW ORDER (dx, then dy, then the death_anim_table roll -- translator-brief rule 13: RNG draws
    // must not be reordered) and the row*4+roll index formula (0x00485642-0x00485658), plus the
    // torus-wrap AND masks (0x00485666/0x00485672) with masks DISTINCT from geom.width_mask/height_mask.
    // =================================================================================================
    {
        Seed s;
        s.player                 = 1;
        s.index                  = 2;
        s.debris_anim_row        = 3;
        s.rand_queue             = {5, 3, 2}; // dx-roll=5 -> scatter_dx=5-8=-3; dy-roll=3 -> scatter_dy=3-8=-5; table-roll=2
        s.death_anim_table_row   = 3;
        s.death_anim_table_roll  = 2; // matches the table-roll draw above
        s.death_anim_table_value = 0x7A7A;
        seed_and_run(fx, s);

        ck(g_rand_calls.size() == 3, "T4: exactly 3 rand_below draws when debris_anim_row!=0");
        if (g_rand_calls.size() == 3) {
            ck_eq((uint32_t)g_rand_calls[0], 16u, "T4: draw #1 rand_below(16) -- X scatter (0x004855fe)");
            ck_eq((uint32_t)g_rand_calls[1], 16u, "T4: draw #2 rand_below(16) -- Y scatter, AFTER draw #1 (0x0048560e)");
            ck_eq((uint32_t)g_rand_calls[2], 4u, "T4: draw #3 rand_below(4) -- death_anim_table column roll (0x0048564b)");
        }

        ck(g_anim_spawns.size() == 2, "T4: 2 fx_anim_spawn calls (explosion + debris burst)");
        if (g_anim_spawns.size() == 2) {
            const auto    &burst          = g_anim_spawns[1];
            const uint32_t expect_burst_x = (uint32_t)(s.fine_x + (5 - 8)) & s.bw_mask; // 0x0048566f-0x00485672
            const uint32_t expect_burst_y = (uint32_t)(s.fine_y + (3 - 8)) & s.bh_mask; // 0x00485663-0x00485666
            ck_eq(burst.x, expect_burst_x, "T4: debris burst_x = (fine_x + scatter_dx) & geom.bw_mask (0x0048566f-0x00485672)");
            ck_eq(burst.y, expect_burst_y, "T4: debris burst_y = (fine_y + scatter_dy) & geom.bh_mask (0x00485663-0x00485666)");
            ck_eq(burst.anim_id, 0x7A7Au,
                  "T4: debris anim_id = death_anim_table[debris_anim_row*4 + roll] = table[3*4+2] (0x00485658-0x0048565a)");
            ck_eq(burst.owner_or_flag, 0u, "T4: debris fx_anim_spawn owner_or_flag=0 (0x0048561e MOV EAX,0)");
        }

        // A second, distinct row -- proves the index formula actually reads `row`, not a constant.
        Seed s2                   = s;
        s2.debris_anim_row        = 9;
        s2.rand_queue             = {0, 0, 1}; // dx-roll=0 -> -8; dy-roll=0 -> -8; table-roll=1
        s2.death_anim_table_row   = 9;
        s2.death_anim_table_roll  = 1;
        s2.death_anim_table_value = 0x5B5B;
        seed_and_run(fx, s2);
        ck(g_anim_spawns.size() == 2 && g_anim_spawns[1].anim_id == 0x5B5Bu,
           "T4b: a DIFFERENT debris_anim_row(9) selects table[9*4+1], not the row=3 case's entry -- index formula uses the field");
    }

    // =================================================================================================
    // T5 -- BIG arm: fine_y REPLACED by unit_calc_render_fine_y's return (fine_x untouched), explosion
    // FX owner_or_flag=2 (vs 1 in the small arm), unit_on_destroyed args.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        s.type   = UNIT_TYPE_A_HELI; // 0xf, BIG arm
        seed_and_run(fx, s);

        ck(g_calc_render_calls.size() == 1 && g_calc_render_calls[0].player == 0 && g_calc_render_calls[0].idx == 1,
           "T5: unit_calc_render_fine_y(cur_player, cur_index) (0x004856a3)");
        ck(g_anim_spawns.size() == 1, "T5: exactly one fx_anim_spawn on the BIG arm");
        if (g_anim_spawns.size() == 1) {
            const auto &a = g_anim_spawns[0];
            ck(a.x == (uint32_t)s.fine_x && a.y == s.calc_render_fine_y_ret,
               "T5: fx_anim_spawn(fine_x UNCHANGED, render_fine_y REPLACING fine_y) (0x004856a8/0x004856d8-0x004856de)");
            ck(a.owner_or_flag == 2u, "T5: BIG-arm explosion owner_or_flag=2 (0x004856ab MOV EAX,0x2), vs 1 in the SMALL arm");
            ck(a.anim_id == (uint32_t)s.anim_explo, "T5: BIG-arm explosion anim_id = proto.anim_explo (same field as the small arm)");
        }
        ck(g_on_destroyed_calls.size() == 1 && g_on_destroyed_calls[0].player == 0 && g_on_destroyed_calls[0].idx == 1,
           "T5: unit_on_destroyed(cur_player, cur_index) (0x004856f1)");
        ck(trace_eq({"unit_get_coords", "unit_calc_render_fine_y", "fx_anim_spawn", "unit_on_destroyed",
                     "unit_housing_count_remove", "storage_release_door_held_by_unit", "unit_set_state",
                     "map_fow_UpdateFoWPlus", "game_SetEvent", "unit_notify_ui"}),
           "T5: BIG-arm call order (non-mother/other-player, so no outcome_announce)");
    }

    // =================================================================================================
    // T6 -- SP mothership-outcome-announce gate: a PLAIN CONJUNCTION of (player==PlayerSide) AND
    // (type==A_HELI_MOTHER OR type==H_HELI_MOTHER) (0x004856f6-0x0048576d). All four combinations.
    // =================================================================================================
    {
        // local + A_HELI_MOTHER -> announce
        Seed s;
        s.player      = 1;
        s.index       = 2;
        s.player_side = 1;
        s.type        = UNIT_TYPE_A_HELI_MOTHER;
        seed_and_run(fx, s);
        ck(g_outcome_announce_count == 1, "T6a: local player + A_HELI_MOTHER(0x13) -> game_sp_outcome_announce fires (0x00485735 JZ taken)");

        // local + H_HELI_MOTHER -> announce (second type check, 0x00485760/0x00485767)
        s.type = UNIT_TYPE_H_HELI_MOTHER;
        seed_and_run(fx, s);
        ck(g_outcome_announce_count == 1, "T6b: local player + H_HELI_MOTHER(0x14) -> announce fires via the SECOND type check");

        // local + non-mother -> no announce
        s.type = UNIT_TYPE_A_HELI; // BIG arm, not a mother type
        seed_and_run(fx, s);
        ck(g_outcome_announce_count == 0, "T6c: local player + non-mother type -> announce does NOT fire (0x00485767 JNZ taken)");

        // other player + mother -> no announce (first CMP fails before the type is even checked)
        s.player      = 0;
        s.player_side = 5; // != player -> "other"
        s.type        = UNIT_TYPE_A_HELI_MOTHER;
        seed_and_run(fx, s);
        ck(g_outcome_announce_count == 0,
           "T6d: other-player mother -> announce does NOT fire (0x00485703 JNZ taken, short-circuits before the type checks)");
    }

    // =================================================================================================
    // T7 -- target_ref/target2_ref release: two INDEPENDENT ifs (both can fire), each clearing its own
    // index+ref pair, in target_ref-then-target2_ref order (0x00485772-0x004857f8).
    // =================================================================================================
    {
        // both zero -> neither fires
        Seed s;
        s.player = 0;
        s.index  = 1;
        seed_and_run(fx, s);
        ck(g_target_release_calls.empty(), "T7a: target_ref==target2_ref==0 -- no release calls (both JZ taken)");

        // only target_ref set
        s.target_ref   = 0x21;
        s.target_index = 3;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 1 && g_target_release_calls[0].player == 0 &&
               g_target_release_calls[0].idx == 1 && g_target_release_calls[0].mode == 1u,
           "T7b: target_ref!=0 -- target_release_ref(cur_player, cur_index, mode=1) (0x00485781-0x00485794)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_ref, 0u, "T7b: target_ref cleared to 0 (0x0048579e)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_index, 0u, "T7b: target_index cleared to 0 (0x004857ac)");

        // only target2_ref set
        s.target_ref    = 0;
        s.target_index  = 0;
        s.target2_ref   = 0x22;
        s.target2_index = 4;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 1 && g_target_release_calls[0].mode == 3u,
           "T7c: target2_ref!=0 -- target_release_ref(..., mode=3) (0x004857c4-0x004857d7)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target2_ref, 0u, "T7c: target2_ref cleared to 0 (0x004857e1)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target2_index, 0u, "T7c: target2_index cleared to 0 (0x004857ea)");

        // both set -> both fire, target_ref (mode=1) BEFORE target2_ref (mode=3)
        s.target_ref    = 0x21;
        s.target_index  = 3;
        s.target2_ref   = 0x22;
        s.target2_index = 4;
        seed_and_run(fx, s);
        ck(g_target_release_calls.size() == 2 && g_target_release_calls[0].mode == 1u && g_target_release_calls[1].mode == 3u,
           "T7d: both set -- mode=1 release fires BEFORE mode=3 release (asm's target_ref block precedes target2_ref block)");
    }

    // =================================================================================================
    // T8 -- path_slot_id gate (0x00485825 CMP/0x0048582c JZ): 0xff (none) vs an assigned slot.
    // =================================================================================================
    {
        Seed s;
        s.player       = 0;
        s.index        = 1;
        s.path_slot_id = 0xff;
        seed_and_run(fx, s);
        ck(g_path_free_calls.empty(), "T8a: path_slot_id==0xff -- path_free_slot does NOT fire");

        s.path_slot_id = 12;
        seed_and_run(fx, s);
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == 0 && g_path_free_calls[0].idx == 1,
           "T8b: path_slot_id!=0xff -- path_free_slot(cur_player, cur_index) fires (0x0048583c)");
    }

    // =================================================================================================
    // T9 -- presence-lost gate: units_alive[planet] reaching exactly 0 AFTER the decrement (not before)
    // fires player_presence_lost(player, mode=0) (0x00485894-0x004858a6); staying nonzero does not.
    // =================================================================================================
    {
        Seed s;
        s.player           = 0;
        s.index            = 1;
        s.units_alive_seed = 5; // -> 4 after decrement, stays nonzero
        seed_and_run(fx, s);
        ck(g_presence_calls.empty(), "T9a: units_alive 5->4 (nonzero) -- player_presence_lost does NOT fire (0x0048589b JNZ taken)");

        s.units_alive_seed = 1; // -> 0 after decrement
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.profiles[0].units_alive[s.planet], 0u, "T9b: units_alive[planet] reaches exactly 0");
        ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == 0 && g_presence_calls[0].mode == 0u,
           "T9b: units_alive 1->0 -- player_presence_lost(cur_player, mode=0) fires (0x004858a6)");
        ck(trace_eq({"unit_get_coords", "fx_anim_spawn", "unit_remove_from_map", "unit_housing_count_remove",
                     "storage_release_door_held_by_unit", "player_presence_lost", "unit_set_state",
                     "map_fow_UpdateFoWPlus", "game_SetEvent", "unit_notify_ui"}),
           "T9b: player_presence_lost is ordered right after the counter arithmetic, before unit_set_state");
    }

    // =================================================================================================
    // T10 -- unit_proto_id caching across effectful callees: STRENGTHENING the refutation of a
    // reviewer-suspected staleness bug. The .cpp reads unit_proto_id/proto ONCE at function entry and
    // reuses that cached copy for BOTH the outcome-announce type gate (whose real .asm re-reads it
    // fresh via roster row/col arithmetic AFTER unit_on_destroyed has already run, 0x00485705-
    // 0x00485767) and the unit_housing_count_remove proto-id argument (whose real .asm re-reads it
    // fresh via the CUR_UNIT pointer AFTER unit_on_destroyed, 0x004857f8-0x00485801). Checked and
    // REFUTED by inspection (neither on_destroyed's own body writes unit.unit_proto_id). This case
    // makes that an EXECUTABLE pin: the on_destroyed recorder ARTIFICIALLY mutates unit_proto_id
    // (something the real callee does not do) through the SAME aliased unit record own.cur_unit()
    // reads, proving exactly which value (cached-at-entry vs freshly-mutated) the .cpp's later reads
    // actually consume -- if a future edit swaps either cached read for a fresh re-read, this flips.
    // =================================================================================================
    {
        Seed s;
        s.player                           = 1;
        s.index                            = 3;
        s.player_side                      = 1; // local player -- reaches the announce gate
        s.cfg_row                          = 20;
        s.type                             = UNIT_TYPE_A_HELI_MOTHER; // cached proto.type == mother
        s.mutate_proto_via_on_destroyed_to = 55;                      // a DIFFERENT cfg row, seeded non-mother below
        s.mutate_target_cfg_type           = 1;                       // non-mother -- would fail the announce gate if freshly re-read

        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.u(1, 3).unit_proto_id, 55u,
              "T10: the on_destroyed recorder's mutation actually landed on the SAME unit record own.cur_unit() reads");
        ck(g_outcome_announce_count == 1,
           "T10: outcome-announce gate used the CACHED proto.type(==A_HELI_MOTHER, read pre-mutation), not a fresh "
           "post-mutation re-read (which would be non-mother and skip the call) -- pins the .cpp's actual cache-based reads");
        ck(g_housing_calls.size() == 1 && g_housing_calls[0].proto_id == (int32_t)s.cfg_row,
           "T10: unit_housing_count_remove got the CACHED unit_proto_id(20), not the mutated 55 -- pins the same "
           "cache-based read at 0x004857f8-0x00485801's counterpart in the .cpp");
    }

    // =================================================================================================
    // T11 -- neighbouring-slot non-corruption: a guard unit/profile pair this function never addresses
    // stays exactly as seeded, and a DIFFERENT planet slot in the SAME player's profile (an
    // array-indexed write, so a wrong index is the realistic bug class) is untouched too.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        s.planet = 4;
        seed_and_run(fx, s); // seed_and_run's own fx.reset()+seed_guard_slot() ran fresh for this call

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 88 && g.x == 111 && g.y == 122, "T11: guard unit's core fields untouched");
        ck(g.target_ref == 5 && g.target_index == 6 && g.target2_ref == 7 && g.target2_index == 8,
           "T11: guard unit's target/target2 pairs untouched");
        ck(g.path_slot_id == 44 && g.move_microstep == 999, "T11: guard unit's path_slot_id/move_microstep untouched");
        ck_eq((uint32_t)fx.profiles[GUARD_PLAYER].units_alive[GUARD_PLANET], 555u,
              "T11: guard player's profile.units_alive untouched");
        ck_eq((uint32_t)fx.profiles[GUARD_PLAYER].units_lost_total[GUARD_PLANET], 333u,
              "T11: guard player's profile.units_lost_total untouched");
        // A different planet slot within the SAME player's profile (adjacent array element).
        ck_eq((uint32_t)fx.profiles[0].units_alive[GUARD_PLANET], 0u,
              "T11: a DIFFERENT planet index in the acted-on player's OWN profile is untouched (array-index precision)");
        ck_eq((uint32_t)fx.profiles[0].units_lost_total[GUARD_PLANET], 0u,
              "T11: same check for units_lost_total at a different planet index");
    }
}

} // namespace mh::sim::test
