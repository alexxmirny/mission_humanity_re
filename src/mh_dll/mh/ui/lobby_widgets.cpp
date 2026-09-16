//
// mh/ui -- the lobby/browser WIDGET fixups: geometry, slot rows, one dialog, one format string.
//
// Every fixup in this file is here because its body reads only the game's own widget/slot records
// and touches nothing on the wire. The ROLE and SESSION questions that decide whether some of them
// should run at all are answered by the net-owned lobby-dispatch seam, which calls in.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "ui/ui_internal.h"
#include "addr/mh_addrs.gen.h" // generated EN VAs (tools/gen_dll_addrs.py)
#include "hook/detour.h"       // install_trampoline (shared inline-detour toolkit)
#include "hook/patch.h"        // patch_bytes_guarded (S7 browser-row format string)
#include "hook/watcall.h"      // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::call_watcall1;
using mh::hook::entry_claim;
using mh::hook::install_trampoline;
using mh::hook::patch_bytes_guarded;
using mh::ui::detail::ui_log;
using mh::ui::detail::ui_verbose;

namespace {

// ---- U3 + U7: settle the client's lobby frame on-screen ------------------------------------------
//
// The lobby renders BLANK on the client when its menu slide-in has not yet run: the frame widget
// (0x651293) is still at the slide-OUT end (x = -683), so llm_ui_widget_list_center folds that into
// the container draw-origin ((640-363)/2 + (-683) = -545) and the whole (healthy) lobby panel blits
// ~545px off-screen left. Root cause pinned via TTD (tools/ttd/; U3).
//
// U7 is the same defect on the right panel: its x (0x6512f3) is DERIVED from the frame's inside the
// slide loop, so at the slide-OUT start it sits at 1481 -- off-screen right on a 640-wide render --
// and the client shows NO map-info/Start panel. Both are the slide's IN end state, which is why this
// asks lobby_slide.cpp for it rather than re-deriving it (ui_internal.h, slide_end_state).
// Self-limiting: once settled the geometry test is false, so this is a compare per lobby frame.
//
// The Start button (0x6502a3) keeps its 0x80 HIDDEN bit, so Start stays host-only.
constexpr int PARKED_FRAME_X = -400; // anything left of this is the slide-OUT end (~-683)

} // namespace

