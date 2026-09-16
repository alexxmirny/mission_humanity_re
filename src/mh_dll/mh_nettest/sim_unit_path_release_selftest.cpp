//
// sim_unit_path_release_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_path_encode_directions @0x0044955f and llm_strat_unit_release_path_and_targets
// @0x004888f4 (sim/resid/sim_unit_path_release.h/.cpp, SIM-RESID-D).
//
// NO SHADOW SITE for either function (see the header's "NO SHADOW SITE" note) -- this offline
// oracle is the only evidence either translation has.
//
// EXPECTED BEHAVIOUR from the .asm (never the Ghidra .c drafts, which this project's translator
// brief records as unreliable for this pair) and the header's own derivation:
//
//   unit_path_encode_directions: reads units[player][unit_idx].path_slot_id (0x0044957c-0x00449596,
//     NOT guarded against the 0xff "no path" sentinel -- a PRESERVE-BUG this oracle deliberately does
//     NOT reproduce, see T1's comment), then walks _G_LLM_STRAT_PATH_BUFFERS from cursor 0
//     (player*0xea60 + slot*0x258 + cursor*2 byte stride, LAB_004495a0). At each cursor: if
//     .heading==0 (CMP @0x004495ba) the loop exits WITHOUT touching that waypoint; otherwise
//     .run_length <<= 5 as a BYTE-WIDTH shift (SHL byte ptr [...],5 @0x004495e7, so bits 3-7 of the
//     original value are discarded, not just masked after a wider shift) and the cursor advances
//     (INC @0x004495c8).
//
//   unit_release_path_and_targets: three INDEPENDENTLY guarded releases against the unit's own
//     record (own.unit_at()), each guard controlling both the callee call AND the field clears:
//       (1) path_slot_id != 0xff (CMP @0x00488926 / JZ @0x0048892d) -> path_free_slot(player,
//           unit_idx) @0x00488936.
//       (2) target_ref != 0 (CMP @0x0048894e / JZ @0x00488956) -> target_release_ref(player,
//           unit_idx, mode=1) @0x00488964, THEN target_ref=0 @0x0048897c, THEN target_index=0
//           @0x00488998.
//       (3) target2_ref != 0 (CMP @0x004889b4 / JZ @0x004889bc) -> target_release_ref(player,
//           unit_idx, mode=3) @0x004889ca, THEN target2_ref=0 @0x004889e2, THEN target2_index=0
//           @0x004889fe.
//     Finally, UNCONDITIONALLY (no guard, always executed): out_cleared_pair[0]=0 @0x00488a0a,
//     out_cleared_pair[1]=0 @0x00488a13.
//
#include "sim/resid/sim_unit_path_release.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- helper for llm_strat_unit_path_encode_directions's waypoint buffer ------------------------
// Same player*PATH_WAYPOINTS_PER_PLAYER + slot*PATH_WAYPOINTS_PER_SLOT + cursor indexing
// sim_store::path_buffer_at() uses (sim_state.h), reproduced here directly against fx.path_buffers
// so seeding does not require going through the store first.
path_waypoint &wp_at(sim_fixture &fx, int32_t player, int32_t slot, int32_t cursor) {
    return fx.path_buffers[(size_t)player * (size_t)PATH_WAYPOINTS_PER_PLAYER +
                           (size_t)slot * (size_t)PATH_WAYPOINTS_PER_SLOT + (size_t)cursor];
}

// ---- mocks for llm_strat_unit_release_path_and_targets's two callees ---------------------------
struct path_free_slot_call {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<path_free_slot_call> g_path_free_slot_calls;
void                             rec_path_free_slot(uint16_t player, int32_t unit_index) {
    g_path_free_slot_calls.push_back({player, unit_index});
}

struct target_release_call {
    uint32_t player_idx;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<target_release_call> g_target_release_calls;
void                             rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    g_target_release_calls.push_back({player_idx, unit_idx, mode});
}

const unit_release_path_and_targets_calls g_calls = {
    &rec_path_free_slot,
    &rec_target_release_ref,
};

void reset_recorders() {
    g_path_free_slot_calls.clear();
    g_target_release_calls.clear();
}

} // namespace

