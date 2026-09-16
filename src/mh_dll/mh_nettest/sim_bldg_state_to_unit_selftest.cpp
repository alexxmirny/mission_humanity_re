//
// sim_bldg_state_to_unit_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_to_unit
// (sim/sim_bldg_state_to_unit.h/.cpp), SIM1-G3 building_tick machinery -- a completed building whose
// cfg row configures an "equivalent" mobile unit converts itself into that unit. LARGEST function in
// this slice (0x66e bytes); this file exists because the task brief called it DO-NOT-ARM regardless
// of the shadow-closure tool's report and asked for a dedicated offline oracle.
//
// SCOPE (honest, not exhaustive): this file covers the equivalent==0 early exit, the full SUCCESS
// stamping sequence (including the two flagged uncertainties -- see below), the FAILURE "dies
// instead" sequence, the shared tail (mother bookkeeping, energy zero + header-row bump, AI notify,
// unmap, alive-count decrement + purge/presence-lost gate, final state/anim), and the A_MOTHER/
// H_MOTHER type gate on both success and failure. It does NOT independently re-derive the
// tile<->fine conversion or the x87/idiom precedents already hand-verified in the .cpp's own header
// banner -- this file trusts and cross-checks against that derivation rather than re-deriving it a
// second time from raw bytes.
//
// TWO FLAGGED UNCERTAINTIES THIS FILE EXISTS TO PIN (see sim_bldg_state_to_unit.h's banner for the
// full derivation) -- both are asserted as the DOCUMENTED (not "corrected") behaviour:
//
//   (1) mother_reelect_primary's (x, y) ARGUMENTS DIFFER BY WHICH PATH RAN. The placement-corner
//       locals are TILE coordinates on the SUCCESS arm (never rewritten) but get REASSIGNED IN PLACE
//       to FINE (pixel) coordinates on the FAILURE arm (0x004717d3-0x004717f8) before the shared tail
//       runs. TU_mother_success asserts TILE coords reach the call; TU_mother_failure asserts FINE
//       (32x-ish, wrapped) coords reach the SAME call site -- a "corrected" implementation that always
//       used tile coords would pass TU_mother_success and fail TU_mother_failure.
//
//   (2) THE ELEVATION LOOKUP READS THE AMBIENT ROSTER SLOT (units[cur_player][cur_index], the SAME
//       index this function uses for the BUILDING roster everywhere else), NOT THE NEW UNIT'S OWN
//       SLOT. TU_success seeds units[cur_player][cur_index]'s cfg-driven elevation to a value DISTINCT
//       from both the new unit's "natural" (cfg_units[equivalent]) elevation and confirms the ambient
//       one wins -- a "corrected" implementation reading the new unit's own slot would disagree here.
//
// EXPECTED CALL/WRITE ORDER, HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_to_unit_00471443.asm), cross-checked against the .cpp/.h's own
// per-line address citations -- NOT read off the .cpp body alone:
//
//   cfg_buildings[bid].equivalent==0 (0x00471460-0x00471471) -> b.state=1 (LAB_00471a9c), RETURN.
//   Else: calc_placement_corner_from_center_by_type(bid, b.x, b.y, &corner_x, &corner_y)
//     (0x00471477-0x004714a4) -> population_remove(player, cfg_units[equivalent].human)
//     (0x004714ad-0x004714ca) -> unit_create(corner_x, corner_y, equivalent, player, is_ship=2)
//     (0x004714cf-0x004714f8) ->
//   SUCCESS (new_index!=0, 0x0047150a-0x004717ce): stamp nu.shuttle_slot/move_heading/facing_target/
//     facing_current/move_microstep/elevation/state/order from the vacating building + cfg tables ->
//     repoint prod_shuttle_slot(player, b.shuttle_slot).{type_ref_id,status,src_building_index}.
//   FAILURE (new_index==0, 0x004717d3-0x004718bb): corner_x/y REASSIGNED tile->fine in place ->
//     [SIM_ACTIVE: snd_play_at(cfg_units[equivalent].sound_explo, tile) -- the original
//      offscreen_snd_volume+snd_play pair, one record since the LIFT-NOTIFY offscreen conversion] ->
//     fx_anim_spawn(corner_x, corner_y, cfg_units[equivalent].anim_explo, game_clock, flag=1)
//     UNCONDITIONALLY -> prod_shuttle_slot(player, b.shuttle_slot).type_ref_id = 0.
//   SHARED TAIL (0x004718bb-0x00471aa7), both arms: roster-derived type A_MOTHER/H_MOTHER gate ->
//     [primary_mother_unit[planet]==0: write new_index] -> [primary_mother_bldg[planet]==cur_index:
//     clear it, mother_reelect_primary(player, corner_x, corner_y)] -> b.energy=0.0 ->
//     buildings[player][0].energy += -1.0 (header-row scratch, ALWAYS slot 0) ->
//     ai_notify_object_removed(player|REF_BLDG_BIT, cur_index, hard_remove=0) ->
//     bldg_unmap_footprint(player, cur_index) -> buildings_alive[planet] -= 1 ->
//     [==0: unit_purge_unregistered(player), player_presence_lost(player, 0)] ->
//     b.state=BLDG_STATE_RUBBLE_SIGHT_DECAY(4), b.anim[0]=0.
//
#include "sim/sim_bldg_state_to_unit.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "ai/ai_state.h"           // mh::ai::REF_BLDG_BIT
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER (0x06) / BUILDING_TYPE_H_MOTHER (0x1a)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 11 callees ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs (settable per case BEFORE seed_and_run) --------------------------------
int32_t  g_corner_tile_x   = 0;
int32_t  g_corner_tile_y   = 0;
uint32_t g_unit_create_ret = 0;

