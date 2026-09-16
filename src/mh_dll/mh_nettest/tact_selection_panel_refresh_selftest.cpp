#include "tact/tact_selection_panel_refresh.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// The background plate's nine arguments (LIFT-TACT L4b) -- surface, pitch, the
// (0,0xf0) anchor, the (0,0x48,0xa0,0xf0) clip and the sel_panel_icon_gfx_at(1) raw pointer -- no
// longer cross the boundary, so this suite can no longer assert them; they are verbatim in
// seams/host_event_sink.cpp draw_sel_panel_bg and proven by the hosted UI capture. What this suite
// asserts instead is the property the conversion CAN break, and which the argument checks never
// covered: the two scopes stay two, in the original ORDER, with the gated middle between them.
enum step_tag { STEP_BG = 1,
                STEP_ROSTER,
                STEP_SIDEBAR_ROSTER,
                STEP_SIDEBAR_ROWS,
                STEP_PANEL,
                STEP_HUD };

struct recorder {
    int      sel_panel_bg_calls               = 0;
    int      squad_roster_refresh_calls       = 0;
    int      ui_sidebar_roster_refresh_calls  = 0;
    int      ui_sidebar_draw_rows_calls       = 0;
    int      ui_sel_panel_draw_calls          = 0;
    int      active_unit_count_hud_draw_calls = 0;
    step_tag seq[8]                           = {};
    int      seq_n                            = 0;
};

recorder g_rec;

void reset() { g_rec = recorder{}; }

void note(step_tag t) {
    if (g_rec.seq_n < 8) g_rec.seq[g_rec.seq_n] = t;
    ++g_rec.seq_n;
}

void mock_sel_panel_bg() {
    ++g_rec.sel_panel_bg_calls;
    note(STEP_BG);
}
void mock_squad_roster_refresh() {
    ++g_rec.squad_roster_refresh_calls;
    note(STEP_ROSTER);
}
void mock_ui_sidebar_roster_refresh() {
    ++g_rec.ui_sidebar_roster_refresh_calls;
    note(STEP_SIDEBAR_ROSTER);
}
void mock_ui_sidebar_draw_rows() {
    ++g_rec.ui_sidebar_draw_rows_calls;
    note(STEP_SIDEBAR_ROWS);
}
void mock_ui_sel_panel_draw() {
    ++g_rec.ui_sel_panel_draw_calls;
    note(STEP_PANEL);
}
void mock_active_unit_count_hud_draw() {
    ++g_rec.active_unit_count_hud_draw_calls;
    note(STEP_HUD);
}

selection_panel_refresh_calls mock_calls() {
    return {mock_sel_panel_bg, mock_squad_roster_refresh,
            mock_ui_sidebar_roster_refresh, mock_ui_sidebar_draw_rows,
            mock_ui_sel_panel_draw, mock_active_unit_count_hud_draw};
}

// The whole body is five steps in a fixed order; only the middle one is gated. This is what makes
// the two-kinds-not-one decision testable: if the background scope were merged with the panel
// scope, STEP_BG would move past the gated middle and this check would red.
void check_sequence(step_tag gated, const char *tag) {
    ck_eq((uint32_t)g_rec.seq_n, 5u, tag);
    ck(g_rec.seq[0] == STEP_BG,
       "the background plate is emitted FIRST, before squad_roster_refresh, 0x00434b0f");
    ck(g_rec.seq[1] == STEP_ROSTER, "squad_roster_refresh() runs second, 0x00434b3a");
    ck(g_rec.seq[2] == gated, "the gated sidebar step runs third, 0x00434b3f");
    ck(g_rec.seq[3] == STEP_PANEL,
       "the panel-contents scope is emitted AFTER the gated middle, not merged with the "
       "background scope, 0x00434b54");
    ck(g_rec.seq[4] == STEP_HUD, "active_unit_count_hud_draw() runs last, 0x00434b5e");
}

} // namespace

