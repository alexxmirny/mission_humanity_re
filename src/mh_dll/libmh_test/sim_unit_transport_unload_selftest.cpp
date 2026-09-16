#include "sim/sim_unit_transport_unload.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Sign-extends a signed-byte soldier field (start_x/start_y/cur_x/cur_y/end_x/end_y are all int8_t)
// to a uint32_t the way ck_eq's bit-pattern comparison expects, so both "got" and "want" go through
// the same conversion for a negative test value.
inline uint32_t su(int8_t v) { return static_cast<uint32_t>(static_cast<int32_t>(v)); }

// soldier_at()/unit_at() are sim_store methods (need a store()'d handle); this is a plain-fixture
// convenience for SEEDING soldier records before a test calls f.view()/f.store(), same role f.u()/
// f.b() play for units/buildings -- sim_test_support.h has no soldier equivalent yet.
soldier &soldier_of(sim_fixture &f, uint32_t player, int32_t idx) {
    return f.soldiers[player * SOLDIERS_PER_PLAYER + static_cast<uint32_t>(idx)];
}

constexpr uint16_t PLAYER       = 2;
constexpr uint16_t PLAYER_LOCAL = 7; // == sim_fixture::reset()'s player_side default

// ---- recorders -----------------------------------------------------------------------------------
// unit_transport_unload_calls is the ONE struct type both llm_unit_transport_unload_field and
// _docked take (field uses all ten members, docked uses five -- see the header's calling-convention
// note) -- two separate recorders / two separate rec_*_calls() functions feed it, one per target
// function, same "one recorder struct per function" shape sim_prod_completion_selftest.cpp's pc_
// recorder/da_recorder/sa_recorder use for three DIFFERENT struct types.
struct ev2 {
    int32_t a, b;
};
struct ev3 {
    int32_t a, b, c;
};
struct ev4 {
    int32_t a, b, c, d;
};

// ==== llm_unit_transport_unload_field's own calls ==================================================
struct uf_recorder {
    std::vector<ev4> spawn_on_tile_calls;           // (tile_x, tile_y, soldier_proto_id, player)
    std::vector<ev4> dir_from_to_calls;             // (origin_x<<5, origin_y<<5, tile_x<<5, tile_y<<5)
    std::vector<ev3> lead_calls;                    // (player, transport's unit_above word, dir_to_tile)
    std::vector<ev3> soldier_unlink_calls;          // (player, tidx, lead_soldier)
    std::vector<ev3> anim_offset_calls;             // (cursor_state_index, applied_x, applied_y)
    std::vector<ev2> walk_anim_calls;               // (player, unit_index) -- one per spawn PLUS one tail call
    std::vector<ev3> ctrl_contains_calls;           // (tidx, ctrl_groups[0].count, group_index=0)
    std::vector<ev2> ctrl_assign_calls;             // (unit_id, new_group_id)
    std::vector<ev4> lookup_calls;                  // (table_col=remaining_after, table_row=i, written_a, written_b)
    int32_t          spawn_docked_unexpected_n = 0; // field's own struct member is never called

    // control knobs
    int32_t  spawn_fail_at_call = 0; // 1-based spawn_on_tile call index that returns 0; 0 = never fails
    int32_t  dir_from_to_ret    = 0;
    uint32_t lead_ret           = 0;
    int8_t   anim_out_x         = 0; // cursor_apply_anim_frame_offset's absolute out-param write
    int8_t   anim_out_y         = 0;
    int32_t  ctrl_contains_ret  = 0;
    int32_t  lookup_out_a_base  = 0; // cursor_lookup_offset_pair writes (base + table_row) into each out
    int32_t  lookup_out_b_base  = 0;

    void reset() { *this = uf_recorder{}; }
};
uf_recorder g_uf;

const unit_transport_unload_calls &rec_uf_calls() {
    static const unit_transport_unload_calls c = {
        [](uint32_t x, uint32_t y, uint16_t proto, uint16_t player) -> int32_t {
            g_uf.spawn_on_tile_calls.push_back(
                {(int32_t)x, (int32_t)y, (int32_t)proto, (int32_t)player});
            const size_t n = g_uf.spawn_on_tile_calls.size();
            if (g_uf.spawn_fail_at_call != 0 && n == (size_t)g_uf.spawn_fail_at_call) return 0;
            return (int32_t)(50 + n); // small, distinct, roster-index-safe spawned ids
        },
        [](uint16_t, uint16_t, uint32_t) -> int32_t { // spawn_docked -- field never calls this member
            g_uf.spawn_docked_unexpected_n++;
            return 0;
        },
        [](int32_t x1, int32_t y1, int32_t x2, int32_t y2) -> int32_t {
            g_uf.dir_from_to_calls.push_back({x1, y1, x2, y2});
            return g_uf.dir_from_to_ret;
        },
        [](int32_t player, uint32_t unit_above_word, uint32_t dir) -> uint32_t {
            g_uf.lead_calls.push_back({player, (int32_t)unit_above_word, (int32_t)dir});
            return g_uf.lead_ret;
        },
        [](uint16_t player, int32_t unit_idx, uint32_t soldier_idx) {
            g_uf.soldier_unlink_calls.push_back({player, unit_idx, (int32_t)soldier_idx});
        },
        [](char *out_x, char *out_y, int32_t cursor_state_index) {
            g_uf.anim_offset_calls.push_back({cursor_state_index, g_uf.anim_out_x, g_uf.anim_out_y});
            *reinterpret_cast<int8_t *>(out_x) = g_uf.anim_out_x;
            *reinterpret_cast<int8_t *>(out_y) = g_uf.anim_out_y;
        },
        [](uint32_t player, int32_t unit_index) {
            g_uf.walk_anim_calls.push_back({(int32_t)player, unit_index});
        },
        [](uint32_t unit_id, int32_t count, int32_t group_index) -> int32_t {
            g_uf.ctrl_contains_calls.push_back({(int32_t)unit_id, count, group_index});
            return g_uf.ctrl_contains_ret;
        },
        [](int32_t unit_id, int32_t new_group_id) {
            g_uf.ctrl_assign_calls.push_back({unit_id, new_group_id});
        },
        [](int32_t table_col, int32_t table_row, char *out_a, char *out_b) {
            const int32_t wa = g_uf.lookup_out_a_base + table_row;
            const int32_t wb = g_uf.lookup_out_b_base + table_row;
            g_uf.lookup_calls.push_back({table_col, table_row, wa, wb});
            *reinterpret_cast<int8_t *>(out_a) = (int8_t)wa;
            *reinterpret_cast<int8_t *>(out_b) = (int8_t)wb;
        },
    };
    return c;
}