namespace mh {
namespace ui {

void lobby_frame_snap_on_screen() {
    // The active screen must BE the lobby: the frame widget is shared with the browsers and the map
    // picker, and settling it while one of those is up would fight their own slides.
    if (*(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != (void *)mh::addr::lobby_widget_origin) return;
    int *frame_x = (int *)mh::addr::lobby_frame_x; // frame widget +0x1c
    if (*frame_x >= PARKED_FRAME_X) return;        // already on-screen

    int frame_end = 0, right_end = 0;
    if (detail::slide_end_state(detail::SLIDE_IN, &frame_end, &right_end)) {
        *frame_x                              = frame_end;
        *(int *)mh::addr::lobby_right_panel_x = right_end;
    } else {
        // DEGENERATE, and kept because it is what shipped: when a sprite width is unusable the
        // shared helper declines, but this site has always fallen back per-width -- the frame to the
        // boot map's -124, the right panel left alone. Unifying the two fallbacks is an F4 question
        // (it has never been observed: every measured run reads refw=248, leftw=363).
        const int refw  = *(const int *)mh::addr::lobby_frame_ref_width;
        *frame_x        = (refw > 0 && refw < 1024) ? -(refw / 2) : -124;
        const int leftw = *(const int *)mh::addr::lobby_left_frame_width;
        if (leftw > 0 && leftw < 1024) *(int *)mh::addr::lobby_right_panel_x = leftw;
    }
    // center() reads frame.x into the container origin, and the steady lobby-draw path never calls
    // it -- so we must.
    call_watcall1(mh::addr::llm_ui_widget_list_center, (void *)mh::addr::lobby_widget_origin);
    if (ui_verbose())
        ui_log("; U3FIX client lobby frame snapped on-screen + re-centered (U7: right panel too)\n");
}

// ---- U8 Gap 2: refresh the lobby display for the host's REAL map ---------------------------------
//
// The client adopts the host's map OUT OF BAND (the recv seam copies current_map_data straight
// over), which BYPASSES the retail map-transfer path. So the two surfaces retail refreshes from that
// path stay frozen at the STUB map the lobby was first built from (tutorial / 64x64 / 2 players)
// even though current_map_data is now the real map (e.g. blue monday / 256x256 / 8p):
//   (a) the map-info panel text -- formatted by llm_lobby_map_transfer_finalize (0x4c01df,
//       __stdcall, reads current_map_data into the static buffer the panel widget points at);
//   (b) the slot rows -- built by llm_lobby_build_slot_widgets (0x4bf497) for
//       current_map_data.field2_0x8, so a 2p stub yields 2 rows on an 8p map.
// Re-run BOTH, ONCE per distinct map, on the main (frame) thread right before the retail dispatch
// body renders. The caller gates on MH_MP_MapReceived (set with release AFTER the recv-thread copy
// completes, so we never read a torn current_map_data); the player-count latch is here because it is
// a property of what is DISPLAYED. Rebuild mirrors the retail dispatch's own pattern
// (WIDGETS_BUILT=0 -> build -> =1); with =0 the build also re-resolves
// _G_LLM_LOBBY_LOCAL_SLOT_INDEX from the slot player_ids. (U8 Gap 2)
//
// U10 audit (C3): KEEP -- this forced refresh is the direct consequence of adopting the host's map
// out of band. The retail map-transfer path that would drive finalize+rebuild natively (chunk types
// 0x0f/0x10/0x11) is dead-stubbed BY DESIGN (the MP bootstrap notes), so there is no reachable retail
// path to ride here -- load-bearing, not fighting retail. See the MP seam audit.
void lobby_refresh_for_map() {
    constexpr uintptr_t A_MAP_PCOUNT    = mh::addr::current_map_player_count; // current_map_data.field2_0x8
    constexpr uintptr_t A_WIDGETS_BUILT = mh::addr::_G_LLM_LOBBY_WIDGETS_BUILT;
    const int           pcount          = *(const int *)A_MAP_PCOUNT;
    static int          g_shown_pcount  = 0;
    if (pcount < 1 || pcount == g_shown_pcount) return;
    g_shown_pcount = pcount;
    ((void(__stdcall *)())mh::addr::llm_lobby_map_transfer_finalize)(); // -> map-info panel
    *(unsigned char *)A_WIDGETS_BUILT = 0;                              // rebuild the slot rows for the real count
    ((void (*)())mh::addr::llm_lobby_build_slot_widgets)();             //   mirrors dispatch
    *(unsigned char *)A_WIDGETS_BUILT = 1;
    if (ui_verbose())
        ui_log("; U8-Gap2 refreshed lobby map-info + rebuilt slot rows for the real map\n");
}

} // namespace ui
} // namespace mh

namespace {

// ---- the browser scrollbar guard ----------------------------------------------------------------
//
// The retail rescan binds the range (curptr [param_block+8], max [param_block+4], min [param_block])
// only on its SECOND tick, so before that -- which on an always-empty discovery list is forever --
// curptr is NULL (deref crash) and max==min (div-by-zero). Run the original ONLY when the range is
// bound and non-degenerate; otherwise skip the thumb draw this frame. EAX is preserved for the
// original. Purely defensive draw skip -> no state/determinism effect.
void *g_c18a6_tramp = nullptr;
// clang-format off
__declspec(naked) void scrollbar_guard_detour() {
    __asm {
        mov  edx, [eax + 0x30] // param_block
        test edx, edx
        jz   sb_skip
        mov  ecx, [edx + 8] // curptr
        test ecx, ecx
        jz   sb_skip
        mov  ecx, [edx + 4]
        sub  ecx, [edx] // max - min
        jz   sb_skip
        jmp  dword ptr [g_c18a6_tramp] // safe -> run the original draw (EAX intact)
      sb_skip:
        ret // empty/unbound scrollbar -> skip the thumb draw this frame
    }
}
// clang-format on

// ---- the host-Start clear-loop fix (2026-07-11) -------------------------------------------------
//
// Setting a lobby slot to AI calls llm_lobby_peer_table_add_ai, which INCREMENTS the peer-table
// count DAT_0065f692. On "Start", the clear fn runs
// `while (0 < DAT_0065f692) llm_lobby_peer_slot_remove(DAT_0065da7a)` -- but peer_slot_remove NEVER
// decrements that count (verified: add_ai is its only writer), so with N AI players the loop spins
// forever (100% CPU, main thread stuck in the assert_stack_capacity probe -> looks hung). The AI
// game players come from _G_LLM_LOBBY_SLOTS (read by build_players_from_slots), NOT this peer table,
// so zeroing the count at the clear fn's entry breaks the loop without losing them (and the loop, if
// it DID run, would wrongly remove the AI's lobby slots). Run-before hook: zero the count, then run
// the original (loop now does 0 iterations).
constexpr uintptr_t ADDR_PEER_CLEAR_FN = mh::addr::peer_table_clear_fn; // the broken clear loop
constexpr uintptr_t ADDR_PEER_COUNT    = mh::addr::peer_table_count;    // add-only guard-86 count
void               *g_pc_tramp         = nullptr;
void                on_peer_clear() {
    *(volatile int *)ADDR_PEER_COUNT = 0;
}
// clang-format off
__declspec(naked) void peer_clear_detour() {
    __asm {
        pushad
        pushfd
        call on_peer_clear
        popfd
        popad
        jmp  dword ptr [g_pc_tramp] // stolen 8-byte prologue + jmp back
    }
}
// clang-format on

// ---- N2: host-protect guard on llm_lobby_remove_player_slot (2026-07-22) ------------------------
//
// Invariant: player 0 IS the host and must NEVER be removed. remove_player_slot(player_id) and its
// caller peer_slot_remove are BOTH player_id-keyed, first-match, with NO status gate. The retail
// AI-add leaves the AI slot's player_id = 0 (uninitialized), and the host is slot0/id0, so when the
// host cycles an AI slot -> "closed", slot_cycle_cb calls peer_slot_remove(0) ->
// remove_player_slot(0), which finds slot0 (the HOST) first and compacts it -> the host removes
// ITSELF (reproduced live). We keep the AI at id 0 by design (its id never reaches gameplay --
// build_players forces AI scenario_side_id=-1 -- and 0 never collides with the host-assigned humans
// 1..k), so instead of assigning the AI a throwaway id we just forbid removing player 0.
// remove_player_slot is __watcall (player_id in EAX); run-before at the entry: if EAX==0, RET (skip
// the whole body -> host untouched); else jmp the stolen-prologue tramp into the original. The AI's
// own slot is still removed correctly by slot INDEX via slot_cycle_cb's slot_compact(user_data). Our
// U12 human-leave vacate only ever calls remove_player_slot(p>=1), so it is unaffected. EAX preserved.
//
// COVERAGE (F3E-COV, 2026-09-13): the PASS-THROUGH is covered -- over-blocking reds
// `leave_frees_slot`. The REFUSAL branch is measured UNREACHABLE from any UI walk on this build (a
// [trace] funcs=0x4bf59f,0x4bf513,0x4bf65d run of ai_closed_start never calls peer_slot_remove at
// all), which is NOT the same as dead: re-run that trace before deleting this guard.
// The measurement is recorded with the UI-testing suite.
constexpr uintptr_t ADDR_REMOVE_PLAYER_SLOT_FN = mh::addr::llm_lobby_remove_player_slot;
void               *g_rps_tramp                = nullptr;
// clang-format off
__declspec(naked) void remove_player_slot_guard() {
    __asm {
        test eax, eax
        jz   rps_skip
        jmp  dword ptr [g_rps_tramp] // player_id != 0 -> run the original (stolen prologue + jmp +8; EAX intact)
      rps_skip:
        ret // player 0 == the host -> never remove it
    }
}
// clang-format on

} // namespace

namespace mh {
namespace ui {

bool install_scrollbar_guard() {
    return install_trampoline(mh::addr::browser_scrollbar_draw_cb, (void *)scrollbar_guard_detour,
                              &g_c18a6_tramp, 8, entry_claim::exclusive, "the browser scrollbar guard");
}

bool install_peer_clear_fix() {
    return install_trampoline(ADDR_PEER_CLEAR_FN, (void *)peer_clear_detour, &g_pc_tramp, 8,
                              entry_claim::exclusive, "the host-Start clear-loop fix");
}

bool install_remove_player_slot_guard() {
    return install_trampoline(ADDR_REMOVE_PLAYER_SLOT_FN, (void *)remove_player_slot_guard, &g_rps_tramp,
                              8, entry_claim::exclusive, "the N2 host-slot0 protect guard");
}

// ---- Phase 2c: suppress the spurious self-removal modal at entry --------------------------------
//
// During the lobby->game handoff the client transiently receives a "you (local player) were removed"
// message (case '\f' of llm_lobby_host_net_dispatch), whose ONLY effect is a modal error dialog
// (text 0x2c1) at call site 0x004bf974, followed by `goto` dispatch-end -- no teardown; the client
// goes on to sync and play (confirmed: caller=0x004bf979, sess=2, gclk=0). NOP just that 5-byte CALL
// (E8 rel32 -> the shared llm_ui_dlg_savegame_io_error) so the spurious popup never shows. This is
// the ONLY caller with text 0x2c1; the other 15 error-dialog sites (map/save/load) are untouched.
//
// Guarded on the E8 OPCODE ONLY, which is weaker than this DLL's usual expected-bytes discipline --
// the rel32 is build-specific and nothing in the generated headers carries it. Tightening it to a
// full 5-byte expectation is an F4 item (it needs the displacement in the addr manifest); the site
// is a fixed VA behind the EN build gate, so the opcode test is the second layer, not the only one.
bool suppress_self_removal_dialog() {
    uint8_t *call = (uint8_t *)mh::addr::removed_popup_call_site; // the 0x2c1 "you were removed" CALL
    if (call[0] != 0xE8) return false;                            // CALL rel32
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    for (int i = 0; i < 5; ++i) call[i] = 0x90; // NOP
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

// ---- S7: the retail browser-row count format ----------------------------------------------------
//
// Both row renderers (llm_mp_session_browser_rescan / _discovery_browser_refresh) format the NORMAL
// branch with the wide string at browser_row_count_fmt (0x503068) = u"%s\t%d+%d/%d" and args
// (name, total_slots - player_count, player_count, max_players) -> "open+filled/max" = "7+1/8" for a
// 1-of-8 game (reads near-full). Patch the format in place to u"%s\t%d/%d" (SHORTER; the trailing
// wchars are NUL-padded) so it consumes only (name, first, second) and renders exactly
// "first/second". build_browser_from_store then loads the synth record so first = occ (occupied incl
// AI) and second = cap (non-closed capacity) -> the row reads "occ/cap".
// Guarded on the exact original bytes (ship-safe no-op on any other build / if already patched).
bool patch_browser_row_format() {
    static const uint8_t ROWFMT_EXPECT[24] = {0x25, 0x00, 0x73, 0x00, 0x09, 0x00, 0x25, 0x00,
                                              0x64, 0x00, 0x2b, 0x00, 0x25, 0x00, 0x64, 0x00,
                                              0x2f, 0x00, 0x25, 0x00, 0x64, 0x00, 0x00, 0x00};
    static const uint8_t ROWFMT_PATCH[24]  = {0x25, 0x00, 0x73, 0x00, 0x09, 0x00, 0x25, 0x00,
                                              0x64, 0x00, 0x2f, 0x00, 0x25, 0x00, 0x64, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    return patch_bytes_guarded(mh::addr::browser_row_count_fmt, ROWFMT_EXPECT, ROWFMT_PATCH, 24);
}

} // namespace ui
} // namespace mh
