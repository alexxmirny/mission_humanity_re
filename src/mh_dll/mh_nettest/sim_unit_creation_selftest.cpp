//
// sim_unit_creation_selftest.cpp -- `simtest` cases for the two SIM1A UNIT-CREATION PRIMITIVES:
//   * llm_strat_unit_spawn_on_tile @0x00464119  (sim/sim_unit_spawn_on_tile.h/.cpp)
//   * llm_unit_create_soldier      @0x00463ac6  (sim/sim_unit_create_soldier.h/.cpp)
//
// Every EXPECTED value below is derived from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_spawn_on_tile_00464119.asm, llm_unit_create_soldier_00463ac6.asm),
// used as the independent oracle -- not from the C++ translation.
//
// ---- A NOTE ON WHAT IS AND IS NOT TESTABLE OFFLINE (read before the create_soldier section) ------
// The two functions differ in ONE decisive way for an offline oracle:
//
//   * spawn_on_tile takes a CALLS STRUCT (unit_spawn_on_tile_calls c) -- its three external callees
//     (unit_init_record / fow_update_plus / ai_notify_unit_lifecycle) are function pointers, so a
//     recording mock supplies them and the FULL body (slot allocation + every record-init write +
//     the callee sequence/args) is exercised here. FULLY tested below.
//
//   * create_soldier does NOT take a calls struct. Its body calls mh::call::llm_strat_unit_init_record
//     (and housing_count_add / population_add / map_fow_UpdateFoWPlus / ai_notify_unit_lifecycle)
//     DIRECTLY -- each marshals through an absolute EN-image VA (e.g. 0x004619b0), which is UNMAPPED
//     in net_selftest.exe. So the moment the function reaches a successful placement it jumps into
//     unmapped memory. THE RECORD-INIT / SLOT-ALLOCATION CORE OF create_soldier IS THEREFORE BLOCKED
//     OFFLINE and is verified by the live shadow-armed run instead. What IS testable offline is every
//     path on which the CORRECT function returns 0 BEFORE reaching any mh::call:: callee -- the
//     population-cap guard rejections, the slot-search-exhaustion bound (incl. the is_special_flag
//     reduction), and the outward-scan exhaustion. Those are covered, with the falsification mechanism
//     documented per case. See the banner above the create_soldier cases.
//
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT (== 1), the unit's own order/state value
#include "sim/sim_unit_create_soldier.h"
#include "sim/sim_unit_spawn_on_tile.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// =================================================================================================
// llm_strat_unit_spawn_on_tile @0x00464119  -- FULLY testable (mockable calls struct)
// =================================================================================================

// Recording mock for the 3-member unit_spawn_on_tile_calls struct. `roster`/`init_stamps`/
// `init_stamp_proto` let the init_record mock MODEL the real callee's one observable write --
// stamping units[player][slot].unit_proto_id -- so the later fow/notify reads (which the asm takes
// off the RECORD at 0x004642e0 / 0x0046431e, a fresh load, NOT the unit_proto_id parameter) can be
// checked against the stamped value rather than the argument.
struct spawn_log_t {
    int      init_calls       = 0;
    int32_t  last_init_slot   = -1;
    uint32_t last_init_proto  = 0;
    uint32_t last_init_player = 0;
    unit    *roster           = nullptr; // where the init mock stamps unit_proto_id (models the real callee)
    bool     init_stamps      = false;
    uint16_t init_stamp_proto = 0;

    int      fow_calls       = 0;
    uint32_t last_fow_player = 0, last_fow_x = 0, last_fow_y = 0;
    uint8_t  last_fow_sight = 0;

    int      notify_calls       = 0;
    uint16_t last_notify_player = 0, last_notify_type = 0;
    uint32_t last_notify_unit_id = 0, last_notify_kind = 0;

    void reset() { *this = spawn_log_t{}; }
};
spawn_log_t g_spawn;

