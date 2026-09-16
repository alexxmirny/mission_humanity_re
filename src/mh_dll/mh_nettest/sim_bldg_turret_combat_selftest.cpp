#include "sim/sim_bldg_turret_combat.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// =====================================================================================================
// shared little-endian packed-full-id helper -- same word format as mh_map_tile_object_data::unit and
// mh_map_object_unit::unit_above (both uint8_t[2], low byte first per unit_full_id_word() in the .cpp
// under test): high nibble = owner player, low 12 bits = roster index.
// =====================================================================================================
void set_full_id(uint8_t (&arr)[2], uint32_t owner, uint32_t index) {
    const uint16_t w = static_cast<uint16_t>(((owner & 0xfu) << 12) | (index & 0xfffu));
    arr[0]           = static_cast<uint8_t>(w & 0xffu);
    arr[1]           = static_cast<uint8_t>((w >> 8) & 0xffu);
}

// =====================================================================================================
// llm_strat_turret_acquire_target's recorders
// =====================================================================================================

uint16_t g_at_self_player = 0;
int32_t  g_at_self_bidx   = 0;
int32_t  g_at_self_x      = 0;
int32_t  g_at_self_y      = 0;

struct coord_call {
    uint16_t player;
    int32_t  index;
};
std::vector<coord_call> g_at_bldg_coord_calls;
std::vector<coord_call> g_at_unit_coord_calls;

// bldg_get_coords -- the SELF call (player/index matching the seeded self building) returns the fixed
// self coords; every other (player,index) pair returns a value DISTINCT per pair so a wrong-arg call
// is observable in g_at_bldg_coord_calls' recorded args and in what dir_from_to received.
void rec_at_bldg_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    g_at_bldg_coord_calls.push_back({player, index});
    int32_t x, y;
    if (player == g_at_self_player && index == g_at_self_bidx) {
        x = g_at_self_x;
        y = g_at_self_y;
    } else {
        x = static_cast<int32_t>(player) * 100000 + index * 10 + 1;
        y = static_cast<int32_t>(player) * 100000 + index * 10 + 2;
    }
    *out_x = x;
    *out_y = y;
}

void rec_at_unit_get_coords(uint16_t player, int32_t index, int32_t *out_x, int32_t *out_y) {
    g_at_unit_coord_calls.push_back({player, index});
    *out_x = static_cast<int32_t>(player) * 200000 + index * 10 + 3;
    *out_y = static_cast<int32_t>(player) * 200000 + index * 10 + 4;
}

struct dir_call {
    int32_t x1, y1, x2, y2;
};
std::vector<dir_call> g_at_dir_calls;
std::vector<int32_t>  g_at_dir_returns; // queued raw_dir results, consumed in call order
int32_t               rec_at_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_at_dir_calls.push_back({x1, y1, x2, y2});
    const size_t i = g_at_dir_calls.size() - 1;
    return (i < g_at_dir_returns.size()) ? g_at_dir_returns[i] : 5; // arbitrary default
}

const turret_acquire_target_calls g_at_calls = {
    &rec_at_bldg_get_coords,
    &rec_at_unit_get_coords,
    &rec_at_dir_from_to,
};

void at_reset_recorders() {
    g_at_bldg_coord_calls.clear();
    g_at_unit_coord_calls.clear();
    g_at_dir_calls.clear();
    g_at_dir_returns.clear();
}

void at_set_relation(sim_fixture &fx, uint32_t a, uint32_t b, uint8_t val) {
    fx.players_raw[(size_t)a * 0x34 + 8 + b] = val;
}

turret &at_trt(sim_fixture &fx, uint16_t player, int32_t sub_id) {
    return fx.turrets[(size_t)player * TURRETS_PER_PLAYER + (size_t)sub_id];
}

// =====================================================================================================
// llm_strat_turret_fire's recorders
// =====================================================================================================

struct anchor_call {
    uint32_t player;
    int32_t  b_index;
    int32_t  anchor_kind;
    uint8_t  axis;
};
std::vector<anchor_call> g_fr_anchor_calls;
// Distinct per (anchor_kind, axis): a swapped axis literal or anchor_kind produces a recognizably
// different value at every downstream consumer (projectile_spawn/dir_from_to/fx_anim_spawn/
// apply_area_damage args), which is what "consuming the wrong axis fails a case" requires.
uint32_t rec_fr_anchor_coord(uint32_t player, int32_t b_index, int32_t anchor_kind, uint8_t axis) {
    g_fr_anchor_calls.push_back({player, b_index, anchor_kind, axis});
    return 10000u + (uint32_t)anchor_kind * 100u + (uint32_t)axis * 10u + player;
}

std::vector<dir_call> g_fr_dir_calls;
int32_t               rec_fr_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_fr_dir_calls.push_back({x1, y1, x2, y2});
    return 4; // fixed dir (dir-1=3) -- every case using it is checked against this constant
}

std::vector<int32_t> g_fr_stride_args;
int32_t              rec_fr_fx_anim_dir_frame_stride(int32_t start_frame) {
    g_fr_stride_args.push_back(start_frame);
    return 7;
}

struct fx_spawn_call {
    uint32_t x, y, frame;
    double   clock;
    uint32_t p5;
};
std::vector<fx_spawn_call> g_fr_fx_spawn_calls;
uint32_t                   rec_fr_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t frame, double clock,
                                                uint32_t p5) {
    g_fr_fx_spawn_calls.push_back({x, y, frame, clock, p5});
    return 0;
}

struct scatter_call {
    int32_t  p1, missing;
    uint32_t anchor_x, anchor_y, x, y;
};
std::vector<scatter_call> g_fr_scatter_calls;
void                      rec_fr_weapon_scatter_offset(int32_t p1, int32_t missing, uint32_t anchor_x,
                                                       uint32_t anchor_y, uint32_t x, uint32_t y,
                                                       int32_t *out_dx, int32_t *out_dy) {
    g_fr_scatter_calls.push_back({p1, missing, anchor_x, anchor_y, x, y});
    *out_dx = 5;
    *out_dy = -3;
}

