//
// issue_selftest.cpp -- `net_selftest.exe issuetest`: the ORDER-ISSUE domain's oracle (O4-0).
//
// WHAT THIS PROVES, and why the domain had nothing until now.
//
// `orderstest` is the order CONTAINER's oracle: it proves the container puts a record where it
// belongs. An order WRAPPER's job is one step upstream -- decide whether to emit, pack the
// arguments, pick the lane -- and nothing proved that a reimplemented wrapper packs the SAME order
// as the original. That is the only thing these 112 functions do, so under Law 5 none of them could
// be promoted. This suite is the missing half.
//
// THE DESIGN: A GENERATED GOLDEN, EVALUATED.
//
// The expected values are NOT written here. `order_issue_golden.gen.h` is extracted by
// tools/gen_order_issue_golden.py from the ORIGINAL binary's disassembly -- per call site, the four
// container arguments and the scratch writes, each as a small EXPRESSION over the wrapper's own
// parameters (`owner_and_kind = movzx16(or8(param0, 0x40))`), plus the lane. The suite evaluates
// those expressions for concrete inputs and compares against what our C++ actually emitted.
//
// That matters for two reasons:
//   1. INDEPENDENCE. Hand-written expectations are authored by the same reading that authored the
//      translation, so they agree with it by construction. The golden comes from the listing, and
//      the generator cross-checks itself against a second, decompiler-pcode reading of the same
//      sites (tools/data/order_matrix_raw.json) and refuses to emit on disagreement.
//   2. IT SCALES TO THE WHOLE DOMAIN. Adding a wrapper to this suite is one row in PILOTS below,
//      not a new set of hand-derived constants. O4A/B/C inherit the oracle rather than re-authoring
//      it 106 times.
//
// WHAT IT DOES NOT COVER, stated so it is not mistaken for coverage:
//   * The GUARD PREDICATE is not in the golden. The extractor models registers and stack slots, not
//     game memory, so "emits only when buildings[..].online_state != 0" is not something it can
//     derive. The negative cases below (guard rejects => nothing emitted at all) are therefore
//     HAND-AUTHORED per wrapper, and they are the one part of this suite that shares an author with
//     the translation. They are still worth having -- a wrapper that dropped its guard entirely
//     would emit on the reject case and fail -- but they are weaker evidence than the positive ones.
//   * Any field the extractor could not resolve is reported UNPINNED and skipped, never silently
//     passed. The count is printed; a rise in it is a regression in the golden, not in the code.
//
#include "orders/issue/issue_state.h"

// The per-unit headers of the O4A-C sweep. issue_state.h declares only O4-0's seven pilot
// wrappers; each translation unit declares its own, which is what keeps that file from being
// the serialisation point of every translation. A unit missing from this list fails to compile
// here rather than silently going undriven -- which is the failure the suite's own anti-vacuity
// check would otherwise have to catch after the fact.
#include "orders/issue/issue_ai_move_primitives.h"
#include "orders/issue/issue_bldg_depart.h"
#include "orders/issue/issue_bldg_footprint.h"
#include "orders/issue/issue_bldg_orders_b.h"
#include "orders/issue/issue_group_formation_move.h"
#include "orders/issue/issue_group_orders.h"
#include "orders/issue/issue_order_admin.h"
#include "orders/issue/issue_order_debug.h"
#include "orders/issue/issue_order_player.h"
#include "orders/issue/issue_turret_orders.h"
#include "orders/issue/issue_unit_attack.h"
#include "orders/issue/issue_unit_move.h"
#include "orders/issue/issue_unit_storage.h"

#include "order_issue_golden.gen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <limits>
#include <vector>

namespace {

int g_checks, g_fails;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

using mh::orders::issue::issue_view;
using mh::orders::issue::order_sink;
using building     = mh::game::mh_map_object_building;
using cfg_bldg     = mh::game::mh_cfg_final_struct_Building;
using unit         = mh::game::mh_map_object_unit;
using cfg_unit     = mh::game::mh_cfg_final_struct_Unit;
using storage_slot = mh::game::mh_map_object_unit_storage;
using map_geom     = mh::game::mh_llm_strat_map_geom;
using ctrl_group   = mh::game::mh_llm_strat_ctrl_group;
namespace golden   = mh::orders::issue::golden;

// ---- the recorder --------------------------------------------------------------------------------
//
// `order_sink` holds plain function pointers (it has to: in production they are calls into the
// promoted container), so the recorder is a file-scope singleton rather than a capture.

enum ev_kind { EV_RESET,
               EV_SET,
               EV_DISPATCH,
               EV_ENQUEUE };

struct ev {
    ev_kind k;
    int32_t a, b, c, d;
};

std::vector<ev> g_ev;

const order_sink &recording_sink() {
    static const order_sink s = {
        []() { g_ev.push_back(ev{EV_RESET, 0, 0, 0, 0}); },
        [](int32_t i, int32_t v) { g_ev.push_back(ev{EV_SET, i, v, 0, 0}); },
        [](uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) -> int32_t {
            g_ev.push_back(ev{EV_DISPATCH, (int32_t)unit_id, (int32_t)player, (int32_t)op_code,
                              (int32_t)arg});
            return 1;
        },
        [](uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
           uint16_t order_code) -> int32_t {
            g_ev.push_back(ev{EV_ENQUEUE, (int32_t)unit_index, (int32_t)owner_and_kind,
                              (int32_t)(uint16_t)param0, (int32_t)order_code});
            return 1;
        },
    };
    return s;
}

// ---- the outward-call recorder -------------------------------------------------------------------
//
// ADDED BY THE O4A-C SWEEP. The pilot's six wrappers called nothing outward, so `issue_calls` was an
// empty placeholder and the suite needed no mock. The sweep's rows call fifteen originals, none of
// which exists inside net_selftest.exe -- their addresses are game VAs and are unmapped here -- so a
// body that reached one directly would FAULT rather than fail. Every member is stubbed.
//
// THE STUB RETURNS ARE PROGRAMMABLE, not fixed. A query forced to one constant sends every case down
// the same branch, and a suite that only ever walks one arm passes vacuously on the others; so each
// answer is a field a case can set before it drives the wrapper. The DEFAULTS are deliberately the
// values that make a guard REJECT where there is a choice, so a case that forgot to seed gets an
// empty emission it can see rather than a plausible one it cannot.

struct call_log {
    // what was called, in order, as "<member>(<args>)" -- compared by cases that care about order
    std::vector<std::string> seq;

    // programmable answers
    uint32_t rand_next           = 0; // c.rand_below_fx returns this, then increments `rand_calls`
    int32_t  target_class_ret    = 0;
    int32_t  is_boarding_ret     = 0;
    uint32_t in_weapon_range_ret = 0;
    uint8_t  select_weapon_ret   = 0;
    int32_t  storage_accepts_ret = 0;
    int32_t  placement_check_ret = 0;
    // out-pointer helpers write these; distinct per helper so consuming the wrong pair fails
    int32_t unit_coords[2]      = {0, 0};
    int32_t bldg_coords[2]      = {0, 0};
    int32_t approach_tile[2]    = {0, 0};
    int32_t placement_corner[2] = {0, 0};
    int32_t tile_delta[2]       = {0, 0};

    // llm_strat_ai_group_move_formation_rotating's emissions. Its sink is
    // llm_strat_unit_order_move_enqueue, ABOVE the container, so nothing reaches `recording_sink`
    // and g_ev stays empty for it -- this vector is where its orders are observed. Every argument is
    // kept (not just a count): the seq id is the field the tail's skip-zero stamp is about, and x/y
    // are the pair a swap would hide.
    struct move_enqueue_call {
        uint16_t player;
        int32_t  unit_idx;
        uint32_t x, y, move_flag;
    };
    std::vector<move_enqueue_call> move_enqueues;

    // Set by the formation-mover cases only. When non-null the move-enqueue recorder reads the unit
    // it was handed AT CALL TIME, which is the only way to see that the two byte stamps happen
    // BEFORE the enqueue rather than after it -- a claim about order needs a case that can see the
    // order, and re-reading the unit after the wrapper returns cannot.
    const unit *observe_units          = nullptr;
    int32_t     observed_notify_status = -1;
    int32_t     observed_status_flags  = -1;

    int     rand_calls  = 0;
    int     snd_calls   = 0;
    int32_t last_snd_id = -1, last_snd_volume = -1;

    void reset() { *this = call_log(); }
    bool called(const char *what) const {
        for (const std::string &e : seq)
            if (e.compare(0, std::strlen(what), what) == 0) return true;
        return false;
    }
    int count(const char *what) const {
        int n = 0;
        for (const std::string &e : seq)
            if (e.compare(0, std::strlen(what), what) == 0) ++n;
        return n;
    }
};

call_log g_calls;

void logf(const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_calls.seq.push_back(buf);
}

const mh::orders::issue::issue_calls &recording_calls() {
    using mh::orders::issue::issue_calls;
    static const issue_calls c = {
        [](int32_t id, int32_t vol) {
            ++g_calls.snd_calls;
            g_calls.last_snd_id     = id;
            g_calls.last_snd_volume = vol;
            logf("snd_play(%d,%d)", id, vol);
        },
        [](uint32_t bound) -> int32_t {
            ++g_calls.rand_calls;
            logf("rand_below_fx(%u)", bound);
            return (int32_t)g_calls.rand_next;
        },
        [](uint32_t f, int32_t slot) -> int32_t {
            logf("target_class(%u,%d)", f, slot);
            return g_calls.target_class_ret;
        },
        [](int32_t st) -> int32_t {
            logf("unit_state_is_boarding(%d)", st);
            return g_calls.is_boarding_ret;
        },
        [](int32_t a, int32_t b, int32_t c2, int32_t d, int32_t e) -> uint32_t {
            logf("unit_in_weapon_range(%d,%d,%d,%d,%d)", a, b, c2, d, e);
            return g_calls.in_weapon_range_ret;
        },
        [](uint16_t p, int32_t u, uint32_t m) -> uint8_t {
            logf("unit_select_weapon(%u,%d,%u)", p, u, m);
            return g_calls.select_weapon_ret;
        },
        [](uint32_t b, uint16_t u) -> int32_t {
            logf("storage_type_accepts_unit(%u,%u)", b, u);
            return g_calls.storage_accepts_ret;
        },
        [](int32_t x, int32_t y, int32_t b) -> int32_t {
            logf("bldg_placement_check_and_preview(%d,%d,%d)", x, y, b);
            return g_calls.placement_check_ret;
        },
        [](uint16_t p, int32_t u, int32_t *ox, int32_t *oy) {
            logf("unit_get_coords(%u,%d)", p, u);
            *ox = g_calls.unit_coords[0];
            *oy = g_calls.unit_coords[1];
        },
        [](uint16_t p, int32_t b, int32_t *ox, int32_t *oy) {
            logf("bldg_get_coords(%u,%d)", p, b);
            *ox = g_calls.bldg_coords[0];
            *oy = g_calls.bldg_coords[1];
        },
        [](uint16_t a, uint16_t b, uint32_t *ox, uint32_t *oy, uint32_t m) {
            logf("storage_get_approach_tile(%u,%u,%u)", a, b, m);
            *ox = g_calls.approach_tile[0];
            *oy = g_calls.approach_tile[1];
        },
        [](uint16_t u, int32_t cx, int32_t cy, uint32_t *oc, uint32_t *orow) {
            logf("bldg_calc_placement_corner_from_center(%u,%d,%d)", u, cx, cy);
            *oc   = g_calls.placement_corner[0];
            *orow = g_calls.placement_corner[1];
        },
        [](int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *odx, int32_t *ody) {
            logf("tile_delta_wrapped(%d,%d,%d,%d)", x1, y1, x2, y2);
            *odx = g_calls.tile_delta[0];
            *ody = g_calls.tile_delta[1];
        },
        [](uint32_t p, int32_t u, uint32_t st) { logf("unit_notify_status(%u,%d,%u)", p, u, st); },
        [](uint16_t p, int32_t u, uint32_t x, uint32_t y, uint32_t mf) {
            g_calls.move_enqueues.push_back({p, u, x, y, mf});
            if (g_calls.observe_units && g_calls.observed_notify_status < 0) {
                const unit &seen               = g_calls.observe_units[(size_t)(p * mh::orders::issue::UNITS_PER_PLAYER + u)];
                g_calls.observed_notify_status = seen.order_notify_status;
                g_calls.observed_status_flags  = seen.order_status_flags;
            }
            logf("unit_order_move_enqueue(%u,%d,%u,%u,%u)", p, u, x, y, mf);
        },
        [](void *, const wchar_t *fmt, int32_t a0, int32_t a1) -> int32_t {
            // The format POINTER is the payload worth pinning: the two arms use different globals
            // out of the view, and swapping them is exactly the mistake a translation can make.
            logf("sprintf_ii(fmt=%p,%d,%d)", (const void *)fmt, a0, a1);
            return 0;
        },
    };
    return c;
}

// ---- the golden evaluator -------------------------------------------------------------------------
//
// A value is a flat node array whose LAST node is the result. Anything the extractor could not model
// is a G_UNKNOWN node, which poisons the whole expression to `known = false` -- that field is then
// reported unpinned rather than compared, so an unmodelled instruction can never masquerade as a
// passing comparison.

struct ev_result {
    int64_t v     = 0;
    bool    known = false;
};

int g_unpinned; // fields skipped because the golden could not resolve them

ev_result eval_node(const golden::g_val &val, int idx, const int32_t *params, int nparams) {
    ev_result r;
    if (idx < 0 || idx >= (int)val.n) return r;
    const golden::g_node &n = val.nodes[idx];

    if (n.op == golden::G_UNKNOWN) return r;
    if (n.op == golden::G_IMM) return ev_result{(int64_t)n.imm, true};
    if (n.op == golden::G_PARAM) {
        if (n.imm < 0 || n.imm >= nparams) return r;
        return ev_result{(int64_t)params[n.imm], true};
    }

    ev_result a = eval_node(val, n.a, params, nparams);
    if (!a.known) return r;

    // A binary op either names a second node in `b`, or carries its constant operand in `imm`.
    int64_t rhs = (int64_t)n.imm;
    if (n.b >= 0) {
        ev_result b = eval_node(val, n.b, params, nparams);
        if (!b.known) return r;
        rhs = b.v;
    }

    const uint32_t ua = (uint32_t)a.v;
    switch (n.op) {
        case golden::G_OR: return ev_result{(int64_t)(int32_t)(ua | (uint32_t)rhs), true};
        // `OR AL,imm` -- an EIGHT-BIT or on the low byte, leaving the upper 24 bits alone. Modelled
        // exactly, not as a 32-bit or: the two differ whenever the constant has bits above bit 7, and
        // writing it the easy way would make the model agree with a wrong translation.
        case golden::G_OR8: {
            const uint32_t low = (ua | (uint32_t)rhs) & 0xffu;
            return ev_result{(int64_t)(int32_t)((ua & 0xffffff00u) | low), true};
        }
        case golden::G_AND: return ev_result{(int64_t)(int32_t)(ua & (uint32_t)rhs), true};
        case golden::G_ADD: return ev_result{(int64_t)(int32_t)(ua + (uint32_t)rhs), true};
        case golden::G_SUB: return ev_result{(int64_t)(int32_t)(ua - (uint32_t)rhs), true};
        case golden::G_IMUL: return ev_result{(int64_t)(int32_t)((int32_t)a.v * (int32_t)rhs), true};
        case golden::G_SHL: return ev_result{(int64_t)(int32_t)(ua << ((uint32_t)rhs & 31u)), true};
        case golden::G_SHR: return ev_result{(int64_t)(int32_t)(ua >> ((uint32_t)rhs & 31u)), true};
        case golden::G_SAR: return ev_result{(int64_t)((int32_t)a.v >> ((uint32_t)rhs & 31u)), true};
        case golden::G_MOVZX16: return ev_result{(int64_t)(ua & 0xffffu), true};
        case golden::G_MOVZX8: return ev_result{(int64_t)(ua & 0xffu), true};
        case golden::G_MOVSX16: return ev_result{(int64_t)(int32_t)(int16_t)(uint16_t)ua, true};
        case golden::G_MOVSX8: return ev_result{(int64_t)(int32_t)(int8_t)(uint8_t)ua, true};
        default: return r;
    }
}

ev_result eval(const golden::g_val &val, const int32_t *params, int nparams) {
    if (val.n == 0) return ev_result{};
    return eval_node(val, (int)val.n - 1, params, nparams);
}

// Compare one field, LOW 16 BITS ONLY -- that is the width of every one of the container's four
// identity parameters, so comparing wider would fail on register garbage the container never sees.
void check_field(const char *what, const golden::g_val &g, const int32_t *params, int nparams,
                 int32_t got) {
    ev_result e = eval(g, params, nparams);
    if (!e.known) {
        ++g_unpinned;
        return;
    }
    char msg[320];
    snprintf(msg, sizeof msg, "%s: expected 0x%04x, got 0x%04x", what,
             (unsigned)((uint32_t)e.v & 0xffffu), (unsigned)((uint32_t)got & 0xffffu));
    check(msg, ((uint32_t)e.v & 0xffffu) == ((uint32_t)got & 0xffffu));
}

// ---- the fixture ----------------------------------------------------------------------------------

// GROWN TO THE WHOLE VIEW BY THE O4A-C SWEEP. Two rules from sim_test_support.h's banner apply here
// verbatim and are the reason the seeds below look the way they do:
//   * seed DISTINCT, non-symmetric values -- two fields holding the same number make a swap pass;
//   * size the vectors with PARENTHESES, not braces (`std::vector<T> v(n)` is n elements,
//     `std::vector<T> v{n}` is ONE element whose value is n).
struct fixture {
    std::vector<building>     buildings;
    std::vector<cfg_bldg>     cfg_buildings;
    std::vector<unit>         units;
    std::vector<cfg_unit>     cfg_units;
    std::vector<storage_slot> unit_storage;
    std::vector<ctrl_group>   ctrl_groups;
    map_geom                  geom{};

