//
// ai_group_building_scan_selftest.cpp -- `aitest` cases for
// llm_strat_ai_group_collect_buildings_of_types @0x004e99be (RI-AI batch C / AI1C, closed at T3).
//
// WHY THIS FILE EXISTS. The row was closed on reimpl-verify plus a shadow site (shadow_ai_c9.ini)
// that ARMED and was reached ZERO times: its sole caller group_task_attack_random_target
// (task_code 0x18) is the rarest of the four attack-dispatch arms and never fired in a 65000-step
// combined all-AI soak. AI1's done_when requires a T3 to carry its written reason PLUS a
// compensating test; this is that test, written at AI1 close (2026-08-30). The scenario gap is
// unchanged -- a zero-call site is still not evidence and is not counted as any.
//
// WHAT IT PINS, all four properties of the COUNT-DRIVEN roster walk that a plain `for
// (i = 1; i <= count; ++i)` translation would get wrong, plus the append protocol:
//   * `remaining` seeds from buildings[player][0].index and the walk runs while remaining != 0 --
//     the roster INDEX is not the bound, so the walk can and does run past the live count's worth
//     of slots (the header's zero-extension is deliberately NOT pinned here: this loop tests
//     `!= 0`, which a signed read reaches identically, so a case asserting it would be theatre);
//   * an EMPTY slot (building_id == 0) advances the index WITHOUT consuming the budget (the branch
//     lands past the DEC at LAB_004e99a8), so a hole does not truncate the scan;
//   * a live but NOT-ALIVE or NON-MATCHING slot DOES consume it;
//   * all four requested type slots are compared, and
//   * the append is RESET-THEN-FILL: this function only ever appends at the inherited count and
//     increments -- the caller is what zeroes it -- with no capacity check against the 256 extent.
//
#include "ai_test_support.h"

#include "ai/ai_group_building_scan.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

// bldg_is_alive is the one outward call this body makes. The stub answers from a per-index table so
// a test can make a slot live-but-dead, and records its calls so the BUDGET rules above are
// observable rather than inferred from the result alone.
struct recorder {
    std::vector<int32_t> alive_calls;
    bool                 dead[64] = {false};

    void clear() {
        alive_calls.clear();
        for (bool &d : dead) d = false;
    }
};
recorder g_rec;

int32_t st_bldg_is_alive(int32_t player, int32_t building_index) {
    (void)player;
    g_rec.alive_calls.push_back(building_index);
    return (building_index >= 0 && building_index < 64 && g_rec.dead[building_index]) ? 0 : 1;
}

// Only the member this body reaches is bound; everything else stays null so an unstubbed call
// crashes loudly rather than returning zero (house style).
const ai_calls &calls() {
    static const ai_calls c = [] {
        ai_calls t{};
        t.bldg_is_alive = &st_bldg_is_alive;
        return t;
    }();
    return c;
}

// Seed the roster header (slot 0's `index` IS the live-building count) and one building.
void seed_count(fixture &f, int p, int live) { f.b(p, 0).index = (int16_t)live; }
void seed_bldg(fixture &f, int p, int slot, int type) {
    f.b(p, slot).building_id = (uint16_t)type;
}

// The collect call, with the two ignored register slots given POISON values -- a translation that
// read either of them instead of a real argument produces a wrong answer rather than a lucky one.
void collect(fixture &f, int32_t p, uint32_t t1, uint32_t t2, uint32_t t3, uint32_t t4) {
    detail::group_collect_buildings_of_types(f.view(), f.store(), calls(), p, 0xdeadbeefu,
                                             0xfeedfaceu, t1, t2, t3, t4);
}

} // namespace