// ==== llm_unit_transport_unload_docked's own calls ==================================================
struct ud_recorder {
    std::vector<ev4> spawn_docked_calls;   // (soldier_proto_id, player, probe_slot, returned_id)
    std::vector<ev3> soldier_unlink_calls; // (player, tidx, unit_above word of transport)
    std::vector<ev3> ctrl_contains_calls;  // (tidx, ctrl_groups[0].count, group_index=0)
    std::vector<ev2> ctrl_assign_calls;    // (unit_id, new_group_id)
    std::vector<ev4> lookup_calls;         // (table_col, table_row, written_a, written_b)
    // docked's own struct only ever calls 5 of the 10 members (see the header's calling-convention
    // note) -- this is the sum of every call into the other five (spawn_on_tile, dir_from_to,
    // squad_pick_lead_soldier_in_direction, cursor_apply_anim_frame_offset, soldiers_start_walk_anim),
    // which per the header must stay 0 for the WHOLE run.
    int32_t unexpected_call_n = 0;

    // control knobs
    int32_t spawn_fail_at_call = 0; // 1-based spawn_docked call index that returns 0; 0 = never fails
    int32_t ctrl_contains_ret  = 0;
    int32_t lookup_out_a_base  = 0;
    int32_t lookup_out_b_base  = 0;

    void reset() { *this = ud_recorder{}; }
};
ud_recorder g_ud;

const unit_transport_unload_calls &rec_ud_calls() {
    static const unit_transport_unload_calls c = {
        [](uint32_t, uint32_t, uint16_t, uint16_t) -> int32_t { // spawn_on_tile -- docked never calls it
            g_ud.unexpected_call_n++;
            return 0;
        },
        [](uint16_t proto, uint16_t player, uint32_t probe_slot) -> int32_t {
            const size_t  n = g_ud.spawn_docked_calls.size() + 1;
            const int32_t ret =
                (g_ud.spawn_fail_at_call != 0 && n == (size_t)g_ud.spawn_fail_at_call)
                    ? 0
                    : (int32_t)(60 + n); // small, distinct, roster-index-safe spawned ids
            g_ud.spawn_docked_calls.push_back(
                {(int32_t)proto, (int32_t)player, (int32_t)probe_slot, ret});
            return ret;
        },
        [](int32_t, int32_t, int32_t, int32_t) -> int32_t { // dir_from_to -- docked never calls it
            g_ud.unexpected_call_n++;
            return 0;
        },
        [](int32_t, uint32_t, uint32_t) -> uint32_t { // squad_pick_lead_soldier_in_direction -- unused
            g_ud.unexpected_call_n++;
            return 0;
        },
        [](uint16_t player, int32_t unit_idx, uint32_t soldier_idx) {
            g_ud.soldier_unlink_calls.push_back({player, unit_idx, (int32_t)soldier_idx});
        },
        [](char *, char *, int32_t) { // cursor_apply_anim_frame_offset -- docked never calls it
            g_ud.unexpected_call_n++;
        },
        [](uint32_t, int32_t) { // soldiers_start_walk_anim -- docked's tail never calls it (confirmed
                                // absent from the disassembly, see the header FINDING)
            g_ud.unexpected_call_n++;
        },
        [](uint32_t unit_id, int32_t count, int32_t group_index) -> int32_t {
            g_ud.ctrl_contains_calls.push_back({(int32_t)unit_id, count, group_index});
            return g_ud.ctrl_contains_ret;
        },
        [](int32_t unit_id, int32_t new_group_id) {
            g_ud.ctrl_assign_calls.push_back({unit_id, new_group_id});
        },
        [](int32_t table_col, int32_t table_row, char *out_a, char *out_b) {
            const int32_t wa = g_ud.lookup_out_a_base + table_row;
            const int32_t wb = g_ud.lookup_out_b_base + table_row;
            g_ud.lookup_calls.push_back({table_col, table_row, wa, wb});
            *reinterpret_cast<int8_t *>(out_a) = (int8_t)wa;
            *reinterpret_cast<int8_t *>(out_b) = (int8_t)wb;
        },
    };
    return c;
}

// ==== llm_unit_transport_unload_field @0x00488a8b ===================================================
// From the header banner: remaining-seed + soldier_proto_id read BEFORE any decrement (HAZARD 1),
// origin snapshot read once, energy/experience share computed ONCE off the INITIAL capacity and
// REUSED every iteration, `while (dir<8 && remaining>1)` with the 8-direction terrain search
// (passable && !building), spawn -> chain-head reseed via the picked lead soldier + cursor anim
// offset -> transport decrement -> new-unit fields -> local-player ctrl-group propagation, then the
// tail's HAZARD-1 re-read + chain walk that refreshes the transport's OWN start_x/y from its OWN
// (untouched) cur_x/y.

void test_field_zero_iterations_capacity_le_one() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX                = 21;
    f.u(PLAYER, TIDX).unit_proto_id       = 40;
    f.cfg_units[40].soldier_count         = 1; // remaining seed = 1 -> `remaining > 1` false -> no loop body
    f.u(PLAYER, TIDX).unit_above[0]       = 5;
    f.u(PLAYER, TIDX).unit_above[1]       = 0;
    soldier_of(f, PLAYER, 5).next_soldier = 0;
    soldier_of(f, PLAYER, TIDX).cur_x     = 9;
    soldier_of(f, PLAYER, TIDX).cur_y     = -9;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.spawn_on_tile_calls.size(), 0u, "field(remaining<=1): no spawn attempts at all");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 40u,
          "field(remaining<=1): transport's proto_id untouched (no loop -> no decrement)");
    // tail still runs unconditionally: remaining_after = cfg_units[40].soldier_count (unchanged row) = 1
    ck_eq((uint32_t)g_uf.lookup_calls.size(), 1u, "field(remaining<=1): tail still runs exactly once");
    ck(g_uf.lookup_calls[0].a == 1 && g_uf.lookup_calls[0].b == 1,
       "field(remaining<=1): cursor_lookup_offset_pair(table_col=remaining_after=1, table_row=1, ..)");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_x), su(9),
          "field: tail refreshes transport's own start_x from its OWN (untouched) cur_x");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_y), su(-9),
          "field: tail refreshes transport's own start_y from its OWN (untouched) cur_y");
    ck((uint32_t)g_uf.walk_anim_calls.size() == 1u && g_uf.walk_anim_calls[0].a == PLAYER &&
           g_uf.walk_anim_calls[0].b == TIDX,
       "field: the tail's unconditional soldiers_start_walk_anim(player, tidx) still fires");
}

