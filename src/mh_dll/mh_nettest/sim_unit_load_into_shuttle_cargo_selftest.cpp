#include <cstring>
#include <vector>

#include "sim/sim_unit_load_into_shuttle_cargo.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER       = 3;
constexpr int32_t  BUILDING_IDX = 2;
constexpr uint16_t UNIT_IDX     = 6;

// The real boot value (see the header banner's "WHAT THIS DOES NOT TOUCH" section: "an ordinary FADD
// against sim_view::shuttle_board_activity_bump, a bound constant -- not a literal"; the .cpp comment
// at sim_view's declaration site gives 20.0). sim_fixture does NOT bind this field in view() (grepped
// sim_test_support.h -- absent, unlike every field the fixture DOES own), so every test below that
// reaches the success path (i.e. every test except the two no-op gate tests) must bind it itself on
// its own local `sim_view` before calling into the function, or the unconditional activity_clock bump
// dereferences a null pointer.
constexpr double SHUTTLE_BOARD_ACTIVITY_BUMP = 20.0;

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// ---- manifest entry accessors, re-derived locally (the .cpp's own accessors are in an anonymous
// namespace and not reachable from here) -- same 14-byte stride / field-offset layout the header
// banner derives: header@0x0 (uint16_t), energy@0x2 (double), experience@0xa (int32_t). ----------
void set_manifest_header(prod_shuttle_slot &slot, int32_t entry, uint16_t value) {
    std::memcpy(slot.cargo_manifest_raw + entry * CARGO_ENTRY_STRIDE, &value, sizeof(value));
}
uint16_t get_manifest_header(prod_shuttle_slot &slot, int32_t entry) {
    uint16_t v;
    std::memcpy(&v, slot.cargo_manifest_raw + entry * CARGO_ENTRY_STRIDE, sizeof(v));
    return v;
}
double get_manifest_energy(prod_shuttle_slot &slot, int32_t entry) {
    double v;
    std::memcpy(&v, slot.cargo_manifest_raw + entry * CARGO_ENTRY_STRIDE + 0x2, sizeof(v));
    return v;
}
int32_t get_manifest_experience(prod_shuttle_slot &slot, int32_t entry) {
    int32_t v;
    std::memcpy(&v, slot.cargo_manifest_raw + entry * CARGO_ENTRY_STRIDE + 0xa, sizeof(v));
    return v;
}

// ---- recorder ---------------------------------------------------------------------------------
struct storage_remove_call {
    uint16_t player;
    int32_t  unit_index;
    int32_t  storage_slot;
};
struct teardown_call {
    uint32_t player;
    uint16_t unit_index;
};

struct rec_t {
    int32_t                          is_boarding_n          = 0;
    int32_t                          is_boarding_last_state = -1;
    int32_t                          is_boarding_ret        = 1; // control knob
    std::vector<storage_remove_call> storage_remove;
    std::vector<teardown_call>       teardown;
    void                             reset() { *this = rec_t{}; }
};
rec_t g_rec;

const unit_load_into_shuttle_cargo_calls &rec_calls() {
    static const unit_load_into_shuttle_cargo_calls c = {
        [](int32_t state) -> int32_t {
            g_rec.is_boarding_n++;
            g_rec.is_boarding_last_state = state;
            return g_rec.is_boarding_ret;
        },
        [](uint16_t player, int32_t unit_index, int32_t storage_slot) {
            g_rec.storage_remove.push_back({player, unit_index, storage_slot});
        },
        [](uint32_t player, uint16_t unit_index) {
            g_rec.teardown.push_back({player, unit_index});
        },
    };
    return c;
}

// ==== the descending free-slot scan (header banner: "THE FREE-SLOT SCAN") ==========================
// `for (i = 49; i >= 0; --i) if (manifest[i].header == 0) candidate = i;` -- no break, so the LAST
// (i.e. lowest-index) empty slot encountered wins, not the first one the descending scan hits.

