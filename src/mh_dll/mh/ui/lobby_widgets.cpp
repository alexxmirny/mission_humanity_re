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
#include <string.h>

#include "ui/ui_internal.h"
#include "ui/player_strings.h" // mp:U51: the TEAM / MODE option texts
#include "addr/mh_addrs.gen.h" // generated EN VAs (tools/gen_dll_addrs.py)
#include "hook/detour.h"       // install_trampoline (shared inline-detour toolkit)
#include "hook/patch.h"        // patch_bytes_guarded (S7 browser-row format string)
#include "hook/watcall.h"      // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::call_watcall1;
using mh::hook::entry_claim;
using mh::hook::install_jmp;
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

// ---- mp:U50 -- the lobby slot rows, owned by the DLL --------------------------------------------
//
// Retail's llm_lobby_build_slot_widgets (0x004bf99b EN) builds FOUR widgets per slot row (state,
// name, race, color) into four static pools, and appends them to the lobby container's children
// array at 0x00653633 (12 static entries, then the slot pointers from 0x00653663, room for 35).
// That has no room for more columns, so this body REPLACES it (install_jmp: the original never
// runs) and builds SIX per row: state, name, TEAM, race, color, PING.
//
//   * state / name / race / color stay in retail's pools (widgets llm_ui_widget[8] stride 0x44 at
//     0x0065252b / 0x0065230b / 0x0065296b / 0x0065274b, spinners stride 0x14 at 0x00645113 (state),
//     0x00645253 (race), 0x006451b3 (color)): retail's callbacks and the per-frame refresh loop in
//     llm_lobby_host_net_dispatch index those pools by slot, and race_cycle_cb reads the race
//     spinner's .value by slot*0x14. The retail pointer array at 0x00653663 is STILL filled
//     {state,name,race,color} per slot, so anything that indexes it keeps working.
//   * TEAM and PING are DLL-owned statics (g_team_*, g_ping_w). TEAM is built HIDDEN (flag 0x80);
//     its spinner is mp:U51. PING is a plain label widget (llm_ui_widget_draw, no frame, no
//     action): lobby_ping.cpp writes its label.
//   * The container's children array is a DLL-owned static (g_children): the 12 static entries are
//     copied from 0x00653633, then 6 widgets per row in DRAW ORDER (state, name, team, race, color,
//     ping -- ping last in the row), then NULL; and the container's children pointer
//     (*(0x00653f3f)) is repointed at it. Every build rewrites the whole array from scratch, so a
//     rebuild (map change, lobby_refresh_for_map, a new lobby visit) can neither leak nor
//     double-append. Retail's llm_lobby_slot_widget_list_clear only NULLs the retail array's first
//     slot; it is always followed by a build in the same screen-open call, so it needs no hook.
//
// LAYOUT. x is an offset from the PREVIOUS widget's resolved X (flag 0x20000 = keep previous X/Y);
// the state widget (flags 0x42) restarts from the container origin. Screen px, 640x480:
//   state x=0x1f w=0x32 -> 44..96       (unchanged)
//   name  x=0x36 w=0x66 -> 98..200      (retail w=0xa0)
//   team  x=0x6a w=0x21 -> 204..237     (hidden until mp:U51)
//   race  x=0x24 w=0x3c -> 240..300     (retail: x=0xa4 from name)
//   color x=0x3f        -> 303 (flag sprite ~305)  (unchanged offset from race)
//   ping  x=0x0c        -> 315; text ends before the panel border at x=349
constexpr uintptr_t POOL_NAME_W     = 0x0065230bu;
constexpr uintptr_t POOL_STATE_W    = 0x0065252bu;
constexpr uintptr_t POOL_COLOR_W    = 0x0065274bu;
constexpr uintptr_t POOL_RACE_W     = 0x0065296bu;
constexpr uintptr_t POOL_STATE_SP   = 0x00645113u; // 4-option spinner (Player/Open/AI/Closed)
constexpr uintptr_t POOL_COLOR_SP   = 0x006451b3u;
constexpr uintptr_t POOL_RACE_SP    = 0x00645253u; // 2-option spinner (Human/Alien)
constexpr uintptr_t OPT_STATE_TBL   = 0x006450afu; // Ghidra: _G_LLM_LOBBY_SPIN_RACE_OPTIONS (names are swapped)
constexpr uintptr_t OPT_RACE_TBL    = 0x006450a3u; // Ghidra: _G_LLM_LOBBY_SPIN_OPEN_OPTIONS
constexpr uintptr_t ADDR_RETAIL_CH  = 0x00653633u; // retail children array (12 static entries first)
constexpr uintptr_t ADDR_RETAIL_PT  = 0x00653663u; // retail slot-pointer array {state,name,race,color}*
constexpr uintptr_t ADDR_PANEL_W    = 0x00650727u; // llm_ui_widget_00650727: the slot panel (height)
constexpr uintptr_t ADDR_FONT_H     = 0x0065426bu; // DAT_0065426b: row text height
constexpr int       STATIC_CHILDREN = 12;
constexpr int       ROW_WIDGETS     = 6;
constexpr int       MAX_ROWS        = 8; // retail pools hold 8