void test_field_successful_spawn_writes_skips_and_tail_hazard1() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX           = 22;
    f.u(PLAYER, TIDX).unit_proto_id  = 50;
    f.cfg_units[50].soldier_count    = 3; // remaining seed=3 -> soldier_proto_id=(50-3+1)=48
    f.u(PLAYER, TIDX).x              = 50;
    f.u(PLAYER, TIDX).y              = 50;
    f.u(PLAYER, TIDX).energy         = 90.0; // energy_share = 90/3 = 30.0 exactly
    f.u(PLAYER, TIDX).experience     = 100;  // experience_share = 100/3 = 33 (int division)
    f.u(PLAYER, TIDX).move_microstep = 8;
    f.u(PLAYER, TIDX).unit_above[0]  = 60; // chain head for the tail
    f.u(PLAYER, TIDX).unit_above[1]  = 0;

    // dir0 -> (51,50): impassable (fixture default passable=0) -> SKIP.
    // dir1 -> (52,50): passable but a building sits on it -> SKIP.
    // dir2 -> (53,50): passable, no building -> the one successful spawn.
    // dirs 3..7 default {dx:0,dy:0} -> (50,50), impassable by default -> SKIP.
    f.dir8_offsets[0]            = {1, 0};
    f.dir8_offsets[1]            = {2, 0};
    f.passable[(52u << 8) | 50u] = 1;
    f.t(52, 50).building         = 7;
    f.dir8_offsets[2]            = {3, 0};
    f.passable[(53u << 8) | 50u] = 1;

    g_uf.dir_from_to_ret   = 5;
    g_uf.lead_ret          = 15;
    g_uf.anim_out_x        = 33;
    g_uf.anim_out_y        = -44;
    g_uf.lookup_out_a_base = 10;
    g_uf.lookup_out_b_base = 20;

    // spawn_on_tile's mock returns 50+call_n -> first (and only) successful call returns 51.
    f.u(PLAYER, 51).unit_above[0]     = 39; // new_chain_head
    f.u(PLAYER, 51).unit_above[1]     = 0;
    soldier_of(f, PLAYER, 15).start_x = 11; // the picked lead soldier
    soldier_of(f, PLAYER, 15).start_y = -22;

    // tail setup: after the one split, unit_proto_id = 49 (HAZARD 1 -- a DIFFERENT cfg row).
    f.cfg_units[49].soldier_count          = 2; // remaining_after, deliberately DISTINCT from the seed(3)
    soldier_of(f, PLAYER, 60).next_soldier = 61;
    soldier_of(f, PLAYER, 61).next_soldier = 0;
    soldier_of(f, PLAYER, TIDX).cur_x      = 5;
    soldier_of(f, PLAYER, TIDX).cur_y      = 6;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.spawn_on_tile_calls.size(), 1u,
          "field: exactly one spawn attempt across all 8 directions (2 skipped, 1 succeeds, 5 default-skip)");
    ck(g_uf.spawn_on_tile_calls[0].a == 53 && g_uf.spawn_on_tile_calls[0].b == 50 &&
           g_uf.spawn_on_tile_calls[0].c == 48 && g_uf.spawn_on_tile_calls[0].d == PLAYER,
       "field: spawn_on_tile(tile_x=53, tile_y=50, soldier_proto_id=48, player)");
    ck((uint32_t)g_uf.dir_from_to_calls.size() == 1u && g_uf.dir_from_to_calls[0].a == (50 << 5) &&
           g_uf.dir_from_to_calls[0].b == (50 << 5) && g_uf.dir_from_to_calls[0].c == (53 << 5) &&
           g_uf.dir_from_to_calls[0].d == (50 << 5),
       "field: dir_from_to(origin_x<<5, origin_y<<5, tile_x<<5, tile_y<<5)");
    ck((uint32_t)g_uf.lead_calls.size() == 1u && g_uf.lead_calls[0].a == PLAYER &&
           g_uf.lead_calls[0].b == 60 && g_uf.lead_calls[0].c == 5,
       "field: squad_pick_lead_soldier_in_direction(player, transport's unit_above word=60, dir_to_tile=5)");

    soldier &nch = soldier_of(f, PLAYER, 39);
    ck_eq(su(nch.start_x), su(33), "field: new chain-head start_x ends at the cursor-offset overwrite (33)");
    ck_eq(su(nch.start_y), su(-44), "field: new chain-head start_y ends at the cursor-offset overwrite (-44)");
    ck_eq(su(nch.end_x), su(0x10), "field: new chain-head end_x hardcoded to 0x10");
    ck_eq(su(nch.end_y), su(0x10), "field: new chain-head end_y hardcoded to 0x10");
    ck_eq(su(nch.cur_x), su(33), "field: new chain-head cur_x = the (post-anim-offset) start_x");
    ck_eq(su(nch.cur_y), su(-44), "field: new chain-head cur_y = the (post-anim-offset) start_y");

    ck((uint32_t)g_uf.soldier_unlink_calls.size() == 1u && g_uf.soldier_unlink_calls[0].a == PLAYER &&
           g_uf.soldier_unlink_calls[0].b == TIDX && g_uf.soldier_unlink_calls[0].c == 15,
       "field: soldier_unlink(player, tidx, lead_soldier=15)");
    ck((uint32_t)g_uf.anim_offset_calls.size() == 1u && g_uf.anim_offset_calls[0].a == 5,
       "field: cursor_apply_anim_frame_offset's cursor_state_index == dir_to_tile(5)");

    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 49u, "field: transport's unit_proto_id decremented once");
    ck_eq_d(f.u(PLAYER, TIDX).energy, 60.0, "field: transport.energy -= energy_share(30.0) -> 60.0");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).experience, 67u, "field: transport.experience -= experience_share(33) -> 67");

    unit &spawned = f.u(PLAYER, 51);
    ck_eq_d(spawned.energy, 30.0, "field: spawned unit's energy = energy_share(30.0)");
    ck_eq((uint32_t)spawned.experience, 33u, "field: spawned unit's experience = experience_share(33)");
    ck_eq((uint32_t)spawned.move_microstep, 8u, "field: spawned unit's move_microstep copied from the transport");
    ck_eq((uint32_t)spawned.facing_target, 5u, "field: spawned unit's facing_target = dir_to_tile(5)");
    ck_eq((uint32_t)spawned.facing_current, 5u, "field: spawned unit's facing_current = SAME dir_to_tile(5)");

    ck((uint32_t)g_uf.walk_anim_calls.size() == 2u && g_uf.walk_anim_calls[0].a == PLAYER &&
           g_uf.walk_anim_calls[0].b == 51 && g_uf.walk_anim_calls[1].a == PLAYER &&
           g_uf.walk_anim_calls[1].b == TIDX,
       "field: soldiers_start_walk_anim fires once for the spawned unit(51) in the loop, once for the "
       "transport(tidx) in the tail");

    ck_eq((uint32_t)g_uf.ctrl_contains_calls.size(), 0u,
          "field: non-local player -> ctrl-group block never entered");
    ck_eq((uint32_t)g_uf.ctrl_assign_calls.size(), 0u, "field: non-local player -> no ctrl_group_assign");

    // tail: remaining_after=2 (from the REDUCED proto_id's cfg row), chain 60 -> 61 -> 0.
    ck((uint32_t)g_uf.lookup_calls.size() == 2u && g_uf.lookup_calls[0].a == 2 && g_uf.lookup_calls[0].b == 1 &&
           g_uf.lookup_calls[1].a == 2 && g_uf.lookup_calls[1].b == 2,
       "field: tail walks exactly remaining_after(2) nodes, cursor_lookup_offset_pair(2, i, ..)");
    ck_eq(su(soldier_of(f, PLAYER, 60).end_x), su(11), "field: tail node 60's end_x written by the lookup");
    ck_eq(su(soldier_of(f, PLAYER, 60).end_y), su(21), "field: tail node 60's end_y written by the lookup");
    ck_eq(su(soldier_of(f, PLAYER, 61).end_x), su(12), "field: tail node 61's end_x written by the lookup");
    ck_eq(su(soldier_of(f, PLAYER, 61).end_y), su(22), "field: tail node 61's end_y written by the lookup");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_x), su(5),
          "field: transport's own start_x refreshed from its OWN (untouched) cur_x every tail iteration");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).cur_x), su(5), "field: transport's own cur_x is left UNTOUCHED");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_y), su(6), "field: transport's own start_y = its OWN cur_y");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).cur_y), su(6), "field: transport's own cur_y is left UNTOUCHED");
}

