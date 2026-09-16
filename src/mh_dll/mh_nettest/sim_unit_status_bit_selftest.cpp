//
// sim_unit_status_bit_selftest.cpp -- `simtest` cases for the BODIES of
// llm_unit_status_bit_set @0x0044b09e and llm_unit_status_bit_clear @0x0044b0ec
// (sim/sim_unit_status_bits.h/.cpp, RI-SIM / SIM0 pilot).
//
// NOT the dispatcher-routing test. sim_dispatch_admin_selftest.cpp's arms 1/2 prove that
// order_queue_dispatch's admin table ROUTES opcodes 0x34/0x35 to these two callees (as recording
// mocks) with the right truncated argument. This file covers what the two functions actually DO to
// the unit record once called: the exact word they OR / AND, the mask they build, and the several
// faithfully-preserved quirks of that mask.
//
// NO CALLS STRUCT. Both bodies are leaves -- they call nothing, they only mutate one 16-bit field of
// one roster record. So the assertions here are purely (a) the exact field mutation for set vs clear
// and (b) that the neighbouring fields are left untouched (sentinels). There is no callee sequence
// to record.
//
// THE FIELD IS `unit::ai_group_index` (+0xd8), a MISNOMER -- the AI group machinery reads the same
// 16-bit slot as a group index (0xffff = none) while these two treat it as a flag word (OR/AND). The
// discrepancy is recorded on the Ghidra field comment and both plates; nothing here depends on which
// reading is right. See sim_unit_status_bits.h.
//
// EXPECTED VALUES ARE DERIVED FROM THE DISASSEMBLY, not echoed from the C++ translation:
//   SET   (llm_unit_status_bit_set_0044b09e.asm):
//     MOV EAX,1 / SHL EAX,CL         @0x0044b0c0/c5 -- mask = 1u << (bit & 31), CL masked to 5 bits
//     OR word ptr [.. + 0xdd8d20],AX @0x0044b0dd     -- a WORD store: only the low 16 bits of the
//                                                       mask reach the field.
//   CLEAR (llm_unit_status_bit_clear_0044b0ec.asm):
//     MOV EAX,1 / SHL EAX,CL          @0x0044b10e/13 -- same 32-bit mask build
//     XOR dword ptr [mask],0xffff     @0x0044b118    -- complement the LOW WORD of the DWORD mask
//     AND word ptr [.. + 0xdd8d20],AX @0x0044b132    -- a WORD store of that complemented low word.
//
// The two WORD stores are what make bit_index >= 16 a no-op in BOTH directions (the mask's low word
// is 0 for set / 0xffff after the XOR for clear), and the CL-masks-to-5-bits shift is what makes
// bit_index == 32 alias bit 0. Each is pinned below with a FINDING comment.
//
#include "sim/sim_unit_status_bits.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A roster slot far from [0][0] so the row/column strides (0x5b04 / 0xe9) are actually exercised, and
// player != index so a translation that swapped the two IMULs lands in a different record. Both stay
// in range (MAX_PLAYERS 8, UNITS_PER_PLAYER 100).
constexpr int32_t PLAYER = 3;
constexpr int32_t INDEX  = 42;

// Distinct, non-symmetric sentinels for the three 16-bit neighbours that bracket ai_group_index
// (+0xd8): ai_group_next (+0xd4), ai_group_prev (+0xd6, the word IMMEDIATELY before the target), and
// incoming_threat_damage (+0xdc, the first field past the +0xda padding). A WORD store at +0xd8 must
// leave all three exactly as seeded -- if any changes, the store hit the wrong width or offset.
constexpr uint16_t SENT_NEXT  = 0x1111;
constexpr uint16_t SENT_PREV  = 0x2222;
constexpr int16_t  SENT_AFTER = 0x3333;

void seed_neighbours(unit &u) {
    u.ai_group_next          = SENT_NEXT;
    u.ai_group_prev          = SENT_PREV;
    u.incoming_threat_damage = SENT_AFTER;
}

void ck_neighbours_intact(const unit &u, const char *what) {
    ck(u.ai_group_next == SENT_NEXT && u.ai_group_prev == SENT_PREV &&
           u.incoming_threat_damage == SENT_AFTER,
       what);
}

// ---- SET: llm_unit_status_bit_set @0x0044b09e --------------------------------------------------