struct projectile_call {
    int32_t  src_x, src_y_ground, src_y_vis_offset;
    uint32_t dst_x, dst_y_ground;
    int32_t  dst_y_vis_offset, weapon_id;
    uint8_t  owner_player;
    uint16_t homing_flags;
    int32_t  homing_target;
    uint32_t shooter_ref;
    int32_t  shooter_unit_index;
};
std::vector<projectile_call> g_fr_projectile_calls;
int32_t                      rec_fr_projectile_spawn(int32_t src_x, int32_t src_y_ground,
                                                     int32_t src_y_vis_offset, uint32_t dst_x,
                                                     uint32_t dst_y_ground, int32_t dst_y_vis_offset,
                                                     int32_t weapon_id, uint8_t owner_player,
                                                     uint16_t homing_flags, int32_t homing_target,
                                                     uint32_t shooter_ref,
                                                     int32_t  shooter_unit_index) {
    g_fr_projectile_calls.push_back({src_x, src_y_ground, src_y_vis_offset, dst_x, dst_y_ground,
                                                          dst_y_vis_offset, weapon_id, owner_player, homing_flags,
                                                          homing_target, shooter_ref, shooter_unit_index});
    return 0;
}

struct snd_at_call {
    int32_t sound_id, col, row;
};
std::vector<snd_at_call> g_fr_snd_calls;
void                     rec_fr_snd_play_at(int32_t sound_id, int32_t tile_col, int32_t tile_row) {
    g_fr_snd_calls.push_back({sound_id, tile_col, tile_row});
}

struct damage_call {
    int32_t  x, y, target_kind;
    double   damage;
    int32_t  ring_count;
    uint32_t owner_filter, killer_info;
    int32_t  killer_unit_index;
};
std::vector<damage_call> g_fr_damage_calls;
void                     rec_fr_apply_area_damage(int32_t x, int32_t y, int32_t target_kind,
                                                  double damage, int32_t ring_count,
                                                  uint32_t owner_filter, uint32_t killer_info,
                                                  int32_t killer_unit_index) {
    g_fr_damage_calls.push_back(
        {x, y, target_kind, damage, ring_count, owner_filter, killer_info, killer_unit_index});
}

const turret_fire_calls g_fr_calls = {
    &rec_fr_anchor_coord,
    &rec_fr_dir_from_to,
    &rec_fr_fx_anim_dir_frame_stride,
    &rec_fr_fx_anim_spawn,
    &rec_fr_weapon_scatter_offset,
    &rec_fr_projectile_spawn,
    &rec_fr_snd_play_at,
    &rec_fr_apply_area_damage,
};

void fr_reset_recorders() {
    g_fr_anchor_calls.clear();
    g_fr_dir_calls.clear();
    g_fr_stride_args.clear();
    g_fr_fx_spawn_calls.clear();
    g_fr_scatter_calls.clear();
    g_fr_projectile_calls.clear();
    g_fr_snd_calls.clear();
    g_fr_damage_calls.clear();
}

turret &fr_trt(sim_fixture &fx, uint16_t player, int32_t sub_id) {
    return fx.turrets[(size_t)player * TURRETS_PER_PLAYER + (size_t)sub_id];
}

cfg_weapon &fr_wpn(sim_fixture &fx, int32_t weapon_id) { return fx.cfg_weapons[(size_t)weapon_id]; }

} // namespace