constexpr unsigned W_STRIDE = 0x44, SP_STRIDE = 0x14, SLOT_STRIDE_ = 0x39;
// llm_ui_widget offsets
constexpr unsigned WO_ACTION = 0x04, WO_FLAGS = 0x08, WO_DRAW = 0x0c, WO_X = 0x1c, WO_Y = 0x20,
                   WO_W = 0x24, WO_H = 0x28, WO_SPIN = 0x30, WO_LABEL = 0x38, WO_USER = 0x40;
// retail callbacks / draw functions
constexpr int32_t CB_KICK = 0x004be552, CB_STATE = 0x004bf65d, CB_RACE = 0x004bf1d9,
                  CB_COLOR = 0x004bf292, DRAW_PLAIN = 0x004c1100, DRAW_CONTENT = 0x004c1b15;

alignas(4) uint8_t g_team_w[MAX_ROWS][W_STRIDE];
alignas(4) uint8_t g_team_sp[MAX_ROWS][SP_STRIDE];
alignas(4) uint8_t g_ping_w[MAX_ROWS][W_STRIDE];
constexpr int EXTRA_CHILDREN = 1; // mp:U51: the MODE selector, right after the statics
uintptr_t     g_children[STATIC_CHILDREN + EXTRA_CHILDREN + MAX_ROWS * ROW_WIDGETS + 1];
int           g_rows_built = 0;

inline int32_t &wf(uintptr_t w, unsigned off) {
    return *(int32_t *)(w + off);
}

// ---- mp:U51 -- TEAM spinner per row + MODE selector --------------------------------------------
//
// TEAM = llm_lobby_player_slot +0x0c (0 = "-", 1..4 = T1..T4). It rides the 0x38-byte client push
// (0x0c) and the host snapshot (0x09) with the rest of the slot, and build_players_step memcpys it to
// Players[].desc+7 (no reader). MODE = host slot 0 +0x07 low byte (0 = FFA, 1 = Team; the slot's `f5`,
// which nothing reads), so it rides the same snapshot to every client.
//
// The spinner draws through llm_ui_widget_draw_content: spinner +0 is a table of cells, cell[value]
// points at a {wchar_t *text} -- g_team_tbl / g_mode_tbl below. The click action is a DLL callback that
// follows the colour spinner's pattern (retail 0x004bf292): the HOST writes the slot and raises dirty
// bit 8 so the dispatch rebroadcasts the snapshot; a CLIENT (own row only) raises bit 8, writes the
// next value tentatively, pushes (0x0c), and restores -- the authoritative value returns in the
// snapshot, whose handler clears the bit it finds echoed in slot[local].reserved_0x0.
constexpr uintptr_t ADDR_DIRTY      = 0x006444c6u; // _G_LLM_LOBBY_DIRTY_FLAG (1 byte)
constexpr uintptr_t ADDR_PENDING_W  = 0x00654299u; // _G_LLM_UI_MENU_PENDING_WIDGET (llm_ui_widget *)
constexpr uintptr_t ADDR_PUSH_LOCAL = 0x004bf15cu; // llm_lobby_push_local_slot_state (__watcall, void)
constexpr unsigned  SLOT_TEAM_OFF = 0x0c, SLOT_MODE_OFF = 0x07, SLOT_STATUS_OFF_ = 0x0b;
constexpr int       TEAM_OPTIONS = 5;
constexpr uint8_t   DIRTY_TEAM   = 8;