    uint16_t player_side   = 0;
    int32_t  session_mode  = 0;
    int32_t  planet_index  = 0;
    int32_t  player_race   = 0;
    double   game_clock    = 0.0;
    uint8_t  key_lalt_held = 0;

    double  ack_voice_cooldown_sec      = 2.0;                                  // the game's own value, 0x0050019e
    int32_t ack_voice_snd_id_by_race[6] = {0x10, 0x12, 0x11, 0x22, 0x24, 0x23}; // the game's own

    // The two debug format strings are compared BY POINTER, never dereferenced (the wrapper only
    // passes them on), so distinct dummies are enough -- and distinct is the point: they are what
    // catches a translation that used the clear arm's format on the set arm.
    wchar_t fmt_clear[2] = {L'c', 0};
    wchar_t fmt_set[2]   = {L's', 0};
    wchar_t text_tmp[512]{};

    // The AI group-relocation scratch the formation mover CONSUMES. Sized to the region's real
    // extent (int32[100]) so an over-run in a body under test lands here rather than in the next
    // member, and held as a signed count because the region is. NOTE there is deliberately NO
    // negative-count case: the original's compare is UNSIGNED (JC @0x004d7e09), so a count of -1
    // reads as 0xffffffff and BOTH arms would run four billion iterations -- the property is real
    // and reproduced in the translation, but it is not one a test can drive. The AI harvester that
    // fills this list only ever counts up from 0.
    int32_t ai_group_scratch_list[100]{};
    int32_t ai_group_scratch_count = 0;

    // The passable-tile spiral search llm_strat_ai_group_scatter_to_passable_tile walks. Held at the
    // REGION'S REAL SHAPE so an over-run lands here rather than in a neighbour: the plane is
    // byte[256][256] indexed (x << 8) | y, and the masks are width-1 / height-1 (NOT the widths --
    // the body does `AND candidate, [width_m]`). Both masks default to 0xff so the whole 256x256
    // plane is addressable; a test that wants wrapping lowers them.
    std::vector<uint8_t> passable = std::vector<uint8_t>(256 * 256, 0);
    // (dx, dy) SIGNED byte pairs, so the stride is 2 and the body indexes [s*2] / [s*2+1]. Sized
    // generously rather than to the region's 65536 entries: the interesting cases walk a handful.
    std::vector<int8_t> ai_tile_spiral_offsets = std::vector<int8_t>(2 * 256, 0);
    uint32_t            width_m                = 0xffu;
    uint32_t            height_m               = 0xffu;

    uint8_t order_seq_id_by_player[mh::orders::issue::MAX_PLAYERS]{};
    uint8_t player_control_mask         = 0;
    int32_t ui_depart_pending_bldg_a    = 0;
    int32_t ui_depart_pending_bldg_b    = 0;
    int32_t ui_planet_sel_action_target = 0;
    double  ack_voice_last_play_time    = 0.0;
    int32_t ack_voice_suppressed_count  = 0;

    fixture()
        : buildings((size_t)(mh::orders::issue::MAX_PLAYERS *
                             mh::orders::issue::BUILDINGS_PER_PLAYER)),
          cfg_buildings(100),
          // 32 players, not MAX_PLAYERS(8), and ONLY this array. unit_flag_and_move masks the
          // player to 4 bits TWICE (AND EAX,0xf @0x004d7e35 for the stride, AND ESI,0xf
          // @0x004d7e5a for the argument), so the mask case drives player 0x13. Sized to 8 that
          // is an out-of-bounds write and a dropped mask CRASHES the run -- red, but stdout is
          // lost on abnormal exit, so it does not even resemble a failed check (src/mh_dll/
          // README.md says exactly this about the plain build). Widened so the mutation lands in
          // a real slot and the NAMED stride check reports it. Every other case indexes < 8.
          units((size_t)(32 * mh::orders::issue::UNITS_PER_PLAYER)),
          cfg_units(100),
          // 8 * 100, matching the buildings/units convention rather than a tight fit. The
          // storage functions index this as player * UNIT_STORAGE_PER_PLAYER(25) +
          // storage_index and do so UNCONDITIONALLY, before any guard runs -- with INPUTS
          // reaching player 7 and storage_index 61 the worst case is 7*25+61 == 236, so the
          // previous size of 64 was an out-of-bounds read on most rows. Caught by the
          // oracle-row review before a single case ran, not by ASan afterwards.
          unit_storage((size_t)(mh::orders::issue::MAX_PLAYERS * 100)),
          ctrl_groups(10) {
        reset();
    }

    void reset() {
        std::memset(buildings.data(), 0, buildings.size() * sizeof(building));
        std::memset(cfg_buildings.data(), 0, cfg_buildings.size() * sizeof(cfg_bldg));
        std::memset(units.data(), 0, units.size() * sizeof(unit));
        std::memset(cfg_units.data(), 0, cfg_units.size() * sizeof(cfg_unit));
        std::memset(unit_storage.data(), 0, unit_storage.size() * sizeof(storage_slot));
        std::memset(ctrl_groups.data(), 0, ctrl_groups.size() * sizeof(ctrl_group));
        std::memset(&geom, 0, sizeof geom);
        std::memset(text_tmp, 0, sizeof text_tmp);
        std::memset(order_seq_id_by_player, 0, sizeof order_seq_id_by_player);
        std::memset(ai_group_scratch_list, 0, sizeof ai_group_scratch_list);
        ai_group_scratch_count = 0;
        std::fill(passable.begin(), passable.end(), (uint8_t)0);
        std::fill(ai_tile_spiral_offsets.begin(), ai_tile_spiral_offsets.end(), (int8_t)0);
        // 0xff, not 0: these are `dim - 1` masks and a zero mask collapses every coordinate to 0 --
        // the same value a DROPPED mask read produces, so the two failures would be
        // indistinguishable. Same reasoning as geom.bw_mask below.
        width_m  = 0xffu;
        height_m = 0xffu;

        // A power-of-two map, since the toroidal masks are `dim - 1` and a zero mask would make
        // every wrapped coordinate 0 -- which is the value a missing mask ALSO produces, so the two
        // failures would be indistinguishable.
        geom.bw_mask = 0x7ff;

        player_side                 = 0;
        session_mode                = 0;
        planet_index                = 0;
        player_race                 = 0;
        game_clock                  = 0.0;
        key_lalt_held               = 0;
        player_control_mask         = 0;
        ui_depart_pending_bldg_a    = 0;
        ui_depart_pending_bldg_b    = 0;
        ui_planet_sel_action_target = 0;
        ack_voice_last_play_time    = 0.0;
        ack_voice_suppressed_count  = 0;
        g_calls.reset();
    }

    building &at(int32_t player, int32_t index) {
        return buildings[(size_t)(player * mh::orders::issue::BUILDINGS_PER_PLAYER + index)];
    }
    unit &unit_at(int32_t player, int32_t index) {
        return units[(size_t)(player * mh::orders::issue::UNITS_PER_PLAYER + index)];
    }
    // The storage array's stride is 25, NOT 100 -- issue_unit_storage.cpp derives its own
    // UNIT_STORAGE_PER_PLAYER from the original's `IMUL EDX,EAX,0x17d4` (== 25 * 0xf4). Spelled
    // here too rather than shared, following the same each-TU-carries-the-idiom convention the
    // translation uses; the two would disagree loudly (an out-of-bounds index) rather than quietly.
    storage_slot &storage_at(int32_t player, int32_t index) {
        return unit_storage[(size_t)(player * 25 + index)];
    }