// =====================================================================================================
// llm_strat_turret_acquire_target @0x0047baf9
// =====================================================================================================
void run_turret_acquire_target_tests() {
    sim_fixture fx;

    constexpr uint16_t AT_PLAYER = 2;
    constexpr int32_t  AT_BIDX   = 5;
    constexpr int32_t  AT_SUBID  = 3;
    // self fine coords chosen so base_col=self_x/32=50, base_row=self_y/32=20 exactly (0x0047bb2c-
    // 0x0047bb51's truncating signed divide-by-32), landing well inside width_mask=0xff/height_mask=
    // 0x3f (sim_test_support.h's reset() defaults) so dx/dy in [-1,1] never wraps.
    constexpr int32_t AT_SELF_X   = 1600;
    constexpr int32_t AT_SELF_Y   = 640;
    constexpr int32_t AT_BASE_COL = 50;
    constexpr int32_t AT_BASE_ROW = 20;

    auto seed_self = [&](int32_t aim_heading, int32_t attack_range) {
        fx.b(AT_PLAYER, AT_BIDX).sub_id = static_cast<uint8_t>(AT_SUBID);
        turret &t                       = at_trt(fx, AT_PLAYER, AT_SUBID);
        t.aim_heading                   = aim_heading;
        t.attack_range                  = attack_range;
        g_at_self_player                = AT_PLAYER;
        g_at_self_bidx                  = AT_BIDX;
        g_at_self_x                     = AT_SELF_X;
        g_at_self_y                     = AT_SELF_Y;
    };
    auto tile = [&](int32_t dx, int32_t dy) -> tile_object & {
        return fx.t(AT_BASE_COL + dx, AT_BASE_ROW + dy);
    };

    // =================================================================================================
    // T1 -- no candidate anywhere in the (range=0) sweep: return 0, *out_1/*out_2 left UNTOUCHED
    // (0x0047bf6e-0x0047bf84's `return best_diff != 0x1e`, out-pointers never written on that path).
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);

        uint32_t  out_1 = 0xDEADBEEFu, out_2 = 0xCAFEBABEu;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);

        ck_eq(ret, 0u, "T1: no candidate -> return 0, 0x0047bf6e-0x0047bf84");
        ck_eq(out_1, 0xDEADBEEFu, "T1: *out_1 left untouched on a 0 return");
        ck_eq(out_2, 0xCAFEBABEu, "T1: *out_2 left untouched on a 0 return");
        ck_eq((uint32_t)g_at_bldg_coord_calls.size(), 1u,
              "T1: only the SELF bldg_get_coords call happens, 0x0047bb1a-0x0047bb27");
    }

    // =================================================================================================
    // T2 -- a plain BUILDING candidate (bit 0x80 clear -> .building read as a building index; enemy
    // relation==2; not decoration): *out_1 = class_owner|owner (a no-op OR, kept literal per
    // 0x0047bda0/0x0047bdae), *out_2 = t.building, return 1.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 3, 2); // 0x0047bcaa-0x0047bcc0: relation==2 required

        tile_object &tl = tile(0, 0);
        tl.building     = 77;
        tl.class_owner  = 0x03; // owner=3, bit 0x80 clear, bit 0x10 clear

        uint32_t out_1 = 0, out_2 = 0;
        g_at_dir_returns = {4}; // heading_diff(4,1)=3 (21 fwd steps, wraps 24-21); see T10 below for a
                                // case that pins the wrap itself
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);

        ck_eq(ret, 1u, "T2: building candidate found -> return 1, 0x0047bf6e-0x0047bf84");
        ck_eq(out_1, 0x03u, "T2: *out_1 = class_owner(0x03)|owner(3) no-op OR, 0x0047bda0/0x0047bdae");
        ck_eq(out_2, 77u, "T2: *out_2 = t.building, 0x0047bdab-0x0047bdae");
        ck((g_at_dir_calls.size() == 1 && g_at_dir_calls[0].x1 == AT_SELF_X &&
            g_at_dir_calls[0].y1 == AT_SELF_Y),
           "T2: dir_from_to's (x1,y1) = the SELF coords from the FIRST bldg_get_coords call, "
           "0x0047bd35-0x0047bd3b");
        ck((g_at_bldg_coord_calls.size() == 2 && g_at_bldg_coord_calls[1].player == 3 &&
            g_at_bldg_coord_calls[1].index == 77),
           "T2: candidate bldg_get_coords called with (owner=3, t.building=77), 0x0047bd1d-0x0047bd2a");
    }

    // =================================================================================================
    // T3 -- FALLTHROUGH: a tile with BOTH a building candidate AND a unit candidate. The unit arm is
    // reached even though the building arm already ran (no JMP between 0x0047bdae and 0x0047bdb0) --
    // pinned by making the UNIT candidate have the smaller heading-diff and asserting it, not the
    // building candidate, wins.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 4, 2);
        at_set_relation(fx, AT_PLAYER, 5, 2);

        tile_object &tl = tile(0, 0);
        tl.building     = 88;
        tl.class_owner  = 0x04; // owner=4
        set_full_id(tl.unit, /*owner*/ 5, /*index*/ 12);

        uint32_t out_1 = 0, out_2 = 0;
        // call#1 (building arm): raw_dir=4 -> diff=3. call#2 (unit arm): raw_dir==aim_heading(1) ->
        // 0 loop iterations -> diff=0, strictly smaller, so the unit candidate overwrites the
        // building candidate's update.
        g_at_dir_returns = {4, 1};
        sim_store own    = fx.store();
        uint32_t  ret    = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                         &out_1, &out_2);

        ck_eq(ret, 1u, "T3: fallthrough finds the unit candidate -> return 1");
        ck_eq(out_1, 0x85u, "T3: *out_1 = chain_owner(5)|0x80, i.e. the UNIT won, not the building, "
                            "0x0047bf52-0x0047bf5a (fallthrough proof)");
        ck_eq(out_2, 12u, "T3: *out_2 = the unit's own index (12), not t.building(88)");
        ck_eq((uint32_t)g_at_bldg_coord_calls.size(), 2u,
              "T3: the building arm's OWN bldg_get_coords call still ran before falling through, "
              "0x0047bd1d-0x0047bd2a");
        ck((g_at_unit_coord_calls.size() == 1 && g_at_unit_coord_calls[0].player == 5 &&
            g_at_unit_coord_calls[0].index == 12),
           "T3: unit_get_coords resolves the candidate (owner=5,index=12), 0x0047bed2-0x0047bedf");
    }

    // =================================================================================================
    // T4 -- manhattan-range rejection on a CORNER the dx/dy loop bounds alone would admit
    // (dx=1,dy=1, |dx|+|dy|=2 > attack_range=1): the whole cell is skipped BEFORE any coordinate
    // resolution or dir_from_to call, 0x0047bc74.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 1);
        at_set_relation(fx, AT_PLAYER, 9, 2);

        tile_object &corner = tile(1, 1);
        corner.building     = 300;
        corner.class_owner  = 0x09; // owner=9, would otherwise pass every other check

        uint32_t  out_1 = 0, out_2 = 0;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);

        ck_eq(ret, 0u, "T4: manhattan(2) > attack_range(1) rejects the corner cell, 0x0047bc74");
        ck_eq((uint32_t)g_at_dir_calls.size(), 0u,
              "T4: dir_from_to never called -- the manhattan check precedes coordinate resolution");
        ck_eq((uint32_t)g_at_bldg_coord_calls.size(), 1u,
              "T4: only the SELF bldg_get_coords call happens (candidate coords never resolved)");
    }

    // =================================================================================================
    // T5 -- not-enemy rejection, TWO ways: (a) relation != 2, (b) player == owner (own building) even
    // when relation[self][self] is deliberately set to 2, proving the `player != owner` half of the
    // guard is load-bearing on its own, 0x0047bcaa-0x0047bcc0.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 6, 1); // not 2

        tile_object &tl = tile(0, 0);
        tl.building     = 400;
        tl.class_owner  = 0x06;

        uint32_t  out_1 = 0, out_2 = 0;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);
        ck_eq(ret, 0u, "T5a: relation(6)==1, not 2 -> rejected, 0x0047bcaa-0x0047bcc0");
    }
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, AT_PLAYER, 2); // relation[self][self] deliberately == 2

        tile_object &tl = tile(0, 0);
        tl.building     = 401;
        tl.class_owner  = static_cast<uint8_t>(AT_PLAYER); // owner == AT_PLAYER (own building)

        uint32_t  out_1 = 0, out_2 = 0;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);
        ck_eq(ret, 0u,
              "T5b: player==owner rejects even though relation[self][self]==2 -- the `player!=owner` "
              "half of the guard is load-bearing on its own, 0x0047bcaa");
    }

    // =================================================================================================
    // T6 -- decoration/tree rejection (class_owner bit 0x10 set), checked AFTER the relation gate but
    // BEFORE coordinate resolution -- no dir_from_to call either, 0x0047bcf3-0x0047bcfa.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 7, 2);

        tile_object &tl = tile(0, 0);
        tl.building     = 500;
        tl.class_owner  = 0x17; // 0x10 (decoration) | 0x07 (owner)

        uint32_t  out_1 = 0, out_2 = 0;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);
        ck_eq(ret, 0u, "T6: class_owner bit 0x10 (decoration) rejects, 0x0047bcf3-0x0047bcfa");
        ck_eq((uint32_t)g_at_dir_calls.size(), 0u,
              "T6: dir_from_to never called -- the decoration check precedes coordinate resolution");
    }

    // =================================================================================================
    // T7 -- a UNIT-only tile (t.building==0): resolved directly via unit_get_coords, *out_1 =
    // chain_owner|0x80, *out_2 = the unit's own index.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 3, 2);

        tile_object &tl = tile(0, 0);
        set_full_id(tl.unit, /*owner*/ 3, /*index*/ 44);

        uint32_t out_1 = 0, out_2 = 0;
        g_at_dir_returns = {4};
        sim_store own    = fx.store();
        uint32_t  ret    = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                         &out_1, &out_2);

        ck_eq(ret, 1u, "T7: unit-only candidate found -> return 1");
        ck_eq(out_1, 0x83u, "T7: *out_1 = chain_owner(3)|0x80, 0x0047bf52-0x0047bf5a");
        ck_eq(out_2, 44u, "T7: *out_2 = the unit's own index (44), 0x0047bf5c-0x0047bf62");
        ck_eq((uint32_t)g_at_bldg_coord_calls.size(), 1u,
              "T7: no candidate bldg_get_coords call -- t.building==0 skips the whole building arm");
        ck((g_at_unit_coord_calls.size() == 1 && g_at_unit_coord_calls[0].player == 3 &&
            g_at_unit_coord_calls[0].index == 44),
           "T7: unit_get_coords(owner=3,index=44), 0x0047bed2-0x0047bedf");
    }

    // =================================================================================================
    // T8 -- chain walk: the FIRST unit on the tile is friendly (player==chain_owner, skipped without
    // even consulting relation), .unit_above advances to a SECOND unit which is a genuine enemy --
    // the candidate resolved is the SECOND unit, not the first, 0x0047be6a-0x0047bebb.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 0);
        at_set_relation(fx, AT_PLAYER, 9, 2);

        tile_object &tl = tile(0, 0);
        set_full_id(tl.unit, /*owner*/ AT_PLAYER, /*index*/ 1); // friendly by identity
        set_full_id(fx.u(AT_PLAYER, 1).unit_above, /*owner*/ 9, /*index*/ 2);

        uint32_t out_1 = 0, out_2 = 0;
        g_at_dir_returns = {4};
        sim_store own    = fx.store();
        uint32_t  ret    = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                         &out_1, &out_2);

        ck_eq(ret, 1u, "T8: chain walk finds the SECOND unit -> return 1");
        ck_eq(out_1, 0x89u, "T8: *out_1 = chain_owner(9)|0x80 -- the second unit, not the first");
        ck_eq(out_2, 2u, "T8: *out_2 = 2 -- the second unit's index");
        ck((g_at_unit_coord_calls.size() == 1 && g_at_unit_coord_calls[0].player == 9 &&
            g_at_unit_coord_calls[0].index == 2),
           "T8: unit_get_coords resolves the FOUND candidate only, not the skipped friendly one");
    }

    // =================================================================================================
    // T9 -- THE ORIGINAL QUIRK: an exhausted chain (no enemy anywhere in the stack) abandons the WHOLE
    // REMAINING dy ROW, not just the current cell -- 0x0047bec1's JZ goes to 0x0047bf69 -> 0x0047bbdb
    // (the OUTER dx increment), not 0x0047bbff (the inner dy increment). dx=0,dy=-1 has an exhausted
    // friendly-only chain; dx=0,dy=0 has a unit that WOULD be a valid enemy candidate if reached, and
    // its relation is deliberately set to 2 so a "fixed" (bug-repaired) translation that used `continue`
    // instead of abandoning the row would find it and return 1 here. This oracle asserts it is NEVER
    // reached.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 1);
        at_set_relation(fx, AT_PLAYER, 3, 2); // would pass, if dy=0 were ever visited

        tile_object &exhausted = tile(0, -1);
        set_full_id(exhausted.unit, /*owner*/ AT_PLAYER, /*index*/ 1); // friendly
        set_full_id(fx.u(AT_PLAYER, 1).unit_above, 0, 0);              // chain ends (0 = no more)

        tile_object &would_find = tile(0, 0);
        set_full_id(would_find.unit, /*owner*/ 3, /*index*/ 7); // genuine enemy, never reached

        uint32_t  out_1 = 0, out_2 = 0;
        sim_store own = fx.store();
        uint32_t  ret = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                      &out_1, &out_2);

        ck_eq(ret, 0u,
              "T9: exhausted chain at dy=-1 abandons dy=0's real candidate too -- return 0, "
              "0x0047bec1/0x0047bf69 (the original quirk, not a bug to 'fix')");
        ck_eq((uint32_t)g_at_unit_coord_calls.size(), 0u,
              "T9: unit_get_coords never called -- dy=0's candidate is never evaluated");
        ck_eq((uint32_t)g_at_dir_calls.size(), 0u, "T9: dir_from_to never called for the same reason");
    }

    // =================================================================================================
    // T10 -- the heading-diff circular helper's `counter > 12` wrap (0x0047bd78-0x0047bd89 /
    // 0x0047bf2d-0x0047bf3e), pinned by letting it DECIDE the winner between two candidates: candidate
    // A (raw_dir=2, aim=1) needs 23 forward steps -- WITHOUT the wrap that reads as diff=23; WITH the
    // wrap (24-23) it is diff=1. Candidate B (raw_dir=15, aim=1) needs 10 forward steps, diff=10 either
    // way. A is evaluated first (dx=0,dy=0), B second (dx=1,dy=0). Correct wrap: A(1) beats B(10), A
    // wins. Missing wrap: A(23) loses to B(10), B would incorrectly win.
    // =================================================================================================
    {
        fx.reset();
        at_reset_recorders();
        seed_self(/*aim*/ 1, /*range*/ 1);
        at_set_relation(fx, AT_PLAYER, 3, 2);
        at_set_relation(fx, AT_PLAYER, 6, 2);

        tile_object &a = tile(0, 0);
        a.building     = 222;
        a.class_owner  = 0x03; // owner=3

        tile_object &b = tile(1, 0);
        set_full_id(b.unit, /*owner*/ 6, /*index*/ 9);

        uint32_t out_1 = 0, out_2 = 0;
        g_at_dir_returns = {2, 15}; // A's raw_dir=2 (call#1), B's raw_dir=15 (call#2)
        sim_store own    = fx.store();
        uint32_t  ret    = detail::turret_acquire_target(fx.view(), own, g_at_calls, AT_PLAYER, AT_BIDX,
                                                         &out_1, &out_2);

        ck_eq(ret, 1u, "T10: a candidate is found -> return 1");
        ck_eq(out_1, 0x03u,
              "T10: A (the wrapped diff=1) wins over B (diff=10) -- proves the counter>12 wrap fires, "
              "0x0047bd78-0x0047bd89");
        ck_eq(out_2, 222u, "T10: *out_2 = A's building id (222), NOT B's unit index (9)");
    }
}