alignas(4) uint8_t g_mode_w[W_STRIDE];
alignas(4) uint8_t g_mode_sp[SP_STRIDE];
const wchar_t  *g_team_txt[TEAM_OPTIONS];
const wchar_t  *g_mode_txt[2];
const wchar_t **g_team_tbl_cells[TEAM_OPTIONS]; // cell[v] -> &g_team_txt[v]
const wchar_t **g_mode_tbl_cells[2];
void          **g_team_tbl = (void **)g_team_tbl_cells;

void init_team_tables() {
    using mh::ui::Str;
    const Str ids[TEAM_OPTIONS] = {Str::LOBBY_TEAM_NONE, Str::LOBBY_TEAM_1, Str::LOBBY_TEAM_2,
                                   Str::LOBBY_TEAM_3, Str::LOBBY_TEAM_4};
    for (int i = 0; i < TEAM_OPTIONS; ++i) {
        g_team_txt[i]       = mh::ui::tr(ids[i]);
        g_team_tbl_cells[i] = &g_team_txt[i];
    }
    g_mode_txt[0]       = mh::ui::tr(Str::LOBBY_MODE_FFA);
    g_mode_txt[1]       = mh::ui::tr(Str::LOBBY_MODE_TEAM);
    g_mode_tbl_cells[0] = &g_mode_txt[0];
    g_mode_tbl_cells[1] = &g_mode_txt[1];
}

inline uint8_t &dirty_flag() {
    return *(uint8_t *)ADDR_DIRTY;
}
inline uint8_t *slot_at(int i) {
    return (uint8_t *)(mh::addr::_G_LLM_LOBBY_SLOTS + (uintptr_t)i * SLOT_STRIDE_);
}

void team_cb_impl() {
    const uintptr_t w = *(const uintptr_t *)ADDR_PENDING_W;
    if (!w) return;
    const uint32_t row = (uint32_t)wf(w, WO_USER);
    if (row >= (uint32_t)MAX_ROWS) return;
    uint8_t *slot = slot_at((int)row);
    if (*(const int *)mh::addr::_G_LLM_NET_IS_HOST) {
        slot[SLOT_TEAM_OFF] = (uint8_t)((slot[SLOT_TEAM_OFF] + 1) % TEAM_OPTIONS);
        dirty_flag() |= DIRTY_TEAM;
        return;
    }
    if ((int)row != *(const int *)mh::addr::_G_LLM_LOBBY_LOCAL_SLOT_INDEX) return; // client: own row only
    if (dirty_flag() & DIRTY_TEAM) return;                                         // one request in flight
    dirty_flag() |= DIRTY_TEAM;
    const uint8_t old   = slot[SLOT_TEAM_OFF];
    slot[SLOT_TEAM_OFF] = (uint8_t)((old + 1) % TEAM_OPTIONS);
    ((void (*)())ADDR_PUSH_LOCAL)();
    slot[SLOT_TEAM_OFF] = old; // the authoritative value comes back in the host's snapshot
}

