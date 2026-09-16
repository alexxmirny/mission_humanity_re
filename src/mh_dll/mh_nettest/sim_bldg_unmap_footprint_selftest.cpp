//
// sim_bldg_unmap_footprint_selftest.cpp -- `simtest` cases for llm_strat_bldg_unmap_footprint
// (sim/sim_bldg_unmap_footprint.h/.cpp), SIM1B (building_tick machinery slice) batch B.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_unmap_footprint_0047b36b.asm), cross-checked against the .cpp's own
// address-cited banner -- not read off the Ghidra .c draft. Every case below cites the .asm range it
// exercises.
//
//   STEP 1 (0x0047b382-0x0047b3c2): building.x/.y snapshotted into locals ONCE, before anything else
//   runs -- read again at the very end (step 10) as mother_reelect_primary's (x,y) argument.
//
//   STEP 2 (0x0047b3c2-0x0047b546): the 10x10 footprint-mask walk. `dx` (EBP-0x30) is the OUTER loop
//   var (0x0047b416: `area_index = dx*10 + dy`, so dx is area's ROW index) and is added to ORIG_X;
//   `dy` (EBP-0x2c) is the INNER loop var, added to ORIG_Y -- the SAME row-feeds-X/col-feeds-Y
//   convention llm_strat_bldg_footprint_set_passable's own selftest (sim_bldg_footprint_set_passable_
//   selftest.cpp) already proved for the sibling SET function; this file re-derives it independently
//   off THIS function's own raw IMUL/ADD chain, not copied from that proof. Per cell with
//   `cfg_buildings[building_id].area[dx][dy] != 0`: `tile_object_at(col,row).building=0`,
//   `.class_owner=0`, `passable_at(col,row)=1`, where `col=(orig_x+dx)&width_mask`,
//   `row=(orig_y+dy)&height_mask` (general.width_mask/height_mask, i.e. `v.geom->width_mask/
//   height_mask` -- DISTINCT from the fixture's width_m/height_m pair, same distinction
//   sim_bldg_footprint_set_passable_selftest.cpp's own banner notes).
//
//   STEP 3 (0x0047b58e-0x0047b592): `sight_remove_circle(player, orig_x, orig_y, building_id,
//   cfg_buildings[building_id].sight)`.
//
//   STEP 4 (0x0047b5c1-0x0047b5c7): `map_region_apply_area(orig_x, orig_y,
//   &cfg_buildings[building_id].area[0][0])` -- the pointer is the ORIGINAL cfg data's address, not a
//   copy; C1 below asserts pointer IDENTITY, not just byte content.
//
//   STEP 5 (0x0047b5e6-0x0047b5ed): `unassign_workers(player, building_index, current_workers)`,
//   return value discarded.
//
//   STEP 6 (0x0047b62f-0x0047b89d): the type-specific switch (see the .cpp's header banner for the
//   full case<->type-value derivation table; A_PRODUCTION=1/H=21, A_MINE=2/H=22, A_TURRET=5/H=25,
//   A_BARRACKS..H_SHUTTLE=7..0x21 (storage group), A_LAB=0xb/H=0x1f). A_PRODUCTION/H_PRODUCTION and
//   A_LAB/H_LAB carry the .cpp's own DECLARED NEED (cross-derived from the jump-table label
//   convention, not independently confirmed off a raw CMP) -- this file inherits that caveat rather
//   than re-litigating it; it re-derives the LOCAL constant values from the same .asm evidence the
//   .cpp cites, matching that file's copy (each TU in this tree keeps its own small constant block by
//   established convention, e.g. sim_bldg_power_network_selftest.cpp's own BLDG_STATE_* block).
//
//   STEP 7 (0x0047b89f-0x0047b8a8): `power_network_recompute(player)`, UNCONDITIONAL.
//
//   STEP 8 (0x0047b8a8-0x0047b949): UI-selection cleanup -- two mutually exclusive branches (local
//   player's ui_selected_bldg_index match, or a remote click-select target match), each doing
//   notify_ui + clear-the-matched-global + a panel-mode/page-gated set_event(BUILD_TAB_BUILDINGS=3).
//
//   STEP 9 (0x0047b949-0x0047b955): `notify_ui(player, building_index)`, UNCONDITIONAL -- the THIRD
//   notify_ui call site, always reached regardless of step 8's outcome.
//
//   STEP 10 (0x0047b955-0x0047ba63): MOTHER-type handling, gated on
//   `(type==A_MOTHER||type==H_MOTHER) && primary_mother_bldg[planet]==building_index`: clear the
//   primary-mother slot, a panel-gated set_event(BUILD_TAB_UNITS=4), `mother_reelect_primary(player,
//   orig_x, orig_y)` (the STEP-1 entry-time coords), then IF the reelect returned 0 AND player is the
//   local player: `print_queue_text_id(player_race==1 ? 0x69 : 0xb1)`; UNCONDITIONALLY within this
//   whole branch (both the reelect==0 and reelect!=0 paths): `player_teardown_hook_stub(player)`.
//
// THE RECORDING-STUB CONTRACT: all 10 outward callees are recorded via `unmap_footprint_calls`
// (mh::sim::unmap_footprint_calls, sim_bldg_unmap_footprint.h), the same pattern
// sim_bldg_power_network_selftest.cpp's Part B uses for its 4-member driver-calls struct.
//
#include "sim/sim_bldg_unmap_footprint.h"

