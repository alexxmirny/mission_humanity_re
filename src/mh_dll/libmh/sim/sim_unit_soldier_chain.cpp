#include "sim/sim_unit_soldier_chain.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

// ---- llm_strat_unit_soldier_remove_last @0x00489595 --------------------------------------------
//
// 0x004895c7-0x004895e1: seed the walk from the chain head (unit_above, byte[2] reassembled LE --
// same pattern sim_unit_update_soldiers.cpp's cur_unit->unit_above read and sim_unit_passive_engage.cpp
// use for the identical field).
//
// 0x004895e4-0x00489614: a do-while carrying TWO lagging cursors, `prev2`/`prev1` below (the exported
// .c's local_1c/local_24), one and two steps behind the walker `cur`. Each iteration: prev2=prev1(old),
// prev1=cur(old), cur=next_soldier(cur(old)); loop continues while the NEW cur != 0. A THIRD stack slot
// (EBP-0x1c) is incremented every iteration (0x004895e7) but never read back -- the one load from it
// (0x004895e4, into EAX) is immediately overwritten by the next instruction before EAX is ever used, so
// it is a dead compiler-generated trip counter with zero observable effect; omitted here.
//
// Hand-simulated against three chain lengths (task hazard #2), all confirmed against this exact
// derivation:
//   length 0 (head==0): iteration 1 reads next_soldier(soldier[0]) -- if that is 0 (the expected state
//     for a never-linked sentinel), the loop ends with prev2==prev1==0. Both post-loop writes therefore
//     land on record 0: its next_soldier is zeroed (twice, redundantly) and its owner_unit is set to 0
//     THEN decremented to -1 (stored as int16_t -1 / 0xffff) -- calling this function on an empty chain
//     corrupts the per-player soldier COUNT. A genuine original bug, transcribed as written, not fixed.
//   length 1 (head==S1, next_soldier(S1)==0): loop ends after one iteration with prev2==0, prev1==S1.
//     The "detach" write (next_soldier=0) lands on record 0 -- a no-op on the real chain, since record 0
//     was never actually linked to S1 -- rather than on units[player][unit_index].unit_above, which this
//     function does NOT write in any code path. So after removing a unit's only soldier, unit_above is
//     left STALE, still pointing at S1 (now blanked: owner_unit=0, next_soldier=0). Preserved, not
//     repaired -- the caller apparently relies on some other path (unit death/order handling) to clear
//     unit_above itself.
//   length 2 (head==S1->S2, next_soldier(S2)==0): loop ends after two iterations with prev2==S1,
//     prev1==S2 -- the intended/normal case: S1.next_soldier is zeroed (correctly detaching the tail),
//     S2 (the tail) is blanked, count decremented. This is the only length where the function does what
//     its plate says without a caveat.
//
// owner_unit is stored `int16_t` (mh_llm_strat_crew_soldier); the original's final decrement is a WORD
// DEC (0x66 prefix, `DEC word ptr`), matching -- a plain `-= 1` on the int16_t field wraps identically.
void unit_soldier_remove_last(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_index) {
    const uint32_t p = player & 0xffffu; // 0x004895c7: MOVZX word -- every index below masks player

    const unit    &u    = unit_of(v, p, unit_index);
    const uint32_t head = uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);

    uint32_t prev2 = 0, prev1 = 0, cur = head;
    do {
        prev2 = prev1;
        prev1 = cur;
        cur   = own.soldier_at(p, prev1).next_soldier; // 0x004895fa-0x0048960d: MOVZX word
    } while (cur != 0);

    own.soldier_at(p, prev2).next_soldier = 0; // 0x00489626
    own.soldier_at(p, prev1).next_soldier = 0; // 0x0048963f
    own.soldier_at(p, prev1).owner_unit   = 0; // 0x00489658
    own.soldier_at(p, 0).owner_unit -= 1;      // 0x0048966b: DEC word -- the per-player count
}