const unit_spawn_on_tile_calls &recording_spawn_calls() {
    static const unit_spawn_on_tile_calls c = {
        [](int32_t unit_idx, uint32_t unit_proto_id, uint32_t player) {
            ++g_spawn.init_calls;
            g_spawn.last_init_slot   = unit_idx;
            g_spawn.last_init_proto  = unit_proto_id;
            g_spawn.last_init_player = player;
            if (g_spawn.init_stamps && g_spawn.roster != nullptr)
                g_spawn.roster[player * UNITS_PER_PLAYER + unit_idx].unit_proto_id =
                    g_spawn.init_stamp_proto;
        },
        [](uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
            ++g_spawn.fow_calls;
            g_spawn.last_fow_player = player;
            g_spawn.last_fow_x      = x;
            g_spawn.last_fow_y      = y;
            g_spawn.last_fow_sight  = sight;
        },
        [](uint16_t player, uint16_t unit_type, uint32_t unit_id, uint32_t kind) {
            ++g_spawn.notify_calls;
            g_spawn.last_notify_player  = player;
            g_spawn.last_notify_type    = unit_type;
            g_spawn.last_notify_unit_id = unit_id;
            g_spawn.last_notify_kind    = kind;
        },
    };
    return c;
}

// ---- (1) occupied tile -> the class_owner!=0 EARLY-OUT (0x0046413a-0x0046414f). Returns 0 before
// the slot search even starts; NO callee fires and NOTHING is written. --------------------------
void test_spawn_occupied_tile_early_out() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 3;
    const uint32_t x = 30, y = 17;

    f.t(x, y).class_owner                = 0x81; // tile already owned -> early-out
    f.u(player, 0).order                 = 0x40; // header-row sentinel: must survive
    f.unit_housing[player].used_soldiers = 100;  // sentinel: must survive

    g_spawn.reset();
    const int32_t rc =
        detail::unit_spawn_on_tile(v, own, recording_spawn_calls(), x, y, /*proto=*/12, player);

    ck_eq((uint32_t)rc, 0u, "spawn/occupied: class_owner!=0 -> returns 0 (early-out)");
    ck(g_spawn.init_calls == 0 && g_spawn.fow_calls == 0 && g_spawn.notify_calls == 0,
       "spawn/occupied: no callee fires on the early-out");
    ck_eq((uint32_t)f.u(player, 0).order, 0x40u,
          "spawn/occupied: the header-row order is NOT touched on the early-out");
    ck_eq((uint32_t)f.u(player, 1).unit_proto_id, 0u,
          "spawn/occupied: slot 1 is untouched -- no placement happened");
    ck_eq((uint32_t)f.unit_housing[player].used_soldiers, 100u,
          "spawn/occupied: used_soldiers is not bumped on the early-out");
}

