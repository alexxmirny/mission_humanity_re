//
// ai_group_member_list_selftest.cpp -- `aitest` cases for the AI unit-group intrusive doubly-linked
// member list's two primitives, RI-AI batch D / AI1D: llm_strat_ai_group_member_link @0x004d4a2e
// (append a unit to the TAIL of ai_group_index's member list and stamp the unit's own group tag) and
// llm_strat_ai_group_member_unlink @0x004d4919 (splice a unit out of its current group's list from
// wherever it sits -- head, tail, middle, or the sole member). Neither function calls through `gc`
// or reads through `v` (see ai_group_member_list.h's banner), so every case here drives `detail::`
// directly over a fixture and the local `calls()` below binds nothing.
//
#include "ai_test_support.h"

#include "ai/ai_group_member_list.h"

namespace mh::ai::test {
namespace {

using namespace mh::ai;

// Neither group_member_link nor group_member_unlink has a single callee besides the inert Watcom
// stack-capacity probe (translator-brief rule 6, omitted from the translation), so this table binds
// NOTHING -- every member stays null. That is deliberate: if a mutation ever introduced a call
// through `gc`, it should crash loudly here rather than silently returning zero.
const ai_calls &calls() {
    static const ai_calls c{};
    return c;
}

} // namespace

void run_group_member_list_tests() {
    printf("-- ai_group_member_list (llm_strat_ai_group_member_link/_unlink) --\n");

    const uint32_t P = 3; // not 0/1: separates a player-row bug from a group-index bug

    // ================================================================================================
    // group_member_link @0x004d4a2e
    // ================================================================================================

    // ---- L1: linking into an EMPTY group (tail_unit == 0) makes the unit both ends -------------
    {
        fixture f;
        f.reset();
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[2];
        g.head_unit     = 0;
        g.tail_unit     = 0;
        g.member_count  = 0;

        detail::group_member_link(f.view(), f.store(), calls(), P, 2, 11);

        ck(f.u(P, 11).ai_group_index == 2,
           "L1 link: 0x004d4a4e stamps the unit's own ai_group_index UNCONDITIONALLY, before the "
           "empty-list test is even evaluated");
        ck(g.head_unit == 11, "L1 link: 0x004d4aac -- empty list, so head_unit becomes the new unit");
        ck(g.tail_unit == 11, "L1 link: 0x004d4ab3 -- and tail_unit becomes the same new unit");
        ck(f.u(P, 11).ai_group_next == 0,
           "L1 link: 0x004d4aba -- the sole member's own next terminates the list");
        ck(f.u(P, 11).ai_group_prev == 0,
           "L1 link: 0x004d4ac3 -- and its own prev terminates the list the other way");
        ck(g.member_count == 1,
           "L1 link: 0x004d4ae8 -- member_count increments in the SHARED tail reached by the empty "
           "branch too, not only the non-empty one");
    }

    // ---- L2: linking into a NON-EMPTY group appends at the tail, both directions stitched -------
    {
        fixture f;
        f.reset();
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[2];
        // A pre-existing one-element list: unit 17 is both head and tail.
        g.head_unit               = 17;
        g.tail_unit               = 17;
        g.member_count            = 1;
        f.u(P, 17).ai_group_next  = 0;
        f.u(P, 17).ai_group_prev  = 0;
        f.u(P, 17).ai_group_index = 2;

        detail::group_member_link(f.view(), f.store(), calls(), P, 2, 23);

        ck(f.u(P, 23).ai_group_index == 2,
           "L2 link: 0x004d4a4e stamps the new unit's ai_group_index unconditionally on the "
           "non-empty path too");
        ck(f.u(P, 23).ai_group_next == 0,
           "L2 link: 0x004d4a7b -- the new tail's own next terminates the list");
        ck(f.u(P, 23).ai_group_prev == 17,
           "L2 link: 0x004d4a84/0x004d4a8b -- the new unit's prev is the OLD tail, read fresh before "
           "being overwritten");
        ck(f.u(P, 17).ai_group_next == 23,
           "L2 link: 0x004d4a92-0x004d4a9b -- the OLD tail's own next is re-pointed at the new unit "
           "(the forward stitch, checked on the OTHER node than the one just linked)");
        ck(g.tail_unit == 23, "L2 link: 0x004d4aa3 -- the group's tail_unit becomes the new unit");
        ck(g.head_unit == 17,
           "L2 link: the non-empty branch never touches head_unit -- a 2-element list still starts "
           "at the original head");
        ck(g.member_count == 2,
           "L2 link: 0x004d4ae8 -- member_count also increments on the non-empty path (SAME shared "
           "instruction as L1, not a second copy that could diverge)");
    }

    // ---- L3: ai_group_index == -1 is UNCHECKED -- it indexes ai_groups[-1], landing before the ---
    // ---- array but still inside player_data (bounds are unchecked per the roster window's own ----
    // ---- comment), and the int32_t->uint16_t truncating stamp collides -1 with the 0xffff "no ----
    // ---- group" sentinel. Preserved, not fixed. ---------------------------------------------------
    {
        fixture f;
        f.reset();
        player_data &pd              = f.players[P];
        pd.ai_groups[0].head_unit    = 0;
        pd.ai_groups[0].tail_unit    = 0;
        pd.ai_groups[0].member_count = 0;

        detail::group_member_link(f.view(), f.store(), calls(), P, -1, 29);

        ck(pd.ai_groups[0].head_unit == 0 && pd.ai_groups[0].tail_unit == 0 &&
               pd.ai_groups[0].member_count == 0,
           "L3 link: ai_group_index=-1 does NOT alias group 0 -- pd.ai_groups[-1] lands BEFORE the "
           "array, so group 0's fields are untouched (bounds-unchecked indexing, preserved per the "
           "roster window's own comment)");
        ck(f.u(P, 29).ai_group_index == 0xffffu,
           "L3 link: PRESERVE-BUG -- the truncating store at 0x004d4a4e writes "
           "(uint16_t)(-1) == 0xffff, so linking with group_index=-1 stamps the unit with the SAME "
           "value the sentinel 'in no group' uses, even though the unit was just linked");
    }

    // ---- L4: group index 0x1f (the top of the 32-slot array) links cleanly, and writing player P's
    // ---- row does not touch player P+1's identical group index -----------------------------------
    {
        fixture f;
        f.reset();
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[0x1f];
        g.head_unit     = 0;
        g.tail_unit     = 0;
        g.member_count  = 0;
        // A same-shape group at the same index on the NEXT player, to prove row isolation.
        unit_group &other  = f.players[P + 1].ai_groups[0x1f];
        other.head_unit    = 0;
        other.tail_unit    = 0;
        other.member_count = 0;

        detail::group_member_link(f.view(), f.store(), calls(), P, 0x1f, 31);

        ck(g.head_unit == 31 && g.tail_unit == 31 && g.member_count == 1,
           "L4 link: group index 0x1f (last of the 32-slot array) links exactly like any other slot");
        ck(other.head_unit == 0 && other.tail_unit == 0 && other.member_count == 0,
           "L4 link: player P+1's ai_groups[0x1f] is untouched -- the player-row multiply, not just "
           "the group-index multiply, addresses the right record");
    }

    // ================================================================================================
    // group_member_unlink @0x004d4919 -- built on a manually-seeded 3-node chain so each scenario is
    // independently attributable: head=41 -> 53 -> tail=67 (three DISTINCT, non-symmetric ids).
    // ================================================================================================
    auto seed_chain = [&](fixture &f, unit_group &g) {
        f.reset();
        g.head_unit               = 41;
        g.tail_unit               = 67;
        g.member_count            = 3;
        f.u(P, 41).ai_group_next  = 53;
        f.u(P, 41).ai_group_prev  = 0;
        f.u(P, 41).ai_group_index = 5;
        f.u(P, 53).ai_group_next  = 67;
        f.u(P, 53).ai_group_prev  = 41;
        f.u(P, 53).ai_group_index = 5;
        f.u(P, 67).ai_group_next  = 0;
        f.u(P, 67).ai_group_prev  = 53;
        f.u(P, 67).ai_group_index = 5;
    };

    // ---- U1: unlink the HEAD (prev == 0, next != 0) ----------------------------------------------
    {
        fixture      f;
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[5];
        seed_chain(f, g);

        detail::group_member_unlink(f.view(), f.store(), calls(), P, 5, 41);

        ck(g.head_unit == 53,
           "U1 unlink-head: 0x004d4980/0x004d4987 -- prev==0 selects the HEAD arm, so head_unit "
           "becomes the unlinked unit's own (unmodified) ai_group_next");
        ck(f.u(P, 53).ai_group_prev == 0,
           "U1 unlink-head: 0x004d4944-0x004d4958's sibling logic on the OTHER side -- the new head "
           "(53) gets its own prev zeroed via the NEXT-side stitch (next(41)=53 != 0 -> "
           "next_unit.prev = this.prev = 0)");
        ck(f.u(P, 53).ai_group_next == 67,
           "U1 unlink-head: unit 53's own next is untouched by either splice -- still the tail id");
        ck(g.tail_unit == 67,
           "U1 unlink-head: the two splices are INDEPENDENT -- removing the head never touches "
           "tail_unit");
        ck(f.u(P, 67).ai_group_prev == 53 && f.u(P, 67).ai_group_next == 0,
           "U1 unlink-head: unit 67 (the tail, two hops away from the removed node) is untouched "
           "entirely");
        ck(f.u(P, 41).ai_group_index == 0xffffu,
           "U1 unlink-head: 0x004d49fc -- the removed unit's own ai_group_index becomes the 0xffff "
           "sentinel, unconditionally");
        ck(f.u(P, 41).ai_group_next == 53 && f.u(P, 41).ai_group_prev == 0,
           "U1 unlink-head: the removed unit's OWN next/prev are read but NEVER WRITTEN by either "
           "branch (per the header's opcode-range check) -- they are left dangling at their old "
           "values, not cleared");
        ck(g.member_count == 2,
           "U1 unlink-head: 0x004d4a22 -- member_count decrements in the function's shared tail, not "
           "gated behind either branch");
    }

    // ---- U2: unlink a MIDDLE node (prev != 0, next != 0) -- both neighbours re-stitched to skip it
    {
        fixture      f;
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[5];
        seed_chain(f, g);

        detail::group_member_unlink(f.view(), f.store(), calls(), P, 5, 53);

        ck(f.u(P, 41).ai_group_next == 67,
           "U2 unlink-middle: 0x004d4944-0x004d4958 -- prev!=0 takes the ordinary splice, prev(41)."
           "next = this(53).next = 67");
        ck(f.u(P, 67).ai_group_prev == 41,
           "U2 unlink-middle: 0x004d49a7-0x004d49bb -- next!=0 takes the ordinary splice the other "
           "way, next(67).prev = this(53).prev = 41");
        ck(g.head_unit == 41 && g.tail_unit == 67,
           "U2 unlink-middle: a middle removal touches NEITHER group.head_unit NOR group.tail_unit");
        ck(f.u(P, 53).ai_group_next == 67 && f.u(P, 53).ai_group_prev == 41,
           "U2 unlink-middle: the removed unit's own next/prev are left dangling at their pre-removal "
           "values -- neither splice branch (both took the 'has a neighbour' arm here) ever writes "
           "back to node 53 itself");
        ck(f.u(P, 53).ai_group_index == 0xffffu, "U2 unlink-middle: 0x004d49fc sentinel, unconditional");
        ck(g.member_count == 2, "U2 unlink-middle: 0x004d4a22 decrement, unconditional");
    }

    // ---- U3: unlink the TAIL (prev != 0, next == 0) -----------------------------------------------
    {
        fixture      f;
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[5];
        seed_chain(f, g);

        detail::group_member_unlink(f.view(), f.store(), calls(), P, 5, 67);

        ck(f.u(P, 53).ai_group_next == 0,
           "U3 unlink-tail: 0x004d4944-0x004d4958 -- prev!=0 ordinary splice, prev(53).next = "
           "this(67).next = 0");
        ck(g.tail_unit == 53,
           "U3 unlink-tail: 0x004d49e1/0x004d49e8 -- next==0 selects the TAIL arm, tail_unit becomes "
           "the removed unit's own (unmodified) ai_group_prev");
        ck(g.head_unit == 41,
           "U3 unlink-tail: the two splices are independent -- removing the tail never touches "
           "head_unit");
        ck(f.u(P, 41).ai_group_next == 53 && f.u(P, 41).ai_group_prev == 0,
           "U3 unlink-tail: the head, two hops from the removed node, is untouched entirely");
        ck(f.u(P, 67).ai_group_next == 0 && f.u(P, 67).ai_group_prev == 53,
           "U3 unlink-tail: the removed unit's own next/prev are left dangling, not cleared");
        ck(f.u(P, 67).ai_group_index == 0xffffu, "U3 unlink-tail: 0x004d49fc sentinel, unconditional");
        ck(g.member_count == 2, "U3 unlink-tail: 0x004d4a22 decrement, unconditional");
    }

    // ---- U4: unlink the ONLY member (prev == 0 AND next == 0) -- both group ends reset to empty --
    {
        fixture f;
        f.reset();
        player_data &pd           = f.players[P];
        unit_group  &g            = pd.ai_groups[6];
        g.head_unit               = 71;
        g.tail_unit               = 71;
        g.member_count            = 1;
        f.u(P, 71).ai_group_next  = 0;
        f.u(P, 71).ai_group_prev  = 0;
        f.u(P, 71).ai_group_index = 6;

        detail::group_member_unlink(f.view(), f.store(), calls(), P, 6, 71);

        ck(g.head_unit == 0,
           "U4 unlink-only: 0x004d4980/0x004d4987 -- prev==0 selects the HEAD arm; head_unit becomes "
           "this unit's own next, which is 0, so the group is empty again");
        ck(g.tail_unit == 0,
           "U4 unlink-only: 0x004d49e1/0x004d49e8 -- next==0 ALSO selects the TAIL arm (both "
           "conditions are independently true for a sole member); tail_unit becomes this unit's own "
           "prev, also 0");
        ck(f.u(P, 71).ai_group_index == 0xffffu, "U4 unlink-only: 0x004d49fc sentinel, unconditional");
        ck(g.member_count == 0, "U4 unlink-only: 0x004d4a22 decrement -- back to zero members");
    }

    // ---- U5: unlink writes only the addressed player's row -----------------------------------------
    {
        fixture f;
        f.reset();
        player_data &pd = f.players[P];
        unit_group  &g  = pd.ai_groups[5];
        seed_chain(f, g);
        // Mirror the same chain shape on player P+1 so a player-row bug would show up as a change
        // here instead of (or in addition to) the intended row.
        unit_group &other            = f.players[P + 1].ai_groups[5];
        other.head_unit              = 41;
        other.tail_unit              = 67;
        other.member_count           = 3;
        f.u(P + 1, 41).ai_group_next = 53;
        f.u(P + 1, 41).ai_group_prev = 0;
        f.u(P + 1, 53).ai_group_next = 67;
        f.u(P + 1, 53).ai_group_prev = 41;
        f.u(P + 1, 67).ai_group_next = 0;
        f.u(P + 1, 67).ai_group_prev = 53;

        detail::group_member_unlink(f.view(), f.store(), calls(), P, 5, 53);

        ck(other.head_unit == 41 && other.tail_unit == 67 && other.member_count == 3,
           "U5 unlink: player P+1's identically-shaped group is untouched by an unlink on player P");
        ck(f.u(P + 1, 41).ai_group_next == 53 && f.u(P + 1, 53).ai_group_prev == 41 &&
               f.u(P + 1, 53).ai_group_next == 67 && f.u(P + 1, 67).ai_group_prev == 53,
           "U5 unlink: player P+1's roster links are untouched -- the player-row multiply addresses "
           "the right unit records, not just the right group");
    }
}

} // namespace mh::ai::test