// ---- T5: the inv 6 R3b hoist (LIFT-R3B -> LIFT-TACT) ---------------------------------------
//
// roster_slots_rebuild is the STATE half of llm_tact_ui_sidebar_roster_refresh @0x00434b68, lifted
// into libmh above the INV_TACT_SIDEBAR_ROSTER emit because llm_tact_sidebar_dispatch reads all
// three of its outputs back later in the same frame and turns a slot id into a hashed
// tact_units.status write. These pin the outputs, not the fact that it was called: a hoist that
// runs and computes the wrong thing is the failure this is guarding, and "it was invoked" would
// pass that.
void run_roster_slots_rebuild_tests() {
    // T5a: the rebuild is a COMPACTION, not a copy. Live units are status bit 0 set AND type != 0;
    // the gaps close up, and the tail of the 64-slot array is zero-filled. 0x00434b7c-0x00434bc9.
    {
        tact_fixture fx;
        fx.sidebar_slot_visible_count = 5;
        fx.sidebar_slot_scroll        = 0;
        for (auto &id : fx.sidebar_slot_unit_ids) id = 0x7f; // poison: a stale map must be overwritten
        fx.units[3].status  = 1;
        fx.units[3].type    = 4; // live
        fx.units[5].status  = 1;
        fx.units[5].type    = 0; // type 0 -> NOT live
        fx.units[9].status  = 0;
        fx.units[9].type    = 7; // status bit clear -> NOT live
        fx.units[11].status = 3;
        fx.units[11].type   = 2; // bit 0 set among other bits -> live

        tact_store own = fx.store();
        detail::roster_slots_rebuild(fx.view(), own);

        ck_eq((uint32_t)fx.sidebar_slot_unit_ids[0], 3u, "T5a: first live unit compacts to slot 0");
        ck_eq((uint32_t)fx.sidebar_slot_unit_ids[1], 11u,
              "T5a: the second live unit is 11 -- 5 (type 0) and 9 (status bit clear) are skipped");
        ck_eq((uint32_t)fx.sidebar_slot_unit_ids[2], 0u, "T5a: the tail is zero-filled, not left stale");
        ck_eq((uint32_t)fx.sidebar_slot_unit_ids[63], 0u, "T5a: zero-fill reaches the last slot, 0x40");
    }

    // T5b: the scroll clamp. It only engages when the live count is below scroll+visible, and it
    // floors at 0 -- both halves of 0x00434bc9-0x00434bf5, which Ghidra renders as one comma
    // expression and which a tidied translation would get wrong in opposite directions.
    {
        tact_fixture fx;
        fx.sidebar_slot_visible_count = 5;
        fx.sidebar_slot_scroll        = 9; // 0 live units -> 0-5 = -5 -> floored to 0
        tact_store own                = fx.store();
        detail::roster_slots_rebuild(fx.view(), own);
        ck_eq((uint32_t)fx.sidebar_slot_scroll, 0u, "T5b: an empty roster floors the scroll at 0");
    }
    {
        tact_fixture fx;
        fx.sidebar_slot_visible_count = 2;
        fx.sidebar_slot_scroll        = 1;
        for (int i = 1; i <= 8; ++i) {
            fx.units[i].status = 1;
            fx.units[i].type   = 1;
        } // 8 live
        tact_store own = fx.store();
        detail::roster_slots_rebuild(fx.view(), own);
        ck_eq((uint32_t)fx.sidebar_slot_scroll, 1u,
              "T5b: 8 live >= scroll+visible, so the clamp does NOT engage and scroll is untouched");
    }

    // T5c: the four scroll-arrow icon states -- the other pair sidebar_dispatch reads back
    // (tact_sidebar_dispatch.cpp:130). Bit 1 is the "can scroll" mark; bit 0 is preserved, which is
    // why the clear arm is `&= 1` and not `= 0`. 0x00434bf5-0x00434c8d.
    {
        tact_fixture fx;
        fx.sidebar_slot_visible_count = 2;
        fx.sidebar_slot_scroll        = 0;
        for (auto &st : fx.sel_panel_icon_slot_state) st = 3; // both bits set going in
        for (int i = 1; i <= 2; ++i) {
            fx.units[i].status = 1;
            fx.units[i].type   = 1;
        } // exactly 2 live
        tact_store own = fx.store();
        detail::roster_slots_rebuild(fx.view(), own);
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[0], 1u,
              "T5c: scroll==0 -> the UP pair drops bit 1 and KEEPS bit 0");
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[1], 1u, "T5c: both UP slots move together");
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[2], 1u,
              "T5c: slot[visible+scroll] is 0 (only 2 live) -> the DOWN pair drops bit 1 too");
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[3], 1u, "T5c: both DOWN slots move together");
    }
    {
        tact_fixture fx;
        fx.sidebar_slot_visible_count = 2;
        fx.sidebar_slot_scroll        = 1;
        for (auto &st : fx.sel_panel_icon_slot_state) st = 0;
        for (int i = 1; i <= 8; ++i) {
            fx.units[i].status = 1;
            fx.units[i].type   = 1;
        }
        tact_store own = fx.store();
        detail::roster_slots_rebuild(fx.view(), own);
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[0], 2u, "T5c: scroll>0 -> the UP pair sets bit 1");
        ck_eq((uint32_t)fx.sel_panel_icon_slot_state[2], 2u,
              "T5c: slot[visible+scroll] is live -> the DOWN pair sets bit 1");
    }
}