// ---- per-callee recorders (11, one per bldg_state_to_unit_calls member) ------------------------
struct CornerCall {
    uint16_t type_arg; // really `bid` (building_id), per the asm -- see the .cpp's own comment
    int32_t  center_x, center_y;
};
std::vector<CornerCall> g_corner_calls;
void                    rec_calc_corner(uint16_t type_arg, int32_t center_x, int32_t center_y, uint32_t *out_x, uint32_t *out_y) {
    tr("calc_placement_corner_from_center_by_type");
    g_corner_calls.push_back({type_arg, center_x, center_y});
    *out_x = (uint32_t)g_corner_tile_x;
    *out_y = (uint32_t)g_corner_tile_y;
}

struct PopRemoveCall {
    uint32_t player;
    int32_t  count;
};
std::vector<PopRemoveCall> g_pop_remove_calls;
void                       rec_population_remove(uint32_t player, int32_t count) {
    tr("population_remove");
    g_pop_remove_calls.push_back({player, count});
}

struct UnitCreateCall {
    uint32_t x, y;
    uint16_t unit, player;
    uint8_t  is_ship;
};
std::vector<UnitCreateCall> g_unit_create_calls;
uint32_t                    rec_unit_create(uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship) {
    tr("unit_create");
    g_unit_create_calls.push_back({x, y, unit, player, is_ship});
    return g_unit_create_ret;
}

struct ReelectCall {
    int32_t player, x, y;
};
std::vector<ReelectCall> g_reelect_calls;
int32_t                  rec_mother_reelect_primary(int32_t player, int32_t x, int32_t y) {
    tr("mother_reelect_primary");
    g_reelect_calls.push_back({player, x, y});
    return 0;
}

struct NotifyRemovedCall {
    uint32_t flags, object_index;
    int32_t  hard_remove;
};
std::vector<NotifyRemovedCall> g_notify_removed_calls;
void                           rec_ai_notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    tr("ai_notify_object_removed");
    g_notify_removed_calls.push_back({flags, object_index, hard_remove});
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

std::vector<uint32_t> g_purge_calls;
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

struct SndAtCall {
    int32_t id, col, row;
};
std::vector<SndAtCall> g_snd_play_at_calls;
void                   rec_snd_play_at(int32_t id, int32_t tile_col, int32_t tile_row) {
    tr("snd_play_at");
    g_snd_play_at_calls.push_back({id, tile_col, tile_row});
}

struct AnimSpawnCall {
    uint32_t x, y, frame;
    double   elapsed;
    uint32_t flag;
};
std::vector<AnimSpawnCall> g_anim_spawn_calls;
uint32_t                   rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t frame, double elapsed, uint32_t flag) {
    tr("fx_anim_spawn");
    g_anim_spawn_calls.push_back({x, y, frame, elapsed, flag});
    return 0;
}

const bldg_state_to_unit_calls g_calls = {
    &rec_calc_corner,
    &rec_population_remove,
    &rec_unit_create,
    &rec_mother_reelect_primary,
    &rec_ai_notify_object_removed,
    &rec_bldg_unmap_footprint,
    &rec_unit_purge_unregistered,
    &rec_player_presence_lost,
    &rec_snd_play_at,
    &rec_fx_anim_spawn,
};

