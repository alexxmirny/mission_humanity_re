//
// D17 launch-to-state harness -- drive mh.exe directly into a chosen state from the command line,
// skipping the main menu. See include/mh_launch_export.h; D17.
//
// STEP 1 (mechanism): parse the exe command line; if a launch verb is present install a one-shot
// inline hook on llm_ui_menu_state_tick (0x004b774b, the per-frame menu/dialog state driver). The
// first frame the MAIN MENU is idle -- _G_LLM_GAME_MODE==3 (llm_ui_menu_frame) and
// _G_LLM_UI_MENU_STATE==3 (idle/poll) -- we latch and fire the parsed action. Firing at menu-idle
// guarantees every boot stage has run, so the load path's cfg/data tables are populated.
//
// STEP 2 (--load <name>): reuse the LIVE save-load callback. Point the selected-save global
// _G_LLM_MENU_SAVE_NAME_PTR (0x00654462, a char*) at our name buffer, clear dlg flag bit8 so the
// enter-game transition takes the fresh-enter branch (GAME_MODE=2, not the in-game-dialog resume
// GAME_MODE=3), then call llm_menu_loadgame_confirm (0x004bb22b) -- which runs
// llm_game_load(save\<name>.sav) and, on success, llm_menu_finish_enter_gameplay ->
// llm_menu_enter_gameplay. Same code the retail "Load Game -> pick save -> confirm" click drives.
//
// STEP 3 (--newgame [race]): start a fresh CAMPAIGN. Set _G_LLM_MENU_NEWGAME_RACE (0x00e642a4, 1=H /
// 2=A -- what the race-select screen would set) then call llm_menu_campaign_start_and_enter
// (0x004b7ffb). Its llm_strat_planet_session_begin(race, 0) does the ENTIRE new-game reset inside
// (llm_strat_session_state_reset(0): CurrentSystem=1, start G_PLANET_INDEX=System[1].planets[1], clear planet
// statuses, new_game_init) + planet load + 3 player profiles, then it sets GAME_MODE=2. Race-only: the
// campaign always starts on System[1]'s planet, there is no planet select. (--skirmish, the .MP-based
// path, is a separate deferred verb -- D17 step 3b.)
//
// The command line is safe to carry extra tokens: mh.exe's own parser (llm_game_parse_cmdline) only
// tokenizes into an argv array that nothing validates, so unknown "--load foo" args are ignored.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "include/mh_launch_export.h"
#include "include/mh_net_export.h"   // MH_Net_PeerCount / MH_Net_IsStarted (F-gate peer wait)
#include "include/mh_seam_export.h"  // MH_Seam_StartTransport (bring the transport up at the menu)
#include "include/mh_run_context.h"  // MH_RunDir (per-run log folder)
#include "include/lobby_session.h"   // llm_net_session_entry (retail lobby session-list record)
#include "addr/mh_addrs.gen.h"       // generated EN VAs (tools/gen_dll_addrs.py)
#include "config/ini_read.h"         // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "state/region_runtime.h"    // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                     // where it IS, not where the binary put it
#include "addr/mh_calls.gen.h"       // typed __watcall wrappers (map_SavePlanetToDisk, --tactical)
#include "en_guard.h"                // EN-only build gate
#include "hook/detour.h"             // install_trampoline (shared inline-detour toolkit)
#include "hook/watcall.h"            // call_watcall1 (Watcom __watcall(EAX) bridge)
#include "tact/tact_mission_start.h" // mh::tact::mission_start -- the --tactical verb enters OURS
#include "seams/map_transfer.h"      // mp:X2/X2b -- maps::host_start_blocked, the Start refusal

using mh::hook::call_watcall1;
using mh::hook::entry_claim; // U30: every install below names its claim and itself
using mh::hook::install_trampoline;

#pragma comment(lib, "user32.lib") // wsprintfA

#if defined(_M_IX86) // x86-only: naked trampoline + absolute exe VAs (mh.exe is 32-bit)

namespace {

// ---- mh.exe fixed VAs (generated EN header; image base 0x00400000, no ASLR) ---------------------
constexpr uintptr_t ADDR_MENU_STATE_TICK  = mh::addr::llm_ui_menu_state_tick;            // per-frame driver
constexpr uintptr_t ADDR_LOADGAME_CONFIRM = mh::addr::llm_menu_loadgame_confirm;         // load + enter
constexpr uintptr_t ADDR_CAMPAIGN_START   = mh::addr::llm_menu_campaign_start_and_enter; // new game + enter
constexpr uintptr_t ADDR_INTRO_FRAME      = mh::addr::llm_intro_frame;                   // mode-7 frame; plays LOGO.AVI
constexpr uintptr_t ADDR_MOVIE_TEARDOWN   = mh::addr::movie_teardown_fn;                 // movie teardown + continuation
constexpr uintptr_t ADDR_INTRO_GATE       = mh::addr::intro_gate_byte;                   // byte; 0=first intro frame, >0=playing
constexpr uintptr_t ADDR_ASYNC_CB         = mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK;     // movie pump ptr
constexpr uintptr_t ADDR_GAME_MODE        = mh::addr::_G_LLM_GAME_MODE;                  // BYTE; 3=menu, 2=strat
constexpr uintptr_t ADDR_MENU_STATE       = mh::addr::_G_LLM_UI_MENU_STATE;              // BYTE; 3=idle
constexpr uintptr_t ADDR_SAVE_NAME_PTR    = mh::addr::_G_LLM_MENU_SAVE_NAME_PTR;         // char*
constexpr uintptr_t ADDR_NEWGAME_RACE     = mh::addr::menu_newgame_race;                 // int; 1=H, 2=A
constexpr uintptr_t ADDR_DLG_FLAGS0       = mh::addr::_G_LLM_DLG_STATE_FLAGS;            // .flags0 (byte, +0)

// ---- TACT-PREP: the --tactical verb -----------------------------------------------------------
// The tactical entry gate (llm_strat_try_enter_tactical_mission) is NOT what we call. Its own body
// has no guards; the gate is llm_strat_bldg_gather_nearby_squad_status, whose predicates are
// authored gameplay state -- control group 0 stocked with soldier-carrying units parked within 15
// tiles of an enemy MAIN BASE. What that function PRODUCES, though, is a plain .bss blackboard, so
// the harness writes the blackboard instead of earning it and then calls OUR mission_start
// (mh::tact::mission_start -- TACT1-P C4; it used to call the original at this address).
// (the tactical-probe work Sect. 1 has the measurement; that function is void(void) with exactly one
// incoming reference, so there is no marshalling to get wrong.)
// (the planet save goes through mh::call::map_SavePlanetToDisk, a generated typed wrapper -- it has
// a committed prototype, so there is no reason to hand-roll a __watcall bridge for it here)
constexpr uintptr_t ADDR_PLANET_INDEX = mh::addr::G_PLANET_INDEX;
constexpr uintptr_t ADDR_PLAYER_SIDE  = mh::addr::PlayerSide; // ushort, the LOCAL player
// WHICH MISSION A RUN PLAYED IS NOT G_PLANET_INDEX, and the log said `planet=` for a month as if it
// were (TACT-REC, 2026-08-24). llm_tact_mission_start builds the name from `CurrentSystem`
// (0x00e58781) -- a DIFFERENT global from G_PLANET_INDEX (0x00e58366) -- and picks the O/L variant
// from the TARGET owner's race. So a per-mission measurement could not be built from the old line at
// all: two runs on different missions logged the same `planet=1`.
constexpr uintptr_t ADDR_CURRENT_SYSTEM  = mh::addr::CurrentSystem;
constexpr uintptr_t ADDR_PLAYERS_PROF    = mh::addr::_G_LLM_STRAT_PLAYERS; // llm_strat_player_profile[8]
constexpr int       PLAYER_PROF_STRIDE   = 0x740;
constexpr int       PLAYER_PROF_OFF_RACE = 0x4;
// RACE == 1 IS HUMAN, read off the instruction and not off the decompiler. The decompile renders the
// test as `... .race == HUMAN`, but `HUMAN` is the STRING label "H" at 0x0050103f; the actual
// instruction is `CMP dword ptr [EAX + 0xcff064],0x1` @0x0042913e -> POZ<n>O.DAT on equal,
// POZ<n>L.DAT otherwise. Taking the symbol at face value would have compared a race id to a pointer.
constexpr int RACE_HUMAN = 1;
// The blackboard: a 64-slot stride-0x10 record array plus five scalars.
// SB-HOSTFREE: THE SAME 1024 BYTES AS `_G_LLM_SQUAD_STATUS`, under a second name. The address
// table calls them `squad_blackboard`, the state registry calls them SQUAD_STATUS, and both mean
// 0x00e15e60 -- so a relocating host MOVES this array while a `mh::addr::` constant keeps writing
// the abandoned copy. Measured: the tactical mission then started with 16 units instead of 24 and
// ended at frame 1 with an empty squad, and it did so with poison OFF too, because the split is a
// write here against a read there rather than a stale read.
inline uintptr_t ADDR_SQUAD_BB() {
    return mh::state::live_base(mh::state::RID_SQUAD_STATUS);
}
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t ADDR_SQUAD_BB_COUNT() {
    return mh::state::live_base(mh::state::RID_SQUAD_STATUS_COUNT);
}
inline uintptr_t ADDR_BB_SCAN_PLAYER() {
    return mh::state::live_base(mh::state::RID_SQUAD_BB_SCAN_PLAYER);
}
inline uintptr_t ADDR_BB_TARGET_OWNER() {
    return mh::state::live_base(mh::state::RID_SQUAD_BB_TARGET_OWNER);
}
inline uintptr_t ADDR_BB_TARGET_ID() {
    return mh::state::live_base(mh::state::RID_SQUAD_BB_TARGET_BUILDING_ID);
}
inline uintptr_t ADDR_BB_TARGET_PCT() {
    return mh::state::live_base(mh::state::RID_SQUAD_BB_TARGET_ENERGY_PCT);
}
inline uintptr_t ADDR_BB_TARGET_IDX() {
    return mh::state::live_base(mh::state::RID_SQUAD_BB_TARGET_BUILDING_IDX);
}
constexpr int SQUAD_BB_STRIDE     = 0x10;
constexpr int SQUAD_BB_SLOTS      = 0x40; // gather() clears and fills at most 64
constexpr int SQUAD_BB_OFF_PROTO  = 0x0;
constexpr int SQUAD_BB_OFF_HP_PCT = 0x4; // == the dumped symbol _G_LLM_SQUAD_STATUS
constexpr int SQUAD_BB_OFF_UNIT   = 0x8;
constexpr int SQUAD_BB_OFF_CMDO   = 0xc;

constexpr int GAME_MODE_MENU  = 3;
constexpr int GAME_MODE_STRAT = 2;
constexpr int GAME_MODE_TACT  = 6; // llm_frame_dispatch case 6 -> llm_tact_frame
constexpr int MENU_STATE_IDLE = 3;
constexpr int RACE_H          = 1; // "H" faction (NewGameH.avi)
constexpr int RACE_A          = 2; // "A" faction (NewGameA.avi)

// APPEND ONLY. These ordinals are the return value of MH_Launch_ParseCmdline, and
// mh_nettest's `launchtest` asserts them as LITERALS -- so inserting a verb mid-list silently
// renumbers every verb after it, and `launchtest` is not in run_selftests' gate suite, so nothing
// would report it. VERB_TACTICAL is at the end for exactly that reason, not for taste.
enum Verb { VERB_NONE,
            VERB_LOAD,
            VERB_NEWGAME,
            VERB_MP_HOST,
            VERB_MP_JOIN,
            VERB_MP_BROWSER,
            VERB_MP_HOST_LOBBY,
            VERB_MP_JOIN_LOBBY,
            VERB_TACTICAL };

Verb g_verb = VERB_NONE;
char g_arg[128];           // verb argument (e.g. the save name, or host ip)
bool g_skip_intro = false; // --skip-intro flag (skip the LOGO.AVI startup movie); combinable with any verb
bool g_intro_done = false; // one-shot latch for the intro-skip teardown
bool g_fired      = false; // one-shot latch

// F-gate (MP): hold at the menu until the peer(s) connect, so all peers enter mode-3 together. N1: the
// host waits for N-1 clients (MH_Net_PeerCount = #clients); each client waits for 1 (its single host
// connection). Computed per-role in on_menu_tick from mp_players(); no longer a fixed constant.
bool            g_mp_armed      = false;          // transport started + role globals set (once, at menu-idle)
int             g_mp_wait_ticks = 0;              // menu-idle frames spent waiting for the peer (for a heartbeat log)
bool            g_manual_mp     = false;          // manual-menu MP active (button->browser / host create): run the lobby driver
extern "C" int  MH_MP_MapReceived(void);          // net_seams -- client: host's selected map has arrived (entry gate)
extern "C" void MH_MP_ResetMapReceived(void);     // net_seams -- U29: forget the dead lobby's map on exit
extern "C" void MH_Seam_ClearStartReceived(void); // net_discovery -- U29: disarm a stale FLAG_START on exit
extern "C" void MH_MP_ClientPollMap(void);        // net_seams -- client: drain the transport for the host's map datagram
extern "C" void MH_Seam_SaveHostSlots(void);      // net_seams -- snapshot lobby slots at entry (transition clears them)
extern "C" int  MH_Net_LocalPlayerId(void);       // net_transport -- this peer's own/assigned id (host-assign or declared)
extern "C" int  MH_Net_IdAssigned(void);          // net_transport -- 1 once this client's id is settled (WELCOME arrived)

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0; // SES1: per-SESSION -- U14 peer-table deltas are match telemetry

void lg(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_launch.log", &g_log_gen);
    char    line[320];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(h, line, lstrlenA(line), &w, nullptr);
    CloseHandle(h);
}

// ---- command-line tokenizer (space-delimited, honours "double quotes") ---------------------------
// Copies the next token starting at *pp into out[cap]; advances *pp past it. Returns false at end.
bool next_token(const char **pp, char *out, int cap) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') {
        *pp = p;
        return false;
    }
    int n = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') {
            if (n < cap - 1) out[n++] = *p;
            ++p;
        }
        if (*p == '"') ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n < cap - 1) out[n++] = *p;
            ++p;
        }
    }
    out[n] = '\0';
    *pp    = p;
    return true;
}