void test_field_spawn_failure_aborts_remaining() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX                 = 23;
    f.u(PLAYER, TIDX).unit_proto_id        = 60;
    f.cfg_units[60].soldier_count          = 4; // remaining seed = 4
    f.u(PLAYER, TIDX).x                    = 10;
    f.u(PLAYER, TIDX).y                    = 10;
    f.u(PLAYER, TIDX).energy               = 40.0;
    f.u(PLAYER, TIDX).experience           = 40;
    f.dir8_offsets[0]                      = {0, 0}; // tile stays (10,10)
    f.passable[(10u << 8) | 10u]           = 1;      // passable -> spawn IS attempted
    f.u(PLAYER, TIDX).unit_above[0]        = 70;
    f.u(PLAYER, TIDX).unit_above[1]        = 0;
    soldier_of(f, PLAYER, 70).next_soldier = 71;
    soldier_of(f, PLAYER, 71).next_soldier = 72;
    soldier_of(f, PLAYER, 72).next_soldier = 73;
    soldier_of(f, PLAYER, 73).next_soldier = 0;

    g_uf.spawn_fail_at_call = 1; // the ONE attempted spawn fails

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.spawn_on_tile_calls.size(), 1u,
          "field(spawn fails): remaining forced to 0 -> the rest of the 8-direction sweep aborts");
    ck_eq((uint32_t)g_uf.dir_from_to_calls.size(), 0u, "field(spawn fails): never reaches dir_from_to");
    ck_eq((uint32_t)g_uf.soldier_unlink_calls.size(), 0u, "field(spawn fails): never reaches soldier_unlink");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 60u,
          "field(spawn fails): the abort path skips the proto_id decrement entirely");
    ck_eq_d(f.u(PLAYER, TIDX).energy, 40.0, "field(spawn fails): transport.energy untouched");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).experience, 40u, "field(spawn fails): transport.experience untouched");
    // tail still runs off the UNCHANGED proto_id's cfg row (remaining_after = 4, same as the seed).
    ck_eq((uint32_t)g_uf.lookup_calls.size(), 4u,
          "field(spawn fails): the tail still walks remaining_after(4) nodes off the unchanged cfg row");
}

void test_field_energy_experience_share_hoisted_and_reused_across_two_spawns() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX          = 24;
    f.u(PLAYER, TIDX).unit_proto_id = 30;
    f.cfg_units[30].soldier_count   = 3; // remaining seed=3 -> exactly 2 successful splits (3->2->1)
    f.u(PLAYER, TIDX).x             = 0;
    f.u(PLAYER, TIDX).y             = 0;
    f.u(PLAYER, TIDX).energy        = 77.0; // NOT evenly divisible by 3 -- makes reuse-vs-recompute observable
    f.u(PLAYER, TIDX).experience    = 10;
    f.u(PLAYER, TIDX).unit_above[0] = 8;
    f.u(PLAYER, TIDX).unit_above[1] = 0;
    f.dir8_offsets[0]               = {1, 0};
    f.dir8_offsets[1]               = {2, 0};
    f.passable[(1u << 8) | 0u]      = 1;
    f.passable[(2u << 8) | 0u]      = 1;
    g_uf.lead_ret                   = 8; // reuse a valid small soldier index for both spawns' chain-head
    f.u(PLAYER, 51).unit_above[0]   = 8;
    f.u(PLAYER, 52).unit_above[0]   = 8;

    f.cfg_units[28].soldier_count = 1; // tail (proto_id ends at 30-2=28); single-node minimal tail

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.spawn_on_tile_calls.size(), 2u, "field(share reuse): exactly 2 successful splits");
    const double share = 77.0 / 3.0; // energy_share, computed the SAME way the header cites (once, off
                                     // the INITIAL capacity(3) and the INITIAL energy(77.0))
    ck_eq_d(f.u(PLAYER, 51).energy, share, "field(share reuse): 1st spawned unit's energy == energy_share");
    ck_eq_d(f.u(PLAYER, 52).energy, share,
            "field(share reuse): 2nd spawned unit's energy == the SAME energy_share bit-for-bit "
            "(hoisted once before the loop, never recomputed -- contrast docked's own recompute test)");
    ck_eq((uint32_t)f.u(PLAYER, 51).experience, 3u, "field(share reuse): 1st spawned unit's experience_share=10/3=3");
    ck_eq((uint32_t)f.u(PLAYER, 52).experience, 3u,
          "field(share reuse): 2nd spawned unit's experience_share is the SAME value(3), also hoisted once");
}

