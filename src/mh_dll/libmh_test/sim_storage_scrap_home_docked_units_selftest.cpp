//
// sim_storage_scrap_home_docked_units_selftest.cpp -- `simtest` oracle for
// llm_strat_storage_scrap_home_docked_units @0x0048f89e (sim/sim_storage_scrap.h/.cpp, RI-SIM /
// SIM1-G3).
//
// VACUOUS under a per-call shadow arm: this function writes NOTHING tracked directly
// (`sim_migration.json`'s own `writes_shared: []` -- every write happens inside the two callees), so
// a shadow site would compare nothing even under a clean run. Same pattern as
// sim_bldg_scrap_stored_units_selftest.cpp's own header note -- every assertion here is on the
// RECORDED CALL SEQUENCE, not on any state comparison. Own recording calls struct, same shape as that
// file's `call_log`/`recording_calls()`.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_scrap_home_docked_units_0048f89e.asm
// (not the .cpp):
//   home_slot = units[player][unit_index].home_storage_slot (byte @+0x30), read ONCE before the loop
//   (0x0048f8bb-0x0048f8d5).
//   for (i = 0; i < storage[player][home_slot].docked_count [RE-READ fresh every pass]; ++i):
//     docked_unit = storage[player][home_slot].docked_units[i] (0x0048f922, full 32-bit read).
//     storage_remove_docked_unit(player, docked_unit, home_slot) -- FULL-WIDTH docked_unit
//       (0x0048f92e-0x0048f935, plain 32-bit MOV into EDX, no MOVZX/MOVSX).
//     unit_teardown(player, docked_unit) -- docked_unit TRUNCATED to 16 bits (0x0048f93a-0x0048f942,
//       MOVZX word), matching the committed teardown prototype's own uint16_t parameter.
//   No early return, no state written by this function itself.
//
#include "sim/sim_storage_scrap.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player < MAX_PLAYERS (8), distinct from the unit_index/home_slot values below so a swapped-operand
// translation lands on the wrong record.
constexpr uint16_t PLAYER     = 5;
constexpr int32_t  UNIT_INDEX = 3;
constexpr int32_t  HOME_SLOT  = 7; // distinct from UNIT_INDEX and from the decoy slot below

struct call_log {
    enum kind_t { REMOVE,
                  TEARDOWN };
    struct ev {
        kind_t   kind;
        uint16_t player;
        int64_t  unit_index;   // widened so it can hold either the full-width or truncated argument
        int32_t  storage_slot; // only meaningful for REMOVE
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
call_log g_log;

const storage_scrap_home_calls &recording_calls() {
    static const storage_scrap_home_calls c = {
        // storage_remove_docked_unit(uint16_t player, int32_t unit_index, int32_t storage_slot) --
        // FULL WIDTH unit_index.
        [](uint16_t player, int32_t unit_index, int32_t storage_slot) -> void {
            g_log.events.push_back(call_log::ev{call_log::REMOVE, player, unit_index, storage_slot});
        },
        // unit_teardown(uint32_t player, uint16_t unit_index) -- the committed prototype already
        // narrows the parameter to uint16_t, so whatever arrives here IS the truncated value.
        [](uint32_t player, uint16_t unit_index) -> void {
            g_log.events.push_back(call_log::ev{call_log::TEARDOWN, (uint16_t)player, unit_index, 0});
        },
    };
    return c;
}

void ck_ev(int32_t n, call_log::kind_t kind, uint16_t player, int64_t unit_index, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    const call_log::ev &e = g_log.events[(size_t)n];
    ck(e.kind == kind && e.player == player && e.unit_index == unit_index, what);
}

void put_storage(sim_fixture &f, uint16_t player, int32_t slot, int32_t docked_count,
                 std::initializer_list<int32_t> units) {
    unit_storage &s = f.storage[(size_t)player * (size_t)STORAGE_PER_PLAYER + (size_t)slot];
    s.docked_count  = docked_count;
    int32_t i       = 0;
    for (int32_t u : units) s.docked_units[i++] = u;
}

} // namespace

// ---- S1: three docked units at the unit's home_storage_slot -> three remove+teardown pairs, IN
// ORDER, each pair's teardown right after its remove (not batched).
void test_home_three_docked_units_remove_then_teardown_in_order() {
    sim_fixture fx;
    fx.u(PLAYER, UNIT_INDEX).home_storage_slot = (uint8_t)HOME_SLOT;
    put_storage(fx, PLAYER, HOME_SLOT, /*docked_count=*/3, {11, 22, 33});

    const sim_view v = fx.view();
    g_log.reset();
    detail::storage_scrap_home_docked_units(v, recording_calls(), PLAYER, UNIT_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 6u,
          "S1: docked_count=3 -> 3 remove + 3 teardown = 6 recorded calls");
    ck_ev(0, call_log::REMOVE, PLAYER, 11, "S1[0]: remove(player, 11, home_slot)");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 11, "S1[1]: teardown(player, 11) right after its remove");
    ck_ev(2, call_log::REMOVE, PLAYER, 22, "S1[2]: remove(player, 22, home_slot)");
    ck_ev(3, call_log::TEARDOWN, PLAYER, 22, "S1[3]: teardown(player, 22)");
    ck_ev(4, call_log::REMOVE, PLAYER, 33, "S1[4]: remove(player, 33, home_slot)");
    ck_ev(5, call_log::TEARDOWN, PLAYER, 33, "S1[5]: teardown(player, 33)");
    ck_eq((uint32_t)g_log.events[0].storage_slot, (uint32_t)HOME_SLOT,
          "S1: remove's third argument is home_storage_slot, not unit_index or a decoy");
}

