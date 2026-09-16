//
// sim_storage_place_exit_ground_selftest.cpp -- `simtest` oracle for
// llm_strat_storage_place_exit_ground @0x00489f54 (sim/sim_storage_exit_placement.h/.cpp,
// RI-SIM / SIM1-G3). Does NOT touch llm_strat_storage_exit_air (this batch's sibling
// function in the same TU) or llm_strat_unit_state_exit_storage_begin (the caller, oracled separately
// by a different agent) -- only this function's own correctness is pinned here.
//
// EXPECTED BEHAVIOUR, hand-derived from
// tmp/decomp_sim/llm_strat_storage_place_exit_ground_00489f54.asm (NEVER the .c beside it) --
// every assertion below cites the instruction address(es) it pins:
//   0x00489f73-0x00489fc4: snapshot exit_x/exit_y/building_index from unit_storage BEFORE the
//     remove_docked_unit call (0x00489fc7-0x00489fd1) fires -- these three become LOCAL COPIES, so a
//     later mutation of the storage record (by remove_docked_unit or anything else) cannot change them.
//   0x00489fd6-0x0048a01a: park_x/park_y are re-read FRESH from storage AFTER the remove call (a
//     SEPARATE fetch, not the pre-call snapshot) and fed with the pre-call exit_x/exit_y into
//     dir_from_to, all four scaled <<5 (tile -> fine coords).
//   0x0048a035-0x0048a08c: facing_current/facing_target both set to (uint8_t)dir; x/y set to
//     (uint8_t)exit_x/exit_y (the PRE-call snapshot, not any post-call value).
//   0x0048a092-0x0048a0f3: passable[(exit_x<<8)|exit_y] read ONCE (the asm reads the identical address
//     twice from independently-derived computations; the .cpp reuses one read) into
//     origin_tile_was_passable (uint8_t) AND move_step_speed_scale (double, an int-to-double
//     CONVERSION of the SAME byte, not a bit-reinterpret); the cell is then cleared to 0 as a THIRD,
//     later access.
//   0x0048a0fa-0x0048a1a4: classify Building[buildings[player][building_index].building_id].type into
//     a 3-way tier -- tier1 (move_microstep=0) for A_GARAGE(0x08)/A_SHUTTLE(0x0d)/H_GARAGE(0x1c)/
//     H_SHUTTLE(0x21), tier2 (-0x20) for H_BARRACKS(0x1b), tier3 (-0x40) otherwise -- using the SAME
//     pre-call building_index snapshot (never re-read from storage).
//   0x0048a1aa-0x0048a346: IF that building type is A_SHUTTLE or H_SHUTTLE AND its shuttle_slot != 0:
//     IF cfg_units[unit_proto_id].soldier_count==0: prod_shuttle_unload_passengers(player,
//       building_index, cfg_units[unit_proto_id].human) [0x0048a249-0x0048a276];
//     IF prod_shuttle_slots[...].passengers_reserved==0 [gate @0x0048a2ae]: count nonzero LEADING
//       uint16_t entries across cargo_manifest_raw's first 50 (kCargoManifestScanCount=0x32) conceptual
//       14-byte (kCargoEntryStride=0xe) entries [0x0048a2b4-0x0048a316]; IF storage.docked_count==0 AND
//       that count==0: bldg_flush_cargo_hold(player, building_index) [0x0048a33a-0x0048a341].
//   0x0048a346-0x0048a52f: IF cfg_units[unit_proto_id].soldier_count > 0 [gate @0x0048a36d]:
//     unit_soldiers_set_heading(player, unit_index, (uint8_t)dir) [0x0048a37d]; total = soldier_count
//     (snapshotted ONCE into a local that is never written again); remaining = a SEPARATE copy that
//     DOES get decremented each iteration; chain = unit.unit_above (word). A DO-WHILE (body runs
//     UNCONDITIONALLY at least once even if chain==0 going in -- there is no pre-loop test, only the
//     tail CMP/JNZ @0x0048a4bd-0x0048a4c1): cursor_lookup_offset_pair(total, remaining,
//     &soldier[chain].start_x, &soldier[chain].start_y) [0x0048a3ce-0x0048a408, args EAX=table_col=
//     remaining? -- NO: verified against addr/mh_calls.gen.h's s_void_EAX_EDX_EBX_ECX, EAX=table_col=
//     the NEVER-decremented local at [EBP-0x1c] (== total), EDX=table_row=the DECREMENTING local at
//     [EBP-0x20] (== remaining, read pre-decrement); a PURE QUERY per the header banner -- do not
//     suppress it under an effect seam, but its written (start_x,start_y) IS immediately overwritten by
//     the next two lines, so this oracle does not depend on what it wrote]; then start_x=cur_x,
//     end_x=start_x, start_y=cur_y, end_y=start_y [0x0048a40d-0x0048a4b4]; chain=soldier[chain]
//     .next_soldier, --remaining [0x0048a4ba]; loop while chain != 0.
//   ELSE (soldier_count<=0): pop_stats[player].human -= cfg_units[unit_proto_id].human;
//     pop_stats[player].human_in_field += cfg_units[unit_proto_id].human (SAME field, opposite signs)
//     [0x0048a4c9-0x0048a52f].
//   0x0048a52f-0x0048a53b: unconditionally, unit_set_state_of(player, unit_index, 0x21) -- the ONE
//     state-setting call in the whole function, and the LAST call to fire on every path.
//
// Cross-checked the tier boundary chain (0x0048a139-0x0048a184) and the shuttle-type re-check
// (0x0048a1ca-0x0048a1fa) instruction-by-instruction against sim_storage_exit_placement.cpp's
// exit_ground_delay_tier() and the BUILDING_TYPE_A_SHUTTLE/_H_SHUTTLE gate -- NO DIVERGENCE FOUND.
// Cross-checked the cursor_lookup_offset_pair arg assignment (which local is table_col vs table_row)
// against addr/mh_calls.gen.h's __watcall register mapping -- NO DIVERGENCE FOUND (see the long note
// above; it looks backwards at first read of the raw asm and is not).
//
#include "sim/sim_storage_exit_placement.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared fixture pointer, for recorders that need to read/mutate backing storage directly -------
// Same pattern as sim_unit_change_proto_and_energy_selftest.cpp's g_active_fx: the recorders sit
// outside the fixture and need a way to reach into it (here: to mutate unit_storage mid-call to prove
// a read happens before vs after it, and to decode which soldier record an out-pointer targets).
sim_fixture *g_active_fx = nullptr;
int32_t      g_call_seq  = 0;