#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- cfg_enum_E_BUILDING members, re-derived from this function's own .asm switch (see the banner
// above) -- same values the .cpp's own anonymous-namespace block carries, independently copied per
// this tree's per-TU-constant-block convention. Only the members this file's cases actually select.
constexpr uint8_t BLDG_TYPE_A_PRODUCTION = 1;
constexpr uint8_t BLDG_TYPE_A_MINE       = 2;
constexpr uint8_t BLDG_TYPE_A_TURRET     = 5;
constexpr uint8_t BLDG_TYPE_A_MOTHER     = 6;
constexpr uint8_t BLDG_TYPE_A_BARRACKS   = 7; // representative of the 12-value storage group
constexpr uint8_t BLDG_TYPE_A_LAB        = 0x0b;

constexpr uint32_t EVENT_BUILD_TAB_BUILDINGS = 3;
constexpr uint32_t EVENT_BUILD_TAB_UNITS     = 4;

// ---- recording stubs, one vector per distinct callee ---------------------------------------------
struct SightCall {
    int32_t player, x, y, building_id;
    uint8_t sight;
};
struct AreaCall {
    uint32_t    x;
    int32_t     y;
    const void *area_ptr;
};
struct UnassignCall {
    uint16_t player;
    uint32_t building_index, count;
};
struct TeardownCall {
    uint32_t player;
    uint16_t unit_index;
};
struct PowerCall {
    uint16_t player;
};
struct NotifyCall {
    uint16_t player;
    uint32_t building_index;
};
struct SetEventCall {
    uint32_t type;
};
struct ReelectCall {
    int32_t player, x, y;
};
struct PrintCall {
    int32_t text_id;
};
struct HookCall {
    int32_t player_index;
};

std::vector<SightCall>    g_sight;
std::vector<AreaCall>     g_area;
std::vector<UnassignCall> g_unassign;
std::vector<TeardownCall> g_teardown;
std::vector<PowerCall>    g_power;
std::vector<NotifyCall>   g_notify;
std::vector<SetEventCall> g_setevent;
std::vector<ReelectCall>  g_reelect;
std::vector<PrintCall>    g_print;
std::vector<HookCall>     g_hook;

// Controls what mother_reelect_primary's stub returns -- the original's return value gates step 10's
// print_queue_text_id call, so a case that needs the "reelected" vs "reelect failed" fork sets this
// before calling.
int32_t g_reelect_return = 1;

int32_t rec_sight(int32_t player, int32_t x, int32_t y, int32_t building_id, uint8_t sight) {
    g_sight.push_back({player, x, y, building_id, sight});
    return 0; // discarded by the original
}
void    rec_area(uint32_t x, int32_t y, char *area_mask) { g_area.push_back({x, y, area_mask}); }
int32_t rec_unassign(uint16_t player, uint32_t building_index, uint32_t count) {
    g_unassign.push_back({player, building_index, count});
    return 0; // discarded by the original
}
void rec_teardown(uint32_t player, uint16_t unit_index) {
    g_teardown.push_back({player, unit_index});
}
void rec_power(uint16_t player) { g_power.push_back({player}); }
void rec_notify(uint16_t player, uint32_t building_index) {
    g_notify.push_back({player, building_index});
}
uint32_t rec_setevent(uint32_t type) {
    g_setevent.push_back({type});
    return 0;
}
int32_t rec_reelect(int32_t player, int32_t x, int32_t y) {
    g_reelect.push_back({player, x, y});
    return g_reelect_return;
}
void    rec_print(int32_t text_id) { g_print.push_back({text_id}); }
int32_t rec_hook(int32_t player_index) {
    g_hook.push_back({player_index});
    return 0;
}

// Field order matches unmap_footprint_calls exactly (sim_bldg_unmap_footprint.h).
const unmap_footprint_calls g_calls = {
    &rec_sight,
    &rec_area,
    &rec_unassign,
    &rec_teardown,
    &rec_power,
    &rec_notify,
    &rec_setevent,
    &rec_reelect,
    &rec_print,
    &rec_hook,
};

void reset_calls() {
    g_sight.clear();
    g_area.clear();
    g_unassign.clear();
    g_teardown.clear();
    g_power.clear();
    g_notify.clear();
    g_setevent.clear();
    g_reelect.clear();
    g_print.clear();
    g_hook.clear();
    g_reelect_return = 1;
}

} // namespace