// ---- S2: docked_count == 0 -> the loop condition fails on its first test -> zero calls at all.
void test_home_zero_docked_count_no_calls() {
    sim_fixture fx;
    fx.u(PLAYER, UNIT_INDEX).home_storage_slot = (uint8_t)HOME_SLOT;
    put_storage(fx, PLAYER, HOME_SLOT, /*docked_count=*/0, {});
    unit_storage &s   = fx.storage[(size_t)PLAYER * (size_t)STORAGE_PER_PLAYER + (size_t)HOME_SLOT];
    s.docked_units[0] = 0x7fffffff; // poison -- a bound-check bug that walked anyway would be caught

    const sim_view v = fx.view();
    g_log.reset();
    detail::storage_scrap_home_docked_units(v, recording_calls(), PLAYER, UNIT_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 0u, "S2: docked_count=0 -> no remove/teardown calls at all");
}

// ---- S3: the 16-bit TRUNCATION finding. docked_units[0] = 0x10005 (> 16 bits). remove must see the
// value FULL WIDTH; teardown must see it TRUNCATED to its low 16 bits.
void test_home_teardown_truncates_to_16_bits_remove_stays_full_width() {
    sim_fixture fx;
    fx.u(PLAYER, UNIT_INDEX).home_storage_slot = (uint8_t)HOME_SLOT;
    put_storage(fx, PLAYER, HOME_SLOT, /*docked_count=*/1, {0x10005});

    const sim_view v = fx.view();
    g_log.reset();
    detail::storage_scrap_home_docked_units(v, recording_calls(), PLAYER, UNIT_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u, "S3: one docked unit -> one remove + one teardown");
    ck_ev(0, call_log::REMOVE, PLAYER, 0x10005, "S3[0]: remove gets the FULL 32-bit value 0x10005");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 0x0005,
          "S3[1] FINDING: teardown gets only the low 16 bits (0x10005 & 0xffff = 0x0005)");
}

// ---- S4: home_storage_slot INDIRECTION. units[player][unit_index].home_storage_slot (7) is
// DIFFERENT from unit_index (3) itself -- a decoy record is planted at storage[player][unit_index]
// with different count/values; if the translation used unit_index instead of home_storage_slot it
// would fire on the decoy's 2 units instead of the real slot's 1.
void test_home_slot_indirection_not_unit_index_home() {
    sim_fixture fx;
    fx.u(PLAYER, UNIT_INDEX).home_storage_slot = (uint8_t)HOME_SLOT;
    put_storage(fx, PLAYER, HOME_SLOT, /*docked_count=*/1, {77});
    // Decoy: storage[player][unit_index] (slot number == unit_index, NOT home_slot).
    put_storage(fx, PLAYER, UNIT_INDEX, /*docked_count=*/2, {1, 2});

    const sim_view v = fx.view();
    g_log.reset();
    detail::storage_scrap_home_docked_units(v, recording_calls(), PLAYER, UNIT_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u,
          "S4: only home_storage_slot's ONE docked unit is walked (1 remove + 1 teardown), not the "
          "decoy unit_index-numbered slot's two");
    ck_ev(0, call_log::REMOVE, PLAYER, 77, "S4[0]: remove(player, 77) from the home_slot-indexed row");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 77, "S4[1]: teardown(player, 77)");
}

// ---- S5: PER-PLAYER row isolation. storage_of strides by player -- a decoy at the SAME home_slot but
// a DIFFERENT player row must not be read while PLAYER's own row is walked.
constexpr uint16_t S5_DECOY_PLAYER = 0; // distinct from PLAYER (5)
void               test_home_storage_row_is_scoped_to_the_players_own_row() {
    sim_fixture fx;
    fx.u(PLAYER, UNIT_INDEX).home_storage_slot = (uint8_t)HOME_SLOT;
    put_storage(fx, PLAYER, HOME_SLOT, /*docked_count=*/1, {55});
    put_storage(fx, S5_DECOY_PLAYER, HOME_SLOT, /*docked_count=*/3, {101, 102, 103});

    const sim_view v = fx.view();
    g_log.reset();
    detail::storage_scrap_home_docked_units(v, recording_calls(), PLAYER, UNIT_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u,
                        "S5: PLAYER's own row (1 docked unit) is walked, not player 0's decoy row (3 units)");
    ck_ev(0, call_log::REMOVE, PLAYER, 55, "S5[0]: remove(PLAYER, 55) from PLAYER's own row");
    ck_ev(1, call_log::TEARDOWN, PLAYER, 55, "S5[1]: teardown(PLAYER, 55)");
}

void run_storage_scrap_home_docked_units_tests() {
    test_home_three_docked_units_remove_then_teardown_in_order();
    test_home_zero_docked_count_no_calls();
    test_home_teardown_truncates_to_16_bits_remove_stays_full_width();
    test_home_slot_indirection_not_unit_index_home();
    test_home_storage_row_is_scoped_to_the_players_own_row();
}

} // namespace mh::sim::test