void test_descending_scan_lowest_free_slot_wins() {
    sim_fixture f;
    g_rec.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, 4);
    // Occupy every entry with a distinct nonzero sentinel, then carve out THREE free slots at
    // non-adjacent indices -- the scan visits 44 and 27 (both free) BEFORE it visits 11 (also free,
    // and the LOWEST), so if the loop wrongly broke on first-hit it would report 44; if it merely
    // preferred the LAST index it scanned regardless of direction it would report 0. Only "lowest
    // free index, found via an exhaustive descending scan" produces 11.
    for (int32_t i = 0; i < CARGO_MANIFEST_SLOTS; ++i) set_manifest_header(s, i, (uint16_t)(0x9000 + i));
    set_manifest_header(s, 44, 0);
    set_manifest_header(s, 27, 0);
    set_manifest_header(s, 11, 0);

    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b; // arbitrary nonzero "boarding" state; stub returns 1
    u.unit_proto_id                        = 5;
    u.ctrl_group_id                        = 2;
    u.energy                               = 11.5;
    u.experience                           = 222;
    u.home_storage_slot                    = 1;
    f.cfg_units[5].soldier_count           = 0;
    f.cfg_units[5].human                   = 1;

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    const uint16_t expected_header = (uint16_t)(5u | (2u << 12)); // proto_id=5 | ctrl_group_id(2)<<12
    ck_eq((uint32_t)get_manifest_header(s, 11), (uint32_t)expected_header,
          "scan: the LOWEST free slot (11), not the first (44) or last-scanned (0), got the write");
    ck_eq((uint32_t)get_manifest_header(s, 27), 0u,
          "scan: a higher-index free slot (27) that the descending scan visited AFTER 11 stays free");
    ck_eq((uint32_t)get_manifest_header(s, 44), 0u,
          "scan: the free slot the descending scan visited FIRST (44) stays free -- no early exit");
    ck_eq((uint32_t)get_manifest_header(s, 0), 0x9000u, "scan: an occupied entry (0) is left untouched");
    ck_eq((uint32_t)get_manifest_header(s, 10), 0x900au,
          "scan: an occupied entry adjacent to the winner (10) is left untouched");
    ck_eq((uint32_t)get_manifest_header(s, 12), 0x900cu,
          "scan: an occupied entry adjacent to the winner (12) is left untouched");
    ck_eq((uint32_t)get_manifest_header(s, 49), 0x9031u, "scan: the scan's own start entry (49) is left untouched");
}

// ==== the two no-op gates ============================================================================

void test_no_free_slot_returns_zero_and_writes_nothing() {
    sim_fixture f;
    g_rec.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, 4);
    // Every one of the 50 entries occupied -- found_slot stays -1.
    for (int32_t i = 0; i < CARGO_MANIFEST_SLOTS; ++i) set_manifest_header(s, i, (uint16_t)(0x9000 + i));

    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b;
    u.activity_clock                       = 77.0;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        (uint32_t)detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    ck_eq(r, 0u, "no-free-slot: returns 0");
    ck_eq((uint32_t)g_rec.is_boarding_n, 0u,
          "no-free-slot: the found_slot<0 gate short-circuits BEFORE the boarding-state check");
    ck_eq((uint32_t)g_rec.storage_remove.size(), 0u, "no-free-slot: storage_remove_docked_unit NOT called");
    ck_eq((uint32_t)g_rec.teardown.size(), 0u, "no-free-slot: unit_teardown NOT called");
    ck_eq_d(u.activity_clock, 77.0, "no-free-slot: activity_clock untouched");
}

void test_not_boarding_returns_zero_and_writes_nothing() {
    sim_fixture f;
    g_rec.reset();
    // Default fixture: manifest is all-zero (memset), so the scan finds a free slot (entry 0, the
    // last one the descending scan overwrites when every entry is free) -- this exercises the SECOND
    // gate specifically, with the first gate already open.
    g_rec.is_boarding_ret                  = 0; // NOT boarding
    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x77; // distinct sentinel, checked below
    u.activity_clock                       = 55.0;

    sim_view       v   = f.view();
    sim_store      own = f.store();
    const uint32_t r =
        (uint32_t)detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    ck_eq(r, 0u, "not-boarding: returns 0");
    ck_eq((uint32_t)g_rec.is_boarding_n, 1u, "not-boarding: the boarding-state check WAS reached and called once");
    ck_eq((uint32_t)g_rec.is_boarding_last_state, 0x77u, "not-boarding: called with the unit's own state");
    ck_eq((uint32_t)g_rec.storage_remove.size(), 0u, "not-boarding: storage_remove_docked_unit NOT called");
    ck_eq((uint32_t)g_rec.teardown.size(), 0u, "not-boarding: unit_teardown NOT called");
    ck_eq((uint32_t)get_manifest_header(slot_of(f, PLAYER, 4), 0), 0u,
          "not-boarding: the manifest entry the scan found is left free -- no write happens after the gate fails");
    ck_eq_d(u.activity_clock, 55.0, "not-boarding: activity_clock untouched");
}