    issue_view view() {
        issue_view v{};
        v.buildings                   = buildings.data();
        v.cfg_buildings               = cfg_buildings.data();
        v.units                       = units.data();
        v.cfg_units                   = cfg_units.data();
        v.unit_storage                = unit_storage.data();
        v.geom                        = &geom;
        v.ctrl_groups                 = ctrl_groups.data();
        v.player_side                 = &player_side;
        v.session_mode                = &session_mode;
        v.planet_index                = &planet_index;
        v.player_race                 = &player_race;
        v.game_clock                  = &game_clock;
        v.key_lalt_held               = &key_lalt_held;
        v.ack_voice_cooldown_sec      = &ack_voice_cooldown_sec;
        v.ack_voice_snd_id_by_race    = ack_voice_snd_id_by_race;
        v.fmt_control_mode_clear      = fmt_clear;
        v.fmt_control_mode_set        = fmt_set;
        v.text_tmp                    = text_tmp;
        v.ai_group_scratch_list       = ai_group_scratch_list;
        v.ai_group_scratch_count      = &ai_group_scratch_count;
        v.passable                    = passable.data();
        v.ai_tile_spiral_offsets      = ai_tile_spiral_offsets.data();
        v.width_m                     = &width_m;
        v.height_m                    = &height_m;
        v.order_seq_id_by_player      = order_seq_id_by_player;
        v.player_control_mask         = &player_control_mask;
        v.ui_depart_pending_bldg_a    = &ui_depart_pending_bldg_a;
        v.ui_depart_pending_bldg_b    = &ui_depart_pending_bldg_b;
        v.ui_planet_sel_action_target = &ui_planet_sel_action_target;
        v.ack_voice_last_play_time    = &ack_voice_last_play_time;
        v.ack_voice_suppressed_count  = &ack_voice_suppressed_count;
        return v;
    }
};

// ---- the pilot table --------------------------------------------------------------------------------

using invoke_fn = void (*)(const issue_view &, const order_sink &, const int32_t *p);
using seed_fn   = void (*)(fixture &, const int32_t *p);

struct pilot {
    const char *wrapper;      // the ORIGINAL's name -- the golden's key
    invoke_fn   call;         //
    int         nparams;      //
    seed_fn     seed_pass;    // make the guard admit (nullptr = unguarded)
    seed_fn     seed_fail[3]; // make the guard reject; each must produce ZERO emissions
    const char *fail_why[3];  // what each rejection is testing, for the check message
    // How many orders the PASS seed must produce. 0 reads as 1, so the pilot rows below are
    // unchanged. Stated per row rather than inferred from the run, because "however many arrived"
    // would pass a wrapper that dropped one of its two emissions. A wrapper whose emission count
    // depends on state (session_mode == 3 adds a second) gets one row per count, with a seed that
    // pins the state -- not one row with a range.
    int emissions;
};

namespace api = mh::orders::issue::detail;

// Inputs are DISTINCT and non-symmetric on purpose: a wrapper that swapped two parameters, or that
// used the player where the index belongs, must produce a different answer rather than the same one.
//
// WIDENED 4 -> 6 BY THE O4A-C SWEEP. The pilot's widest wrapper took four parameters; the unit
// movers and attackers take five (EAX/EDX/EBX/ECX + one stack slot) and
// bldg_footprint_random_point takes six. A row whose nparams exceeds the row width would read
// whatever followed the array, so this is a correctness bound and not a convenience. The two new
// columns continue the distinct/non-symmetric rule -- no value repeats within a row, and no column
// is a constant across rows.
//
// COLUMN 0 IS A PLAYER SLOT AND COLUMN 1 AN OBJECT INDEX in almost every row, so both stay inside
// their arrays: 0..7 and 0..99. Widening those would index the fixture out of bounds, which ASan
// would (correctly) report as the SUITE's bug rather than the translation's.
const int32_t INPUTS[][6] = {
    {3, 42, 17, 9, 23, 6},
    {0, 1, 5, 88, 12, 34},
    {7, 99, 61, 2, 45, 19},
};

const pilot PILOTS[] = {
    {"llm_strat_bldg_order_activate",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_activate(v, s, (uint32_t)p[0], (uint16_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_deactivate_enqueue",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_deactivate_enqueue(v, s, (uint32_t)p[0], (uint16_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_production_add",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_production_add(v, s, (uint32_t)p[0], p[1], p[2]);
     },
     3,
     [](fixture &f, const int32_t *p) { f.at(p[0], p[1]).online_state = 1; },
     {[](fixture &f, const int32_t *p) { f.at(p[0], p[1]).online_state = 0; }, nullptr, nullptr},
     {"online_state == 0 must emit nothing (CMP word [..+0x17],0x0 / JZ, 0x0046db12)", nullptr,
      nullptr}},

    {"llm_strat_bldg_order_production_remove",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_production_remove(v, s, (uint32_t)p[0], p[1], p[2]);
     },
     3,
     [](fixture &f, const int32_t *p) { f.at(p[0], p[1]).state = 7; },
     {[](fixture &f, const int32_t *p) { f.at(p[0], p[1]).state = 100; },
      // The guard is `== 100`, NOT `>= 100` and not "under construction-ish". 99 and 101 must both
      // still emit -- that is what distinguishes the original's equality test from a range test a
      // reimplementation might reach for. Seeded here as a PASS case in the fail slot's place would
      // be confusing, so it is asserted separately below.
      nullptr, nullptr},
     {"state == 100 must emit nothing (CMP word [..+0xd],0x64 / JZ, 0x0046dcf0)", nullptr, nullptr}},

    {"llm_strat_bldg_order_load_resource",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_load_resource(v, s, (uint32_t)p[0], p[1], p[2], p[3]);
     },
     4,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_repair_cycle_start",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_repair_cycle_start(v, s, (uint32_t)p[0], p[1]);
     },
     2,
     [](fixture &f, const int32_t *p) {
         building &b               = f.at(p[0], p[1]);
         b.building_id             = 5;
         b.energy                  = 50.0;
         f.cfg_buildings[5].energy = 100.0;
         f.cfg_buildings[5].type   = 3;
     },
     {[](fixture &f, const int32_t *p) { // charge already at/above the type's full value
          building &b               = f.at(p[0], p[1]);
          b.building_id             = 5;
          b.energy                  = 100.0;
          f.cfg_buildings[5].energy = 100.0;
          f.cfg_buildings[5].type   = 3;
      },
      [](fixture &f, const int32_t *p) { // excluded type 0x22
          building &b               = f.at(p[0], p[1]);
          b.building_id             = 5;
          b.energy                  = 50.0;
          f.cfg_buildings[5].energy = 100.0;
          f.cfg_buildings[5].type   = 0x22;
      },
      [](fixture &f, const int32_t *p) { // excluded type 0x0e
          building &b               = f.at(p[0], p[1]);
          b.building_id             = 5;
          b.energy                  = 50.0;
          f.cfg_buildings[5].energy = 100.0;
          f.cfg_buildings[5].type   = 0x0e;
      }},
     {"energy EQUAL to cfg energy must emit nothing -- the compare is `<`, not `<=` (FCOMP/JNC, "
      "0x0046e386)",
      "cfg type 0x22 must emit nothing (CMP byte [..+0x8],0x22, 0x0046e3b1)",
      "cfg type 0x0e must emit nothing (CMP byte [..+0x8],0x0e, 0x0046e3da)"}},

    // The one batch-C row in the pilot, and it is here for coverage the six above cannot give:
    // param0 (0xf7) DIFFERS from order_code (0xf8), the owner is masked rather than OR'd so no kind
    // nibble is set, unit_index is a literal 0 rather than a parameter, and it is the only pilot
    // that calls scratch_reset -- so `scratch_reset_before` gets compared against TRUE somewhere.
    {"llm_strat_order_debug_kill_group",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_debug_kill_group(v, s, (uint32_t)p[0], p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    // ==============================================================================================
    // THE O4A-C SWEEP (2026-08-28) -- the remaining 52 wrappers of the domain.
    //
    // The seven rows ABOVE are O4-0's pilot and take the two-parameter detail:: form; every row
    // below takes the three-parameter one and passes recording_calls(), because the sweep populated
    // `issue_calls` from the whole domain's call-target scan. Rows are grouped by translation unit.
    //
    // WHERE A WRAPPER APPEARS TWICE, that is deliberate: its emission COUNT or its branch depends on
    // state, and one row per pinned state states the count instead of averaging it (the attack
    // family's session_mode==3 second emission, exit_storage's two mutually-exclusive arms).
    // ==============================================================================================

    // ======== unit(s): bldg_orders_b ========
    // ---- unit bldg_orders_b (O4A-C sweep) -----------------------------------------------------
    // All twelve golden-site rows below are UNCONDITIONAL dispatch wrappers: the .asm for each has no
    // guard at all (no TEST/CMP before the CALL to llm_strat_order_dispatch @0x00465fdf) -- confirmed
    // per-function against tmp/decomp_orders_issue/<name>_<addr>.asm. seed_pass/seed_fail are all
    // nullptr for that reason: there is no predicate to admit or reject.

    {"llm_strat_bldg_order_assign_workers", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_assign_workers(v, s, recording_calls(), (uint16_t)p[0], (uint16_t)p[1],
                                        (uint32_t)p[2]);
     },
     3,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_unassign_workers", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_unassign_workers(v, s, recording_calls(), (uint16_t)p[0], (uint16_t)p[1],
                                          (uint32_t)p[2]);
     },
     3,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_deactivate", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_deactivate(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_upgrade", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_upgrade(v, s, recording_calls(), (uint32_t)p[0], (uint32_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_hangar_recharge", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_hangar_recharge(v, s, recording_calls(), (uint32_t)p[0], p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_restart_construction", // Unguarded (no guard in the .asm). Parameter
                                                  // roles are swapped -- p[0] (target_id) becomes the
                                                  // OWNER, p[1] (player) becomes the unit_id; see the
                                                  // .cpp's header comment and golden N173-N176.
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_restart_construction(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_purge_dead_docked", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_purge_dead_docked(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_flush_cargo_hold", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_flush_cargo_hold(v, s, recording_calls(), (uint32_t)p[0], (uint32_t)p[1]);
     },
     2,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_load_passengers", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_load_passengers(v, s, recording_calls(), (uint16_t)p[0], (uint16_t)p[1],
                                         p[2]);
     },
     3,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_unload_passengers", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_unload_passengers(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1],
                                           p[2]);
     },
     3,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_unload_resource", // Unguarded (no guard in the .asm). Two scratch slots,
                                             // written 3 (resource_slot) then 2 (count).
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_unload_resource(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1],
                                         p[2], p[3]);
     },
     4,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_start_research_project", // Unguarded (no guard in the .asm)
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_start_research_project(v, s, recording_calls(), (uint32_t)p[0],
                                                (uint16_t)p[1], p[2]);
     },
     3,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    // ======== unit(s): bldg_depart ========
    {"llm_strat_bldg_order_shuttle_depart",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_shuttle_depart(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1], p[2]);
     },
     3,
     nullptr, // unguarded -- the .asm is straight-line from entry to the dispatch call, no CMP at all
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_shuttle_depart_enqueue",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_shuttle_depart_enqueue(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1],
                                                p[2]);
     },
     3,
     nullptr, // unguarded -- same straight-line shape as shuttle_depart, IMMEDIATE lane
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_port_depart",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_port_depart(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1], p[2]);
     },
     3,
     nullptr, // unguarded AT THIS FUNCTION'S OWN LEVEL -- the "not the current planet" CMP
              // (0x0046efc5/JZ 0x0046efcb) lives in the caller, depart_dispatch_by_type, not here
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_port_depart_enqueue",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_port_depart_enqueue(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1],
                                             p[2]);
     },
     3,
     nullptr, // unguarded at this function's own level, same as port_depart above
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_mother_depart",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_mother_depart(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1], p[2]);
     },
     3,
     nullptr, // unguarded -- straight-line, no CMP
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    {"llm_strat_bldg_order_mother_depart_enqueue",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::bldg_order_mother_depart_enqueue(v, s, recording_calls(), (uint32_t)p[0], (uint16_t)p[1],
                                               p[2]);
     },
     3,
     nullptr, // unguarded -- same straight-line shape, IMMEDIATE lane
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr}},

    // ======== unit(s): unit_move ========
    // ---- llm_strat_unit_order_move @0x00469fe6 --------------------------------------------------
    // GUARD: none. The primary dispatch always fires; the ONLY conditional is the MP-echo branch
    // (CMP dword [_G_LLM_GAME_SESSION_MODE],0x3 / JNZ, 0x0046a097), which adds a SECOND emission
    // rather than gating the first. seed_pass pins session_mode so that count is STATED, not
    // defaulted -- pinned to 3 so BOTH golden sites (primary dispatch @0x0046a092, RAW-player
    // 0xfb/0xfb echo @0x0046a0b7) get exercised.
    {"llm_strat_unit_order_move",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_move(v, s, recording_calls(), (uint32_t)p[0], p[1], p[2], p[3], p[4]);
     },
     5,
     [](fixture &f, const int32_t *p) {
         f.session_mode               = 3; // pins the second (0xfb/0xfb) emission ON -- emissions must be 2
         unit &u                      = f.unit_at(p[0], p[1]);
         u.unit_proto_id              = 37; // distinct cfg_units row (unit_proto_id -> cfg_units[proto])
         f.cfg_units[37].move_op_code = 0x21;
         f.cfg_units[37].move_op_arg  = 0x22;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_move_default @0x0046a402 ------------------------------------------
    // GUARD: none, same shape as unit_order_move (op_code is the constant 0x10, not proto-derived;
    // only move_op_arg comes off cfg_units). MP-echo branch at 0x0046a48d/0x0046a494.
    {"llm_strat_unit_order_move_default",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_move_default(v, s, recording_calls(), (uint32_t)p[0], p[1], p[2], p[3],
                                      p[4]);
     },
     5,
     [](fixture &f, const int32_t *p) {
         f.session_mode               = 3;
         unit &u                      = f.unit_at(p[0], p[1]);
         u.unit_proto_id              = 53; // distinct from the other rows' proto rows on purpose
         f.cfg_units[53].move_op_code = 0x33;
         f.cfg_units[53].move_op_arg  = 0x35;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_move_confirmed_with_bump @0x0046accf ------------------------------
    // GUARD: none on emission -- the primary dispatch (op_code CONSTANT 0x18) always fires; the
    // MP-echo branch at 0x0046ad4d/0x0046ad54 adds the second. The LOCAL-VIEWER bump/snd_play branch
    // (CMP AX,[PlayerSide], 0x0046ad85/0x0046ad8c) is a SEPARATE, non-emission effect (it calls
    // issue_calls, not order_sink) and is deliberately kept OFF here by pinning player_side to a
    // value no INPUTS row's p[0] can equal (the player column is only 0/3/7), so this PASS run's
    // snd_play/placement calls never fire and the emission comparison stays uncontaminated.
    // Its own coverage is the standalone block below.
    {"llm_strat_unit_order_move_confirmed_with_bump",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_move_confirmed_with_bump(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                                  p[2], p[3]);
     },
     4,
     [](fixture &f, const int32_t *p) {
         f.session_mode              = 3;
         f.player_side               = 250; // outside {0,3,7} -- keeps the bump/snd_play branch OFF for this row
         unit &u                     = f.unit_at(p[0], p[1]);
         u.unit_proto_id             = 61;
         f.cfg_units[61].move_op_arg = 0x3d; // op_code is the constant 0x18, never read from cfg
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_move_relative @0x0046c87a -----------------------------------------
    // GUARD: none on emission -- it always dispatches at least once. The near/far split
    // (CMP [chebyshev],0x1 / JLE 0x0046c928) picks WHICH order, not whether one is issued. Pinned to
    // the FAR branch (chebyshev >= 2) via the tile_delta_wrapped stub's programmable output, because
    // the stub's default {0,0} makes chebyshev == 0 -- i.e. an un-seeded row hits the NEAR branch,
    // whose golden site CANNOT BE MATCHED (see the finding recorded beside this file).
    {"llm_strat_unit_order_move_relative",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_move_relative(v, s, recording_calls(), (uint32_t)p[0], p[1], p[2], p[3]);
     },
     4,
     [](fixture &f, const int32_t *) {
         f.session_mode        = 3;
         g_calls.tile_delta[0] = -6; // |dx|=6
         g_calls.tile_delta[1] = 3;  // |dy|=3 -> chebyshev = max(6,3) = 6 >= 2 -> FAR branch
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_scatter_from_spawn @0x0046a626 ------------------------------------
    // GUARD: none -- unconditional enqueue + notify_status, and NO session-mode/MP-echo branch
    // exists in this function at all (confirmed against both the .asm and the .cpp's banner).
    // session_mode is pinned anyway, explicitly, so this row does not silently rely on the fixture
    // default for a field the unit's other four rows treat as load-bearing.
    {"llm_strat_unit_order_scatter_from_spawn",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_scatter_from_spawn(v, s, recording_calls(), (uint16_t)p[0], p[1], p[2],
                                            p[3]);
     },
     4,
     [](fixture &f, const int32_t *p) {
         f.session_mode              = 0; // explicit, though this wrapper never branches on it
         unit &u                     = f.unit_at(p[0], p[1]);
         u.unit_proto_id             = 71;
         f.cfg_units[71].move_op_arg = 0x47; // op_code is the constant 0x10
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    // ======== unit(s): unit_attack ========
    // ---- llm_strat_unit_order_attack_target @0x0046af1e -- UNGUARDED (every path ends in >=1
    // dispatch: the "otherwise" direct-fire shape @0x0046b149, the heli shape @0x0046b246, or the
    // "any other ground type" shape @0x0046b263 -- there is no reject-to-zero branch), so seed_fail
    // is all-nullptr. Two rows cover the ONLY two golden sites this harness can actually match --
    // see the GOLDEN-GENERATOR FINDING below.
    {"llm_strat_unit_order_attack_target",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_target(v, s, recording_calls(), (uint32_t)p[0], p[1], (uint32_t)p[2],
                                       p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (1 order): in_range == 0 sends every call straight to LAB_0046b10d/@0x0046b149
     // (TEST EAX,EAX / JZ 0x0046b065), skipping the boarding/class checks entirely. That IS the
     // call_log default, and it is pinned anyway rather than relied on silently.
     // session_mode pinned OFF 3 so emit_mp_ack_or_notify (0x0046b14e: CMP [session_mode],0x3 /
     // JNZ 0x0046b175) takes the unit_notify_status arm, not the second dispatch.
     [](fixture &f, const int32_t *) {
         f.session_mode              = 0;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_unit_order_attack_target",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_target(v, s, recording_calls(), (uint32_t)p[0], p[1], (uint32_t)p[2],
                                       p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (2 orders): same in_range==0 "otherwise" path, but session_mode == 3
     // (0x0046b14e/0x0046b155) pinned so the SECOND dispatch (the 0xfb/0xfb admin ack @0x0046b16e)
     // fires too.
     [](fixture &f, const int32_t *) {
         f.session_mode              = 3;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_attack_target_alt @0x0046b599 -- same UNGUARDED shape as its twin,
    // verified from its OWN listing. Same two-row split, same reason.
    {"llm_strat_unit_order_attack_target_alt",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_target_alt(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                           (uint32_t)p[2], p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (1 order): in_range == 0 -> LAB_0046b788/@0x0046b7c4 (the alt's own "otherwise" shape,
     // param0 0x1b -- NOT its twin's 0x1a).
     [](fixture &f, const int32_t *) {
         f.session_mode              = 0;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_unit_order_attack_target_alt",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_target_alt(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                           (uint32_t)p[2], p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (2 orders): same "otherwise" path, session_mode == 3 for the mp-ack dispatch @0x0046b7e9.
     [](fixture &f, const int32_t *) {
         f.session_mode              = 3;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ---- llm_strat_unit_order_attack_unit @0x0046bc14 -- fully UNGUARDED: no range/boarding/class
    // branch at all, a single unconditional dispatch (0x0046bcef, order 0x1e/0x1e). nullptr
    // seed_pass is correct here: there is no state this row needs to pin.
    {"llm_strat_unit_order_attack_unit",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_unit(v, s, recording_calls(), (uint32_t)p[0], p[1], (uint32_t)p[2],
                                     p[3], (uint32_t)p[4]);
     },
     5,
     nullptr,
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    // ---- llm_strat_unit_order_attack_building_reposition @0x0046bf9c -- UNGUARDED (every path ends
    // in >=1 dispatch: the "otherwise" shape @0x0046c149, or the reposition/jitter shape
    // @0x0046c20e). Same two-row split; the third golden site is unmatchable (see the finding).
    {"llm_strat_unit_order_attack_building_reposition",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_building_reposition(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                                    (uint32_t)p[2], p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (1 order): in_range == 0 -> straight to LAB_0046c10d/@0x0046c149, never touching
     // bldg_footprint_random_point (so no PRNG draw on this path).
     [](fixture &f, const int32_t *) {
         f.session_mode              = 0;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_unit_order_attack_building_reposition",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_attack_building_reposition(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                                    (uint32_t)p[2], p[3], (uint32_t)p[4]);
     },
     5,
     // PASS (2 orders): same "otherwise" path, session_mode == 3 for the mp-ack dispatch @0x0046c16e.
     [](fixture &f, const int32_t *) {
         f.session_mode              = 3;
         g_calls.in_weapon_range_ret = 0;
     },
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     2},

    // ======== unit(s): storage_group_footprint ========
    // Guard (short-circuit): buildings[player][st.b_index].online_state == 0 ||
    //   storage_type_accepts_unit(...) == 0  -->  FALLBACK to unit_order_move (no golden site for
    //   exit_storage on that path -- see the STANDALONE seq-id block).
    // Row A pins the ACCEPTED branch's move_op_arg==0xa arm (golden site 1, order 0x24/0x38) AND
    // session_mode==3 to also reach the MP resync dispatch (site 3, 0xfb/0xfb) -- emissions == 2.
    // Site 2 (order 0x29/0xb, the elevation-restricted arm) is mutually exclusive with site 1, so
    // it gets its own row below.
    {"llm_strat_unit_order_exit_storage",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_exit_storage(v, s, recording_calls(), (uint32_t)p[0], (uint32_t)p[1], p[2],
                                      p[3], p[4]);
     },
     5,
     [](fixture &f, const int32_t *p) {
         auto &st                    = f.storage_at(p[0], p[2]);
         st.b_index                  = 5;
         f.at(p[0], 5).online_state  = 1;
         g_calls.storage_accepts_ret = 1; // guard admits
         unit &u                     = f.unit_at(p[0], p[1]);
         u.unit_proto_id             = 9;
         f.cfg_units[9].move_op_arg  = 0xa; // elevation-unaffected class -> site 1 (0x24/0x38)
         f.session_mode              = 3;   // MP lockstep -> ALSO site 3 (0xfb/0xfb)
     },
     // THE FIRST TWO GUARDS ARE NOT "EMIT NOTHING" GUARDS, and the first draft of this row said
     // they were. `online_state == 0` and `storage_type_accepts_unit() == 0` fall through to a
     // FALLBACK that calls unit_order_move -- so the wrapper still emits, just a different order.
     // Asserting "nothing" there failed 6 checks against a correct translation. The fallback is
     // pinned properly in its own hand-authored block below (it is also the only path that writes
     // order_seq_id_by_player). Only the THIRD guard is a true early return.
     {nullptr, nullptr,
      [](fixture &f, const int32_t *p) {
          f.storage_at(p[0], p[2]).b_index = 5;
          f.at(p[0], 5).online_state       = 1;
          g_calls.storage_accepts_ret      = 1;
          unit &u                          = f.unit_at(p[0], p[1]);
          u.unit_proto_id                  = 11;
          u.elevation                      = 10;
          f.cfg_units[11].move_op_arg      = 5;  // != 0xa -> elevation-restricted branch
          f.cfg_units[11].elevation        = 20; // 10 < 20 -> guard rejects
      }},
     {nullptr, nullptr,
      "elevation < cfg elevation must emit nothing -- full early return, no dispatch and no "
      "session-mode branch at all (CMP EAX,[EDX+0xe4a233] / JL, 0x0046a848)"},
     2},

    // Row B (the elevation-restricted arm, order 0x29/0xb) IS NOT A PILOTS ROW, and cannot be.
    // Its golden site declares `scratch_reset_before = false, scratch_n = 2`, while the arm really
    // runs scratch_reset() + THREE writes (slot 2 before the branch, then slots 0 and 1 inside it).
    // verify_emission selects a site by exact scratch count, so a correct translation taking this
    // arm matches 0 of 3 -- measured, 3 failures, before this row was demoted. That is the same
    // address-order-not-control-flow attribution bug this sweep hit on move_relative, attack_target,
    // attack_target_alt, attack_building_reposition and auto_launch_from_storage; here the shared
    // pre-branch `scratch_reset` + slot-2 write are credited only to the first site in address
    // order. The arm's own coverage is the hand-authored block below.

    // state gates two disjoint dispatch arms (0x1f, 0x22); any other state is a no-op. Inside the
    // 0x1f arm there is a SECOND guard on built_flags != 3. This row drives state==0x22 (golden
    // site 2, order 0x1f/0x23) -- site 1 (state==0x1f) is UNMATCHABLE, see the finding below.
    {"llm_strat_unit_order_auto_launch_from_storage",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::unit_order_auto_launch_from_storage(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                                  (uint32_t)p[2], (uint32_t)p[3]);
     },
     4,
     [](fixture &f, const int32_t *p) { f.unit_at(p[0], p[1]).state = 0x22; },
     {[](fixture &f, const int32_t *p) { f.unit_at(p[0], p[1]).state = 0x10; },
      [](fixture &f, const int32_t *p) {
          unit &u                       = f.unit_at(p[0], p[1]);
          u.state                       = 0x1f;
          u.home_storage_slot           = 3; // fixed, small, safe
          f.storage_at(p[0], 3).b_index = 5;
          f.at(p[0], 5).built_flags     = 0; // != 3 -> guard rejects
      },
      nullptr},
     {"state not in {0x1f,0x22} must emit nothing (CMP word ..,0x1f/JNZ 0x0046cdc7, then CMP "
      "..,0x22/JNZ 0x0046cf03)",
      "state==0x1f with built_flags != 3 must emit nothing (CMP byte [..+0xc3d2a4],0x3 / JNZ, "
      "0x0046ce1a)",
      nullptr},
     1},

    // ======== unit(s): turret_debug_admin_player ========
    {"llm_strat_turret_order_target_unit",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::turret_order_target_unit(v, s, recording_calls(), (uint16_t)p[0], (uint16_t)p[1],
                                       (uint32_t)p[2], (uint32_t)p[3]);
     },
     4,
     nullptr, // unguarded -- straight-line, no branch in the 0x60-byte body
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_turret_order_target_building",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::turret_order_target_building(v, s, recording_calls(), (uint16_t)p[0], (uint16_t)p[1],
                                           (uint32_t)p[2], (uint32_t)p[3]);
     },
     4,
     nullptr, // unguarded -- straight-line, identical shape to the sibling above
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_create_unit_debug",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_create_unit_debug(v, s, recording_calls(), p[0], p[1], p[2], (uint32_t)p[3]);
     },
     4,
     nullptr, // unguarded -- unconditional, no branch (0x0046d800)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_queue_construction_debug",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_queue_construction_debug(v, s, recording_calls(), p[0], p[1], p[2],
                                             (uint32_t)p[3]);
     },
     4,
     nullptr, // unguarded -- unconditional, no branch (0x0046da00)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_population_delta_debug",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_population_delta_debug(v, s, recording_calls(), (uint32_t)p[0], p[1]);
     },
     2,
     nullptr, // unguarded -- unconditional (0x0046f5ed)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_debug_energy_refill_full",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_debug_energy_refill_full(v, s, recording_calls(), (uint32_t)p[0], p[1]);
     },
     2,
     nullptr, // unguarded -- unconditional (0x0046fa9a)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_debug_damage_scaled",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_debug_damage_scaled(v, s, recording_calls(), (uint32_t)p[0], p[1]);
     },
     2,
     nullptr, // unguarded -- unconditional (0x0046fb5c)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_grant_resource",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_grant_resource(v, s, recording_calls(), (uint32_t)p[0], p[1], p[2]);
     },
     3,
     nullptr, // unguarded -- unconditional (0x0046f537)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_collect_available_projects",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_collect_available_projects(v, s, recording_calls(), (uint32_t)p[0]);
     },
     1,
     nullptr, // unguarded -- unconditional, no scratch (0x0046f687)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_recheck_projects",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_recheck_projects(v, s, recording_calls(), (uint32_t)p[0]);
     },
     1,
     nullptr, // unguarded -- unconditional, no scratch (0x0046f6fb)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_recheck_buildings",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_recheck_buildings(v, s, recording_calls(), (uint32_t)p[0]);
     },
     1,
     nullptr, // unguarded -- unconditional, no scratch (0x0046f76f)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_recheck_planet_system_all_players",
     [](const issue_view &v, const order_sink &s, const int32_t *) {
         api::order_recheck_planet_system_all_players(v, s, recording_calls());
     },
     0,
     nullptr, // unguarded, no parameters -- owner comes off v.player_side, which the golden's
              // owner_and_kind node cannot model (G_UNKNOWN); see the STANDALONE block for the
              // hand-authored check that field actually needs
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_credit_conquest_kills",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_credit_conquest_kills(v, s, recording_calls(), (uint32_t)p[0]);
     },
     1,
     nullptr, // unguarded -- unconditional, no scratch (0x0046f852)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_fow_reveal_full",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_fow_reveal_full(v, s, recording_calls(), (uint32_t)p[0]);
     },
     1,
     nullptr, // unguarded -- unconditional (0x0046fce0)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_set_player_relation",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_set_player_relation(v, s, recording_calls(), (uint32_t)p[0], p[1], (uint8_t)p[2]);
     },
     3,
     nullptr, // unguarded -- unconditional (0x0046f8c6)
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},

    {"llm_strat_order_set_player_control_mode",
     [](const issue_view &v, const order_sink &s, const int32_t *p) {
         api::order_set_player_control_mode(v, s, recording_calls(), (uint32_t)p[0], p[1],
                                            (uint8_t)p[2]);
     },
     3,
     nullptr, // unguarded from the golden's point of view -- the internal clear/set branch (see
              // STANDALONE) does not gate the dispatch, which fires unconditionally either way
     {nullptr, nullptr, nullptr},
     {nullptr, nullptr, nullptr},
     1},
};