// A single bit into a cleared field, with the neighbours seeded: proves the mask is 1<<bit, the store
// is exactly the one 16-bit field, and the row/column addressing lands where seeded.
void test_set_single_bit_into_clear_field() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0;
    seed_neighbours(u);

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/3);

    ck_eq(u.ai_group_index, 0x0008u, "set bit 3 into 0x0000 -> 0x0008 (mask 1<<3)");
    ck_neighbours_intact(u, "set: the WORD store at +0xd8 leaves both neighbour words + the field "
                            "past the pad untouched");
}

// OR accumulates and is idempotent: setting a second bit ORs it in, and re-setting an already-set bit
// is a no-op. `OR word ptr,AX` never clears, so a translation that assigned the mask instead of ORing
// would fail the first assert.
void test_set_accumulates_and_is_idempotent() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x0041; // bits 0 and 6 already set

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/1); // add bit 1 (mask 0x02)
    ck_eq(u.ai_group_index, 0x0043u, "set bit 1 into 0x0041 -> 0x0043 (OR, existing bits kept)");

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/0); // bit 0 already set
    ck_eq(u.ai_group_index, 0x0043u, "set an already-set bit 0 -> 0x0043 unchanged (OR is idempotent)");
}

// Bit 15 is the top bit of the 16-bit word and the last one a WORD store can reach.
void test_set_top_word_bit() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0;

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/15);
    ck_eq(u.ai_group_index, 0x8000u, "set bit 15 -> 0x8000 (top bit reachable by the WORD store)");
}

// FINDING (faithful, not a bug): bit_index >= 16 is a NO-OP for SET. The mask `1u << bit` is a full
// 32-bit value whose LOW 16 bits are 0 for bit 16..31, and `OR word ptr,AX` ORs only that low word --
// so ORing 0 changes nothing. A translation that did `field |= (uint16_t)(1u << bit)` after a 16-bit
// shift, or that stored the full DWORD, would set a spurious bit here instead.
void test_set_bit_ge_16_is_noop_FINDING() {
    // bit 16: 1<<16 = 0x1_0000, low word 0x0000.
    {
        sim_fixture f;
        sim_store   own  = f.store();
        unit       &u    = own.unit_at(PLAYER, INDEX);
        u.ai_group_index = 0x00ff;
        detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/16);
        ck_eq(u.ai_group_index, 0x00ffu,
              "FINDING set bit 16 -> unchanged (1<<16 low word is 0; WORD OR of 0 is a no-op)");
    }
    // bit 20: 1<<20 = 0x10_0000, low word 0x0000.
    {
        sim_fixture f;
        sim_store   own  = f.store();
        unit       &u    = own.unit_at(PLAYER, INDEX);
        u.ai_group_index = 0x1234;
        detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/20);
        ck_eq(u.ai_group_index, 0x1234u, "FINDING set bit 20 -> unchanged (low word of 1<<20 is 0)");
    }
}

// FINDING (faithful): bit_index == 32 sets bit 0, NOT nothing. `SHL EAX,CL` masks CL to 5 bits, so a
// shift count of 32 becomes 0 and the mask is 1<<0. Reproduced by the translation's `bit_index & 31`.
// This is the exact input the rig can never manufacture and the one that separates the CPU-masking
// reading from a "shift of 32 is undefined / zero" one.
void test_set_bit_32_aliases_bit_0_FINDING() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x0010;

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/32);
    ck_eq(u.ai_group_index, 0x0011u,
          "FINDING set bit 32 -> sets bit 0 (SHL count masked to 5 bits: 32 & 31 == 0)");
}

// ---- CLEAR: llm_unit_status_bit_clear @0x0044b0ec ----------------------------------------------

// A single bit cleared from a full field, neighbours seeded. mask = (1<<0) ^ 0xffff = 0xfffe, and
// `AND word ptr,AX` clears exactly bit 0.
void test_clear_single_bit_from_full_field() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x00ff;
    seed_neighbours(u);

    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/0);

    ck_eq(u.ai_group_index, 0x00feu, "clear bit 0 from 0x00ff -> 0x00fe (mask (1<<0)^0xffff = 0xfffe)");
    ck_neighbours_intact(u, "clear: the WORD store at +0xd8 leaves the neighbour words untouched");
}