// ---- (2) HAPPY PATH: empty tile, slot 1 free -> full record init + the exact callee sequence/args.
// Asserts EVERY state write the body itself does and proves the fow/notify re-read unit_proto_id off
// the RECORD (the value init_record stamped), not the parameter. ---------------------------------
void test_spawn_places_slot_one_full_record_init_and_callees() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player      = 3;
    const uint16_t param_proto = 12; // the ARGUMENT
    const uint16_t stamp_proto = 9;  // what the (mocked) init_record writes onto the record
    const uint32_t x = 30, y = 17;

    // Distinct sight rows: the STAMPED proto's sight must be what fow gets, NOT the argument's.
    f.cfg_units[stamp_proto].sight = 0x2a;
    f.cfg_units[param_proto].sight = 0x77; // must never surface

    f.passable[(x << 8) | y]             = 5;    // origin_tile_was_passable source (distinct, nonzero)
    f.u(player, 0).order                 = 0x40; // header-row sentinel -> +1 = 0x41
    f.unit_housing[player].used_soldiers = 100;  // -> +1 = 101

    g_spawn.reset();
    g_spawn.roster           = f.units.data();
    g_spawn.init_stamps      = true;
    g_spawn.init_stamp_proto = stamp_proto;

    const int32_t rc =
        detail::unit_spawn_on_tile(v, own, recording_spawn_calls(), x, y, param_proto, player);

    // Slot choice: search is [1, 0x5b); all slots free -> the FIRST candidate, slot 1, is taken.
    ck_eq((uint32_t)rc, 1u, "spawn/place: all-free roster -> slot 1 (search starts at 1, not 0)");

    // init_record: called once, with (slot, ARGUMENT proto, player) -- the arg, pre-stamp.
    ck(g_spawn.init_calls == 1 && g_spawn.last_init_slot == 1 &&
           g_spawn.last_init_proto == param_proto && g_spawn.last_init_player == player,
       "spawn/place: unit_init_record(slot=1, arg_proto=12, player=3) fires once, BEFORE the field writes");

    // The header-row increment (0x0046419f, INC word ptr on units[player][0].order) -- slot 0, NOT
    // the placed slot; width WORD.
    ck_eq((uint32_t)f.u(player, 0).order, 0x41u,
          "spawn/place: units[player][0].order (the HEADER row) is incremented by 1");

    unit &u = f.u(player, 1);
    // The NEW unit's own order/state (0x004641c9/0x004641e5) -- distinct field meaning from the header.
    ck_eq((uint32_t)u.order, (uint32_t)UNIT_STATE_STOP_TO_DEFAULT,
          "spawn/place: the placed unit's own order = STOP_TO_DEFAULT (1)");
    ck_eq((uint32_t)u.state, (uint32_t)UNIT_STATE_STOP_TO_DEFAULT,
          "spawn/place: the placed unit's own state = STOP_TO_DEFAULT (1)");
    ck_eq((uint32_t)u.x, (uint32_t)x, "spawn/place: unit.x = tile_x, truncated to byte");
    ck_eq((uint32_t)u.y, (uint32_t)y, "spawn/place: unit.y = tile_y, truncated to byte");
    ck_eq((uint32_t)u.origin_tile_was_passable, 5u,
          "spawn/place: origin_tile_was_passable = passable[x][y] read BEFORE the clear");
    ck_eq_d(u.move_step_speed_scale, 5.0,
            "spawn/place: move_step_speed_scale = (double)origin_tile_was_passable (FILD m16int, exact)");
    ck_eq((uint32_t)u.unit_proto_id, (uint32_t)stamp_proto,
          "spawn/place: the record now holds the STAMPED proto (init_record's write is visible)");

    // Tile claim (0x0046428f/0x004642aa): building=slot (word), class_owner=player|0x80 (byte).
    ck_eq((uint32_t)own.tile_object_at((int32_t)x, (int32_t)y).building, 1u,
          "spawn/place: tile_objects[x][y].building = slot (1)");
    ck_eq((uint32_t)own.tile_object_at((int32_t)x, (int32_t)y).class_owner, (uint32_t)(player | 0x80u),
          "spawn/place: tile_objects[x][y].class_owner = player|0x80 (0x83) -- the 0x80 bit is set");

    // passable cleared AFTER the tile claim (0x004642b9).
    ck_eq((uint32_t)own.passable_at((int32_t)x, (int32_t)y), 0u,
          "spawn/place: passable[x][y] cleared to 0 (after the tile claim, after the origin read)");

    // used_soldiers bump (0x004642c0, INC dword ptr, indexed by player).
    ck_eq((uint32_t)f.unit_housing[player].used_soldiers, 101u,
          "spawn/place: unit_housing[player].used_soldiers += 1");

    // FoW then AI-notify, in that order; BOTH read unit_proto_id off the RECORD (the stamped 9), not
    // the argument (12). This is the re-read-off-record quirk.
    ck(g_spawn.fow_calls == 1 && g_spawn.last_fow_player == player && g_spawn.last_fow_x == x &&
           g_spawn.last_fow_y == y,
       "spawn/place: map_fow_UpdateFoWPlus(player, x, y, sight) fires once");
    ck_eq((uint32_t)g_spawn.last_fow_sight, 0x2au,
          "spawn/place: FoW sight = cfg_units[STAMPED proto=9].sight (0x2a) -- re-read off the record, "
          "NOT cfg_units[arg=12].sight (0x77)");
    ck(g_spawn.notify_calls == 1 && g_spawn.last_notify_player == player &&
           g_spawn.last_notify_unit_id == 1u && g_spawn.last_notify_kind == 4u,
       "spawn/place: ai_notify_unit_lifecycle(player, .., unit_id=slot, kind=4) fires once, LAST");
    ck_eq((uint32_t)g_spawn.last_notify_type, (uint32_t)stamp_proto,
          "spawn/place: notify unit_type = the STAMPED proto (9) read off the record, not the arg (12)");
}