void run_bldg_unmap_footprint_tests() {
    // ================================================================================================
    // C1 -- footprint clear (TRANSPOSE-discriminating) + the full non-mother, non-storage orchestration
    // (steps 1,2,3,4,5,7,9), a plain PRODUCTION building, isolating step 8/10 by leaving player !=
    // player_side and picking a non-mother type. -----------------------------------------------------
    // ================================================================================================
    {
        sim_fixture fx;
        fx.reset();
        reset_calls();

        const uint16_t player  = 3; // != fixture's default player_side (7)
        const int32_t  bidx    = 5;
        const int32_t  cfg_row = 40;
        const int32_t  orig_x = 10, orig_y = 20;

        building &bld       = fx.b(player, bidx);
        bld.building_id     = (uint16_t)cfg_row;
        bld.x               = (uint8_t)orig_x;
        bld.y               = (uint8_t)orig_y;
        bld.sub_id          = 2;
        bld.current_workers = 7;

        cfg_building &cb = fx.cfg_buildings[cfg_row];
        cb.type          = BLDG_TYPE_A_PRODUCTION;
        cb.sight         = 4;
        // Sparse, TRANSPOSE-discriminating footprint: dx (outer/row-in-area) feeds X, dy (inner/
        // col-in-area) feeds Y -- distinct dx/dy everywhere so a row<->col swap lands on a genuinely
        // different tile, same rigor as sim_bldg_footprint_set_passable_selftest.cpp's S2.
        cb.area[1][0] = 1; // -> tile (orig_x+1, orig_y+0) = (11,20)
        cb.area[0][1] = 1; // -> tile (orig_x+0, orig_y+1) = (10,21)
        cb.area[4][3] = 1; // -> tile (orig_x+4, orig_y+3) = (14,23)

        // Sentinel-fill: building=0xAAAA, class_owner=0xAA, passable=0xAA -- none of these are the
        // real cleared/set values (0 / 0 / 1), so any touched cell is unambiguous.
        std::memset(fx.tile_objects.data(), 0xAA, fx.tile_objects.size() * sizeof(tile_object));
        std::memset(fx.passable.data(), 0xAA, fx.passable.size());

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::bldg_unmap_footprint(v, own, g_calls, player, bidx);

        ck_eq(own.tile_object_at(11, 20).building, 0u, "C1: footprint (11,20) building cleared");
        ck_eq(own.tile_object_at(11, 20).class_owner, 0u, "C1: footprint (11,20) class_owner cleared");
        ck_eq(own.passable_at(11, 20), 1u, "C1: footprint (11,20) passable set");
        ck_eq(own.tile_object_at(10, 21).building, 0u,
              "C1: footprint (10,21) building cleared (dy feeds Y)");
        ck_eq(own.passable_at(10, 21), 1u, "C1: footprint (10,21) passable set");
        ck_eq(own.tile_object_at(14, 23).building, 0u, "C1: footprint (14,23) building cleared");
        ck_eq(own.passable_at(14, 23), 1u, "C1: footprint (14,23) passable set");
        // The TRANSPOSED reading of area[4][3] would be tile (orig_x+3, orig_y+4) = (13,24) -- no
        // real cell maps there, must stay at the sentinel.
        ck_eq(own.tile_object_at(13, 24).building, 0xAAAAu,
              "C1: transposed reading of area[4][3] (13,24) untouched (building)");
        ck_eq(own.passable_at(13, 24), 0xAAu,
              "C1: transposed reading of area[4][3] (13,24) untouched (passable)");
        // An unrelated neighbour, to catch a gross off-by-stride bug.
        ck_eq(own.tile_object_at(12, 21).building, 0xAAAAu, "C1: unrelated tile (12,21) untouched");
        ck_eq(own.passable_at(12, 21), 0xAAu, "C1: unrelated tile (12,21) untouched (passable)");

        ck_eq((uint32_t)g_sight.size(), 1u, "C1: sight_remove_circle called once");
        if (!g_sight.empty()) {
            ck_eq((uint32_t)g_sight[0].player, player, "C1: sight player");
            ck_eq((uint32_t)g_sight[0].x, (uint32_t)orig_x, "C1: sight x (ORIGINAL coord)");
            ck_eq((uint32_t)g_sight[0].y, (uint32_t)orig_y, "C1: sight y (ORIGINAL coord)");
            ck_eq((uint32_t)g_sight[0].building_id, (uint32_t)cfg_row, "C1: sight building_id");
            ck_eq((uint32_t)g_sight[0].sight, 4u, "C1: sight radius == cfg_buildings[bid].sight");
        }

        ck_eq((uint32_t)g_area.size(), 1u, "C1: map_region_apply_area called once");
        if (!g_area.empty()) {
            ck_eq(g_area[0].x, (uint32_t)orig_x, "C1: area x");
            ck_eq((uint32_t)g_area[0].y, (uint32_t)orig_y, "C1: area y");
            ck(g_area[0].area_ptr == &fx.cfg_buildings[cfg_row].area[0][0],
               "C1: area pointer IS &cfg_buildings[building_id].area[0][0] (identity, not a copy)");
        }

        ck_eq((uint32_t)g_unassign.size(), 1u, "C1: unassign_workers called once");
        if (!g_unassign.empty()) {
            ck_eq((uint32_t)g_unassign[0].player, player, "C1: unassign player");
            ck_eq(g_unassign[0].building_index, (uint32_t)bidx, "C1: unassign building_index");
            ck_eq(g_unassign[0].count, 7u, "C1: unassign count == building.current_workers");
        }

        // PRODUCTION arm: zero sub_id slot, THEN decrement [0] (fresh state -> [0] ends at -1).
        ck_eq((uint32_t)own.production_at(player, 2).b_index, 0u,
              "C1: production[player][sub_id=2].b_index zeroed");
        ck_eq((uint32_t)(int32_t)own.production_at(player, 0).b_index, (uint32_t)-1,
              "C1: production[player][0].b_index decremented (0 -> -1)");

        ck_eq((uint32_t)g_power.size(), 1u, "C1: power_network_recompute called once (unconditional)");

        // Step 8's two branches don't match (player 3 != default player_side 7; click flags default
        // 0) -> only step 9's UNCONDITIONAL notify_ui fires.
        ck_eq((uint32_t)g_notify.size(), 1u, "C1: notify_ui fired exactly once (step 9 only)");
        ck_eq((uint32_t)g_setevent.size(), 0u, "C1: no set_event (neither step-8 branch matched)");

        // Non-mother type -> the whole step-10 branch is skipped.
        ck_eq((uint32_t)g_reelect.size(), 0u, "C1: non-mother -> mother_reelect_primary not called");
        ck_eq((uint32_t)g_hook.size(), 0u, "C1: non-mother -> player_teardown_hook_stub not called");
        ck_eq((uint32_t)g_print.size(), 0u, "C1: non-mother -> print_queue_text_id not called");
    }

    // ================================================================================================
    // C2 -- footprint clear, MASK-WRAP-discriminating: distinct width_mask/height_mask, origin chosen
    // near each axis's own wrap point so a translation that swapped which mask gates which axis lands
    // on a different tile. Same construction as sim_bldg_footprint_set_passable_selftest.cpp's S3.
    // ================================================================================================
    {
        sim_fixture fx;
        fx.reset();
        reset_calls();

        fx.geom.width_mask  = 0xff; // 256-wide axis
        fx.geom.height_mask = 0x3f; // 64-wide axis (distinct from width_mask)

        const uint16_t player  = 3;
        const int32_t  bidx    = 6;
        const int32_t  cfg_row = 41;
        const int32_t  orig_x = 0xfe, orig_y = 0x3e; // each near ITS OWN axis's wrap point

        building &bld    = fx.b(player, bidx);
        bld.building_id  = (uint16_t)cfg_row;
        bld.x            = (uint8_t)orig_x;
        bld.y            = (uint8_t)orig_y;
        cfg_building &cb = fx.cfg_buildings[cfg_row];
        cb.type          = BLDG_TYPE_A_PRODUCTION;

        // area[1][5]: tile_x=(0xfe+1)&0xff=0xff=255 (no wrap); tile_y=(0x3e+5)&0x3f=0x43&0x3f=3
        // (wraps). A mask-swapped translation would instead compute tile_x=(0xfe+1)&0x3f=63.
        cb.area[1][5] = 1;

        std::memset(fx.tile_objects.data(), 0xAA, fx.tile_objects.size() * sizeof(tile_object));
        std::memset(fx.passable.data(), 0xAA, fx.passable.size());

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::bldg_unmap_footprint(v, own, g_calls, player, bidx);

        ck_eq(own.tile_object_at(255, 3).building, 0u,
              "C2: area[1][5] with height-axis wrap -> tile (255,3) cleared");
        ck_eq(own.passable_at(255, 3), 1u, "C2: tile (255,3) passable set");
        ck_eq(own.tile_object_at(63, 3).building, 0xAAAAu,
              "C2: mask-swapped reading (63,3) must NOT be touched");
        ck_eq(own.passable_at(63, 3), 0xAAu, "C2: mask-swapped reading (63,3) passable untouched");
    }

    // ================================================================================================
    // C3 -- the type-gated slot arithmetic's ORDER-OF-OPERATIONS. Only observable when sub_id==0 (the
    // slot the per-player COUNT lives in is the SAME slot being cleared): PRODUCTION/MINE/TURRET zero
    // sub_id THEN decrement [0] (0x0047b652-0x0047b6dd); LAB decrements [0] THEN zeroes sub_id
    // (0x0047b871-0x0047b89d, the opposite order). Seeded value 5 diverges to -1 vs 0 depending on
    // order -- the sharpest possible observable for an order bug. Plus a MINE/TURRET sanity check at a
    // non-zero sub_id (order-insensitive there, just confirms the right region+field is touched).
    // ================================================================================================
    {
        // C3a: PRODUCTION, sub_id == 0 -> zero-THEN-decrement -> ends at -1.
        sim_fixture fx;
        fx.reset();
        reset_calls();
        const uint16_t player          = 1;
        const int32_t  bidx            = 3;
        const int32_t  cfg_row         = 10;
        building      &bld             = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        bld.sub_id                     = 0;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_PRODUCTION;

        sim_store own                        = fx.store();
        own.production_at(player, 0).b_index = 5;
        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);
        ck_eq((uint32_t)(int32_t)own.production_at(player, 0).b_index, (uint32_t)-1,
              "C3a: PRODUCTION sub_id==0 order is zero-THEN-decrement (5 -> 0 -> -1)");
    }
    {
        // C3b: LAB, sub_id == 0 -> decrement-THEN-zero -> ends at 0 (distinguishes from C3a's -1).
        sim_fixture fx;
        fx.reset();
        reset_calls();
        const uint16_t player          = 1;
        const int32_t  bidx            = 3;
        const int32_t  cfg_row         = 11;
        building      &bld             = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        bld.sub_id                     = 0;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_LAB;

        sim_store own                 = fx.store();
        own.lab_at(player, 0).b_index = 5;
        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);
        ck_eq((uint32_t)own.lab_at(player, 0).b_index, 0u,
              "C3b: LAB sub_id==0 order is decrement-THEN-zero (5 -> 4 -> 0)");
    }
    {
        // C3c: MINE, non-zero sub_id -- confirms the right region/field (int32_t f1).
        sim_fixture fx;
        fx.reset();
        reset_calls();
        const uint16_t player     = 4;
        building      &bld        = fx.b(player, 2);
        bld.building_id           = 12;
        bld.sub_id                = 3;
        fx.cfg_buildings[12].type = BLDG_TYPE_A_MINE;

        sim_store own                  = fx.store();
        own.mine_at(player, 0).b_index = 9;
        own.mine_at(player, 3).b_index = 77;
        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, 2);
        ck_eq((uint32_t)own.mine_at(player, 3).b_index, 0u, "C3c: MINE[player][sub_id=3].b_index zeroed");
        ck_eq((uint32_t)own.mine_at(player, 0).b_index, 8u, "C3c: MINE[player][0].b_index decremented (9 -> 8)");
    }
    {
        // C3d: TURRET, non-zero sub_id -- confirms the right region/field (int16_t f1).
        sim_fixture fx;
        fx.reset();
        reset_calls();
        const uint16_t player     = 4;
        building      &bld        = fx.b(player, 6);
        bld.building_id           = 13;
        bld.sub_id                = 5;
        fx.cfg_buildings[13].type = BLDG_TYPE_A_TURRET;

        sim_store own                    = fx.store();
        own.turret_at(player, 0).b_index = 4;
        own.turret_at(player, 5).b_index = 66;
        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, 6);
        ck_eq((uint32_t)own.turret_at(player, 5).b_index, 0u, "C3d: TURRET[player][sub_id=5].b_index zeroed");
        ck_eq((uint32_t)own.turret_at(player, 0).b_index, 3u,
              "C3d: TURRET[player][0].b_index decremented (4 -> 3)");
    }

    // ================================================================================================
    // C4 -- STORAGE-group type (caseD_7, 0x0047b6e2-0x0047b87f): docked_count > 0 drives a
    // unit_teardown per docked unit (docked_count read ONCE before the loop) plus clearing
    // docked_units[]/docked_count/occupancy, THEN (regardless) the roster walk clears
    // home_storage_slot on units that reference this slot -- gated by a "remaining" COUNT seeded from
    // units[player][0].unit_above, decremented only on LIVE units (unit_above != {0,0}), not a blind
    // scan of all 100 -- exercised at the exhaustion boundary (0x0047b7fb/0x0047b805).
    // ================================================================================================
    {
        sim_fixture fx;
        fx.reset();
        reset_calls();

        const uint16_t player  = 2;
        const int32_t  bidx    = 4;
        const int32_t  cfg_row = 20;
        const int32_t  sub_id  = 9;

        building &bld                  = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        bld.sub_id                     = (uint8_t)sub_id;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_BARRACKS;

        sim_store own                                  = fx.store();
        own.storage_at(player, 0).b_index              = 6;  // per-player occupied-slot COUNT
        own.storage_at(player, sub_id).b_index         = 88; // this slot's own b_index (zeroed)
        own.storage_at(player, sub_id).docked_count    = 2;
        own.storage_at(player, sub_id).occupancy       = 42;
        own.storage_at(player, sub_id).docked_units[0] = 11;
        own.storage_at(player, sub_id).docked_units[1] = 22;

        // Roster: seed unit[player][0].unit_above as the "remaining" WORD count = 3.
        unit &seed         = own.unit_at(player, 0);
        seed.unit_above[0] = 3;
        seed.unit_above[1] = 0;
        // idx1: LIVE, home_storage_slot == sub_id -> cleared (1st live unit, remaining 3->2).
        unit &u1             = own.unit_at(player, 1);
        u1.unit_above[0]     = 1;
        u1.home_storage_slot = (uint8_t)sub_id;
        // idx2: DEAD (unit_above == {0,0}) -- must be skipped entirely, even though its
        // home_storage_slot also matches: proves the "live" gate, not just a value scan.
        unit &u2             = own.unit_at(player, 2);
        u2.unit_above[0]     = 0;
        u2.unit_above[1]     = 0;
        u2.home_storage_slot = (uint8_t)sub_id;
        // idx3: LIVE, home_storage_slot != sub_id -> consumes remaining (2->1) but not cleared.
        unit &u3             = own.unit_at(player, 3);
        u3.unit_above[0]     = 1;
        u3.home_storage_slot = (uint8_t)(sub_id + 1);
        // idx4: LIVE, home_storage_slot == sub_id -> cleared (3rd live unit, exhausts remaining 1->0).
        unit &u4             = own.unit_at(player, 4);
        u4.unit_above[0]     = 1;
        u4.home_storage_slot = (uint8_t)sub_id;
        // idx5: LIVE, home_storage_slot == sub_id, but remaining is ALREADY 0 by the time idx5 would
        // be visited -> the loop's `remaining != 0` top-of-iteration guard means idx5 is NEVER
        // reached at all. Must stay untouched despite matching.
        unit &u5             = own.unit_at(player, 5);
        u5.unit_above[0]     = 1;
        u5.home_storage_slot = (uint8_t)sub_id;

        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);

        ck_eq((uint32_t)g_teardown.size(), 2u, "C4: unit_teardown called exactly twice (docked_count)");
        if (g_teardown.size() == 2) {
            ck_eq((uint32_t)g_teardown[0].player, player, "C4: teardown[0] player");
            ck_eq((uint32_t)g_teardown[0].unit_index, 11u,
                  "C4: teardown[0] unit_index (docked_units[0])");
            ck_eq((uint32_t)g_teardown[1].unit_index, 22u,
                  "C4: teardown[1] unit_index (docked_units[1])");
        }
        ck_eq((uint32_t)own.storage_at(player, sub_id).docked_units[0], 0u,
              "C4: docked_units[0] cleared");
        ck_eq((uint32_t)own.storage_at(player, sub_id).docked_units[1], 0u,
              "C4: docked_units[1] cleared");
        ck_eq((uint32_t)own.storage_at(player, sub_id).docked_count, 0u, "C4: docked_count reset");
        ck_eq((uint32_t)own.storage_at(player, sub_id).occupancy, 0u, "C4: occupancy reset");
        ck_eq((uint32_t)own.storage_at(player, sub_id).b_index, 0u, "C4: this slot's b_index zeroed");
        ck_eq((uint32_t)(int32_t)own.storage_at(player, 0).b_index, 5u,
              "C4: storage[player][0] occupied-count decremented (6 -> 5)");

        ck_eq((uint32_t)own.unit_at(player, 1).home_storage_slot, 0u,
              "C4: idx1 (live, matching, 1st) home_storage_slot cleared");
        ck_eq((uint32_t)own.unit_at(player, 2).home_storage_slot, (uint32_t)sub_id,
              "C4: idx2 (DEAD) untouched despite matching value -- the live gate, not a value scan");
        ck_eq((uint32_t)own.unit_at(player, 3).home_storage_slot, (uint32_t)(sub_id + 1),
              "C4: idx3 (live, non-matching) untouched, still consumed 'remaining'");
        ck_eq((uint32_t)own.unit_at(player, 4).home_storage_slot, 0u,
              "C4: idx4 (live, matching, 3rd) home_storage_slot cleared (exhausts remaining)");
        ck_eq((uint32_t)own.unit_at(player, 5).home_storage_slot, (uint32_t)sub_id,
              "C4: idx5 (live, matching) NEVER REACHED -- remaining exhausted at idx4, untouched");
    }

    // ================================================================================================
    // C5 -- STORAGE-group type, docked_count == 0: the ENTIRE docked-unit loop AND the
    // docked_count/occupancy reset are INSIDE the `if (docked_count != 0)` guard
    // (0x0047b72f-0x0047b7e0), so a pre-existing nonzero `occupancy` is left AS-IS (not a separate
    // unconditional reset) -- and unit_teardown is never called. The roster walk (0x0047b7e0 onward)
    // still runs unconditionally, regardless of the docked branch.
    // ================================================================================================
    {
        sim_fixture fx;
        fx.reset();
        reset_calls();

        const uint16_t player  = 2;
        const int32_t  bidx    = 4;
        const int32_t  cfg_row = 22;
        const int32_t  sub_id  = 3;

        building &bld                  = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        bld.sub_id                     = (uint8_t)sub_id;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_BARRACKS;

        sim_store own                               = fx.store();
        own.storage_at(player, 0).b_index           = 4;
        own.storage_at(player, sub_id).b_index      = 99;
        own.storage_at(player, sub_id).docked_count = 0;  // the case under test
        own.storage_at(player, sub_id).occupancy    = 55; // pre-existing, must NOT be reset

        unit &seed           = own.unit_at(player, 0);
        seed.unit_above[0]   = 1; // remaining = 1
        unit &u1             = own.unit_at(player, 1);
        u1.unit_above[0]     = 1; // live
        u1.home_storage_slot = (uint8_t)sub_id;

        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);

        ck_eq((uint32_t)g_teardown.size(), 0u, "C5: docked_count==0 -> unit_teardown never called");
        ck_eq((uint32_t)own.storage_at(player, sub_id).occupancy, 55u,
              "C5: occupancy left AS-IS (the reset is INSIDE the docked_count!=0 guard)");
        // b_index zero+decrement are OUTSIDE the docked_count guard (0x0047b6ec/0x0047b705, BEFORE
        // the docked_count read at 0x0047b722) -- still happen even when docked_count==0.
        ck_eq((uint32_t)own.storage_at(player, sub_id).b_index, 0u,
              "C5: this slot's b_index still zeroed (outside the docked_count guard)");
        ck_eq((uint32_t)(int32_t)own.storage_at(player, 0).b_index, 3u,
              "C5: storage[player][0] count still decremented (4 -> 3)");
        // The roster walk runs regardless of the docked-unit branch.
        ck_eq((uint32_t)own.unit_at(player, 1).home_storage_slot, 0u,
              "C5: roster walk still runs when docked_count==0 -- matching live unit cleared");
    }

    // ================================================================================================
    // C6 -- MOTHER-type handling (step 10, 0x0047b955-0x0047ba63), gated on
    // `(type==MOTHER) && primary_mother_bldg[planet]==building_index`.
    // ================================================================================================
    {
        // C6a: was primary, reelect SUCCEEDS (nonzero return) -> primary cleared, reelect called with
        // the STEP-1 ENTRY-time (x,y) (not re-derived), teardown-hook called, NO print_queue_text_id.
        // player != player_side here so the print gate and the panel-mode set_event stay isolated.
        sim_fixture fx;
        fx.reset();
        reset_calls();
        g_reelect_return = 7; // nonzero -> "reelected"

        const uint16_t player  = 2;
        const int32_t  bidx    = 1;
        const int32_t  cfg_row = 30;
        const int32_t  planet  = 5;
        const int32_t  orig_x = 44, orig_y = 55;

        fx.planet_index = planet;
        fx.player_side  = 9; // != player, isolates the print/panel-event gates

        building &bld                  = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        bld.x                          = (uint8_t)orig_x;
        bld.y                          = (uint8_t)orig_y;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_MOTHER;

        sim_store own                                      = fx.store();
        own.profile_at(player).primary_mother_bldg[planet] = bidx; // IS primary

        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);

        ck_eq((uint32_t)own.profile_at(player).primary_mother_bldg[planet], 0u,
              "C6a: primary_mother_bldg[planet] cleared");
        ck_eq((uint32_t)g_reelect.size(), 1u, "C6a: mother_reelect_primary called once");
        if (!g_reelect.empty()) {
            ck_eq((uint32_t)g_reelect[0].player, player, "C6a: reelect player");
            ck_eq((uint32_t)g_reelect[0].x, (uint32_t)orig_x, "C6a: reelect x is the ENTRY-time coord");
            ck_eq((uint32_t)g_reelect[0].y, (uint32_t)orig_y, "C6a: reelect y is the ENTRY-time coord");
        }
        ck_eq((uint32_t)g_print.size(), 0u, "C6a: reelect succeeded -> no print_queue_text_id");
        ck_eq((uint32_t)g_hook.size(), 1u,
              "C6a: player_teardown_hook_stub called UNCONDITIONALLY within the mother branch");
        if (!g_hook.empty())
            ck_eq((uint32_t)g_hook[0].player_index, player, "C6a: hook player_index");

        // ---- C6b: was primary, reelect FAILS (returns 0), player IS player_side, race==1 -> prints
        // text_id 0x69; ALSO the panel-mode-gated set_event(BUILD_TAB_UNITS=4) fires here.
        fx.reset();
        reset_calls();
        g_reelect_return = 0; // "reelect failed"
        fx.planet_index  = planet;
        fx.player_side   = player; // == player, opens the print gate
        fx.player_race   = 1;
        fx.ui_panel_mode = 1;
        fx.ui_panel_page = 0; // matches the mother branch's own gate (mode==1 && page==0)

        building &bld2                 = fx.b(player, bidx);
        bld2.building_id               = (uint16_t)cfg_row;
        bld2.x                         = (uint8_t)orig_x;
        bld2.y                         = (uint8_t)orig_y;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_MOTHER;

        sim_store own2                                      = fx.store();
        own2.profile_at(player).primary_mother_bldg[planet] = bidx;

        detail::bldg_unmap_footprint(fx.view(), own2, g_calls, player, bidx);

        ck_eq((uint32_t)g_setevent.size(), 1u,
              "C6b: BUILD_TAB_UNITS set_event fired (player==player_side, mode==1, page==0)");
        if (!g_setevent.empty())
            ck_eq(g_setevent[0].type, EVENT_BUILD_TAB_UNITS, "C6b: set_event type is BUILD_TAB_UNITS(4)");
        ck_eq((uint32_t)g_print.size(), 1u, "C6b: reelect failed + local player -> print fires");
        if (!g_print.empty())
            ck_eq((uint32_t)g_print[0].text_id, 0x69u, "C6b: race==1 -> text_id 0x69");
        ck_eq((uint32_t)g_hook.size(), 1u, "C6b: teardown-hook still called (unconditional)");

        // ---- C6c: same as C6b but race != 1 -> text_id 0xb1 instead.
        fx.reset();
        reset_calls();
        g_reelect_return = 0;
        fx.planet_index  = planet;
        fx.player_side   = player;
        fx.player_race   = 0; // != 1

        building &bld3                 = fx.b(player, bidx);
        bld3.building_id               = (uint16_t)cfg_row;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_MOTHER;

        sim_store own3                                      = fx.store();
        own3.profile_at(player).primary_mother_bldg[planet] = bidx;

        detail::bldg_unmap_footprint(fx.view(), own3, g_calls, player, bidx);

        ck_eq((uint32_t)g_print.size(), 1u, "C6c: reelect failed + local player -> print fires");
        if (!g_print.empty())
            ck_eq((uint32_t)g_print[0].text_id, 0xb1u, "C6c: race!=1 -> text_id 0xb1");

        // ---- C6d: MOTHER type, but NOT the primary (primary_mother_bldg[planet] != building_index)
        // -> the WHOLE branch is skipped: no reelect, no hook, no print, primary left untouched.
        fx.reset();
        reset_calls();

        building &bld4                 = fx.b(player, bidx);
        bld4.building_id               = (uint16_t)cfg_row;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_MOTHER;

        sim_store own4                                      = fx.store();
        own4.profile_at(player).primary_mother_bldg[planet] = bidx + 1; // a DIFFERENT building

        detail::bldg_unmap_footprint(fx.view(), own4, g_calls, player, bidx);

        ck_eq((uint32_t)g_reelect.size(), 0u, "C6d: not primary -> mother_reelect_primary not called");
        ck_eq((uint32_t)g_hook.size(), 0u, "C6d: not primary -> player_teardown_hook_stub not called");
        ck_eq((uint32_t)g_print.size(), 0u, "C6d: not primary -> print_queue_text_id not called");
        ck_eq((uint32_t)own4.profile_at(player).primary_mother_bldg[planet], (uint32_t)(bidx + 1),
              "C6d: primary_mother_bldg[planet] left untouched (still the OTHER building)");
    }

    // ================================================================================================
    // C7 -- step 8's UI-selection cleanup (0x0047b8a8-0x0047b949): two mutually exclusive branches,
    // both converging on step 9's unconditional notify_ui -- so a matching branch means notify_ui
    // fires TWICE total per call, versus C1's baseline of once when neither matches.
    // ================================================================================================
    {
        // C7a: local-player branch -- player == player_side AND ui_selected_bldg_index ==
        // building_index -> notify_ui x2, ui_selected_bldg_index cleared, and (panel mode==1,
        // page==2) -> set_event(BUILD_TAB_BUILDINGS=3).
        sim_fixture fx;
        fx.reset();
        reset_calls();

        const uint16_t player  = 6;
        const int32_t  bidx    = 8;
        const int32_t  cfg_row = 50;

        fx.player_side            = player;
        fx.ui_selected_bldg_index = (uint16_t)bidx;
        fx.ui_panel_mode          = 1;
        fx.ui_panel_page          = 2;

        building &bld                  = fx.b(player, bidx);
        bld.building_id                = (uint16_t)cfg_row;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_PRODUCTION; // non-mother, isolates step 10

        sim_store own = fx.store();
        detail::bldg_unmap_footprint(fx.view(), own, g_calls, player, bidx);

        ck_eq((uint32_t)g_notify.size(), 2u,
              "C7a: notify_ui fired TWICE (step-8 branch + the unconditional step-9 call)");
        ck_eq((uint32_t)own.ui_selected_bldg_index(), 0u, "C7a: ui_selected_bldg_index cleared");
        ck_eq((uint32_t)g_setevent.size(), 1u, "C7a: BUILD_TAB_BUILDINGS set_event fired");
        if (!g_setevent.empty())
            ck_eq(g_setevent[0].type, EVENT_BUILD_TAB_BUILDINGS,
                  "C7a: set_event type is BUILD_TAB_BUILDINGS(3)");

        // C7b: click-select branch -- player != player_side (so the local branch's first condition
        // fails), click_select_target_flags == (player|0x40) AND click_select_target_id ==
        // building_index -> same shape (notify_ui x2, click_select_target_id cleared, panel-gated
        // set_event(3)).
        fx.reset();
        reset_calls();

        const uint16_t player_side_local = 1; // != player below
        fx.player_side                   = player_side_local;
        fx.click_select_target_flags     = (uint16_t)(player | 0x40u);
        fx.click_select_target_id        = (uint16_t)bidx;
        fx.ui_panel_mode                 = 1;
        fx.ui_panel_page                 = 2;

        building &bld2                 = fx.b(player, bidx);
        bld2.building_id               = (uint16_t)cfg_row;
        fx.cfg_buildings[cfg_row].type = BLDG_TYPE_A_PRODUCTION;

        sim_store own2 = fx.store();
        detail::bldg_unmap_footprint(fx.view(), own2, g_calls, player, bidx);

        ck_eq((uint32_t)g_notify.size(), 2u,
              "C7b: notify_ui fired TWICE (click-select branch + the unconditional step-9 call)");
        ck_eq((uint32_t)own2.click_select_target_id(), 0u, "C7b: click_select_target_id cleared");
        ck_eq((uint32_t)g_setevent.size(), 1u, "C7b: BUILD_TAB_BUILDINGS set_event fired");
        if (!g_setevent.empty())
            ck_eq(g_setevent[0].type, EVENT_BUILD_TAB_BUILDINGS,
                  "C7b: set_event type is BUILD_TAB_BUILDINGS(3)");
    }
}

} // namespace mh::sim::test