void run_unit_path_release_tests() {
    sim_fixture fx;

    // =================================================================================================
    // llm_strat_unit_path_encode_directions @0x0044955f
    // =================================================================================================

    // =================================================================================================
    // T1 -- shift-in-place for pre-sentinel waypoints (with byte-width truncation visible), the
    // heading==0 sentinel stopping the walk WITHOUT touching its own run_length, waypoints beyond the
    // sentinel left untouched, and both stride negative arms (a neighbouring path slot of the SAME
    // player, and the SAME slot index of a DIFFERENT player). PLAYER and SLOT are both non-zero
    // (player*0xea60 + slot*0x258 strides @0x004495a4/0x004495aa) so a wrong stride is visible --
    // player 0 / slot 0 would make every stride bug silent.
    //
    // NOT tested: path_slot_id==0xff. The header's PRESERVE-BUG note records that this function has
    // no guard against the "no path assigned" sentinel, so calling it that way indexes far outside the
    // fixture's per-player path_buffers slice (slot 0xff=255 vs the real 100 slots/player) and would
    // trip ASan on an out-of-bounds fixture read, not exercise real behaviour.
    // =================================================================================================
    {
        fx.reset();
        constexpr uint16_t PLAYER          = 3;
        constexpr int32_t  UNIT_IDX        = 5;
        constexpr int32_t  SLOT            = 7;
        constexpr int32_t  NEIGHBOR_SLOT   = 8;
        constexpr uint16_t NEIGHBOR_PLAYER = 4;

        fx.u(PLAYER, UNIT_IDX).path_slot_id = (uint8_t)SLOT;

        // pre-sentinel waypoints (cursor 0, 1) -- both get shifted.
        wp_at(fx, PLAYER, SLOT, 0).heading    = 1;
        wp_at(fx, PLAYER, SLOT, 0).run_length = 0x05; // low3=5 (101b), high bits=0 -> 0x05<<5 = 0xA0
        wp_at(fx, PLAYER, SLOT, 1).heading    = 2;
        wp_at(fx, PLAYER, SLOT, 1).run_length = 0xD6; // low3=6 (110b) AND high bits=0x1A both nonzero
                                                      // -> byte-width shift truncates: 0xD6<<5 = 0xC0

        // sentinel at cursor 2 -- loop exits here; this waypoint's OWN run_length is untouched.
        wp_at(fx, PLAYER, SLOT, 2).heading    = 0;
        wp_at(fx, PLAYER, SLOT, 2).run_length = 0x77;

        // beyond-sentinel waypoints (cursor 3, 4) -- never reached, untouched.
        wp_at(fx, PLAYER, SLOT, 3).heading    = 9;
        wp_at(fx, PLAYER, SLOT, 3).run_length = 0x11;
        wp_at(fx, PLAYER, SLOT, 4).heading    = 3;
        wp_at(fx, PLAYER, SLOT, 4).run_length = 0x22;

        // neighbouring slot, SAME player -- must be untouched (slot stride negative arm).
        wp_at(fx, PLAYER, NEIGHBOR_SLOT, 0).heading    = 1;
        wp_at(fx, PLAYER, NEIGHBOR_SLOT, 0).run_length = 0x33;

        // SAME slot index, neighbouring player -- must be untouched (player stride negative arm).
        wp_at(fx, NEIGHBOR_PLAYER, SLOT, 0).heading    = 1;
        wp_at(fx, NEIGHBOR_PLAYER, SLOT, 0).run_length = 0x44;

        sim_store own = fx.store();
        detail::unit_path_encode_directions(fx.view(), own, PLAYER, UNIT_IDX);

        ck_eq((uint32_t)wp_at(fx, PLAYER, SLOT, 0).run_length, 0xA0u,
              "T1: waypoint 0 run_length <<= 5 (0x05 -> 0xA0), SHL byte ptr[...],5 @0x004495e7");
        ck_eq((uint32_t)wp_at(fx, PLAYER, SLOT, 1).run_length, 0xC0u,
              "T1: waypoint 1 run_length <<= 5 with BYTE-WIDTH truncation (0xD6 -> 0xC0), @0x004495e7");
        ck_eq((uint32_t)wp_at(fx, PLAYER, SLOT, 2).run_length, 0x77u,
              "T1: the sentinel waypoint's OWN run_length is NOT shifted (heading==0 test @0x004495ba)");
        ck_eq((uint32_t)wp_at(fx, PLAYER, SLOT, 3).run_length, 0x11u,
              "T1: waypoint beyond the sentinel untouched (loop already exited, JMP @0x004495c3)");
        ck_eq((uint32_t)wp_at(fx, PLAYER, SLOT, 4).run_length, 0x22u,
              "T1: second waypoint beyond the sentinel untouched");
        ck_eq((uint32_t)wp_at(fx, PLAYER, NEIGHBOR_SLOT, 0).run_length, 0x33u,
              "T1: neighbouring SLOT's waypoint untouched -- slot stride IMUL ...,0x258 @0x004495aa");
        ck_eq((uint32_t)wp_at(fx, NEIGHBOR_PLAYER, SLOT, 0).run_length, 0x44u,
              "T1: neighbouring PLAYER's waypoint untouched -- player stride IMUL ...,0xea60 @0x004495a4");
    }

    // =================================================================================================
    // llm_strat_unit_release_path_and_targets @0x004888f4
    // =================================================================================================

    constexpr uint16_t REL_PLAYER   = 2;
    constexpr int32_t  REL_UNIT_IDX = 11;

    // =================================================================================================
    // T2 -- ALL THREE guards TRUE: path_free_slot called once with the right args, target_release_ref
    // called TWICE in program order (primary mode=1 first, secondary mode=3 second), all four target
    // fields cleared, and the out_cleared_pair unconditional zero still holds on this arm too.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u              = fx.u(REL_PLAYER, REL_UNIT_IDX);
        u.path_slot_id       = 42; // real slot, != 0xff -- guard 1 TRUE
        u.target_ref         = 17; // nonzero -- guard 2 TRUE
        u.target_index       = 101;
        u.target2_ref        = 23; // nonzero -- guard 3 TRUE
        u.target2_index      = 202;
        uint32_t out_pair[2] = {0xDEADBEEFu, 0xCAFEBABEu};

        sim_store own = fx.store();
        detail::unit_release_path_and_targets(own, g_calls, REL_PLAYER, REL_UNIT_IDX, out_pair);

        ck_eq((uint32_t)g_path_free_slot_calls.size(), 1u,
              "T2: path_slot_id(42)!=0xff -- path_free_slot called once, guard @0x00488926/JZ@0x0048892d");
        if (!g_path_free_slot_calls.empty()) {
            ck_eq((uint32_t)g_path_free_slot_calls[0].player, (uint32_t)REL_PLAYER,
                  "T2: path_free_slot's player arg forwarded correctly");
            ck_eq((uint32_t)g_path_free_slot_calls[0].unit_index, (uint32_t)REL_UNIT_IDX,
                  "T2: path_free_slot's unit_index arg forwarded correctly");
        }
        ck_eq((uint32_t)g_target_release_calls.size(), 2u,
              "T2: both target guards true -- target_release_ref called exactly twice");
        if (g_target_release_calls.size() == 2) {
            ck_eq(g_target_release_calls[0].mode, 1u,
                  "T2: FIRST call is the PRIMARY release, EBX=1 @0x00488958 (program order, before secondary)");
            ck_eq(g_target_release_calls[1].mode, 3u,
                  "T2: SECOND call is the SECONDARY release, EBX=3 @0x004889be");
        }
        ck_eq((uint32_t)(uint16_t)u.target_ref, 0u, "T2: target_ref cleared @0x0048897c");
        ck_eq((uint32_t)(uint16_t)u.target_index, 0u, "T2: target_index cleared @0x00488998");
        ck_eq((uint32_t)(uint16_t)u.target2_ref, 0u, "T2: target2_ref cleared @0x004889e2");
        ck_eq((uint32_t)(uint16_t)u.target2_index, 0u, "T2: target2_index cleared @0x004889fe");
        ck_eq(out_pair[0], 0u, "T2: out_cleared_pair[0] zeroed, unconditional @0x00488a0a");
        ck_eq(out_pair[1], 0u, "T2: out_cleared_pair[1] zeroed, unconditional @0x00488a13");
    }

    // =================================================================================================
    // T3 -- ALL THREE guards FALSE (the negative arm of every guard at once): neither callee is ever
    // called, NEITHER the ref NOR the index field of a guard that never fired is touched (seeded with
    // distinct sentinels to prove it), and the out_cleared_pair is STILL unconditionally zeroed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u              = fx.u(REL_PLAYER, REL_UNIT_IDX);
        u.path_slot_id       = 0xff; // guard 1 FALSE
        u.target_ref         = 0;    // guard 2 FALSE
        u.target_index       = 555;  // sentinel -- must survive: the clear is inside the SAME guard as the call
        u.target2_ref        = 0;    // guard 3 FALSE
        u.target2_index      = 777;  // sentinel -- must survive
        uint32_t out_pair[2] = {0x11111111u, 0x22222222u};

        sim_store own = fx.store();
        detail::unit_release_path_and_targets(own, g_calls, REL_PLAYER, REL_UNIT_IDX, out_pair);

        ck_eq((uint32_t)g_path_free_slot_calls.size(), 0u,
              "T3: path_slot_id==0xff -- path_free_slot NEVER called, JZ @0x0048892d taken");
        ck_eq((uint32_t)g_target_release_calls.size(), 0u,
              "T3: target_ref==0 AND target2_ref==0 -- target_release_ref NEVER called");
        ck_eq((uint32_t)(uint16_t)u.target_index, 555u,
              "T3: target_index NOT cleared when guard 2 is false (sentinel survives)");
        ck_eq((uint32_t)(uint16_t)u.target2_index, 777u,
              "T3: target2_index NOT cleared when guard 3 is false (sentinel survives)");
        ck_eq(out_pair[0], 0u,
              "T3: out_cleared_pair[0] STILL zeroed -- unconditional even on the all-guards-false arm");
        ck_eq(out_pair[1], 0u,
              "T3: out_cleared_pair[1] STILL zeroed -- unconditional even on the all-guards-false arm");
    }

    // =================================================================================================
    // T4 -- ONLY the path-slot guard true: path_free_slot fires alone; both target guards stay false
    // and independent (their index sentinels survive) -- proves guard 1 does not gate guards 2/3.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u              = fx.u(REL_PLAYER, REL_UNIT_IDX);
        u.path_slot_id       = 13; // real slot -- guard 1 TRUE
        u.target_ref         = 0;  // guard 2 FALSE
        u.target_index       = 555;
        u.target2_ref        = 0; // guard 3 FALSE
        u.target2_index      = 777;
        uint32_t out_pair[2] = {1u, 1u};

        sim_store own = fx.store();
        detail::unit_release_path_and_targets(own, g_calls, REL_PLAYER, REL_UNIT_IDX, out_pair);

        ck_eq((uint32_t)g_path_free_slot_calls.size(), 1u,
              "T4: path_slot_id(13)!=0xff -- path_free_slot called once, guard 1 alone");
        ck_eq((uint32_t)g_target_release_calls.size(), 0u,
              "T4: both target guards false -- target_release_ref never called");
        ck_eq((uint32_t)(uint16_t)u.target_index, 555u,
              "T4: target_index untouched -- guard 2 is independent of guard 1");
        ck_eq((uint32_t)(uint16_t)u.target2_index, 777u,
              "T4: target2_index untouched -- guard 3 is independent of guard 1");
        ck_eq(out_pair[0], 0u, "T4: out_cleared_pair[0] zeroed, unconditional, on this arm too");
        ck_eq(out_pair[1], 0u, "T4: out_cleared_pair[1] zeroed, unconditional, on this arm too");
    }

    // =================================================================================================
    // T5 -- ONLY the PRIMARY target guard true: target_release_ref fires once with mode=1; path-slot
    // and secondary guards stay false and independent.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u              = fx.u(REL_PLAYER, REL_UNIT_IDX);
        u.path_slot_id       = 0xff; // guard 1 FALSE
        u.target_ref         = 9;    // nonzero -- guard 2 TRUE
        u.target_index       = 606;
        u.target2_ref        = 0; // guard 3 FALSE
        u.target2_index      = 808;
        uint32_t out_pair[2] = {1u, 1u};

        sim_store own = fx.store();
        detail::unit_release_path_and_targets(own, g_calls, REL_PLAYER, REL_UNIT_IDX, out_pair);

        ck_eq((uint32_t)g_path_free_slot_calls.size(), 0u, "T5: path_slot_id==0xff -- never called");
        ck_eq((uint32_t)g_target_release_calls.size(), 1u,
              "T5: only the primary guard true -- target_release_ref called once");
        if (!g_target_release_calls.empty())
            ck_eq(g_target_release_calls[0].mode, 1u,
                  "T5: PRIMARY release mode==1, MOV EBX,1 @0x00488958");
        ck_eq((uint32_t)(uint16_t)u.target_ref, 0u, "T5: target_ref cleared @0x0048897c");
        ck_eq((uint32_t)(uint16_t)u.target_index, 0u, "T5: target_index cleared @0x00488998");
        ck_eq((uint32_t)(uint16_t)u.target2_ref, 0u,
              "T5: target2_ref already 0, guard 3 false -- untouched");
        ck_eq((uint32_t)(uint16_t)u.target2_index, 808u,
              "T5: target2_index NOT cleared -- guard 3 is independent and false");
        ck_eq(out_pair[0], 0u, "T5: out_cleared_pair[0] zeroed, unconditional, on this arm too");
        ck_eq(out_pair[1], 0u, "T5: out_cleared_pair[1] zeroed, unconditional, on this arm too");
    }

    // =================================================================================================
    // T6 -- ONLY the SECONDARY target guard true: target_release_ref fires once with mode=3; path-slot
    // and primary guards stay false and independent. Mirrors T5 to prove target_ref/target2_ref are
    // not aliased or swapped.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u              = fx.u(REL_PLAYER, REL_UNIT_IDX);
        u.path_slot_id       = 0xff; // guard 1 FALSE
        u.target_ref         = 0;    // guard 2 FALSE
        u.target_index       = 606;
        u.target2_ref        = 11; // nonzero -- guard 3 TRUE
        u.target2_index      = 909;
        uint32_t out_pair[2] = {1u, 1u};

        sim_store own = fx.store();
        detail::unit_release_path_and_targets(own, g_calls, REL_PLAYER, REL_UNIT_IDX, out_pair);

        ck_eq((uint32_t)g_path_free_slot_calls.size(), 0u, "T6: path_slot_id==0xff -- never called");
        ck_eq((uint32_t)g_target_release_calls.size(), 1u,
              "T6: only the secondary guard true -- target_release_ref called once");
        if (!g_target_release_calls.empty())
            ck_eq(g_target_release_calls[0].mode, 3u,
                  "T6: SECONDARY release mode==3, MOV EBX,3 @0x004889be");
        ck_eq((uint32_t)(uint16_t)u.target2_ref, 0u, "T6: target2_ref cleared @0x004889e2");
        ck_eq((uint32_t)(uint16_t)u.target2_index, 0u, "T6: target2_index cleared @0x004889fe");
        ck_eq((uint32_t)(uint16_t)u.target_ref, 0u,
              "T6: target_ref already 0, guard 2 false -- untouched");
        ck_eq((uint32_t)(uint16_t)u.target_index, 606u,
              "T6: target_index NOT cleared -- guard 2 is independent and false");
        ck_eq(out_pair[0], 0u, "T6: out_cleared_pair[0] zeroed, unconditional, on this arm too");
        ck_eq(out_pair[1], 0u, "T6: out_cleared_pair[1] zeroed, unconditional, on this arm too");
    }
}

} // namespace mh::sim::test
