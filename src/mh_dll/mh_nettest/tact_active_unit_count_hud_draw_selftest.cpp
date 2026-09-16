//
// tact_active_unit_count_hud_draw_selftest.cpp -- offline oracle for
// llm_tact_active_unit_count_hud_draw (TACT1E, 2026-08-28). See
// tact/tact_active_unit_count_hud_draw.h for the derivation.
//
// WHY OFFLINE: same llm_ui_text_draw_rgb16 font-blend twice-test hazard as
// llm_tact_ui_sidebar_row_draw_right/llm_tact_ui_sel_panel_init (tools/data/tact_shared_callees.json).
// Proven here via the fully-mockable calls-struct.
//
// WHAT THIS SUITE DOES NOT ASSERT, AND WHY (LIFT-TACT L4b). It cannot pin the
// pixel arguments of the draw tail: x=0x8a, clip 0x16x0x18, the packed colour 0xbeef on both lines,
// and a text-buffer trace proving each line re-formatted rather than redrawing the first result.
// Those arguments no longer exist in libmh -- the tail is one scope emit now, and the literals live
// in mh.dll's sink (seams/host_event_sink.cpp draw_active_count_hud). The checks were not moved
// somewhere else offline, because the sink calls the real game thunks and cannot run here; they are
// covered by the hosted oracle (the UI capture diff) instead. That is a REAL narrowing of offline
// coverage, and it is the intended effect of the lift rather than an oversight -- see the banner
// over kinds 10-13 in libmh_host_events.h.
//
// WHAT REPLACED THEM is the property the conversion can actually get wrong: the two counters are
// TACT_HASH_REGIONS members, so what must survive is that the readout carries the values the loop
// just computed, and that it is emitted BEFORE the margin clear. Both are asserted below.
//
#include "tact/tact_active_unit_count_hud_draw.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// The emit ORDER is load-bearing now (records dispatch where they are emitted), so the recorder
// keeps a sequence rather than only per-call counters.
enum emit_tag { EMIT_HUD    = 1,
                EMIT_MARGIN = 2 };

struct recorder {
    int      hud_calls           = 0;
    int32_t  hud_active_seen     = -1;
    int32_t  hud_cached_seen     = -1;
    int      vis_map_clear_calls = 0;
    emit_tag seq[4]              = {};
    int      seq_n               = 0;
};

recorder g_rec;

void reset() { g_rec = recorder{}; }

void note(emit_tag t) {
    if (g_rec.seq_n < 4) g_rec.seq[g_rec.seq_n] = t;
    ++g_rec.seq_n;
}

void mock_hud_readout_changed(int32_t active, int32_t cached) {
    ++g_rec.hud_calls;
    g_rec.hud_active_seen = active;
    g_rec.hud_cached_seen = cached;
    note(EMIT_HUD);
}
void mock_vis_map_clear_right_margin() {
    ++g_rec.vis_map_clear_calls;
    note(EMIT_MARGIN);
}

active_unit_count_hud_draw_calls mock_calls() {
    return {mock_hud_readout_changed, mock_vis_map_clear_right_margin};
}

// The emit shape every case shares: exactly one readout carrying the counters the loop produced,
// then exactly one margin clear, in that order.
void check_emit_shape(const tact_store &own, int32_t active, int32_t cached, const char *tag) {
    ck_eq((uint32_t)g_rec.hud_calls, 1u, tag);
    ck_eq((uint32_t)g_rec.hud_active_seen, (uint32_t)active,
          "readout carries active_unit_count, 0x00434e8b");
    ck_eq((uint32_t)g_rec.hud_cached_seen, (uint32_t)cached,
          "readout carries active_unit_count_cached, 0x00434ed2");
    ck_eq((uint32_t)g_rec.hud_active_seen, (uint32_t)const_cast<tact_store &>(own).active_unit_count(),
          "readout's active value IS the counter the loop just wrote, not a stale copy");
    ck_eq((uint32_t)g_rec.hud_cached_seen,
          (uint32_t)const_cast<tact_store &>(own).active_unit_count_cached(),
          "readout's cached value IS the counter the loop just wrote");
    ck_eq((uint32_t)g_rec.vis_map_clear_calls, 1u,
          "margin clear called exactly once, unconditional, 0x00434f19");
    ck_eq((uint32_t)g_rec.seq_n, 2u, "exactly two records leave this body");
    ck(g_rec.seq[0] == EMIT_HUD && g_rec.seq[1] == EMIT_MARGIN,
       "the readout is emitted BEFORE the margin clear -- the original draws, then clears");
}

} // namespace