// ---- recorder: storage_remove_docked_unit -----------------------------------------------------------
struct RemoveDockedCall {
    uint32_t player;
    int32_t  unit_index;
    int32_t  storage_slot;
    int32_t  seq;
};
std::vector<RemoveDockedCall> g_remove_docked_calls;

// When armed, the mock mutates the (g_mutate_player, g_mutate_slot) storage record to these POST-call
// values -- this is what lets a case tell whether a given field was read BEFORE this call (and so is
// immune to the mutation) or AFTER it (and so observes the mutation). See the header's park_x/park_y
// CORRECTION and its "exit_x/exit_y/building_index captured before the call" claim.
bool     g_mutate_storage_on_remove = false;
uint16_t g_mutate_player            = 0;
int32_t  g_mutate_slot              = 0;
uint8_t  g_mutate_park_x_to         = 0;
uint8_t  g_mutate_park_y_to         = 0;
int32_t  g_mutate_exit_x_to         = 0;
int32_t  g_mutate_exit_y_to         = 0;
int32_t  g_mutate_b_index_to        = 0;

void rec_storage_remove_docked_unit(uint16_t player, int32_t unit_index, int32_t storage_slot) {
    RemoveDockedCall c;
    c.player       = player;
    c.unit_index   = unit_index;
    c.storage_slot = storage_slot;
    c.seq          = ++g_call_seq;
    g_remove_docked_calls.push_back(c);
    if (g_mutate_storage_on_remove && g_active_fx != nullptr) {
        unit_storage &st =
            g_active_fx->storage[(size_t)g_mutate_player * STORAGE_PER_PLAYER + (size_t)g_mutate_slot];
        st.park_x      = g_mutate_park_x_to;
        st.park_y      = g_mutate_park_y_to;
        st.exit_tile_x = g_mutate_exit_x_to;
        st.exit_tile_y = g_mutate_exit_y_to;
        st.b_index     = g_mutate_b_index_to;
    }
}

// ---- recorder: dir_from_to --------------------------------------------------------------------------
struct DirFromToCall {
    int32_t x1, y1, x2, y2;
    int32_t seq;
};
std::vector<DirFromToCall> g_dir_from_to_calls;
int32_t                    g_dir_from_to_result = 0;
int32_t                    rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_dir_from_to_calls.push_back({x1, y1, x2, y2, ++g_call_seq});
    return g_dir_from_to_result;
}

// ---- recorder: prod_shuttle_unload_passengers -------------------------------------------------------
struct UnloadCall {
    uint32_t player;
    int32_t  building_index;
    int32_t  cap;
    int32_t  seq;
};
std::vector<UnloadCall> g_unload_calls;
int32_t                 rec_prod_shuttle_unload_passengers(uint16_t player, int32_t building_index, int32_t cap) {
    g_unload_calls.push_back({player, building_index, cap, ++g_call_seq});
    return 0; // header: return value unused by the caller
}

// ---- recorder: bldg_flush_cargo_hold ------------------------------------------------------------------
struct FlushCall {
    uint32_t player;
    int32_t  building_index;
    int32_t  seq;
};
std::vector<FlushCall> g_flush_calls;
void                   rec_bldg_flush_cargo_hold(uint32_t player, int32_t building_index) {
    g_flush_calls.push_back({player, building_index, ++g_call_seq});
}

// ---- recorder: unit_soldiers_set_heading --------------------------------------------------------------
struct HeadingCall {
    uint32_t player;
    int32_t  unit_index;
    uint8_t  sprite_frame;
    int32_t  seq;
};
std::vector<HeadingCall> g_heading_calls;
void                     rec_unit_soldiers_set_heading(uint16_t player, int32_t unit_index,
                                                       uint8_t sprite_frame) {
    g_heading_calls.push_back({player, unit_index, sprite_frame, ++g_call_seq});
}

// ---- recorder: cursor_lookup_offset_pair --------------------------------------------------------------
// A pure query per the header banner (its written pair is immediately overwritten by cur_x/cur_y in
// the .cpp), so this recorder does NOT need to emulate its write -- only capture (table_col, table_row)
// and decode which soldier record the out-pointers target, from their raw address relative to
// g_active_fx->soldiers.data().
struct CursorLookupCall {
    int32_t   table_col;
    int32_t   table_row;
    ptrdiff_t soldier_index;
    ptrdiff_t out_a_field_offset; // byte offset within the soldier record (expect 7 == offsetof start_x)
    ptrdiff_t out_b_field_offset; // expect 8 == offsetof start_y
    int32_t   seq;
};
std::vector<CursorLookupCall> g_cursor_calls;
void                          rec_cursor_lookup_offset_pair(int32_t table_col, int32_t table_row, char *out_a, char *out_b) {
    CursorLookupCall c;
    c.table_col = table_col;
    c.table_row = table_row;
    c.seq       = ++g_call_seq;
    if (g_active_fx != nullptr) {
        char           *base  = reinterpret_cast<char *>(g_active_fx->soldiers.data());
        const ptrdiff_t off_a = out_a - base;
        const ptrdiff_t off_b = out_b - base;
        c.soldier_index       = off_a / (ptrdiff_t)sizeof(soldier);
        c.out_a_field_offset  = off_a % (ptrdiff_t)sizeof(soldier);
        c.out_b_field_offset  = off_b % (ptrdiff_t)sizeof(soldier);
    } else {
        c.soldier_index = c.out_a_field_offset = c.out_b_field_offset = -1;
    }
    g_cursor_calls.push_back(c);
}

