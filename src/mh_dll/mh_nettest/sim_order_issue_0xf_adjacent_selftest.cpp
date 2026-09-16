//
// sim_order_issue_0xf_adjacent_selftest.cpp -- `simtest` oracle for
// llm_strat_order_issue_0xf_adjacent_by_offset @0x0046a19d (sim/sim_order_issue_0xf_adjacent.h/.cpp,
// RI-SIM / SIM1-G1).
//
// arm_ready:false -- `by_offset`'s own note says it was NOT armed on a live rig; this offline oracle
// is its only evidence.
//
// `by_offset` tail-calls its sibling `detail::order_issue_0xf_adjacent_enqueue` DIRECTLY (same TU,
// not through the `_calls` table -- see the header banner), so it cannot be isolated from `enqueue`:
// every case here runs BOTH bodies, and the only observation window is `enqueue`'s own five outward
// callees (tiles_adjacent, order_scratch_reset, order_scratch_set_field x2, order_enqueue,
// unit_notify_status -- 6 invocations total on the happy path), which ARE mockable via the `_calls`
// struct. `enqueue` itself is already T1-verified on its own armed rig evidence (per the batch brief)
// -- this file is not re-proving `enqueue`'s gates, it is proving the MASKED COORDINATES `by_offset`
// hands to `enqueue`, using `enqueue`'s callees as the window onto them. `enqueue`'s two guards each
// get one case anyway (T1/T2 below) because those are exactly the paths on which `by_offset`'s own
// work is discarded or exposed only through `tiles_adjacent`'s arguments.
//
// EXPECTED BEHAVIOUR from the .asm
// (tmp/decomp_sim/llm_strat_order_issue_0xf_adjacent_by_offset_0046a19d.asm), which the .cpp matches
// exactly (no divergence found):
//   0x0046a1be-0x0046a1e0: new_x = width_mask & (unit.x + dx)  -- unsigned 32-bit add (MOVZX byte,
//     then ADD, then AND), so a negative dx wraps via two's complement before the mask bites.
//   0x0046a1e3-0x0046a205: new_y = height_mask & (unit.y + dy), same shape on the other axis.
//   0x0046a208-0x0046a215: register shuffle (ECX=new_y, EBX=new_x, EDX=unit_idx, AX=player
//     ZERO-EXTENDED FROM A WORD, i.e. only the low 16 bits of the full-width `player` dword survive
//     into the tail call -- see the NOTE below) falling straight into `enqueue`'s own entry point,
//     no other call in between.
//   enqueue's own gates (already-verified territory, exercised here only to see by_offset's
//   coordinates through them): move_op_code==0xf (0x0046a346-0x0046a36d) -> tiles_adjacent(target,
//   unit)!=0 (0x0046a373-0x0046a3b4) -> scratch_reset, scratch_set_field(0,target_x),
//   scratch_set_field(1,target_y), order_enqueue(unit_idx, player|0x80, 0xf, 0x36),
//   unit_notify_status(player, unit_idx, 0) (0x0046a3b6-0x0046a3f6).
//
// NOTE (observed, not independently pinned by a case): `by_offset`'s own body reads `player` at
// FULL 32-bit width to index the unit roster (`unit_of(v, player, unit_idx)`, matching the .asm's
// `IMUL EAX,[EBP-0x20],0x5b04` using the full dword), but then narrows it to a 16-bit WORD
// (`MOVZX EAX, word ptr [EBP+-0x20]` at 0x0046a211) before tail-calling `enqueue` -- the .cpp
// mirrors this exactly (`unit_of(v, player, ...)` on the full uint32_t, then
// `static_cast<uint16_t>(player)` at the call into `enqueue`). Every real caller passes a small
// (0..7) player id, so the two never disagree in practice, and exercising the disagreement here
// would require indexing the unit roster with a value large enough to set a bit above 16 -- i.e. an
// out-of-bounds roster read no fixture extent covers. Left unpinned; flagging it for the record.
//
#include "sim/sim_order_issue_0xf_adjacent.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A single log of WHICH mock fired, in firing order -- see sim_dmp_enqueue_scripted_order_selftest's
// same pattern. This is what lets a case pin the [tiles_adjacent, reset, set_field, set_field,
// enqueue, notify_status] SEQUENCE, not just five isolated call counts.
enum class call_kind : uint32_t { TILES_ADJACENT,
                                  SCRATCH_RESET,
                                  SET_FIELD,
                                  ENQUEUE,
                                  NOTIFY_STATUS };