// =====================================================================================================
// llm_strat_turret_fire @0x0047bf8e
// =====================================================================================================
void run_turret_fire_tests() {
    sim_fixture fx;

    constexpr uint16_t FR_PLAYER = 2;
    constexpr int32_t  FR_BIDX   = 6;
    constexpr int32_t  FR_SUBID  = 4;
    constexpr int32_t  FR_WEAPON = 3;

    auto seed_bldg = [&]() { fx.b(FR_PLAYER, FR_BIDX).sub_id = static_cast<uint8_t>(FR_SUBID); };

    // =================================================================================================
    // F1/F1b -- the target-bit gate: fire_kind's required bit clear -> a PURE no-op (0x0047bfed-
    // 0x0047c017), before even the ammo/reload bookkeeping runs.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret &t                    = fr_trt(fx, FR_PLAYER, FR_SUBID);
        t.weapon_id                  = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                       = 5;
        t.reload_ready_flag          = 1;
        t.reload_timer               = 999.0;
        fr_wpn(fx, FR_WEAPON).target = WEAPON_TARGET_AIR; // fire_kind=1 needs GROUND -- missing

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.ammo, 5u, "F1: target-bit gate closed -> ammo untouched, 0x0047bfed-0x0047c017");
        ck_eq((uint32_t)t.reload_ready_flag, 1u, "F1: reload_ready_flag untouched (gate closed)");
        ck_eq_d(t.reload_timer, 999.0, "F1: reload_timer untouched (gate closed)");
        ck_eq((uint32_t)g_fr_anchor_calls.size(), 0u, "F1: no dispatch calls at all -- immediate return");
    }
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret &t                    = fr_trt(fx, FR_PLAYER, FR_SUBID);
        t.weapon_id                  = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                       = 5;
        fr_wpn(fx, FR_WEAPON).target = WEAPON_TARGET_GROUND; // fire_kind=2 needs AIR -- missing

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 2);

        ck_eq((uint32_t)t.ammo, 5u, "F1b: AIR fire_kind against a GROUND-only weapon -> also a no-op");
    }

    // =================================================================================================
    // F2 -- ammo != 0 after the decrement: reload_timer = short_time; resupply bookkeeping (cycles/
    // available) is UNTOUCHED; reload_ready_flag unconditionally cleared. weapon.type=0 keeps the
    // dispatch on the default (no-op) arm so this case isolates the bookkeeping.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 0; // invalid -> default arm (JA @0x0047c147)
        w.short_time                = 11.0;
        w.long_time                 = 22.0;
        t.ammo                      = 5;
        t.reload_ready_flag         = 1;
        t.reload_timer              = 999.0;
        t.resupply_cycles_remaining = 77;
        t.resupply_available        = 1;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.ammo, 4u, "F2: ammo -= 1 (5 -> 4), 0x0047c02d");
        ck_eq_d(t.reload_timer, 11.0, "F2: ammo!=0 -> reload_timer = short_time, 0x0047c063-0x0047c069");
        ck_eq((uint32_t)t.reload_ready_flag, 0u,
              "F2: reload_ready_flag unconditionally cleared, 0x0047c11a-0x0047c12a");
        ck_eq((uint32_t)t.resupply_cycles_remaining, 77u,
              "F2: resupply_cycles_remaining UNTOUCHED on the ammo!=0 branch");
        ck_eq((uint32_t)t.resupply_available, 1u, "F2: resupply_available UNTOUCHED on the ammo!=0 branch");
        ck_eq((uint32_t)g_fr_anchor_calls.size(), 0u, "F2: type=0 -> default arm, no dispatch calls");
    }

    // =================================================================================================
    // F3 -- ammo==0 after the decrement, resupply_cycles_remaining != -1 and decrements to a NONZERO
    // value: reload_timer = long_time; resupply_available untouched.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 0;
        w.long_time                 = 33.0;
        t.ammo                      = 1;
        t.resupply_cycles_remaining = 5;
        t.resupply_available        = 1;
        t.reload_timer              = 999.0;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.ammo, 0u, "F3: ammo -= 1 (1 -> 0)");
        ck_eq((uint32_t)t.resupply_cycles_remaining, 4u,
              "F3: resupply_cycles_remaining -= 1 (5 -> 4), 0x0047c09d");
        ck_eq_d(t.reload_timer, 33.0, "F3: cycles nonzero -> reload_timer = long_time, 0x0047c0d3-0x0047c0d9");
        ck_eq((uint32_t)t.resupply_available, 1u, "F3: resupply_available UNTOUCHED (cycles didn't hit 0)");
    }

    // =================================================================================================
    // F4 -- ammo==0, resupply_cycles_remaining decrements to EXACTLY 0: resupply_available cleared to
    // 0; reload_timer is NOT touched on this branch (the else-branch's long_time assign does not run).
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 0;
        w.long_time                 = 44.0;
        t.ammo                      = 1;
        t.resupply_cycles_remaining = 1;
        t.resupply_available        = 1;
        t.reload_timer              = 999.0;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.resupply_cycles_remaining, 0u, "F4: resupply_cycles_remaining -= 1 (1 -> 0)");
        ck_eq((uint32_t)t.resupply_available, 0u,
              "F4: cycles hit exactly 0 -> resupply_available cleared, 0x0047c0e1-0x0047c0f1");
        ck_eq_d(t.reload_timer, 999.0,
                "F4: reload_timer UNTOUCHED -- the long_time assign is the ELSE of this branch");
    }

    // =================================================================================================
    // F5 -- resupply_cycles_remaining starts at -1: the decrement is SKIPPED (0x0047c084's compare
    // against -1), so it stays -1, and since -1 != 0 the else-branch (reload_timer = long_time) runs;
    // resupply_available is untouched.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 0;
        w.long_time                 = 55.0;
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        t.resupply_available        = 1;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.resupply_cycles_remaining, (uint32_t)-1,
              "F5: -1 (unlimited) is NOT decremented, stays -1, 0x0047c084-0x0047c08b");
        ck_eq_d(t.reload_timer, 55.0, "F5: -1 != 0 -> else branch (reload_timer = long_time) runs");
        ck_eq((uint32_t)t.resupply_available, 1u, "F5: resupply_available untouched");
    }

    // =================================================================================================
    // F6 -- the dispatch's OTHER invalid boundary: weapon.type=9 (AL=type-1=8 > 7, JA @0x0047c147) also
    // falls to the no-op default arm -- pinned separately from F2's type=0 to cover both ends of the
    // range check.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 9; // out of the valid 1..8 range on the high side
        w.long_time                 = 66.0;
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 0, 0, 0, 0, 0, /*fire_kind*/ 1);

        ck_eq((uint32_t)t.ammo, 0u, "F6: bookkeeping still runs for an invalid type");
        ck_eq_d(t.reload_timer, 66.0, "F6: bookkeeping still runs for an invalid type");
        ck_eq((uint32_t)g_fr_anchor_calls.size(), 0u, "F6: type=9 -> default arm, JA @0x0047c147");
        ck_eq((uint32_t)g_fr_fx_spawn_calls.size(), 0u, "F6: no fx-anim spawns either");
        ck_eq((uint32_t)g_fr_damage_calls.size(), 0u, "F6: no area damage either");
    }

    // =================================================================================================
    // F7 -- weapon.type=1 (the {1,5,8} arm), SIM_ACTIVE=0: exactly ONE projectile_spawn, anchor-slot 1
    // with axis=0(Y) then axis=1(X) (a1_1,a1_2), no sound calls (gate closed).
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        t.counter_ref               = 0x1234;
        t.counter_target_slot       = 5678;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 1;
        fx.sim_active               = 0;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, /*target_fine_x*/ 500,
                            /*target_fine_y*/ 600, /*target_elevation*/ 50, 0, 0, /*fire_kind*/ 1);

        ck((g_fr_anchor_calls.size() == 2 && g_fr_anchor_calls[0].anchor_kind == 1 &&
            g_fr_anchor_calls[0].axis == 0 && g_fr_anchor_calls[1].anchor_kind == 1 &&
            g_fr_anchor_calls[1].axis == 1),
           "F7: {1,5,8} arm reads anchor-slot 1, axis=0(Y) then axis=1(X), 0x0047c562-0x0047c582");
        ck_eq((uint32_t)g_fr_projectile_calls.size(), 1u, "F7: exactly ONE projectile_spawn, 0x0047c58e");
        const auto &pc = g_fr_projectile_calls[0];
        ck_eq((uint32_t)pc.src_x, 10112u, "F7: src_x = a1_2 (axis=1/X), 0x0047c587");
        ck_eq((uint32_t)pc.src_y_ground, 10102u, "F7: src_y_ground = a1_1 (axis=0/Y), 0x0047c58b");
        ck_eq((uint32_t)pc.src_y_vis_offset, 0u, "F7: src_y_vis_offset = 0 (XOR EDI,EDI), 0x0047c559");
        ck_eq(pc.dst_x, 500u, "F7: dst_x = target_fine_x, 0x0047c556");
        ck_eq(pc.dst_y_ground, 600u, "F7: dst_y_ground = target_fine_y, 0x0047c497");
        ck_eq((uint32_t)pc.dst_y_vis_offset, 50u, "F7: dst_y_vis_offset = target_elevation, 0x0047c493");
        ck_eq((uint32_t)pc.weapon_id, (uint32_t)FR_WEAPON, "F7: weapon_id, 0x0047c48f");
        ck_eq((uint32_t)pc.owner_player, 1u, "F7: owner_player = fire_kind, 0x0047c545-0x0047c549");
        ck_eq((uint32_t)pc.homing_flags, 0x1234u, "F7: homing_player_and_flags = t.counter_ref@0x16");
        ck_eq((uint32_t)pc.homing_target, 5678u, "F7: homing_target_unit = t.counter_target_slot@0x18");
        ck_eq(pc.shooter_ref, 0x42u, "F7: shooter_ref = player|0x40 (2|0x40), 0x0047c4df-0x0047c4e4");
        ck_eq((uint32_t)pc.shooter_unit_index, (uint32_t)FR_BIDX, "F7: shooter_unit_index = bldg_index");
        ck_eq((uint32_t)g_fr_snd_calls.size(), 0u, "F7: SIM_ACTIVE=0 -> no sound calls, 0x0047c593-0x0047c59a");
    }

    // =================================================================================================
    // F7b -- weapon.type=5 (same {1,5,8} arm), SIM_ACTIVE!=0: two MORE anchor calls feed the ONE
    // snd_play_at(sound_fire, sa2/32, sa1/32) record (the original offscreen_snd_volume+snd_play
    // pair, fused since the LIFT-NOTIFY offscreen conversion).
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 5;
        w.sound_fire                = 777;
        fx.sim_active               = 1;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 500, 600, 50, 0, 0,
                            /*fire_kind*/ 1);

        ck_eq((uint32_t)g_fr_anchor_calls.size(), 4u,
              "F7b: 2 (projectile source) + 2 more (sound pan), 0x0047c59c-0x0047c5d1");
        ck((g_fr_snd_calls.size() == 1 && g_fr_snd_calls[0].sound_id == 777 &&
            g_fr_snd_calls[0].col == 316 && g_fr_snd_calls[0].row == 315),
           "F7b: snd_play_at(w.sound_fire, sa2/32=316, sa1/32=315), 0x0047c5ec-0x0047c601");
    }

    // =================================================================================================
    // F8 -- weapon.type=3 (the scatter/area-damage arm), fire_kind=2 (AIR), SIM_ACTIVE!=0. Anchor-slot
    // 1 with axis=1(X) then axis=0(Y) -- REVERSED from the {1,5,8} arm's own axis order, a case that
    // would fail if the two arms' axis literals were mixed up. impact_y_ground (elevation-subtracted)
    // feeds the sound pan; impact_y_FULL (no elevation) feeds apply_area_damage -- distinguished by a
    // nonzero target_elevation.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 3;
        w.missing[FR_PLAYER]        = 15;
        w.fite_explo                = 300;
        w.target_explo              = 400;
        w.power[FR_PLAYER]          = 8.5;
        w.fire_range[FR_PLAYER]     = 12.9; // trunc -> 12
        w.area_damage_owner_filter  = 9;
        w.sound_fire                = 501;
        w.sound_target              = 502;
        fx.sim_active               = 1;
        fx.game_clock               = 12.5;
        fx.geom.bw_mask             = 0x1ffff;
        fx.geom.bh_mask             = 0x1ffff;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, /*target_fine_x*/ 1000,
                            /*target_fine_y*/ 2000, /*target_elevation*/ 64, 0, 0, /*fire_kind*/ 2);

        ck((g_fr_anchor_calls.size() == 2 && g_fr_anchor_calls[0].anchor_kind == 1 &&
            g_fr_anchor_calls[0].axis == 1 && g_fr_anchor_calls[1].anchor_kind == 1 &&
            g_fr_anchor_calls[1].axis == 0),
           "F8: {3} arm reads anchor-slot 1, axis=1(X) then axis=0(Y) -- REVERSED vs {1,5,8}, "
           "0x0047c1f0-0x0047c20d");
        ck((g_fr_scatter_calls.size() == 1 && g_fr_scatter_calls[0].missing == 15 &&
            g_fr_scatter_calls[0].anchor_x == 10112u && g_fr_scatter_calls[0].anchor_y == 10102u &&
            g_fr_scatter_calls[0].x == 1000 && g_fr_scatter_calls[0].y == 2000),
           "F8: weapon_scatter_offset(0, missing, a1_1=X, a1_2=Y, target_x, target_y), "
           "0x0047c22f-0x0047c25d");
        ck_eq((uint32_t)g_fr_fx_spawn_calls.size(), 2u, "F8: two fx_anim_spawn calls, muzzle + impact");
        const auto &muzzle = g_fr_fx_spawn_calls[0];
        ck_eq(muzzle.x, 10112u, "F8: muzzle fx-anim x = a1_1(X), 0x0047c1de");
        ck_eq(muzzle.y, 10102u, "F8: muzzle fx-anim y = a1_2(Y), 0x0047c1db");
        ck_eq(muzzle.frame, 321u, "F8: frame = fite_explo(300) + (dir(4)-1)*stride(7) = 321, 0x0047c1c9-0x0047c1e1");
        ck_eq(muzzle.p5, 1u, "F8: muzzle fx-anim p5 = literal 1, not fire_kind, 0x0047bfaf/0x0047c1a4");
        const auto &impact = g_fr_fx_spawn_calls[1];
        ck_eq(impact.x, 1005u, "F8: impact_x = (target_fine_x(1000)+scatter_dx(5)) & bw_mask, 0x0047c2a6-0x0047c2b4");
        ck_eq(impact.y, 1933u,
              "F8: impact fx-anim y = impact_y_GROUND (elevation-subtracted) = "
              "(2000-3-64)&bh_mask = 1933, 0x0047c2b7-0x0047c2d9");
        ck_eq(impact.frame, 400u, "F8: impact frame = w.target_explo, 0x0047c2ed-0x0047c300");
        ck_eq(impact.p5, 2u, "F8: impact fx-anim p5 = fire_kind(2), 0x0047c2dc-0x0047c2e0");
        ck((g_fr_snd_calls.size() == 2 && g_fr_snd_calls[0].sound_id == 501 &&
            g_fr_snd_calls[1].sound_id == 502 && g_fr_snd_calls[0].col == 31 &&
            g_fr_snd_calls[0].row == 60 && g_fr_snd_calls[1].col == 31 &&
            g_fr_snd_calls[1].row == 60),
           "F8: snd_play_at(sound_fire) then snd_play_at(sound_target), BOTH at (impact_x/32=31, "
           "impact_y_GROUND/32=60), 0x0047c312-0x0047c38b");
        ck_eq((uint32_t)g_fr_damage_calls.size(), 1u, "F8: exactly ONE apply_area_damage call");
        const auto &dmg = g_fr_damage_calls[0];
        ck_eq((uint32_t)dmg.x, 31u, "F8: damage x = impact_x/32 = 31, 0x0047c400-0x0047c40e");
        ck_eq((uint32_t)dmg.y, 62u,
              "F8: damage y = impact_y_FULL/32 = (2000-3)&mask/32 = 1997/32 = 62 -- NOT "
              "impact_y_ground(60), 0x0047c3ed-0x0047c3fe");
        ck_eq((uint32_t)dmg.target_kind, 2u, "F8: target_kind = fire_kind, 0x0047c3e9");
        ck_eq_d(dmg.damage, 8.5, "F8: damage = w.power[player], 0x0047c3cd-0x0047c3e3");
        ck_eq((uint32_t)dmg.ring_count, 12u, "F8: ring_count = trunc(w.fire_range[player]) = 12, 0x0047c3c2-0x0047c3c7");
        ck_eq(dmg.owner_filter, 9u, "F8: owner_filter = w.area_damage_owner_filter, 0x0047c3ab");
        ck_eq(dmg.killer_info, 0x42u, "F8: killer_info = player|0x40, 0x0047c394-0x0047c39c");
        ck_eq((uint32_t)dmg.killer_unit_index, (uint32_t)FR_BIDX, "F8: killer_unit_index = bldg_index, 0x0047c390");
    }

    // =================================================================================================
    // F9 -- weapon.type=2 FALLS THROUGH into the {1,5,8} arm: TWO real projectile_spawn calls (NOT a
    // redundant duplicate) -- one at anchor-slot 2, one at anchor-slot 1, each with its own axis pair.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 2;
        fx.sim_active               = 0;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 100, 200, 0, 0, 0,
                            /*fire_kind*/ 1);

        ck((g_fr_anchor_calls.size() == 4 && g_fr_anchor_calls[0].anchor_kind == 2 &&
            g_fr_anchor_calls[0].axis == 0 && g_fr_anchor_calls[1].anchor_kind == 2 &&
            g_fr_anchor_calls[1].axis == 1 && g_fr_anchor_calls[2].anchor_kind == 1 &&
            g_fr_anchor_calls[2].axis == 0 && g_fr_anchor_calls[3].anchor_kind == 1 &&
            g_fr_anchor_calls[3].axis == 1),
           "F9: anchor-slot 2 pair THEN anchor-slot 1 pair -- proves the {2,6}->{1,5,8} fallthrough, "
           "0x0047c4a2-0x0047c582");
        ck_eq((uint32_t)g_fr_projectile_calls.size(), 2u,
              "F9: TWO real projectile_spawn calls, not a redundant duplicate, 0x0047c4d3/0x0047c58e");
        ck_eq((uint32_t)g_fr_projectile_calls[0].src_x, 10212u, "F9: call#1 src_x = anchor-slot-2's a2_2(X)");
        ck_eq((uint32_t)g_fr_projectile_calls[0].src_y_ground, 10202u, "F9: call#1 src_y = anchor-slot-2's a2_1(Y)");
        ck_eq((uint32_t)g_fr_projectile_calls[1].src_x, 10112u, "F9: call#2 src_x = anchor-slot-1's a1_2(X)");
        ck_eq((uint32_t)g_fr_projectile_calls[1].src_y_ground, 10102u, "F9: call#2 src_y = anchor-slot-1's a1_1(Y)");
    }

    // =================================================================================================
    // F10 -- weapon.type=6 reaches the SAME {2,6}->{1,5,8} dispatch shape as type=2 (F9), pinning the
    // OTHER half of the case-value -> physical-target mapping the header derives indirectly.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 6;
        fx.sim_active               = 0;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 100, 200, 0, 0, 0,
                            /*fire_kind*/ 1);

        ck_eq((uint32_t)g_fr_projectile_calls.size(), 2u,
              "F10: type=6 also falls through to {1,5,8} -- two projectile_spawn calls");
    }

    // =================================================================================================
    // F11 -- weapon.type=4 FALLS THROUGH into the {3} arm: ONE fx-anim at anchor-slot 2 (case4's own,
    // axis=1(X) then axis=0(Y)) THEN case3's own TWO fx-anims (muzzle at anchor-slot 1, impact) plus
    // its scatter offset and area damage -- three fx_anim_spawn calls total, not two.
    // =================================================================================================
    {
        fx.reset();
        fr_reset_recorders();
        seed_bldg();
        turret     &t               = fr_trt(fx, FR_PLAYER, FR_SUBID);
        cfg_weapon &w               = fr_wpn(fx, FR_WEAPON);
        t.weapon_id                 = static_cast<uint8_t>(FR_WEAPON);
        t.ammo                      = 1;
        t.resupply_cycles_remaining = -1;
        w.target                    = WEAPON_TARGET_GROUND | WEAPON_TARGET_AIR;
        w.type                      = 4;
        w.missing[FR_PLAYER]        = 20;
        w.fite_explo                = 300;
        w.target_explo              = 400;
        w.power[FR_PLAYER]          = 3.0;
        w.fire_range[FR_PLAYER]     = 5.0;
        fx.sim_active               = 0; // keep this case focused on the fallthrough shape
        fx.game_clock               = 8.0;
        fx.geom.bw_mask             = 0x1ffff;
        fx.geom.bh_mask             = 0x1ffff;

        sim_store own = fx.store();
        detail::turret_fire(fx.view(), own, g_fr_calls, FR_PLAYER, FR_BIDX, 50, 60, 10, 0, 0,
                            /*fire_kind*/ 1);

        ck((g_fr_anchor_calls.size() == 4 && g_fr_anchor_calls[0].anchor_kind == 2 &&
            g_fr_anchor_calls[0].axis == 1 && g_fr_anchor_calls[1].anchor_kind == 2 &&
            g_fr_anchor_calls[1].axis == 0 && g_fr_anchor_calls[2].anchor_kind == 1 &&
            g_fr_anchor_calls[2].axis == 1 && g_fr_anchor_calls[3].anchor_kind == 1 &&
            g_fr_anchor_calls[3].axis == 0),
           "F11: anchor-slot 2 (X,Y) THEN anchor-slot 1 (X,Y) -- the {4}->{3} fallthrough, "
           "0x0047c165-0x0047c20d");
        ck_eq((uint32_t)g_fr_dir_calls.size(), 2u, "F11: dir_from_to called once per arm (case4, case3)");
        ck_eq((uint32_t)g_fr_fx_spawn_calls.size(), 3u,
              "F11: THREE fx_anim_spawn calls (case4's muzzle + case3's OWN muzzle + case3's impact), "
              "0x0047c1e1/0x0047c1e6-0x0047c300");
        ck_eq(g_fr_fx_spawn_calls[0].x, 10212u, "F11: case4's own fx-anim uses anchor-slot 2 (a2_1=X)");
        ck_eq(g_fr_fx_spawn_calls[0].p5, 1u, "F11: case4's own fx-anim p5 = literal 1");
        ck_eq(g_fr_fx_spawn_calls[1].x, 10112u, "F11: case3's own muzzle fx-anim uses anchor-slot 1 (a1_1=X)");
        ck_eq(g_fr_fx_spawn_calls[1].p5, 1u, "F11: case3's own muzzle fx-anim p5 = literal 1 too");
        ck_eq(g_fr_fx_spawn_calls[0].frame, g_fr_fx_spawn_calls[1].frame,
              "F11: case4's and case3's own muzzle frames are IDENTICAL (same fite_explo/dir/stride)");
        ck_eq(g_fr_fx_spawn_calls[2].frame, 400u, "F11: the THIRD (impact) fx-anim uses w.target_explo, a DIFFERENT field");
        ck_eq(g_fr_fx_spawn_calls[2].p5, 1u, "F11: impact fx-anim p5 = fire_kind(1) here (coincides with the literal)");
        ck_eq((uint32_t)g_fr_scatter_calls.size(), 1u, "F11: exactly one weapon_scatter_offset (case3's own)");
        ck_eq((uint32_t)g_fr_damage_calls.size(), 1u, "F11: exactly one apply_area_damage (case3's own)");
    }
}

} // namespace mh::sim::test