void mode_cb_impl() {
    if (!*(const int *)mh::addr::_G_LLM_NET_IS_HOST) return; // host-only
    uint8_t *slot0       = slot_at(0);
    slot0[SLOT_MODE_OFF] = (slot0[SLOT_MODE_OFF] != 0) ? 0 : 1;
    dirty_flag() |= DIRTY_TEAM;
}

// Watcom __watcall callbacks, no args, returning 1 in EAX: preserve everything around the cdecl body.
// clang-format off
__declspec(naked) int team_cb_detour() {
    __asm {
        pushad
        pushfd
        cld
        call team_cb_impl
        popfd
        popad
        mov  eax, 1
        ret
    }
}
__declspec(naked) int mode_cb_detour() {
    __asm {
        pushad
        pushfd
        cld
        call mode_cb_impl
        popfd
        popad
        mov  eax, 1
        ret
    }
}
// clang-format on

// MODE selector geometry: a clone of the lobby's 12th static child (0x0065054b, a hidden+disabled
// right-panel widget centred under Cancel at ~(501,347)), so it inherits the right panel's own
// anchoring; only the hidden/disabled bits differ (lobby_team_tick sets disabled on a client).
void init_mode_widget() {
    const int       font_h = *(const int *)ADDR_FONT_H;
    const uintptr_t tmpl   = *(const uintptr_t *)(ADDR_RETAIL_CH + (uintptr_t)(STATIC_CHILDREN - 1) * 4);
    memset(g_mode_w, 0, W_STRIDE);
    memset(g_mode_sp, 0, SP_STRIDE);
    const uintptr_t w  = (uintptr_t)g_mode_w;
    const uintptr_t sp = (uintptr_t)g_mode_sp;
    wf(w, WO_X)        = wf(tmpl, WO_X);
    wf(w, WO_Y)        = wf(tmpl, WO_Y);
    wf(w, WO_W)        = wf(tmpl, WO_W);
    wf(w, WO_H)        = font_h;
    wf(w, WO_FLAGS)    = (wf(tmpl, WO_FLAGS) & ~0xc0) | 0x2; // 0x2 = takes input (Start/Cancel carry it)
    wf(w, WO_ACTION)   = (int32_t)(uintptr_t)mode_cb_detour;
    wf(w, WO_DRAW)     = DRAW_CONTENT;
    wf(w, WO_SPIN)     = (int32_t)sp;
    wf(sp, 0)          = (int32_t)(uintptr_t)g_mode_tbl_cells;
    wf(sp, 4)          = 2;
    wf(sp, 8)          = 0;
    wf(sp, 0xc)        = 0;
}