// ---- recorder: unit_set_state_of --------------------------------------------------------------------
struct SetStateCall {
    int32_t player;
    int32_t unit_index;
    int16_t state;
    int32_t seq;
};
std::vector<SetStateCall> g_set_state_calls;
void                      rec_unit_set_state_of(int32_t player, int32_t unit_index, int16_t state) {
    g_set_state_calls.push_back({player, unit_index, state, ++g_call_seq});
}

// ---- recorder: storage_setup_exit_path ----------------------------------------------------------------
// Belongs to llm_strat_storage_exit_air (this TU's OTHER function, oracled separately) -- this ground
// function must never call it. Stubbed here only because both functions share one `calls` struct.
struct SetupExitPathCall {
    uint32_t player;
    int32_t  unit_index;
    uint8_t  exit_x, exit_y;
    int32_t  path_slot;
    int32_t  storage_slot;
    int32_t  seq;
};
std::vector<SetupExitPathCall> g_setup_exit_path_calls;
void                           rec_storage_setup_exit_path(uint32_t player, int32_t unit_index, uint8_t exit_x, uint8_t exit_y,
                                                           int32_t path_slot, int32_t storage_slot) {
    g_setup_exit_path_calls.push_back(
        {player, unit_index, exit_x, exit_y, path_slot, storage_slot, ++g_call_seq});
}

const storage_exit_placement_calls g_calls = {
    &rec_storage_remove_docked_unit,
    &rec_dir_from_to,
    &rec_prod_shuttle_unload_passengers,
    &rec_bldg_flush_cargo_hold,
    &rec_unit_soldiers_set_heading,
    &rec_cursor_lookup_offset_pair,
    &rec_unit_set_state_of,
    &rec_storage_setup_exit_path,
};

constexpr uint16_t PLAYER       = 2;
constexpr int32_t  UNIT_INDEX   = 5;
constexpr int32_t  STORAGE_SLOT = 3;
constexpr uint16_t PROTO_ID     = 7;

void reset_recorders() {
    g_call_seq = 0;
    g_remove_docked_calls.clear();
    g_mutate_storage_on_remove = false;
    g_dir_from_to_calls.clear();
    g_dir_from_to_result = 0;
    g_unload_calls.clear();
    g_flush_calls.clear();
    g_heading_calls.clear();
    g_cursor_calls.clear();
    g_set_state_calls.clear();
    g_setup_exit_path_calls.clear();
}

unit_storage &seed_storage(sim_fixture &fx, uint16_t player, int32_t slot, int32_t b_index,
                           int32_t exit_x, int32_t exit_y, uint8_t park_x, uint8_t park_y,
                           int32_t docked_count = 0) {
    unit_storage &st = fx.storage[(size_t)player * STORAGE_PER_PLAYER + (size_t)slot];
    st.b_index       = b_index;
    st.exit_tile_x   = exit_x;
    st.exit_tile_y   = exit_y;
    st.park_x        = park_x;
    st.park_y        = park_y;
    st.docked_count  = docked_count;
    return st;
}

void seed_building(sim_fixture &fx, uint16_t player, int32_t b_index, uint16_t building_id,
                   uint8_t type, uint8_t shuttle_slot = 0) {
    building &bd                       = fx.b((int32_t)player, b_index);
    bd.building_id                     = building_id;
    bd.shuttle_slot                    = shuttle_slot;
    fx.cfg_buildings[building_id].type = type;
}

// A minimal do-while-safe soldier chain: unit.unit_above points at soldier record 0, whose
// next_soldier is 0, so the reseat loop runs exactly once and terminates. Used by shuttle/cargo cases
// that need soldier_count>0 (to isolate a DIFFERENT branch) without also exercising the multi-soldier
// chain mechanics (those get their own dedicated cases).
void seed_trivial_soldier_chain(sim_fixture &fx, unit &u, uint16_t player, int8_t cx, int8_t cy) {
    u.unit_above[0] = 0;
    u.unit_above[1] = 0;
    soldier &s0     = fx.soldiers[(size_t)player * SOLDIERS_PER_PLAYER + 0];
    s0.cur_x        = cx;
    s0.cur_y        = cy;
    s0.next_soldier = 0;
    s0.start_x = s0.start_y = s0.end_x = s0.end_y = 0;
}

// Shared setup for the shuttle/cargo family of cases (T3-T11): a unit docked at building `b_index`
// (cfg building_id -> `bldg_type`, shuttle_slot as given), with the given soldier_count/human on its
// cfg Unit row. exit/park storage fields are fixed, uninteresting sentinel values -- these cases don't
// assert on facing/x/y/passable (that is T1's job).
void seed_shuttle_case(sim_fixture &fx, uint16_t player, int32_t unit_index, int32_t storage_slot,
                       int32_t b_index, uint16_t building_id, uint8_t bldg_type, uint8_t shuttle_slot,
                       uint16_t proto_id, int32_t soldier_count, int32_t human) {
    unit &u                              = fx.u(player, unit_index);
    u.unit_proto_id                      = proto_id;
    fx.cfg_units[proto_id].soldier_count = soldier_count;
    fx.cfg_units[proto_id].human         = human;
    seed_storage(fx, player, storage_slot, b_index, /*exit_x=*/10, /*exit_y=*/20, /*park_x=*/3,
                 /*park_y=*/9);
    seed_building(fx, player, b_index, building_id, bldg_type, shuttle_slot);
}

} // namespace