int ieq(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

// Pure parse of an explicit command line into (verb, arg_out) + the --skip-intro flag (*skip_out). No
// game state touched, so this is the unit-testable core (mh_nettest launchtest). Skips argv[0]; the
// first recognized verb wins; --skip-intro may appear anywhere (scanned across all tokens). Returns
// VERB_NONE if no verb is present or --load's required argument is missing.
Verb parse_into(const char *cl, char *arg_out, int arg_cap, bool *skip_out) {
    arg_out[0] = '\0';
    Verb verb  = VERB_NONE;
    if (skip_out) *skip_out = false;
    char tok[256];
    next_token(&cl, tok, sizeof(tok)); // skip argv[0] (the exe path)
    while (next_token(&cl, tok, sizeof(tok))) {
        if (ieq(tok, "--skip-intro")) {
            if (skip_out) *skip_out = true;
            continue;
        }
        if (verb != VERB_NONE) continue; // verb already found; keep scanning for --skip-intro
        if (ieq(tok, "--tactical")) {
            // Same required argument as --load, and for the same reason: the save to enter FROM. A
            // tactical mission needs a live strategic session behind it (the planet, Players[],
            // units[]/buildings[], CurrentSystem and the .rsr/cfg tables), and --load is the
            // shortest way to have one. Everything ELSE the entry needs is synthesised (fire_action).
            if (!next_token(&cl, arg_out, arg_cap) || arg_out[0] == '\0') {
                arg_out[0] = '\0';
                continue;
            }
            verb = VERB_TACTICAL;
        } else if (ieq(tok, "--load")) {
            if (!next_token(&cl, arg_out, arg_cap) || arg_out[0] == '\0') {
                arg_out[0] = '\0';
                continue;
            }
            verb = VERB_LOAD;
        } else if (ieq(tok, "--newgame")) {
            next_token(&cl, arg_out, arg_cap);
            verb = VERB_NEWGAME;
        } else if (ieq(tok, "--mp-host")) {
            arg_out[0] = '\0';
            verb       = VERB_MP_HOST;
        } else if (ieq(tok, "--mp-join")) {
            next_token(&cl, arg_out, arg_cap);
            verb = VERB_MP_JOIN;
        } else if (ieq(tok, "--mp-browser")) {
            arg_out[0] = '\0';
            verb       = VERB_MP_BROWSER;
        } else if (ieq(tok, "--mp-host-lobby")) {
            arg_out[0] = '\0';
            verb       = VERB_MP_HOST_LOBBY;
        } else if (ieq(tok, "--mp-join-lobby")) {
            arg_out[0] = '\0';
            verb       = VERB_MP_JOIN_LOBBY;
        } // host from mh_net.ini
    }
    return verb;
}

// Parse the live command line into (g_verb, g_arg, g_skip_intro).
Verb parse_cmdline() {
    return parse_into(GetCommandLineA(), g_arg, sizeof(g_arg), &g_skip_intro);
}

// Map a --newgame race argument to the game's race id (1=H / 2=A). Accepts a/A/2 -> A; everything
// else (h/H/1/human, or empty) -> H (the default faction). Only the first char is inspected.
int parse_race(const char *a) {
    if (a && (a[0] == 'a' || a[0] == 'A' || a[0] == '2')) return RACE_A;
    return RACE_H;
}

// ---- MP force-entry (D17 step 4) ----------------------------------------------------------------
// Bypass discovery / lobby slots / peer->peer map streaming: BOTH peers load the SAME local .MP via
// the skirmish template (llm_game_start_tutorial minus its tutorial tail), force mode-3 lockstep, and
// let the A1 transport seams (net_seams.cpp) carry the order/horizon stream. The sim RNG (ch0) auto-
// seeds inside session_begin_multi from the map's own byte (current_map_data+0x14), so two peers
// reading identical .MP bytes seed identically => deterministic lockstep from step 0. Full derivation
// + open risks: the "MP force-entry recipe" (decompile-verified 2026-07-10).
constexpr uintptr_t ADDR_READMAPFILE   = mh::addr::cfg_ReadMapFile;                // map_header* in EAX -- header load
constexpr uintptr_t ADDR_SESSION_BEGIN = mh::addr::llm_strat_session_begin_multi;  // map_header* in EAX
constexpr uintptr_t ADDR_FINISH_ENTER  = mh::addr::llm_menu_finish_enter_gameplay; // -> strategic frame
constexpr uintptr_t ADDR_CUR_MAP       = mh::addr::current_map_data;               // cfg::struct::map_header
constexpr uintptr_t ADDR_PLAYERS       = mh::addr::Players;                        // game::g::Players[8] (stride 0x34)
constexpr uintptr_t ADDR_PLANETS       = mh::addr::Planets;                        // cfg::final::data::Planets[32] (stride 0x427)
constexpr uintptr_t ADDR_NET_PCOUNT    = mh::addr::mode3_trigger_player_count;     // >=2 => SESSION_MP_LOCKSTEP
constexpr uintptr_t ADDR_NET_LSCOUNT   = mh::addr::_G_LLM_NET_LOCKSTEP_PLAYER_COUNT;
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t ADDR_NET_PLAYERSIDE() { // the injected
    return mh::state::live_base(mh::state::RID_NET_LOCAL_PLAYER_SLOT);
}
// stack's local-slot int. NOTE (2026-07-27): this is NOT Ghidra's PlayerSide -- that is a different
// global (0x00e58354, ushort) which the SIM indexes players by. The manifest entry used to carry the
// wrong name; the address here is unchanged, only the symbol it resolves through.
constexpr uintptr_t ADDR_NET_LOCALIDX = mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
constexpr uintptr_t ADDR_NET_IS_HOST  = mh::addr::_G_LLM_NET_IS_HOST;
constexpr uintptr_t ADDR_SESSION_MODE = mh::addr::_G_LLM_GAME_SESSION_MODE; // BYTE; 3=lockstep
// mp:RM1 -- the two sim clocks (doubles, game-seconds) the rematch prep zeroes; see mp_zero_sim_clocks.
constexpr uintptr_t ADDR_GAME_CLOCK = mh::addr::_G_LLM_STRAT_GAME_CLOCK;
constexpr uintptr_t ADDR_TOTAL_TIME = mh::addr::TOTAL_GAME_TIME;

constexpr int PLAYER_STRIDE = 0x34;
constexpr int PLANET_STRIDE = 0x427;
// llm_strat_player_desc field offsets
constexpr int PL_RACE = 0x00, PL_COLOR = 0x01, PL_CTRL = 0x06, PL_RELATION = 0x08, PL_NAME = 0x10, PL_SIDE = 0x30;
// cfg::struct::map_header field offsets
constexpr int MD_PATH = 0x18, MD_MAPNAME = 0xfc, MD_TLONAME = 0x11c;
// cfg::final::struct::Planet field offsets
constexpr int PZ_PATH = 0x10, PZ_MAPNAME = 0x10f, PZ_TLO = 0x20e, PZ_SRCMUL = 0x3f1, PZ_SRCADD = 0x401;

// Default PoC map: present in every clean install; the tutorial proves it loads as a 1v1 skirmish. Both
// peers use it by construction (identical bytes => identical RNG seed). N1: overridable via [net] mp_map --
// point it at a real .mpm MP map (Maps\*.mpm, >=N start positions) for a >2-player game. A .mpm loads via
// the loose-file fallback under "Maps\"; a .MP (campaign) under "Dane\" from the rsr. Path auto-picked by ext.
const char    MP_MAP_PATH[] = "Dane\\";
const char    MP_MAP_NAME[] = "TUTORIAL.MP";
constexpr int MD_PCOUNT     = 0x08; // cfg::struct::map_header.player_count (overridden to N)

// Distinct player color per slot (was hand-set p0=3, p1=1). Determinism doesn't depend on the value --
// only that every peer computes the SAME array -- so any fixed distinct table works; this keeps 0,1 as before.
const unsigned char MP_PLAYER_COLOR[8] = {3, 1, 4, 2, 5, 6, 7, 0};

// ---- N-player config (N1): read [net] mp_players / player_id / mp_map from the ini next to the exe -------
// Same mh_net.ini the transport configures from, so [net] player_id here == the transport's g_my_id. Read
// live per call (the ini is tiny + OS-cached). The defaults preserve the original 2-player force-entry.
// Generalised from net_ini_int when TACT-PREP added a [tactical] section: same file, same lookup,
// the SECTION is now the caller's business. net_ini_int keeps its name and its [net] default, so no
// existing call site changes.
int ini_int(const char *section, const char *key, int def) {
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *slash = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    wsprintfA(ini, "%smh_net.ini", exe);
    return GetPrivateProfileIntA(section, key, def, ini);
}
int net_ini_int(const char *key, int def) { return ini_int("net", key, def); }

// U29 negative arm: net_seams owns the menu-slide take-over but has no ini reader of its own (it is
// configured through the g_a struct at init), so the one knob it needs is published from here.
// [net] u29_slide_dir=0 makes mh_intro_wait_take_over ignore the slide DIRECTION again -- the pre-fix
// behaviour that pinned the lobby on-screen during its own slide-OUT. Cached: the take-over runs in a
// render path and the ini is a file read.
extern "C" int MH_Cfg_U29SlideDir(void) {
    static int v = -1;
    if (v < 0) v = net_ini_int("u29_slide_dir", 1);
    return v;
}

// U3b's MISSING NEGATIVE CONTROL. [net] u3b_park=0 disables mh_intro_wait_take_over outright, so the
// retail blocking slide loop runs on every caller including the lobby dispatch. The fix shipped
// 2026-07-28 with no way to turn it off, which is why its stated cause ("on the client the game
// ms-clock does not advance inside that loop") went thirteen months without ever being tested: the
// fix works by removing a blocking call from the dispatch's critical path, and that works whether the
// loop hung, replayed, or merely took its 400 ms -- so success never discriminated between them. This
// knob is what makes the original symptom observable again. Same caching rationale as above.
extern "C" int MH_Cfg_U3bPark(void) {
    static int v = -1;
    if (v < 0) v = net_ini_int("u3b_park", 1);
    return v;
}

// [net] dedup_cancel_slide=0 restores the retail DOUBLE slide-in on Browser -> Internet server ->
// Cancel (user-reported 2026-08-30). Its own knob rather than riding u3b_park: that one gates the
// take-over, this suppresses one redundant retail call, and folding two fixes behind one switch is how
// you get a control that cannot isolate either.
extern "C" int MH_Cfg_DedupCancelSlide(void) {
    static int v = -1;
    if (v < 0) v = net_ini_int("dedup_cancel_slide", 1);
    return v;
}

// [net] slide_diag=1 logs EVERY menu-slide entry -- both loops, parked or not -- with the caller, the
// active list, the widget being animated and whether that widget is even a child of the active list.
// That last column is the point: a slide whose widget the current screen does not draw presents for
// 400 ms with nothing moving and no cursor (the loops bypass llm_ui_frame_tick, which is what draws
// it), which is what a "freeze" looks like from the outside. Off by default; diagnostic only.
extern "C" int MH_Cfg_SlideDiag(void) {
    static int v = -1;
    if (v < 0) v = net_ini_int("slide_diag", 0);
    return v;
}

// [net] dedup_dead_slide=0 restores the ~400 ms motionless, cursorless slide on the Create-game click
// (user-reported 2026-08-30 as a freeze). Separate knob again: this one suppresses a slide that draws
// NOTHING, which is a different claim from dedup_cancel_slide's "this screen is slid in twice", and
// each wants to be falsifiable on its own.
extern "C" int MH_Cfg_DedupDeadSlide(void) {
    static int v = -1;
    if (v < 0) v = net_ini_int("dedup_dead_slide", 1);
    return v;
}
void net_ini_str(const char *key, const char *def, char *out, int cap) {
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *slash = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    wsprintfA(ini, "%smh_net.ini", exe);
    mh::config::read_ini_string("net", key, def, out, cap, ini); // TL-HARN4
}
// N = total players (clamped 2..8, the lobby-slot cap).
int mp_players(void) {
    int n = net_ini_int("mp_players", 2);
    return n < 2 ? 2 : (n > 8 ? 8 : n);
}
// This peer's own player id / slot. From [net] player_id (matches the transport's g_my_id); default = the
// 2-player convention (host 0, client 1). For N>2, mp_run.py assigns a distinct player_id per peer.
int mp_local_slot(bool is_host) {
    int s = net_ini_int("player_id", is_host ? 0 : 1);
    return s < 0 ? 0 : (s > 7 ? 7 : s);
}
// True if the name looks like a loose .mpm MP map (loads from "Maps\"), vs a .MP campaign map ("Dane\").
bool map_is_mpm(const char *name) {
    int n = lstrlenA(name);
    return n >= 4 && lstrcmpiA(name + n - 4, ".mpm") == 0;
}

void str_copy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// Networking role/count/slot globals (DAT_005d54bc>=2 is the sole mode-3 trigger). Split out because
// the F-gate arms the transport (which reads IS_HOST/LOCAL_PLAYER_INDEX) at the MENU, before the full
// force-entry runs. Idempotent -- safe to call at arm time and again inside mp_force_entry.
void mp_set_net_role(bool is_host) {
    int n                         = mp_players();           // N1: N from [net] mp_players (default 2)
    int slot                      = mp_local_slot(is_host); // N1: own slot from [net] player_id (default host 0 / client 1)
    *(int *)ADDR_NET_PCOUNT       = n;
    *(int *)ADDR_NET_LSCOUNT      = n;
    *(int *)ADDR_NET_PLAYERSIDE() = slot;
    *(int *)ADDR_NET_LOCALIDX     = slot;
    *(int *)ADDR_NET_IS_HOST      = is_host ? 1 : 0;
}

// Force-enter a 2-player mode-3 lockstep game. is_host picks the role/slot triple (host=slot0, join=
// slot1); everything else is symmetric so both peers build byte-identical session state. Fired once at
// main-menu idle (same context llm_game_start_tutorial runs in), so every boot stage / cfg table is up.
void mp_force_entry(bool is_host) {
    char *md   = (char *)ADDR_CUR_MAP;
    int   n    = mp_players();           // N1: total players (2..8)
    int   slot = mp_local_slot(is_host); // N1: this peer's own slot

    // 1. map identity + header load (fills player-count +0x8, seed byte +0x14, tlo_name from the file). N1:
    //    the map name comes from [net] mp_map (default TUTORIAL.MP); a .mpm loads loose from "Maps\", a .MP
    //    from the rsr under "Dane\". After the header load, FORCE the map's player-count (+0x8) to N so
    //    session_begin seats exactly our N players regardless of the map's designed count -- all peers apply
    //    the identical override, so the seed/state stay byte-identical. Map must have >=N start positions.
    char mapname[64];
    net_ini_str("mp_map", MP_MAP_NAME, mapname, sizeof(mapname));
    str_copy(md + MD_PATH, map_is_mpm(mapname) ? "Maps\\" : MP_MAP_PATH, 128);
    str_copy(md + MD_MAPNAME, mapname, 32);
    md[MD_TLONAME] = 0;
    call_watcall1(ADDR_READMAPFILE, md);
    *(int *)(md + MD_PCOUNT) = n; // override designed count -> exactly N players

    // 2. N HUMAN players (zero the whole array first, as the real build_players_from_slots does). Each is a
    //    HUMAN (ctrl=7) enemy of every other player -- the full N×N symmetric relation matrix. Every peer
    //    builds the byte-identical Players[] (same n, same deterministic per-slot race/color), so the local
    //    slot only picks WHICH player this peer drives (PlayerSide), not the array contents.
    char *P = (char *)ADDR_PLAYERS;
    for (int i = 0; i < 0x1a0; ++i) P[i] = 0;
    for (int i = 0; i < n; ++i) {
        char *pi               = P + i * PLAYER_STRIDE;
        pi[PL_RACE]            = (unsigned char)((i & 1) ? 2 : 1); // alternate H/A (deterministic)
        pi[PL_COLOR]           = MP_PLAYER_COLOR[i];               // distinct per slot
        pi[PL_CTRL]            = 7;                                // HUMAN (not AI)
        *(int *)(pi + PL_SIDE) = i;
        for (int j = 0; j < n; ++j)
            if (j != i) pi[PL_RELATION + j] = 2; // enemy of every other player
        char nm[16];
        wsprintfA(nm, "Player%d", i + 1);
        str_copy(pi + PL_NAME, nm, 32);
    }

    // 3. networking role / count / local slot (DAT_005d54bc>=2 is the sole mode-3 trigger). Set BEFORE
    //    the sim's first net pump so net_seams' lazy transport start reads consistent role globals.
    //    (Already set at arm time by the F-gate; re-set here is idempotent + keeps this self-contained.)
    mp_set_net_role(is_host);

    // 4. inject the map into reserved planet slot 0x1f (session_begin forces G_PLANET_INDEX=0x1f and
    //    constructs from Planets[0x1f]; it sets system_index/icon itself but NOT the name -> we do).
    char *pz = (char *)(ADDR_PLANETS + 0x1f * PLANET_STRIDE);
    str_copy(pz + PZ_PATH, md + MD_PATH, 128);
    str_copy(pz + PZ_MAPNAME, md + MD_MAPNAME, 32);
    str_copy(pz + PZ_TLO, md + MD_TLONAME, 12);
    for (int i = 0; i < 4; ++i) {
        ((int *)(pz + PZ_SRCMUL))[i] = 1;
        ((int *)(pz + PZ_SRCADD))[i] = 0;
    }

    // 5. bootstrap the session (seeds RNG, inits profiles, sets _G_LLM_GAME_SESSION_MODE). <0 = failure.
    int r = call_watcall1(ADDR_SESSION_BEGIN, md);
    if (r < 0) {
        lg("; --mp-%s FAILED: session_begin_multi returned %d (map load/setup error)",
           is_host ? "host" : "join", r);
        return;
    }

    // 6. enter the strategic frame (fresh-enter branch => GAME_MODE=2, in-game widget list, CD music).
    *(uint8_t *)ADDR_DLG_FLAGS0 &= (uint8_t)~0x08u;
    ((void (*)(void))ADDR_FINISH_ENTER)();
    int gm = *(const uint8_t *)ADDR_GAME_MODE, sm = *(const uint8_t *)ADDR_SESSION_MODE;
    lg("; --mp-%s -> session_begin ok(%d), GAME_MODE=%d SESSION_MODE=%d slot=%d/%d map=%s (%s)",
       is_host ? "host" : "join", r, gm, sm, slot, n, mapname,
       (gm == GAME_MODE_STRAT && sm == 3) ? "OK: entered mode-3 lockstep strategic game"
                                          : "WARN: not in mode-3 strategic (check SESSION_MODE==3 & GAME_MODE==2)");
}

// ---- MP browser smoke-test entry (--mp-browser) -------------------------------------------------
// Enter the network session-JOIN browser directly, so a single-box launch exercises Workstream U
// Phase 1 (net_seams.cpp install_mp_bootstrap): the browser's per-frame rescan FUN_004bd60d polls the
// discovery stub FUN_0049b905 (now our synthetic-record detour -> lists "MH Host") and draws the
// session-list scrollbar FUN_004c18a6 (now our null-deref guard). Replicates FUN_004bd276's tail (the
// severed "open join browser" head) minus its own dead FUN_0049bb78 gate: set network mode = enumerate
// (DAT_0065678a=-1), point the menu widget list at the browser's widget array, then call the browser
// screen-entry FUN_004bd3e4 (installs FUN_004bd60d as the per-frame tick + sets the screen id). The menu
// state machine then drives it every frame. No transport / 2nd machine needed to validate list+no-crash.
constexpr uintptr_t ADDR_NET_MODE          = mh::addr::net_mode_byte; // -1 = enumerate/join
constexpr uintptr_t ADDR_SEL_SESSION_STATE = mh::addr::selected_session_state;
constexpr uintptr_t ADDR_MENU_WIDGET_LIST  = mh::addr::_G_LLM_UI_MENU_WIDGET_LIST; // widget-array ptr
constexpr uintptr_t ADDR_MENU_XITION       = mh::addr::menu_transition_fn;         // (1) -- screen-transition bookkeeping
constexpr uintptr_t ADDR_BROWSER_ENTER     = mh::addr::browser_screen_enter_fn;    // session-browser screen entry
constexpr uintptr_t PTR_BROWSER_WIDGETS    = mh::addr::browser_widget_array_ptr;   // &(browser widget array)

void mp_open_browser() {
    *(int *)ADDR_NET_MODE          = -1; // enumerate/join mode (the FUN_0049b905-polling path)
    *(int *)ADDR_SEL_SESSION_STATE = 0;
    *(int *)ADDR_MENU_WIDGET_LIST  = (int)PTR_BROWSER_WIDGETS;
    call_watcall1(ADDR_MENU_XITION, (void *)1); // FUN_004bbe0a(1): mark the screen transition
    ((void (*)(void))ADDR_BROWSER_ENTER)();     // FUN_004bd3e4: enter the session browser
    lg("; --mp-browser -> entered network session browser (Phase 1: synth record + scrollbar guard)");
}

// ---- MP Phase 2: auto-drive BOTH peers through the REAL lobby (no cursor needed) ----------------
// (2026-07-14: the "VM cursor can't move" note is STALE -- the VM IS clickable now. These verbs remain
// for HANDS-FREE automation; a manual 2-peer MENU run clicking both peers is also possible.) These
// verbs drive the intact lobby path programmatically end-to-end so mp_run.py can run Phase 2 hands-free:
//   --mp-host-lobby : set the map -> llm_lobby_host_new_game_start -> host lobby; then, once a peer is
//                     connected + slots settle, trigger the game entry (llm_lobby_begin_map_load).
//   --mp-join-lobby : synth-join the host's session -> client lobby (browser rescan not needed; we set
//                     the selected UI row's handle to the synth sentinel and call the join handler).
// Both then ride the A1 transport seams for slot sync; the host's begin_map_load drives the async entry
// (llm_lobby_map_load_async_step -> build_players_from_slots + session_begin_multi) on BOTH peers (the
// two dead map stubs are neutralized in net_seams install_mp_bootstrap). Unlike the force-entry, this
// exercises the REAL lobby (slots, roles, handoff) -- the Phase 2 validation target.
constexpr uintptr_t    ADDR_HOST_NEWGAME   = mh::addr::llm_lobby_host_new_game_start; // map path -> lobby
constexpr uintptr_t    ADDR_JOIN_BY_SEL    = mh::addr::llm_lobby_join_head;           // join the selected session -> lobby
constexpr uintptr_t    ADDR_BEGIN_MAP_LOAD = mh::addr::llm_lobby_begin_map_load;      // registers the async entry
constexpr uintptr_t    ADDR_UI_ROW_ARRAY   = mh::addr::browser_ui_rows;               // {void* handle, u32 idx}[]
constexpr uintptr_t    ADDR_SEL_ROW        = mh::addr::selected_browser_row;
constexpr uintptr_t    ADDR_LOBBY_SLOTS    = mh::addr::_G_LLM_LOBBY_SLOTS;               // stride 0x39
constexpr int          SLOT_STRIDE = 0x39, SLOT_STATUS_OFF = 0x0b, SLOT_NAME_OFF = 0x15; // slot_status: 0=empty; name@+0x15
extern "C" const char *MH_MP_PeerName(int i);                                            // S6: peer i's JOIN player name (net_seams), "" if unset
extern "C" void        MH_MP_ClearPeerName(int player_id);                               // U12: forget a departed peer's JOIN name (net_seams)
extern "C" void        MH_MP_ResetJoinGate(void);                                        // U12: re-arm the S4 join gate when empty (net_seams)
extern "C" int         MH_MP_IsLobbyLeft(int player_id);                                 // U12: 1 = player left the lobby (Cancel), still connected (net_seams)
extern "C" int         MH_MP_HasJoined(int player_id);                                   // N2: 1 = peer sent its own admitted JOIN (net_seams)
extern "C" void        MH_MP_ClearJoined(int player_id);                                 // N2: forget a departed peer's JOIN mark (net_seams)
extern "C" int         MH_MP_IsManual(void);                                             // 1 = pure manual session (gate the join-required filter)
constexpr int          SYNTH_HANDLE = 1;                                                 // net_seams synth session_handle sentinel

int lobby_slots_occupied() {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (*(const uint8_t *)(ADDR_LOBBY_SLOTS + i * SLOT_STRIDE + SLOT_STATUS_OFF) != 0) ++n;
    return n;
}

// N2: count only HUMAN lobby slots (slot_status==1; AI slots are status==2, empty is 0). This is what
// _G_LLM_NET_PCOUNT (0x5d54bc) must be -- it is the network/lockstep PEER count (the mode-3 gate + the
// finalize_custom_map_and_sync wait bound + the peer-removal decrement), NOT the total player count. An
// AI is a LOCAL-sim player, not a network peer; counting it inflated PCOUNT above the human-slot count,
// and finalize's `do{ map_recv_step_stub(); }while(humans_built < PCOUNT)` (whose counter only counts
// humans, and whose stub is hollow) then spun forever -> the host freeze at Start (N2 dump). build_players
// still builds the AI as a Players[] entry -- that is bounded by current_map_data.field2_0x8 (kept = total).
int lobby_human_slots_occupied() {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        if (*(const uint8_t *)(ADDR_LOBBY_SLOTS + i * SLOT_STRIDE + SLOT_STATUS_OFF) == 1) ++n;
    return n;
}

// N1: the client's own lobby slot. In host_assign mode this is the host-assigned id (MH_Net_LocalPlayerId
// after the WELCOME); in declared-id mode it's the configured player_id -- either way MH_Net_LocalPlayerId
// gives it. Falls back to 1 (the original 2-player default) before the transport is up. Host is always 0.
//
// THIS IS THE WIRE/CONNECTION IDENTITY, not necessarily our ARRAY POSITION in ADDR_LOBBY_SLOTS/
// Players[] -- see mp_lobby_array_index below for that, and mp:GS1(b) for why the two are NOT always
// the same number and must NOT be conflated (net_seams.cpp's own N2 comment documents
// _G_LLM_NET_LOCAL_PLAYER_INDEX as needing exactly this wire value, compared against incoming wire
// side_ids -- do not repoint it at the array index).
int mp_lobby_slot(bool is_host) {
    if (is_host) return 0;
    int lp = MH_Net_LocalPlayerId();
    return (lp >= 1 && lp <= 7) ? lp : 1;
}

// mp:GS1(b) (2026-09-21): our own ARRAY POSITION in ADDR_LOBBY_SLOTS/Players[] -- what retail's
// session_begin_multi actually wants when it seeds `PlayerSide = _G_LLM_NET_LOCAL_PLAYER_SLOT`
// (sim_session_begin_multi.cpp, retail VA 0x0045438c-0x004543a9) and then indexes
// `Players[PlayerSide].relation[k] = 2`/`.race_or_faction` by it. mp_lobby_slot() above answers a
// DIFFERENT question (our wire/connection id) and the two used to be written from the same value on
// the unstated assumption that a peer's wire id always equals its lobby SLOT index -- true for the
// first join of a slot, but false the instant a slot is REUSED. The host's own lobby-slot allocator
// (slot_find_or_alloc, retail's admin loop) COMPACTS: when an earlier peer's slot is vacated (a LEAVE),
// the next joiner's *slot* is the freed (lower) index, while its *wire id* is a fresh, never-reused,
// monotonically-increasing connection id (higher). A brand-new process re-joining after an earlier
// peer's process-exit is exactly this: measured on the rig (ghost_exit_rejoin, client2) the ADOPTED
// slots read `slot[1] pid=2` (client2's real seat is ARRAY INDEX 1) while the wire id is 2 -- an EMPTY
// slot (`slot[2] status=0`). Feeding the wire id into `_G_LLM_NET_LOCAL_PLAYER_SLOT` made retail's own
// PlayerSide seed wrong, so turn_engine.cpp's participates()/commit_horizon skip-self test
// (`*player_side != i`) failed to exclude our OWN real row (index 1 != PlayerSide 2), and commit_horizon
// capped our committed horizon against our own never-written _G_LLM_NET_PEER_HORIZON[1] slot (stuck at
// its 10000 ms boot sentinel) forever -- the exact 10000/10030 ms freeze this row (mp:GS1) is about. The
// same-process re-join (`ghost_leave_rejoin`, GREEN) never exposed this because a re-join over a link
// that was never closed keeps the SAME wire id, which still happens to equal its (also unchanged) slot.
//
// Fix: resolve the ARRAY INDEX by SCANNING the lobby slots for the one whose player_id field (+0x01)
// matches our own wire id -- the same side_id -> array-index translation player_by_side_id does for
// every OTHER peer's messages, just applied to ourselves before Players[] exists to look it up in. The
// lobby slots are kept in sync with the host's allocation every lobby frame (S6/U28), so once populated
// the scan is authoritative; the wire-id guess survives only as the PRE-population fallback (right
// after our own JOIN, before the host's first per-frame slot broadcast has arrived). Feeds ONLY
// `ADDR_NET_PLAYERSIDE()` (`_G_LLM_NET_LOCAL_PLAYER_SLOT`) in mp_lobby_entry_tick below -- NOT
// `ADDR_NET_LOCALIDX` (`_G_LLM_NET_LOCAL_PLAYER_INDEX`), which stays wire-id per mp_lobby_slot() above.
// `static`: net_internal.h's seam family (net_seams.cpp/net_discovery.cpp) declares its OWN
// `mp_lobby_array_index` doing the identical scan for the manual-join path -- this TU's copy must not
// clash with that external symbol at link time (this file is force-entry/mp_lobby_entry_tick's own
// family and does not include net_internal.h).
static int mp_lobby_array_index(bool is_host) {
    if (is_host) return 0;
    int                  lp    = mp_lobby_slot(false);
    const unsigned char *slots = (const unsigned char *)ADDR_LOBBY_SLOTS;
    for (int i = 0; i < 8; ++i) {
        const unsigned char *s = slots + i * SLOT_STRIDE;
        if (s[SLOT_STATUS_OFF] != 0 && *(const int *)(s + 0x01) == lp) return i;
    }
    return lp; // slots not populated with our id yet -- pre-sync fallback, corrected within a frame or two
}

// The lobby-entry latches. `g_entry_started` disarms mp_lobby_entry_tick (client entry driver) and
// MH_MP_HostEntryTick; `g_host_start_done` disarms on_begin_map_load's host prep. Both are PER LOBBY
// since mp:RM1 -- cleared by MH_MP_RearmLobbyEntry at the match-end boundary (see it, below).
bool g_entry_started   = false;
int  g_entry_ticks     = 0;
int  g_entry_hb        = 0;
bool g_host_start_done = false;

// N1: set the full N×N symmetric enemy matrix over the OCCUPIED lobby slots (replaces the 2 hand-written
// slot0<->slot1 lines). Deterministic: every peer has the same host-synced slots, so build_players_from_slots
// yields byte-identical Players[] on all peers. Slot relation base = +0x0d (the slot copy starts at +0x05 ->
// Players+0, Players.relation at +0x08 -> slot +0x0d).
//
// D18 (2026-08-29): THE DIAGONAL IS INCLUDED ON PURPOSE, and it is the whole fix. This loop used to skip
// j == i, which left the one cell retail also writes -- and writes PEER-LOCALLY. llm_lobby_build_players_finish
// (0x004beb33) runs `Players[PlayerSide].relation[k] = 2` once per built player k, so it fills the LOCAL
// player's entire relation row INCLUDING k == PlayerSide. PlayerSide differs per peer, so with the diagonal
// left at 0 in the slots each peer ended up with a 2 on its own diagonal and a 0 on the other peer's:
//     host (PlayerSide=0)   P[0] rel=02 02 ..   P[1] rel=02 00 ..
//     peer (PlayerSide=1)   P[0] rel=00 02 ..   P[1] rel=02 02 ..
// i.e. P[i][j] == slot[i][j] everywhere except P[local][local]. That is a permanent per-peer divergence in
// game::g::Players[] in EVERY match, independent of anything else -- one of the TWO root causes of the
// 2026-08-28 external game's step-1 desync (the other is U28; p2_ai_gates/p1_ai_econ/units were downstream
// of it, see D19). Writing the diagonal here makes retail's local write IDEMPOTENT on both peers, so the
// rows come out byte-identical.
// THE DETERMINISM GATE CANNOT SEE THIS, which is why it survived from N1 to 2026-08-29: Players[]
// (0x00e587e9, 416 B) overlaps NO hashed region -- `strat_players` is the OTHER per-player table
// (_G_LLM_STRAT_PLAYERS @0x00cff060). Measured both arms on the 400-step 2-peer scenario: ALL PAIRS
// IDENTICAL with the bug live AND with it fixed. The observable that moves is the PLAYERDUMP[sbm] P[]
// rows, not the hash. Closing that hole is D23; do not read a green gate as proof about this line.
// WHY 2 AND NOT 0: converging on 2 is the conservative direction -- it is the value the owning peer has always
// had for itself, so after this every peer sees for player i exactly what peer i already saw. Clearing the
// diagonal instead would change behaviour for the local player, which retail has never done. Unoccupied slots
// keep 0 throughout (the outer `continue`), matching the observed rel[2..7] == 0.
void mp_set_lobby_relations_nxn() {
    char *s = (char *)ADDR_LOBBY_SLOTS;
    for (int i = 0; i < 8; ++i) {
        if (*(const uint8_t *)(s + i * SLOT_STRIDE + SLOT_STATUS_OFF) == 0) continue; // slot i empty
        for (int j = 0; j < 8; ++j)
            if (*(const uint8_t *)(s + j * SLOT_STRIDE + SLOT_STATUS_OFF) != 0)
                s[i * SLOT_STRIDE + 0x0d + j] = 2; // slot i is enemy of occupied slot j (j == i included -- D18)
    }
}

// mp:RM1 -- zero the sim clocks at the entry prep, so a REMATCH enters session_begin_multi with the
// clock a fresh process would have. The first match of a process reads gclk=0 there because nothing
// has run yet; a rematch read the previous match's final clock (field: 56460 / 8430 / 6059 ms; rig:
// 6390 / 3399) until session_state_reset zeroed it a few instructions into the body. Retail zeroes
// both clocks itself inside llm_strat_session_state_reset (0x00453ee4, called first thing by
// session_begin_multi), so this is NOT what fixes the desync -- the relation rows and the peer count
// are -- but every seam that reads the clock between Start and that reset (the sbm entry logger,
// the lockstep pacing's ms_of) otherwise sees the dead match, and the RM1 gate measures the entry
// state a fresh process has, not the state after retail's own repair. Called in the lobby, with no
// sim running (SESSION_MODE != 3 is the entry driver's own precondition), so nothing is mid-flight.
void mp_zero_sim_clocks() {
    *(double *)ADDR_GAME_CLOCK = 0.0;
    *(double *)ADDR_TOTAL_TIME = 0.0;
}

// mp:RM1 -- THE MANUAL-LOBBY ENTRY PREP RUNS ONCE PER LOBBY, NOT ONCE PER PROCESS. Two latches made
// it once-per-process, and that is the writer of the 2026-09-20 rematch residue (session report
// §11.2): `g_entry_started` disarms the client's mp_lobby_entry_tick, and `g_host_start_done`
// disarms the host's on_begin_map_load. Both are exactly right for the LIFE OF A MATCH -- the
// client's driver must not re-fire into a live game (the 2026-07-26 crash in its own header), and
// the host must not re-prep a Start it already sent -- and both were wrong for the life of the
// PROCESS, which is what a `static bool` / a never-cleared global is. Every 2nd+ match in one
// process therefore skipped the prep: the host never re-derived the N x N relation rows nor the
// peer count, never re-snapshotted the slots and never sent FLAG_START (its P[1..] rel rows read 00,
// the joiner got only retail's bare 0x0a handoff); the joiner's tick never re-set p54bc, PlayerSide
// or the relations, so it built a mode-2 SOLO game from an un-adopted slot array and free-ran past
// the host's horizon while the host waited for lockstep frames that never came (committed pinned at
// the previous match's last clock: 6060 in the field, 3410 on the rig). Re-armed HERE, at the U40
// match-end boundary (mp_session_close, net_discovery.cpp) -- the same seam that re-mints the
// host's session identity and releases the client's link, because "this match is over" is the one
// fact both latches were latched on. NOT at lobby creation: the host's latch guards the Start of a
// lobby, and a lobby that was created but whose match has not ended is exactly the state the latch
// must survive. Verified by tools/test_ui.py `rematch_play` (tools/check_rematch_residue.py).
extern "C" void MH_MP_RearmLobbyEntry(void) {
    g_entry_started   = false;
    g_entry_ticks     = 0;
    g_entry_hb        = 0;
    g_host_start_done = false;
    lg("; RM1: match over -> lobby entry prep RE-ARMED for the next lobby (client driver + host Start prep)");
}

// N2 (2026-07-22): the former mp_normalize_ai_slot_ids() is GONE. It gave every AI lobby slot a distinct
// non-zero player_id to avoid the id-0 "collision", but the RE dig proved that was solving the wrong problem
// and actively causing a worse one:
//   * The AI's player_id NEVER reaches gameplay: build_players_from_slots forces an AI slot's
//     scenario_side_id to -1 (neutral, auto-placed) regardless of its player_id -- host-vs-AI plays fine at
//     id 0. So the id VALUE is irrelevant to placement (the "AI at the host's spot" symptom was the separate
//     OVER-CAPACITY case: seating more players than the map has sides).
//   * Humans are host-assigned ids 1..k (bottom-up), so an AI left at its default 0 can NEVER collide with a
//     human. Handing the AI a low id (as the normalize did) is what REINTRODUCED the collision: a human then
//     shared the AI's id, and slot_for_player_fixed8 (player_id keyed, NO status gate) let the human hijack
//     the AI's slot and drop it on leave.
// The one real hazard of AI-at-0 is that the retail id-keyed removal path (peer_slot_remove ->
// remove_player_slot, both player_id-keyed, first-match, no status gate) aliases the host (slot0, id0) when
// the host cycles an AI slot -> "closed". That is fixed by the invariant "player 0 (the host) is never
// removed" -- a run-before guard on remove_player_slot (net_seams remove_player_slot_guard). The AI slot is
// still removed correctly by its slot INDEX (slot_cycle_cb's slot_compact(user_data)). So: keep AI at id 0,
// assign nothing, and protect the host.

void mp_host_lobby() {
    char *md = (char *)ADDR_CUR_MAP; // the client adopts THIS map out-of-band (host_send_map)
    char  mapname[64];
    net_ini_str("mp_map", MP_MAP_NAME, mapname, sizeof(mapname)); // N1: >=N-start map for >2p
    str_copy(md + MD_PATH, map_is_mpm(mapname) ? "Maps\\" : MP_MAP_PATH, 128);
    str_copy(md + MD_MAPNAME, mapname, 32);
    md[MD_TLONAME] = 0;
    call_watcall1(ADDR_READMAPFILE, md); // fill the header (host_new_game_start reads the path).
    // The lobby is SLOT-driven (build_players_from_slots), so the map's own player-count field drives how
    // many lobby slots show -- leave it (a >=N map shows >=N slots, N filled). No +0x8 override here.
    *(int *)ADDR_NET_IS_HOST      = 1;     // host: the transport LISTENs (lazy-start on lobby poll)
    *(int *)ADDR_NET_PLAYERSIDE() = 0;     // host DRIVES player 0 (game::g::PlayerSide)
    *(int *)ADDR_NET_LOCALIDX     = 0;     // host's local slot index
    ((void (*)(void))ADDR_HOST_NEWGAME)(); // -> FUN_0049b765 (no-op) -> llm_lobby_screen_open
    lg("; --mp-host-lobby -> host new-game entered lobby (map=%s), waiting for peers to auto-start", mapname);
}

void mp_join_lobby() {
    int slot                      = mp_lobby_slot(false); // N1: own/assigned slot (was hardcoded 1)
    *(int *)ADDR_NET_IS_HOST      = 0;                    // client: the transport CONNECTs to mh_net.ini host
    *(int *)ADDR_NET_PLAYERSIDE() = slot;                 // client DRIVES its own player (game::g::PlayerSide) --
                                                          // without a DISTINCT slot both peers are player 0 (same
                                                          // start/color/race) and the lockstep drops the phantom.
    *(int *)ADDR_NET_LOCALIDX         = slot;             // client's local slot index
    *(int *)ADDR_NET_MODE             = -1;               // enumerate/join mode (the FUN_0049b905-polling path)
    *(int *)ADDR_SEL_SESSION_STATE    = 0;
    *(void **)(ADDR_UI_ROW_ARRAY + 0) = (void *)SYNTH_HANDLE; // row 0 handle = synth sentinel (matched on join)
    *(int *)(ADDR_UI_ROW_ARRAY + 4)   = 0;                    // row 0 adapter idx
    *(int *)ADDR_SEL_ROW              = 0;                    // select row 0
    ((void (*)(void))ADDR_JOIN_BY_SEL)();                     // FUN_004bddff: polls synth (count=1) -> match -> lobby
    lg("; --mp-join-lobby -> synth-joined host session -> client lobby (slots=%d)", lobby_slots_occupied());
}

// Phase 2b: the retail lobby allocates a client slot by walking the game peer table
// (DAT_005d5368[], count DAT_005d54b0) -- populated by retail net_udp on connect, but NOT by our TCP
// transport. So we mirror our transport's connections into it each frame: the host's per-frame
// host_net_dispatch admin loop then calls slot_find_or_alloc(player_id,1), allocates the slot, and
// rebroadcasts the 0x1cd snapshot (mode=-1 broadcast -> reaches the client, which copies it wholesale).
constexpr uintptr_t ADDR_PEER_TABLE = mh::addr::peer_table;             // llm_net_session_entry[8] (stride 0x29)
constexpr uintptr_t ADDR_PEER_COUNT = mh::addr::peer_table_admin_count; // delta count; admin-loop bound

// Keep the game peer table in sync with our transport (host side). Remote peers are players 1..N (host
// is 0). +0x25 (accept threshold) must be >=2 or the admin loop kicks the peer; +0x20 (left flag) 0.
// U13: the host peer-mirror's one-shot JOIN/LEAVE edge tracker. Hoisted to file scope so a host lobby-leave
// can reset it: mp_sync is gated OFF (g_host_join_seen=0) + not called while the host is out of the lobby,
// so the DEPART edge that would normally clear a member never fires across a leave -> a re-JOIN into a
// re-created lobby would see was_member still true and emit NO JOIN edge -> the peer is never re-seated.
bool g_was_member[8] = {false};

void mp_sync_host_peer_table() {
    // N2 (2026-07-22): no AI-id normalize -- AI stays at its default id 0 (never collides with humans 1..k;
    // its id never reaches gameplay). The host is protected from the id-0 removal alias by
    // remove_player_slot_guard (net_seams). See the note where mp_normalize_ai_slot_ids used to live.
    // Build the ACTIVE lobby-peer set from the transport's REAL assigned ids (N2 -- NOT positional 1..N).
    // The assigned id is what the client uses as its own local slot index and stamps as the sender of every
    // lobby push, so the host slot allocated for it (slot_find_or_alloc(assigned_id)) carries player_id ==
    // assigned id == the client's local index -- the invariant that keeps a client's race/color edit on its
    // OWN slot. Positional numbering renumbered the survivors when a MIDDLE peer left (id set {1,3} read as
    // {1,2}), which corrupted that invariant end-to-end (the N2 dig: mislabeled -> client's local-slot scan
    // fails -> it edits+pushes ALICE's record incl. player_id 0 -> host memcpy zeroes the real slot ->
    // duplicate slot + race-toggle hits the wrong player + host freeze). Two things make a peer inactive,
    // handled UNIFORMLY: (a) a HARD disconnect drops it out of the active id set; (b) a SOFT leave -- client
    // Cancel back to the browser while staying transport-connected -- sets MH_MP_IsLobbyLeft (via FLAG_LEAVE).
    // The verb/determinism path never sends LEAVE, so the mask is empty and `active` == the transport ids.
    // N2 (2026-07-22): in a MANUAL session a transport-connected peer must NOT be slotted until it has sent
    // its OWN admitted JOIN -- otherwise, the instant the FIRST joiner opens the (global) join gate, every
    // browse-connected-but-not-yet-joined peer pops into the host's lobby as a "Player2" placeholder before it
    // has joined at all. The AUTOMATED force-entry path never sends a UI JOIN (on_join_connect is manual-only),
    // so it must keep slotting all connected peers -> gate the extra requirement on MH_MP_IsManual only (keeps
    // the determinism regression byte-identical). Departure clears the joined mark (vacate loop below).
    const bool require_joined = (MH_MP_IsManual() != 0);
    int        assigned[8], nassigned = MH_Net_ActivePeerIds(assigned, 8);
    int        active[8], nactive     = 0;
    for (int i = 0; i < nassigned; ++i) {
        int pid = assigned[i];
        if (pid >= 1 && pid < 8 && !MH_MP_IsLobbyLeft(pid) && (!require_joined || MH_MP_HasJoined(pid)))
            active[nactive++] = pid;
    }
    bool now_active[8] = {false};
    for (int k = 0; k < nactive; ++k) now_active[active[k]] = true;

    // U14 (2026-07-23): emit the retail delta set (JOIN / LEFT) into the session list and let
    // llm_lobby_host_net_dispatch's admin loop do ALL slot alloc + removal + announces -- we no longer call
    // remove_player_slot ourselves. Mirrors extermin's llm_net_lobby_roster_pump -> merge_refresh
    // (the Extermination cross-version reference): only CHANGED peers (the was_member edge) go in; count = #deltas.
    //
    // BOTH a clean LEAVE and a transport DROP -> event_tag=1 (the admin LEFT branch: remove_player_slot +
    // "<name> left the game"), matching retail (absence == LEFT). We do NOT use the aux<2 path for a drop: that
    // is the retail version/compat KICK -- kick_player_confirm_cb announces "kicked off" AND "incompatible
    // version" + broadcasts a kick -- wrong for a disconnection. The game has no distinct "dropped" line.
    //
    // The JOIN/LEAVE events are one-shot (an admitted JOIN, a single LEAVE/drop), so this was_member edge fires
    // exactly once per real join/departure -- NO debounce needed. The earlier "left"/"kicked" spam was NOT
    // oscillation: it was a STALE COUNT. mp_sync writes the LEFT delta (count=1), then -- since the departed
    // peer was the last one -- MH_MP_ResetJoinGate() below closes g_host_join_seen, so on_lobby_dispatch stops
    // calling mp_sync and the count sticks at 1 -> the retail admin loop re-processes that one delta every
    // frame. Fixed at the source: on_lobby_dispatch now zeroes the count whenever mp_sync is gated out
    // (net_seams.cpp). The N2 remove_player_slot_guard stays installed (shields player 0).
    llm_net_session_entry *entries    = (llm_net_session_entry *)ADDR_PEER_TABLE;
    bool                  *was_member = g_was_member; // U13: file-scope so a host-leave can reset it
    int                    ndelta = 0, vacated = 0;
    for (int p = 1; p < 8; ++p) {
        if (now_active[p] && !was_member[p]) {
            // JOIN. event_tag=2 (retail JOINED) + aux>=2 -> the admin JOIN branch: slot_find_or_alloc + announce
            // "<name> joined". on_join_recv publishes the name before the joined flag, so MH_MP_PeerName is real.
            was_member[p]             = true;
            llm_net_session_entry &e  = entries[ndelta++];
            const char            *pn = MH_MP_PeerName(p);
            str_copy(e.name, pn[0] ? pn : "Player2", sizeof(e.name)); // admin loop announces off entry.name
            e.player_id = p;
            e.event_tag = 2; // JOINED
            e.aux_value = 2; // >= 2 => accepted (not kicked)
            {                // U16: broadcast "<name> joined" so EVERY peer announces it (host announces locally below)
                unsigned char ab[34];
                ab[0] = 1;                // is_join
                ab[1] = (unsigned char)p; // affected player id -> the joiner itself skips (self-announce exists)
                lstrcpynA((char *)(ab + 2), e.name, 32);
                MH_Net_SendAnnounce(ab, 2 + lstrlenA((char *)(ab + 2)) + 1);
            }
        } else if (!now_active[p] && was_member[p]) {
            // DEPART (clean leave OR transport drop) -> LEFT: the game removes the slot + "<name> left the game".
            was_member[p]             = false;
            llm_net_session_entry &e  = entries[ndelta++];
            const char            *pn = MH_MP_PeerName(p); // captured before ClearPeerName below
            str_copy(e.name, pn[0] ? pn : "Player2", sizeof(e.name));
            e.player_id = p;
            e.event_tag = 1; // LEFT
            e.aux_value = 2; // (ignored by the LEFT branch)
            {                // U16: broadcast "<name> left" to every remaining peer (name captured before ClearPeerName)
                unsigned char ab[34];
                ab[0] = 0;                // is_join = 0 -> "left"
                ab[1] = (unsigned char)p; // affected player id (departed; remaining peers all keep it)
                lstrcpynA((char *)(ab + 2), e.name, 32);
                MH_Net_SendAnnounce(ab, 2 + lstrlenA((char *)(ab + 2)) + 1);
            }
            ++vacated;
            lg("; U14: player %d departed [%s] -> LEFT delta; game vacates", p,
               MH_MP_IsLobbyLeft(p) ? "LEFT" : "DROPPED");
            MH_MP_ClearPeerName(p); // DLL bookkeeping -- the GAME does the slot removal, not us
            MH_MP_ClearJoined(p);   // N2: a departed peer must re-JOIN to be re-slotted
        }
    }
    if (vacated) {
        if (nactive == 0) MH_MP_ResetJoinGate();                // all peers gone -> re-arm the S4 join gate
        *(int *)ADDR_NET_PCOUNT = lobby_human_slots_occupied(); // N2: sim PEER count = HUMAN slots (not incl AI)
    }
    *(int *)ADDR_PEER_COUNT = ndelta; // deltas only -> 0 on a stable roster (admin loop no-ops)

    // S6: stamp real player names into the lobby slots every host frame (host-authoritative -- the retail
    // dispatch that runs right after us broadcasts the slot snapshot to the client, so both peers see the same
    // names). slot0 = the host's own name (_G_LLM_MP_PLAYER_NAME @0x5d0d88, from U9), slot p = active player p.
    // U10 audit (D3): KEEP -- the ONLY live per-frame re-read of MH_MP_PeerName, needed when a peer's connection
    // is counted before its JOIN name arrives (the peer-table write is change-gated and would freeze it at
    // "Player2"). Stamps ACTIVE players only, so a left/departed peer's name is never re-written into a slot.
    char *slots = (char *)ADDR_LOBBY_SLOTS;
    str_copy(slots + 0 * SLOT_STRIDE + SLOT_NAME_OFF, (const char *)mh::addr::mp_player_name, 0x20); // host slot0
    for (int k = 0; k < nactive; ++k) {
        const char *pn = MH_MP_PeerName(active[k]);
        if (!pn[0]) continue;
        // N2: stamp into the slot whose player_id MATCHES the id -- don't assume slot index == player_id
        // (slot_find_or_alloc can seat player p into a different slot index). Keeps our name belt consistent
        // with the retail admin-loop stamp (which writes SLOTS[find_or_alloc(pid)].name).
        for (int si = 0; si < 8; ++si)
            if (*(const int *)(slots + si * SLOT_STRIDE + 0x01) == active[k]) {
                str_copy(slots + si * SLOT_STRIDE + SLOT_NAME_OFF, pn, 0x20);
                break;
            }
    }
    // Sim player count (mode-3 / build_players wait bound) = occupied lobby slots once the client synced.
    int occ = lobby_slots_occupied();
    if (occ >= 2) *(int *)ADDR_NET_PCOUNT = lobby_human_slots_occupied(); // N2: PEER count = HUMAN slots (not incl AI)
}

// Unified lobby-entry watch (per lobby frame, both roles). Host also mirrors the transport into the
// game peer table (so the client slot allocates). BOTH peers, once 2 slots have synced + a short settle,
// self-trigger the game entry via begin_map_load -> each runs its own async_step
// (host/client branch) into build_players_from_slots + session_begin_multi with the identical synced
// slots + same .MP (same seed) -> deterministic 2-player lockstep. Self-triggering both is more robust
// than relying on the host's one-shot 0x0a cross-trigger (the host enters the game immediately after
// sending it). The host still broadcasts 0x0a as well; harmless if the client already entered.
// Dump the full lobby role + per-slot state (player_id/race/color/name) so a run shows whether the two
// peers are correctly differentiated (the "same start/color/race" symptom = both driving PlayerSide 0).
void log_lobby_state(bool is_host, const char *when) {
    char *s = (char *)ADDR_LOBBY_SLOTS;
    lg("; --mp-%s: LOBBY %s is_host=%d PlayerSide=%d localidx=%d pcount(54bc)=%d peers(54b0)=%d",
       is_host ? "host" : "join", when, *(int *)ADDR_NET_IS_HOST, *(int *)ADDR_NET_PLAYERSIDE(),
       *(int *)ADDR_NET_LOCALIDX, *(int *)ADDR_NET_PCOUNT, *(int *)ADDR_PEER_COUNT);
    for (int i = 0; i < 3; ++i) {
        unsigned char stt  = *(unsigned char *)(s + i * SLOT_STRIDE + 0x0b); // slot_status
        int           pid  = *(int *)(s + i * SLOT_STRIDE + 0x01);           // player_id
        unsigned char race = *(unsigned char *)(s + i * SLOT_STRIDE + 0x05); // spin_open_val -> race
        unsigned char col  = *(unsigned char *)(s + i * SLOT_STRIDE + 0x06); // color_index
        lg(";   slot[%d] status=%d player_id=%d race=%d color=%d name=%.8s",
           i, stt, pid, race, col, s + i * SLOT_STRIDE + 0x15);
    }
}

// g_entry_started / g_entry_ticks / g_entry_hb: defined beside mp_set_lobby_relations_nxn (mp:RM1).
constexpr int ENTRY_SETTLE_TICKS = 60; // ~1 s of lobby frames after 2 slots are synced (stabilize)
void          mp_lobby_entry_tick(bool is_host) {
    if (g_entry_started) return;
    // A LIVE SESSION IS NOT A LOBBY. g_entry_started alone is not enough: it is set only after OUR
    // begin_map_load call, so a peer that entered the game by the RETAIL route (the host's Start
    // handoff -- the normal path for a manual client) leaves it false forever, and this driver stays
    // armed for the whole match with its gates satisfied by stale lobby slot state.
    //
    // It then only takes one more menu tick to fire. In-game there normally are none -- but the
    // lockstep sync overlay runs in GAME_MODE 3, which DOES tick the menu state machine. So a single
    // stall overlay mid-match re-triggered lobby entry: begin_map_load -> llm_lobby_announce_line ->
    // deref of _G_LLM_UI_MENU_WIDGET_LIST, which is null once no menu screen exists -> access
    // violation, killing the client ~69 s into a game (reproduced twice over the relay, 2026-07-26;
    // root-caused from a full dump: g_entry_started=false, GAME_MODE=3, SESSION_MODE=3).
    //
    // Latch it as well as returning, so the driver is permanently disarmed once a session is live
    // however the peer got there.
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) {
        if (!g_entry_started) lg("; --mp-%s: session already live -- lobby entry driver disarmed", is_host ? "host" : "join");
        g_entry_started = true;
        return;
    }
    // N1: an assign-mode client must not seat itself until the host's WELCOME has settled its id (else it
    // would drive the default slot 1). No-op in declared-id mode / on the host (MH_Net_IdAssigned is 1).
    if (!is_host && !MH_Net_IdAssigned()) {
        if ((++g_entry_hb % 60) == 0) lg("; --mp-join: waiting for host to assign our player id (WELCOME)");
        return;
    }
    int slot = mp_lobby_slot(is_host); // N1: own/assigned WIRE id (was hardcoded is_host?0:1)
    if (is_host) mp_sync_host_peer_table();
    // mp:GS1(b): PLAYERSIDE wants our ARRAY INDEX (mp_lobby_array_index), LOCALIDX wants our WIRE id
    // (`slot`, unchanged) -- see both functions' banners for why these are NOT interchangeable.
    *(int *)ADDR_NET_PLAYERSIDE() = mp_lobby_array_index(is_host); // re-affirm each frame: build_players reads it at entry
    *(int *)ADDR_NET_LOCALIDX     = slot;
    int occ                       = lobby_slots_occupied();
    int want                      = mp_players(); // N1: N from [net] mp_players (default 2)
    // N1: the host advertises the LIVE occupied count as the map player count every frame, so host_send_map
    // propagates it to the client (which adopts the host's map) BEFORE the client's build_players runs -- no
    // stale natural-count clobber race. build_players is bounded by this field (see the at-entry override).
    if (is_host && occ >= 1) *(int *)(ADDR_CUR_MAP + MD_PCOUNT) = occ;
    if (!is_host && occ >= 2) {
        // N2: client lockstep PEER count = HUMAN slots (not incl AI). RECOMPUTE unconditionally so a DEPARTURE
        // decreases it (match the host at line ~656). The old `pcount < occ` guard only ever increased it:
        // `occ` is ALL occupied slots (incl AI) but pcount is HUMANS, so with an AI present a stale pcount can
        // equal occ (e.g. a human leaves: occ 4->3 = host+AI+human, humans 3->2, but stale pcount 3 == occ 3 =>
        // guard false => pcount stuck at 3 => client waits for a phantom 3rd lockstep peer => Start hang/timeout).
        int hp = lobby_human_slots_occupied();
        if (*(int *)ADDR_NET_PCOUNT != hp) {
            *(int *)ADDR_NET_PCOUNT = hp;
            lg("; --mp-join: pcount resync (slots_occupied=%d) -> human pcount=%d", occ, hp);
        }
    }
    if (occ < want) { // wait for all N slots to actually sync (was: exactly 2)
        g_entry_ticks = 0;
        if ((++g_entry_hb % 60) == 0)
            lg("; --mp-%s: waiting for slot sync (slots_occupied=%d/%d peers=%d pcount=%d)",
               is_host ? "host" : "join", occ, want, MH_Net_PeerCount(), *(int *)ADDR_NET_PCOUNT);
        return;
    }
    if (++g_entry_ticks < ENTRY_SETTLE_TICKS) return;
    // Manual-menu client: don't enter until the host's selected map has propagated (else the client
    // session_begins on its own synth map and desyncs). The host broadcasts current_map_data each lobby
    // frame; net_seams intercepts it into current_map_data and flags MH_MP_MapReceived. (Force-entry: the
    // host still sends its fixed map, so this is satisfied there too.)
    if (!is_host && g_verb == VERB_NONE && g_manual_mp && !MH_MP_MapReceived()) { // MANUAL client only --
        if ((++g_entry_hb % 60) == 0) lg("; --mp-join: waiting for host map to propagate before entry");
        return; // force-entry uses its fixed map + no map broadcast, so it must NOT wait on this gate
    }
    // U2: the MANUAL client must not auto-enter at slot-sync either -- it enters only when the host clicks
    // Start (net_seams sets this on the host's FLAG_START). Force-entry (verb) keeps its auto-enter.
    if (!is_host && g_verb == VERB_NONE && g_manual_mp && !MH_Seam_ClientStartReceived()) {
        if ((++g_entry_hb % 60) == 0) lg("; --mp-join: slots+map ready -- waiting for host Start (U2)");
        return;
    }
    // U28: the Start gate has just opened, so the host's AUTHORITATIVE slot array is published. Adopt
    // it BEFORE anything below reads the slots -- relations, the map player count and build_players
    // all derive from this array, and the whole point is that they derive from the HOST's copy rather
    // than ours. Everything downstream is recomputed from the adopted bytes, `occ` included: it was
    // sampled near the top of this function from our own slots, and using that stale value here would
    // reintroduce the disagreement one field to the left.
    if (!is_host && g_verb == VERB_NONE && g_manual_mp) {
        unsigned char hs[SLOT_STRIDE * 8];
        int           n = MH_Seam_TakeStartSlots(hs, sizeof(hs));
        if (n == (int)sizeof(hs)) {
            // DIFF BEFORE OVERWRITING, and log it. Adopting the host's array makes the peers agree
            // whatever happened, so without this the interesting event -- "the host and I disagreed at
            // Start, and my copy just lost" -- would be silently repaired and never observed. That is
            // precisely how the 2026-08-28 desync stayed invisible for eight minutes. A disagreement
            // here is EXPECTED and benign (a slot edit in flight); a disagreement that recurs every
            // match is a bug in the lobby sync, and this line is what would show it.
            // THE RELATION ROW (+0x0d..+0x14) IS EXCLUDED FROM THIS COMPARISON, and that is measured,
            // not assumed. The host runs mp_set_lobby_relations_nxn() BEFORE it snapshots, and we run
            // ours a few lines BELOW this adopt, so at this instant the host's rows read 02 02 and
            // ours still read 00 00 -- a guaranteed, structural difference on every single match.
            // Comparing it made this line fire every time with every named field identical (measured
            // on the 2026-08-29 rig run: "bytes differing: 0d 0e"), which is an alarm that trains you
            // to ignore it -- the mostly-green-occasionally-red oracle this ledger refuses elsewhere.
            // Both peers recompute the row from the (now identical) adopted array immediately after,
            // and D18 made that computation agree, so nothing is lost by not checking it here.
            constexpr int        REL_OFF = 0x0d, REL_LEN = 8;
            const unsigned char *mine = (const unsigned char *)ADDR_LOBBY_SLOTS;
            for (int si = 0; si < 8; ++si) {
                const unsigned char *a = mine + si * SLOT_STRIDE, *b = hs + si * SLOT_STRIDE;
                char                 offs[160];
                int                  no = 0;
                for (int k = 0; k < SLOT_STRIDE && no < 128; ++k) {
                    if (k == REL_OFF) k += REL_LEN - 1; // skip the relation row, see above
                    else if (a[k] != b[k]) no += wsprintfA(offs + no, " %02x", k);
                }
                offs[no] = 0;
                if (no == 0) continue; // agrees on everything that matters here
                // Name the fields the 2026-08-28 desync actually turned on rather than dumping 0x39
                // bytes: status@+0x0b, player_id@+0x01, race@+0x05, color@+0x06 -- plus the raw offsets
                // so a difference in a field nobody has named yet is still diagnosable.
                lg("; U28 SLOT DIFF at Start slot[%d]: ours status=%d pid=%d race=%d color=%d | HOST "
                                     "status=%d pid=%d race=%d color=%d -- taking the host's; bytes differing:%s",
                            si, a[0x0b], *(const int *)(a + 0x01), a[0x05], a[0x06], b[0x0b],
                            *(const int *)(b + 0x01), b[0x05], b[0x06], offs);
            }
            memcpy((void *)ADDR_LOBBY_SLOTS, hs, sizeof(hs));
            occ = lobby_slots_occupied(); // recompute from the host's array, not ours
            // mp:GS1(b): re-resolve OUR OWN array index against the array we just adopted -- it was
            // computed from our PRE-adoption copy above, and the one case a "U28 SLOT DIFF" line above
            // can name is our own row moving (a slot edit in flight right at Start). Cheap and
            // idempotent when nothing moved; the alternative is a PlayerSide that silently outlives its
            // own array. LOCALIDX (the wire id, `slot`) does not depend on this array at all.
            const int array_idx           = mp_lobby_array_index(is_host);
            *(int *)ADDR_NET_PLAYERSIDE() = array_idx;
            *(int *)ADDR_NET_LOCALIDX     = slot;
            lg("; U28: adopted the host's authoritative lobby slots at Start (occ=%d, PlayerSide=%d)", occ, array_idx);
        } else {
            // Not fatal, and deliberately loud: the host sent the legacy bare signal, so we are back
            // to each peer building from its own copy -- the exact configuration that desynced on
            // 2026-08-28. Almost always a build mismatch between the peers.
            lg("; U28: host sent NO slot payload with Start -- building from OUR OWN slots (occ=%d). "
                                 "Peers on different builds? This is the pre-U28 desync configuration.",
                        occ);
        }
    }
    // Symmetric enemy relations: build_players_from_slots copies slot bytes +0x0d.. -> Players.relation[]
    // then sets ONLY the local player's row (Players[PlayerSide].relation[i]=2) -> the peers end with
    // different relation arrays (strat_players desync). Set the FULL N×N matrix in the (identical, synced)
    // slots the SAME way on every peer so build_players yields byte-identical Players[]. (N1: was the 2 hand-
    // written slot0<->slot1 lines; now all occupied-slot pairs -- D18: INCLUDING THE DIAGONAL, which the
    // local write also touches and which N1 originally skipped. See mp_set_lobby_relations_nxn.)
    mp_set_lobby_relations_nxn();
    // U12 test hook: [net] hold_start=1 keeps a VERB peer in the lobby instead of auto-entering, so the host's
    // mp_sync_host_peer_table (called at the top of this fn every frame) stays live to exercise the peer-
    // departure vacate. The client just idles in the lobby; kill it to drive the disconnect. Dormant unless set.
    if (MH_Seam_HoldStart()) {
        if ((++g_entry_hb % 120) == 0)
            lg("; --mp-%s: hold_start -- staying in lobby (occ=%d) for U12 disconnect test", is_host ? "host" : "join", occ);
        return;
    }
    // N1: the retail build (llm_lobby_build_players_from_slots -> _finish) bounds the players it builds by
    // current_map_data.field2_0x8 (the map's DESIGNED player count), NOT the occupied slot count -- so on a
    // 2-player map only 2 Players[] get built even with 3 slots synced, and the 3rd human is never marked
    // network-human (host never locksteps with it, client hangs entering). Override it to the occupied count
    // (identical on every peer -> byte-identical Players[] build). Mirrors the force-entry's +0x8 override.
    *(int *)(ADDR_CUR_MAP + MD_PCOUNT) = occ;
    // U29 (c): THE SCREEN GATE. Every condition above is satisfied by lobby STATE -- occupied slots, a
    // received map, a Start flag -- and none of them notices that the lobby is gone. After a host-left
    // the client sits on the discovery browser while all three remain true (measured 2026-08-28: the
    // driver logged "slots+map ready -- waiting for host Start" unbroken for 39 s from the browser,
    // occ=2 map=1, on a lobby that no longer existed). A FLAG_START arriving in that window would have
    // force-entered a game built from the dead lobby's slots. This is the same shape as the 2026-07-26
    // crash in this function's own header -- a driver whose gates are satisfied by stale lobby state
    // and which nothing disarms when the lobby goes away; that fix latched on SESSION_MODE==3, this is
    // the other direction. So require the lobby to actually BE the screen we are entering from.
    // MANUAL CLIENT ONLY: the manual host enters through the Start button (on_begin_map_load) and
    // force-entry has no menu screen at all, so gating either would break a working path.
    // [net] u29_screen_gate=0 removes THIS gate only, leaving the teardown in place -- which is what
    // isolates the two. With both on, (b) is closed twice over; with the teardown off and this on, the
    // gate is what refuses (and says so); with both off, the injected Start force-enters from the
    // browser, i.e. the 2026-08-28 hazard reproduced on demand.
    if (!is_host && g_verb == VERB_NONE && g_manual_mp && net_ini_int("u29_screen_gate", 1) && *(unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != mh::addr::lobby_widget_origin) {
        if ((++g_entry_hb % 60) == 0)
            lg("; U29: entry gates are satisfied but the LOBBY IS NOT THE ACTIVE SCREEN (list=%08X) "
                                 "-- refusing begin_map_load (stale lobby state after a host-left)",
                        *(unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST);
        return;
    }
    log_lobby_state(is_host, "at-entry");
    mp_zero_sim_clocks(); // RM1: enter with a fresh process's clock (see the host prep)
    lg("; --mp-%s: TRIGGERING game entry via begin_map_load (slots_occupied=%d pcount=%d map_pcount->%d)",
       is_host ? "host" : "join", occ, *(int *)ADDR_NET_PCOUNT, occ);
    ((void (*)(void))ADDR_BEGIN_MAP_LOAD)();
    g_entry_started = true;
}

// ---- the parsed action, fired once at main-menu idle --------------------------------------------
// ---- TACT-PREP piece 5: --tactical -------------------------------------------------------------
//
// Load a save, synthesise the squad blackboard, and enter a tactical mission -- with no human input
// and no gameplay setup. This is the entry an oracle drives; the cadence hook in seams/harness.cpp
// ([harness] tact_hash_step) is what watches it.
//
// WHY SYNTHESISE RATHER THAN CALL THE GATE. llm_strat_try_enter_tactical_mission would make the
// harness satisfy llm_strat_bldg_gather_nearby_squad_status's predicates against a real roster --
// control group 0 non-empty, a member whose unit proto carries soldiers, wrapped tile distance to an
// enemy MAIN BASE under 15, total soldiers under 0x41. Those are properties of a played game. What
// gather() writes is a 64-slot stride-0x10 record array and five scalars, so the harness writes
// those and skips the gameplay entirely.
//
// WHAT THE MISSION ACTUALLY READS BACK. llm_tact_mission_load takes only two fields per slot -- the
// hp PERCENT and the commando flag -- and pairs them with the CHARACTER type from the mission file;
// the unit_proto_id field is not consulted at spawn. The map is chosen by ONE scalar:
// _G_LLM_STRAT_PLAYERS[squad_bb_target_owner].race picks POZ<CurrentSystem>{O,L}.DAT. The remaining
// scalars matter on the way OUT, where llm_strat_squad_assault_resolve indexes
// buildings[target_owner][target_building_idx] -- which is why the defaults below name player 1's
// building 0 and why every slot's strategic unit index is left ZERO: a zero index makes the exit
// resolve skip the slot cleanly instead of damaging a unit that was never in the mission.
//
// KNOBS live in [tactical] of mh_net.ini, so a scenario is a config change and not a rebuild.
void enter_tactical() {
    // 1. The strategic session, via the same live callback --load uses.
    *(char **)ADDR_SAVE_NAME_PTR = g_arg;
    *(uint8_t *)ADDR_DLG_FLAGS0 &= (uint8_t)~0x08u;
    ((void (*)(void))ADDR_LOADGAME_CONFIRM)();
    if (*(const uint8_t *)ADDR_GAME_MODE != GAME_MODE_STRAT) {
        lg("; --tactical \"%s\": FAILED to load the save (still in menu) -- not entering tactical", g_arg);
        return;
    }

    // 2. The blackboard. Cleared in full first, exactly as gather() does: a stale slot past the
    //    count is not merely untidy, llm_tact_mission_load walks slots as it consumes DISPOSITION
    //    lines and a leftover nonzero hp would spawn a unit the caller never asked for.
    int squad  = ini_int("tactical", "squad", 8);
    int hp_pct = ini_int("tactical", "hp_pct", 100);
    int cmdo   = ini_int("tactical", "commando", 0);
    int owner  = ini_int("tactical", "target_owner", 1);
    int bidx   = ini_int("tactical", "target_building", 0);
    // CLAMPED because the mission-identity log below INDEXES _G_LLM_STRAT_PLAYERS[owner] to read the
    // race, and the table is 8 wide. The game indexes it too (llm_tact_mission_start @0x0042913e), so
    // an out-of-range owner was already a bad idea -- it just had no reader here until now.
    if (owner < 0) owner = 0;
    if (owner > 7) owner = 7;
    if (squad < 1) squad = 1;
    if (squad > SQUAD_BB_SLOTS) squad = SQUAD_BB_SLOTS;
    if (hp_pct < 1) hp_pct = 1; // gather() floors at 1; 0 means "this soldier died"
    if (hp_pct > 100) hp_pct = 100;

    uint8_t *bb = (uint8_t *)ADDR_SQUAD_BB();
    for (int i = 0; i < SQUAD_BB_SLOTS * SQUAD_BB_STRIDE; ++i) bb[i] = 0;
    for (int i = 0; i < squad; ++i) {
        uint8_t *slot                        = bb + i * SQUAD_BB_STRIDE;
        *(int *)(slot + SQUAD_BB_OFF_PROTO)  = 0;      // not read at spawn
        *(int *)(slot + SQUAD_BB_OFF_HP_PCT) = hp_pct; // == _G_LLM_SQUAD_STATUS[i]
        *(int *)(slot + SQUAD_BB_OFF_UNIT)   = 0;      // exit resolve skips a zero index
        *(int *)(slot + SQUAD_BB_OFF_CMDO)   = cmdo;
    }
    *(int *)ADDR_SQUAD_BB_COUNT()  = squad;
    *(int *)ADDR_BB_SCAN_PLAYER()  = *(const uint16_t *)ADDR_PLAYER_SIDE;
    *(int *)ADDR_BB_TARGET_OWNER() = owner;
    *(int *)ADDR_BB_TARGET_ID()    = 0;
    *(int *)ADDR_BB_TARGET_PCT()   = 100;
    *(int *)ADDR_BB_TARGET_IDX()   = bidx;

    // 3. Save the planet. NOT optional and not bookkeeping: the exit
    //    (llm_tact_mission_end_return_to_strategic) restores tile_objects, passable and
    //    _G_LLM_STRAT_PATH_BUFFERS by RE-READING this file, because the mission overwrites all three
    //    in place. Skip it and the exit restores whatever the last save left on disk.
    //    AND ITS RETURN IS CHECKED HERE, deliberately unlike the original. The retail entry gate
    //    llm_strat_try_enter_tactical_mission DISCARDS it -- 0x0044d38f CALL, then 0x0044d394
    //    MOV GAME_MODE,6 with no TEST -- so a failed save (utils_open_file returning NULL, or any
    //    block write failing) silently leaves the exit restoring a STALE planet. Reproducing that
    //    here would be worse than in the game: this verb feeds the tactical determinism oracle, and
    //    a stale restore is exactly the shape of a green run that proves nothing (TACT-WRITERS,
    //    2026-08-24).
    // 2b. WHICH MISSION. `system` overrides CurrentSystem so all six shipped POZ files are reachable
    //     from ONE save -- the shipped set is POZ1L..POZ3O and a save pins exactly one system, so
    //     without this a per-mission table needs a save per system that may not exist. 0 = leave the
    //     save's own value alone, which is the shipping default and changes nothing.
    //     It is set BEFORE mission_start because that is the only reader that matters here; it is not
    //     restored afterwards, so a run that overrides is a MEASUREMENT run and not a game.
    int sys_override = ini_int("tactical", "system", 0);
    if (sys_override > 0) {
        lg("; --tactical: [tactical] system=%d OVERRIDES CurrentSystem (was %d) -- measurement run",
           sys_override, *(const int *)ADDR_CURRENT_SYSTEM);
        *(int *)ADDR_CURRENT_SYSTEM = sys_override;
    }

    int      planet  = *(const int *)ADDR_PLANET_INDEX;
    uint32_t save_ok = mh::call::map_SavePlanetToDisk((uint32_t)planet, 1u);
    if (!save_ok) {
        lg("; --tactical: map_SavePlanetToDisk(planet=%d) FAILED -- the exit would restore a stale "
           "planet; refusing to enter tactical mode",
           planet);
        return;
    }

    // 4. In. mission_start is void(void) and reads only the globals set above.
    //
    // OURS, NOT THE ORIGINAL (TACT1-P C4, 2026-09-04). This raw call was the FIRST thing the
    // whole-verified-set tombstone sweep caught: `llm_tact_mission_start @004290F2 ENTERED`. The
    // shipping path is fine -- llm_strat_try_enter_tactical_mission binds this row through
    // MH_LIBMH_BIND and reaches our body -- so the only route into the original was this force-entry
    // verb, i.e. the harness that exists to TEST the tactical code calling the code it is not
    // testing. Every tactical scenario enters through here, so leaving it raw would have made the
    // whole tact domain's rig coverage a measurement of the original.
    *(uint8_t *)ADDR_GAME_MODE = (uint8_t)GAME_MODE_TACT;
    mh::tact::mission_start();

    int mode = *(const uint8_t *)ADDR_GAME_MODE;
    // THE MISSION IDENTITY, derived the same way llm_tact_mission_start derives it. Read AFTER the
    // call, so the values logged are the ones the load actually used.
    int sys  = *(const int *)ADDR_CURRENT_SYSTEM;
    int race = *(const int *)(ADDR_PLAYERS_PROF + (uintptr_t)owner * PLAYER_PROF_STRIDE +
                              PLAYER_PROF_OFF_RACE);
    lg("; --tactical \"%s\": squad=%d hp=%d%% owner=%d bldg=%d planet=%d -> GAME_MODE=%d (%s)", g_arg,
       squad, hp_pct, owner, bidx, planet, mode,
       mode == GAME_MODE_TACT ? "OK: tactical mission running" : "FAILED: mission_start bounced us out");
    lg("; --tactical MISSION POZ%d%c.DAT (CurrentSystem=%d owner=%d race=%d %s)", sys,
       race == RACE_HUMAN ? 'O' : 'L', sys, owner, race,
       race == RACE_HUMAN ? "HUMAN" : "alien");
}

void fire_action() {
    if (g_verb == VERB_LOAD) {
        *(char **)ADDR_SAVE_NAME_PTR = g_arg;           // selected-save name (llm_game_load reads it)
        *(uint8_t *)ADDR_DLG_FLAGS0 &= (uint8_t)~0x08u; // force the fresh-enter (GAME_MODE=2) branch
        ((void (*)(void))ADDR_LOADGAME_CONFIRM)();      // llm_menu_loadgame_confirm: load + enter
        int mode = *(const uint8_t *)ADDR_GAME_MODE;
        lg("; --load \"%s\" -> GAME_MODE=%d (%s)", g_arg, mode,
           mode == GAME_MODE_STRAT ? "OK: entered strategic game" : "FAILED: still in menu (bad save name?)");
    } else if (g_verb == VERB_TACTICAL) {
        enter_tactical();
    } else if (g_verb == VERB_NEWGAME) {
        // Campaign new game: set the race the race-select screen would set, then call the terminal
        // callback. session_begin(race, 0) inside it does the full new-game reset (CurrentSystem=1,
        // start G_PLANET_INDEX, clear planet statuses, new_game_init) + planet load + player profiles,
        // and campaign_start_and_enter sets GAME_MODE=2 itself. Race-only, no planet select.
        int race                  = parse_race(g_arg);
        *(int *)ADDR_NEWGAME_RACE = race;        // _G_LLM_MENU_NEWGAME_RACE (1=H, 2=A)
        ((void (*)(void))ADDR_CAMPAIGN_START)(); // llm_menu_campaign_start_and_enter
        int mode = *(const uint8_t *)ADDR_GAME_MODE;
        lg("; --newgame race=%d(%s) -> GAME_MODE=%d (%s)", race, race == RACE_A ? "A" : "H", mode,
           mode == GAME_MODE_STRAT ? "OK: entered new campaign" : "FAILED: still in menu");
    } else if (g_verb == VERB_MP_HOST || g_verb == VERB_MP_JOIN) {
        // D17 step 4: force-enter a 2-player mode-3 lockstep game (host=slot0 / join=slot1). Both peers
        // load the fixed PoC map; the A1 transport seams carry the lockstep stream. Host/port come from
        // mh_net.ini (read by the net_seams transport); the verb only picks the role. --mp-join's arg
        // (host ip) is currently informational -- configure the peer via mh_net.ini [net] host=.
        mp_force_entry(g_verb == VERB_MP_HOST);
    } else if (g_verb == VERB_MP_BROWSER) {
        mp_open_browser(); // Workstream U Phase 1 smoke test: reach the session browser (no transport)
    } else if (g_verb == VERB_MP_HOST_LOBBY) {
        mp_host_lobby(); // Phase 2: host enters the real lobby; auto-start watch drives the entry
    } else if (g_verb == VERB_MP_JOIN_LOBBY) {
        mp_join_lobby(); // Phase 2: client synth-joins into the real lobby (cursor-free)
    }
}

void on_menu_tick() {
    // Manual-menu MP (no launch verb): once the transport is up (i.e. we're in the real lobby, reached by
    // the hand-clicked button->browser->join / host create), run the SAME proven lobby driver the
    // force-entry uses -- slot sync + host peer-table mirror + PlayerSide/relations + auto-enter. This is
    // what drives the client past the "connecting" wait screen (its own lobby dispatch stalls there). Role
    // comes from the net stubs (IS_HOST). Runs every menu frame (llm_ui_menu_state_tick always ticks).
    // PURE manual path only (no launch verb). A force-entry verb (VERB_MP_*_LOBBY) must keep its own
    // proven g_fired path below -- letting this branch preempt it broke force-entry determinism (the host
    // stopped running mp_lobby_entry_tick and fell through to MH_MP_HostEntryTick + a double-entry).
    // g_manual_mp is also set by on_host_advertise, which fires for the force-entry host too, so gate on
    // the verb being absent.
    if (g_verb == VERB_NONE && g_manual_mp) {
        // U29DIAG: the menu-tick counterpart of U3DIAG. U3DIAG lives in the LOBBY DISPATCH, which is
        // exactly what dies when the client is thrown out of a lobby -- it went silent for the whole
        // 41 s of the 2026-08-28 phantom, which is why that report had no render state at all behind
        // it. This one is driven by llm_ui_menu_state_tick, so it keeps reporting on the browser.
        // It prints the ACTIVE widget list plus the shared slide frame's geometry, because the whole
        // U22/U29 class of bug is "the container being drawn is not the one that was last centred".
        {
            static int  dc           = 0;
            static char u29last[256] = {0}; // mp:SES5 decision (3): dedupe -- U29DIAG was 1105 of
                                            // 1107 mh_launch.log lines in one measured run, almost
                                            // all identical; log only when the text changes.
            if ((dc++ % 120) == 0) {
                // menu_state/saved/gm/dlg are the DRAW GATES (0x004b79a8): the lobby draws only if
                // gm==3 && (dlg&0x10), and when saved==1 the draw walks widget_list+0x14 -- the PARENT
                // -- not the active list. That last one is the whole question here, so log it.
                unsigned wl = *(unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
                char     b[256];
                wsprintfA(b,
                          "; U29DIAG list=%08X (lobby=%d localbr=%d sessbr=%d mappick=%d) parent=%08X frame_x=%d right_x=%d ms=%u saved=%u gm=%u dlg=%02X occ=%d map=%d",
                          wl, wl == mh::addr::lobby_widget_origin,
                          wl == mh::addr::local_browser_widget_origin,
                          wl == mh::addr::browser_widget_array_ptr,
                          wl == mh::addr::map_picker_widget_origin,
                          wl ? *(unsigned *)(wl + 0x14) : 0u,
                          *(int *)mh::addr::lobby_frame_x, *(int *)mh::addr::lobby_right_panel_x,
                          *(const unsigned char *)mh::addr::_G_LLM_UI_MENU_STATE,
                          *(const unsigned char *)mh::addr::_G_LLM_UI_MENU_SAVED_STATE,
                          *(const unsigned char *)mh::addr::_G_LLM_GAME_MODE,
                          *(const unsigned char *)mh::addr::_G_LLM_DLG_STATE_FLAGS,
                          lobby_slots_occupied(), MH_MP_MapReceived());
                if (lstrcmpA(b, u29last) != 0) {
                    lg("%s", b);
                    lstrcpynA(u29last, b, sizeof(u29last));
                }
            }
        }
        // U29 (a): TEAR THE LOBBY MODEL DOWN when the lobby stops being the active screen. The
        // render fix below stops the player SEEING a dead lobby; this stops the client BELIEVING in
        // one. Over the 41 s of the 2026-08-28 phantom the driver kept reporting occ=2 map=1 from the
        // closed lobby, on the old map, with the entry driver armed -- so the slots, the received-map
        // flag and any pending Start are all stale the moment we leave, whichever way we left
        // (host 0x0e, link loss, or our own Cancel). Edge-triggered, so it costs one compare a frame
        // and cannot fight the lobby while we are still in it.
        {
            static int was_lobby = 0;
            const int  is_lobby  = (*(unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST == mh::addr::lobby_widget_origin);
            // [net] u29_teardown=0 restores the pre-fix behaviour (the model survives the lobby),
            // so the negative arm is reachable from an ini instead of by reverting code -- same
            // reasoning and same shape as U28's [net] start_slots.
            if (was_lobby && !is_lobby && net_ini_int("u29_teardown", 1)) {
                memset((void *)ADDR_LOBBY_SLOTS, 0, SLOT_STRIDE * 8);
                MH_MP_ResetMapReceived();
                MH_Seam_ClearStartReceived();
                lg("; U29: left the lobby screen -> lobby model torn down (slots cleared, map + Start "
                   "flags reset; occ=%d map=%d)",
                   lobby_slots_occupied(), MH_MP_MapReceived());
            }
            // ...AND THE SAME ON THE WAY BACK IN. A Start belongs to the lobby we were in when it
            // arrived. Clearing only on the way OUT leaves one that arrived while we sat on the
            // browser still latched, so the moment we re-join a NEW lobby every entry condition is
            // satisfied at once and the client force-enters a game the host never started. Found by
            // U29 (b)'s own test on its first run: the injected Start was swallowed on the browser
            // exactly as intended, then fired on the rejoin and dumped the client into the game
            // (it ended on the in-game "Player not responding" dialog). Real hazard, not an artefact
            // of the hook -- a late or duplicated FLAG_START on the wire does the same thing.
            if (!was_lobby && is_lobby) {
                MH_Seam_ClearStartReceived();
                lg("; U29: entered a lobby -> dropped any Start latched while we were off it");
            }
            was_lobby = is_lobby;
        }
        // U29/U22: A BROWSER IS NEVER A MODAL CHILD OF THE LOBBY -- repair the link if it is.
        // llm_ui_menu_push_screen is a MODAL push: it writes `list->parent = <current list>` and sets
        // SAVED_STATE=1, and SAVED_STATE=1 means "draw the PARENT" (see _G_LLM_UI_MENU_SAVED_STATE,
        // "byte; 1 => draw parent"). On a host-left llm_lobby_finalize_transfer_or_enter pushes the
        // browser exactly that way -- as a child OF THE LOBBY -- so input goes to the browser while
        // the LOBBY is what gets drawn. That is the phantom the user reported on 2026-08-28, and it
        // is a parent pointer, not geometry: U29DIAG measured frame_x=-124 (on-screen, correct) in
        // BOTH the broken and the healthy state, so every coordinate was already right while the
        // wrong container was being drawn.
        //     list=00653EA7 (session browser)  parent=00653F3F (lobby)  saved=1   <- broken
        //     list=00653EA7                    parent=00000000          saved=10  <- healthy root
        // Repaired HERE, per frame, rather than at the push: the browser is re-pushed as a child by
        // more than one route (finalize's push, and the S3 discovery re-arm behind it), and fixing
        // one pusher left the other -- measured, the broken state still held 111 of 113 ticks. The
        // invariant is the thing worth stating anyway: once the lobby has been left, it is not a
        // backdrop for anything. Scoped to a browser whose parent is SPECIFICALLY the lobby, so a
        // genuine modal (a dialog over the browser) is untouched.
        {
            unsigned wl = *(unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
            // [net] u29_reroot=0 leaves the browser parented to the dead lobby -- i.e. the phantom,
            // reachable from an ini so the render fix's negative arm needs no code edit.
            if (net_ini_int("u29_reroot", 1) && (wl == mh::addr::browser_widget_array_ptr || wl == mh::addr::local_browser_widget_origin) && *(unsigned *)(wl + 0x14) == mh::addr::lobby_widget_origin) {
                *(unsigned *)(wl + 0x14)                               = 0;  // root screen: no backdrop
                *(unsigned char *)mh::addr::_G_LLM_UI_MENU_SAVED_STATE = 10; // draw THIS list, not a parent
                call_watcall1(mh::addr::llm_ui_widget_list_center, (void *)wl);
                static int logged = 0;
                if (logged < 3) {
                    ++logged;
                    lg("; U29: browser was parented to the dead lobby (saved=1) -> re-rooted + re-centred");
                }
            }
        }
        // S3/S5-core: real client discovery — kick the connect once a host IP is typed, then re-arm the
        // browser to list the host's received SESSION_INFO. Runs pre-start too (to kick the connect);
        // no-ops for the host + once listed. Must be before the IsStarted gate below.
        MH_Seam_ClientDiscoveryTick();
        // ISOLATION: run the driver ONLY on the client. The host's lobby worked from its dispatch+mirror
        // alone (previous increment); adding the host-side on_menu_tick driver stalled it the moment the
        // client connected. So keep the host on its dispatch path; host entry is triggered from the
        // lobby dispatch instead (MH_MP_HostEntryTick). Client still needs this driver to auto-enter.
        if (MH_Net_IsStarted() && *(int *)ADDR_NET_IS_HOST == 0) {
            static int c = 0;
            if ((c++ % 120) == 0)
                lg("; DIAG manual-mp CLIENT driver tick #%d occ=%d map=%d", c - 1,
                   lobby_slots_occupied(), MH_MP_MapReceived());
            if (lobby_slots_occupied() >= 2) MH_MP_ClientPollMap(); // drain the map ourselves (dispatch stalls)
            mp_lobby_entry_tick(false);
        }
        return;
    }
    if (g_fired) {
        // Lobby verbs: keep watching every lobby frame -- both peers sync slots then self-trigger the
        // game entry once 2 slots are present (host also mirrors the transport into the peer table).
        if (g_verb == VERB_MP_HOST_LOBBY) mp_lobby_entry_tick(true);
        else if (g_verb == VERB_MP_JOIN_LOBBY) mp_lobby_entry_tick(false);
        return;
    }
    // Fire only once the main menu is fully IDLE: GAME_MODE==3 (menu frame) AND MENU_STATE==3 (the
    // idle/poll state). Waiting for idle is REQUIRED for correctness, not cosmetics: the state-0 handler
    // llm_ui_main_menu_screen_load runs the planet/system "known" recheck
    // (llm_progress_recheck_planet_system_all_players -> HandleProgress) + galaxy-map init; reaching
    // idle (0->1->2->3) guarantees it has run. An earlier attempt to fire on the first mode-3 tick (top
    // of the tick, before state-0) skipped it and corrupted planet state -- --load showed ALL planets
    // unlocked, --newgame crashed opening the starmap. (And it didn't even remove the flash: llm_ui_menu_frame
    // draws+presents the menu AFTER state_tick returns in the same frame, so the fire frame still draws
    // once. The brief menu flash is an accepted cosmetic cost of this hook approach.)
    // Both globals are single BYTES in mh.exe -- read as bytes (an int read of MENU_STATE straddles into
    // _G_LLM_UI_MENU_WIDGET_LIST at 0x0065428d and never matches).
    if (*(const uint8_t *)ADDR_GAME_MODE != GAME_MODE_MENU) return;   // not the main-menu frame
    if (*(const uint8_t *)ADDR_MENU_STATE != MENU_STATE_IDLE) return; // menu not idle yet

    // F-gate: MP verbs start the transport here and WAIT (staying in the menu) until the expected
    // peer(s) have connected, so both peers enter mode-3 together from step 0. Without this the host
    // would force-enter and start the sim solo -- running ahead before the client joins (and continuing
    // after it drops) -- a latent desync since the force-entry bypasses the lobby's "all present" gate.
    if (g_verb == VERB_MP_HOST || g_verb == VERB_MP_JOIN) {
        // N1: host waits for N-1 clients; a client waits for its 1 host connection.
        int expected = (g_verb == VERB_MP_HOST) ? mp_players() - 1 : 1;
        if (!g_mp_armed) {
            mp_set_net_role(g_verb == VERB_MP_HOST); // IS_HOST/LOCAL_PLAYER_INDEX for the transport
            MH_Seam_StartTransport();                // host listens / client connects to mh_net.ini host
            g_mp_armed = true;
            lg("; --mp-%s: transport armed at menu, waiting for %d peer(s) before entering...",
               g_verb == VERB_MP_HOST ? "host" : "join", expected);
        }
        if (!MH_Net_IsStarted() || MH_Net_PeerCount() < expected) {
            if ((++g_mp_wait_ticks % 300) == 0) // ~heartbeat every ~300 idle frames
                lg("; --mp: still waiting for peer (peers=%d started=%d)",
                   MH_Net_PeerCount(), (int)MH_Net_IsStarted());
            return; // stay in the menu; re-check next frame
        }
        lg("; --mp: %d peer(s) present -- entering game", MH_Net_PeerCount());
    }

    g_fired = true;
    lg("; main menu idle -- firing launch action");
    fire_action();
}

// --skip-intro: end the mode-7 LOGO.AVI early. Runs at the top of each llm_intro_frame. On the FIRST
// intro frame (DAT_00644633==0) we do nothing -- let the original run its one-time init (CD audio etc.)
// AND open/start the movie (so no missing-file modal). On a LATER frame (gate>0 = movie playing) we
// call the game's own movie teardown (llm_ui_avi_close_return_to_menu: releases the AVI + runs any
// continuation) and clear the pump pointer, so llm_intro_frame's else-branch advances to boot (mode 1)
// instead of waiting ~10s.
//
// ---- mp:U21: WHICH later frame -- not before the movie's audio thread owns its sound buffer ------
// The movie's first frame ends by starting llm_ui_avi_audio_playback_loop on its own thread
// (llm_game_boot_async_thread_pump). That thread's FIRST act is
//     IDirectSound::CreateSoundBuffer({..., lpwfxFormat = _G_LLM_AVI_PLAYER_CTX.fmt_header})
// and fmt_header is the utils_malloc'd WAVEFORMAT that llm_ui_avi_stream_end utils_free's. Tearing
// down on frame 2 -- which is what this hook did until 2026-09-24 -- lands 2 ms after that thread is
// created: measured on 45/45 boots of rig peer B, CreateSoundBuffer returned 2-22 ms AFTER the teardown
// had freed the format block it was reading. When the Watcom heap had already rewritten the freed
// block's first bytes, wFormatTag stopped reading as PCM, DirectSound believed the cbSize word behind
// it (0xA001) and memcpy'd sizeof(WAVEFORMATEX)+cbSize = 0xA013 bytes off the end of the heap region:
// dsound.dll+0x1f3a3, the U21 crash (22 Application-log records 2026-07-25..09-24, all 1.0-2.75 s
// after process start, all on the peer whose frame 2 comes soonest; EBX=0xA013 in all 5 captured
// contexts). A human pressing Space cannot land inside that window, which is why retail never shows it.
// The fix: tear down only once the thread is past the format read -- its buffer AND its PCM block
// exist (the state a human skip meets), or it has already gone, or it never had anything to read.
// See MH_Launch_IntroSkipReady for the decision table; the frame cap keeps a dead audio path from
// turning --skip-intro into a hang.
constexpr uintptr_t ADDR_AVI_AUDIO_STREAM = 0x0064463fu; // _G_LLM_AVI_PLAYER_CTX+0x04 (active_flag = PAVISTREAM)
constexpr uintptr_t ADDR_DSOUND           = 0x006572b2u; // the game's IDirectSound*
constexpr uintptr_t ADDR_AVI_DSBUF        = 0x006572ceu; // the movie audio IDirectSoundBuffer* (thread-written)
constexpr uintptr_t ADDR_AVI_PCMBUF       = 0x0065f70fu; // the audio thread's 2-chunk PCM block (thread-written)
constexpr uintptr_t ADDR_BOOT_TASK        = 0x0064485bu; // _G_LLM_BOOT_ASYNC_PUMP_FN: task, +0xc = thread id
constexpr unsigned  INTRO_SKIP_CAP_MS     = 2000u;

DWORD g_intro_wait_t0 = 0; // GetTickCount at the first frame we could have skipped on

// The audio thread by id: the pump closes its own handle as soon as the thread exists (seen on every
// rig boot), so the task's handle slot cannot say whether it is still running.
int intro_audio_thread_alive() {
    const DWORD tid = *(volatile const DWORD *)(ADDR_BOOT_TASK + 0xc);
    if (tid == 0) return 0;
    HANDLE h = OpenThread(SYNCHRONIZE, FALSE, tid);
    if (h == nullptr) return 0;
    const int alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// `[debug] u21_repro=N` -- the U21 REPRODUCTION arms, off by default; diagnostic only, never set in
// a shipped ini. Both restore the pre-fix frame-2 teardown.
//   1 = OBSERVE: read the audio format block's wFormatTag and the cbSize word behind it (a
//       PCMWAVEFORMAT is 16 bytes, so DirectSound's cbSize read at +16 lands on whatever the heap put
//       there) before the teardown, and the tag again after the teardown has utils_free'd the block.
//       The crash needs a non-PCM tag; this shows what the free itself writes there.
//   2 = FORCE: right after frame 1 has started the audio thread, suspend it, overwrite ONLY the tag
//       with 0x0020 -- the word utils_free leaves there, per REPRO=1 -- and resume it -- the state a free landing before
//       CreateSoundBuffer's format read produces. If the mechanism is right this turns the ~1% crash
//       into a reliable one at dsound.dll+0x1f3a3 with EBX=0xA013 (18 + that cbSize), and the crash
//       marker's stack sidecar names the audio thread and its CreateSoundBuffer call.
constexpr uintptr_t ADDR_AVI_FMT_PTR = 0x006446d7u; // _G_LLM_AVI_PLAYER_CTX.fmt_header (audio WAVEFORMAT*)

int g_u21_repro = -1; // [debug] u21_repro, read once

int u21_repro() {
    if (g_u21_repro < 0) g_u21_repro = ini_int("debug", "u21_repro", 0);
    return g_u21_repro;
}

// Runs AFTER the original llm_intro_frame (intro_detour_repro only). Frame 1 is the one that starts
// the audio thread, so this is the earliest point the thread id and the format pointer both exist.
bool g_u21_forced = false;
void on_intro_post() {
    if (u21_repro() != 2 || g_u21_forced) return;
    uint8_t    *fmt = *(uint8_t *volatile *)ADDR_AVI_FMT_PTR;
    const DWORD tid = *(volatile const DWORD *)(ADDR_BOOT_TASK + 0xc);
    if (fmt == nullptr || tid == 0) return;
    g_u21_forced              = true;
    HANDLE              h     = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    const DWORD         prev  = h ? SuspendThread(h) : (DWORD)-1;
    const unsigned      tag   = *(volatile uint16_t *)fmt;
    const unsigned      cb    = *(volatile uint16_t *)(fmt + 16);
    const unsigned long dsb   = *(volatile const unsigned long *)ADDR_AVI_DSBUF;
    *(volatile uint16_t *)fmt = 0x0020u; // wFormatTag: the value utils_free leaves there (REPRO=1)
    if (h) {
        ResumeThread(h);
        CloseHandle(h);
    }
    lg("; --skip-intro: U21 REPRO=2 -- audio thread %lu %s, format %08lX tag %04X -> 0020, cbSize word "
       "behind it %04X, dsbuf %s",
       (unsigned long)tid, prev == (DWORD)-1 ? "NOT suspended" : "suspended", (unsigned long)(uintptr_t)fmt,
       tag, cb, dsb ? "ALREADY made (too late)" : "not yet made");
}

void on_intro_tick() {
    if (!g_skip_intro || g_intro_done) return;
    if (*(const uint8_t *)ADDR_INTRO_GATE == 0) return; // first frame: let init + movie-open run
    if (u21_repro() != 0) {
        uint8_t       *fmt = *(uint8_t *volatile *)ADDR_AVI_FMT_PTR;
        const unsigned tag = fmt ? (unsigned)*(volatile uint16_t *)fmt : 0u;
        const unsigned cb  = fmt ? (unsigned)*(volatile uint16_t *)(fmt + 16) : 0u;
        ((void (*)(void))ADDR_MOVIE_TEARDOWN)(); // the pre-fix frame-2 teardown
        *(void **)ADDR_ASYNC_CB = nullptr;
        g_intro_done            = true;
        lg("; --skip-intro: U21 REPRO=%d -- frame-2 teardown; format %08lX tag %04X cbSize-word %04X "
           "before the free, tag %04X after it",
           u21_repro(), (unsigned long)(uintptr_t)fmt, tag, cb, fmt ? (unsigned)*(volatile uint16_t *)fmt : 0u);
        return;
    }
    const DWORD now = GetTickCount();
    if (g_intro_wait_t0 == 0) g_intro_wait_t0 = now ? now : 1;
    const unsigned      waited = (unsigned)(now - g_intro_wait_t0);
    const unsigned long stream = *(volatile const unsigned long *)ADDR_AVI_AUDIO_STREAM;
    const unsigned long ds     = *(volatile const unsigned long *)ADDR_DSOUND;
    const unsigned long dsbuf  = *(volatile const unsigned long *)ADDR_AVI_DSBUF;
    const unsigned long pcm    = *(volatile const unsigned long *)ADDR_AVI_PCMBUF;
    const int           alive  = intro_audio_thread_alive();
    const int           go     = MH_Launch_IntroSkipReady(stream != 0, ds != 0, dsbuf != 0, pcm != 0, alive, waited,
                                                          INTRO_SKIP_CAP_MS);
    if (go == MH_INTRO_SKIP_WAIT) return;    // the movie plays on for another frame
    ((void (*)(void))ADDR_MOVIE_TEARDOWN)(); // llm_ui_avi_close_return_to_menu: teardown + continuation
    *(void **)ADDR_ASYNC_CB = nullptr;       // pump cleared -> else-branch advances to mode 1
    g_intro_done            = true;
    lg("; --skip-intro: LOGO.AVI ended early (movie teardown)");
    lg("; --skip-intro: U21 teardown after %u ms -- %s (audio stream=%d dsound=%d dsbuf=%d pcm=%d "
       "thread=%s)",
       waited, go == MH_INTRO_SKIP_TIMEOUT ? "CAP HIT, audio thread never settled" : "audio thread settled",
       stream != 0, ds != 0, dsbuf != 0, pcm != 0, alive ? "running" : "gone");
}

// ---- run-before-original inline hooks (steal 8-byte prologue; jmp back to target+8) --------------
void *g_tramp       = nullptr;
void *g_intro_tramp = nullptr;

__declspec(naked) void menu_tick_detour() {
    __asm {
        pushad
        pushfd
        call on_menu_tick
        popfd
        popad
        jmp  dword ptr [g_tramp] // stolen prologue + jmp back to llm_ui_menu_state_tick+8
    }
}

__declspec(naked) void intro_detour() {
    __asm {
        pushad
        pushfd
        call on_intro_tick
        popfd
        popad
        jmp  dword ptr [g_intro_tramp] // stolen prologue + jmp back to llm_intro_frame+8
    }
}

// U21 repro=2 only: run the original llm_intro_frame as a SUBROUTINE so on_intro_post sees what frame 1
// just did. The normal hook above stays a plain run-before; this shape is installed only on request.
__declspec(naked) void intro_detour_repro() {
    __asm {
        pushad
        pushfd
        call on_intro_tick
        popfd
        popad
        call dword ptr [g_intro_tramp] // the original, returning here
        pushad
        pushfd
        call on_intro_post
        popfd
        popad
        ret
    }
}

// Ensure the per-frame menu-state-tick hook is installed (idempotent). The launch path installs it only
// for command-line verbs; the manual-menu MP path needs it too (to run the lobby driver), so we install
// it on demand when the manual flow arms. No-op if already hooked or the prologue is unexpected.
void ensure_menu_tick_hook() {
    if (g_tramp) return;
    // U30: this was one of the two FULLY SILENT sites in the tree -- no prologue message, no failure
    // message, no log line of any kind. The manual-menu MP flow depends on this hook to run its lobby
    // driver, so a refusal here presents as the client never auto-entering, with nothing in any log
    // pointing at a hook. Now it says so, and the refusal is enumerated at the end of arming.
    if (!install_trampoline(ADDR_MENU_STATE_TICK, (void *)menu_tick_detour, &g_tramp, 8,
                            entry_claim::exclusive, "the menu-state-tick hook (manual MP lobby driver)"))
        lg("; menu-tick hook NOT armed -- the manual-MP lobby driver will not run (see the "
           "[interlock] line for the reason)");
}

} // namespace

// Menu-triggered client entry (Workstream U Phase 2 -- real-menu join). Open the discovery browser from
// the restored menu button ([menu] mp_join=1). NOT via a detour on llm_mp_create_game_action -- that claim was
// stale (mp_menu.cpp never references it; it goes through llm_lobby_network_setup_screen +
// llm_mp_netsetup_name_confirm). Corrected 2026-08-30 with the rename of that function.
// instead of the command-line --mp-browser verb, so the client browses + selects the host's session by
// hand. Reuses the proven Phase-1 mp_open_browser (net-mode=-1, widget list = browser array, enter the
// session browser -> its rescan polls FUN_0049b905 = our synth discovery). The transport role/host come
// from mh_net.ini [net] role=client / host=<host ip>, exactly as the force-entry client does.
// Arm the manual-menu MP lobby driver (idempotent): flag it + ensure the per-frame menu-tick hook is
// installed so on_menu_tick runs mp_lobby_entry_tick each frame. Called from the client browser-open
// (below) and the host advertise stub (net_seams on_host_advertise) -- whichever role's flow fires.
void            ensure_begin_map_load_hook(); // fwd (defined below) -- U2 host Start-entry hook
extern "C" void MH_MP_ArmManualLobby(void) {
    if (!g_manual_mp) lg("; DIAG manual-mp lobby driver ARMED (menu-tick hook %s)", g_tramp ? "already up" : "installing");
    g_manual_mp = true;
    ensure_menu_tick_hook();
    ensure_begin_map_load_hook(); // U2: intercept the lobby Start button (begin_map_load) for host prep + FLAG_START
}

// True only for a PURE manual-menu MP session (hand-clicked, no launch verb). net_seams gates ALL the
// manual host-side lobby work (peer-table mirror, map broadcast, host auto-entry) on this so a
// force-entry run (VERB_MP_*_LOBBY) keeps its own proven, deterministic path completely untouched.
extern "C" int MH_MP_IsManual(void) { return (g_verb == VERB_NONE && g_manual_mp) ? 1 : 0; }

// U2: the manual host no longer auto-enters at slot-sync. It enters ONLY when the player clicks the lobby
// Start button, which drives llm_lobby_begin_map_load -> our run-before hook (on_begin_map_load below) does
// the proven host prep (symmetric relations + slot snapshot + PCOUNT) and broadcasts FLAG_START so the
// client enters too. So this per-lobby-frame tick now only re-affirms the host's PlayerSide (the entry +
// prep moved onto the actual Start choke point). Client enters via its own mp_lobby_entry_tick + FLAG_START.
namespace {
int g_host_entry_hb = 0;
}
extern "C" void MH_MP_HostEntryTick(void) {
    if (g_verb != VERB_NONE) return; // force-entry uses its verb path (mp_lobby_entry_tick)
    if (g_entry_started) return;
    // Same trap as mp_lobby_entry_tick: the manual HOST enters via the Start button (on_begin_map_load),
    // which never sets g_entry_started -- so without this the tick keeps writing PlayerSide/LOCALIDX
    // into a LIVE session on every lobby-dispatch call. Harmless values today (the host really is 0),
    // but writing net globals mid-match is not something to leave to luck.
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) {
        g_entry_started = true;
        return;
    }
    *(int *)ADDR_NET_PLAYERSIDE() = 0; // host drives player 0 (re-affirm each lobby frame)
    *(int *)ADDR_NET_LOCALIDX     = 0;
    if ((++g_host_entry_hb % 300) == 0)
        lg("; --mp-host(manual): in lobby (occ=%d) -- waiting for the Start button", lobby_slots_occupied());
}

// U2 host entry: run-before llm_lobby_begin_map_load (the lobby Start button -> INT_006502a7 -> here). This
// is the real host-entry choke point, so it's where U2 does the host prep + tells the client to enter. Fires
// for EVERY begin_map_load (host retail-Start, client, force-entry); gated to the PURE MANUAL HOST, one-shot.
// Client's own begin_map_load skips (its prep is in mp_lobby_entry_tick); force-entry skips (verb != NONE).
//
// mp:X2 / X2b -- THE START REFUSAL IS ENFORCED HERE, not by the widget. map_transfer.cpp greys the
// Start widget (its 0x40 DISABLED bit) while a joiner's map is missing or different, but retail
// re-derives that bit every lobby frame, so by the time the input tick hit-tests a click the bit is
// clear again and the click activates Start (measured on the rig 2026-09-23: a click on the
// "refused" Start launched a TCP match with two different maps -- dead-ends G291). The greyed look
// and the status-line notice stay as they are; THIS is what makes the refusal hold: while the host's
// map gate is shut, the Start activation is swallowed before retail's begin_map_load runs.
// g_bml_refuse is the detour's verdict (read after popad, so it cannot ride in EAX).
volatile int g_bml_refuse = 0;

void on_begin_map_load() {
    g_bml_refuse = 0;
    if (g_verb != VERB_NONE || !g_manual_mp) return;  // pure manual menu only (force-entry untouched)
    if (*(int *)ADDR_NET_IS_HOST == 0) return;        // client: nothing to prep/send here
    if (mh::seams::maps::host_refuse_start_click()) { // logs `start CLICK REFUSED` to mh_net.log
        g_bml_refuse = 1;
        return;
    }
    if (MH_Net_PeerCount() < 1) return; // skirmish-vs-AI (no network client): leave the proven path alone
    // ONE-SHOT PER LOBBY, not per process: this used to be a function-static `host_done`, which is
    // the mp:RM1 writer on the host side (see MH_MP_RearmLobbyEntry).
    if (g_host_start_done) return;
    g_host_start_done             = true;
    int occ                       = lobby_slots_occupied();
    *(int *)ADDR_NET_PLAYERSIDE() = 0; // host drives player 0
    *(int *)ADDR_NET_LOCALIDX     = 0;
    mp_zero_sim_clocks();                                              // RM1: enter with a fresh process's clock
    mp_set_lobby_relations_nxn();                                      // N1: full N×N enemy matrix over occupied slots (was slot0<->slot1)
    *(int *)(ADDR_CUR_MAP + MD_PCOUNT) = occ;                          // N1: build exactly the occupied players (incl AI) -- the Players[] size
    *(int *)ADDR_NET_PCOUNT            = lobby_human_slots_occupied(); // N2: sim PEER count = HUMAN slots (not incl AI) -- unblocks finalize's wait
    log_lobby_state(true, "at-entry(start)");
    MH_Seam_SaveHostSlots(); // U0: snapshot; build_players_detour restores it post-clear
    // U28: ship THAT SNAPSHOT with the Start. FLAG_START used to be a bare signal and the client then
    // built Players[] from its OWN _G_LLM_LOBBY_SLOTS -- two independently-maintained copies, so a
    // client slot edit still in flight at this instant survived on the client and was dropped here.
    // Now the host's copy is the only one that decides, and a lost edit is lost SYMMETRICALLY (the
    // player's last click may not take effect, but the two peers still agree, which is the difference
    // between a UX annoyance and a step-1 desync). The bytes are exactly what build_players_detour
    // will restore on this side, so both peers build from one array.
    // [net] start_slots=0 sends the LEGACY BARE SIGNAL instead. This exists so U28's negative arm is
    // reproducible from an ini rather than by reverting code: with it off, each peer builds from its
    // own array again -- the pre-U28 configuration -- and the race_inflight scenario must DESYNC. A
    // fix whose absence cannot be demonstrated is not a proven fix. Default 1; never ship 0.
    const int send_slots = net_ini_int("start_slots", 1);
    MH_Net_SendStart(send_slots ? (const unsigned char *)ADDR_LOBBY_SLOTS : nullptr,
                     send_slots ? SLOT_STRIDE * 8 : 0);
    g_entry_started = true;
    if (send_slots)
        lg("; U2: host Start -> begin_map_load: prep + slot snapshot done, FLAG_START sent with %d B "
           "of authoritative slots (occ=%d)",
           SLOT_STRIDE * 8, occ);
    else
        lg("; U2: host Start -> begin_map_load: prep + slot snapshot done, FLAG_START sent with NO "
           "slot payload ([net] start_slots=0 -- the PRE-U28 configuration, peers may desync) (occ=%d)",
           occ);
}

// Run-before detour on llm_lobby_begin_map_load (steal 8-byte prologue, jmp back to +8).
// mp:X2/X2b: when on_begin_map_load refused the Start, return to the caller WITHOUT running the
// original -- begin_map_load is a void, argument-less callback, so a bare `ret` is its whole contract.
void                  *g_bml_tramp = nullptr;
__declspec(naked) void begin_map_load_detour() {
    __asm {
        pushad
        pushfd
        call on_begin_map_load
        popfd
        popad
        cmp  dword ptr [g_bml_refuse], 0
        jne  refused
        jmp  dword ptr [g_bml_tramp] // stolen 8-byte prologue + jmp back to begin_map_load+8
    refused:
        ret
    }
}

// Install the begin_map_load run-before hook (idempotent). PURE MANUAL only -- a force-entry verb run must
// keep begin_map_load completely untouched (its proven deterministic path), so we don't install for verbs.
void ensure_begin_map_load_hook() {
    if (g_bml_tramp) return;
    if (g_verb != VERB_NONE) return; // force-entry: leave begin_map_load unhooked
    // U30: the other fully-silent site. Same treatment, same reason.
    if (!install_trampoline(ADDR_BEGIN_MAP_LOAD, (void *)begin_map_load_detour, &g_bml_tramp, 8,
                            entry_claim::exclusive, "the begin_map_load run-before hook (manual MP)"))
        lg("; begin_map_load hook NOT armed -- the manual-MP map handoff is unhooked (see the "
           "[interlock] line for the reason)");
}

// (U10 audit, 2026-07-16) MH_MP_OpenBrowser wrapper REMOVED -- orphaned dead code (0 callers anywhere;
// not a PE export). The manual-menu button was re-routed 2026-07-14 to the retail name-confirm transition
// (mp_menu.cpp mp_after_name_confirmed), superseding it. The --mp-browser CLI verb still calls
// mp_open_browser() directly. See the MP seam audit finding J2.

// Menu-triggered lobby sync (Workstream U Phase 2 -- real-menu join). The manual menu path enters the
// REAL lobby but never runs the force-entry's per-frame support, so the host never mirrors our TCP peers
// into the game peer table -> its lobby admin loop never allocates the client's slot -> keepalive-only,
// blank client lobby. net_seams calls this from a per-lobby-frame detour on llm_lobby_host_net_dispatch
// (host only). Reuses the proven mp_sync_host_peer_table.
extern "C" void MH_MP_SyncHostPeerTable(void) { mp_sync_host_peer_table(); }

// U13: reset the host peer-mirror edge tracker when the host LEAVES its lobby, so a re-JOIN into a
// re-created lobby is seen as a fresh JOIN (the peer is re-seated). Called from the host-leave detour.
extern "C" void MH_MP_ResetHostMirror(void) {
    for (int i = 0; i < 8; ++i) g_was_member[i] = false;
}

// Test seam (mh_nettest launchtest): parse an explicit command line with no game state touched.
// Returns verb code (0=none,1=load,2=newgame,3=mp-host,4=mp-join), fills arg_out[arg_cap], and (if
// skip_out != nullptr) writes 1/0 for whether --skip-intro was present.
extern "C" int MH_Launch_ParseCmdline(const char *cmdline, char *arg_out, int arg_cap, int *skip_out) {
    bool skip = false;
    int  v    = (int)parse_into(cmdline ? cmdline : "", arg_out, arg_cap, &skip);
    if (skip_out) *skip_out = skip ? 1 : 0;
    return v;
}

extern "C" int MH_Launch_Init(void) {
    g_verb = parse_cmdline();                           // sets g_verb + g_skip_intro
    if (g_verb == VERB_NONE && !g_skip_intro) return 0; // nothing requested -> inert (ship-safe)
    if (!mh::en_build_ok()) {                           // EN-only: arm nothing on any other image
        lg("; launch NOT armed: not the EN build");
        return 0;
    }

    bool armed = false;

    // --skip-intro (independent of the verb): hook llm_intro_frame so the mode-7 LOGO.AVI ends on its
    // 2nd frame instead of playing ~10s. Frame 1 runs normally (one-time init + movie open, so no
    // missing-file modal); on_intro_tick then tears the movie down via the game's own path.
    if (g_skip_intro) {
        // U30: the prologue branch is gone -- the primitive distinguishes a wrong build from a taken
        // entry, which this site could not.
        bool sok = install_trampoline(ADDR_INTRO_FRAME,
                                      u21_repro() == 2 ? (void *)intro_detour_repro : (void *)intro_detour,
                                      &g_intro_tramp, 8,
                                      entry_claim::exclusive, "the --skip-intro LOGO.AVI hook");
        lg(sok ? "; --skip-intro armed (hooked llm_intro_frame; LOGO.AVI will end early)"
               : "; --skip-intro NOT armed (see the [interlock] line for the reason)");
        armed = armed || sok;
    }

    // A launch verb installs the one-shot menu-idle hook that auto-enters the chosen state.
    if (g_verb != VERB_NONE) {
        bool vok = install_trampoline(ADDR_MENU_STATE_TICK, (void *)menu_tick_detour, &g_tramp, 8,
                                      entry_claim::exclusive,
                                      "the launch-to-state menu-state-tick hook");
        lg(vok ? "; ==== launch-to-state armed: verb=%d arg=\"%s\" skip_intro=%d (hooked llm_ui_menu_state_tick) ===="
               : "; launch NOT armed: menu-state-tick REFUSED (verb=%d arg=\"%s\" skip_intro=%d; see "
                 "the [interlock] line for the reason)",
           g_verb, g_arg, (int)g_skip_intro);
        armed = armed || vok;
    }
    return armed ? 1 : 0;
}

#else // non-x86: inert (mh.exe is 32-bit; only Win32 is ever shipped)

extern "C" int MH_Launch_Init(void) { return 0; }

#endif

// mp:U21 -- when may --skip-intro tear the LOGO.AVI down? Pure, so launchtest proves the table with no
// game running. The hazard is the audio thread's CreateSoundBuffer reading the format block the
// teardown frees (see on_intro_tick). It is safe once that read cannot be in flight:
//   * no audio stream in the movie, or no IDirectSound  -> the thread never dereferences the format;
//   * the thread's sound buffer AND PCM block exist      -> it is past CreateSoundBuffer, in the same
//                                                           state a human's Space press meets;
//   * the thread is gone                                 -> nothing left to race (e.g. the create failed).
// Otherwise wait, but never past cap_ms: a skip that can hang is worse than the race it avoids.
extern "C" int MH_Launch_IntroSkipReady(int has_audio_stream, int has_dsound, int has_dsbuf, int has_pcm,
                                        int thread_alive, unsigned waited_ms, unsigned cap_ms) {
    if (!has_audio_stream || !has_dsound) return MH_INTRO_SKIP_GO;
    if (has_dsbuf && has_pcm) return MH_INTRO_SKIP_GO;
    if (!thread_alive) return MH_INTRO_SKIP_GO;
    if (waited_ms >= cap_ms) return MH_INTRO_SKIP_TIMEOUT;
    return MH_INTRO_SKIP_WAIT;
}