// ---- (3) SLOT-SEARCH EXHAUSTION at the RAW 0x5b bound. The search is [1, 0x5b=91), NOT
// UNITS_PER_PLAYER (100): slots 1..90 occupied, 91..99 FREE -> returns 0 without ever inspecting a
// free slot. A translation using bound 100 would find slot 91 free and place there (return 91). --
void test_spawn_slot_search_bound_is_0x5b_not_units_per_player() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 4;
    const uint32_t x = 12, y = 200;

    for (int32_t s = 1; s <= 90; ++s) f.u(player, s).unit_proto_id = 1; // slots 1..90 occupied
    // slots 91..99 left FREE (unit_proto_id == 0) on purpose.
    f.u(player, 0).order                 = 0x55; // header sentinel
    f.unit_housing[player].used_soldiers = 200;  // sentinel

    g_spawn.reset();
    const int32_t rc =
        detail::unit_spawn_on_tile(v, own, recording_spawn_calls(), x, y, /*proto=*/7, player);

    ck_eq((uint32_t)rc, 0u,
          "spawn/bound: slots 1..90 full, 91..99 free -> returns 0 (search bound is 0x5b=91, not 100)");
    ck_eq((uint32_t)f.u(player, 91).unit_proto_id, 0u,
          "spawn/bound: slot 91 was NEVER inspected -- it stays free (a bound of 100 would have used it)");
    ck(g_spawn.init_calls == 0 && g_spawn.fow_calls == 0 && g_spawn.notify_calls == 0,
       "spawn/bound: exhausted search fires no callees");
    ck_eq((uint32_t)f.u(player, 0).order, 0x55u,
          "spawn/bound: header-row order untouched on exhaustion");
    ck_eq((uint32_t)f.unit_housing[player].used_soldiers, 200u,
          "spawn/bound: used_soldiers untouched on exhaustion");
    ck_eq((uint32_t)f.t(x, y).class_owner, 0u, "spawn/bound: the tile is never claimed on exhaustion");
}

// ---- (4) the search SKIPS occupied slots and takes the first FREE one. Slots 1,2 occupied, 3 free
// -> slot 3; slots 1,2 must be left untouched. ---------------------------------------------------
void test_spawn_skips_occupied_takes_first_free() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 2;
    const uint32_t x = 44, y = 90;

    f.u(player, 1).unit_proto_id = 111; // occupied
    f.u(player, 2).unit_proto_id = 222; // occupied

    g_spawn.reset();
    const int32_t rc =
        detail::unit_spawn_on_tile(v, own, recording_spawn_calls(), x, y, /*proto=*/8, player);

    ck_eq((uint32_t)rc, 3u, "spawn/skip: slots 1,2 occupied -> placed in slot 3 (first free)");
    ck(g_spawn.init_calls == 1 && g_spawn.last_init_slot == 3,
       "spawn/skip: init_record was called for slot 3, the chosen slot");
    ck(f.u(player, 1).unit_proto_id == 111 && f.u(player, 2).unit_proto_id == 222,
       "spawn/skip: the skipped occupied slots 1 and 2 are left untouched");
    ck_eq((uint32_t)own.tile_object_at((int32_t)x, (int32_t)y).building, 3u,
          "spawn/skip: tile_objects.building names the chosen slot (3)");
}