// ==== the success path: manifest write, callees, activity_clock bump ================================

void test_success_writes_manifest_entry_and_calls_removal_chain() {
    sim_fixture f;
    g_rec.reset();
    prod_shuttle_slot &s = slot_of(f, PLAYER, 4);
    // Default fixture manifest is all-zero -> found_slot lands on entry 0 (see the comment in
    // test_not_boarding above).

    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b; // arbitrary nonzero -- stub returns "boarding"
    u.unit_proto_id                        = 53;   // < 100, the fixture's cfg_units extent
    u.ctrl_group_id                        = 6;
    u.energy                               = 246.75;
    u.experience                           = 13579;
    u.home_storage_slot                    = 42;
    f.cfg_units[53].soldier_count          = 0; // <1 -> result = human
    f.cfg_units[53].human                  = 77;

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    const uint32_t r =
        (uint32_t)detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    const uint16_t expected_header = (uint16_t)(53u | (6u << 12)); // proto_id(53) | ctrl_group_id(6)<<12
    ck_eq((uint32_t)get_manifest_header(s, 0), (uint32_t)expected_header,
          "success: manifest header = proto_id | (ctrl_group_id << 12)");
    ck_eq_d(get_manifest_energy(s, 0), 246.75, "success: manifest energy = units[].energy, plain copy");
    ck_eq((uint32_t)get_manifest_experience(s, 0), 13579u,
          "success: manifest experience = units[].experience, plain copy");

    ck_eq((uint32_t)g_rec.is_boarding_n, 1u, "success: boarding-state checked exactly once");
    ck_eq((uint32_t)g_rec.is_boarding_last_state, 0x2bu, "success: boarding check called with the unit's state");

    ck(g_rec.storage_remove.size() == 1 && g_rec.storage_remove[0].player == PLAYER &&
           g_rec.storage_remove[0].unit_index == (int32_t)UNIT_IDX &&
           g_rec.storage_remove[0].storage_slot == 42,
       "success: storage_remove_docked_unit(player, unit_idx, home_storage_slot=42) called once");
    ck(g_rec.teardown.size() == 1 && g_rec.teardown[0].player == PLAYER &&
           g_rec.teardown[0].unit_index == UNIT_IDX,
       "success: unit_teardown(player, unit_idx) called once");

    ck_eq(r, 77u, "success: soldier_count(0) < 1 -> returns cfg_units[proto].human (77)");
}

void test_activity_clock_bumped_by_bound_constant() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b;
    u.activity_clock                       = 100.25; // distinct, non-zero baseline

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    // The .cpp RE-FETCHES the unit record via own.unit_at() after both callees run (rather than
    // reusing the earlier `u` read-view reference) -- observing the change through the SAME `u`
    // reference proves that re-fetch landed on the same physical record, since the stub callees here
    // do not themselves touch the roster.
    ck_eq_d(u.activity_clock, 100.25 + SHUTTLE_BOARD_ACTIVITY_BUMP,
            "success: activity_clock += shuttle_board_activity_bump(20.0), re-fetched after both callees");
}

// ==== the two-way return value (header banner: "THE `human` RESULT") ================================
// result = (Unit[proto_id].soldier_count < 1) ? Unit[proto_id].human : 0