void run_group_building_scan_tests() {
    printf("-- ai_group_building_scan (llm_strat_ai_group_collect_buildings_of_types) --\n");

    const int32_t P = 5; // not 0/1: separates a player-row bug from a roster-index bug

    // ---- C1: the plain case -- every alive building whose id matches one of the four requested
    // types is appended, in ROSTER ORDER, and the count tracks the appends. --------------------
    {
        fixture f;
        g_rec.clear();
        seed_count(f, P, 3);
        seed_bldg(f, P, 1, 11); // match
        seed_bldg(f, P, 2, 99); // live, no match -- consumes budget
        seed_bldg(f, P, 3, 12); // match

        collect(f, P, 11, 12, 13, 14);

        ck(f.bldg_cand_count == 2, "C1: two matches appended");
        ck(f.bldg_cands[0] == 1 && f.bldg_cands[1] == 3,
           "C1: ROSTER INDICES are appended, in roster order");
        ck(g_rec.alive_calls.size() == 3,
           "C1: liveness is asked once per occupied slot reached, 0x004e9931");
    }

    // ---- C2: an EMPTY slot does not consume the budget. With one hole among three live
    // buildings the walk must reach roster index 4, not stop at 3. -----------------------------
    {
        fixture f;
        g_rec.clear();
        seed_count(f, P, 3);
        seed_bldg(f, P, 1, 11);
        // slot 2 left empty (building_id == 0)
        seed_bldg(f, P, 3, 11);
        seed_bldg(f, P, 4, 11);

        collect(f, P, 11, 11, 11, 11);

        ck(f.bldg_cand_count == 3, "C2: the hole does not truncate the scan -- all three found");
        ck(f.bldg_cands[0] == 1 && f.bldg_cands[1] == 3 && f.bldg_cands[2] == 4,
           "C2: the empty slot is skipped WITHOUT consuming `remaining` (branch lands past the DEC)");
        ck(g_rec.alive_calls.size() == 3,
           "C2: an empty slot is not even asked about -- the id test comes first, 0x004e991f");
    }

    // ---- C3: a NOT-ALIVE building consumes the budget and is skipped. ------------------------
    {
        fixture f;
        g_rec.clear();
        seed_count(f, P, 2);
        seed_bldg(f, P, 1, 11);
        g_rec.dead[1] = true; // occupied, matching type, but not alive
        seed_bldg(f, P, 2, 11);
        seed_bldg(f, P, 3, 11); // beyond the budget once slots 1 and 2 have consumed it

        collect(f, P, 11, 11, 11, 11);

        ck(f.bldg_cand_count == 1 && f.bldg_cands[0] == 2,
           "C3: the dead slot is skipped but DOES consume the budget, so slot 3 is never reached");
        ck(g_rec.alive_calls.size() == 2, "C3: two occupied slots reached");
    }

    // ---- C4: all FOUR type slots are compared -- a translation that dropped the fourth
    // comparison finds nothing here. ------------------------------------------------------------
    {
        fixture f;
        g_rec.clear();
        seed_count(f, P, 4);
        seed_bldg(f, P, 1, 21);
        seed_bldg(f, P, 2, 22);
        seed_bldg(f, P, 3, 23);
        seed_bldg(f, P, 4, 24);

        collect(f, P, 21, 22, 23, 24);

        ck(f.bldg_cand_count == 4, "C4: each of the four requested types matches its own building");
        ck(f.bldg_cands[3] == 4, "C4: the FOURTH type slot is compared too, 0x004e9a3c");
    }

    // ---- C5: RESET-THEN-FILL -- the append starts at the INHERITED count. The caller
    // (group_task_attack_random_target) is what zeroes it before its four collect calls; this
    // function never does, and two collects in a row must accumulate. --------------------------
    {
        fixture f;
        g_rec.clear();
        f.bldg_cands[0]   = 77; // an inherited entry the function must not disturb
        f.bldg_cands[1]   = 88;
        f.bldg_cand_count = 2;
        seed_count(f, P, 1);
        seed_bldg(f, P, 1, 11);

        collect(f, P, 11, 11, 11, 11);

        ck(f.bldg_cand_count == 3, "C5: appends at the inherited count, 0x004e9a54-0x004e9a62");
        ck(f.bldg_cands[0] == 77 && f.bldg_cands[1] == 88 && f.bldg_cands[2] == 1,
           "C5: the inherited entries are left alone -- no reset of its own");
    }

    // ---- C6: a zero live count is a NO-OP -- the walk never starts, and nothing is appended.
    // This is the "nothing" case the done_when checklist asks for. ------------------------------
    {
        fixture f;
        g_rec.clear();
        seed_count(f, P, 0);
        seed_bldg(f, P, 1, 11); // present in the array but outside the live budget

        collect(f, P, 11, 11, 11, 11);

        ck(f.bldg_cand_count == 0 && g_rec.alive_calls.empty(),
           "C6: remaining == 0 at entry -- no walk, no call, no append");
    }
}

} // namespace mh::ai::test