void build_slot_rows_impl() {
    constexpr uintptr_t A_BUILT = mh::addr::_G_LLM_LOBBY_WIDGETS_BUILT;
    constexpr uintptr_t A_SLOTS = mh::addr::_G_LLM_LOBBY_SLOTS;
    uint32_t            n       = (uint32_t)*(const int *)mh::addr::current_map_player_count;
    if (n > (uint32_t)MAX_ROWS) n = MAX_ROWS;
    const bool rebuild = *(const uint8_t *)A_BUILT != 0;
    if (!rebuild) {
        const int local_id = *(const int *)mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
        for (uint32_t i = 0; i < n; ++i) {
            if (*(const int *)(A_SLOTS + i * SLOT_STRIDE_ + 1) == local_id) {
                *(int *)mh::addr::_G_LLM_LOBBY_LOCAL_SLOT_INDEX = (int)i;
                break;
            }
        }
    } else {
        *(int *)mh::addr::_G_LLM_LOBBY_LOCAL_SLOT_INDEX = 0;
    }

    // Children: the 12 statics, then the rows (below), then NULL.
    int nc = 0;
    for (; nc < STATIC_CHILDREN; ++nc)
        g_children[nc] = *(const uintptr_t *)(ADDR_RETAIL_CH + (uintptr_t)nc * 4);
    g_children[nc++]     = (uintptr_t)g_mode_w;
    int32_t *retail_ptrs = (int32_t *)ADDR_RETAIL_PT;
    init_mode_widget();
    init_team_tables();

    const int font_h = *(const int *)ADDR_FONT_H;
    int       y      = 0x36;
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t slot = A_SLOTS + i * SLOT_STRIDE_;
        const uintptr_t ws   = POOL_STATE_W + i * W_STRIDE;
        const uintptr_t wn   = POOL_NAME_W + i * W_STRIDE;
        const uintptr_t wr   = POOL_RACE_W + i * W_STRIDE;
        const uintptr_t wc   = POOL_COLOR_W + i * W_STRIDE;
        const uintptr_t wt   = (uintptr_t)g_team_w[i];
        const uintptr_t wp   = (uintptr_t)g_ping_w[i];
        const uintptr_t sps  = POOL_STATE_SP + i * SP_STRIDE;
        const uintptr_t spr  = POOL_RACE_SP + i * SP_STRIDE;
        const uintptr_t spc  = POOL_COLOR_SP + i * SP_STRIDE;
        const uintptr_t spt  = (uintptr_t)g_team_sp[i];
        if (rebuild) {
            ((uint8_t *)slot)[5]   = 1;
            ((uint8_t *)slot)[0xb] = 0;
            ((uint8_t *)slot)[6]   = (uint8_t)i;
        }
        *(uint8_t *)slot = 0;

        // ---- retail widgets: the same field writes the retail body made, new x / width ----------
        wf(ws, WO_X) = 0x1f;
        wf(ws, WO_W) = 0x32;
        wf(wn, WO_X) = 0x36;
        wf(wn, WO_W) = 0x66;
        wf(wr, WO_X) = 0x24;
        wf(wr, WO_W) = 0x3c;
        wf(wc, WO_X) = 0x3f;
        wf(wc, WO_W) = 0x12;
        wf(ws, WO_Y) = y;
        wf(wn, WO_Y) = 0;
        wf(wr, WO_Y) = 0;
        wf(wc, WO_Y) = 0;
        y += font_h + 3;
        wf(ws, WO_H)      = font_h;
        wf(wn, WO_H)      = font_h;
        wf(wr, WO_H)      = font_h;
        wf(wc, WO_H)      = font_h;
        wf(ws, WO_FLAGS)  = 0x42;
        wf(wn, WO_FLAGS)  = 0x30002;
        wf(wr, WO_FLAGS)  = 0x200c2;
        wf(wc, WO_FLAGS)  = 0x200c0;
        wf(wn, WO_LABEL)  = 0;
        wf(wc, WO_LABEL)  = 0;
        wf(ws, WO_ACTION) = CB_STATE;
        wf(wn, WO_ACTION) = CB_KICK;
        wf(wr, WO_ACTION) = CB_RACE;
        wf(wc, WO_ACTION) = CB_COLOR;
        wf(wn, WO_DRAW)   = DRAW_PLAIN;
        wf(wc, WO_DRAW)   = DRAW_CONTENT;
        wf(wr, WO_DRAW)   = DRAW_CONTENT;
        wf(ws, WO_DRAW)   = DRAW_CONTENT;
        wf(wn, WO_SPIN)   = 0;
        wf(ws, WO_SPIN)   = (int32_t)sps;
        wf(wr, WO_SPIN)   = (int32_t)spr;
        wf(wc, WO_SPIN)   = (int32_t)spc;
        wf(wn, WO_USER)   = (int32_t)i;
        wf(wr, WO_USER)   = (int32_t)i;
        wf(ws, WO_USER)   = (int32_t)i;

        // ---- TEAM (mp:U51): a 5-option spinner (-, T1..T4) over slot byte +0x0c. Built hidden +
        // disabled; lobby_team_tick() sets visibility/enable and the value every lobby frame.
        memset((void *)wt, 0, W_STRIDE);
        memset((void *)spt, 0, SP_STRIDE);
        wf(wt, WO_X)      = 0x6a;
        wf(wt, WO_W)      = 0x21;
        wf(wt, WO_H)      = font_h;
        wf(wt, WO_FLAGS)  = 0x200c2; // keep-prev + hidden (0x80) + disabled (0x40)
        wf(wt, WO_ACTION) = (int32_t)(uintptr_t)team_cb_detour;
        wf(wt, WO_DRAW)   = DRAW_CONTENT;
        wf(wt, WO_SPIN)   = (int32_t)spt;
        wf(wt, WO_USER)   = (int32_t)i;
        wf(spt, 0)        = (int32_t)(uintptr_t)g_team_tbl;
        wf(spt, 4)        = TEAM_OPTIONS;
        wf(spt, 8)        = 0;
        wf(spt, 0xc)      = 0;

        // ---- PING: a plain label (llm_ui_widget_draw), no frame, no action. lobby_ping.cpp writes
        // +0x38. 0x20000 chains from the color widget; 0x40 keeps it out of hit-testing.
        memset((void *)wp, 0, W_STRIDE);
        wf(wp, WO_X)     = 0x0c;
        wf(wp, WO_W)     = 0x20;
        wf(wp, WO_H)     = font_h;
        wf(wp, WO_FLAGS) = 0x20042;
        wf(wp, WO_DRAW)  = DRAW_PLAIN;
        wf(wp, WO_USER)  = (int32_t)i;

        // ---- spinners (the contents retail wrote) -----------------------------------------------
        wf(sps, 0)    = (int32_t)OPT_STATE_TBL;
        wf(sps, 4)    = 4;
        wf(sps, 8)    = 0;
        wf(sps, 0xc)  = 0;
        wf(spr, 0)    = (int32_t)OPT_RACE_TBL;
        wf(spr, 4)    = 2;
        wf(spr, 8)    = 0;
        wf(spr, 0xc)  = 0;
        wf(spc, 0)    = 0;
        wf(spc, 4)    = 8;
        wf(spc, 8)    = 0;
        wf(spc, 0xc)  = (int32_t)(i & 7);
        wf(spc, 0x10) = 0x14;

        // ---- publish: retail's array (4/slot, kept for anything that indexes it) + ours (6/slot) -
        retail_ptrs[i * 4 + 0] = (int32_t)ws;
        retail_ptrs[i * 4 + 1] = (int32_t)wn;
        retail_ptrs[i * 4 + 2] = (int32_t)wr;
        retail_ptrs[i * 4 + 3] = (int32_t)wc;
        g_children[nc++]       = ws;
        g_children[nc++]       = wn;
        g_children[nc++]       = wt;
        g_children[nc++]       = wr;
        g_children[nc++]       = wc;
        g_children[nc++]       = wp; // LAST in the row
    }
    retail_ptrs[n * 4]                          = 0;
    g_children[nc]                              = 0;
    wf(ADDR_PANEL_W, WO_H)                      = (font_h + 3) * 8;
    *(uintptr_t *)mh::addr::lobby_widget_origin = (uintptr_t)g_children; // container +0 = children
    g_rows_built                                = (int)n;
}