int g_wrappers_compared, g_sites_compared, g_negative_cases;

// Compare one recorded run against the golden.
//
// GENERALISED TO N EMISSIONS BY THE O4A-C SWEEP, and that is not a relaxation. The pilot's six
// wrappers each emitted exactly one order, so this asserted `emissions == 1` and treated every
// scratch write in the run as belonging to it. Batch B breaks both assumptions: `unit_order_move`
// emits a SECOND order when session_mode == 3, `attack_target` has four static call sites, and each
// emission owns only the scratch writes made since the previous one. So the run is now PARTITIONED
// at each emission and every group is matched to its OWN golden site -- and the expected COUNT stays
// an explicit per-row number (`pilot::emissions`), because "however many arrived" would pass a
// wrapper that dropped one.
//
// Returns false if the run could not be matched at all (reported as a failure by the caller).
bool verify_emission(const pilot &pi, const int32_t *params) {
    const golden::g_site *sites[8];
    const int             n = golden::find_all(pi.wrapper, sites, 8);

    char msg[320];
    snprintf(msg, sizeof msg, "%s: the golden knows this wrapper", pi.wrapper);
    check(msg, n > 0);
    if (n <= 0) return false;

    // ---- partition the run at each emission ----
    struct group {
        const ev               *emit;
        std::vector<const ev *> writes;
        bool                    saw_reset;
    };
    std::vector<group> groups;
    {
        group cur;
        cur.emit      = nullptr;
        cur.saw_reset = false;
        for (const ev &e : g_ev) {
            if (e.k == EV_SET) cur.writes.push_back(&e);
            else if (e.k == EV_RESET) cur.saw_reset = true;
            else {
                cur.emit = &e;
                groups.push_back(cur);
                cur = group{nullptr, {}, false};
            }
        }
        // Anything after the last emission belongs to no order and is reported rather than dropped:
        // a trailing scratch write is a real emission-shape difference.
        if (!cur.writes.empty() || cur.saw_reset) {
            snprintf(msg, sizeof msg,
                     "%s: %u scratch write(s) and reset=%d AFTER the last emission -- they belong to "
                     "no order",
                     pi.wrapper, (unsigned)cur.writes.size(), (int)cur.saw_reset);
            check(msg, false);
        }
    }

    const int want = pi.emissions ? pi.emissions : 1;
    snprintf(msg, sizeof msg, "%s: %d order(s) emitted, expected %d", pi.wrapper,
             (int)groups.size(), want);
    check(msg, (int)groups.size() == want);
    if ((int)groups.size() != want) return false;

    // Each emission must match a DISTINCT golden site: two emissions collapsing onto one site would
    // otherwise let a wrapper that emitted the same order twice pass.
    bool used[8] = {false, false, false, false, false, false, false, false};

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const group &G    = groups[gi];
        const ev    &e    = *G.emit;
        const int    lane = (e.k == EV_ENQUEUE) ? 1 : 0;

        const golden::g_site *site  = nullptr;
        int                   found = 0;
        int                   pick  = -1;
        for (int i = 0; i < n; ++i) {
            if (used[i]) continue;
            if (n > 1) {
                if (sites[i]->lane != (uint8_t)lane) continue;
                ev_result p0 = eval(sites[i]->param0, params, pi.nparams);
                ev_result oc = eval(sites[i]->order_code, params, pi.nparams);
                if (p0.known && ((uint32_t)p0.v & 0xffffu) != ((uint32_t)e.c & 0xffffu)) continue;
                if (oc.known && ((uint32_t)oc.v & 0xffffu) != ((uint32_t)e.d & 0xffffu)) continue;
                // The scratch COUNT is a second discriminant, and it is what separates two sites of
                // one wrapper that pack the same triple down the same lane and differ only in how
                // much they wrote.
                if (G.writes.size() != (size_t)sites[i]->scratch_n) continue;
            }
            site = sites[i];
            pick = i;
            ++found;
        }
        snprintf(msg, sizeof msg,
                 "%s: emission %u matches exactly ONE unused golden site (matched %d of %d)",
                 pi.wrapper, (unsigned)(gi + 1), found, n);
        check(msg, found == 1);
        if (found != 1) return false;
        used[pick] = true;

        ++g_sites_compared;

        snprintf(msg, sizeof msg, "%s @%08x: LANE is %s", pi.wrapper, site->site_va,
                 site->lane ? "immediate (enqueue)" : "replicated (dispatch)");
        check(msg, site->lane == (uint8_t)lane);

        char what[280];
        snprintf(what, sizeof what, "%s @%08x unit_index", pi.wrapper, site->site_va);
        check_field(what, site->unit_index, params, pi.nparams, e.a);
        snprintf(what, sizeof what, "%s @%08x owner_and_kind", pi.wrapper, site->site_va);
        check_field(what, site->owner_and_kind, params, pi.nparams, e.b);
        snprintf(what, sizeof what, "%s @%08x param0", pi.wrapper, site->site_va);
        check_field(what, site->param0, params, pi.nparams, e.c);
        snprintf(what, sizeof what, "%s @%08x order_code", pi.wrapper, site->site_va);
        check_field(what, site->order_code, params, pi.nparams, e.d);

        // ---- the scratch sequence, in order, for THIS emission ----
        // The container copies all 13 slots into the record, so the final CONTENTS would be the same
        // for any write order -- but a differing sequence is a differing emission, and a dropped or
        // extra write changes the record outright. Compared as a sequence for that reason.
        snprintf(msg, sizeof msg, "%s @%08x: %u scratch write(s), golden says %u", pi.wrapper,
                 site->site_va, (unsigned)G.writes.size(), (unsigned)site->scratch_n);
        check(msg, G.writes.size() == (size_t)site->scratch_n);

        snprintf(msg, sizeof msg, "%s @%08x: scratch_reset %s called", pi.wrapper, site->site_va,
                 site->scratch_reset_before ? "IS" : "is NOT");
        check(msg, G.saw_reset == site->scratch_reset_before);

        if (G.writes.size() == (size_t)site->scratch_n) {
            for (uint8_t i = 0; i < site->scratch_n; ++i) {
                snprintf(what, sizeof what, "%s @%08x scratch[%u] INDEX", pi.wrapper, site->site_va,
                         (unsigned)i);
                check_field(what, site->scratch[i].index, params, pi.nparams, G.writes[i]->a);
                snprintf(what, sizeof what, "%s @%08x scratch[%u] VALUE (write %u of %u, at %08x)",
                         pi.wrapper, site->site_va, (unsigned)i, (unsigned)(i + 1),
                         (unsigned)site->scratch_n, site->scratch[i].at);
                check_field(what, site->scratch[i].value, params, pi.nparams, G.writes[i]->b);
            }
        }
    }
    return true;
}

} // namespace