void test_field_loop_capped_at_eight_direction_attempts() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX          = 25;
    f.u(PLAYER, TIDX).unit_proto_id = 10;
    f.cfg_units[10].soldier_count   = 20; // remaining seed=20, far more than the 8-direction bound
    f.u(PLAYER, TIDX).x             = 0;
    f.u(PLAYER, TIDX).y             = 0;
    for (int32_t i = 0; i < 8; ++i) {
        f.dir8_offsets[i]                         = {i + 1, 0};
        f.passable[((uint32_t)(i + 1) << 8) | 0u] = 1;
    }
    f.cfg_units[2].soldier_count    = 1; // tail off the post-loop proto_id (10-8=2)
    f.u(PLAYER, TIDX).unit_above[0] = 9;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.spawn_on_tile_calls.size(), 8u,
          "field: `for (dir=0; dir<8 && remaining>1; ++dir)` -- the 8-direction bound stops the loop "
          "even though remaining(20) never got anywhere near 1");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 2u, "field: proto_id decremented exactly 8 times");
}

void test_field_tile_wrap_uses_geom_width_height_masks_not_swapped() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX          = 26;
    f.u(PLAYER, TIDX).unit_proto_id = 15;
    f.cfg_units[15].soldier_count   = 2; // remaining seed=2 -> exactly 1 split needed to stop
    f.u(PLAYER, TIDX).x             = 250;
    f.u(PLAYER, TIDX).y             = 10;
    // geom.width_mask=0xff, geom.height_mask=0x3f (fixture reset() defaults) -- DISTINCT from each
    // other and from width_m/height_m(0x7f/0x1f), so a translation that reached for the wrong pair or
    // swapped the axes disagrees with these exact numbers.
    // tile_x = (250+200) & 0xff = 450 & 0xff = 194 (would be 66 under width_m=0x7f, or 2 if height_mask
    // were used for X instead -- all three wrong answers are distinct from 194).
    // tile_y = (10+100) & 0x3f = 110 & 0x3f = 46 (would be 14 under height_m=0x1f).
    f.dir8_offsets[0]               = {200, 100};
    f.passable[(194u << 8) | 46u]   = 1;
    f.cfg_units[14].soldier_count   = 1; // tail off the post-loop proto_id (15-1=14)
    f.u(PLAYER, TIDX).unit_above[0] = 4;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck((uint32_t)g_uf.spawn_on_tile_calls.size() == 1u && g_uf.spawn_on_tile_calls[0].a == 194 &&
           g_uf.spawn_on_tile_calls[0].b == 46,
       "field: tile coords wrap via geom.width_mask(0xff)/height_mask(0x3f), not width_m/height_m or a "
       "swapped axis pairing -- spawn_on_tile(tile_x=194, tile_y=46, ..)");
}

void test_field_ctrl_group_both_gates_fire_for_local_player() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX                = 27;
    f.u(PLAYER_LOCAL, TIDX).unit_proto_id = 90;
    f.cfg_units[90].soldier_count         = 2; // 1 split
    f.u(PLAYER_LOCAL, TIDX).x             = 0;
    f.u(PLAYER_LOCAL, TIDX).y             = 0;
    f.u(PLAYER_LOCAL, TIDX).ctrl_group_id = 9; // nonzero -> gate B fires
    f.dir8_offsets[0]                     = {1, 0};
    f.passable[(1u << 8) | 0u]            = 1;
    f.ctrl_groups[0].count                = 4; // distinct, passed straight through to the mock
    f.cfg_units[89].soldier_count         = 1;
    f.u(PLAYER_LOCAL, TIDX).unit_above[0] = 2;

    g_uf.ctrl_contains_ret = 1; // nonzero -> gate A fires

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER_LOCAL, TIDX);

    ck((uint32_t)g_uf.ctrl_contains_calls.size() == 1u && g_uf.ctrl_contains_calls[0].a == TIDX &&
           g_uf.ctrl_contains_calls[0].b == 4 && g_uf.ctrl_contains_calls[0].c == 0,
       "field(local player): ctrl_group_contains_unit(tidx, ctrl_groups[0].count=4, group_index=0)");
    ck((uint32_t)g_uf.ctrl_assign_calls.size() == 2u && g_uf.ctrl_assign_calls[0].a == 51 &&
           g_uf.ctrl_assign_calls[0].b == 0 && g_uf.ctrl_assign_calls[1].a == 51 &&
           g_uf.ctrl_assign_calls[1].b == 9,
       "field(local player, both sub-gates true): TWO independent ctrl_group_assign calls fire -- "
       "(spawned,0) from contains_unit, (spawned,ctrl_group_id=9) from the transport's own group -- "
       "these are two separate `if`s, not an else-if");
}

void test_field_ctrl_group_neither_gate_fires_for_local_player() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX                = 28;
    f.u(PLAYER_LOCAL, TIDX).unit_proto_id = 90;
    f.cfg_units[90].soldier_count         = 2;
    f.u(PLAYER_LOCAL, TIDX).x             = 0;
    f.u(PLAYER_LOCAL, TIDX).y             = 0;
    f.u(PLAYER_LOCAL, TIDX).ctrl_group_id = 0; // gate B closed
    f.dir8_offsets[0]                     = {1, 0};
    f.passable[(1u << 8) | 0u]            = 1;
    f.cfg_units[89].soldier_count         = 1;
    f.u(PLAYER_LOCAL, TIDX).unit_above[0] = 2;

    g_uf.ctrl_contains_ret = 0; // gate A closed too

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER_LOCAL, TIDX);

    ck_eq((uint32_t)g_uf.ctrl_contains_calls.size(), 1u,
          "field(local player, both sub-gates false): contains_unit is STILL called (and returns 0)");
    ck_eq((uint32_t)g_uf.ctrl_assign_calls.size(), 0u,
          "field(local player, both sub-gates false): no ctrl_group_assign at all");
}