void test_return_is_human_when_soldier_count_below_one() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b;
    u.unit_proto_id                        = 9;
    f.cfg_units[9].soldier_count           = -3; // < 1 (a negative count, not just 0)
    f.cfg_units[9].human                   = 42;

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    const uint32_t r =
        (uint32_t)detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    ck_eq(r, 42u, "return: soldier_count(-3) < 1 -> returns human(42) verbatim, not normalized to 1");
}

void test_return_is_zero_when_soldier_count_at_least_one() {
    sim_fixture f;
    g_rec.reset();
    f.b(PLAYER, BUILDING_IDX).shuttle_slot = 4;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b;
    u.unit_proto_id                        = 9;
    f.cfg_units[9].soldier_count           = 1;  // boundary: NOT < 1
    f.cfg_units[9].human                   = 99; // must be ignored on this path

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    const uint32_t r =
        (uint32_t)detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    ck_eq(r, 0u, "return: soldier_count(1), the boundary case, is NOT < 1 -> returns 0, human ignored");
}

// ==== the preserve_bug: entry 49's experience write spills 4 bytes past cargo_manifest_raw ==========
//
// The header banner's "OUT-OF-BOUNDS HAZARD" section: entry 49 sits at byte offsets [686,700) within
// cargo_manifest_raw (header@[686,688), energy@[688,696) reaching the array's declared 696-byte
// extent EXACTLY, experience@[696,700) -- FOUR BYTES PAST the array. The general-case target of that
// overrun is "the first 4 bytes of the NEXT array element's _pad_0x00[20]" (confirmed against
// addr/mh_structs.gen.h: cargo_manifest_raw is mh_llm_prod_shuttle_slot's LAST field, and the very
// next thing in that struct's own layout -- and hence in the array -- is `_pad_0x00[20]`, the FIRST
// field of the next mh_llm_prod_shuttle_slot), EXCEPT for the single true corner player==7 &&
// shuttle_slot==9 (the last of the 80-element _G_LLM_PROD_SHUTTLE_SLOTS[8][10] array), which instead
// spills past the region entirely.
//
// SAFETY DECISION (per this task's brief): sim_fixture::prod_shuttle_slots is a REAL-EXTENT
// std::vector<prod_shuttle_slot> sized exactly MAX_PLAYERS*PROD_SHUTTLE_SLOTS_PER_PLAYER (80) -- the
// "real extents, so an out-of-range index lands in the fixture's own memory and can be OBSERVED
// rather than corrupting the heap" convention sim_test_support.h documents for every other array
// member. That convention gives safe slack for EVERY corner except the true LAST element of the
// vector: exercising player==7 && shuttle_slot==9 there would write 4 bytes immediately past the
// vector's own heap allocation -- a genuine, ASan-visible heap-buffer-overflow, not a "lands in
// fixture memory" case, because there IS no fixture memory after the last element. There is also no
// other sim_fixture member reliably placed immediately after prod_shuttle_slots in the object's
// layout to treat as an intentional landing zone (relying on incidental C++ member ordering for a
// safety property would be fragile and unrelated to what the ORIGINAL binary's memory layout does
// there anyway). So: this test exercises the GENERAL case (a mid-array shuttle_slot, landing in the
// next element's own _pad_0x00, still fully inside the vector's real allocation) and deliberately does
// NOT construct the player==7/shuttle_slot==9 corner. That corner is the "DECLARED NEED" the header
// banner already flags for the conductor (pad the fixture's buffer, or accept it as untestable
// in-process) -- this file does not resolve it, consistent with RULE 6 of this task's brief.
void test_experience_overrun_spills_into_next_slot_pad() {
    sim_fixture f;
    g_rec.reset();
    // NOTE: the function looks up the manifest slot as prod_shuttle_slot_at(PLAYER, shuttle_slot) --
    // the SAME `player` argument passed to the call, read back out of buildings[PLAYER][BUILDING_IDX]
    // .shuttle_slot -- so the slot under test must be addressed via PLAYER (3), not an independent
    // player id, or this test would corrupt/inspect the wrong prod_shuttle_slots row entirely.
    constexpr uint32_t OOB_SLOT = 3; // NOT slot 9 -- next element (slot 4) is still mid-array; PLAYER
                                     // (3) is NOT the last player row (7) either -- see the safety
                                     // note above for why both matter.

    prod_shuttle_slot &s      = slot_of(f, PLAYER, OOB_SLOT);
    prod_shuttle_slot &next_s = slot_of(f, PLAYER, OOB_SLOT + 1);

    // Force found_slot == 49: every other entry occupied, entry 49 free.
    for (int32_t i = 0; i < CARGO_MANIFEST_SLOTS - 1; ++i) set_manifest_header(s, i, (uint16_t)(0x9000 + i));
    set_manifest_header(s, CARGO_MANIFEST_SLOTS - 1, 0);

    // Poison the next slot's leading _pad_0x00[20] with a recognizable non-zero byte so the write can
    // be told apart from the fixture's own zero-initialized default, and so any spill PAST 4 bytes
    // would also be visible.
    std::memset(next_s._pad_0x00, 0xAA, sizeof(next_s._pad_0x00));

    f.b(PLAYER, BUILDING_IDX).shuttle_slot = (uint8_t)OOB_SLOT;
    unit &u                                = f.u(PLAYER, UNIT_IDX);
    u.state                                = 0x2b;
    u.unit_proto_id                        = 12;
    u.ctrl_group_id                        = 3;
    u.energy                               = 9.5;
    u.experience                           = (int32_t)0x11223344; // recognizable, byte-distinguishable pattern
    f.cfg_units[12].soldier_count          = 0;
    f.cfg_units[12].human                  = 1;

    sim_view  v                   = f.view();
    sim_store own                 = f.store();
    double    bump                = SHUTTLE_BOARD_ACTIVITY_BUMP;
    v.shuttle_board_activity_bump = &bump;

    detail::unit_load_into_shuttle_cargo(v, own, rec_calls(), PLAYER, BUILDING_IDX, UNIT_IDX);

    // In-bounds part of entry 49 still writes correctly, right up to the boundary.
    const uint16_t expected_header = (uint16_t)(12u | (3u << 12));
    ck_eq((uint32_t)get_manifest_header(s, 49), (uint32_t)expected_header,
          "oob: entry 49's header (still fully in-bounds) writes normally");
    ck_eq_d(get_manifest_energy(s, 49), 9.5, "oob: entry 49's energy (still fully in-bounds, reaching "
                                             "the 696-byte boundary exactly) writes normally");

    // The 4-byte experience write spilled past cargo_manifest_raw[696] and landed in the FIRST 4 bytes
    // of the NEXT slot's _pad_0x00 -- exactly what the header banner's derivation predicts.
    int32_t spilled;
    std::memcpy(&spilled, next_s._pad_0x00, sizeof(spilled));
    ck_eq((uint32_t)spilled, 0x11223344u,
          "oob (preserve_bug): entry 49's experience write spills 4 bytes past cargo_manifest_raw's "
          "declared 696-byte extent and lands in the NEXT prod_shuttle_slot's leading _pad_0x00[0..4) "
          "-- reproducing the original's boundary overrun byte-for-byte, not a bounds-safe rewrite");

    // Nothing beyond those 4 bytes was touched -- the overrun is exactly 4 bytes, not a larger stomp.
    bool rest_untouched = true;
    for (size_t i = 4; i < sizeof(next_s._pad_0x00); ++i)
        if (next_s._pad_0x00[i] != (uint8_t)0xAA) rest_untouched = false;
    ck(rest_untouched, "oob: bytes [4..20) of the next slot's _pad_0x00 are untouched -- the overrun is "
                       "exactly the 4-byte experience write, nothing more");
}

} // namespace

void run_unit_load_into_shuttle_cargo_tests() {
    test_descending_scan_lowest_free_slot_wins();
    test_no_free_slot_returns_zero_and_writes_nothing();
    test_not_boarding_returns_zero_and_writes_nothing();
    test_success_writes_manifest_entry_and_calls_removal_chain();
    test_activity_clock_bumped_by_bound_constant();
    test_return_is_human_when_soldier_count_below_one();
    test_return_is_zero_when_soldier_count_at_least_one();
    test_experience_overrun_spills_into_next_slot_pad();
}

} // namespace mh::sim::test