// A non-zero bit position: clearing bit 7 leaves bits 0..6.
void test_clear_mid_bit() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x00ff;

    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/7);
    ck_eq(u.ai_group_index, 0x007fu, "clear bit 7 from 0x00ff -> 0x007f (mask (1<<7)^0xffff = 0xff7f)");
}

// Clearing an already-clear bit is a no-op (the AND leaves it 0).
void test_clear_already_clear_bit_is_noop() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x0004; // only bit 2 set

    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/1); // bit 1 already clear
    ck_eq(u.ai_group_index, 0x0004u, "clear already-clear bit 1 -> 0x0004 unchanged");
}

// Clearing the top word bit (15). mask = 0x8000 ^ 0xffff = 0x7fff.
void test_clear_top_word_bit() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x8001;

    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/15);
    ck_eq(u.ai_group_index, 0x0001u, "clear bit 15 from 0x8001 -> 0x0001 (mask 0x8000^0xffff = 0x7fff)");
}

// FINDING (faithful): bit_index >= 16 is a NO-OP for CLEAR -- "clear bit 20" clears NOTHING. For bit
// >= 16 the mask's low word is 0, XOR 0xffff makes it 0xffff, and `AND word ptr,0xffff` is a no-op.
// Computing the mask as `(uint16_t)~(1u << bit)` instead would clear bit (bit-16) here -- a different
// function. This is the exact asymmetry the header's own note calls out (mask built in 32 bits then
// truncated to the WORD, XOR before the truncation).
void test_clear_bit_ge_16_is_noop_FINDING() {
    // bit 16.
    {
        sim_fixture f;
        sim_store   own  = f.store();
        unit       &u    = own.unit_at(PLAYER, INDEX);
        u.ai_group_index = 0x1234;
        detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/16);
        ck_eq(u.ai_group_index, 0x1234u,
              "FINDING clear bit 16 -> unchanged (low word of mask is 0xffff; WORD AND is a no-op)");
    }
    // bit 20.
    {
        sim_fixture f;
        sim_store   own  = f.store();
        unit       &u    = own.unit_at(PLAYER, INDEX);
        u.ai_group_index = 0xabcd;
        detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/20);
        ck_eq(u.ai_group_index, 0xabcdu,
              "FINDING clear bit 20 -> unchanged (clears nothing, per the header note)");
    }
}

// FINDING (faithful): bit_index == 32 clears bit 0 (CL masked to 5 bits -> shift 0 -> mask 1 ->
// 1^0xffff = 0xfffe). Same CPU-masking quirk as the set path.
void test_clear_bit_32_aliases_bit_0_FINDING() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x0f0f;

    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/32);
    ck_eq(u.ai_group_index, 0x0f0eu,
          "FINDING clear bit 32 -> clears bit 0 (SHL count masked to 5 bits: 32 & 31 == 0)");
}

// SET then CLEAR of the same in-word bit round-trips to the original field -- the two are inverse for
// bit < 16, and this also re-confirms clear does not disturb the OTHER bits set meanwhile.
void test_set_then_clear_roundtrips() {
    sim_fixture f;
    sim_store   own  = f.store();
    unit       &u    = own.unit_at(PLAYER, INDEX);
    u.ai_group_index = 0x00c0; // bits 6,7 pre-set, must survive the round-trip

    detail::unit_status_bit_set(own, PLAYER, INDEX, /*bit_index=*/5);
    ck_eq(u.ai_group_index, 0x00e0u, "set bit 5 into 0x00c0 -> 0x00e0");
    detail::unit_status_bit_clear(own, PLAYER, INDEX, /*bit_index=*/5);
    ck_eq(u.ai_group_index, 0x00c0u, "clear bit 5 -> back to 0x00c0 (set/clear inverse for bit < 16)");
}

} // namespace

void run_unit_status_bit_tests() {
    test_set_single_bit_into_clear_field();
    test_set_accumulates_and_is_idempotent();
    test_set_top_word_bit();
    test_set_bit_ge_16_is_noop_FINDING();
    test_set_bit_32_aliases_bit_0_FINDING();
    test_clear_single_bit_from_full_field();
    test_clear_mid_bit();
    test_clear_already_clear_bit_is_noop();
    test_clear_top_word_bit();
    test_clear_bit_ge_16_is_noop_FINDING();
    test_clear_bit_32_aliases_bit_0_FINDING();
    test_set_then_clear_roundtrips();
}

} // namespace mh::sim::test