// ---- (5) the header-row increment is a WORD store: 0xffff -> 0x0000, and it targets slot 0, NOT the
// placed slot (whose own order/state become STOP_TO_DEFAULT=1). ----------------------------------
void test_spawn_header_increment_is_word_wide_and_hits_slot_zero() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 5;
    const uint32_t x = 7, y = 8;

    f.u(player, 0).order = 0xffff; // header row at the WORD boundary

    g_spawn.reset(); // init_stamps stays false -> record keeps proto 0; this case does not assert on it
    const int32_t rc =
        detail::unit_spawn_on_tile(v, own, recording_spawn_calls(), x, y, /*proto=*/6, player);

    ck_eq((uint32_t)rc, 1u, "spawn/word-inc: places in slot 1");
    ck_eq((uint32_t)f.u(player, 0).order, 0x0000u,
          "spawn/word-inc: header-row order 0xffff + 1 WRAPS to 0x0000 (a WORD store, not a dword)");
    ck_eq((uint32_t)f.u(player, 1).order, (uint32_t)UNIT_STATE_STOP_TO_DEFAULT,
          "spawn/word-inc: the placed slot's OWN order is STOP_TO_DEFAULT(1) -- a different field than the header");
}

// =================================================================================================
// llm_unit_create_soldier @0x00463ac6  -- PARTIALLY BLOCKED OFFLINE
// =================================================================================================
//
// BLOCKED: the record-init / slot-allocation CORE. On any successful placement the body calls
// mh::call::llm_strat_unit_housing_count_add -> ...init_record -> ...population_add ->
// map_fow_UpdateFoWPlus -> ...ai_notify_unit_lifecycle DIRECTLY, each an absolute EN-image VA that is
// unmapped here (see mh_calls.gen.h: e.g. init_record -> 0x004619b0). There is NO calls struct to
// mock, so the instant the function reaches an empty slot it jumps into unmapped memory. That path is
// verified by the live shadow-armed run, not offline.
//
// TESTABLE: every path on which the CORRECT function returns 0 BEFORE reaching any mh::call:: callee.
// FALSIFICATION MECHANISM for these cases: they are set up so a mis-translation that WRONGLY proceeds
// to placement dereferences an unmapped VA and crashes the process (a hard failure signal); the
// correct function returns 0 cleanly. Each case documents which mutation it would catch.
// Signature: create_soldier(v, own, tile_x, tile_y, unit_proto_id, player, is_special_flag).
// -------------------------------------------------------------------------------------------------

// ---- (CS-1) POPULATION-CAP GUARD reject (0x00463afd-0x00463b37). soldier_count!=0 and
// zx(owner_unit)+soldier_count+1 >= 100 -> returns 0 BEFORE the tile/slot logic. The tile is left
// directly placeable with slot 1 free, so a guard that wrongly passed would place and crash. ------
void test_create_soldier_popcap_guard_rejects_at_boundary_100() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 2;
    const uint16_t proto  = 17;
    const uint32_t tx = 20, ty = 15;

    // 39 + 60 + 1 == 100, and the CMP is `>= 0x64` (JL past it) -> EXACTLY 100 rejects.
    f.cfg_units[proto].soldier_count                        = 60;
    f.soldiers[player * SOLDIERS_PER_PLAYER + 0].owner_unit = 39;

    // Placeable target + free slot: the trap that turns a guard mis-translation into a crash.
    f.t(tx, ty).class_owner    = 0;    // class_owner==0
    f.passable[(tx << 8) | ty] = 1;    // passable!=0  (both needed for direct placement)
    f.u(player, 0).order       = 0x30; // header sentinel

    const uint32_t rc = detail::create_soldier(v, own, tx, ty, proto, player, /*is_special=*/0);

    ck_eq(rc, 0u, "create_soldier/cap: owner_unit(39)+soldier_count(60)+1 == 100 -> reject (>=100)");
    ck_eq((uint32_t)f.u(player, 0).order, 0x30u,
          "create_soldier/cap: rejected before the header increment -- slot-0 order untouched");
    ck_eq((uint32_t)f.t(tx, ty).class_owner, 0u,
          "create_soldier/cap: rejected before the tile claim -- tile still unowned");
    ck_eq((uint32_t)f.passable[(tx << 8) | ty], 1u,
          "create_soldier/cap: rejected before passable is cleared");
    ck_eq((uint32_t)f.u(player, 1).unit_proto_id, 0u,
          "create_soldier/cap: no unit was placed in the free slot");
}