void run_selection_panel_refresh_tests() {
    // T1: single-select mode (ui_sel_panel_multi_mode == 0) -> ui_sidebar_roster_refresh() fires,
    // ui_sidebar_draw_rows() does NOT. The other four calls fire unconditionally exactly once each,
    // and set_draw_surface's nine arguments match the header's own derivation bit-for-bit.
    // 0x00434b0f-0x00434b63.
    {
        tact_fixture fx;
        fx.ui_sel_panel_multi_mode = 0;
        // Distinct, non-symmetric slot values so consuming the WRONG slot (0 instead of 1) fails.
        fx.sel_panel_icon_gfx[0] = reinterpret_cast<void *>(0x1111);
        fx.sel_panel_icon_gfx[1] = reinterpret_cast<void *>(0x2222);
        reset();

        tact_store own = fx.store();
        detail::selection_panel_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)g_rec.sel_panel_bg_calls, 1u,
              "T1: the background scope is emitted exactly once, 0x00434b0f");
        check_sequence(STEP_SIDEBAR_ROSTER, "T1: exactly five steps leave this body");
        ck_eq((uint32_t)g_rec.squad_roster_refresh_calls, 1u, "T1: squad_roster_refresh(), 0x00434b3a");
        ck_eq((uint32_t)g_rec.ui_sidebar_roster_refresh_calls, 1u,
              "T1: multi_mode==0 -> ui_sidebar_roster_refresh() fires, 0x00434b3f");
        ck_eq((uint32_t)g_rec.ui_sidebar_draw_rows_calls, 0u,
              "T1: multi_mode==0 -> ui_sidebar_draw_rows() does NOT fire");
        ck_eq((uint32_t)g_rec.ui_sel_panel_draw_calls, 1u, "T1: ui_sel_panel_draw(), 0x00434b54");
        ck_eq((uint32_t)g_rec.active_unit_count_hud_draw_calls, 1u,
              "T1: active_unit_count_hud_draw(), 0x00434b5e");
    }

    // T2: multi-select mode (ui_sel_panel_multi_mode != 0) -> the gate flips: ui_sidebar_draw_rows()
    // fires instead of ui_sidebar_roster_refresh(). Every other call is unaffected by the gate.
    {
        tact_fixture fx;
        fx.ui_sel_panel_multi_mode = 1;
        reset();

        tact_store own = fx.store();
        detail::selection_panel_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)g_rec.ui_sidebar_roster_refresh_calls, 0u,
              "T2: multi_mode!=0 -> ui_sidebar_roster_refresh() does NOT fire");
        ck_eq((uint32_t)g_rec.ui_sidebar_draw_rows_calls, 1u,
              "T2: multi_mode!=0 -> ui_sidebar_draw_rows() fires instead, 0x00434b4c");
        ck_eq((uint32_t)g_rec.squad_roster_refresh_calls, 1u, "T2: squad_roster_refresh() still fires");
        ck_eq((uint32_t)g_rec.ui_sel_panel_draw_calls, 1u, "T2: ui_sel_panel_draw() still fires");
        ck_eq((uint32_t)g_rec.active_unit_count_hud_draw_calls, 1u,
              "T2: active_unit_count_hud_draw() still fires");
        check_sequence(STEP_SIDEBAR_ROWS, "T2: five steps, with the OTHER gated middle");
    }

    // T3: a large, arbitrary multi_mode value (not just 1) takes the SAME branch as T2 -- the gate
    // is `== 0`, not `== 1`, so any nonzero value must select ui_sidebar_draw_rows().
    {
        tact_fixture fx;
        fx.ui_sel_panel_multi_mode = -7;
        reset();

        tact_store own = fx.store();
        detail::selection_panel_refresh(fx.view(), own, mock_calls());

        ck_eq((uint32_t)g_rec.ui_sidebar_roster_refresh_calls, 0u,
              "T3: any nonzero multi_mode takes the draw_rows branch");
        ck_eq((uint32_t)g_rec.ui_sidebar_draw_rows_calls, 1u, "T3: draw_rows() fires for multi_mode=-7");
    }

    run_roster_slots_rebuild_tests();
}

} // namespace mh::tact::test