std::vector<call_kind> g_call_order;

struct tiles_adjacent_call {
    int32_t x1, y1, x2, y2;
};
std::vector<tiles_adjacent_call> g_tiles_adjacent_calls;
int32_t                          g_tiles_adjacent_result = 1;
int32_t                          rec_tiles_adjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_tiles_adjacent_calls.push_back({x1, y1, x2, y2});
    g_call_order.push_back(call_kind::TILES_ADJACENT);
    return g_tiles_adjacent_result;
}

int32_t g_reset_calls = 0;
void    rec_order_scratch_reset() {
    ++g_reset_calls;
    g_call_order.push_back(call_kind::SCRATCH_RESET);
}

struct set_field_call {
    int32_t index, value;
};
std::vector<set_field_call> g_set_field_calls;
void                        rec_order_scratch_set_field(int32_t index, int32_t value) {
    g_set_field_calls.push_back({index, value});
    g_call_order.push_back(call_kind::SET_FIELD);
}

struct enqueue_call {
    uint16_t unit_index, owner_and_kind;
    int16_t  param0;
    uint16_t order_code;
};
std::vector<enqueue_call> g_enqueue_calls;
int32_t                   rec_order_enqueue(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                                            uint16_t order_code) {
    g_enqueue_calls.push_back({unit_index, owner_and_kind, param0, order_code});
    g_call_order.push_back(call_kind::ENQUEUE);
    return 0; // enqueue's own return value is never read by `enqueue`'s body (0x0046a3ed zeroes EBX
              // unconditionally right after the call) -- already-verified territory, not this row's.
}

struct notify_call {
    uint32_t player;
    int32_t  unit_index;
    uint32_t status_code;
};
std::vector<notify_call> g_notify_calls;
void                     rec_unit_notify_status(uint32_t player, int32_t unit_index, uint32_t status_code) {
    g_notify_calls.push_back({player, unit_index, status_code});
    g_call_order.push_back(call_kind::NOTIFY_STATUS);
}

const order_issue_0xf_adjacent_calls g_calls = {
    &rec_tiles_adjacent,
    &rec_order_scratch_reset,
    &rec_order_scratch_set_field,
    &rec_order_enqueue,
    &rec_unit_notify_status,
};

void reset_recorders() {
    g_call_order.clear();
    g_tiles_adjacent_calls.clear();
    g_tiles_adjacent_result = 1;
    g_reset_calls           = 0;
    g_set_field_calls.clear();
    g_enqueue_calls.clear();
    g_notify_calls.clear();
}

// Sets up a ground-mover unit (move_op_code==0xf, enqueue's gate 1) at tile (x,y), owned by
// (player, unit_idx) via a dedicated cfg_unit proto slot -- callers that want gate 1 to FAIL
// overwrite move_op_code afterward.
unit &make_ground_unit(sim_fixture &fx, uint32_t player, int32_t unit_idx, int32_t proto_id, uint8_t x,
                       uint8_t y) {
    unit &u                             = fx.u((int32_t)player, unit_idx);
    u.unit_proto_id                     = (uint16_t)proto_id;
    u.x                                 = x;
    u.y                                 = y;
    fx.cfg_units[proto_id].move_op_code = 0xf;
    return u;
}