// ---- llm_strat_unit_soldier_unlink @0x0048967b --------------------------------------------------
//
// 0x004896ad-0x004896ca: the original reads unit_above TWICE at this point -- once into a local
// (0x004896b4), once fresh for the CMP (0x004896ca) -- Watcom recomputing rather than keeping a value
// live across the branch, the same idiom sim_unit_ctrl_group.cpp's unit_ctrl_group_assign documents for
// its own ctrl_group_id re-reads. Nothing runs between the two reads that could change unit_above, so
// caching ONE read into `head` below is value-identical, not a behaviour change.
//
// HEAD CASE (0x004896d6-0x00489707, head==soldier_idx): rewrite units[player][unit_idx].unit_above to
// the removed head's next_soldier (a plain 16-bit copy, MOV word -- no truncation) and skip the walk.
// This is the ONE write in this whole file that touches the unit record rather than only the soldier
// roster.
//
// ELSE (0x00489709-0x0048974c, head!=soldier_idx): scan forward from the head for the PREDECESSOR --
// the soldier whose next_soldier equals the target -- with NO bound and NO null check (task hazard #4):
// if soldier_idx is not actually reachable from this chain, `walker` eventually becomes 0 (record 0,
// the count sentinel) and the loop keeps reading _G_LLM_STRAT_SOLDIERS[player][0].next_soldier and
// whatever it chains to next, forever. Transcribed exactly, no guard added. A second dead stack slot
// (EBP-0x18) is incremented every iteration (0x00489749) and never read back -- same vestigial
// trip-counter pattern as remove_last's EBP-0x1c above, omitted.
//
// Once found (LAB_0048974e): predecessor.next_soldier = target.next_soldier -- splices the target out.
//
// TAIL (0x0048977c-0x004897ae, unconditional either way): blank the removed record and decrement the
// per-player count at record 0 -- identical to remove_last's own tail, including the word-width DEC.
void unit_soldier_unlink(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_idx,
                         uint32_t soldier_idx) {
    const uint32_t p = uint32_t(player) & 0xffffu;

    unit          &u    = own.unit_at(p, unit_idx);
    const uint32_t head = uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);

    if (head == soldier_idx) {
        const uint16_t new_head = own.soldier_at(p, head).next_soldier; // 0x004896f9
        u.unit_above[0]         = uint8_t(new_head & 0xffu);            // 0x00489700 MOV word, split LE
        u.unit_above[1]         = uint8_t((new_head >> 8) & 0xffu);
    } else {
        uint32_t walker = head;
        while (own.soldier_at(p, walker).next_soldier != soldier_idx) { // 0x00489720/0x00489727
            walker = own.soldier_at(p, walker).next_soldier;            // 0x0048973c/0x00489743
        }
        own.soldier_at(p, walker).next_soldier =
            own.soldier_at(p, soldier_idx).next_soldier; // 0x0048976e-0x00489775
    }

    own.soldier_at(p, soldier_idx).next_soldier = 0; // 0x0048978c
    own.soldier_at(p, soldier_idx).owner_unit   = 0; // 0x004897a5
    own.soldier_at(p, 0).owner_unit -= 1;            // 0x004897b8: DEC word -- the per-player count
}

// ---- llm_strat_unit_soldiers_set_heading @0x00489ab6 ---------------------------------------------
//
// 0x00489af9-0x00489b36: UNCONDITIONAL do-while, seeded from the chain head exactly like the two
// functions above. Task hazard #5 confirmed against the .asm: there is no "if (head != 0) skip the
// loop" guard anywhere in the body, so a unit with an EMPTY chain (unit_above==0) still runs the loop
// body once against record 0 -- _G_LLM_STRAT_SOLDIERS[player][0].sprite_frame gets stamped with the
// caller's value. sprite_frame (offset 0x06) is a DIFFERENT field from owner_unit (offset 0x00, the
// count this record doubles as), so the count itself is not corrupted by this write, but the write to
// the sentinel record is real and is reproduced, not guarded away.
//
// sprite_frame is copied byte-for-byte (0x00489b09-0x00489b0c: MOV AL, byte / MOV byte, AL -- no
// sign/zero extension either side), matching the struct's `int8_t sprite_frame` via a plain
// reinterpreting cast of the incoming `uint8_t` parameter.
void unit_soldiers_set_heading(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_index,
                               uint8_t sprite_frame) {
    const uint32_t p = uint32_t(player) & 0xffffu;

    const unit &u      = unit_of(v, p, unit_index);
    uint32_t    walker = uint32_t(u.unit_above[0]) | (uint32_t(u.unit_above[1]) << 8);

    do {
        own.soldier_at(p, walker).sprite_frame = int8_t(sprite_frame);                   // 0x00489b0c
        walker                                 = own.soldier_at(p, walker).next_soldier; // 0x00489b28
    } while (walker != 0);
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

void unit_soldier_remove_last(uint32_t player, int32_t unit_index) {
    sim_state st = state();
    detail::unit_soldier_remove_last(st.read, st.own, player, unit_index);
}

void unit_soldier_unlink(uint16_t player, int32_t unit_idx, uint32_t soldier_idx) {
    sim_state st = state();
    detail::unit_soldier_unlink(st.read, st.own, player, unit_idx, soldier_idx);
}

void unit_soldiers_set_heading(uint16_t player, int32_t unit_index, uint8_t sprite_frame) {
    sim_state st = state();
    detail::unit_soldiers_set_heading(st.read, st.own, player, unit_index, sprite_frame);
}


} // namespace mh::sim