void run_storage_place_exit_ground_tests() {
    sim_fixture fx;
    g_active_fx = &fx;

    // =================================================================================================
    // T1 -- the full happy path with NOTHING gated off: non-shuttle building (tier1), soldier_count<=0.
    // This is the anti-vacuous-pass case -- every UNCONDITIONAL call/write in the function must fire
    // here (a naive oracle that only checks the conditional shuttle/soldier arms would pass even if
    // this whole top half were deleted). Also the ORDER-sensitive case: remove_docked_unit's mock
    // mutates the storage record it was just asked to operate on, proving which reads happen before vs
    // after the call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        unit &u                              = fx.u(PLAYER, UNIT_INDEX);
        u.unit_proto_id                      = PROTO_ID;
        fx.cfg_units[PROTO_ID].human         = 9;
        fx.cfg_units[PROTO_ID].soldier_count = 0;
        fx.population[PLAYER].human          = 100;
        fx.population[PLAYER].human_in_field = 40;

        seed_storage(fx, PLAYER, STORAGE_SLOT, /*b_index=*/6, /*exit_x=*/10, /*exit_y=*/20,
                     /*park_x=*/3, /*park_y=*/9);
        seed_building(fx, PLAYER, /*b_index=*/6, /*building_id=*/40, BUILDING_TYPE_A_GARAGE); // tier1
        // Decoy: a DIFFERENT building at a DIFFERENT tier, at the index the mock will (wrongly, if the
        // .cpp had a bug) redirect building_index to.
        seed_building(fx, PLAYER, /*b_index=*/50, /*building_id=*/41, BUILDING_TYPE_H_BARRACKS); // tier2

        fx.passable[(10 << 8) | 20] = 5; // distinct sentinel, not 0/1
        g_dir_from_to_result        = 137;

        g_mutate_storage_on_remove = true;
        g_mutate_player            = PLAYER;
        g_mutate_slot              = STORAGE_SLOT;
        g_mutate_park_x_to         = 44; // POST-call value dir_from_to's park args MUST observe
        g_mutate_park_y_to         = 55;
        g_mutate_exit_x_to         = 200; // POST-call value u.x/dir_from_to's exit args must NOT observe
        g_mutate_exit_y_to         = 210;
        g_mutate_b_index_to        = 50; // POST-call value the tier/building lookups must NOT observe

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_remove_docked_calls.size(), 1u,
              "T1: storage_remove_docked_unit called exactly once, 0x00489fc7-0x00489fd1");
        if (!g_remove_docked_calls.empty()) {
            ck_eq(g_remove_docked_calls[0].player, (uint32_t)PLAYER, "T1: remove_docked_unit player arg");
            ck_eq((uint32_t)g_remove_docked_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T1: remove_docked_unit unit_index arg");
            ck_eq((uint32_t)g_remove_docked_calls[0].storage_slot, (uint32_t)STORAGE_SLOT,
                  "T1: remove_docked_unit storage_slot arg");
        }

        ck_eq((uint32_t)g_dir_from_to_calls.size(), 1u, "T1: dir_from_to called exactly once, 0x0048a01a");
        if (!g_dir_from_to_calls.empty()) {
            const auto &d = g_dir_from_to_calls[0];
            ck_eq((uint32_t)d.x1, 44u << 5,
                  "T1: dir_from_to x1 = POST-remove park_x<<5 (fresh re-read AFTER the call), "
                  "0x00489ff5-0x00489ffb");
            ck_eq((uint32_t)d.y1, 55u << 5,
                  "T1: dir_from_to y1 = POST-remove park_y<<5, 0x0048a011-0x0048a01a");
            ck_eq((uint32_t)d.x2, 10u << 5,
                  "T1: dir_from_to x2 = PRE-remove exit_tile_x<<5 (snapshotted into a local BEFORE the "
                  "call, immune to the mock's mutation), 0x00489f73-0x00489f8c");
            ck_eq((uint32_t)d.y2, 20u << 5,
                  "T1: dir_from_to y2 = PRE-remove exit_tile_y<<5, 0x00489f8f-0x00489fa8");
            if (!g_remove_docked_calls.empty())
                ck(g_remove_docked_calls[0].seq < d.seq,
                   "T1: storage_remove_docked_unit fires BEFORE dir_from_to (call-order pin)");
        }

        ck_eq((uint32_t)u.facing_current, 137u, "T1: facing_current = (uint8_t)dir, 0x0048a038");
        ck_eq((uint32_t)u.facing_target, 137u, "T1: facing_target = (uint8_t)dir, same lookup, 0x0048a054");
        ck_eq((uint32_t)u.x, 10u,
              "T1: x = (uint8_t)PRE-remove exit_tile_x, NOT the mock's post-mutation value, 0x0048a070");
        ck_eq((uint32_t)u.y, 20u,
              "T1: y = (uint8_t)PRE-remove exit_tile_y, NOT the mock's post-mutation value, 0x0048a08c");
        ck_eq((uint32_t)u.origin_tile_was_passable, 5u,
              "T1: origin_tile_was_passable = passable[(exit_x<<8)|exit_y], one read, 0x0048a0b4");
        ck_eq_d(u.move_step_speed_scale, 5.0,
                "T1: move_step_speed_scale = (double)passable_val, an int-to-double CONVERSION of the "
                "SAME read (not a bit-reinterpret), 0x0048a0ce-0x0048a0e4");
        ck_eq((uint32_t)own.passable_at(10, 20), 0u,
              "T1: passable[(exit_x<<8)|exit_y] cleared to 0 as a THIRD, later access, 0x0048a0f3");
        ck_eq((uint32_t)(int32_t)u.move_microstep, 0u,
              "T1: move_microstep = 0x20 - tier(A_GARAGE=1)*0x20 = 0, using the PRE-remove "
              "building_index=6 snapshot (not the mock's mutated 50), 0x0048a123-0x0048a1a4");
        ck_eq((uint32_t)g_unload_calls.size(), 0u,
              "T1: non-shuttle building type -- prod_shuttle_unload_passengers never fires, "
              "0x0048a1ca-0x0048a1fa gate");
        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T1: non-shuttle building type -- bldg_flush_cargo_hold never fires, same gate");
        ck_eq((uint32_t)g_heading_calls.size(), 0u,
              "T1: soldier_count<=0 -- unit_soldiers_set_heading never fires, 0x0048a36d gate");
        ck_eq((uint32_t)g_cursor_calls.size(), 0u,
              "T1: soldier_count<=0 -- the soldier do-while never runs, 0x0048a36d gate");
        ck_eq((uint32_t)fx.population[PLAYER].human, 91u,
              "T1: pop_stats.human -= cfg_units[proto].human (100-9), 0x0048a4f6");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 49u,
              "T1: pop_stats.human_in_field += cfg_units[proto].human (40+9), SAME field read once, "
              "opposite sign, 0x0048a529");
        ck_eq((uint32_t)g_set_state_calls.size(), 1u,
              "T1: unit_set_state_of called exactly once -- the ONE state-setting call in the whole "
              "function, 0x0048a53b");
        if (!g_set_state_calls.empty()) {
            ck_eq((uint32_t)g_set_state_calls[0].player, (uint32_t)PLAYER, "T1: unit_set_state_of player arg");
            ck_eq((uint32_t)g_set_state_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T1: unit_set_state_of unit_index arg");
            ck_eq((uint32_t)(uint16_t)g_set_state_calls[0].state, 0x21u,
                  "T1: unit_set_state_of state = 0x21 (UNIT_STATE_EXIT_WALK_OUT)");
            if (!g_dir_from_to_calls.empty())
                ck(g_set_state_calls[0].seq > g_dir_from_to_calls[0].seq,
                   "T1: unit_set_state_of fires LAST among all recorded calls, 0x0048a52f-0x0048a53b");
        }
        ck_eq((uint32_t)g_setup_exit_path_calls.size(), 0u,
              "T1: storage_setup_exit_path belongs to the sibling llm_strat_storage_exit_air -- never "
              "fires here");
    }

    // =================================================================================================
    // T2 -- the 3-way building-type exit-delay tier classification (0x0048a139-0x0048a184), both sides
    // of every threshold: A_GARAGE(0x08), A_SHUTTLE(0x0d), H_BARRACKS(0x1b), H_GARAGE(0x1c),
    // H_SHUTTLE(0x21). shuttle_slot=0 throughout so the shuttle block never engages and cannot
    // interfere with the move_microstep read.
    // =================================================================================================
    {
        struct TierCase {
            uint8_t     type;
            int32_t     expect_microstep;
            const char *label;
        };
        static const TierCase kCases[] = {
            {0x00, -0x40, "0x00 (below A_GARAGE) -> tier3"},
            {0x07, -0x40, "0x07 (just below A_GARAGE) -> tier3"},
            {0x08, 0x00, "0x08 A_GARAGE -> tier1"},
            {0x09, -0x40, "0x09 (just above A_GARAGE) -> tier3"},
            {0x0c, -0x40, "0x0c (just below A_SHUTTLE) -> tier3"},
            {0x0d, 0x00, "0x0d A_SHUTTLE -> tier1"},
            {0x0e, -0x40, "0x0e (just above A_SHUTTLE) -> tier3"},
            {0x1a, -0x40, "0x1a (just below H_BARRACKS) -> tier3"},
            {0x1b, -0x20, "0x1b H_BARRACKS -> tier2"},
            {0x1c, 0x00, "0x1c H_GARAGE -> tier1"},
            {0x1d, -0x40, "0x1d (just above H_GARAGE) -> tier3"},
            {0x20, -0x40, "0x20 (just below H_SHUTTLE) -> tier3"},
            {0x21, 0x00, "0x21 H_SHUTTLE -> tier1"},
            {0x22, -0x40, "0x22 (just above H_SHUTTLE) -> tier3"},
            {0xff, -0x40, "0xff (arbitrary high) -> tier3"},
        };

        for (const TierCase &tc : kCases) {
            fx.reset();
            reset_recorders();

            unit &u                              = fx.u(PLAYER, UNIT_INDEX);
            u.unit_proto_id                      = PROTO_ID;
            fx.cfg_units[PROTO_ID].human         = 0;
            fx.cfg_units[PROTO_ID].soldier_count = 0;

            seed_storage(fx, PLAYER, STORAGE_SLOT, /*b_index=*/6, 10, 20, 3, 9);
            seed_building(fx, PLAYER, /*b_index=*/6, /*building_id=*/40, tc.type, /*shuttle_slot=*/0);

            g_dir_from_to_result = 0;

            sim_store own = fx.store();
            detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "T2_TIER: building type %s -> move_microstep, 0x0048a123-0x0048a1a4", tc.label);
            ck_eq((uint32_t)(int32_t)u.move_microstep, (uint32_t)tc.expect_microstep, msg);
        }
    }

    // =================================================================================================
    // T3 -- shuttle_slot==0 suppresses the ENTIRE shuttle/cargo block, even with both the unload and
    // flush preconditions otherwise satisfied (soldier_count==0, passengers_reserved==0, all-zero
    // cargo). A naive oracle gating only on building TYPE would miss this.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, /*b_index=*/6, /*building_id=*/40,
                          BUILDING_TYPE_A_SHUTTLE, /*shuttle_slot=*/0, PROTO_ID, /*soldier_count=*/0,
                          /*human=*/9);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + 0;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 0;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_unload_calls.size(), 0u,
              "T3: shuttle_slot==0 -- prod_shuttle_unload_passengers never fires, 0x0048a213-0x0048a21a "
              "gate");
        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T3: shuttle_slot==0 -- bldg_flush_cargo_hold never fires, same gate");
    }

    // =================================================================================================
    // T4 -- shuttle, shuttle_slot!=0, soldier_count==0: prod_shuttle_unload_passengers fires with
    // (player, building_index, cfg_units[proto].human) -- the CAP arg is `human`, NOT `soldier_count`
    // (the two fields sit right next to each other in the cfg struct, so this is a real swap risk).
    // passengers_reserved!=0 isolates: flush must NOT also fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 4;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, /*b_index=*/6, /*building_id=*/40,
                          BUILDING_TYPE_H_SHUTTLE, SHUTTLE_SLOT_ID, PROTO_ID, /*soldier_count=*/0,
                          /*human=*/13);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 1;
        g_dir_from_to_result                                  = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_unload_calls.size(), 1u,
              "T4: soldier_count==0 -- prod_shuttle_unload_passengers fires once, 0x0048a249-0x0048a276");
        if (!g_unload_calls.empty()) {
            ck_eq(g_unload_calls[0].player, (uint32_t)PLAYER, "T4: unload player arg");
            ck_eq((uint32_t)g_unload_calls[0].building_index, 6u,
                  "T4: unload building_index arg = the storage's b_index (pre-call snapshot)");
            ck_eq((uint32_t)g_unload_calls[0].cap, 13u,
                  "T4: unload cap arg = cfg_units[proto].human (13), NOT soldier_count (0), "
                  "0x0048a269-0x0048a276");
        }
        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T4: passengers_reserved!=0 -- bldg_flush_cargo_hold never fires, 0x0048a2ae gate");
    }

    // =================================================================================================
    // T5 -- same shuttle setup but soldier_count!=0: prod_shuttle_unload_passengers must NOT fire
    // (the two checks -- unload's soldier_count gate and flush's passengers_reserved gate -- are
    // independent, not mutually exclusive, so this isolates just the first one).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 4;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, /*b_index=*/6, /*building_id=*/40,
                          BUILDING_TYPE_H_SHUTTLE, SHUTTLE_SLOT_ID, PROTO_ID, /*soldier_count=*/3,
                          /*human=*/13);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 1; // also keeps flush from firing here
        unit &u                                               = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_unload_calls.size(), 0u,
              "T5: soldier_count!=0 -- prod_shuttle_unload_passengers never fires, 0x0048a247 gate");
    }

    // =================================================================================================
    // T6 -- flush fires: passengers_reserved==0, docked_count==0, all cargo entries zero.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 2;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, /*b_index=*/6, /*building_id=*/40,
                          BUILDING_TYPE_A_SHUTTLE, SHUTTLE_SLOT_ID, PROTO_ID, /*soldier_count=*/1,
                          /*human=*/0);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 0;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        fx.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_count = 0;
        unit &u                                                                     = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_flush_calls.size(), 1u,
              "T6: passengers_reserved==0, docked_count==0, no nonzero cargo -- bldg_flush_cargo_hold "
              "fires once, 0x0048a33a-0x0048a341");
        if (!g_flush_calls.empty()) {
            ck_eq(g_flush_calls[0].player, (uint32_t)PLAYER, "T6: flush player arg");
            ck_eq((uint32_t)g_flush_calls[0].building_index, 6u,
                  "T6: flush building_index arg = the storage's b_index");
        }
    }

    // =================================================================================================
    // T7 -- flush blocked by passengers_reserved != 0.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 2;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, 6, 40, BUILDING_TYPE_A_SHUTTLE,
                          SHUTTLE_SLOT_ID, PROTO_ID, 1, 0);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 7;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        unit &u = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T7: passengers_reserved!=0 -- bldg_flush_cargo_hold never fires, 0x0048a2ae gate");
    }

    // =================================================================================================
    // T8 -- flush blocked by docked_count != 0 (passengers_reserved==0, no nonzero cargo).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 2;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, 6, 40, BUILDING_TYPE_A_SHUTTLE,
                          SHUTTLE_SLOT_ID, PROTO_ID, 1, 0);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 0;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        fx.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_count = 3;
        unit &u                                                                     = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T8: docked_count!=0 -- bldg_flush_cargo_hold never fires, 0x0048a329-0x0048a336 gate");
    }

    // =================================================================================================
    // T9 -- flush blocked by a nonzero cargo entry INSIDE the scan range: entry #49 (the LAST index the
    // i<50 loop visits) has a nonzero leading uint16_t.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 2;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, 6, 40, BUILDING_TYPE_A_SHUTTLE,
                          SHUTTLE_SLOT_ID, PROTO_ID, 1, 0);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 0;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        fx.prod_shuttle_slots[slot_index].cargo_manifest_raw[49 * 14 + 0]           = 0xAA;
        fx.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_count = 0;
        unit &u                                                                     = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_flush_calls.size(), 0u,
              "T9: cargo entry #49 (LAST index the i<50 loop scans) is nonzero -- bldg_flush_cargo_hold "
              "never fires, 0x0048a2c2-0x0048a30c");
    }

    // NOTE: a mirror "entry #50 lies OUTSIDE the i<50 scan range" case was deliberately NOT written --
    // cargo_manifest_raw is uint8_t[696] (696/14 = 49.71 entries), so entry #50's own leading bytes
    // (index 700-701) lie past the array's declared 696-byte extent entirely; writing there would be an
    // out-of-bounds write in THIS TEST, not a legitimate probe of the loop bound. T9 (entry #49, the
    // LAST index the loop legitimately visits, whose leading 2 bytes at 686-687 are still just inside
    // the array) is the boundary evidence from the in-range side. See this file's REPORT for the same
    // note -- flagged as something this oracle could not safely resolve.

    // =================================================================================================
    // T11 -- only entry #0's LEADING uint16_t (bytes 0-1) is checked, not the whole 14-byte entry: a
    // nonzero byte elsewhere in the entry (offset 5) must not be seen.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        const uint8_t SHUTTLE_SLOT_ID = 2;
        seed_shuttle_case(fx, PLAYER, UNIT_INDEX, STORAGE_SLOT, 6, 40, BUILDING_TYPE_A_SHUTTLE,
                          SHUTTLE_SLOT_ID, PROTO_ID, 1, 0);
        const int32_t slot_index                              = PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SHUTTLE_SLOT_ID;
        fx.prod_shuttle_slots[slot_index].passengers_reserved = 0;
        std::memset(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw, 0,
                    sizeof(fx.prod_shuttle_slots[slot_index].cargo_manifest_raw));
        fx.prod_shuttle_slots[slot_index].cargo_manifest_raw[0 * 14 + 5]            = 0xFF; // mid-entry, not leading
        fx.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].docked_count = 0;
        unit &u                                                                     = fx.u(PLAYER, UNIT_INDEX);
        seed_trivial_soldier_chain(fx, u, PLAYER, 66, 77);
        g_dir_from_to_result = 0;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_flush_calls.size(), 1u,
              "T11: entry #0's leading uint16_t (bytes 0-1) stays zero -- a nonzero byte elsewhere in "
              "the 14-byte entry is not counted -- bldg_flush_cargo_hold still fires, 0x0048a304 "
              "stride-width pin");
    }

    // =================================================================================================
    // T12 -- soldier reseat chain, TWO soldiers, all seed values distinct and non-symmetric: pins the
    // (total, remaining) argument order to cursor_lookup_offset_pair (total is a NEVER-decremented
    // local, remaining IS decremented -- see the header's long note), that each iteration's out-pointers
    // target the CURRENT chain link (not a stale one), and that start_x/end_x/start_y/end_y are each
    // soldier's OWN cur_x/cur_y (not the cursor_lookup_offset_pair-written value, which is immediately
    // overwritten).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        unit &u                              = fx.u(PLAYER, UNIT_INDEX);
        u.unit_proto_id                      = PROTO_ID;
        fx.cfg_units[PROTO_ID].soldier_count = 2; // total = 2
        fx.cfg_units[PROTO_ID].human         = 0;
        seed_storage(fx, PLAYER, STORAGE_SLOT, 6, 10, 20, 3, 9);
        seed_building(fx, PLAYER, 6, 40, BUILDING_TYPE_A_GARAGE, /*shuttle_slot=*/0); // non-shuttle
        g_dir_from_to_result = 55;                                                    // facing sentinel, distinct from every soldier field below

        u.unit_above[0] = 3;
        u.unit_above[1] = 0; // chain head = soldier index 3

        soldier &sA     = fx.soldiers[(size_t)PLAYER * SOLDIERS_PER_PLAYER + 3];
        sA.cur_x        = 11;
        sA.cur_y        = 21;
        sA.next_soldier = 8;
        sA.start_x = sA.start_y = sA.end_x = sA.end_y = -1; // garbage, must be overwritten

        soldier &sB     = fx.soldiers[(size_t)PLAYER * SOLDIERS_PER_PLAYER + 8];
        sB.cur_x        = 31;
        sB.cur_y        = 41;
        sB.next_soldier = 0; // terminates the chain
        sB.start_x = sB.start_y = sB.end_x = sB.end_y = -2;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_heading_calls.size(), 1u,
              "T12: soldier_count>0 -- unit_soldiers_set_heading fires once, 0x0048a37d");
        if (!g_heading_calls.empty()) {
            ck_eq(g_heading_calls[0].player, (uint32_t)PLAYER, "T12: heading player arg");
            ck_eq((uint32_t)g_heading_calls[0].unit_index, (uint32_t)UNIT_INDEX,
                  "T12: heading unit_index arg");
            ck_eq((uint32_t)g_heading_calls[0].sprite_frame, 55u,
                  "T12: heading sprite_frame = (uint8_t)dir, SAME facing as facing_target/current");
        }

        ck_eq((uint32_t)g_cursor_calls.size(), 2u,
              "T12: chain of 2 soldiers -- cursor_lookup_offset_pair fires exactly twice, 0x0048a3ce "
              "do-while");
        if (g_cursor_calls.size() == 2) {
            ck_eq((uint32_t)g_cursor_calls[0].table_col, 2u,
                  "T12 iter1: table_col = total, the NEVER-decremented local, 0x0048a3a8/0x0048a405");
            ck_eq((uint32_t)g_cursor_calls[0].table_row, 2u,
                  "T12 iter1: table_row = remaining PRE-decrement (== total on iter1), "
                  "0x0048a3ab-0x0048a3ae/0x0048a402");
            ck_eq((uint32_t)g_cursor_calls[0].soldier_index, (uint32_t)PLAYER * SOLDIERS_PER_PLAYER + 3,
                  "T12 iter1: out-pointers target chain==unit_above's soldier record (index 3, flat "
                  "index PLAYER*SOLDIERS_PER_PLAYER+3 -- the recorder decodes a FLAT array offset)");
            ck_eq((uint32_t)g_cursor_calls[0].out_a_field_offset, 7u,
                  "T12 iter1: out_a points at the record's start_x field (+0x7)");
            ck_eq((uint32_t)g_cursor_calls[0].out_b_field_offset, 8u,
                  "T12 iter1: out_b points at the record's start_y field (+0x8)");
            ck_eq((uint32_t)g_cursor_calls[1].table_col, 2u,
                  "T12 iter2: table_col = the SAME total, unchanged across iterations");
            ck_eq((uint32_t)g_cursor_calls[1].table_row, 1u,
                  "T12 iter2: table_row = remaining, decremented once (2->1) by the iter1 tail, "
                  "0x0048a4ba");
            ck_eq((uint32_t)g_cursor_calls[1].soldier_index, (uint32_t)PLAYER * SOLDIERS_PER_PLAYER + 8,
                  "T12 iter2: out-pointers now target the NEXT chain link (soldier[3].next_soldier==8, "
                  "flat index PLAYER*SOLDIERS_PER_PLAYER+8), 0x0048a4a1-0x0048a4b4 -> 0x0048a3ce");
        }

        ck_eq((uint32_t)(uint8_t)sA.start_x, 11u,
              "T12: soldier[3].start_x = cur_x (11), overwriting cursor_lookup_offset_pair's write, "
              "0x0048a433");
        ck_eq((uint32_t)(uint8_t)sA.end_x, 11u, "T12: soldier[3].end_x = start_x, 0x0048a44f");
        ck_eq((uint32_t)(uint8_t)sA.start_y, 21u, "T12: soldier[3].start_y = cur_y (21), 0x0048a47b");
        ck_eq((uint32_t)(uint8_t)sA.end_y, 21u, "T12: soldier[3].end_y = start_y, 0x0048a497");
        ck_eq((uint32_t)(uint8_t)sB.start_x, 31u,
              "T12: soldier[8].start_x = its OWN cur_x (31), not soldier[3]'s");
        ck_eq((uint32_t)(uint8_t)sB.end_x, 31u, "T12: soldier[8].end_x = start_x");
        ck_eq((uint32_t)(uint8_t)sB.start_y, 41u, "T12: soldier[8].start_y = its OWN cur_y (41)");
        ck_eq((uint32_t)(uint8_t)sB.end_y, 41u, "T12: soldier[8].end_y = start_y");

        ck_eq((uint32_t)fx.population[PLAYER].human, 0u,
              "T12: soldier_count>0 -- pop_stats untouched, the two arms are mutually exclusive, "
              "0x0048a36d branch split");
    }

    // =================================================================================================
    // T13 -- DO-WHILE, not while(chain!=0): unit_above==0, so `chain` starts at 0 going into the loop.
    // A while-loop mistranslation would run zero iterations; the real asm has no pre-loop test (only
    // the tail CMP/JNZ), so the body runs exactly once regardless.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        unit &u                              = fx.u(PLAYER, UNIT_INDEX);
        u.unit_proto_id                      = PROTO_ID;
        fx.cfg_units[PROTO_ID].soldier_count = 1; // total = 1, just >0
        fx.cfg_units[PROTO_ID].human         = 0;
        seed_storage(fx, PLAYER, STORAGE_SLOT, 6, 10, 20, 3, 9);
        seed_building(fx, PLAYER, 6, 40, BUILDING_TYPE_A_GARAGE, 0);
        g_dir_from_to_result = 0;

        u.unit_above[0] = 0;
        u.unit_above[1] = 0; // chain head == 0

        soldier &s0     = fx.soldiers[(size_t)PLAYER * SOLDIERS_PER_PLAYER + 0];
        s0.cur_x        = 66;
        s0.cur_y        = 77;
        s0.next_soldier = 0; // terminates after exactly one pass
        s0.start_x = s0.start_y = s0.end_x = s0.end_y = -9;

        sim_store own = fx.store();
        detail::storage_place_exit_ground(fx.view(), own, g_calls, PLAYER, UNIT_INDEX, STORAGE_SLOT);

        ck_eq((uint32_t)g_cursor_calls.size(), 1u,
              "T13: chain starts at 0 yet the loop fires ONCE -- proves DO-WHILE, not a "
              "while(chain!=0) that would run zero times, 0x0048a3ce (unconditional first pass, tail "
              "test only at 0x0048a4bd-0x0048a4c1)");
        if (!g_cursor_calls.empty()) {
            ck_eq((uint32_t)g_cursor_calls[0].soldier_index, (uint32_t)PLAYER * SOLDIERS_PER_PLAYER + 0,
                  "T13: the single iteration operates on soldier record 0 (chain's initial value, flat "
                  "index PLAYER*SOLDIERS_PER_PLAYER+0)");
            ck_eq((uint32_t)g_cursor_calls[0].table_col, 1u, "T13: table_col = total (1)");
            ck_eq((uint32_t)g_cursor_calls[0].table_row, 1u,
                  "T13: table_row = remaining PRE-decrement (== total on the only iteration)");
        }
        ck_eq((uint32_t)(uint8_t)s0.start_x, 66u,
              "T13: soldier[0].start_x = cur_x (66) -- the do-while body really executed");
        ck_eq((uint32_t)(uint8_t)s0.start_y, 77u, "T13: soldier[0].start_y = cur_y (77)");
    }

    g_active_fx = nullptr;
}

} // namespace mh::sim::test