void ck_call_order(const std::vector<call_kind> &want, const char *what) {
    ck_eq((uint32_t)g_call_order.size(), (uint32_t)want.size(), what);
    const size_t n = g_call_order.size() < want.size() ? g_call_order.size() : want.size();
    for (size_t i = 0; i < n; ++i) {
        ck_eq((uint32_t)g_call_order[i], (uint32_t)want[i], what);
    }
}

} // namespace

void run_order_issue_0xf_adjacent_tests() {
    sim_fixture fx;

    constexpr uint32_t PLAYER   = 2;
    constexpr int32_t  UNIT_IDX = 5;
    constexpr int32_t  PROTO_ID = 7;

    // =================================================================================================
    // T1 -- GUARD 1: move_op_code != 0xf (not a ground mover). by_offset's own coordinate arithmetic
    // (0x0046a1be-0x0046a205) still runs unconditionally inside by_offset's body, but enqueue's class
    // test (0x0046a366 CMP / 0x0046a36d JNZ) fails BEFORE tiles_adjacent is ever called, so ALL FIVE
    // callees see zero invocations -- by_offset's masked-coordinate work is entirely discarded here,
    // unobservable through anything this oracle can see.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 17, 42);
        fx.cfg_units[PROTO_ID].move_op_code = 0x10; // not 0xf -- gate 1 fails

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/3,
                                                   /*dy=*/9);

        ck_eq((uint32_t)g_call_order.size(), 0u,
              "T1: move_op_code!=0xf -- zero calls to any of the 5 callees, 0x0046a366/0x0046a36d");
    }

    // =================================================================================================
    // T2 -- GUARD 2: gate 1 passes, but tiles_adjacent(...) returns 0 (not adjacent). This is the
    // window onto by_offset's DISCARDED-but-still-computed coordinates: tiles_adjacent is called with
    // by_offset's MASKED new_x/new_y (not the raw, unwrapped sum) as its first two arguments, then the
    // gate fails (0x0046a3b2 TEST / 0x0046a3b4 JZ) before scratch/enqueue/notify ever fire. x wraps
    // past the width edge (254+10=264 raw, masked to 8) specifically so the assertion below can tell
    // "the masked value" (8) from "the raw value" (264) at a glance.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 254, 5);
        g_tiles_adjacent_result = 0;

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/10,
                                                   /*dy=*/2);

        ck_eq((uint32_t)g_call_order.size(), 1u,
              "T2: tiles_adjacent==0 -- exactly ONE call total (tiles_adjacent itself), then the gate "
              "at 0x0046a3b4 discards everything else");
        if (g_call_order.size() == 1) {
            ck_eq((uint32_t)g_call_order[0], (uint32_t)call_kind::TILES_ADJACENT,
                  "T2: the one call that fires is tiles_adjacent, 0x0046a3ad");
        }
        ck_eq((uint32_t)g_tiles_adjacent_calls.size(), 1u, "T2: tiles_adjacent called exactly once");
        if (!g_tiles_adjacent_calls.empty()) {
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x1, 8u,
                  "T2: tiles_adjacent x1 == new_x == 0xff & (254+10) == 8, the MASKED value not the "
                  "raw 264, 0x0046a1d8-0x0046a1e0/0x0046a3aa");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y1, 7u,
                  "T2: tiles_adjacent y1 == new_y == 0x3f & (5+2) == 7, 0x0046a1fd-0x0046a205/0x0046a3a7");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x2, 254u,
                  "T2: tiles_adjacent x2 == unit.x (unmasked, the unit's OWN tile), 0x0046a3a0");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y2, 5u,
                  "T2: tiles_adjacent y2 == unit.y (unmasked), 0x0046a386");
        }
        ck_eq((uint32_t)g_reset_calls, 0u, "T2: order_scratch_reset never fires, 0x0046a3b4 discards it");
        ck_eq((uint32_t)g_set_field_calls.size(), 0u, "T2: order_scratch_set_field never fires");
        ck_eq((uint32_t)g_enqueue_calls.size(), 0u, "T2: order_enqueue never fires");
        ck_eq((uint32_t)g_notify_calls.size(), 0u, "T2: unit_notify_status never fires");
    }

    // =================================================================================================
    // T3 -- HAPPY PATH, plain in-range step on both axes (no wrap): the baseline full 6-invocation
    // sequence and every argument at every one of enqueue's 5 outward callees, so a later wrap-focused
    // case only needs to re-check the coordinate-dependent arguments.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 10, 20);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/5,
                                                   /*dy=*/7);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T3: full 6-invocation order [tiles_adjacent, reset, set_field x2, enqueue, "
                      "notify_status], 0x0046a3ad-0x0046a3f6");
        if (g_tiles_adjacent_calls.size() == 1) {
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x1, 15u,
                  "T3: new_x = 0xff & (10+5) = 15, 0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y1, 27u,
                  "T3: new_y = 0x3f & (20+7) = 27, 0x0046a1e3-0x0046a205");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x2, 10u,
                  "T3: tiles_adjacent x2 == unit.x (10), 0x0046a3a0");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y2, 20u,
                  "T3: tiles_adjacent y2 == unit.y (20), 0x0046a386");
        }
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].index, 0u, "T3: 1st set_field index==0, 0x0046a3be");
            ck_eq((uint32_t)g_set_field_calls[0].value, 15u,
                  "T3: 1st set_field value==new_x==15, 0x0046a3bb");
            ck_eq((uint32_t)g_set_field_calls[1].index, 1u, "T3: 2nd set_field index==1, 0x0046a3c8");
            ck_eq((uint32_t)g_set_field_calls[1].value, 27u,
                  "T3: 2nd set_field value==new_y==27, 0x0046a3c5");
        }
        if (g_enqueue_calls.size() == 1) {
            ck_eq((uint32_t)g_enqueue_calls[0].unit_index, (uint32_t)UNIT_IDX,
                  "T3: order_enqueue unit_index == unit_idx pass-through, 0x0046a3e4");
            ck_eq((uint32_t)g_enqueue_calls[0].owner_and_kind, (PLAYER | 0x80u),
                  "T3: order_enqueue owner_and_kind == player|ORDER_KIND_UNIT(0x80) == 0x82, "
                  "0x0046a3dc-0x0046a3e1");
            ck_eq((uint32_t)(uint16_t)g_enqueue_calls[0].param0, 0xfu,
                  "T3: order_enqueue param0 == 0xf LITERAL, 0x0046a3d7");
            ck_eq((uint32_t)g_enqueue_calls[0].order_code, 0x36u,
                  "T3: order_enqueue order_code == 0x36 LITERAL, 0x0046a3d2");
        }
        if (g_notify_calls.size() == 1) {
            ck_eq(g_notify_calls[0].player, PLAYER,
                  "T3: unit_notify_status player pass-through, 0x0046a3f2");
            ck_eq((uint32_t)g_notify_calls[0].unit_index, (uint32_t)UNIT_IDX,
                  "T3: unit_notify_status unit_index pass-through, 0x0046a3ef");
            ck_eq(g_notify_calls[0].status_code, 0u,
                  "T3: unit_notify_status status_code == 0 LITERAL, 0x0046a3ed");
        }
    }

    // =================================================================================================
    // T4 -- POSITIVE OVERFLOW wrapping past the X high edge (250+206=456 raw > 256), chosen so the
    // masked result (200) also DISCRIMINATES an axis/mask swap: 456 & width_mask(0xff) == 200, but
    // 456 & height_mask(0x3f) == 8 -- a translation that used the wrong mask for this axis fails here,
    // not just on the wrap itself. y stays in range (no wrap) on the other axis.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 250, 10);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/206,
                                                   /*dy=*/4);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T4: full 6-invocation order preserved on a wrapping X, 0x0046a3ad-0x0046a3f6");
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].value, 200u,
                  "T4: new_x = 0xff & (250+206=456) = 200 (positive wrap past width; would be 8 if "
                  "width/height masks were swapped), 0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_set_field_calls[1].value, 14u,
                  "T4: new_y = 0x3f & (10+4) = 14, no wrap, 0x0046a1e3-0x0046a205");
        }
    }

    // =================================================================================================
    // T5 -- POSITIVE OVERFLOW wrapping past the Y high edge (61+10=71 raw > 64), same
    // axis-discrimination shape as T4 but on the other axis: 71 & height_mask(0x3f) == 7, but
    // 71 & width_mask(0xff) == 71 -- a swapped mask fails here too. x stays in range (110, and
    // 110 & 0x3f == 46 != 110, so x also discriminates a wrong-mask application even though it
    // doesn't wrap).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 100, 61);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/10,
                                                   /*dy=*/10);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T5: full 6-invocation order preserved on a wrapping Y, 0x0046a3ad-0x0046a3f6");
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].value, 110u,
                  "T5: new_x = 0xff & (100+10) = 110, no wrap, 0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_set_field_calls[1].value, 7u,
                  "T5: new_y = 0x3f & (61+10=71) = 7 (positive wrap past height; would be 71 if "
                  "width/height masks were swapped), 0x0046a1e3-0x0046a205");
        }
    }

    // =================================================================================================
    // T6 -- NEGATIVE dx wraps X below zero. The add is unsigned 32-bit (per the .asm's MOVZX-then-ADD
    // order): 5 + (uint32_t)(-10) = 0xFFFFFFFB, masked to 0xff & 0xFFFFFFFB = 251 -- exactly the two's
    // complement wrap a signed-vs-unsigned slip in the translation would get wrong (a naive signed
    // clamp/max(0,...) would instead floor at 0). Also discriminates the mask: 0xFFFFFFFB & 0x3f == 59,
    // != 251, so a swapped mask fails here too.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 5, 30);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/-10,
                                                   /*dy=*/3);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T6: full 6-invocation order preserved on a negative-dx wrap, 0x0046a3ad-0x0046a3f6");
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].value, 251u,
                  "T6: new_x = 0xff & (uint32_t(5) + uint32_t(-10)) = 0xff & 0xFFFFFFFB = 251 -- UNSIGNED "
                  "add-then-mask two's-complement wrap, not a signed clamp, 0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_set_field_calls[1].value, 33u,
                  "T6: new_y = 0x3f & (30+3) = 33, no wrap, 0x0046a1e3-0x0046a205");
        }
    }

    // =================================================================================================
    // T7 -- NEGATIVE dy wraps Y below zero, same unsigned two's-complement shape as T6 but on the
    // other axis: 2 + (uint32_t)(-5) = 0xFFFFFFFD, masked to 0x3f & 0xFFFFFFFD = 61. Discriminates the
    // mask the other way too: 0xFFFFFFFD & 0xff == 253, != 61.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        make_ground_unit(fx, PLAYER, UNIT_IDX, PROTO_ID, 40, 2);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER, UNIT_IDX, /*dx=*/8,
                                                   /*dy=*/-5);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T7: full 6-invocation order preserved on a negative-dy wrap, 0x0046a3ad-0x0046a3f6");
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].value, 48u,
                  "T7: new_x = 0xff & (40+8) = 48, no wrap, 0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_set_field_calls[1].value, 61u,
                  "T7: new_y = 0x3f & (uint32_t(2) + uint32_t(-5)) = 0x3f & 0xFFFFFFFD = 61 -- UNSIGNED "
                  "add-then-mask two's-complement wrap, 0x0046a1e3-0x0046a205");
        }
    }

    // =================================================================================================
    // T8 -- BOTH axes wrap simultaneously, under a SECOND, DISTINCT (width,height) mask pair
    // (0x1ff/0x7f, i.e. dims 512x128, both != T1-T7's 256x64) and a DIFFERENT (player, unit_idx, proto)
    // triple, with dx != dy throughout -- the combined "nothing hardcoded, nothing swapped" case.
    // Chosen so BOTH axes discriminate a mask/axis swap: 700 & 0x1ff == 188 but 700 & 0x7f == 60;
    // 150 & 0x7f == 22 but 150 & 0x1ff == 150. Full 6-invocation sequence + every argument re-checked
    // (not just the coordinates) so a hardcoded PLAYER/UNIT_IDX/proto from an earlier case can't hide.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.geom.width_mask           = 0x1ff; // dim 512, DISTINCT from T1-T7's 0xff and from height_mask below
        fx.geom.height_mask          = 0x7f;  // dim 128, DISTINCT from T1-T7's 0x3f and from width_mask above
        constexpr uint32_t PLAYER2   = 4;
        constexpr int32_t  UNIT_IDX2 = 11;
        constexpr int32_t  PROTO2    = 22;
        make_ground_unit(fx, PLAYER2, UNIT_IDX2, PROTO2, 200, 100);

        detail::order_issue_0xf_adjacent_by_offset(fx.view(), g_calls, PLAYER2, UNIT_IDX2, /*dx=*/500,
                                                   /*dy=*/50);

        ck_call_order({call_kind::TILES_ADJACENT, call_kind::SCRATCH_RESET, call_kind::SET_FIELD,
                       call_kind::SET_FIELD, call_kind::ENQUEUE, call_kind::NOTIFY_STATUS},
                      "T8: full 6-invocation order under a distinct mask pair + distinct identity, "
                      "0x0046a3ad-0x0046a3f6");
        if (g_tiles_adjacent_calls.size() == 1) {
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x1, 188u,
                  "T8: new_x = 0x1ff & (200+500=700) = 188 (would be 60 under a swapped/wrong mask), "
                  "0x0046a1be-0x0046a1e0");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y1, 22u,
                  "T8: new_y = 0x7f & (100+50=150) = 22 (would be 150 under a swapped/wrong mask), "
                  "0x0046a1e3-0x0046a205");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].x2, 200u,
                  "T8: tiles_adjacent x2 == unit.x (200), 0x0046a3a0");
            ck_eq((uint32_t)g_tiles_adjacent_calls[0].y2, 100u,
                  "T8: tiles_adjacent y2 == unit.y (100), 0x0046a386");
        }
        if (g_set_field_calls.size() == 2) {
            ck_eq((uint32_t)g_set_field_calls[0].value, 188u, "T8: set_field(0, new_x==188), 0x0046a3bb");
            ck_eq((uint32_t)g_set_field_calls[1].value, 22u, "T8: set_field(1, new_y==22), 0x0046a3c5");
        }
        if (g_enqueue_calls.size() == 1) {
            ck_eq((uint32_t)g_enqueue_calls[0].unit_index, (uint32_t)UNIT_IDX2,
                  "T8: order_enqueue unit_index == 11, not T3's UNIT_IDX -- catches a hardcoded index, "
                  "0x0046a3e4");
            ck_eq((uint32_t)g_enqueue_calls[0].owner_and_kind, (PLAYER2 | 0x80u),
                  "T8: order_enqueue owner_and_kind == 4|0x80 == 0x84, 0x0046a3dc-0x0046a3e1");
        }
        if (g_notify_calls.size() == 1) {
            ck_eq(g_notify_calls[0].player, PLAYER2,
                  "T8: unit_notify_status player == 4, not T3's PLAYER -- catches a hardcoded player, "
                  "0x0046a3f2");
            ck_eq((uint32_t)g_notify_calls[0].unit_index, (uint32_t)UNIT_IDX2,
                  "T8: unit_notify_status unit_index == 11, 0x0046a3ef");
        }
    }
}

} // namespace mh::sim::test