void test_field_ctrl_group_gate_closed_for_non_local_player() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX          = 29;
    f.u(PLAYER, TIDX).unit_proto_id = 90;
    f.cfg_units[90].soldier_count   = 2;
    f.u(PLAYER, TIDX).x             = 0;
    f.u(PLAYER, TIDX).y             = 0;
    f.u(PLAYER, TIDX).ctrl_group_id = 9; // would fire gate B if only the outer gate were open
    f.dir8_offsets[0]               = {1, 0};
    f.passable[(1u << 8) | 0u]      = 1;
    f.cfg_units[89].soldier_count   = 1;
    f.u(PLAYER, TIDX).unit_above[0] = 2;

    g_uf.ctrl_contains_ret = 1; // would fire gate A too, if reached

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_field(v, own, rec_uf_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_uf.ctrl_contains_calls.size(), 0u,
          "field(non-local player): the outer `player == local` gate closes BEFORE ctrl_group_contains_unit "
          "is even called, regardless of what it would have returned");
    ck_eq((uint32_t)g_uf.ctrl_assign_calls.size(), 0u, "field(non-local player): no ctrl_group_assign");
}

void test_field_player_high_bits_masked_unit_index_used_at_full_width() {
    sim_fixture f;
    g_uf.reset();
    constexpr int32_t TIDX                 = 29;
    const uint32_t    player_dirty         = 0x00050000u | PLAYER; // low16==2, garbage ONLY above bit 15
    f.u(PLAYER, TIDX).unit_proto_id        = 5;
    f.cfg_units[5].soldier_count           = 1; // remaining<=1 -> loop skipped, isolates the masking question
    f.u(PLAYER, TIDX).unit_above[0]        = 11;
    soldier_of(f, PLAYER, 11).next_soldier = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    // transport_unit_index passed as an ordinary, already-small value -- per the header banner
    // (lines 12-17) the SECOND param is used at full 32-bit width, UNMASKED, unlike the first; there
    // is no safe way to demonstrate that positively without an out-of-bounds roster index (which the
    // real game never passes either), so this test isolates the ONE param that genuinely narrows.
    detail::unload_field(v, own, rec_uf_calls(), player_dirty, (uint32_t)TIDX);

    ck((uint32_t)g_uf.lookup_calls.size() == 1u,
       "field(dirty player bits): the tail still ran -> the masked player correctly resolved a real "
       "unit_at/cfg_units row (an unmasked read would have indexed off the end of the fixture and "
       "either crashed or landed in another player's memory)");
    ck((uint32_t)g_uf.walk_anim_calls.size() == 1u && g_uf.walk_anim_calls[0].a == PLAYER &&
           g_uf.walk_anim_calls[0].b == TIDX,
       "field(dirty player bits): soldiers_start_walk_anim(player, tidx) sees player=2 (masked to its "
       "low 16 bits), not the dirty 0x50002 input");
}

// ==== llm_unit_transport_unload_docked @0x004891d8 ==================================================
// From the header banner: NO terrain/direction search at all (always uses the transport's OWN
// home_storage_slot as spawn_docked's probe), always detaches the HEAD of the transport's own chain
// (not a "picked lead in a direction"), the energy share is RECOMPUTED every iteration off the
// CURRENT (already-decremented) remaining -- unlike field's hoisted-once share -- experience is NEVER
// touched, the loop has NO 8-iteration cap (only `remaining>1`), and the tail writes the transport's
// OWN cur_x/y FROM the walked node's end_x/y (then start_x/y from that new cur) -- the opposite
// direction of field's "start from OWN untouched cur" -- and never calls soldiers_start_walk_anim.

void test_docked_zero_iterations_tail_writes_cur_and_start_from_node() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 21;
    f.u(PLAYER, TIDX).unit_proto_id     = 40;
    f.cfg_units[40].soldier_count       = 1; // remaining seed=1 -> the while loop never runs
    f.u(PLAYER, TIDX).unit_above[0]     = 80;
    f.u(PLAYER, TIDX).unit_above[1]     = 0;
    soldier_of(f, PLAYER, TIDX).cur_x   = -9; // sentinels, all distinct from the node's end value below
    soldier_of(f, PLAYER, TIDX).cur_y   = -8;
    soldier_of(f, PLAYER, TIDX).start_x = -7;
    soldier_of(f, PLAYER, TIDX).start_y = -6;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.spawn_docked_calls.size(), 0u, "docked(remaining<=1): no spawn attempts");
    ck_eq((uint32_t)g_ud.soldier_unlink_calls.size(), 0u, "docked(remaining<=1): no unlink either");
    // tail: remaining_after=1 (same unchanged cfg row), chain=80. lookup writes (base+table_row)=(0+1)=1
    // into both out-params -- soldier(80).end_x/y become 1,1.
    ck_eq((uint32_t)g_ud.lookup_calls.size(), 1u, "docked(remaining<=1): the tail still runs once");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).cur_x), su(1),
          "docked: transport's own cur_x is SET FROM the walked node's end_x (unlike field, which leaves cur "
          "untouched)");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_x), su(1),
          "docked: transport's own start_x is then set from that SAME new cur_x");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).cur_y), su(1), "docked: transport's own cur_y from the node's end_y");
    ck_eq(su(soldier_of(f, PLAYER, TIDX).start_y), su(1), "docked: transport's own start_y from the new cur_y");
    ck_eq((uint32_t)g_ud.unexpected_call_n, 0u,
          "docked: none of the 5 field-only callees fired (including soldiers_start_walk_anim, confirmed "
          "absent from the tail per the header's own disasm note)");
}