void run_active_unit_count_hud_draw_tests() {
    // T1: the counting loop's gate and both counters, plus the draw scope firing exactly once with
    // the counts it just produced. Seeds: slot 1 owned+alive+selected (counts both), slot 2
    // owned+alive+NOT selected (counts active only), slot 3 owner!=0 (excluded), slot 4 type==0
    // (excluded), slot 5 type==0x80 (excluded, boundary), slot 6 type==0x7f (INCLUDED, boundary).
    // 0x00434dda-0x00434f19.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1; // default: not owned, excluded
            fx.units[i].type  = 0;
        }
        fx.units[1]              = {};
        fx.units[1].owner        = 0;
        fx.units[1].type         = 5;
        fx.units[1].status       = 1; // selected -> counts toward cached too
        fx.units[2]              = {};
        fx.units[2].owner        = 0;
        fx.units[2].type         = 5;
        fx.units[2].status       = 0; // not selected -> active only
        fx.units[3]              = {};
        fx.units[3].owner        = 1; // excluded: owner != 0
        fx.units[3].type         = 5;
        fx.units[3].status       = 1;
        fx.units[4]              = {};
        fx.units[4].owner        = 0;
        fx.units[4].type         = 0; // excluded: type == 0
        fx.units[4].status       = 1;
        fx.units[5]              = {};
        fx.units[5].owner        = 0;
        fx.units[5].type         = 0x80; // excluded: type >= 0x80 boundary
        fx.units[5].status       = 1;
        fx.units[6]              = {};
        fx.units[6].owner        = 0;
        fx.units[6].type         = 0x7f; // included: type < 0x80 boundary
        fx.units[6].status       = 0;
        fx.sel_panel_icon_gfx[0] = reinterpret_cast<void *>(0x3333);
        reset();

        tact_store own = fx.store();
        detail::active_unit_count_hud_draw(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.active_unit_count(), 3u,
              "T1: active count = slots 1,2,6 + reset-then-recount, 0x00434dee-0x00434e5d");
        ck_eq((uint32_t)own.active_unit_count_cached(), 1u,
              "T1: cached count = only slot 1 (status bit 0 set)");
        check_emit_shape(own, 3, 1, "T1: the HUD readout scope is emitted exactly once");
    }

    // T2: the all-quiet case -- zero eligible units resets both counters to 0 and STILL emits (the
    // draw path is unconditional, not gated on the count). This is the case a naive "only notify on
    // change" conversion would break.
    {
        tact_fixture fx;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        reset();

        tact_store own = fx.store();
        detail::active_unit_count_hud_draw(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.active_unit_count(), 0u, "T2: zero eligible units -> count resets to 0");
        ck_eq((uint32_t)own.active_unit_count_cached(), 0u, "T2: cached also resets to 0");
        check_emit_shape(own, 0, 0, "T2: the readout still fires with both counts zero");
    }

    // T3: RESET semantics -- a stale nonzero value already in the counters must be overwritten, not
    // accumulated onto (this function is the PRODUCER, not a running accumulator).
    {
        tact_fixture fx;
        fx.active_unit_count        = 999;
        fx.active_unit_count_cached = 999;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            fx.units[i].owner = 1;
            fx.units[i].type  = 0;
        }
        fx.units[TACT_UNIT_FIRST_SLOT]        = {};
        fx.units[TACT_UNIT_FIRST_SLOT].owner  = 0;
        fx.units[TACT_UNIT_FIRST_SLOT].type   = 1;
        fx.units[TACT_UNIT_FIRST_SLOT].status = 0;
        reset();

        tact_store own = fx.store();
        detail::active_unit_count_hud_draw(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.active_unit_count(), 1u,
              "T3: stale 999 is RESET then recounted to 1, not accumulated to 1000");
        ck_eq((uint32_t)own.active_unit_count_cached(), 0u, "T3: cached likewise reset then recounted");
        // The stale 999 must not survive into the record either -- the emit reads the counters
        // AFTER the loop, which is what makes it safe to move the formatting host-side.
        check_emit_shape(own, 1, 0, "T3: the readout carries the RECOUNTED values, not the stale ones");
    }
}

} // namespace mh::tact::test
