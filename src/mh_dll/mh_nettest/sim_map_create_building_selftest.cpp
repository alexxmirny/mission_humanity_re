//
// sim_map_create_building_selftest.cpp -- `simtest` offline oracle for map_CreateBuilding
// (sim/sim_map_create_building.{h,cpp}, RI-SIM / SIM1F batch F). map_CreateBuilding is the
// building-record CREATION primitive: it zero-fills a new buildings[player][index] record, stamps its
// scalar fields, walks the 10x10 cfg footprint mask into tile_objects/passable, registers sight +
// map-region occupancy, inits ONE of five type-specific sub-records (production/mine/turret/storage/
// lab), does the unconditional shared-tail bookkeeping (slot-0 scratch bump + player_profile counters +
// the conditional foreign-building AI event), and finally auto-assigns construction workers.
//
// All five translated siblings it reaches (sight_add_circle / apply_area_to_map / assign_workers /
// set_staffed_flag / refresh_building) plus the two ORIGINAL callees (utils_fill_data / game_SetEvent)
// are routed through create_building_calls, so this oracle stubs every one with a recording stub and
// asserts THIS function's own writes + that each sibling fired with the right building handle/args.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/map_CreateBuilding_004622cb.asm), not
// the .cpp. Struct field names/offsets are from addr/mh_structs.gen.h (mh_map_object_building at 0x286,
// _production/_mine/_turret/_lab, mh_llm_strat_player_profile, mh_map_tile_object_data).
//
// MUTATION NOTES (perturbations these checks would catch red):
//   * b.building_id check: a translation stamping `index` where it should stamp `building_id` (the two
//     are distinct params, seeded 3 vs 5) diverges.
//   * turret +0xe/+0x12 == 0x1e checks: dropping either pad write (or writing 1 instead of 0x1e) diverges.
//   * b0 shared-tail (state+=1/energy+=1.0/index+=1): a translation that SET slot-0 to 1 instead of
//     incrementing (seeded 10/3.0/20) reads 1/1.0/1 instead of 11/4.0/21.
//   * foreign-flag `> 5` gate: seeded built_total 5 (->6, clears) vs 3 (->4, no clear); an off-by-one
//     (`>= 5`) would clear in the not-met case too.
//   * assign_workers -2 clamp: human 20 -> count 10 clamped to builder_count 7; dropping the clamp reads 10.
//
#include "sim/sim_map_create_building.h"

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_* switch constants (reused, per the .cpp)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recording stubs for every create_building_calls member -------------------------------------
struct FillCall {
    void    *ptr;
    uint32_t size;
    uint8_t  def;
};
struct SightCall {
    uint32_t player;
    int32_t  x, y, building_id;
    uint8_t  sight;
};
struct AreaCall {
    int32_t x, y;
    void   *area;
};
struct AssignCall {
    uint32_t player, building_id;
    int32_t  count;
};
struct StaffCall {
    uint16_t player;
    int32_t  idx;
};
struct RefreshCall {
    uint16_t p;
    int32_t  b;
};

std::vector<FillCall>    g_fill;
std::vector<uint32_t>    g_event;
std::vector<SightCall>   g_sight;
std::vector<AreaCall>    g_area;
std::vector<AssignCall>  g_assign;
std::vector<StaffCall>   g_staff;
std::vector<RefreshCall> g_refresh;

// When set, stub_assign writes current_workers into this record -- used to reach the
// `b.current_workers != 0` disjunct of the set_staffed gate (the live callee is what writes it).
building *g_assign_target         = nullptr;
uint16_t  g_assign_workers_writes = 0;

void *stub_fill(void *ptr, uint32_t size, uint8_t def) {
    g_fill.push_back({ptr, size, def});
    memset(ptr, def, size); // faithful: the body relies on the whole record being zeroed first
    return ptr;
}
uint32_t stub_event(uint32_t type) {
    g_event.push_back(type);
    return 0;
}
void stub_sight(uint32_t player, int32_t x, int32_t y, int32_t bid, uint8_t sight) {
    g_sight.push_back({player, x, y, bid, sight});
}
void    stub_area(int32_t x, int32_t y, uint8_t *area) { g_area.push_back({x, y, area}); }
int32_t stub_assign(uint32_t player, uint32_t bid, int32_t count) {
    g_assign.push_back({player, bid, count});
    if (g_assign_target != nullptr) g_assign_target->current_workers = g_assign_workers_writes;
    return 0;
}
void stub_staff(uint16_t player, int32_t idx) { g_staff.push_back({player, idx}); }
void stub_refresh(uint16_t p, int32_t b) { g_refresh.push_back({p, b}); }