void test_docked_two_splits_probe_slot_reused_and_experience_never_touched() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 22;
    f.u(PLAYER, TIDX).unit_proto_id     = 70;
    f.cfg_units[70].soldier_count       = 3; // remaining seed=3 -> exactly 2 successful splits
    f.u(PLAYER, TIDX).home_storage_slot = 44;
    f.u(PLAYER, TIDX).energy            = 60.0; // 60/3=20, then 40/2=20 -- evenly divisible on purpose
                                                // (the FP-divergence case is its own dedicated test below)
    f.u(PLAYER, TIDX).experience    = 555;      // sentinel -- docked must NEVER touch this field
    f.u(PLAYER, TIDX).unit_above[0] = 90;
    f.u(PLAYER, TIDX).unit_above[1] = 0;
    f.cfg_units[68].soldier_count   = 1; // tail off the post-loop proto_id (70-2=68)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.spawn_docked_calls.size(), 2u, "docked: exactly 2 successful splits (seed 3 -> 1)");
    ck(g_ud.spawn_docked_calls[0].a == 68 && g_ud.spawn_docked_calls[0].b == PLAYER &&
           g_ud.spawn_docked_calls[0].c == 44,
       "docked: 1st spawn_docked(soldier_proto_id=68, player, probe_slot=44)");
    ck(g_ud.spawn_docked_calls[1].a == 68 && g_ud.spawn_docked_calls[1].b == PLAYER &&
           g_ud.spawn_docked_calls[1].c == 44,
       "docked: 2nd call reuses the SAME soldier_proto_id(68) and the SAME probe_slot(44) -- both are "
       "computed ONCE before the loop, not per-iteration");
    ck(g_ud.soldier_unlink_calls.size() == 2 && g_ud.soldier_unlink_calls[0].a == PLAYER &&
           g_ud.soldier_unlink_calls[0].b == TIDX && g_ud.soldier_unlink_calls[0].c == 90 &&
           g_ud.soldier_unlink_calls[1].c == 90,
       "docked: soldier_unlink always detaches the HEAD of the transport's own chain (unit_above=90), not "
       "a 'picked lead in a direction' like field's own");

    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 68u, "docked: proto_id decremented twice");
    ck_eq_d(f.u(PLAYER, TIDX).energy, 20.0, "docked: transport.energy after two 20.0 shares -> 20.0");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).experience, 555u,
          "docked: transport.experience is UNTOUCHED -- unlike field, docked never decrements it");

    ck_eq_d(f.u(PLAYER, 61).energy, 20.0, "docked: 1st spawned unit's energy = share(20.0)");
    ck_eq_d(f.u(PLAYER, 62).energy, 20.0, "docked: 2nd spawned unit's energy = share(20.0)");
    ck_eq((uint32_t)f.u(PLAYER, 61).experience, 0u,
          "docked: spawned unit's experience is left at its default -- docked never writes it (field does)");
    ck_eq((uint32_t)f.u(PLAYER, 62).experience, 0u, "docked: same for the 2nd spawned unit");

    ck_eq((uint32_t)g_ud.ctrl_contains_calls.size(), 0u, "docked: non-local player -> ctrl-group block skipped");
    ck_eq((uint32_t)g_ud.unexpected_call_n, 0u,
          "docked: none of the 5 field-only callees (spawn_on_tile/dir_from_to/squad_pick_lead_soldier_"
          "in_direction/cursor_apply_anim_frame_offset/soldiers_start_walk_anim) ever fired");
}

void test_docked_spawn_failure_unlink_still_happens_decrement_skipped() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX                 = 23;
    f.u(PLAYER, TIDX).unit_proto_id        = 60;
    f.cfg_units[60].soldier_count          = 5; // remaining seed=5
    f.u(PLAYER, TIDX).home_storage_slot    = 44;
    f.u(PLAYER, TIDX).energy               = 33.0;
    f.u(PLAYER, TIDX).unit_above[0]        = 95;
    f.u(PLAYER, TIDX).unit_above[1]        = 0;
    soldier_of(f, PLAYER, 95).next_soldier = 96;
    soldier_of(f, PLAYER, 96).next_soldier = 97;
    soldier_of(f, PLAYER, 97).next_soldier = 98;
    soldier_of(f, PLAYER, 98).next_soldier = 99;
    soldier_of(f, PLAYER, 99).next_soldier = 0;

    g_ud.spawn_fail_at_call = 1; // the very first attempt fails

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.spawn_docked_calls.size(), 1u, "docked(spawn fails): remaining forced to 0 -> stops");
    ck((uint32_t)g_ud.soldier_unlink_calls.size() == 1u && g_ud.soldier_unlink_calls[0].c == 95,
       "docked(spawn fails): soldier_unlink is UNCONDITIONAL -- it runs BEFORE spawn_docked is even "
       "attempted, so it fires even though the spawn itself then fails");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 60u,
          "docked(spawn fails): the abort `continue` skips the proto_id decrement");
    ck_eq_d(f.u(PLAYER, TIDX).energy, 33.0, "docked(spawn fails): transport.energy untouched");
    // tail off the UNCHANGED cfg row: remaining_after=5, same as the seed.
    ck_eq((uint32_t)g_ud.lookup_calls.size(), 5u,
          "docked(spawn fails): the tail still walks remaining_after(5) nodes off the unchanged cfg row");
}

void test_docked_energy_share_recomputed_every_iteration_diverges_from_hoisted() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 24;
    f.u(PLAYER, TIDX).unit_proto_id     = 80;
    f.cfg_units[80].soldier_count       = 7; // remaining seed=7
    f.u(PLAYER, TIDX).home_storage_slot = 1;
    f.u(PLAYER, TIDX).energy            = 100.0; // NOT evenly divisible by 7 -- makes the recompute-vs-
                                                 // hoist divergence observable in the raw IEEE754 bits
    f.u(PLAYER, TIDX).experience    = 12345;     // sentinel, must stay untouched
    f.u(PLAYER, TIDX).unit_above[0] = 40;
    f.u(PLAYER, TIDX).unit_above[1] = 0;
    f.cfg_units[78].soldier_count   = 1; // tail off the post-loop proto_id (80-2=78)

    g_ud.spawn_fail_at_call = 3; // succeed twice, then fail -> exactly 2 splits observed

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.spawn_docked_calls.size(), 3u, "docked(recompute): 2 successes + the failing 3rd call");

    // Ground truth per the header banner: "docked recomputes the energy share EVERY iteration off the
    // CURRENT (already-decremented) remaining" -- mechanically applying that formula with plain
    // double arithmetic (same as the C++ under test uses) from the SAME starting inputs:
    double       e      = 100.0;
    int32_t      r      = 7;
    const double share0 = e / (double)r; // iteration 1: remaining is still the seed(7)
    e -= share0;
    r -= 1;                              // r=6
    const double share1 = e / (double)r; // iteration 2: remaining is the ALREADY-DECREMENTED 6
    e -= share1;

    ck(share0 != share1,
       "docked(recompute): share0 and share1 are NOT bit-identical -- IEEE754 rounding makes "
       "recompute-every-iteration diverge from field's hoist-once-and-reuse by the 2nd split, even "
       "though both describe the same real number algebraically");
    ck_eq_d(f.u(PLAYER, 61).energy, share0, "docked(recompute): 1st spawned unit's energy == share0");
    ck_eq_d(f.u(PLAYER, 62).energy, share1, "docked(recompute): 2nd spawned unit's energy == share1 (!= share0)");
    ck_eq_d(f.u(PLAYER, TIDX).energy, e, "docked(recompute): transport.energy after both decrements");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).experience, 12345u,
          "docked(recompute): experience is STILL untouched even across multiple splits");
}

