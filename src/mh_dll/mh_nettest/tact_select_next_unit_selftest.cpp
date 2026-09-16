//
// tact_select_next_unit_selftest.cpp -- offline oracle for llm_tact_select_next_unit. See
// tact/tact_select_next_unit.h for the derivation (gate, LOOP A/B/C, the STAMP, and the
// PRESERVE-BUG note on stamping `target` unconditionally even when loop B never found a real
// candidate).
//
#include "tact/tact_select_next_unit.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    int panel_refresh_calls = 0;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log{}; }

void mock_panel_refresh() { ++log().panel_refresh_calls; }

select_next_unit_calls mock_calls() { return {mock_panel_refresh}; }

} // namespace

void run_select_next_unit_tests() {
    // T1: GATE closed (active_unit_count == active_unit_count_cached) -> no-op entirely, no call,
    // no status bits touched anywhere in the roster. @0x0042edde-0x0042ede9.
    {
        tact_fixture fx;
        fx.active_unit_count        = 5;
        fx.active_unit_count_cached = 5;
        fx.units[3].status          = 1; // already selected -- must survive untouched
        fx.units[3].owner           = 0;
        fx.units[3].type            = 1;

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)log().panel_refresh_calls, 0u, "T1: gate closed -> no panel_refresh call, 0x0042edde");
        ck_eq((uint32_t)own.unit_at(3).status, 1u, "T1: gate closed -> roster status bits untouched");
    }

    // T2: GATE open, LOOP A finds NO match (no unit has status&1==1 && owner==0) -> candidate stays
    // at its initial value TACT_UNIT_FIRST_SLOT (1). LOOP B then starts at i=2 and, finding no
    // owner==0 unowned unit either, exhausts its budget -> target stays at its own initial value
    // (1) too -- the PRESERVE-BUG path: target is stamped selected even though nothing was found.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 1; // populated, so loop B never wraps on type==0
            fx.units[i].status = 0;
            fx.units[i].owner  = 7; // every unit OWNED -> loop B's owner==0 branch never taken
        }

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)log().panel_refresh_calls, 1u, "T2: gate open -> panel_refresh fires once, 0x0042ef0a");
        ck_eq((uint32_t)(own.unit_at(TACT_UNIT_FIRST_SLOT).status & 1), 1u,
              "T2: PRESERVE-BUG -- target stamped selected even though loop B found nothing, 0x0042eeed");
    }

    // T3: LOOP A -- multiple candidates (status&1==1 && owner==0) exist; NO EARLY BREAK means
    // `candidate` ends up the LAST match, not the first. Verified indirectly: seed candidates at
    // slots 10 and 50 (both status&1==1, owner==0), and an unowned empty target at slot 51 (just
    // after 50) plus ALSO one at slot 11 (just after 10). If `candidate` were the FIRST match (10),
    // loop B would find the slot-11 target; since it is actually the LAST match (50), loop B finds
    // the slot-51 target instead.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 1;
            fx.units[i].status = 0;
            fx.units[i].owner  = 7;
        }
        fx.units[10].status = 1;
        fx.units[10].owner  = 0; // candidate match #1
        fx.units[50].status = 1;
        fx.units[50].owner  = 0; // candidate match #2 -- LAST, so candidate should end at 50
        fx.units[11].owner  = 0; // a decoy target reachable only if candidate wrongly ends at 10
        fx.units[51].owner  = 0; // the REAL expected target, reachable only if candidate ends at 50

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)(own.unit_at(51).status & 1), 1u,
              "T3: candidate = LAST match (50), so loop B selects slot 51, not the slot-11 decoy, 0x0042ee2e");
        ck_eq((uint32_t)(own.unit_at(11).status & 1), 0u,
              "T3: slot 11 (the first-match decoy target) must stay unselected");
    }

    // T4: LOOP B wraps on an empty (type==0) slot WITHOUT consuming a budget step, then finds the
    // target after wrapping. candidate = TACT_UNIT_LAST_SLOT (128) so i starts at 129 (the OOB read,
    // see the fixture's +1 slack) with type left at 0 there -> wraps to slot 1 on the SAME budget
    // value. Slot 1 is POPULATED but owned by someone else (falls through to advance, consuming a
    // budget step and landing on i=2); slot 2 is the real target. A slot-1 type==0 would wrap
    // forever without consuming a step (matching the original's own hang on a truly empty roster),
    // so slot 1 must be non-empty for this case to terminate -- that is itself the wrap contract.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 0; // empty by default; the three slots below override this
            fx.units[i].status = 0;
            fx.units[i].owner  = 0;
        }
        fx.units[TACT_UNIT_LAST_SLOT].type   = 1;
        fx.units[TACT_UNIT_LAST_SLOT].status = 1; // status&1==1, owner==0 -> LOOP A's sole match
        fx.units[TACT_UNIT_LAST_SLOT].owner  = 0;
        // fx.units[TACT_UNIT_SLOTS] (index 129, the OOB slot) left at type==0 -- default-constructed.
        fx.units[1].type  = 1; // populated but owned -> falls through to advance, not a wrap or a match
        fx.units[1].owner = 9;
        fx.units[2].type  = 1; // the real target, reached only after wrapping past 129 and advancing past 1
        fx.units[2].owner = 0;

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)(own.unit_at(2).status & 1), 1u,
              "T4: wraps past the OOB slot 129, advances past the owned slot 1, selects slot 2, 0x0042ee4c-0x0042ee98");
        ck_eq((uint32_t)(own.unit_at(TACT_UNIT_LAST_SLOT).status & 1), 0u,
              "T4: LOOP C unconditionally clears the old candidate's own selection bit, 0x0042eea1");
    }

    // T5: LOOP B skips an already-selected unit (status&1==1) and a differently-owned unit (owner!=0)
    // before finding the real target -- both must consume a budget step via the "fall through to
    // advance" path (0x0042ee7f/0x0042ee98), not be mistaken for a match.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 1;
            fx.units[i].status = 0;
            fx.units[i].owner  = 7; // owned -> not a valid target
        }
        fx.units[5].status = 1;
        fx.units[5].owner  = 0; // LOOP A's candidate
        fx.units[6].status = 1;
        fx.units[6].owner  = 0; // already selected -- must be SKIPPED, not selected as target
        fx.units[7].status = 0;
        fx.units[7].owner  = 9; // owned by someone else -- must be SKIPPED too
        fx.units[8].status = 0;
        fx.units[8].owner  = 0; // the real target

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)(own.unit_at(8).status & 1), 1u,
              "T5: skips the already-selected slot 6 and the owned slot 7, selects slot 8, 0x0042ee6e-0x0042ee98");
    }

    // T6: LOOP C clears the selection bit on EVERY currently-selected unit, unconditionally,
    // regardless of how loop B ended -- seed TWO pre-selected units besides the LOOP A candidate,
    // and confirm all are cleared even though only one target gets the bit set back.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 1;
            fx.units[i].status = 0;
            fx.units[i].owner  = 7;
        }
        fx.units[20].status = 1;
        fx.units[20].owner  = 0; // LOOP A's candidate (also pre-selected)
        fx.units[30].status = 1;
        fx.units[30].owner  = 9; // ANOTHER pre-selected unit, unrelated to the candidate scan
        fx.units[21].status = 0;
        fx.units[21].owner  = 0; // the new target

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)(own.unit_at(20).status & 1), 0u, "T6: old candidate's selection bit cleared");
        ck_eq((uint32_t)(own.unit_at(30).status & 1), 0u,
              "T6: an UNRELATED pre-selected unit is also cleared -- loop C is unconditional over the whole roster, 0x0042eea1-0x0042eeeb");
        ck_eq((uint32_t)(own.unit_at(21).status & 1), 1u, "T6: the new target is selected");
    }

    // T7: a non-selection status bit (e.g. FIRE, 0x08) on the old candidate must survive loop C's
    // clear -- it masks only bit 0 (`status &= 0xfe`), never the whole byte.
    {
        tact_fixture fx;
        fx.active_unit_count        = 1;
        fx.active_unit_count_cached = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].type   = 1;
            fx.units[i].status = 0;
            fx.units[i].owner  = 7;
        }
        fx.units[15].status = 1 | 8; // selected AND firing
        fx.units[15].owner  = 0;
        fx.units[16].owner  = 0;

        reset_log();
        tact_store own = fx.store();
        detail::select_next_unit(own, mock_calls());

        ck_eq((uint32_t)own.unit_at(15).status, 8u,
              "T7: loop C clears only bit 0 (status &= 0xfe) -- bit 3 (FIRE) survives, 0x0042eea8-0x0042eeb0");
    }
}

} // namespace mh::tact::test