const create_building_calls g_calls = {
    stub_fill,
    stub_event,
    stub_sight,
    stub_area,
    stub_assign,
    stub_staff,
    stub_refresh,
};

void clear_calls() {
    g_fill.clear();
    g_event.clear();
    g_sight.clear();
    g_area.clear();
    g_assign.clear();
    g_staff.clear();
    g_refresh.clear();
    g_assign_target         = nullptr;
    g_assign_workers_writes = 0;
}

// little-endian 4-byte read over a raw uint8_t[] span (matches the .cpp's flattened-array helper).
uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void run(sim_fixture &fx, uint16_t player, uint32_t index, uint32_t x_b, int32_t y_b,
         int32_t building_id, uint32_t param_6, uint32_t sub_id) {
    clear_calls();
    sim_store own = fx.store();
    detail::create_building(fx.view(), own, g_calls, player, index, x_b, y_b, building_id, param_6,
                            sub_id);
}

constexpr double CLK = 1234.5; // distinct nonzero game clock, so last_tick_time/anim_dur/pip_timer bite

} // namespace

void run_map_create_building_tests() {
    sim_fixture fx;

    // ================================================================================================
    // CASE 1 -- generic (default-arm) building: every scalar record field + shared tail + all siblings.
    // Type A_PLANT (0x03) hits the switch default (no sub-record). builder_count 0 -> set_staffed fires
    // via the first disjunct, assign does NOT fire. Footprint mask left all-zero (tile writes covered
    // in CASE 5); the two unconditional siblings still fire.
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock   = CLK;
        fx.player_side  = 7;
        fx.planet_index = 0; // planet 0 -> foreign-event gate (planet>3) never fires here

        const uint16_t player = 2;
        const uint32_t index  = 5;
        const uint32_t x_b    = 20;
        const int32_t  y_b    = 9;
        const int32_t  bid    = 3;
        const uint32_t sub_id = 4;

        cfg_building &cfg = fx.cfg_buildings[bid];
        cfg.type          = BUILDING_TYPE_A_PLANT; // 0x03 -> default arm
        cfg.energy        = 77.5;
        cfg.sight         = 6;
        cfg.builder_count = 0;
        cfg.anim[0]       = 0x11;
        cfg.anim[1]       = 0x22;
        cfg.anim[2]       = 0x33;
        cfg.anim[3]       = 0x44; // LE dword 0x44332211

        // slot-0 scratch (distinct from the new record so the increment vs set is observable)
        building &b0 = fx.b(player, 0);
        b0.state     = 10;
        b0.energy    = 3.0;
        b0.index     = 20;

        // profile counters + foreign flag
        fx.profiles[player].buildings_alive[0]       = 30;
        fx.profiles[player].buildings_built_total[0] = 40;
        fx.foreign_bldg_event_pending                = 1;

        run(fx, player, index, x_b, y_b, bid, 0u, sub_id);

        // ---- utils_fill_data: whole record zeroed via the original callee ----
        ck(g_fill.size() == 1 && g_fill[0].ptr == (void *)&fx.b(player, (int32_t)index),
           "C1 fill: called once on &buildings[player][index]");
        ck(g_fill.size() == 1 && g_fill[0].size == (uint32_t)sizeof(building) && g_fill[0].def == 0,
           "C1 fill: size == sizeof(building), fill byte 0");

        // ---- the new record's own scalar fields ----
        building &b = fx.b(player, (int32_t)index);
        ck_eq((uint32_t)(uint16_t)b.index, index, "C1 b.index == index");
        ck_eq((uint32_t)b.building_id, (uint32_t)bid, "C1 b.building_id == building_id");
        ck_eq((uint32_t)b.state, 0x64u, "C1 b.state == CONSTRUCTION(0x64)");
        ck_eq_d(b.cycle_progress, 0.0, "C1 b.cycle_progress == 0.0");
        ck_eq((uint32_t)(uint16_t)b.online_state, 0u, "C1 b.online_state == 0");
        ck_eq((uint32_t)b.built_flags, 0u, "C1 b.built_flags == 0");
        ck_eq_d(b.last_tick_time, CLK, "C1 b.last_tick_time == game_clock");
        ck_eq_d(b.energy, 77.5, "C1 b.energy == cfg.energy");
        ck_eq_d(b.pending_damage, 0.0, "C1 b.pending_damage == 0.0");
        ck_eq((uint32_t)b.shuttle_slot, 0u, "C1 b.shuttle_slot == 0");
        ck_eq((uint32_t)(uint16_t)b.incoming_damage_tally, 0u, "C1 b.incoming_damage_tally == 0");
        ck_eq((uint32_t)b.sub_id, sub_id, "C1 b.sub_id == sub_id");
        ck_eq((uint32_t)b.x, x_b, "C1 b.x == x_b");
        ck_eq((uint32_t)b.y, (uint32_t)y_b, "C1 b.y == y_b");
        ck_eq((uint32_t)b.current_workers, 0u, "C1 b.current_workers == 0");

        // ---- anim / anim_dur / pip triple ----
        ck_eq_d(b.anim_dur[0], CLK, "C1 b.anim_dur[0] == game_clock");
        ck_eq_d(b.anim_dur[11], CLK, "C1 b.anim_dur[11] == game_clock");
        ck_eq(ld32(&b.anim[0]), 0x44332211u, "C1 b.anim[0] re-seeded from cfg.anim[0]");
        ck_eq(ld32(&b.anim[4]), 0u, "C1 b.anim[1] stays zero (only slot 0 re-seeded)");
        ck_eq((uint32_t)b.pip_active_count, 0u, "C1 b.pip_active_count == 0");
        ck_eq((uint32_t)b.pip_frame[0], 0u, "C1 b.pip_frame[0] == 0");
        ck_eq((uint32_t)b.pip_level[0], 0u, "C1 b.pip_level[0] == 0");
        ck_eq_d(b.pip_timer[0], CLK, "C1 b.pip_timer[0] == game_clock");

        // ---- shared-tail slot-0 scratch bump (increment, NOT set) ----
        ck_eq((uint32_t)b0.state, 11u, "C1 b0.state incremented 10->11");
        ck_eq_d(b0.energy, 4.0, "C1 b0.energy incremented 3.0->4.0");
        ck_eq((uint32_t)(uint16_t)b0.index, 21u, "C1 b0.index incremented 20->21");

        // ---- profile counters ----
        ck_eq((uint32_t)fx.profiles[player].buildings_alive[0], 31u, "C1 buildings_alive[planet] 30->31");
        ck_eq((uint32_t)fx.profiles[player].buildings_built_total[0], 41u,
              "C1 buildings_built_total[planet] 40->41");

        // ---- foreign flag NOT cleared (planet 0 fails the planet>3 gate) ----
        ck_eq((uint32_t)fx.foreign_bldg_event_pending, 1u, "C1 foreign flag untouched (planet<=3)");

        // ---- game_SetEvent ----
        ck(g_event.size() == 1 && g_event[0] == CREATE_BUILDING_MAP_OBJECTS_REFRESH,
           "C1 set_event(MAP_OBJECTS_REFRESH=14) fired once");

        // ---- siblings ----
        ck(g_sight.size() == 1 && g_sight[0].player == player && g_sight[0].x == (int32_t)x_b &&
               g_sight[0].y == y_b && g_sight[0].building_id == bid && g_sight[0].sight == 6,
           "C1 sight_add_circle(player,x_b,y_b,building_id,cfg.sight)");
        ck(g_area.size() == 1 && g_area[0].x == (int32_t)x_b && g_area[0].y == y_b &&
               g_area[0].area == (void *)&fx.cfg_buildings[bid].area[0][0],
           "C1 apply_area_to_map(x_b,y_b,&cfg.area)");
        ck(g_assign.empty(), "C1 assign_workers NOT fired (builder_count==0)");
        ck(g_staff.size() == 1 && g_staff[0].player == player && g_staff[0].idx == (int32_t)index,
           "C1 set_staffed_flag(player,index) fired (builder_count==0)");
        ck(g_refresh.size() == 1 && g_refresh[0].p == player && g_refresh[0].b == (int32_t)index,
           "C1 refresh_building(player,index) fired last");
    }

    // ================================================================================================
    // CASE 2 -- lab-kind sub-record (A_LAB 0x0b -> caseD_b).
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 3;
        const uint32_t index  = 6;
        const int32_t  bid    = 12;
        const uint32_t sub_id = 6;

        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_LAB; // 0x0b
        fx.cfg_buildings[bid].builder_count = 0;

        lab &l0                = fx.labs[player * LABS_PER_PLAYER + 0];
        l0.b_index             = 50;
        lab &lsub              = fx.labs[player * LABS_PER_PLAYER + sub_id];
        lsub.active_project_id = 99; // must be zeroed
        lsub.b_index           = -7; // must be overwritten with index

        run(fx, player, index, 30u, 8, bid, 0u, sub_id);

        ck_eq((uint32_t)fx.labs[player * LABS_PER_PLAYER + 0].b_index, 51u, "C2 lab slot0.b_index 50->51");
        ck_eq((uint32_t)fx.labs[player * LABS_PER_PLAYER + sub_id].b_index, index,
              "C2 lab[sub_id].b_index == index");
        ck_eq((uint32_t)fx.labs[player * LABS_PER_PLAYER + sub_id].active_project_id, 0u,
              "C2 lab[sub_id].active_project_id == 0");
        ck(g_staff.size() == 1 && g_staff[0].idx == (int32_t)index, "C2 set_staffed_flag(index)");
        ck(g_refresh.size() == 1 && g_refresh[0].b == (int32_t)index, "C2 refresh_building(index)");
        ck(g_sight.size() == 1 && g_sight[0].building_id == bid, "C2 sight_add_circle(building_id)");
    }

    // ================================================================================================
    // CASE 3 -- mine-kind sub-record (A_MINE 0x02 -> caseD_2).
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 1;
        const uint32_t index  = 8;
        const int32_t  bid    = 2;
        const uint32_t sub_id = 7;

        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_MINE; // 0x02
        fx.cfg_buildings[bid].builder_count = 0;

        fx.mines[player * MINES_PER_PLAYER + 0].b_index      = 60;
        fx.mines[player * MINES_PER_PLAYER + sub_id].b_index = -3; // overwritten with index

        run(fx, player, index, 40u, 11, bid, 0u, sub_id);

        ck_eq((uint32_t)fx.mines[player * MINES_PER_PLAYER + 0].b_index, 61u, "C3 mine slot0.b_index 60->61");
        ck_eq((uint32_t)fx.mines[player * MINES_PER_PLAYER + sub_id].b_index, index,
              "C3 mine[sub_id].b_index == index");
        ck(g_staff.size() == 1 && g_staff[0].idx == (int32_t)index, "C3 set_staffed_flag(index)");
        ck(g_refresh.size() == 1 && g_refresh[0].b == (int32_t)index, "C3 refresh_building(index)");
    }

    // ================================================================================================
    // CASE 4 -- turret-kind sub-record (A_TURRET 0x05 -> caseD_5), armed weapon path.
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 2;
        const uint32_t index  = 9;
        const int32_t  bid    = 5;
        const uint32_t sub_id = 9;

        cfg_building &cfg                   = fx.cfg_buildings[bid];
        cfg.type                            = BUILDING_TYPE_A_TURRET; // 0x05
        cfg.builder_count                   = 0;
        cfg.sight                           = 8;
        cfg.weapon_id                       = 3;     // low byte 3, within the 32-entry weapon table
        fx.cfg_weapons[3].range_max[player] = 0x123; // attack_range source

        fx.turrets[player * TURRETS_PER_PLAYER + 0].b_index = 40; // int16 slot-0 counter (WORD inc)

        run(fx, player, index, 50u, 12, bid, 0u, sub_id);

        turret &t0 = fx.turrets[player * TURRETS_PER_PLAYER + 0];
        turret &t  = fx.turrets[player * TURRETS_PER_PLAYER + sub_id];
        ck_eq((uint32_t)(uint16_t)t0.b_index, 41u, "C4 turret slot0.b_index 40->41");
        ck_eq((uint32_t)(uint16_t)t.b_index, index, "C4 turret[sub_id].b_index == index");
        // boot-field writes (byte offsets read straight off the asm; +0x1c/+0x25 named 2026-08-22 --
        // this test is what independently corroborated cached_sight and resupply_available)
        ck_eq((uint32_t)t.cached_sight, 8u, "C4 turret.cached_sight (+0x1c) == cfg.sight");
        ck_eq((uint32_t)t.aim_heading, 1u, "C4 turret +0x2 aim_heading == 1");
        ck_eq((uint32_t)t.aim_heading_boot_scratch, 1u, "C4 turret +0x6 aim_heading_boot_scratch == 1");
        ck_eq((uint32_t)t.aim_step_dir, 1u, "C4 turret +0xa aim_step_dir == 1");
        ck_eq((uint32_t)t.acquire_retry_seed, 0x1eu, "C4 turret +0xe acquire_retry_seed == 0x1e");
        ck_eq((uint32_t)t.acquire_retry_counter, 0x1eu, "C4 turret +0x12 acquire_retry_counter == 0x1e");
        // armed-weapon branch
        ck_eq((uint32_t)t.weapon_id, 3u, "C4 turret.weapon_id == cfg.weapon_id low byte");
        ck_eq((uint32_t)t.resupply_available, 1u, "C4 turret.resupply_available (+0x25) == 1 (armed)");
        ck_eq((uint32_t)t.reload_ready_flag, 1u, "C4 turret.reload_ready_flag == 1 (armed)");
        ck_eq((uint32_t)t.attack_range, 0x123u, "C4 turret.attack_range == Weapon[weapon_id].range_max[player]");
    }

    // ---- CASE 4b -- turret with weapon_id 0: unarmed branch (attack_range 0, no +0x25/reload) --------
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 2;
        const uint32_t index  = 4;
        const int32_t  bid    = 5;
        const uint32_t sub_id = 10;

        cfg_building &cfg = fx.cfg_buildings[bid];
        cfg.type          = BUILDING_TYPE_A_TURRET;
        cfg.builder_count = 0;
        cfg.sight         = 8;
        cfg.weapon_id     = 0; // unarmed

        run(fx, player, index, 50u, 12, bid, 0u, sub_id);

        turret &t = fx.turrets[player * TURRETS_PER_PLAYER + sub_id];
        ck_eq((uint32_t)t.weapon_id, 0u, "C4b turret.weapon_id == 0");
        ck_eq((uint32_t)t.attack_range, 0u, "C4b turret.attack_range == 0 (unarmed)");
        ck_eq((uint32_t)t.resupply_available, 0u, "C4b turret.resupply_available (+0x25) untouched (unarmed)");
        ck_eq((uint32_t)t.reload_ready_flag, 0u, "C4b turret.reload_ready_flag untouched (unarmed)");
    }

    // ================================================================================================
    // CASE 5 -- footprint stamp over the 10x10 mask: tile_objects.building/.class_owner + passable clear.
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 2; // class_owner low nibble
        const uint32_t index  = 5;
        const uint32_t x_b    = 20;
        const int32_t  y_b    = 9;
        const int32_t  bid    = 3;

        cfg_building &cfg = fx.cfg_buildings[bid];
        cfg.type          = BUILDING_TYPE_A_PLANT;
        cfg.builder_count = 0;
        // non-symmetric nonzero cells + a zero cell (i,j distinct so an axis swap is caught)
        cfg.area[1][2] = 1;
        cfg.area[7][8] = 9;
        cfg.area[0][0] = 0; // skipped

        // masks: width_mask 0xff, height_mask 0x3f (fixture geom defaults)
        // pre-seed passable at the three tiles to 1 so a clear (->0) is observable
        auto      tile = [](int tx, int ty) { return (tx << 8) | ty; };
        const int tx0 = (20 + 1) & 0xff, ty0 = (9 + 2) & 0x3f; // (21,11)
        const int tx1 = (20 + 7) & 0xff, ty1 = (9 + 8) & 0x3f; // (27,17)
        const int txz = (20 + 0) & 0xff, tyz = (9 + 0) & 0x3f; // (20,9), the skipped cell
        fx.passable[tile(tx0, ty0)] = 1;
        fx.passable[tile(tx1, ty1)] = 1;
        fx.passable[tile(txz, tyz)] = 1;

        run(fx, player, index, x_b, y_b, bid, 0u, 4u);

        const uint16_t idx16       = (uint16_t)index;
        const uint8_t  class_owner = (uint8_t)((uint8_t)player | 0x40u); // 0x42

        ck_eq((uint32_t)fx.t(tx0, ty0).building, idx16, "C5 tile(21,11).building == index");
        ck_eq((uint32_t)fx.t(tx0, ty0).class_owner, (uint32_t)class_owner, "C5 tile(21,11).class_owner == player|0x40");
        ck_eq((uint32_t)fx.passable[tile(tx0, ty0)], 0u, "C5 passable(21,11) cleared to 0");

        ck_eq((uint32_t)fx.t(tx1, ty1).building, idx16, "C5 tile(27,17).building == index");
        ck_eq((uint32_t)fx.t(tx1, ty1).class_owner, (uint32_t)class_owner, "C5 tile(27,17).class_owner == player|0x40");
        ck_eq((uint32_t)fx.passable[tile(tx1, ty1)], 0u, "C5 passable(27,17) cleared to 0");

        // the zero cell must NOT be stamped
        ck_eq((uint32_t)fx.t(txz, tyz).building, 0u, "C5 zero-mask tile(20,9).building untouched");
        ck_eq((uint32_t)fx.passable[tile(txz, tyz)], 1u, "C5 zero-mask tile(20,9).passable untouched");
    }

    // ================================================================================================
    // CASE 6 -- conditional foreign-building AI event (clears _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING).
    // Gate: player != PlayerSide && planet>3 && planet!=0x1f && built_total[planet] (post-++) > 5.
    // ================================================================================================
    {
        // (a) condition MET -> flag cleared to 0
        fx.reset();
        fx.game_clock                                = CLK;
        fx.player_side                               = 7;
        fx.planet_index                              = 4; // >3, !=0x1f
        const uint16_t player                        = 2;
        fx.cfg_buildings[3].type                     = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[3].builder_count            = 0;
        fx.profiles[player].buildings_built_total[4] = 5; // ++ -> 6 > 5
        fx.foreign_bldg_event_pending                = 1;
        run(fx, player, 5u, 20u, 9, 3, 0u, 4u);
        ck_eq((uint32_t)fx.foreign_bldg_event_pending, 0u, "C6a foreign flag cleared (gate met)");

        // (b) NOT met via built_total (4 <= 5) -> flag unchanged
        fx.reset();
        fx.game_clock                                = CLK;
        fx.player_side                               = 7;
        fx.planet_index                              = 4;
        fx.cfg_buildings[3].type                     = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[3].builder_count            = 0;
        fx.profiles[player].buildings_built_total[4] = 3; // ++ -> 4, not > 5
        fx.foreign_bldg_event_pending                = 1;
        run(fx, player, 5u, 20u, 9, 3, 0u, 4u);
        ck_eq((uint32_t)fx.foreign_bldg_event_pending, 1u, "C6b foreign flag unchanged (built_total gate)");

        // (c) NOT met via player == PlayerSide -> flag unchanged even with high built_total
        fx.reset();
        fx.game_clock                           = CLK;
        fx.player_side                          = 7;
        fx.planet_index                         = 4;
        fx.cfg_buildings[3].type                = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[3].builder_count       = 0;
        fx.profiles[7].buildings_built_total[4] = 9;
        fx.foreign_bldg_event_pending           = 1;
        run(fx, /*player==side*/ 7u, 5u, 20u, 9, 3, 0u, 4u);
        ck_eq((uint32_t)fx.foreign_bldg_event_pending, 1u, "C6c foreign flag unchanged (player==PlayerSide)");
    }

    // ================================================================================================
    // CASE 7 -- auto-assign construction workers (step 8) + the set_staffed disjuncts.
    // ================================================================================================
    {
        const uint16_t player = 2;
        const uint32_t index  = 5;
        const int32_t  bid    = 3;

        // (a) plain positive param_6: assign fires with that count; current_workers stays 0 -> set_staffed
        //     does NOT fire (builder_count!=0 && current_workers==0).
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 5; // != 0 -> gate open
        run(fx, player, index, 20u, 9, bid, /*param_6*/ 4u, 4u);
        ck(g_assign.size() == 1 && g_assign[0].player == player &&
               g_assign[0].building_id == index && g_assign[0].count == 4,
           "C7a assign_workers(player,index,4)");
        ck(g_staff.empty(), "C7a set_staffed NOT fired (builder_count!=0, current_workers==0)");
        ck(g_refresh.size() == 1 && g_refresh[0].b == (int32_t)index, "C7a refresh_building fired");

        // (a2) population human == 0 -> assign gate closed even with builder_count!=0.
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 0;
        run(fx, player, index, 20u, 9, bid, 4u, 4u);
        ck(g_assign.empty(), "C7a2 assign NOT fired (human==0)");
        ck(g_staff.empty(), "C7a2 set_staffed NOT fired (builder_count!=0, current_workers==0)");

        // (b) param_6 == -1 -> count resolves to cfg.builder_count.
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 5;
        run(fx, player, index, 20u, 9, bid, (uint32_t)-1, 4u);
        ck(g_assign.size() == 1 && g_assign[0].count == 7, "C7b param_6==-1 -> count = builder_count(7)");

        // (c) param_6 == -2, no clamp: count = half of idle human (9>>1 == 4) < builder_count(7).
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 9;
        run(fx, player, index, 20u, 9, bid, (uint32_t)-2, 4u);
        ck(g_assign.size() == 1 && g_assign[0].count == 4, "C7c param_6==-2 -> half idle human (4), no clamp");

        // (c2) param_6 == -2 WITH clamp: half of 20 == 10, clamped down to builder_count 7.
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 20;
        run(fx, player, index, 20u, 9, bid, (uint32_t)-2, 4u);
        ck(g_assign.size() == 1 && g_assign[0].count == 7, "C7c2 param_6==-2 -> 10 clamped to builder_count(7)");

        // (d) set_staffed via the current_workers!=0 disjunct: the (stubbed) assign writes current_workers.
        fx.reset();
        fx.game_clock                       = CLK;
        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PLANT;
        fx.cfg_buildings[bid].builder_count = 7;
        fx.population[player].human         = 5;
        // run() would clear_calls() (resetting the arming), so drive the body directly here after
        // arming stub_assign to write current_workers into the new record -- reaching the
        // `b.current_workers != 0` disjunct of the set_staffed gate.
        clear_calls();
        g_assign_target         = &fx.b(player, (int32_t)index);
        g_assign_workers_writes = 3;
        {
            sim_store own = fx.store();
            detail::create_building(fx.view(), own, g_calls, player, index, 20u, 9, bid, 4u, 4u);
        }
        ck(g_assign.size() == 1 && g_assign[0].count == 4, "C7d assign fired (count 4)");
        ck(g_staff.size() == 1 && g_staff[0].idx == (int32_t)index,
           "C7d set_staffed fired via current_workers!=0 disjunct");
    }

    // ================================================================================================
    // CASE 8 -- production-kind sub-record (A_PRODUCTION 0x01 -> caseD_1): slot-0 bump + queue clear.
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 2;
        const uint32_t index  = 5;
        const int32_t  bid    = 4;
        const uint32_t sub_id = 3;

        fx.cfg_buildings[bid].type          = BUILDING_TYPE_A_PRODUCTION; // 0x01
        fx.cfg_buildings[bid].builder_count = 0;

        production &p0        = fx.productions[player * PRODUCTIONS_PER_PLAYER + 0];
        p0.b_index            = 70;
        production &psub      = fx.productions[player * PRODUCTIONS_PER_PLAYER + sub_id];
        psub.active_unit_type = 55;  // must be zeroed
        psub.queued_count[3]  = 9;   // must be zeroed
        psub.queued_count[99] = 7;   // whole [0..99] range cleared
        psub.b_index          = -11; // overwritten with index

        run(fx, player, index, 20u, 9, bid, 0u, sub_id);

        ck_eq((uint32_t)fx.productions[player * PRODUCTIONS_PER_PLAYER + 0].b_index, 71u,
              "C8 production slot0.b_index 70->71");
        production &q = fx.productions[player * PRODUCTIONS_PER_PLAYER + sub_id];
        ck_eq((uint32_t)q.b_index, index, "C8 production[sub_id].b_index == index");
        ck_eq((uint32_t)q.active_unit_type, 0u, "C8 production[sub_id].active_unit_type == 0");
        ck_eq((uint32_t)q.queued_count[3], 0u, "C8 production[sub_id].queued_count[3] cleared");
        ck_eq((uint32_t)q.queued_count[99], 0u, "C8 production[sub_id].queued_count[99] cleared");
    }

    // ================================================================================================
    // CASE 9 -- storage-kind sub-record (A_BARRAKS 0x07 -> caseD_7): park_x/park_y + exit tiles.
    // The original stores each as ONE full masked dword. park_x/park_y were mistyped uint8_t + 3
    // "reserved" bytes until 2026-08-22 and are now int32_t, so a narrowing bug is no longer even
    // expressible -- but the pre-seed-nonzero setup and the paired dword assertions below are KEPT:
    // they are what would catch a regression if the width were ever narrowed again.
    // ================================================================================================
    {
        fx.reset();
        fx.game_clock         = CLK;
        const uint16_t player = 2;
        const uint32_t index  = 5;
        const uint32_t x_b    = 20;
        // y_b is deliberately LARGER than the height mask (0x3f) so that the &hmask actually bites:
        // at the old y_b=9 every y value fitted in 6 bits, so masking with wmask (0xff) instead of
        // hmask produced identical results and the park_y/exit_tile_y checks could not tell the two
        // masks apart. Mutation-verified 2026-08-22: swapping hmask->wmask now turns them red.
        const int32_t  y_b    = 70;
        const int32_t  bid    = 6;
        const uint32_t sub_id = 5;

        cfg_building &cfg        = fx.cfg_buildings[bid];
        cfg.type                 = BUILDING_TYPE_A_BARRAKS; // 0x07
        cfg.builder_count        = 0;
        cfg.park_offset_x        = 2;
        cfg.park_offset_y        = 3;
        cfg.shuttle_pad_offset_x = 4;
        cfg.shuttle_pad_offset_y = 5;

        unit_storage &s0 = fx.storage[player * STORAGE_PER_PLAYER + 0];
        s0.b_index       = 80;
        unit_storage &su = fx.storage[player * STORAGE_PER_PLAYER + sub_id];
        // pre-seed the fields the arm zeroes + the reserved spill bytes, to distinct nonzero values
        su.occupancy         = 12;
        su.docked_count      = 34;
        su.door_mutex_unit   = 56;
        su.door_waiter_count = 78;
        su.b_index           = -13;
        // Pre-seed park_x/park_y to values whose UPPER bytes are nonzero. Before EN v313 these were
        // uint8_t + reserved_0xdd/reserved_0xe1 and this seeded the reserved bytes directly; now they
        // are int32_t, and seeding the whole dword still does the same job -- if the store ever
        // narrowed to a byte, the upper bytes would survive and the ld32 checks below would fail.
        su.park_x = (int32_t)0xccbbaa00;
        su.park_y = (int32_t)0x00dd0000;

        run(fx, player, index, x_b, y_b, bid, 0u, sub_id);

        const uint32_t px = (x_b + 2u) & 0xffu;           // 22
        const uint32_t py = ((uint32_t)y_b + 3u) & 0x3fu; // 12

        ck_eq((uint32_t)fx.storage[player * STORAGE_PER_PLAYER + 0].b_index, 81u,
              "C9 storage slot0.b_index 80->81");
        unit_storage &s = fx.storage[player * STORAGE_PER_PLAYER + sub_id];
        ck_eq((uint32_t)s.b_index, index, "C9 storage[sub_id].b_index == index");
        ck_eq((uint32_t)s.door_mutex_unit, 0u, "C9 storage.door_mutex_unit == 0");
        ck_eq((uint32_t)s.door_waiter_count, 0u, "C9 storage.door_waiter_count == 0");
        ck_eq((uint32_t)s.docked_count, 0u, "C9 storage.docked_count == 0");
        ck_eq((uint32_t)s.occupancy, 0u, "C9 storage.occupancy == 0");
        // park_x/park_y are written as full masked dwords (int32_t since EN v313). NOTE the ld32
        // checks below are now REDUNDANT with the ck_eq above them, and deliberately kept as
        // documentation rather than as coverage: once the fields are int32_t, "the store did not
        // leave stale upper bytes" is guaranteed by the type, so no mutation of this function can
        // make ld32 disagree with the direct read. Mutation-tested 2026-08-22 -- narrowing the store
        // to (uint8_t) does NOT turn these red, because the assignment still writes all four bytes.
        // They earn their place only if the width is ever narrowed again in Ghidra.
        ck_eq((uint32_t)s.park_x, px, "C9 storage.park_x == (x_b+park_offset_x)&wmask");
        ck_eq(ld32((const uint8_t *)&s.park_x), px, "C9 storage.park_x covers all 4 bytes at +0xdc");
        ck_eq((uint32_t)s.park_y, py, "C9 storage.park_y == (y_b+park_offset_y)&hmask");
        ck_eq(ld32((const uint8_t *)&s.park_y), py, "C9 storage.park_y covers all 4 bytes at +0xe0");
        // exit tiles are plain int32 fields
        ck_eq((uint32_t)s.exit_tile_x, (x_b + 4u) & 0xffu, "C9 storage.exit_tile_x");
        ck_eq((uint32_t)s.exit_tile_y, ((uint32_t)y_b + 5u) & 0x3fu, "C9 storage.exit_tile_y");
    }
}

} // namespace mh::sim::test