int run_issuetest() {
    printf("issuetest -- the order-issue wrappers, against a golden extracted from the original\n");
    g_checks = g_fails = g_unpinned = 0;
    g_wrappers_compared = g_sites_compared = g_negative_cases = 0;

    fixture f;

    for (const pilot &pi : PILOTS) {
        ++g_wrappers_compared;

        for (const int32_t(&in)[6] : INPUTS) {
            f.reset();
            if (pi.seed_pass) pi.seed_pass(f, in);
            g_ev.clear();
            const issue_view v = f.view();
            pi.call(v, recording_sink(), in);
            verify_emission(pi, in);
        }

        for (int k = 0; k < 3; ++k) {
            if (!pi.seed_fail[k]) continue;
            for (const int32_t(&in)[6] : INPUTS) {
                f.reset();
                pi.seed_fail[k](f, in);
                g_ev.clear();
                const issue_view v = f.view();
                pi.call(v, recording_sink(), in);
                ++g_negative_cases;
                char msg[320];
                snprintf(msg, sizeof msg, "%s: %s", pi.wrapper,
                         pi.fail_why[k] ? pi.fail_why[k] : "guard rejects => no emission");
                bool emitted = false;
                for (const ev &e : g_ev)
                    if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) emitted = true;
                check(msg, !emitted);
            }
        }
    }

    // The equality-vs-range case that does not fit the seed table: production_remove's guard is
    // `state == 100` exactly, so 99 and 101 must BOTH still emit. A reimplementation that reached
    // for `>= 100` (plausible, since 100 is the under-construction state) passes every other check
    // in this file and fails only here.
    {
        const int32_t in[6] = {3, 42, 17, 9, 23, 6};
        for (uint16_t st : {(uint16_t)99, (uint16_t)101}) {
            f.reset();
            f.at(in[0], in[1]).state = st;
            g_ev.clear();
            const issue_view v = f.view();
            api::bldg_order_production_remove(v, recording_sink(), (uint32_t)in[0], in[1], in[2]);
            bool emitted = false;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) emitted = true;
            char msg[200];
            snprintf(msg, sizeof msg,
                     "production_remove: state == %u still emits -- the guard is `== 100`, not a "
                     "range (0x0046dcf0)",
                     (unsigned)st);
            check(msg, emitted);
            ++g_negative_cases;
        }
    }

    // NaN. The original's first guard is an x87 FCOMP whose UNORDERED result sets CF, so the JNC is
    // NOT taken and the order IS emitted -- the opposite of what the obvious C++ translation
    // `if (!(a < b)) return;` does, since `!(NaN < x)` is true. Caught by this slice's adversarial
    // review and pinned here so a future "simplification" back to the negated form goes red.
    {
        const int32_t in[6] = {3, 42, 17, 9, 23, 6};
        f.reset();
        building &b               = f.at(in[0], in[1]);
        b.building_id             = 5;
        b.energy                  = std::numeric_limits<double>::quiet_NaN();
        f.cfg_buildings[5].energy = 100.0;
        f.cfg_buildings[5].type   = 3;
        g_ev.clear();
        const issue_view v = f.view();
        api::bldg_order_repair_cycle_start(v, recording_sink(), (uint32_t)in[0], in[1]);
        bool emitted = false;
        for (const ev &e : g_ev)
            if (e.k == EV_DISPATCH) emitted = true;
        check("repair_cycle_start: a NaN energy still EMITS -- an unordered FCOMP sets CF, so the "
              "JNC at 0x0046e38f is not taken (`>=`, never `!(<)`)",
              emitted);
        ++g_negative_cases;
    }

    // The owner mask. debug_kill_group builds its owner with `AND EAX,0xf` (0x0046fc67), not an OR,
    // so a player id with bits above the low nibble must be TRUNCATED. Driven with a value the other
    // pilots cannot use (they index the fixture with it); the expectation is still the golden's, so
    // nothing here is hand-authored.
    {
        const int32_t in[6] = {0x1234, 77, 0, 0, 0, 0};
        f.reset();
        g_ev.clear();
        const issue_view v = f.view();
        api::order_debug_kill_group(v, recording_sink(), (uint32_t)in[0], in[1]);
        for (const pilot &pi : PILOTS)
            if (std::strcmp(pi.wrapper, "llm_strat_order_debug_kill_group") == 0)
                verify_emission(pi, in);
    }


    // ==============================================================================================
    // HAND-AUTHORED CASES -- the O4A-C sweep. Everything below covers what the golden
    // CANNOT: rows with no golden site at all (they emit no order of their own), and
    // observables outside the emission (a written global, a sound, an out-pointer). These
    // SHARE AN AUTHOR with the reading that produced the translations, so they are weaker
    // evidence than the golden comparisons above -- the same caveat the file already makes
    // about its guard-reject cases, and it is why these rows are graded T2.
    // ==============================================================================================

    // toggle_active @0x0046e1f6 -- unit bldg_orders_b. NO golden site (order_matrix has no entry for
    // it): it emits nothing itself, only reads buildings[player][building_index].built_flags bit 0x2
    // (`TEST byte [...],0x2` / `JZ`, 0x0046e226/0x0046e22d) and calls one of its two siblings --
    // detail::bldg_order_activate (bit CLEAR, the JZ taken; emits order 0x80/0x80) or this unit's own
    // detail::bldg_order_deactivate (bit SET, fall-through; emits order 0x81/0x81). HAND-AUTHORED,
    // like the production_remove/NaN/owner-mask blocks above: there is no golden site to compare
    // against, so this pins WHICH branch fires for which flag value by asserting on the order code
    // recorded in g_ev, which is weaker evidence than a golden comparison.
    {
        const int32_t in[6] = {3, 42, 17, 9, 23, 6};

        // built_flags bit 0x2 CLEAR -> JZ taken -> calls bldg_order_activate -> order 0x80
        f.reset();
        f.at(in[0], in[1]).built_flags = 0;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::bldg_order_toggle_active(v, recording_sink(), recording_calls(), (uint16_t)in[0],
                                          in[1]);
        }
        {
            bool    emitted    = false;
            int32_t order_code = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) {
                    emitted    = true;
                    order_code = e.d;
                }
            check("toggle_active: built_flags bit 0x2 CLEAR calls activate, order 0x80 (JZ taken, "
                  "0x0046e22d -> 0x0046e0f2)",
                  emitted && order_code == 0x80);
        }
        ++g_negative_cases;

        // built_flags bit 0x2 SET -> JZ not taken -> calls this unit's bldg_order_deactivate -> order
        // 0x81
        f.reset();
        f.at(in[0], in[1]).built_flags = 2;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::bldg_order_toggle_active(v, recording_sink(), recording_calls(), (uint16_t)in[0],
                                          in[1]);
        }
        {
            bool    emitted    = false;
            int32_t order_code = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) {
                    emitted    = true;
                    order_code = e.d;
                }
            check("toggle_active: built_flags bit 0x2 SET calls deactivate, order 0x81 (JZ not taken "
                  "at 0x0046e22d)",
                  emitted && order_code == 0x81);
        }
        ++g_negative_cases;

        // Every OTHER bit set, 0x2 clear -- proves the test is `TEST ...,0x2` specifically, not "any
        // nonzero built_flags".
        f.reset();
        f.at(in[0], in[1]).built_flags = 0xfd;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::bldg_order_toggle_active(v, recording_sink(), recording_calls(), (uint16_t)in[0],
                                          in[1]);
        }
        {
            bool    emitted    = false;
            int32_t order_code = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) {
                    emitted    = true;
                    order_code = e.d;
                }
            check("toggle_active: built_flags = 0xfd (every bit but 0x2) still routes to activate, "
                  "order 0x80 -- only bit 0x2 is tested (0x0046e226)",
                  emitted && order_code == 0x80);
        }
        ++g_negative_cases;
    }

    // llm_strat_bldg_order_depart_dispatch_by_type @0x0046ee88 -- the TYPE SWITCH, one case per arm
    // plus the default/no-match fall-through. HAND-AUTHORED: no golden site (the router emits no
    // order of its own), so this is the only spec the conductor can write oracle cases from.
    {
        const int32_t in[6] = {3, 42, 17, 9, 23, 6}; // player=3, bldg_idx=42, dest_planet_idx=17

        struct depart_case {
            uint8_t     type;
            int32_t     dest_planet_idx;
            int32_t     cur_planet_index;
            bool        want_emit;
            int32_t     want_order; // meaningless when want_emit is false
            const char *why;
        };
        const depart_case cases[] = {
            {0x21, 17, 0, true, 0xd2, "type 0x21 -> shuttle_depart (JZ 0x0046eece)"},
            {0x0d, 17, 0, true, 0xd2, "type 0x0d -> shuttle_depart (JNZ fallthrough, 0x0046eef7)"},
            {0x1a, 17, 0, true, 0xd4, "type 0x1a -> mother_depart (JZ 0x0046ef34)"},
            {0x06, 17, 0, true, 0xd4, "type 0x06 -> mother_depart (JNZ fallthrough, 0x0046ef5d)"},
            {0x20, 17, 0, true, 0xd3, "type 0x20, dest != planet_index -> port_depart (JZ 0x0046ef97)"},
            {0x0c, 17, 0, true, 0xd3,
             "type 0x0c, dest != planet_index -> port_depart (JNZ fallthrough, 0x0046efc0)"},
            {0x20, 5, 5, false, 0,
             "type 0x20, dest == planet_index -> no order (CMP 0x0046efc5 / JZ 0x0046efcb)"},
            {0xff, 17, 0, false, 0, "unmatched type -> no order (falls through to 0x0046efdc)"},
        };

        for (const depart_case &tc : cases) {
            f.reset();
            building &b             = f.at(in[0], in[1]);
            b.building_id           = 5;
            f.cfg_buildings[5].type = tc.type;
            f.planet_index          = tc.cur_planet_index;
            g_ev.clear();
            const issue_view v = f.view();
            api::bldg_order_depart_dispatch_by_type(v, recording_sink(), recording_calls(),
                                                    (uint32_t)in[0], in[1], tc.dest_planet_idx);
            bool    emitted    = false;
            int32_t order_code = 0;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) {
                    emitted    = true;
                    order_code = e.d;
                }

            char msg[280];
            snprintf(msg, sizeof msg, "depart_dispatch_by_type: %s -- emits %s", tc.why,
                     tc.want_emit ? "an order" : "nothing");
            check(msg, emitted == tc.want_emit);
            if (tc.want_emit) {
                snprintf(msg, sizeof msg, "depart_dispatch_by_type: %s -- order code is 0x%x", tc.why,
                         (unsigned)tc.want_order);
                check(msg, order_code == tc.want_order);
            } else {
                ++g_negative_cases;
            }
        }
    }

    // llm_strat_bldg_order_depart_confirm_dispatch @0x00415b41 -- the UI hand-off. HAND-AUTHORED: no
    // golden site of its own. Asserts BOTH halves: the pre-clear values reached the emitted order
    // (via dispatch_by_type -> shuttle_depart, order 0xd2), AND all three globals read zero
    // afterwards. Seeded with three DISTINCT non-zero values (bldg_a=7, bldg_b=13, planet=21) so a
    // swapped read or a slot cleared before it was consumed would be visible.
    {
        const uint16_t player = 2;

        // ---- case 1: slot A nonzero -> A's branch runs; B must be left untouched ----
        {
            f.reset();
            f.player_side                 = player;
            f.ui_depart_pending_bldg_a    = 7;
            f.ui_depart_pending_bldg_b    = 13; // distinct nonzero -- must survive the A branch
            f.ui_planet_sel_action_target = 21;
            building &b                   = f.at(player, 7);
            b.building_id                 = 5;
            f.cfg_buildings[5].type       = 0x21; // shuttle_depart arm, order 0xd2, unconditional
            g_ev.clear();
            const issue_view v = f.view();
            api::bldg_order_depart_confirm_dispatch(v, recording_sink(), recording_calls());

            bool    saw_dispatch = false, saw_set = false;
            int32_t got_unit = -1, got_owner = -1, got_arg = -1, got_scratch_idx = -1,
                    got_scratch_val = -1;
            for (const ev &e : g_ev) {
                if (e.k == EV_SET) {
                    saw_set         = true;
                    got_scratch_idx = e.a;
                    got_scratch_val = e.b;
                }
                if (e.k == EV_DISPATCH) {
                    saw_dispatch = true;
                    got_unit     = e.a;
                    got_owner    = e.b;
                    got_arg      = e.d;
                }
            }
            check("confirm_dispatch (slot A): emits the shuttle_depart order", saw_dispatch);
            check("confirm_dispatch (slot A): unit_index == pending_bldg_a (7), read BEFORE the clear "
                  "at 0x00415b7a",
                  got_unit == 7);
            check("confirm_dispatch (slot A): owner_and_kind == player_side|0x40 (2|0x40)",
                  got_owner == (int32_t)(player | 0x40));
            check("confirm_dispatch (slot A): order code == 0xd2 (shuttle_depart arm)",
                  got_arg == 0xd2);
            check("confirm_dispatch (slot A): scratch[6] == planet target (21), read BEFORE the clear "
                  "at 0x00415bb1",
                  saw_set && got_scratch_idx == 6 && got_scratch_val == 21);
            check("confirm_dispatch (slot A): ui_depart_pending_bldg_a cleared to 0 (0x00415b7a)",
                  f.ui_depart_pending_bldg_a == 0);
            check("confirm_dispatch (slot A): ui_depart_pending_bldg_b untouched (still 13) -- only "
                  "the taken branch's own slot is cleared",
                  f.ui_depart_pending_bldg_b == 13);
            check("confirm_dispatch (slot A): ui_planet_sel_action_target cleared to 0 "
                  "unconditionally (0x00415bb1)",
                  f.ui_planet_sel_action_target == 0);
        }

        // ---- case 2: slot A zero, slot B nonzero -> B's branch runs (else-if at 0x00415b86) ----
        {
            f.reset();
            f.player_side                 = player;
            f.ui_depart_pending_bldg_a    = 0;
            f.ui_depart_pending_bldg_b    = 13;
            f.ui_planet_sel_action_target = 21;
            building &b                   = f.at(player, 13);
            b.building_id                 = 5;
            f.cfg_buildings[5].type       = 0x21;
            g_ev.clear();
            const issue_view v = f.view();
            api::bldg_order_depart_confirm_dispatch(v, recording_sink(), recording_calls());

            bool    saw_dispatch = false, saw_set = false;
            int32_t got_unit = -1, got_scratch_idx = -1, got_scratch_val = -1;
            for (const ev &e : g_ev) {
                if (e.k == EV_SET) {
                    saw_set         = true;
                    got_scratch_idx = e.a;
                    got_scratch_val = e.b;
                }
                if (e.k == EV_DISPATCH) {
                    saw_dispatch = true;
                    got_unit     = e.a;
                }
            }
            check("confirm_dispatch (slot B): emits the shuttle_depart order via the else-if branch",
                  saw_dispatch);
            check("confirm_dispatch (slot B): unit_index == pending_bldg_b (13), read BEFORE the "
                  "clear at 0x00415ba7",
                  got_unit == 13);
            check("confirm_dispatch (slot B): scratch[6] == planet target (21), read BEFORE the clear",
                  saw_set && got_scratch_idx == 6 && got_scratch_val == 21);
            check("confirm_dispatch (slot B): ui_depart_pending_bldg_b cleared to 0 (0x00415ba7)",
                  f.ui_depart_pending_bldg_b == 0);
            check("confirm_dispatch (slot B): ui_depart_pending_bldg_a still 0 (never touched)",
                  f.ui_depart_pending_bldg_a == 0);
            check("confirm_dispatch (slot B): ui_planet_sel_action_target cleared to 0 "
                  "unconditionally (0x00415bb1)",
                  f.ui_planet_sel_action_target == 0);
        }

        // ---- case 3: both slots zero -> neither branch runs, planet target STILL cleared ----
        {
            f.reset();
            f.player_side                 = player;
            f.ui_depart_pending_bldg_a    = 0;
            f.ui_depart_pending_bldg_b    = 0;
            f.ui_planet_sel_action_target = 21;
            g_ev.clear();
            const issue_view v = f.view();
            api::bldg_order_depart_confirm_dispatch(v, recording_sink(), recording_calls());

            bool emitted = false;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) emitted = true;
            check("confirm_dispatch (neither slot set): no order emitted", !emitted);
            check("confirm_dispatch (neither slot set): ui_planet_sel_action_target STILL cleared to "
                  "0 -- the clear at 0x00415bb1 is unconditional, reached whether or not either "
                  "branch ran",
                  f.ui_planet_sel_action_target == 0);
            ++g_negative_cases;
        }
    }

    // ============================================================================================
    // exit_storage's ELEVATION-RESTRICTED arm (order 0x29/0xb) -- demoted from a PILOTS row because
    // its golden site is unmatchable (see the note at the row). Hand-authored, therefore weaker
    // evidence than a golden comparison, and it is why exit_storage is graded T2.
    // ============================================================================================
    {
        const int32_t in[6] = {3, 42, 2, 9, 23, 6};

        // elevation EQUAL to the cfg value: the guard is `<`, strictly, so equal must still PASS.
        // With the reject case in the PILOTS row's third fail seed (10 < 20) that is the boundary
        // from both sides.
        f.reset();
        f.storage_at(in[0], in[2]).b_index = 6;
        f.at(in[0], 6).online_state        = 1;
        g_calls.storage_accepts_ret        = 1;
        {
            unit &u                     = f.unit_at(in[0], in[1]);
            u.unit_proto_id             = 13;
            u.elevation                 = 50;
            f.cfg_units[13].move_op_arg = 5;  // != 0xa -> the elevation-restricted branch
            f.cfg_units[13].elevation   = 50; // == u.elevation -- the `<` boundary, PASSES
        }
        g_calls.approach_tile[0] = 777; // distinct from the storage slot's own exit tiles, so a
        g_calls.approach_tile[1] = 888; // body that shipped the wrong pair is visible
        f.session_mode           = 0;   // not MP -> exactly one emission
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::unit_order_exit_storage(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                         (uint32_t)in[1], in[2], in[3], in[4]);
        }
        {
            int     emitted = 0;
            int32_t p0 = -1, oc = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH) {
                    ++emitted;
                    p0 = e.c;
                    oc = e.d;
                }
            check("exit_storage elevation arm: elevation == cfg.elevation still EMITS -- the guard "
                  "is `<`, strictly (CMP EAX,[EDX+0xe4a233] / JL, 0x0046a848)",
                  emitted == 1);
            check("exit_storage elevation arm: order is 0x29/0xb, NOT the 0x24/0x38 of the "
                  "move_op_arg==0xa arm",
                  p0 == 0x29 && oc == 0xb);
        }
        {
            // the approach tile, not the storage slot's exit tile, reaches scratch slots 0 and 1.
            int32_t got0 = -1, got1 = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_SET) {
                    if (e.a == 0) got0 = e.b;
                    if (e.a == 1) got1 = e.b;
                }
            check("exit_storage elevation arm: scratch[0]/scratch[1] carry storage_get_approach_tile's "
                  "OUT values (777/888), not the slot's exit_tile pair",
                  got0 == 777 && got1 == 888);
        }
        check("exit_storage elevation arm: scratch[2] carries storage_index", [&] {
            for (const ev &e : g_ev)
                if (e.k == EV_SET && e.a == 2) return e.b == in[2];
            return false;
        }());
        ++g_negative_cases;
    }

    // ============================================================================================
    // exit_storage's FALLBACK path -- what the first two guards actually do. NOT "emit nothing":
    // they fall through to unit_order_move, so the wrapper emits a DIFFERENT order. This is the
    // assertion that replaces the two wrong fail seeds.
    // ============================================================================================
    {
        const int32_t in[6] = {3, 42, 2, 9, 23, 6};
        for (int guard = 0; guard < 2; ++guard) {
            f.reset();
            f.session_mode                     = 0; // keep the sibling's own MP echo out of it
            f.storage_at(in[0], in[2]).b_index = 5;
            // guard 0: online_state == 0 (the fixture default, set explicitly).
            // guard 1: online_state != 0 but storage_type_accepts_unit() == 0.
            f.at(in[0], 5).online_state = (uint16_t)(guard == 0 ? 0 : 1);
            g_calls.storage_accepts_ret = 0;
            g_ev.clear();
            {
                const issue_view v = f.view();
                api::unit_order_exit_storage(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                             (uint32_t)in[1], in[2], in[3], in[4]);
            }
            int     emitted = 0;
            int32_t p0      = -1;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) {
                    ++emitted;
                    p0 = e.c;
                }
            char msg[240];
            snprintf(msg, sizeof msg,
                     "exit_storage fallback (guard %d): the reject path EMITS via unit_order_move -- "
                     "it does not fall silent (0x0046a73b / 0x0046a794)",
                     guard);
            check(msg, emitted >= 1);
            snprintf(msg, sizeof msg,
                     "exit_storage fallback (guard %d): the emitted order is NOT an exit-storage one "
                     "(param0 0x%x is neither 0x24 nor 0x29)",
                     guard, (unsigned)p0);
            check(msg, p0 != 0x24 && p0 != 0x29);
            ++g_negative_cases;
        }
    }

    // ============================================================================================
    // exit_storage's OWN write to order_seq_id_by_player -- only on the FALLBACK (guard-reject)
    // path (0x0046a8f2-0x0046a90f). verify_emission cannot see it (it only reads g_ev), so this is
    // hand-authored, not golden-derived.
    // ============================================================================================
    {
        const int32_t in[6] = {3, 42, 2, 9, 23, 6}; // storage_index kept small (2)

        // guard rejects by default (online_state == 0 after reset()) -> fallback branch runs.
        f.reset();
        f.order_seq_id_by_player[in[0]] = 5;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::unit_order_exit_storage(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                         (uint32_t)in[1], in[2], in[3], in[4]);
        }
        check("exit_storage fallback: order_seq_id_by_player[player] increments by exactly 1 "
              "(0x0046a8f2)",
              f.order_seq_id_by_player[in[0]] == 6);

        // wrap: 0xff -> INC wraps to 0 -> re-INC to 1 -- 0 is never emitted as a seq id.
        f.reset();
        f.order_seq_id_by_player[in[0]] = 0xff;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::unit_order_exit_storage(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                         (uint32_t)in[1], in[2], in[3], in[4]);
        }
        check("exit_storage fallback: order_seq_id_by_player[player] wraps 0xff -> 0x01, never "
              "emits 0 (0x0046a8fc CMP ..,0x0 / JNZ, then the re-INC at 0x0046a909)",
              f.order_seq_id_by_player[in[0]] == 1);

        // the write must land on the player's OWN slot and no other -- `ours = original + 1` is
        // this project's signature bug shape for a stamped field.
        f.reset();
        f.order_seq_id_by_player[in[0]] = 5;
        for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
            if (pl != in[0]) f.order_seq_id_by_player[pl] = 77; // sentinel, must survive untouched
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::unit_order_exit_storage(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                         (uint32_t)in[1], in[2], in[3], in[4]);
        }
        {
            bool others_untouched = true;
            for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
                if (pl != in[0] && f.order_seq_id_by_player[pl] != 77) others_untouched = false;
            check("exit_storage fallback: the seq-id write lands on the player's OWN slot only, "
                  "not another's",
                  others_untouched);
        }
        g_negative_cases += 3;
    }

    // ============================================================================================
    // llm_strat_group_order_ack_voice @0x004259b2 -- NO golden site (emits a sound, not an order).
    // ============================================================================================
    {
        // the suppressed-count boundary: CMP/JLE at 0x00425a42/0x00425a49 is `5 < count`, not
        // `<=` -- the generic sound fires on the 6th consecutive refusal, not the 5th. Both sides.
        f.reset();
        f.ack_voice_last_play_time   = 100.0;
        f.ack_voice_cooldown_sec     = 2.0;
        f.game_clock                 = 100.0; // last+cooldown(102) >= clock(100) -> suppressed
        f.ack_voice_suppressed_count = 4;     // about to become 5 -- must NOT fire yet
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_order_ack_voice(v, recording_sink(), recording_calls());
        }
        check("ack_voice: suppressed_count 4->5 must NOT play the generic sound (CMP ..,0x5 / JLE, "
              "0x00425a42/0x00425a49)",
              g_calls.snd_calls == 0 && f.ack_voice_suppressed_count == 5);

        f.reset();
        f.ack_voice_last_play_time   = 100.0;
        f.ack_voice_cooldown_sec     = 2.0;
        f.game_clock                 = 100.0;
        f.ack_voice_suppressed_count = 5; // about to become 6 -- the 6th refusal -- MUST fire
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_order_ack_voice(v, recording_sink(), recording_calls());
        }
        check("ack_voice: suppressed_count 5->6 (the 6th refusal) plays the generic sound 0x44 "
              "(0x00425a4b/0x00425a50)",
              g_calls.snd_calls == 1 && g_calls.last_snd_id == 0x44 &&
                  g_calls.last_snd_volume == 100);
        check("ack_voice: after the 6th-refusal sound, suppressed_count resets to 0 (0x00425a5a)",
              f.ack_voice_suppressed_count == 0);
        g_negative_cases += 2;
    }
    {
        // NaN: an unordered FCOMP does NOT leave the JNC at 0x004259df untaken -- the SUPPRESSED
        // branch is SKIPPED and the emit arm runs. The `>=` mirror agrees with hardware on THIS
        // direction (unlike bldg_order_repair_cycle_start's NaN case, where it would not).
        f.reset();
        f.ack_voice_last_play_time   = std::numeric_limits<double>::quiet_NaN();
        f.ack_voice_cooldown_sec     = 2.0;
        f.game_clock                 = 100.0;
        f.ack_voice_suppressed_count = 3; // must be RESET to 0 by the emit arm, not incremented
        f.player_race                = 0;
        g_calls.rand_next            = 1;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_order_ack_voice(v, recording_sink(), recording_calls());
        }
        check("ack_voice: NaN last_play_time takes the EMIT arm, not the suppressed one (0x004259df "
              "JNC on an unordered compare -- `>=`, never `!(<)`)",
              g_calls.snd_calls == 1 && g_calls.last_snd_id == f.ack_voice_snd_id_by_race[1]);
        check("ack_voice: the emit arm resets the suppressed counter and stamps last_play_time = "
              "game_clock",
              f.ack_voice_suppressed_count == 0 && f.ack_voice_last_play_time == f.game_clock);
        ++g_negative_cases;
    }
    {
        // race bucket: player_race == 2 selects entries [3..5], any other race selects [0..2]
        // (0x004259e8-0x004259f8).
        f.reset();
        f.game_clock      = 100.0; // past last_play_time(0) + cooldown(2) -- else this SUPPRESSES
        f.player_race     = 2;
        g_calls.rand_next = 2;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_order_ack_voice(v, recording_sink(), recording_calls());
        }
        check("ack_voice: player_race == 2 selects ack_voice_snd_id_by_race[rand_below_fx(3)+3]",
              g_calls.last_snd_id == f.ack_voice_snd_id_by_race[2 + 3]);
        check("ack_voice: rand_below_fx(3) is drawn exactly once per emit", g_calls.rand_calls == 1);

        f.reset();
        f.game_clock      = 100.0; // same -- the emit arm needs the cooldown to have elapsed
        f.player_race     = 5;     // != 2 -> the low bucket
        g_calls.rand_next = 0;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_order_ack_voice(v, recording_sink(), recording_calls());
        }
        check("ack_voice: player_race != 2 selects ack_voice_snd_id_by_race[rand_below_fx(3)+0]",
              g_calls.last_snd_id == f.ack_voice_snd_id_by_race[0]);
    }

    // ============================================================================================
    // llm_strat_group_issue_move_order_deferred @0x00444e5f -- NO golden site (emits only via the
    // sibling movers). session_mode stays 0 throughout, so each moved member contributes exactly
    // ONE order to g_ev.
    // ============================================================================================
    {
        f.reset();
        f.player_side               = 1;
        f.order_seq_id_by_player[1] = 10;
        for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
            if (pl != 1) f.order_seq_id_by_player[pl] = 55; // sentinel -- must survive untouched
        f.ctrl_groups[0].count = 3;
        for (int i = 0; i < 3; ++i) {
            f.ctrl_groups[0].unit_ids[i]  = (uint16_t)i;
            f.unit_at(1, i).unit_proto_id = (uint16_t)(20 + i);
            f.cfg_units[20 + i].type      = 1; // not 0x17/0x18 -- moves
        }
        f.key_lalt_held = 0; // -> unit_order_move (not _move_default)
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_deferred(v, recording_sink(), recording_calls(), 0x10, 0x38,
                                                 7);
        }
        {
            int emitted = 0;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) ++emitted;
            check("group_issue_move_order_deferred: a 3-member, all-non-cargo-heli control group "
                  "reaches g_ev exactly 3 times",
                  emitted == 3);
        }
        check("group_issue_move_order_deferred: order_seq_id_by_player[player_side] increments by "
              "exactly 1 when modifier != 0 (0x00444fa7)",
              f.order_seq_id_by_player[1] == 11);
        {
            bool others_untouched = true;
            for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
                if (pl != 1 && f.order_seq_id_by_player[pl] != 55) others_untouched = false;
            check("group_issue_move_order_deferred: the seq-id write lands on player_side's OWN "
                  "slot only",
                  others_untouched);
        }
    }
    {
        // wrap arithmetic, AND: the seq-stamp `if (mod != 0)` (0x00444f9a) is INDEPENDENT of the
        // did_move `if` (0x00444f93) -- it fires even with ZERO group members.
        f.reset();
        f.player_side               = 2;
        f.order_seq_id_by_player[2] = 0xff;
        f.ctrl_groups[0].count      = 0;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_deferred(v, recording_sink(), recording_calls(), 0x10, 0x38,
                                                 9);
        }
        check("group_issue_move_order_deferred: the seq-id write wraps 0xff -> 0x01 and fires even "
              "with ZERO group members -- the 0x00444f9a `if` is not gated by the 0x00444f93 one",
              f.order_seq_id_by_player[2] == 1);
        {
            bool emitted = false;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) emitted = true;
            check("group_issue_move_order_deferred: zero members -> zero orders reach g_ev",
                  !emitted);
        }
        ++g_negative_cases;
    }
    {
        // modifier == 0 -> mod stays 0 -> the seq id is NOT stamped, even though a member moved.
        f.reset();
        f.player_side                 = 3;
        f.order_seq_id_by_player[3]   = 20;
        f.ctrl_groups[0].count        = 1;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.unit_at(3, 0).unit_proto_id = 30;
        f.cfg_units[30].type          = 1;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_deferred(v, recording_sink(), recording_calls(), 0x10, 0x38,
                                                 0);
        }
        check("group_issue_move_order_deferred: modifier == 0 -> seq id is NOT stamped even though "
              "a member moved (0x00444e85 CMP ..,0x0 / JZ)",
              f.order_seq_id_by_player[3] == 20);
        ++g_negative_cases;
    }
    {
        // a UNIT_TYPE_A_HELI_CARGO (0x17) member is skipped entirely.
        f.reset();
        f.player_side                 = 4;
        f.ctrl_groups[0].count        = 2;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.ctrl_groups[0].unit_ids[1]  = 1;
        f.unit_at(4, 0).unit_proto_id = 40;
        f.cfg_units[40].type          = 0x17; // A_HELI_CARGO -- skipped
        f.unit_at(4, 1).unit_proto_id = 41;
        f.cfg_units[41].type          = 5; // ordinary -- moves
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_deferred(v, recording_sink(), recording_calls(), 0x10, 0x38,
                                                 0);
        }
        {
            int emitted = 0;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) ++emitted;
            check("group_issue_move_order_deferred: a UNIT_TYPE_A_HELI_CARGO (0x17) member is "
                  "skipped (0x00444ee9 CMP ..,0x17 / JZ)",
                  emitted == 1);
        }
    }

    // ============================================================================================
    // llm_strat_group_issue_move_order_confirmed @0x00444fcb -- NO golden site. `mod` is set ONLY
    // inside the `equivalent==0 || type<0xf` arm: the OPPOSITE asymmetry from `_deferred`, where
    // `mod` could be set with zero members moving. Here members can move with `mod` never set.
    // ============================================================================================
    {
        f.reset();
        f.player_side               = 1;
        f.order_seq_id_by_player[1] = 30;
        for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
            if (pl != 1) f.order_seq_id_by_player[pl] = 66;
        f.ctrl_groups[0].count = 3;
        for (int i = 0; i < 3; ++i) {
            f.ctrl_groups[0].unit_ids[i]   = (uint16_t)i;
            f.unit_at(1, i).unit_proto_id  = (uint16_t)(50 + i);
            f.cfg_units[50 + i].type       = 1; // not cargo heli
            f.cfg_units[50 + i].equivalent = 0; // == 0 -> the mod-setting arm
        }
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_confirmed(v, recording_sink(), recording_calls(), 100, 200);
        }
        {
            int emitted = 0;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) ++emitted;
            check("group_issue_move_order_confirmed: a 3-member group (all equivalent==0) reaches "
                  "g_ev exactly 3 times",
                  emitted == 3);
        }
        check("group_issue_move_order_confirmed: order_seq_id_by_player[player_side] increments by "
              "exactly 1 (0x004451a2)",
              f.order_seq_id_by_player[1] == 31);
        {
            bool others_untouched = true;
            for (int pl = 0; pl < mh::orders::issue::MAX_PLAYERS; ++pl)
                if (pl != 1 && f.order_seq_id_by_player[pl] != 66) others_untouched = false;
            check("group_issue_move_order_confirmed: the seq-id write lands on player_side's OWN "
                  "slot only",
                  others_untouched);
        }
    }
    {
        f.reset();
        f.player_side                 = 2;
        f.order_seq_id_by_player[2]   = 0xff;
        f.ctrl_groups[0].count        = 1;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.unit_at(2, 0).unit_proto_id = 60;
        f.cfg_units[60].type          = 1;
        f.cfg_units[60].equivalent    = 0;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_confirmed(v, recording_sink(), recording_calls(), 100, 200);
        }
        check("group_issue_move_order_confirmed: the seq-id write wraps 0xff -> 0x01 "
              "(0x004451a8/0x004451b1)",
              f.order_seq_id_by_player[2] == 1);
    }
    {
        // the OPPOSITE asymmetry: 2 members both take the no-mod arm (equivalent != 0 &&
        // type >= 0xf) -- both still move, but the seq id is never stamped.
        f.reset();
        f.player_side                 = 3;
        f.order_seq_id_by_player[3]   = 40;
        f.ctrl_groups[0].count        = 2;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.ctrl_groups[0].unit_ids[1]  = 1;
        f.unit_at(3, 0).unit_proto_id = 70;
        f.cfg_units[70].type          = 0x20;
        f.cfg_units[70].equivalent    = 1; // != 0 -> the no-mod arm
        f.unit_at(3, 1).unit_proto_id = 71;
        f.cfg_units[71].type          = 0x21;
        f.cfg_units[71].equivalent    = 1;
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_confirmed(v, recording_sink(), recording_calls(), 100, 200);
        }
        {
            int emitted = 0;
            for (const ev &e : g_ev)
                if (e.k == EV_DISPATCH || e.k == EV_ENQUEUE) ++emitted;
            check("group_issue_move_order_confirmed: 2 members on the move_confirmed_with_bump arm "
                  "both still move",
                  emitted == 2);
        }
        check("group_issue_move_order_confirmed: `mod` is never set by that arm, so the seq id is "
              "NOT stamped even though members moved (0x00445195 CMP dword ptr[..],0x0 / JZ)",
              f.order_seq_id_by_player[3] == 40);
        ++g_negative_cases;
    }
    {
        // the TYPE half of `equivalent==0 || type<0xf`, isolated by holding equivalent != 0: a
        // SIGNED compare (0x004450ea), boundary at 0xe (mod-setting arm) vs 0xf (no-mod arm).
        f.reset();
        f.player_side                 = 5;
        f.ctrl_groups[0].count        = 1;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.unit_at(5, 0).unit_proto_id = 80;
        f.cfg_units[80].equivalent    = 1;
        f.cfg_units[80].type          = 0xe; // < 0xf -> the mod-setting arm
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_confirmed(v, recording_sink(), recording_calls(), 100, 200);
        }
        check("group_issue_move_order_confirmed: type == 0xe (< 0xf) takes the mod-setting arm even "
              "with equivalent != 0",
              f.order_seq_id_by_player[5] == 1);

        f.reset();
        f.player_side                 = 5;
        f.ctrl_groups[0].count        = 1;
        f.ctrl_groups[0].unit_ids[0]  = 0;
        f.unit_at(5, 0).unit_proto_id = 81;
        f.cfg_units[81].equivalent    = 1;
        f.cfg_units[81].type          = 0xf; // NOT < 0xf -> the no-mod arm
        g_ev.clear();
        {
            const issue_view v = f.view();
            api::group_issue_move_order_confirmed(v, recording_sink(), recording_calls(), 100, 200);
        }
        check("group_issue_move_order_confirmed: type == 0xf is NOT < 0xf -- takes the no-mod arm "
              "(0x004450ea CMP ..,0xe / JG)",
              f.order_seq_id_by_player[5] == 0);
        ++g_negative_cases;
    }

    // ============================================================================================
    // llm_strat_bldg_footprint_random_point @0x00449d41 -- NOT an order wrapper: a pure geometry
    // helper, no golden site, `s` unused.
    // ============================================================================================
    {
        f.reset();
        f.geom.bw_mask            = 0xff; // small, distinct masks so the final AND is exercised
        f.geom.bh_mask            = 0x3f;
        f.at(3, 42).building_id   = 9;
        f.cfg_buildings[9].width  = 10;      // width_px = 160
        f.cfg_buildings[9].height = 4;       // height_px = 64
        g_calls.rand_next         = 50;      // both rand_below_fx calls return this pinned value
        uint32_t out_x = 1000, out_y = 2000; // distinct, nonzero in/out seed values
        {
            const issue_view v = f.view();
            api::bldg_footprint_random_point(v, recording_sink(), recording_calls(), 111, 222, 3, 42,
                                             &out_x, &out_y);
        }
        // half_trunc(160) = 80, half_trunc(64) = 32 -- both exact (width_px/height_px are always
        // even, being *16 of a byte), so the SAR-truncate vs plain-halve distinction never
        // manifests here.
        const uint32_t expect_x = (uint32_t)(1000 + (int32_t)(50 - 80)) & 0xff;
        const uint32_t expect_y = (uint32_t)(2000 + (int32_t)(50 - 32)) & 0x3f;
        check("bldg_footprint_random_point: *out_x == (*out_x_in + (rand_below_fx(width_px) - "
              "half_trunc(width_px))) & bw_mask",
              out_x == expect_x);
        check("bldg_footprint_random_point: *out_y == (*out_y_in + (rand_below_fx(height_px) - "
              "half_trunc(height_px))) & bh_mask",
              out_y == expect_y);
        check("bldg_footprint_random_point: rand_below_fx is drawn exactly twice -- once per axis "
              "(0x00449db9, 0x00449dd5)",
              g_calls.rand_calls == 2);
    }
    {
        // param_1/param_2 are dead (stored to locals, never read again): two calls differing ONLY
        // in those must produce IDENTICAL outputs. Proved by construction, not asserted true.
        f.reset();
        f.geom.bw_mask            = 0xff;
        f.geom.bh_mask            = 0x3f;
        f.at(5, 10).building_id   = 3;
        f.cfg_buildings[3].width  = 6;
        f.cfg_buildings[3].height = 8;
        g_calls.rand_next         = 7;
        uint32_t ax = 500, ay = 600;
        {
            const issue_view va = f.view();
            api::bldg_footprint_random_point(va, recording_sink(), recording_calls(), 1, 2, 5, 10,
                                             &ax, &ay);
        }

        f.reset();
        f.geom.bw_mask            = 0xff;
        f.geom.bh_mask            = 0x3f;
        f.at(5, 10).building_id   = 3;
        f.cfg_buildings[3].width  = 6;
        f.cfg_buildings[3].height = 8;
        g_calls.rand_next         = 7;
        uint32_t bx = 500, by = 600;
        {
            const issue_view vb = f.view();
            api::bldg_footprint_random_point(vb, recording_sink(), recording_calls(), 0xdeadbeef,
                                             0xcafebabe, 5, 10, &bx, &by);
        }
        check("bldg_footprint_random_point: param_1/param_2 are dead -- changing them alone does "
              "not change the result",
              ax == bx && ay == by);
    }

    // order_recheck_planet_system_all_players: the golden's owner_and_kind node for this site is
    // G_UNKNOWN (the value comes off the game global v.player_side, 0x0046f805, not a stack slot the
    // extractor models), so the PILOTS row's golden compare reports that field UNPINNED rather than
    // checking it. Proven by hand instead: seed player_side to a value distinct from every p[0] used
    // in INPUTS (3, 0, 7) and confirm the dispatch's owner argument is exactly that seeded value --
    // not 0 (the fixture default, which a wrapper that dropped the read entirely would also produce)
    // and not any of INPUTS' p[0]s.
    {
        f.reset();
        f.player_side = 55;
        g_ev.clear();
        const issue_view v = f.view();
        api::order_recheck_planet_system_all_players(v, recording_sink(), recording_calls());
        bool    found = false;
        int32_t owner = -1;
        for (const ev &e : g_ev)
            if (e.k == EV_DISPATCH) {
                found = true;
                owner = e.b;
            }
        check("recheck_planet_system_all_players: dispatch happens", found);
        check("recheck_planet_system_all_players: owner == v.player_side (MOVZX EDX,word "
              "[0x00e58354] @0x0046f805), not the fixture default and not an INPUTS p[0]",
              found && owner == 55);
    }

    // order_set_player_control_mode: the branch the unit spec flags as a hazard. The PILOTS row
    // proves only the emitted 0xf5 order and its two unconditional scratch writes; the mask
    // arithmetic, the shift's hardware 5-bit masking, and which format pointer reaches sprintf_ii
    // are untouched by the golden (it models registers/stack, not v.player_control_mask or
    // v.text_tmp) and are proven here by hand.
    {
        // ---- CLEAR arm (set_human == 0). target_player = 33 (>= 32) to exercise the SHL's hardware
        // 5-bit masking (SHL AL,CL @0x0046f9c8 masks CL to 5 bits regardless of C++ operand width,
        // so shift = 33 & 0x1f = 1, bit = 0x02). Starting mask 0xff -- EVERY bit set, nonzero, so an
        // OR-instead-of-AND bug leaves the mask UNCHANGED at 0xff (distinguishable from the correct
        // AND's 0xfd), and an assignment bug (mask = bit) lands at 0x02.
        const int32_t in[6] = {9, 33, 0, 0, 0, 0};
        f.reset();
        f.player_control_mask = 0xff;
        g_ev.clear();
        const issue_view v = f.view();
        api::order_set_player_control_mode(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                           in[1], (uint8_t)in[2]);
        check("set_player_control_mode: CLEAR arm 0xff & ~(1<<(33&0x1f)) == 0xfd (AND @0x0046f9cc, "
              "shift masked to 5 bits by SHL AL,CL @0x0046f9c8)",
              f.player_control_mask == 0xfd);
        char want_clear[64];
        snprintf(want_clear, sizeof want_clear, "sprintf_ii(fmt=%p,", (const void *)f.fmt_clear);
        check("set_player_control_mode: CLEAR arm's sprintf_ii uses v.fmt_control_mode_clear, not "
              "fmt_control_mode_set (0x0046f9d2-e9)",
              g_calls.called(want_clear));
    }
    {
        // ---- SET arm (set_human != 0). target_player = 37 (>= 32): shift = 37 & 0x1f = 5, bit =
        // 0x20. Starting mask 0xdf -- every bit set EXCEPT the target one, nonzero -- so an
        // AND-instead-of-OR bug leaves the mask unchanged at 0xdf (distinguishable from the correct
        // OR's 0xff), and an assignment bug (mask = bit) lands at 0x20.
        const int32_t in[6] = {9, 37, 1, 0, 0, 0};
        f.reset();
        f.player_control_mask = 0xdf;
        g_ev.clear();
        const issue_view v = f.view();
        api::order_set_player_control_mode(v, recording_sink(), recording_calls(), (uint32_t)in[0],
                                           in[1], (uint8_t)in[2]);
        check("set_player_control_mode: SET arm 0xdf | (1<<(37&0x1f)) == 0xff (OR @0x0046f9fb, shift "
              "masked to 5 bits by SHL AL,CL @0x0046f9f9)",
              f.player_control_mask == 0xff);
        char want_set[64];
        snprintf(want_set, sizeof want_set, "sprintf_ii(fmt=%p,", (const void *)f.fmt_set);
        check("set_player_control_mode: SET arm's sprintf_ii uses v.fmt_control_mode_set, not "
              "fmt_control_mode_clear (0x0046fa01-1e)",
              g_calls.called(want_set));
    }

    // ==============================================================================================
    // llm_strat_ai_group_move_formation_rotating @0x004d7da9 -- the O4B re-home (batch B unit
    // `group_formation_move`). HAND-AUTHORED IN FULL, and structurally so rather than by choice: its
    // sink is llm_strat_unit_order_move_enqueue, one level ABOVE the container, so
    // gen_order_issue_golden.py -- whose roots are llm_strat_order_dispatch / llm_strat_order_enqueue
    // -- records ZERO sites for it and `recording_sink` never fires. Every emission below is observed
    // through g_calls.move_enqueues instead. That makes this row a T2 on the same terms as the other
    // no-golden-site rows above: the expectations share an author with the translation.
    {
        // Distinct, non-symmetric inputs throughout: player 3 (so the *100 stride is exercised and a
        // dropped player term lands on player 0's units), x 41 != y 17 (a swap fails), and member ids
        // 12/5/61 in a NON-sorted order so a translation that walked the list backwards is visible.
        const int32_t PLAYER = 3;
        const int32_t OTHER  = 5;
        const int32_t TX = 41, TY = 17;
        const int32_t IDS[3]    = {12, 5, 61};
        const uint8_t SEQ_START = 0x27;

        f.reset();
        f.ai_group_scratch_count = 3;
        for (int i = 0; i < 3; ++i) f.ai_group_scratch_list[i] = IDS[i];
        for (int i = 0; i < 3; ++i) {
            // Pre-set to values the two stores must CHANGE in a specific way: notify_status 7 proves
            // the `= 1` is an assignment and not an OR, and status_flags 0x81 proves the `|= 0x40` is
            // an OR and not an assignment (correct result 0xc1; an assignment leaves 0x40, a dropped
            // write leaves 0x81).
            f.unit_at(PLAYER, IDS[i]).order_notify_status = 7;
            f.unit_at(PLAYER, IDS[i]).order_status_flags  = 0x81;
            // The SAME slots on another player, seeded identically: nothing may touch them.
            f.unit_at(OTHER, IDS[i]).order_notify_status = 7;
            f.unit_at(OTHER, IDS[i]).order_status_flags  = 0x81;
        }
        f.order_seq_id_by_player[PLAYER] = SEQ_START;
        f.order_seq_id_by_player[OTHER]  = 0x11;
        g_calls.observe_units            = f.units.data();

        {
            const issue_view v = f.view();
            // unused_param2 / unused_param3 given LOUD values: the original never reads them (EDX is
            // never read, EBX is clobbered by the loop's first instruction @0x004d7dc4), so if either
            // ever reached an emission it would show up as 0x7ffffffe / 0x7ffffffd rather than as a
            // plausible coordinate.
            api::group_move_formation_rotating(v, recording_sink(), recording_calls(),
                                               (uint32_t)PLAYER, 0x7ffffffe, 0x7ffffffd, TX, TY);
        }

        check("formation_move: one enqueue per scratch entry, in list order with the member id "
              "unscaled (0x004d7ded MOV EDX,[EDI*4+list]; 0x004d7dfd CALL)",
              g_calls.move_enqueues.size() == 3 && g_calls.move_enqueues[0].unit_idx == IDS[0] &&
                  g_calls.move_enqueues[1].unit_idx == IDS[1] &&
                  g_calls.move_enqueues[2].unit_idx == IDS[2]);
        check("formation_move: enqueue gets player in the low 16 bits and (x,y) = (target_x, "
              "target_y) NOT swapped (EAX=MOVZX SI @0x004d7df4, EBX=[EBP-0xc]=target_x @0x004d7dfa, "
              "ECX=[EBP+8]=target_y @0x004d7df7)",
              g_calls.move_enqueues.size() == 3 && g_calls.move_enqueues[0].player == PLAYER &&
                  g_calls.move_enqueues[0].x == (uint32_t)TX &&
                  g_calls.move_enqueues[0].y == (uint32_t)TY);
        check("formation_move: every member of ONE formation carries the SAME seq id, the value "
              "BEFORE the tail bump (MOVZX @0x004d7de5 reads it fresh per member; nothing in the "
              "loop writes it)",
              g_calls.move_enqueues.size() == 3 &&
                  g_calls.move_enqueues[0].move_flag == SEQ_START &&
                  g_calls.move_enqueues[1].move_flag == SEQ_START &&
                  g_calls.move_enqueues[2].move_flag == SEQ_START);
        check("formation_move: order_notify_status is ASSIGNED 1 over a pre-existing 7 (MOV byte "
              "[..+0xe8],1 @0x004d7dd5)",
              f.unit_at(PLAYER, IDS[0]).order_notify_status == 1 &&
                  f.unit_at(PLAYER, IDS[1]).order_notify_status == 1 &&
                  f.unit_at(PLAYER, IDS[2]).order_notify_status == 1);
        check("formation_move: order_status_flags is OR'd with 0x40, not assigned -- 0x81 becomes "
              "0xc1 (OR byte [..+0xe3],0x40 @0x004d7ddd)",
              f.unit_at(PLAYER, IDS[0]).order_status_flags == 0xc1 &&
                  f.unit_at(PLAYER, IDS[1]).order_status_flags == 0xc1 &&
                  f.unit_at(PLAYER, IDS[2]).order_status_flags == 0xc1);
        check("formation_move: both stamps land BEFORE the enqueue for that member, observed at "
              "call time rather than after the return (stores @0x004d7dd5/0x004d7ddd, CALL "
              "@0x004d7dfd)",
              g_calls.observed_notify_status == 1 && g_calls.observed_status_flags == 0xc1);
        check("formation_move: the player term is player*0x5b04 -- another player's SAME slots are "
              "untouched (IMUL EAX,ESI,0x5b04 @0x004d7dcf)",
              f.unit_at(OTHER, IDS[0]).order_notify_status == 7 &&
                  f.unit_at(OTHER, IDS[0]).order_status_flags == 0x81 &&
                  f.unit_at(OTHER, IDS[2]).order_status_flags == 0x81);
        check("formation_move: the tail bumps ONLY this player's seq id, by exactly one (INC byte "
              "[ESI+0x5d01e8] @0x004d7e0b)",
              f.order_seq_id_by_player[PLAYER] == (uint8_t)(SEQ_START + 1) &&
                  f.order_seq_id_by_player[OTHER] == 0x11);
        g_calls.observe_units = nullptr;
    }
    {
        // THE EMPTY LIST -- the case that separates a tail inside the loop from one after it. The
        // original's INC/JNZ/INC sequence (0x004d7e0b-0x004d7e1d) sits past the loop's exit edge and
        // runs on EVERY call, so a formation with no members still burns a sequence id.
        f.reset();
        f.ai_group_scratch_count             = 0;
        f.ai_group_scratch_list[0]           = 12; // present but out of range: must NOT be read
        f.order_seq_id_by_player[2]          = 0x40;
        f.unit_at(2, 12).order_notify_status = 7;

        const issue_view v = f.view();
        api::group_move_formation_rotating(v, recording_sink(), recording_calls(), 2u, 0, 0, 8, 9);

        check("formation_move: count 0 emits NOTHING and reads no list entry (CMP EDI,[count] / JC "
              "@0x004d7e03-09; the loop is entered only through that bottom test)",
              g_calls.move_enqueues.empty() && f.unit_at(2, 12).order_notify_status == 7);
        check("formation_move: the seq bump runs even on an EMPTY formation -- 0x40 -> 0x41 (the "
              "tail @0x004d7e0b is past the loop exit, not inside it)",
              f.order_seq_id_by_player[2] == 0x41);
        ++g_negative_cases;
    }
    {
        // THE WRAP. The tail is INC / JNZ-over / INC: the second increment fires only when the first
        // wrapped the byte to 0, so 0xff lands on 1 and 0 is never left live. The member's emitted
        // seq is still the PRE-bump 0xff, which is what separates "read before bump" from "after".
        f.reset();
        f.ai_group_scratch_count    = 1;
        f.ai_group_scratch_list[0]  = 61;
        f.order_seq_id_by_player[7] = 0xff;

        const issue_view v = f.view();
        api::group_move_formation_rotating(v, recording_sink(), recording_calls(), 7u, 0, 0, 3, 4);

        check("formation_move: seq 0xff wraps to 1, never to 0 (INC @0x004d7e0b, JNZ @0x004d7e11 "
              "skips the second INC @0x004d7e17 only when the first left it non-zero)",
              f.order_seq_id_by_player[7] == 1);
        check("formation_move: the order emitted on the wrapping call still carries the PRE-bump "
              "0xff (MOVZX @0x004d7de5 is inside the loop, the bump is after it)",
              g_calls.move_enqueues.size() == 1 && g_calls.move_enqueues[0].move_flag == 0xff &&
                  g_calls.move_enqueues[0].player == 7);
        ++g_negative_cases;
    }

    // ==============================================================================================
    // llm_strat_ai_unit_flag_and_move @0x004d7e22 and
    // llm_strat_ai_group_scatter_to_passable_tile @0x004d7e6b -- the 2026-08-28 re-home (batch B
    // unit `ai_move_primitives`). HAND-AUTHORED for the same structural reason as the formation
    // mover above: their sink is llm_strat_unit_order_move_enqueue, one level ABOVE the container,
    // so gen_order_issue_golden.py records ZERO sites and every emission is observed through
    // g_calls.move_enqueues. Both rows are T2 on those terms.
    {
        // flag_and_move: the whole body is two stamps and one enqueue. Distinct, non-symmetric
        // inputs: player 3 (exercises the *0x5b04 stride), x 41 != y 17 (a swap fails), unit 12.
        const int32_t PLAYER = 3;
        const int32_t OTHER  = 5;
        const int32_t ID = 12, TX = 41, TY = 17;

        f.reset();
        // Pre-set so the two stores must CHANGE them in a specific way: 7 proves `= 1` is an
        // assignment not an OR; 0x81 proves `|= 0x40` is an OR not an assignment (correct 0xc1).
        f.unit_at(PLAYER, ID).order_notify_status = 7;
        f.unit_at(PLAYER, ID).order_status_flags  = 0x81;
        f.unit_at(OTHER, ID).order_notify_status  = 7;
        f.unit_at(OTHER, ID).order_status_flags   = 0x81;
        f.order_seq_id_by_player[PLAYER]          = 0x27;
        g_calls.observe_units                     = f.units.data();

        {
            const issue_view v = f.view();
            api::unit_flag_and_move(v, recording_sink(), recording_calls(), (uint32_t)PLAYER, ID,
                                    (uint32_t)TX, (uint32_t)TY);
        }

        check("flag_and_move: exactly ONE enqueue, carrying the unit index UNSCALED and (x,y) not "
              "swapped (EDX survives the IMUL @0x004d7e41; EBX/ECX pass through untouched to CALL "
              "@0x004d7e60)",
              g_calls.move_enqueues.size() == 1 && g_calls.move_enqueues[0].unit_idx == ID &&
                  g_calls.move_enqueues[0].x == (uint32_t)TX &&
                  g_calls.move_enqueues[0].y == (uint32_t)TY);
        check("flag_and_move: the enqueue's fifth argument is a LITERAL ZERO, not a sequence id "
              "(PUSH 0x0 @0x004d7e58) -- this wrapper consumes no seq id at all",
              g_calls.move_enqueues.size() == 1 && g_calls.move_enqueues[0].move_flag == 0u);
        check("flag_and_move: order_notify_status is ASSIGNED 1 over a pre-existing 7 (MOV byte "
              "[..+0xe8],1 @0x004d7e4a)",
              f.unit_at(PLAYER, ID).order_notify_status == 1);
        check("flag_and_move: order_status_flags is OR'd with 0x40, not assigned -- 0x81 becomes "
              "0xc1 (OR byte [..+0xe3],0x40 @0x004d7e51)",
              f.unit_at(PLAYER, ID).order_status_flags == 0xc1);
        check("flag_and_move: both stamps land BEFORE the enqueue, observed AT CALL TIME rather "
              "than after the return (stores @0x004d7e4a/0x004d7e51, CALL @0x004d7e60)",
              g_calls.observed_notify_status == 1 && g_calls.observed_status_flags == 0xc1);
        check("flag_and_move: the player term is player*0x5b04 -- another player's SAME slot is "
              "untouched (IMUL EAX,EAX,0x5b04 @0x004d7e38)",
              f.unit_at(OTHER, ID).order_notify_status == 7 &&
                  f.unit_at(OTHER, ID).order_status_flags == 0x81);
        check("flag_and_move: it does NOT touch the order sequence id -- unlike the formation "
              "mover, there is no tail bump anywhere in its 0x49 bytes",
              f.order_seq_id_by_player[PLAYER] == 0x27);
        g_calls.observe_units = nullptr;
    }
    {
        // flag_and_move: THE PLAYER IS MASKED TO 4 BITS, twice and independently -- once for the
        // record stride (AND EAX,0xf @0x004d7e35) and once for the emitted argument (AND ESI,0xf
        // @0x004d7e5a). Driving it with player 0x13 (== 3 | 0x10) separates them: a translation that
        // dropped EITHER mask writes/emits for player 0x13 instead of 3.
        const int32_t ID = 61;
        f.reset();
        f.unit_at(3, ID).order_notify_status = 7;

        const issue_view v = f.view();
        api::unit_flag_and_move(v, recording_sink(), recording_calls(), 0x13u, ID, 8u, 9u);

        check("flag_and_move: player 0x13 stamps player 3's record -- the STRIDE mask is applied "
              "(AND EAX,0xf @0x004d7e35)",
              f.unit_at(3, ID).order_notify_status == 1 && f.unit_at(3, ID).order_status_flags == 0x40);
        check("flag_and_move: player 0x13 EMITS player 3 -- the ARGUMENT mask is applied separately "
              "(AND ESI,0xf @0x004d7e5a, MOVZX EAX,SI @0x004d7e5d)",
              g_calls.move_enqueues.size() == 1 && g_calls.move_enqueues[0].player == 3);
        ++g_negative_cases;
    }
    {
        // scatter_to_passable_tile: THE SPIRAL INDEX CARRIES ACROSS MEMBERS. `XOR ESI,ESI` runs once
        // at entry (0x004d7e89), OUTSIDE the loop, so member 1 resumes the spiral where member 0
        // stopped. This is the case a per-member reset would fail -- and only this one: with a reset,
        // every member would be handed the SAME tile and a one-member test would still pass.
        //
        // The table is laid out so each member needs a different number of steps, which also pins
        // that the search stops on the FIRST non-zero cell rather than scanning a fixed count:
        //   s=0 (dx,dy)=(0,0)   -> tile (10,20)  passable[.]=0  -> keep going
        //   s=1 (dx,dy)=(1,0)   -> tile (11,20)  passable[.]=1  -> MEMBER 0 lands here
        //   s=2 (dx,dy)=(0,1)   -> tile (10,21)  passable[.]=0  -> keep going
        //   s=3 (dx,dy)=(3,4)   -> tile (13,24)  passable[.]=1  -> MEMBER 1 lands here
        const int32_t PLAYER = 2;
        const int32_t AX = 10, AY = 20;
        const int32_t IDS[2] = {12, 5};

        f.reset();
        f.ai_group_scratch_count   = 2;
        f.ai_group_scratch_list[0] = IDS[0];
        f.ai_group_scratch_list[1] = IDS[1];
        const int8_t SPIRAL[8]     = {0, 0, 1, 0, 0, 1, 3, 4};
        for (int i = 0; i < 8; ++i) f.ai_tile_spiral_offsets[i] = SPIRAL[i];
        f.passable[((uint32_t)(AX + 1) << 8) | (uint32_t)AY]       = 1;
        f.passable[((uint32_t)(AX + 3) << 8) | (uint32_t)(AY + 4)] = 1;
        for (int i = 0; i < 2; ++i) {
            f.unit_at(PLAYER, IDS[i]).order_notify_status = 7;
            f.unit_at(PLAYER, IDS[i]).order_status_flags  = 0x81;
        }
        f.order_seq_id_by_player[PLAYER] = 0x27;
        g_calls.observe_units            = f.units.data();

        {
            const issue_view v = f.view();
            api::group_scatter_to_passable_tile(v, recording_sink(), recording_calls(),
                                                (uint32_t)PLAYER, AX, AY);
        }

        check("scatter: one enqueue per scratch entry, in list order with the id UNSCALED (MOV EDX,"
              "[EDX*4+list] @0x004d7eeb; CALL @0x004d7ef6)",
              g_calls.move_enqueues.size() == 2 && g_calls.move_enqueues[0].unit_idx == IDS[0] &&
                  g_calls.move_enqueues[1].unit_idx == IDS[1]);
        check("scatter: member 0 gets the FIRST passable tile found, (anchor + spiral[1]) = (11,20) "
              "-- the search stops on the first NON-zero cell (CMP byte ..,0x0 / JZ @0x004d7eb6-be)",
              g_calls.move_enqueues.size() == 2 && g_calls.move_enqueues[0].x == (uint32_t)(AX + 1) &&
                  g_calls.move_enqueues[0].y == (uint32_t)AY);
        check("scatter: member 1 gets a DIFFERENT tile, (13,24) -- the spiral index is NOT reset per "
              "member (XOR ESI,ESI @0x004d7e89 is outside the loop). A per-member reset would put "
              "member 1 on (11,20) with member 0",
              g_calls.move_enqueues.size() == 2 && g_calls.move_enqueues[1].x == (uint32_t)(AX + 3) &&
                  g_calls.move_enqueues[1].y == (uint32_t)(AY + 4));
        check("scatter: x and y are not swapped -- the anchor is asymmetric (10,20) and both deltas "
              "differ (EBX=x from AND [width_m] @0x004d7e98, ECX=y from AND [height_m] @0x004d7eae)",
              g_calls.move_enqueues.size() == 2 && g_calls.move_enqueues[0].x != g_calls.move_enqueues[0].y);
        check("scatter: order_notify_status ASSIGNED 1 and order_status_flags OR'd to 0xc1 on every "
              "member (0x004d7edb / 0x004d7ee2)",
              f.unit_at(PLAYER, IDS[0]).order_notify_status == 1 &&
                  f.unit_at(PLAYER, IDS[0]).order_status_flags == 0xc1 &&
                  f.unit_at(PLAYER, IDS[1]).order_notify_status == 1 &&
                  f.unit_at(PLAYER, IDS[1]).order_status_flags == 0xc1);
        check("scatter: the stamps land BEFORE that member's enqueue, observed at call time "
              "(stores @0x004d7edb/0x004d7ee2, CALL @0x004d7ef6)",
              g_calls.observed_notify_status == 1 && g_calls.observed_status_flags == 0xc1);
        check("scatter: the fifth enqueue argument is a LITERAL ZERO and the seq id is untouched "
              "(PUSH 0x0 @0x004d7ee9; no bump anywhere in the body)",
              g_calls.move_enqueues.size() == 2 && g_calls.move_enqueues[0].move_flag == 0u &&
                  g_calls.move_enqueues[1].move_flag == 0u &&
                  f.order_seq_id_by_player[PLAYER] == 0x27);
        g_calls.observe_units = nullptr;
    }
    {
        // scatter: THE TOROIDAL WRAP, and that the two masks are not interchanged. width_m 0x0f and
        // height_m 0x07 are DIFFERENT, so a translation that masked x with height_m (or vice versa)
        // lands somewhere else. Anchor (15,7) + (1,1) wraps to (0,0) under those masks.
        f.reset();
        f.width_m                   = 0x0fu;
        f.height_m                  = 0x07u;
        f.ai_group_scratch_count    = 1;
        f.ai_group_scratch_list[0]  = 3;
        f.ai_tile_spiral_offsets[0] = 1; // dx
        f.ai_tile_spiral_offsets[1] = 1; // dy
        // EVERY tile passable, deliberately. An earlier version set only the expected tile, which
        // made this case pass a mask-swap BY ACCIDENT: the swapped build missed the single target,
        // spiralled through a table of zeros, read past its end and eventually stumbled onto a
        // matching pair. With the whole plane passable, BOTH builds terminate at s = 0 and the case
        // compares the EMITTED coordinate instead of relying on which one finds a tile.
        std::fill(f.passable.begin(), f.passable.end(), (uint8_t)1);
        // ... except the tile spiral entry 1 (a (0,0) delta) resolves to. Without a reachable ZERO
        // cell an INVERTED passable test (`== 0` instead of `!= 0`) never terminates, walks off the
        // offsets table and kills the process -- which is red, but prints no verdict at all. With
        // this hole the inverted build stops here and emits (15,7), so the coordinate check below
        // names the failure instead.
        f.passable[(15u << 8) | 7u] = 0;

        const issue_view v = f.view();
        api::group_scatter_to_passable_tile(v, recording_sink(), recording_calls(), 1u, 15, 7);

        check("scatter: (15+1)&0x0f = 0 and (7+1)&0x07 = 0 -- x uses width_m and y uses height_m, "
              "and the two masks are DISTINCT so a swap is visible (AND EBX,[width_m] @0x004d7e98, "
              "AND ECX,EAX from [height_m] @0x004d7ea8-ae)",
              g_calls.move_enqueues.size() == 1 && g_calls.move_enqueues[0].x == 0u &&
                  g_calls.move_enqueues[0].y == 0u);
        ++g_negative_cases;
    }
    // scatter: THAT THE PLAYER IS *NOT* MASKED HERE (unlike unit_flag_and_move, which masks it
    // twice) IS ALSO NOT TESTABLE, and for a reason worth stating rather than re-deriving: the
    // difference only shows for player >= 0x10, and the real units array is map_object_unit[8][100].
    // A player index that large is out of range for the ORIGINAL too, so no valid game state can
    // distinguish the masked from the unmasked form. The asymmetry is documented in the header and
    // reproduced in the body; a test for it would have to index the fixture out of bounds, which is
    // a crash, not a check. Measured: the mutation that adds the mask leaves this suite green.
    //
    // scatter: THE SIGNEDNESS OF THE SPIRAL DELTAS IS NOT TESTABLE. MOVSX @0x004d7e8d/0x004d7e9e reads the
    // table as SIGNED bytes and the translation reproduces that -- but the result is then masked,
    // and the masks are `dim - 1` while the plane index is (x << 8) | y, so width_m and height_m are
    // never wider than 0xff. For any mask 2^k - 1 with k <= 8, (v + 255) mod 2^k == (v - 1) mod 2^k
    // because 256 == 0 mod 2^k -- an UNSIGNED read of -1 lands on exactly the same tile. Measured
    // rather than reasoned into: a mutation making both loads unsigned left this suite at 2475
    // checks / 0 failures. The property is real, reproduced, and undrivable; it would become
    // drivable only on a map wider than 256, which the plane's own indexing forbids -- so there is
    // no case to write here, and an attempt at one passes vacuously.
    {
        // scatter: THE EMPTY LIST. Unlike the formation mover there is no unconditional tail here,
        // so an empty call must be completely inert -- no emission, no stamp, and no spiral step.
        f.reset();
        f.ai_group_scratch_count             = 0;
        f.ai_group_scratch_list[0]           = 12; // present but out of range: must NOT be read
        f.unit_at(2, 12).order_notify_status = 7;
        f.order_seq_id_by_player[2]          = 0x40;

        const issue_view v = f.view();
        api::group_scatter_to_passable_tile(v, recording_sink(), recording_calls(), 2u, 5, 6);

        check("scatter: count 0 emits nothing, reads no list entry and leaves the seq id alone -- "
              "the loop is entered only through the bottom test (CMP/JC @0x004d7f01-07), and this "
              "body has NO unconditional tail",
              g_calls.move_enqueues.empty() && f.unit_at(2, 12).order_notify_status == 7 &&
                  f.order_seq_id_by_player[2] == 0x40);
        ++g_negative_cases;
    }

    printf("  wrappers compared: %d   sites compared: %d   negative cases: %d   fields UNPINNED: "
           "%d\n",
           g_wrappers_compared, g_sites_compared, g_negative_cases, g_unpinned);
    printf("  (golden holds %d site(s) across the whole domain; this suite drives the %d wrapper(s) "
           "reimplemented so far)\n",
           golden::SITE_COUNT, g_wrappers_compared);
    // A suite that drives nothing passes vacuously; say so as a failure rather than printing "0
    // failures" over an empty run.
    check("issuetest is NOT vacuous: at least one wrapper was actually compared",
          g_wrappers_compared > 0 && g_sites_compared > 0);

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