// ---- (CS-2) the guard ZERO-EXTENDS owner_unit (asm: MOVZX word). A high-bit owner_unit (0x8000)
// must read as 32768, so +1+1 = 32770 >= 100 -> reject. The bug reimpl-verify caught (2026-08-10) was
// a SIGN-extending cast: (int16_t)0x8000 == -32768 -> -32766 < 100 -> would pass -> placement -> the
// crash this case would produce. Correct code returns 0 cleanly. ---------------------------------
void test_create_soldier_popcap_guard_zero_extends_owner_unit() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 2;
    const uint16_t proto  = 18;
    const uint32_t tx = 22, ty = 16;

    f.cfg_units[proto].soldier_count = 1;
    // high bit set. `owner_unit` is int16_t, so the cast is what the assignment means anyway -- spelled
    // explicitly because the bare literal is a C4309 truncation warning (unmasked 2026-08-16 by an
    // unrelated sim_test_support.h edit forcing a full rebuild; the value is unchanged).
    f.soldiers[player * SOLDIERS_PER_PLAYER + 0].owner_unit = (int16_t)0x8000;

    f.t(tx, ty).class_owner    = 0;
    f.passable[(tx << 8) | ty] = 1;
    f.u(player, 0).order       = 0x31;

    const uint32_t rc = detail::create_soldier(v, own, tx, ty, proto, player, /*is_special=*/0);

    ck_eq(rc, 0u,
          "create_soldier/zx: owner_unit 0x8000 ZERO-extends to 32768 -> 32770 >= 100 -> reject "
          "(a sign-extend would pass and crash here)");
    ck_eq((uint32_t)f.u(player, 0).order, 0x31u,
          "create_soldier/zx: rejected before the header increment");
    ck_eq((uint32_t)f.u(player, 1).unit_proto_id, 0u, "create_soldier/zx: nothing placed");
}

// ---- (CS-4) SLOT-SEARCH EXHAUSTION at the full bound (is_special=0 -> bound 100 -> slots [1,100)).
// soldier_count==0 skips the pop-cap guard; the target tile is directly placeable; slots 1..99 are
// ALL occupied -> the search finds no free slot and returns 0 without reaching a placement. A bound
// of 101 (slot<=100) would inspect slot 100 (== next player's slot 0, free) and crash. -----------
void test_create_soldier_slot_search_exhausts_full_bound() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 3;
    const uint16_t proto  = 5; // soldier_count stays 0 (default) -> pop-cap guard skipped
    const uint32_t tx = 25, ty = 33;

    f.t(tx, ty).class_owner    = 0;
    f.passable[(tx << 8) | ty] = 1;                                     // directly placeable
    for (int32_t s = 1; s <= 99; ++s) f.u(player, s).unit_proto_id = 1; // slots 1..99 all occupied
    f.u(player, 0).order = 0x22;                                        // header sentinel

    const uint32_t rc = detail::create_soldier(v, own, tx, ty, proto, player, /*is_special=*/0);

    ck_eq(rc, 0u, "create_soldier/exhaust: placeable tile + all of slots 1..99 occupied -> returns 0");
    ck_eq((uint32_t)f.u(player, 0).order, 0x22u,
          "create_soldier/exhaust: no free slot found -> header increment never ran");
    ck_eq((uint32_t)f.t(tx, ty).class_owner, 0u,
          "create_soldier/exhaust: tile never claimed (placement never entered)");
    ck_eq((uint32_t)f.passable[(tx << 8) | ty], 1u,
          "create_soldier/exhaust: passable never cleared");
}