void test_docked_loop_unbounded_exceeds_eight_iterations() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 25;
    f.u(PLAYER, TIDX).unit_proto_id     = 50;
    f.cfg_units[50].soldier_count       = 12; // remaining seed=12 -> 11 successful splits (12->1)
    f.u(PLAYER, TIDX).home_storage_slot = 1;
    f.u(PLAYER, TIDX).energy            = 0.0;
    f.u(PLAYER, TIDX).unit_above[0]     = 2;
    f.cfg_units[39].soldier_count       = 1; // tail off the post-loop proto_id (50-11=39)

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.spawn_docked_calls.size(), 11u,
          "docked: `while (remaining>1)` has NO 8-iteration cap -- unlike field's `dir<8` bound, this "
          "loop runs all 11 splits a 12-seed capacity allows");
    ck_eq((uint32_t)f.u(PLAYER, TIDX).unit_proto_id, 39u, "docked: proto_id decremented 11 times");
}

void test_docked_ctrl_group_both_gates_fire_for_local_player() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX                    = 26;
    f.u(PLAYER_LOCAL, TIDX).unit_proto_id     = 90;
    f.cfg_units[90].soldier_count             = 2; // 1 split
    f.u(PLAYER_LOCAL, TIDX).home_storage_slot = 1;
    f.u(PLAYER_LOCAL, TIDX).energy            = 0.0;
    f.u(PLAYER_LOCAL, TIDX).ctrl_group_id     = 9; // nonzero -> gate B fires
    f.u(PLAYER_LOCAL, TIDX).unit_above[0]     = 3;
    f.ctrl_groups[0].count                    = 4;
    f.cfg_units[89].soldier_count             = 1;

    g_ud.ctrl_contains_ret = 1; // nonzero -> gate A fires

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER_LOCAL, TIDX);

    ck((uint32_t)g_ud.ctrl_contains_calls.size() == 1u && g_ud.ctrl_contains_calls[0].a == TIDX &&
           g_ud.ctrl_contains_calls[0].b == 4 && g_ud.ctrl_contains_calls[0].c == 0,
       "docked(local player): ctrl_group_contains_unit(tidx, ctrl_groups[0].count=4, group_index=0) -- "
       "same shape as field's own tail");
    ck((uint32_t)g_ud.ctrl_assign_calls.size() == 2u && g_ud.ctrl_assign_calls[0].a == 61 &&
           g_ud.ctrl_assign_calls[0].b == 0 && g_ud.ctrl_assign_calls[1].a == 61 &&
           g_ud.ctrl_assign_calls[1].b == 9,
       "docked(local player, both sub-gates true): two independent ctrl_group_assign calls fire");
}

void test_docked_ctrl_group_gate_closed_for_non_local_player() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 27;
    f.u(PLAYER, TIDX).unit_proto_id     = 90;
    f.cfg_units[90].soldier_count       = 2;
    f.u(PLAYER, TIDX).home_storage_slot = 1;
    f.u(PLAYER, TIDX).energy            = 0.0;
    f.u(PLAYER, TIDX).ctrl_group_id     = 9; // would fire gate B if only the outer gate were open
    f.u(PLAYER, TIDX).unit_above[0]     = 3;
    f.cfg_units[89].soldier_count       = 1;

    g_ud.ctrl_contains_ret = 1; // would fire gate A too, if reached

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), PLAYER, TIDX);

    ck_eq((uint32_t)g_ud.ctrl_contains_calls.size(), 0u,
          "docked(non-local player): outer `player == local` gate closes before ctrl_group_contains_unit");
    ck_eq((uint32_t)g_ud.ctrl_assign_calls.size(), 0u, "docked(non-local player): no ctrl_group_assign");
}

void test_docked_player_high_bits_masked_regression() {
    sim_fixture f;
    g_ud.reset();
    constexpr int32_t TIDX              = 28;
    const uint32_t    player_dirty      = 0x00070000u | PLAYER; // low16==2, garbage ONLY above bit 15
    f.u(PLAYER, TIDX).unit_proto_id     = 15;
    f.cfg_units[15].soldier_count       = 2; // 1 split, enough to surface player in a real call's args
    f.u(PLAYER, TIDX).home_storage_slot = 9;
    f.u(PLAYER, TIDX).energy            = 10.0;
    f.u(PLAYER, TIDX).unit_above[0]     = 4;
    f.cfg_units[14].soldier_count       = 1;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unload_docked(v, own, rec_ud_calls(), player_dirty, (uint32_t)TIDX);

    ck((uint32_t)g_ud.spawn_docked_calls.size() == 1u && g_ud.spawn_docked_calls[0].b == PLAYER,
       "docked(dirty player bits): spawn_docked's player arg is the MASKED low16 value(2), not the dirty "
       "0x70002 input");
    ck((uint32_t)g_ud.soldier_unlink_calls.size() == 1u && g_ud.soldier_unlink_calls[0].a == PLAYER,
       "docked(dirty player bits): soldier_unlink's player arg is likewise masked to 2");
}

} // namespace

void run_unit_transport_unload_tests() {
    test_field_zero_iterations_capacity_le_one();
    test_field_successful_spawn_writes_skips_and_tail_hazard1();
    test_field_spawn_failure_aborts_remaining();
    test_field_energy_experience_share_hoisted_and_reused_across_two_spawns();
    test_field_loop_capped_at_eight_direction_attempts();
    test_field_tile_wrap_uses_geom_width_height_masks_not_swapped();
    test_field_ctrl_group_both_gates_fire_for_local_player();
    test_field_ctrl_group_neither_gate_fires_for_local_player();
    test_field_ctrl_group_gate_closed_for_non_local_player();
    test_field_player_high_bits_masked_unit_index_used_at_full_width();

    test_docked_zero_iterations_tail_writes_cur_and_start_from_node();
    test_docked_two_splits_probe_slot_reused_and_experience_never_touched();
    test_docked_spawn_failure_unlink_still_happens_decrement_skipped();
    test_docked_energy_share_recomputed_every_iteration_diverges_from_hoisted();
    test_docked_loop_unbounded_exceeds_eight_iterations();
    test_docked_ctrl_group_both_gates_fire_for_local_player();
    test_docked_ctrl_group_gate_closed_for_non_local_player();
    test_docked_player_high_bits_masked_regression();
}

} // namespace mh::sim::test
