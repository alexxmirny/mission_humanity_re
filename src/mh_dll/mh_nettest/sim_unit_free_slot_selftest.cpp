//
// sim_unit_free_slot_selftest.cpp -- `simtest` cases for llm_strat_unit_free_slot @0x00487b25
// (sim/sim_unit_free_slot.h/.cpp, SIM1-G1).
//
// SCOPE: the two direct field writes on the target slot (0x00487b55 unit_proto_id=0, 0x00487b71
// unit_above={0,0}); the two row-only header decrements at units[player][0] (0x00487b84
// unit_above-=1, 0x00487b95 order-=1) as INDEPENDENT counters, seeded to DISTINCT values from each
// other and from the target-slot's own seeds; the player==?,slot==0 edge case where the "target
// slot" record and the "header" record are the SAME object, so the 0x00487b84 header DEC reads
// units[player][0].unit_above AFTER the 0x00487b71 store already zeroed that same word (proving
// write ORDER, not merely write correctness -- a translation that computed the header decrement
// from a pre-zero snapshot would disagree here); the player value's low-16-bit truncation (all
// four `MOVZX EAX, word ptr [...]` reads at 0x00487b42/0x00487b5e/0x00487b7a/0x00487b8b); and
// non-corruption -- neighbouring roster slots (previous/next slot in the same player row, the same
// slot in a different player row) and the fields this function never reaches on the target slot /
// header record (order/state/activity_clock on the slot; unit_proto_id/state on the header) read
// back exactly as seeded. There is no branch in the original (straight-line, no CALLs besides the
// inert Watcom stack-capacity probe at 0x00487b2d) so there is no gate/early-return/loop to
// separately cover.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_unit_free_slot_00487b25.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_free_slot.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// `unit_above` is `uint8_t[2]` -- pack/unpack exactly like every sibling TU's own local copy of
// this pair (see sim_unit_put_on_map.cpp's original definition). The .cpp under test defines its
// own copy too (internal linkage, not reusable from here), so this is this TU's own copy.
uint16_t above_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}
void set_above_word(uint8_t (&packed)[2], uint16_t value) {
    packed[0] = static_cast<uint8_t>(value & 0xffu);
    packed[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

constexpr uint32_t P_MAIN = 3; // nonzero player -- separates the row stride (IMUL ...,0x5b04)
constexpr int32_t  S_MAIN = 5; // nonzero, non-header slot -- separates the per-record stride (IMUL ...,0xe9)

} // namespace

void run_unit_free_slot_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1/T2/T3 -- one call, seeded for all three at once: all four writes are unconditional and
    // mutually independent (no gate reorders or suppresses any of them), so a single invocation is
    // enough to observe all four writes plus every non-corruption fact together.
    // =================================================================================================
    {
        fx.reset();

        unit &target         = fx.u(P_MAIN, S_MAIN);
        target.unit_proto_id = 0xbeef;             // nonzero sentinel, must become 0
        set_above_word(target.unit_above, 0xcafe); // nonzero sentinel, must become 0
        target.order          = 0x1357;            // untouched by this function -- must read back unchanged
        target.state          = 0x2468;            // untouched by this function -- must read back unchanged
        target.activity_clock = 12345.5;           // untouched by this function -- must read back unchanged

        unit &header = fx.u(P_MAIN, 0);
        set_above_word(header.unit_above, 7); // distinct from order's seed below
        header.order         = 13;            // distinct from unit_above's seed above
        header.unit_proto_id = 0xabcd;        // untouched -- row-only DECs never reach this field
        header.state         = 0x9876;        // untouched -- row-only DECs never reach this field

        // Neighbours this call must NOT touch: previous/next slot in the same player row, and the
        // SAME slot in a different player's row. Each seeded with its own distinct sentinel so a
        // wrong-index write (row stride or per-record stride off by one) lands somewhere observable.
        unit &prev_slot         = fx.u(P_MAIN, S_MAIN - 1);
        unit &next_slot         = fx.u(P_MAIN, S_MAIN + 1);
        unit &other_row         = fx.u(P_MAIN + 1, S_MAIN);
        prev_slot.unit_proto_id = 0x1111;
        set_above_word(prev_slot.unit_above, 0x2222);
        prev_slot.order         = 0x3333;
        next_slot.unit_proto_id = 0x4444;
        set_above_word(next_slot.unit_above, 0x5555);
        next_slot.order         = 0x6666;
        other_row.unit_proto_id = 0x7777;
        set_above_word(other_row.unit_above, 0x8888);
        other_row.order = 0x9999;

        sim_store own = fx.store();
        detail::unit_free_slot(own, P_MAIN, (int32_t)S_MAIN);

        // T1: the two direct field writes on the target slot.
        ck_eq(target.unit_proto_id, 0, "T1: units[player][slot].unit_proto_id == 0 (0x00487b55)");
        ck_eq(above_word(target.unit_above), 0, "T1: units[player][slot].unit_above == 0 (0x00487b71)");

        // T2: the two row-only header decrements -- INDEPENDENT counters, DIFFERENT fields, seeded
        // to different values so a swap between the two DECs would disagree here.
        ck_eq(above_word(header.unit_above), 6, "T2: units[player][0].unit_above == 7-1 (0x00487b84 DEC)");
        ck_eq(header.order, 12, "T2: units[player][0].order == 13-1 (0x00487b95 DEC)");

        // T3: non-corruption -- fields on the target slot / header this function never reaches.
        ck_eq(target.order, 0x1357, "T3: units[player][slot].order untouched (no store reaches it)");
        ck_eq(target.state, 0x2468, "T3: units[player][slot].state untouched (no store reaches it)");
        ck_eq_d(target.activity_clock, 12345.5, "T3: units[player][slot].activity_clock untouched");
        ck_eq(header.unit_proto_id, 0xabcd, "T3: units[player][0].unit_proto_id untouched (row-only DECs skip it)");
        ck_eq(header.state, 0x9876, "T3: units[player][0].state untouched (row-only DECs skip it)");

        // T3: non-corruption -- neighbouring roster slots this call never addresses.
        ck_eq(prev_slot.unit_proto_id, 0x1111, "T3: units[player][slot-1] untouched (unit_proto_id)");
        ck_eq(above_word(prev_slot.unit_above), 0x2222, "T3: units[player][slot-1] untouched (unit_above)");
        ck_eq(prev_slot.order, 0x3333, "T3: units[player][slot-1] untouched (order)");
        ck_eq(next_slot.unit_proto_id, 0x4444, "T3: units[player][slot+1] untouched (unit_proto_id)");
        ck_eq(above_word(next_slot.unit_above), 0x5555, "T3: units[player][slot+1] untouched (unit_above)");
        ck_eq(next_slot.order, 0x6666, "T3: units[player][slot+1] untouched (order)");
        ck_eq(other_row.unit_proto_id, 0x7777, "T3: units[player+1][slot] untouched (unit_proto_id)");
        ck_eq(above_word(other_row.unit_above), 0x8888, "T3: units[player+1][slot] untouched (unit_above)");
        ck_eq(other_row.order, 0x9999, "T3: units[player+1][slot] untouched (order)");
    }

    // =================================================================================================
    // T4 -- slot==0 edge case: the "target slot" record and the "header" record are the SAME object
    // (both address computations resolve to row+0*0xe9). This pins the WRITE ORDER, not just write
    // correctness: the 0x00487b84 header DEC reads units[player][0].unit_above AFTER the 0x00487b71
    // store already zeroed it (same address, re-derived fresh via IMUL each time -- there is no
    // cached copy anywhere in the asm), so it must wrap 0-1 -> 0xffff, NOT decrement the pre-zero
    // seed value (9-1=8, which is what a translation using a stale snapshot would produce). The
    // header's order DEC (0x00487b95, a DIFFERENT field at offset 0x4) is never touched by the first
    // two stores, so it decrements the ORIGINAL seeded value normally.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint32_t P_EDGE = 6; // nonzero, distinct from P_MAIN

        unit &rec         = fx.u(P_EDGE, 0);
        rec.unit_proto_id = 0x2222;
        set_above_word(rec.unit_above, 9);
        rec.order = 22;
        rec.state = 0x55; // untouched -- must read back unchanged

        sim_store own = fx.store();
        detail::unit_free_slot(own, P_EDGE, 0);

        ck_eq(rec.unit_proto_id, 0, "T4: slot==0 -- unit_proto_id == 0 (0x00487b55, same record as header)");
        ck_eq(above_word(rec.unit_above), 0xffff,
              "T4: slot==0 -- unit_above == 0xffff (0x00487b71 zeroes it, then 0x00487b84 DECs the "
              "JUST-ZEROED same word: 0-1 wraps to 0xffff, NOT the pre-zero seed's 9-1=8)");
        ck_eq(rec.order, 21, "T4: slot==0 -- order == 22-1 (0x00487b95, a DIFFERENT field, decrements the original seed)");
        ck_eq(rec.state, 0x55, "T4: slot==0 -- state untouched (no store reaches it)");
    }

    // =================================================================================================
    // T5 -- player truncation to its low 16 bits (MOVZX EAX, word ptr [...] at all four uses:
    // 0x00487b42, 0x00487b5e, 0x00487b7a, 0x00487b8b). A `player` value with garbage set above bit
    // 15 must land in the SAME row as its low-16-bit value, not be used full-width (which would
    // compute a wildly out-of-range row).
    // =================================================================================================
    {
        fx.reset();
        constexpr uint32_t P_HIGH_GARBAGE = 0x00070000u | P_MAIN; // low 16 bits == P_MAIN's value (3)
        constexpr int32_t  S_TRUNC        = 2;

        unit &target         = fx.u(P_MAIN, S_TRUNC); // the row the TRUNCATED player value must land in
        target.unit_proto_id = 0x3333;
        set_above_word(target.unit_above, 0x4444);

        unit &header = fx.u(P_MAIN, 0);
        set_above_word(header.unit_above, 4);
        header.order = 8;

        sim_store own = fx.store();
        detail::unit_free_slot(own, P_HIGH_GARBAGE, S_TRUNC);

        ck_eq(target.unit_proto_id, 0,
              "T5: player=0x70003 truncated to 0x0003 -- writes land at units[3][2].unit_proto_id (0x00487b42 MOVZX)");
        ck_eq(above_word(target.unit_above), 0,
              "T5: player truncated -- units[3][2].unit_above == 0 (0x00487b5e/0x00487b7a MOVZX)");
        ck_eq(above_word(header.unit_above), 3,
              "T5: player truncated -- units[3][0].unit_above == 4-1 (0x00487b7a MOVZX + 0x00487b84 DEC)");
        ck_eq(header.order, 7,
              "T5: player truncated -- units[3][0].order == 8-1 (0x00487b8b MOVZX + 0x00487b95 DEC)");
    }
}

} // namespace mh::sim::test