// ---- (CS-5) the is_special_flag REDUCTION of the slot-search bound (asm: is_special==1 -> the
// reduction local = 9, so the loop bound is 100-9 = 91 -> slots [1,91)). Slots 1..90 occupied,
// 91..99 FREE: is_special=1 exhausts at 91 without ever reaching the free high slots. If the flag
// were ignored (bound stayed 100) it would place in slot 91 and crash. The is_special=0 counterpart
// (bound 100) WOULD place in slot 91 -> crash -> so only the reduced-bound side is asserted here. -
void test_create_soldier_is_special_flag_reduces_slot_bound_to_91() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 4;
    const uint16_t proto  = 6; // soldier_count 0 -> guard skipped
    const uint32_t tx = 18, ty = 44;

    f.t(tx, ty).class_owner    = 0;
    f.passable[(tx << 8) | ty] = 1;                                     // directly placeable
    for (int32_t s = 1; s <= 90; ++s) f.u(player, s).unit_proto_id = 1; // slots 1..90 occupied
    // slots 91..99 FREE.
    f.u(player, 0).order = 0x23;

    const uint32_t rc = detail::create_soldier(v, own, tx, ty, proto, player, /*is_special=*/1);

    ck_eq(rc, 0u,
          "create_soldier/special: is_special=1 -> bound 91; slots 1..90 full -> returns 0 without "
          "reaching the free slots 91..99");
    ck_eq((uint32_t)f.u(player, 91).unit_proto_id, 0u,
          "create_soldier/special: slot 91 was never inspected -- the reduction stops the search at 91");
    ck_eq((uint32_t)f.u(player, 0).order, 0x23u,
          "create_soldier/special: header increment never ran (no placement)");
    ck_eq((uint32_t)f.t(tx, ty).class_owner, 0u, "create_soldier/special: tile never claimed");
}

// ---- (CS-6) UNPLACEABLE target tile (class_owner!=0) -> the outward-scan branch
// (0x00463b69-0x00463bdf). With the whole map non-passable (fixture default: passable all 0), the
// scan finds no placeable tile along the row and returns 0 without recursing/placing. A mutation
// that treated a non-passable tile as placeable would recurse into placement and crash. ----------
void test_create_soldier_unplaceable_tile_scan_exhausts() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player = 6;
    const uint16_t proto  = 4; // soldier_count 0 -> guard skipped
    const uint32_t tx = 40, ty = 22;

    f.t(tx, ty).class_owner = 0x81; // initial tile occupied -> not directly placeable -> scan branch
    // passable is all 0 (default) -> no tile in the scanned row is placeable -> scan exhausts.
    f.u(player, 0).order = 0x24;

    const uint32_t rc = detail::create_soldier(v, own, tx, ty, proto, player, /*is_special=*/0);

    ck_eq(rc, 0u,
          "create_soldier/scan: occupied tile + no passable tile in the row -> scan exhausts, returns 0");
    ck_eq((uint32_t)f.t(tx, ty).class_owner, 0x81u,
          "create_soldier/scan: the occupied tile is left as-is (no write on the exhausted scan)");
    ck_eq((uint32_t)f.u(player, 0).order, 0x24u,
          "create_soldier/scan: header increment never ran (never recursed into a placement)");
}

} // namespace

void run_unit_creation_tests() {
    // llm_strat_unit_spawn_on_tile -- fully covered (mockable calls struct)
    test_spawn_occupied_tile_early_out();
    test_spawn_places_slot_one_full_record_init_and_callees();
    test_spawn_slot_search_bound_is_0x5b_not_units_per_player();
    test_spawn_skips_occupied_takes_first_free();
    test_spawn_header_increment_is_word_wide_and_hits_slot_zero();

    // llm_unit_create_soldier -- guard/exhaustion paths only (record-init core is BLOCKED offline)
    test_create_soldier_popcap_guard_rejects_at_boundary_100();
    test_create_soldier_popcap_guard_zero_extends_owner_unit();
    test_create_soldier_slot_search_exhausts_full_bound();
    test_create_soldier_is_special_flag_reduces_slot_bound_to_91();
    test_create_soldier_unplaceable_tile_scan_exhausts();
}

} // namespace mh::sim::test