// Watcom __watcall, no args, void: the caller may keep values in any register across the call, so
// save them all around the cdecl body.
// clang-format off
__declspec(naked) void slot_rows_detour() {
    __asm {
        pushad
        pushfd
        cld
        call build_slot_rows_impl
        popfd
        popad
        ret
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

bool install_slot_rows() {
    return install_jmp(mh::addr::llm_lobby_build_slot_widgets, (void *)slot_rows_detour,
                       entry_claim::exclusive, "the mp:U50 lobby slot rows");
}

void *lobby_slot_ping_widget(int slot) {
    if (slot < 0 || slot >= g_rows_built) return nullptr;
    return g_ping_w[slot];
}

// mp:U51: refresh the TEAM spinners + the MODE selector from the slot records (see the block above).
void lobby_team_tick() {
    if (*(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != (void *)mh::addr::lobby_widget_origin) return;
    if (g_rows_built <= 0) return;
    const bool host      = *(const int *)mh::addr::_G_LLM_NET_IS_HOST != 0;
    const int  local     = *(const int *)mh::addr::_G_LLM_LOBBY_LOCAL_SLOT_INDEX;
    uint8_t   *slot0     = slot_at(0);
    const bool team_mode = slot0[SLOT_MODE_OFF] != 0;

    for (int i = 0; i < g_rows_built; ++i) {
        uint8_t      *slot   = slot_at(i);
        const uint8_t status = slot[SLOT_STATUS_OFF_];
        const bool    seated = (status == 1 || status == 2); // HUMAN | AI
        if (host && (!seated || slot[SLOT_TEAM_OFF] >= TEAM_OPTIONS) && slot[SLOT_TEAM_OFF] != 0) {
            slot[SLOT_TEAM_OFF] = 0; // a vacated / closed slot forgets its team; the snapshot carries it
            dirty_flag() |= DIRTY_TEAM;
        }
        const uint8_t   team             = (seated && slot[SLOT_TEAM_OFF] < TEAM_OPTIONS) ? slot[SLOT_TEAM_OFF] : 0;
        const uintptr_t wt               = (uintptr_t)g_team_w[i];
        wf((uintptr_t)g_team_sp[i], 0xc) = team;
        int32_t f                        = wf(wt, WO_FLAGS);
        f                                = seated ? (f & ~0x80) : (f | 0x80); // visible in FFA and Team alike (mp:U51)
        f                                = (host || (i == local && status == 1)) ? (f & ~0x40) : (f | 0x40);
        wf(wt, WO_FLAGS)                 = f;
    }
    const uintptr_t wm            = (uintptr_t)g_mode_w;
    wf((uintptr_t)g_mode_sp, 0xc) = team_mode ? 1 : 0;
    wf(wm, WO_FLAGS)              = host ? (wf(wm, WO_FLAGS) & ~0x40) : (wf(wm, WO_FLAGS) | 0x40);
}

// ---- mp:U52: a NEW lobby starts with no teams and FFA mode ---------------------------------------------
//
// The MODE byte (host slot 0 +0x07) and every TEAM byte (slot +0x0c) have no retail writer, so nothing ever
// clears them: a host who finished a Team match and created another game would open it in Team mode with
// the old teams. Run-before llm_lobby_screen_open (shared by the host's new-game start and a client's join):
// the HOST zeroes them before the lobby's first dispatch. A client's bytes arrive in the host's snapshot.
// MEASURED (mp_host_u52_relobby.txt): the host Cancel -> Create path ALREADY comes back FFA / "-" with this detour
// disabled -- retail clears the slot array when the lobby is rebuilt -- so this is insurance for the paths that script
// does not walk (e.g. back from a finished match), not the fix of an observed bug. A host whose lobby opens with the
// net-host flag still clear is not covered.
void *g_so_tramp = nullptr;
void  on_lobby_open() {
    if (*(const int *)mh::addr::_G_LLM_NET_IS_HOST == 0) return;
    slot_at(0)[SLOT_MODE_OFF] = 0;
    for (int i = 0; i < 8; ++i) slot_at(i)[SLOT_TEAM_OFF] = 0;
}
// clang-format off
__declspec(naked) void lobby_open_detour() {
    __asm {
        pushad
        pushfd
        call on_lobby_open
        popfd
        popad
        jmp  dword ptr [g_so_tramp] // stolen 8-byte prologue + jmp back
    }
}
// clang-format on

bool install_lobby_open_reset() {
    return install_trampoline(mh::addr::llm_lobby_screen_open, (void *)lobby_open_detour, &g_so_tramp, 8,
                              entry_claim::exclusive, "the mp:U52 new-lobby team/mode reset");
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