void reset_observations() {
    g_trace.clear();
    g_corner_calls.clear();
    g_pop_remove_calls.clear();
    g_unit_create_calls.clear();
    g_reelect_calls.clear();
    g_notify_removed_calls.clear();
    g_unmap_calls.clear();
    g_purge_calls.clear();
    g_presence_calls.clear();
    g_snd_play_at_calls.clear();
    g_anim_spawn_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player       = 0;
    int32_t  cur_index    = 1; // NEVER 0 in any case here -- 0 is the "header row" slot0 distinguishes
    uint16_t cfg_row      = 20;
    int32_t  equivalent   = 7; // cfg_buildings[cfg_row].equivalent -> cfg_units row
    uint8_t  bldg_type    = 1; // non-mother by default
    uint8_t  door_route10 = 3; // cfg_buildings[cfg_row].door_approach_route[10] -> move_heading, <8
    uint8_t  bx = 40, by = 45; // building's own tile position

    uint8_t shuttle_slot = 4;
    int32_t planet       = 2;

    int32_t buildings_alive_seed     = 5;
    int32_t primary_mother_unit_seed = 0;
    int32_t primary_mother_bldg_seed = 0; // set == cur_index to trigger the reelect gate

    int32_t sim_active = 0;
    double  game_clock = 500.0;

    double  bldg_energy_seed   = 77.5;   // b.energy before run -- must become 0.0
    double  header_energy_seed = 30.0;   // buildings[player][0].energy before run -- must become -=1.0
    int32_t anim0_seed         = 0x1234; // b.anim[0] before run -- must become 0

    // calc_placement_corner_from_center_by_type mock return (TILE space)
    int32_t corner_tile_x = 50, corner_tile_y = 60;

    uint32_t new_index   = 9;  // nonzero -> SUCCESS, 0 -> FAILURE
    int32_t  human_count = 42; // cfg_units[equivalent].human

    // UNCERTAINTY (2): the ambient roster slot's own cfg-driven elevation, DISTINCT from both the
    // new unit's own natural elevation (equivalent_elevation) and from each other.
    int32_t ambient_proto        = 3;   // units[player][cur_index].unit_proto_id, DISTINCT from equivalent
    int32_t ambient_elevation    = 555; // cfg_units[ambient].elevation_2 -- the field the asm READS
    int32_t equivalent_elevation = 111; // cfg_units[equivalent].elevation_2 -- must NOT be the answer
    // THE FIELD NEXT DOOR, and the reason this test used to pass a wrong body (2026-09-07). The asm
    // reads cfg offset 0x19f = `elevation_2`; the .cpp, this fixture and the header banner all said
    // `elevation` (0x19b) -- two adjacent, identically-typed, uncommented int32s. All three agreed
    // with each other and none of them with the binary, so the assertions below were satisfied by
    // the defect they were written to pin. Seeded to a THIRD distinct value so a body that reads
    // `elevation` fails rather than reading a zero that could be mistaken for anything.
    int32_t ambient_wrong_field = 777; // cfg_units[ambient].elevation -- the WRONG neighbour

    int32_t nu_pre_microstep = 5;   // nu.move_microstep BEFORE the call (what unit_create left behind)
    uint8_t facing_sentinel  = 200; // move_microsteps[door_route10*32 + nu_pre_microstep].facing

    int32_t slot_dest_planet = -1; // mismatched vs `planet` by default -> AWAY_FROM_DEST order arm

    int32_t sound_explo = 4242; // cfg_units[equivalent].sound_explo -- the unit's OWN sound
    int32_t anim_explo  = 5252; // cfg_units[equivalent].anim_explo

    uint32_t bw_mask = 0xffffu, bh_mask = 0xffffu; // generous -- no wrap for the values used here
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b    = fx.b(s.player, s.cur_index);
    b.building_id  = s.cfg_row;
    b.shuttle_slot = s.shuttle_slot;
    b.x            = s.bx;
    b.y            = s.by;
    b.energy       = s.bldg_energy_seed;
    b.state        = (uint16_t)0xBEEF; // sentinel, distinct from every state literal this fn writes
    std::memset(b.anim, 0, sizeof(b.anim));
    std::memcpy(&b.anim[0], &s.anim0_seed, sizeof(int32_t));

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.cur_index;

    cfg_building &cb           = fx.cfg_buildings[s.cfg_row];
    cb.equivalent              = s.equivalent;
    cb.type                    = s.bldg_type;
    cb.door_approach_route[10] = s.door_route10;

    cfg_unit &cu   = fx.cfg_units[(size_t)s.equivalent];
    cu.human       = s.human_count;
    cu.elevation_2 = s.equivalent_elevation;
    cu.elevation   = s.equivalent_elevation; // both, so `equivalent` cannot be the answer either way
    cu.sound_explo = s.sound_explo;
    cu.anim_explo  = s.anim_explo;

    // UNCERTAINTY (2): the AMBIENT roster slot (units[player][cur_index]) -- NOT the new unit's own
    // slot -- is what the elevation lookup re-derives from.
    fx.cfg_units[(size_t)s.ambient_proto].elevation_2 = s.ambient_elevation;
    fx.cfg_units[(size_t)s.ambient_proto].elevation   = s.ambient_wrong_field;
    fx.u(s.player, s.cur_index).unit_proto_id         = (uint16_t)s.ambient_proto;

    if (s.new_index != 0) {
        // The stub does not populate the created record the way the real llm_strat_unit_create
        // would -- seed unit_proto_id/move_microstep ourselves so the fields this function re-reads
        // AFTER the create call have something real, same posture as every other sim/ oracle's
        // create-style stubs (e.g. sim_prod_completion_selftest.cpp's own comment on this exact
        // pattern).
        unit &nu          = fx.u(s.player, (int32_t)s.new_index);
        nu.unit_proto_id  = (uint16_t)s.equivalent;
        nu.move_microstep = s.nu_pre_microstep;
    }

    fx.move_microsteps[(size_t)s.door_route10 * MICROSTEPS_PER_HEADING + (size_t)s.nu_pre_microstep]
        .facing = s.facing_sentinel;

    fx.profiles[s.player].buildings_alive[s.planet]     = s.buildings_alive_seed;
    fx.profiles[s.player].primary_mother_unit[s.planet] = s.primary_mother_unit_seed;
    fx.profiles[s.player].primary_mother_bldg[s.planet] = s.primary_mother_bldg_seed;

    fx.planet_index = s.planet;
    fx.sim_active   = s.sim_active;
    fx.game_clock   = s.game_clock;
    fx.geom.bw_mask = s.bw_mask;
    fx.geom.bh_mask = s.bh_mask;

    // header-row scratch (buildings[player][0]) -- cur_index is never 0 in this file, so this is
    // always a DIFFERENT roster slot than `b`.
    fx.b(s.player, 0).energy = s.header_energy_seed;

    prod_shuttle_slot &slot = fx.prod_shuttle_slots[(size_t)s.player * PROD_SHUTTLE_SLOTS_PER_PLAYER +
                                                    (size_t)s.shuttle_slot];
    slot.dest_planet        = (int16_t)s.slot_dest_planet;
    slot.type_ref_id        = 0xBEEFu & 0xffffu; // nonzero sentinel -- FAILURE must clear it to 0
    slot.status             = 0;
    slot.src_building_index = -1;

    g_corner_tile_x   = s.corner_tile_x;
    g_corner_tile_y   = s.corner_tile_y;
    g_unit_create_ret = s.new_index;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_to_unit(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_to_unit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // TU1 -- cfg_buildings[bid].equivalent==0: state=1 (LAB_00471a9c, 0x00471a9c-0x00471aa1), RETURN
    // with NO other observable effect -- no calls, no other field writes.
    // =================================================================================================
    {
        Seed s;
        s.equivalent = 0;
        seed_and_run(fx, s);

        ck(g_trace.empty(), "TU1 (0x00471460-0x00471471): equivalent==0 -- NO calls of any kind fire");
        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 1u,
              "TU1 (0x00471a9c-0x00471aa1): b.state = BLDG_STATE_NO_EQUIVALENT_UNIT(1)");
        ck_eq_d(fx.b(s.player, s.cur_index).energy, s.bldg_energy_seed,
                "TU1: b.energy UNTOUCHED (early exit precedes any energy write)");
        ck_eq_d(fx.b(s.player, 0).energy, s.header_energy_seed,
                "TU1: header-row buildings[player][0].energy UNTOUCHED");
        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet], (uint32_t)s.buildings_alive_seed,
              "TU1: buildings_alive[planet] UNTOUCHED");
        {
            int32_t anim0 = -1;
            std::memcpy(&anim0, &fx.b(s.player, s.cur_index).anim[0], sizeof(anim0));
            ck_eq((uint32_t)anim0, (uint32_t)s.anim0_seed, "TU1: b.anim[0] UNTOUCHED");
        }
    }

    // =================================================================================================
    // TU2 -- SUCCESS, non-mother, AWAY_FROM_DEST order arm (default slot_dest_planet=-1 != planet=2).
    // The main stamping case: covers the full field-by-field new-unit stamp, the shuttle-slot repoint,
    // UNCERTAINTY (2) (ambient elevation lookup), the header-row -1.0 energy bump, the AI-notify/unmap
    // calls, and confirms buildings_alive staying nonzero suppresses purge/presence-lost. Non-mother
    // type also proves primary_mother_unit/_bldg are NOT touched.
    // =================================================================================================
    {
        Seed s; // all defaults: player=0, cur_index=1, cfg_row=20, equivalent=7, bldg_type=1 (non-mother)
        seed_and_run(fx, s);

        ck(trace_eq({"calc_placement_corner_from_center_by_type", "population_remove", "unit_create",
                     "ai_notify_object_removed", "bldg_unmap_footprint"}),
           "TU2: exact call order, SUCCESS/non-mother/alive-stays-nonzero spine "
           "(0x00471477-0x00471a29, no mother_reelect_primary, no purge/presence-lost)");

        ck(g_corner_calls.size() == 1 && g_corner_calls[0].type_arg == s.cfg_row &&
               g_corner_calls[0].center_x == (int32_t)s.bx && g_corner_calls[0].center_y == (int32_t)s.by,
           "TU2 (0x00471477-0x0047149f): calc_placement_corner_from_center_by_type(bid, b.x, b.y)");

        ck(g_pop_remove_calls.size() == 1 && g_pop_remove_calls[0].player == s.player &&
               g_pop_remove_calls[0].count == s.human_count,
           "TU2 (0x004714ad-0x004714ca): population_remove(player, cfg_units[equivalent].human)");

        ck(g_unit_create_calls.size() == 1 && g_unit_create_calls[0].x == (uint32_t)s.corner_tile_x &&
               g_unit_create_calls[0].y == (uint32_t)s.corner_tile_y &&
               g_unit_create_calls[0].unit == (uint16_t)s.equivalent &&
               g_unit_create_calls[0].player == s.player && g_unit_create_calls[0].is_ship == 2,
           "TU2 (0x004714cf-0x004714f8): unit_create(corner_x, corner_y, equivalent, player, is_ship=2 "
           "LITERAL -- not truthy 1)");

        unit &nu = fx.u(s.player, (int32_t)s.new_index);
        ck_eq((uint32_t)nu.shuttle_slot, (uint32_t)s.shuttle_slot,
              "TU2 (0x0047150a-0x00471532): nu.shuttle_slot = b.shuttle_slot");
        ck_eq((uint32_t)nu.move_heading, (uint32_t)s.door_route10,
              "TU2 (0x00471532-0x00471563): nu.move_heading = cfg_buildings[bid].door_approach_route[10]");
        ck_eq((uint32_t)nu.facing_target, (uint32_t)s.facing_sentinel,
              "TU2 (0x00471563-0x004715e6): nu.facing_target = move_microsteps[old_heading][old_microstep]"
              ".facing (read using the PRE-overwrite move_microstep)");
        ck_eq((uint32_t)nu.facing_current, (uint32_t)s.facing_sentinel,
              "TU2 (0x004715e6-0x00471629): nu.facing_current = SAME source byte as facing_target");
        ck_eq((uint32_t)nu.move_microstep, (uint32_t)MOVE_MICROSTEP_FULL,
              "TU2 (0x0047163f-0x00471649): nu.move_microstep = 0x1f, written AFTER the facing lookup "
              "used the OLD value");

        ck_eq((uint32_t)nu.elevation, (uint32_t)s.ambient_elevation,
              "TU2 UNCERTAINTY(2) PINNED (0x00471649-0x00471694): nu.elevation = "
              "cfg_units[units[cur_player][cur_index].unit_proto_id].elevation_2 -- the AMBIENT roster "
              "slot (same index this fn uses for the BUILDING roster), NOT cfg_units[equivalent] "
              "and NOT the new unit's own natural elevation");
        ck(nu.elevation != s.equivalent_elevation,
           "TU2 UNCERTAINTY(2) NEGATIVE CHECK: nu.elevation is NOT cfg_units[equivalent].elevation_2 "
           "-- a 'corrected' implementation reading equivalent's own elevation would fail this");
        ck(nu.elevation != s.ambient_wrong_field,
           "TU2 FIELD-NEIGHBOUR CHECK (0x00471688 reads cfg offset 0x19f): nu.elevation is NOT "
           "cfg_units[ambient].elevation (0x19b), the field next door. This is the assertion whose "
           "absence let the swap live in the .cpp, the header banner AND this test at once");

        ck_eq((uint32_t)nu.state, (uint32_t)UNIT_STATE_JUST_DEPLOYED,
              "TU2 (0x00471694-0x004716b3): nu.state = 0x7c");
        ck_eq((uint32_t)nu.order, (uint32_t)UNIT_ORDER_AWAY_FROM_DEST,
              "TU2 (0x004716b3-0x00471734): nu.order = 0x31 (dest_planet -1 != planet_index 2, "
              "AWAY_FROM_DEST arm)");

        prod_shuttle_slot &slot = fx.prod_shuttle_slots[(size_t)s.player * PROD_SHUTTLE_SLOTS_PER_PLAYER +
                                                        (size_t)s.shuttle_slot];
        ck_eq((uint32_t)slot.type_ref_id, (uint32_t)(uint16_t)s.equivalent,
              "TU2 (0x00471734-0x0047177e): prod_shuttle_slot.type_ref_id = nu.unit_proto_id");
        ck_eq((uint32_t)(uint16_t)slot.status, (uint32_t)(uint16_t)SHUTTLE_SLOT_STATUS_UNIT_DEPLOYED,
              "TU2 (0x0047177e-0x004717a3): prod_shuttle_slot.status = 0xcb (undocumented in the field's "
              "own comment -- see the header's DECLARED NEED)");
        ck_eq((uint32_t)(uint16_t)slot.src_building_index, s.new_index,
              "TU2 (0x004717a3-0x004717c7): prod_shuttle_slot.src_building_index = new_index");

        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_unit[s.planet], 0u,
              "TU2: non-mother type -- primary_mother_unit UNTOUCHED (mother gate at 0x004718de-0x0047191d"
              " never taken)");
        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_bldg[s.planet], 0u,
              "TU2: non-mother type -- primary_mother_bldg UNTOUCHED");
        ck(g_reelect_calls.empty(), "TU2: non-mother type -- mother_reelect_primary NEVER called");

        ck_eq_d(fx.b(s.player, s.cur_index).energy, 0.0,
                "TU2 (0x004719be-0x004719ca): b.energy zeroed (the DOUBLE two-dword-store idiom)");
        ck_eq_d(fx.b(s.player, 0).energy, s.header_energy_seed - 1.0,
                "TU2 (0x004719d1-0x004719ea): buildings[player][0].energy += "
                "BLDG_TO_UNIT_HEADER_ROW_ENERGY_DELTA(-1.0) -- the per-player HEADER ROW, slot 0, "
                "regardless of which building (cur_index=1) converted");

        ck(g_notify_removed_calls.size() == 1 &&
               g_notify_removed_calls[0].flags == (s.player | mh::ai::REF_BLDG_BIT) &&
               g_notify_removed_calls[0].object_index == (uint32_t)s.cur_index &&
               g_notify_removed_calls[0].hard_remove == 0,
           "TU2 (0x004719f0-0x00471a04): ai_notify_object_removed(player|REF_BLDG_BIT(0x40), cur_index, "
           "hard_remove=0)");
        ck(g_unmap_calls.size() == 1 && g_unmap_calls[0].player == s.player &&
               g_unmap_calls[0].index == s.cur_index,
           "TU2 (0x00471a24): bldg_unmap_footprint(player, cur_index)");

        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet],
              (uint32_t)(s.buildings_alive_seed - 1),
              "TU2 (0x00471a29-0x00471a40): buildings_alive[planet] -= 1");
        ck(g_purge_calls.empty() && g_presence_calls.empty(),
           "TU2 (0x00471a5d-0x00471a64): buildings_alive stayed nonzero (5-1=4) -- NO purge/presence-lost");

        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, (uint32_t)BLDG_STATE_RUBBLE_SIGHT_DECAY,
              "TU2 (0x00471a80-0x00471a8b): b.state = RUBBLE_SIGHT_DECAY(4)");
        {
            int32_t anim0 = -1;
            std::memcpy(&anim0, &fx.b(s.player, s.cur_index).anim[0], sizeof(anim0));
            ck_eq((uint32_t)anim0, 0u, "TU2 (0x00471a8b-0x00471a9a): b.anim[0] cleared to 0");
        }
    }

    // =================================================================================================
    // TU3 -- SUCCESS, non-mother, AT_DEST order arm: dest_planet MATCHES planet_index -> UNIT_STATE_
    // IDLE_SCATTER(0x13), the arm at LAB_00471715 (0x00471715-0x00471734) instead of LAB_00471734's
    // sibling literal 0x31.
    // =================================================================================================
    {
        Seed s;
        s.slot_dest_planet = s.planet; // now MATCHES -> "at destination planet" arm
        seed_and_run(fx, s);

        unit &nu = fx.u(s.player, (int32_t)s.new_index);
        ck_eq((uint32_t)nu.order, (uint32_t)UNIT_STATE_IDLE_SCATTER,
              "TU3 (0x004716e5-0x00471715): dest_planet == planet_index -- nu.order = "
              "UNIT_STATE_IDLE_SCATTER(0x13), the LAB_00471715 arm");
    }

    // =================================================================================================
    // TU4 -- SUCCESS, buildings_alive[planet] reaches exactly ZERO after the decrement: unit_purge_
    // unregistered then player_presence_lost(player, 0) fire (0x00471a66-0x00471a80).
    // =================================================================================================
    {
        Seed s;
        s.buildings_alive_seed = 1; // -> 0 after the decrement
        seed_and_run(fx, s);

        ck(trace_eq({"calc_placement_corner_from_center_by_type", "population_remove", "unit_create",
                     "ai_notify_object_removed", "bldg_unmap_footprint", "unit_purge_unregistered",
                     "player_presence_lost"}),
           "TU4: buildings_alive hits 0 -- purge+presence-lost fire, ordered right after "
           "bldg_unmap_footprint (0x00471a5d-0x00471a80)");
        ck_eq((uint32_t)fx.profiles[s.player].buildings_alive[s.planet], 0u,
              "TU4: buildings_alive[planet] == 0");
        ck(g_purge_calls.size() == 1 && g_purge_calls[0] == s.player,
           "TU4 (0x00471a6d): unit_purge_unregistered(player)");
        ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == s.player &&
               g_presence_calls[0].mode == 0,
           "TU4 (0x00471a7b): player_presence_lost(player, mode=0)");
    }

    // =================================================================================================
    // TU5a -- FAILURE (unit_create returns 0), SIM_ACTIVE=0, non-mother: "dies instead". Corner
    // REASSIGNED tile->fine in place (0x004717d3-0x004717f8); explosion sound SUPPRESSED by SIM_ACTIVE
    // ==0; fx_anim_spawn fires UNCONDITIONALLY; shuttle slot cleared; shared tail still runs.
    // =================================================================================================
    {
        Seed s;
        s.new_index  = 0; // FAILURE
        s.sim_active = 0;
        seed_and_run(fx, s);

        ck(trace_eq({"calc_placement_corner_from_center_by_type", "population_remove", "unit_create",
                     "fx_anim_spawn", "ai_notify_object_removed", "bldg_unmap_footprint"}),
           "TU5a: FAILURE/sim-inactive/non-mother spine -- NO snd_play_at, fx_anim_spawn "
           "still fires (0x004717d3-0x00471a29)");

        const int32_t fine_x = (int32_t)(((uint32_t)(s.corner_tile_x << 5) + 0x10u) & s.bw_mask);
        const int32_t fine_y = (int32_t)(((uint32_t)(s.corner_tile_y << 5) + 0x10u) & s.bh_mask);
        ck(g_snd_play_at_calls.empty(),
           "TU5a (0x004717fb-0x00471802): SIM_ACTIVE==0 -- the explosion sound record does not fire");
        ck(g_anim_spawn_calls.size() == 1, "TU5a: fx_anim_spawn called exactly once");
        if (g_anim_spawn_calls.size() == 1) {
            const auto &a = g_anim_spawn_calls[0];
            ck(a.x == (uint32_t)fine_x && a.y == (uint32_t)fine_y,
               "TU5a (0x004717d3-0x004717f8): fx_anim_spawn coords = FINE-space, "
               "((tile<<5)+0x10)&bw/bh_mask -- tile(50,60) -> fine(1616,1936)");
            ck_eq(a.frame, (uint32_t)s.anim_explo,
                  "TU5a (0x00471867-0x0047188c): fx_anim_spawn frame = cfg_units[equivalent].anim_explo "
                  "(the unit's OWN anim, not the building's)");
            ck_eq_d(a.elapsed, s.game_clock, "TU5a: fx_anim_spawn elapsed = *v.game_clock (no tick_budget"
                                             " subtraction, unlike the destroyed sibling)");
            ck_eq(a.flag, 1u, "TU5a: fx_anim_spawn flag = 1");
        }

        prod_shuttle_slot &slot = fx.prod_shuttle_slots[(size_t)s.player * PROD_SHUTTLE_SLOTS_PER_PLAYER +
                                                        (size_t)s.shuttle_slot];
        ck_eq((uint32_t)slot.type_ref_id, 0u,
              "TU5a (0x00471891-0x004718b2): prod_shuttle_slot.type_ref_id cleared to 0 (was seeded "
              "nonzero) -- the transfer is abandoned");

        // Shared tail still runs on the failure arm.
        ck_eq_d(fx.b(s.player, s.cur_index).energy, 0.0, "TU5a: b.energy zeroed even on FAILURE (shared tail)");
        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, (uint32_t)BLDG_STATE_RUBBLE_SIGHT_DECAY,
              "TU5a: b.state = RUBBLE_SIGHT_DECAY(4) even on FAILURE");
    }

    // =================================================================================================
    // TU5b -- FAILURE, SIM_ACTIVE=1: the explosion-sound record (the original offscreen_snd_volume
    // -> snd_play pair, 0x0047180d-0x00471855) fires BEFORE fx_anim_spawn, using the unit's OWN
    // sound -- not any field on the building.
    // =================================================================================================
    {
        Seed s;
        s.new_index  = 0; // FAILURE
        s.sim_active = 1;
        seed_and_run(fx, s);

        ck(trace_eq({"calc_placement_corner_from_center_by_type", "population_remove", "unit_create",
                     "snd_play_at", "fx_anim_spawn", "ai_notify_object_removed",
                     "bldg_unmap_footprint"}),
           "TU5b: FAILURE/sim-active spine -- explosion sound BEFORE fx_anim_spawn");

        // fine_to_tile((tile<<5)+0x10) truncates back to `tile` for tile*32+16 < mask -- round-trip
        // sanity on the SAME conversion TU5a pins directly.
        ck(g_snd_play_at_calls.size() == 1 && g_snd_play_at_calls[0].id == s.sound_explo &&
               g_snd_play_at_calls[0].col == s.corner_tile_x &&
               g_snd_play_at_calls[0].row == s.corner_tile_y,
           "TU5b (0x0047180d-0x00471855): snd_play_at(cfg_units[equivalent].sound_explo -- the "
           "UNIT's own explosion sound, per its cfg row -- fine_to_tile(fine_x), "
           "fine_to_tile(fine_y)) round-trips to the original tile coords");
    }

    // =================================================================================================
    // TU6 -- A_MOTHER type, SUCCESS path, primary_mother_bldg[planet] == cur_index (the gate matches):
    // primary_mother_unit[planet] takes new_index; primary_mother_bldg[planet] clears; mother_reelect_
    // primary is called with the STILL-TILE corner (the success arm never rewrites corner_x/y) --
    // UNCERTAINTY (1) pinned for the SUCCESS side.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type                = BUILDING_TYPE_A_MOTHER;
        s.primary_mother_unit_seed = 0;           // gate condition (==0) TRUE -> gets written
        s.primary_mother_bldg_seed = s.cur_index; // gate condition (==cur_index) TRUE -> cleared+reelect
        seed_and_run(fx, s);

        ck(trace_eq({"calc_placement_corner_from_center_by_type", "population_remove", "unit_create",
                     "mother_reelect_primary", "ai_notify_object_removed", "bldg_unmap_footprint"}),
           "TU6: mother_reelect_primary sits in the SHARED TAIL, BEFORE ai_notify_object_removed "
           "(0x004719ac-0x004719b9, then LAB_004719be at 0x004719be)");

        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_unit[s.planet], s.new_index,
              "TU6 (0x0047195c-0x0047195f): primary_mother_unit[planet] = new_index (gate was ==0)");
        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_bldg[s.planet], 0u,
              "TU6 (0x0047199c-0x004719a2): primary_mother_bldg[planet] cleared to 0");

        ck(g_reelect_calls.size() == 1 && g_reelect_calls[0].player == (int32_t)s.player,
           "TU6: mother_reelect_primary(player, ...) called exactly once");
        if (g_reelect_calls.size() == 1) {
            ck(g_reelect_calls[0].x == s.corner_tile_x && g_reelect_calls[0].y == s.corner_tile_y,
               "TU6 UNCERTAINTY(1) PINNED, SUCCESS SIDE (0x004719ac-0x004719b9): mother_reelect_primary "
               "receives TILE coords (corner_x/y never rewritten on the success arm)");
        }
    }

    // =================================================================================================
    // TU7 -- A_MOTHER type, FAILURE path, primary_mother_bldg[planet] == cur_index: mother_reelect_
    // primary is called with the FINE (pixel, ~32x) corner the failure arm reassigned in place --
    // UNCERTAINTY (1) pinned for the FAILURE side, the case that catches a "corrected" implementation
    // that always passes tile coords. primary_mother_unit_seed is NONZERO here so the (==0) gate is
    // FALSE and the slot is provably left alone (not just coincidentally still 0).
    // =================================================================================================
    {
        Seed s;
        s.new_index                = 0; // FAILURE
        s.bldg_type                = BUILDING_TYPE_A_MOTHER;
        s.primary_mother_unit_seed = 777; // nonzero -> gate (==0) FALSE -> must stay 777
        s.primary_mother_bldg_seed = s.cur_index;
        seed_and_run(fx, s);

        const int32_t fine_x = (int32_t)(((uint32_t)(s.corner_tile_x << 5) + 0x10u) & s.bw_mask);
        const int32_t fine_y = (int32_t)(((uint32_t)(s.corner_tile_y << 5) + 0x10u) & s.bh_mask);

        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_unit[s.planet], 777u,
              "TU7 (0x0047195c-0x0047195f): primary_mother_unit[planet] UNCHANGED -- new_index==0 but "
              "the slot started NONZERO so the (==0) gate is FALSE, not a coincidental no-op");
        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_bldg[s.planet], 0u,
              "TU7: primary_mother_bldg[planet] cleared to 0 -- this gate is independent of the one above");

        ck(g_reelect_calls.size() == 1, "TU7: mother_reelect_primary called exactly once");
        if (g_reelect_calls.size() == 1) {
            ck(g_reelect_calls[0].x == fine_x && g_reelect_calls[0].y == fine_y,
               "TU7 UNCERTAINTY(1) PINNED, FAILURE SIDE (0x004717d3-0x004717f8 feeds 0x004719ac-"
               "0x004719b9): mother_reelect_primary receives FINE (pixel) coords, ~32x the tile "
               "values -- a 'corrected' implementation using tile coords here would disagree");
        }
    }

    // =================================================================================================
    // TU8 -- H_MOTHER(0x1a) takes the SAME mother arm as A_MOTHER(0x06) -- the type gate at
    // 0x004718de-0x00471923 checks EITHER cfg value.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type                = BUILDING_TYPE_H_MOTHER;
        s.primary_mother_bldg_seed = s.cur_index;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.profiles[s.player].primary_mother_bldg[s.planet], 0u,
              "TU8 (0x004718e4-0x00471923): H_MOTHER(0x1a) takes the same mother-election arm as "
              "A_MOTHER(0x06)");
        ck(g_reelect_calls.size() == 1, "TU8: mother_reelect_primary fires for H_MOTHER too");
    }

    // =================================================================================================
    // TU9 -- the header-row energy delta is EXACTLY -1.0 on buildings[player][0], regardless of which
    // building/cur_index/shuttle_slot converted, on the FAILURE arm with a DIFFERENT building/slot
    // than TU2/TU5 used (cross-checks 0x004719d1-0x004719ea's "always slot 0" claim a second way).
    // =================================================================================================
    {
        Seed s;
        s.new_index          = 0; // FAILURE arm this time
        s.cur_index          = 6;
        s.cfg_row            = 21;
        s.equivalent         = 8;
        s.shuttle_slot       = 7;
        s.header_energy_seed = 12.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, 0).energy, 11.0,
                "TU9 (0x004719d1-0x004719ea): buildings[player][0].energy == 12.0 + (-1.0) == 11.0, "
                "with a DIFFERENT cur_index/cfg_row/shuttle_slot than TU2/TU5 -- confirms the target is "
                "ALWAYS roster slot 0, not `cur_index`'s own building");
        ck_eq_d(fx.b(s.player, s.cur_index).energy, 0.0,
                "TU9: the CONVERTING building's own energy (cur_index=6) is zeroed, not decremented by "
                "the header-row delta");
    }
}

} // namespace mh::sim::test
