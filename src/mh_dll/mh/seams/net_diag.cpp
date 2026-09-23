//
// net_diag.cpp -- MP perf/trace INSTRUMENTATION, split out of net_seams.cpp (2026-07 refactor,
// Phase 4). All of this is observe-only and determinism-irrelevant: the
// per-EVENT temporal trace ([trace] temporal=1 -> mh_temporal.log) and the generic function-entry
// tracer ([trace] funcs=VA,... -> mh_trace.log). Kept together because the tracer feeds the temporal
// trace (a traced event id -> temporal_capture). The load-bearing netcode stays in net_seams.cpp;
// this TU reaches it only through the shared spine (net_internal.h): a few init-written globals in,
// temporal_capture/flush + install_trace_hooks + MH_Seam_TraceDump out.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h> // memcpy (tev_ms)
#include <stdlib.h> // strtoul ([trace] funcs= parse)

#include "net_internal.h"
#include "include/mh_log_rotate.h" // SES2: the shared size cap + one-generation ".prev.log" rotation
#include "addr/mh_addrs.gen.h"
#include "state/region_runtime.h" // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                  // where it IS, not where the binary put it
#include "hook/promoted.h"        // C9/G68: a promoted target is not a wrong-build prologue mismatch
#include "addr/mh_export.gen.h"   // entry_utils_abort (its 8-byte arm guard; no Watcom prologue)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::install_trampoline;

namespace {

// D15 exit witness: the binary's two self-driven exits, both of which leave no WER report.
constexpr uintptr_t ADDR_UTILS_ABORT    = mh::addr::utils_abort;        // ends in _exit()
constexpr uintptr_t ADDR_WND_ON_DESTROY = mh::addr::llm_wnd_on_destroy; // WM_DESTROY -> ExitProcess
// The four lockstep clock globals the temporal trace samples (RU/EN-identical data VAs).
constexpr uintptr_t ADDR_SESSION_MODE = mh::addr::_G_LLM_GAME_SESSION_MODE; // byte; 3 = live lockstep
constexpr uintptr_t ADDR_GAME_CLOCK   = mh::addr::_G_LLM_STRAT_GAME_CLOCK;
constexpr uintptr_t ADDR_TOTAL_TIME   = mh::addr::TOTAL_GAME_TIME;
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t ADDR_COMMITTED() {
    return mh::state::live_base(mh::state::RID_STRAT_LOCKSTEP_COMMITTED_HORIZON);
}
constexpr uintptr_t ADDR_LOCAL_HORIZON = mh::addr::_G_LLM_STRAT_LOCKSTEP_HORIZON;
// The lockstep-diagnostic loggers' targets (moved from net_seams.cpp, Phase 4 stage 3).
constexpr uintptr_t ADDR_GAME_MODE     = mh::addr::_G_LLM_GAME_MODE;               // byte; 2=strategic, 3=sync overlay, 6=tactical
constexpr uintptr_t ADDR_PRESENCE_LOST = mh::addr::llm_strat_player_presence_lost; // the ONLY early writer of SESSION=2
constexpr uintptr_t ADDR_G_PLAYERS     = mh::addr::_G_LLM_STRAT_PLAYERS;           // [8] (llm_strat_player_profile, stride 0x740)
constexpr uint32_t  PROFILE_STRIDE     = 0x740;
constexpr uint32_t  OFF_UNITS_ALIVE    = 0x314; // llm_strat_player_profile.units_alive[32]  (+788)
constexpr uint32_t  OFF_BLDGS_ALIVE    = 0x394; // llm_strat_player_profile.buildings_alive[32] (+916)
constexpr uint32_t  ACTIVE_PLANET      = 0x1f;  // MP starts on planet slot 0x1f
// D20: the two things the gate label needs and this file did not have.
// ALIVE is llm_strat_player_profile.status_flags bit1 -- "alive / has presence, cleared on
// elimination" (mh_structs.gen.h); the function tests it as `TEST byte ptr [...],0x2`.
constexpr uint32_t ALIVE_FLAG = 0x2;
// And the gate indexes the per-planet arrays by the LIVE G_PLANET_INDEX, not by ACTIVE_PLANET. The
// two agree in every MP run we have (MP starts on slot 0x1f), but a label derived from the wrong
// planet would be confidently wrong rather than absent, so the label reads the real one and the line
// says when they disagree.
constexpr uintptr_t ADDR_PLANET_INDEX     = mh::addr::G_PLANET_INDEX; // 4 bytes @0x00e58366
constexpr uintptr_t ADDR_SAVEGAME_ERR_DLG = mh::addr::llm_ui_dlg_savegame_io_error;

// ---- per-EVENT temporal trace state (see net_internal.h for the TEV_* ids) -----------------------
struct TemporalEvt {
    long long qpc;
    int       id;
    long      clk, tot, com, loc;
};
constexpr int TEV_MAX = 60000; // ~60 s of lockstep at ~1k events/s; linear (stops when full)
TemporalEvt   g_tev[TEV_MAX];
int           g_tev_n               = 0;     // events captured since the last drain (main-thread only)
int           g_tev_flushed         = 0;     // events written to file since the last drain
bool          g_tev_overflow        = false; // buffer filled between drains -> events WERE dropped
bool          g_tev_overflow_logged = false; // one-shot marker so truncation is never silent again
HANDLE        g_tev_h               = INVALID_HANDLE_VALUE;
// File-size cap ([trace] temporal_max_mb). At the cap the log ROTATES to mh_temporal.prev.log rather
// than stopping: for a post-mortem it is the tail (what happened just before the freeze/desync) that
// matters, and rotation bounds disk at 2x the cap however long the session runs.
long long g_tev_bytes     = 0;     // bytes written to the CURRENT file
long long g_tev_max_bytes = 0;     // 0 = uncapped (temporal_configure sets it)
bool      g_temporal_sp   = false; // [trace] temporal_sp -- also record outside SESSION_MODE 3

inline long tev_ms(uintptr_t a) {
    double d;
    memcpy(&d, (const void *)a, sizeof(double));
    return (long)(d * 1000.0);
}

// ---- function-entry tracer state -----------------------------------------------------------------
constexpr int TRACE_MAX = 16;
struct TraceEnt {
    uintptr_t     addr;
    char          name[28];
    volatile LONG count;
    int           tev_id;
};
TraceEnt      g_trace[TRACE_MAX];
void         *g_ttramp[TRACE_MAX];
char          g_trace_path[MAX_PATH];
unsigned long g_trace_gen = 0; // SES1: per-SESSION -- a traced call belongs to the match it fired in

void trace_log(const char *s) {
    mh_run_path(g_trace_path, MAX_PATH, "%smh_trace.log", &g_trace_gen);
    HANDLE h = CreateFileA(g_trace_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w;
    WriteFile(h, s, lstrlenA(s), &w, nullptr);
    CloseHandle(h);
}

// Called run-before each traced function (idx = pool slot). Logs the FIRST appearance immediately (the
// host-vs-client diff signal) and keeps a call count (dumped by MH_Seam_TraceDump).
void __cdecl trace_hit(int idx) {
    if ((unsigned)idx >= (unsigned)g_trace_n) return;
    if (g_trace[idx].tev_id >= 0) temporal_capture(g_trace[idx].tev_id); // per-EVENT temporal trace
    LONG n = InterlockedIncrement(&g_trace[idx].count);
    if (n == 1) {
        char b[160];
        wsprintfA(b, "; TRACE first [%d] %s (%08X) tid=%lu\n", idx, g_trace[idx].name,
                  (unsigned)g_trace[idx].addr, GetCurrentThreadId());
        trace_log(b);
    }
}

// clang-format off
// (guarded: a multi-line __asm continuation macro -- clang-format would join the instructions
// onto one line; legal MSVC, since __asm is a statement separator, but unreadable.)
#define TRACE_THUNK(N) __declspec(naked) void trace_thunk_##N() { \
    __asm pushad \
    __asm pushfd \
    __asm push N \
    __asm call trace_hit \
    __asm add esp, 4 \
    __asm popfd \
    __asm popad \
    __asm jmp dword ptr [g_ttramp + (N)*4] }
TRACE_THUNK(0)  TRACE_THUNK(1)  TRACE_THUNK(2)  TRACE_THUNK(3)
TRACE_THUNK(4)  TRACE_THUNK(5)  TRACE_THUNK(6)  TRACE_THUNK(7)
TRACE_THUNK(8)  TRACE_THUNK(9)  TRACE_THUNK(10) TRACE_THUNK(11)
TRACE_THUNK(12) TRACE_THUNK(13) TRACE_THUNK(14) TRACE_THUNK(15)
void *g_trace_thunks[TRACE_MAX] = {
    trace_thunk_0,  trace_thunk_1,  trace_thunk_2,  trace_thunk_3,
    trace_thunk_4,  trace_thunk_5,  trace_thunk_6,  trace_thunk_7,
    trace_thunk_8,  trace_thunk_9,  trace_thunk_10, trace_thunk_11,
    trace_thunk_12, trace_thunk_13, trace_thunk_14, trace_thunk_15,
};
// clang-format on

// Readable labels for the VAs we commonly trace (lobby/UI). Fallback = the hex VA.
const char *trace_known_name(uintptr_t va) {
    switch (va) {
        case mh::addr::llm_lobby_screen_open: return "lobby_screen_open";
        case mh::addr::llm_lobby_host_net_dispatch: return "lobby_host_net_dispatch";
        case mh::addr::llm_lobby_begin_map_load: return "lobby_begin_map_load";
        case mh::addr::llm_lobby_build_players_from_slots: return "lobby_build_players_from_slots";
        case mh::addr::llm_lobby_host_start_game: return "lobby_host_start_game";
        case mh::addr::llm_lobby_host_new_game_start: return "lobby_host_new_game_start";
        case mh::addr::llm_lobby_join_head: return "lobby_join_head";
        case mh::addr::llm_lobby_join_handler: return "lobby_join_handler";
        default: return nullptr;
    }
}

// Map a traced VA to a temporal-trace event id (only the perf-latency inventory; else -1 = count only).
int trace_tev_id(uintptr_t va) {
    switch (va) {
        case mh::addr::llm_strat_frame: return TEV_FRAME;
        case mh::addr::llm_net_lockstep_pump: return TEV_PUMP;
        case mh::addr::llm_net_lockstep_commit_horizon: return TEV_COMMIT;
        case mh::addr::llm_strat_sim_tick: return TEV_SIMTICK;
        case mh::addr::llm_net_send_lockstep_extend: return TEV_SENDEXT;
        default: return -1;
    }
}

// ---- lockstep-diagnostic loggers (pure log, moved from net_seams.cpp Phase 4 stage 3) ------------

// Phase 2c thread-1: presence_lost entry logger. This is the ONLY function that writes SESSION_MODE=2
// early; the client leaves lockstep (3->2) within ~2 frames, so log every call -- who called (return
// addr), which player (param_1, __mhfastocall EAX), mode arg (param_2, EDX), and BOTH players' presence
// on the active planet -- to pin the exact trigger. Gated by lockstep_log so no new ini knob. Rare
// (only on elimination), so seam_log (mh_net.log) is fine.
void      *g_pl_tramp = nullptr;
inline int prof_i(uintptr_t off, uint32_t player) {
    return *(const int *)(ADDR_G_PLAYERS + (uintptr_t)player * PROFILE_STRIDE + off + ACTIVE_PLANET * 4);
}
// D20: WHICH WAY THE GATE WENT -- the whole point of the item.
//
// llm_strat_player_presence_lost opens with an early return that rejects most calls a few
// instructions in (decompiled at 0x00498089, lines 35-39):
//
//   if ((PLAYERS[player].status_flags & ALIVE) && mode == 0 &&
//       (0 < PLAYERS[player].buildings_alive[G_PLANET_INDEX] ||
//        0 < PLAYERS[player].units_alive[G_PLANET_INDEX]))
//       return PLAYERS[player].units_alive[G_PLANET_INDEX];   // NOT eliminated, before any effect
//
// This is an ENTRY trampoline, so it fires before that test and used to log a rejected call in
// exactly the same shape as a real elimination. In the 2026-08-28 external run FIVE of the six
// observed events were no-ops and read like eliminations. Evaluating the identical predicate here --
// pure reads, no writes, the sim is untouched -- lets the line say which branch the call is about
// to take.
//
// MODE IS PART OF THE GATE and the earlier prose kept dropping it: a FORCED removal (mode != 0, from
// the MP drop handlers llm_net_player_remove / _timeout and the lockstep dispatch) takes the
// elimination path REGARDLESS of the unit/building counts. A reader who checks only ua/ba against the
// label would call that a mislabel; it is not.
//
// The counts printed were also only ever players 0 and 1, which cannot check the label for a call
// about player 2+. The called player's own triple is now printed as its own field.
inline unsigned prof_sf(uint32_t player) {
    return *(const uint32_t *)(ADDR_G_PLAYERS + (uintptr_t)player * PROFILE_STRIDE);
}
// The same read as prof_i but on the LIVE planet index, which is what the gate itself uses.
inline int prof_i_live(uintptr_t off, uint32_t player, uint32_t planet) {
    return *(const int *)(ADDR_G_PLAYERS + (uintptr_t)player * PROFILE_STRIDE + off + planet * 4);
}
void on_presence_lost(unsigned player, unsigned mode, unsigned caller) {
    if (!g_ls_log) return;
    unsigned sf0 = prof_sf(0);
    unsigned sf1 = prof_sf(1);
    // Bounded before indexing: `player` arrives in EAX from game code and this is a diagnostic, so it
    // must not be the thing that faults. Out of range -> report it rather than guess a branch.
    const uint32_t planet = *(const uint32_t *)ADDR_PLANET_INDEX;
    const bool     ok     = player < 8u && planet < 32u;
    const unsigned sfp    = ok ? prof_sf(player) : 0u;
    const int      uap    = ok ? prof_i_live(OFF_UNITS_ALIVE, player, planet) : -1;
    const int      bap    = ok ? prof_i_live(OFF_BLDGS_ALIVE, player, planet) : -1;
    const char    *gate =
        !ok ? "UNREADABLE"
               : (((sfp & ALIVE_FLAG) != 0 && mode == 0 && (bap > 0 || uap > 0)) ? "alive" : "eliminated");
    char b[416];
    wsprintfA(b, "; presence_lost player=%u mode=%u gate=%s self(sf=0x%x ua=%d ba=%d planet=%u%s) "
                 "caller=0x%08x sess=%d gclk=%ld | sf0=0x%x ua0=%d ba0=%d  sf1=0x%x ua1=%d ba1=%d\n",
              player, mode, gate, sfp, uap, bap, planet,
              (planet == ACTIVE_PLANET) ? "" : " !=ACTIVE_PLANET, the sf0/sf1 columns below are on 0x1f",
              caller, (int)*(const uint8_t *)ADDR_SESSION_MODE, ms_of(ADDR_GAME_CLOCK),
              sf0, prof_i(OFF_UNITS_ALIVE, 0), prof_i(OFF_BLDGS_ALIVE, 0),
              sf1, prof_i(OFF_UNITS_ALIVE, 1), prof_i(OFF_BLDGS_ALIVE, 1));
    seam_log(b);
}
__declspec(naked) void presence_lost_detour() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp+0x24] // return address of the caller (pre-pushad [esp])
        push eax
        mov  eax, [esp+0x1c] // param_2 (EDX, saved by pushad; +4 for the one push above)
        push eax
        mov  eax, [esp+0x28] // param_1 (EAX, saved by pushad; +8 for the two pushes above)
        push eax
        call on_presence_lost // cdecl(param_1, param_2, caller)
        add  esp, 12
        popfd
        popad
        jmp  dword ptr [g_pl_tramp] // stolen 8-byte prologue + jmp back to presence_lost+8
    }
}

// Phase 2c thread-1 residual: the modal error dialog llm_ui_dlg_savegame_io_error (0x004bd58e) has 16
// callers (each a distinct call site / text id) -- map-load errors, save/load errors, AND the case-'\f'
// self-removal ("you were removed", 0x2c1, call site 0x004bf974). Log the CALLER return address to pin
// exactly which one fires on the client at entry -> then NOP just that site. Gated by lockstep_log.
void *g_se_tramp = nullptr;
void  on_savegame_err(unsigned caller) {
    if (!g_ls_log) return;
    char b[112];
    wsprintfA(b, "; DLG savegame_io_error caller=0x%08x sess=%d gclk=%ld\n",
               caller, (int)*(const uint8_t *)ADDR_SESSION_MODE, ms_of(ADDR_GAME_CLOCK));
    seam_log(b);
}
__declspec(naked) void savegame_err_detour() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp+0x24] // caller return address (pre-pushad [esp])
        push eax
        call on_savegame_err
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_se_tramp]
    }
}

// GAME_MODE transition logger (diagnostic): a DR0 hardware write-breakpoint on _G_LLM_GAME_MODE +
// a vectored exception handler that logs the writing instruction (EIP) + new value + clock every time
// the game changes the frame mode. Finds the pump-path mode-flip that enters the 2 s freeze without
// reading the 774-line dispatch. Observe-only -> determinism-safe. Gated by mh_net.ini log_gamemode.
int    g_gm_log = 0; // 1 = arm the GAME_MODE write logger
HANDLE g_gm_h   = INVALID_HANDLE_VALUE;
char   g_gm_path[MAX_PATH];
PVOID  g_gm_veh = nullptr;

// Vectored handler for the DR0 write-breakpoint on _G_LLM_GAME_MODE. A data breakpoint traps AFTER the
// store, so *ADDR_GAME_MODE already holds the new value and ContextRecord->Eip is the instruction just
// PAST the writer (look up Eip-<insn> in Ghidra). Logs only while SESSION_MODE==3 (the live lockstep sim)
// to skip boot/menu churn. Must not throw; keeps other exceptions flowing (CONTINUE_SEARCH).
LONG CALLBACK gamemode_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if ((c->Dr6 & 0x1) == 0) return EXCEPTION_CONTINUE_SEARCH; // not our DR0 hit
    c->Dr6 = 0;                                                // acknowledge
    if (g_gm_h != INVALID_HANDLE_VALUE && *(volatile uint8_t *)ADDR_SESSION_MODE == 3) {
        char  line[128];
        int   n = wsprintfA(line, "%lu mode=%d eip=%08X clk_ms=%ld\n",
                            GetTickCount(), (int)*(volatile uint8_t *)ADDR_GAME_MODE,
                            (unsigned)c->Eip, ms_of(ADDR_GAME_CLOCK));
        DWORD w;
        WriteFile(g_gm_h, line, n, &w, nullptr);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Arm the logger from a helper thread: register the VEH, then suspend the main (frame) thread and set
// DR0 = &_G_LLM_GAME_MODE with DR7 = slot-0 1-byte write breakpoint (0x00010001). Runs off the main
// thread so we can SetThreadContext on it. One-shot.
DWORD WINAPI gamemode_arm_thread(LPVOID) {
    g_gm_h = CreateFileA(g_gm_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_gm_h == INVALID_HANDLE_VALUE) {
        seam_log("; gamemode logger: cannot open log\n");
        return 0;
    }
    const char *hdr = "# wall_ms mode eip clk_ms  (writer instr is just BEFORE eip)\n";
    DWORD       w;
    WriteFile(g_gm_h, hdr, lstrlenA(hdr), &w, nullptr);
    g_gm_veh  = AddVectoredExceptionHandler(1, gamemode_veh);
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_main_tid);
    if (!th) {
        seam_log("; gamemode logger FAILED (OpenThread)\n");
        return 0;
    }
    SuspendThread(th);
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &c)) {
        c.Dr0          = ADDR_GAME_MODE;
        c.Dr6          = 0;
        c.Dr7          = 0x00010001; // L0 enable (bit0) + slot0 R/W=01 write, LEN=00 1 byte (bit16)
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(th, &c);
    }
    ResumeThread(th);
    CloseHandle(th);
    seam_log("; gamemode logger armed (DR0 write-bp on _G_LLM_GAME_MODE -> mh_gamemode.log)\n");
    return 0;
}

} // namespace

// ==== temporal trace (public: called from the present/time_tick detours + harness sim_step) =======
void temporal_capture(int id) {
    if (!g_temporal) return;
    // MODE GATE. The recorder was written for the live lockstep sim, and its com_ms/loc_ms columns are
    // lockstep horizons that sit idle anywhere else. But "not useful" became "impossible": in
    // SESSION_MODE 2 it armed, logged `present hook armed: ... temporal=1`, wrote its header and then
    // emitted nothing -- so an empty trace and a quiet one looked identical, and a single-player
    // investigation could not use the one instrument built for exactly that question. [trace]
    // temporal_sp=1 opens it to SP; default 0, so no archived run's behaviour changes.
    // Expect only 4 of the 8 event ids in SP: PRESENT, TIMETICK, SIMTICK, SIMSTEP. FRAME/PUMP/COMMIT/
    // SENDEXT come from install_trace_hooks ([trace] funcs=) or are lockstep-only by construction, so a
    // zero there is "not applicable", not a finding.
    // VOLUME: SP lanes reach several thousand strategic fps, so this is ~400 KB/s -- set
    // temporal_max_mb explicitly for a long run or it rotates and leaves only the tail.
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3 && !g_temporal_sp) return;
    if (GetCurrentThreadId() != g_main_tid) return; // main-thread only -> no lock needed
    if (g_tev_n >= TEV_MAX) {
        g_tev_overflow = true;
        return;
    } // NEVER silent: flagged + logged once
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    TemporalEvt *e = &g_tev[g_tev_n];
    e->qpc         = g_qpc_freq.QuadPart ? (t.QuadPart * 1000000LL) / g_qpc_freq.QuadPart : t.QuadPart;
    e->id          = id;
    e->clk         = tev_ms(ADDR_GAME_CLOCK);
    e->tot         = tev_ms(ADDR_TOTAL_TIME);
    e->com         = tev_ms(ADDR_COMMITTED());
    e->loc         = tev_ms(ADDR_LOCAL_HORIZON);
    g_tev_n++;
}

unsigned long g_tev_gen = 0; // SES1: the run-directory generation g_tev_path was composed for

void temporal_flush() {
    if (!g_temporal) return;
    // SES1: a session boundary re-points the path; the OPEN handle must go with it. Closing here and
    // letting the block below reopen also re-writes the column header into the new file, which is
    // what makes a session directory's mh_temporal.log readable on its own. g_tev_bytes restarts, so
    // the rotation cap applies per session rather than carrying a previous match's size across.
    if (mh_run_path(g_tev_path, MAX_PATH, "%smh_temporal.log", &g_tev_gen) &&
        g_tev_h != INVALID_HANDLE_VALUE) {
        CloseHandle(g_tev_h);
        g_tev_h     = INVALID_HANDLE_VALUE;
        g_tev_bytes = 0;
    }
    if (g_tev_h == INVALID_HANDLE_VALUE) {
        g_tev_h = CreateFileA(g_tev_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_tev_h == INVALID_HANDLE_VALUE) {
            g_temporal = false;
            return;
        }
        const char *hdr = "# qpc_us id clk_ms tot_ms com_ms loc_ms | id 0=frame 1=pump 2=commit "
                          "3=time_tick 4=sim_tick 5=sim_step 6=send_ext 7=present\n";
        DWORD       w;
        WriteFile(g_tev_h, hdr, lstrlenA(hdr), &w, nullptr);
        g_tev_bytes = lstrlenA(hdr);
    }
    // Rotate at the cap: close, replace mh_temporal.prev.log, reopen empty (the header is rewritten
    // by the block above on the next flush). Keeps the most recent <=2x cap of trace, bounded.
    if (g_tev_max_bytes > 0 && g_tev_bytes >= g_tev_max_bytes) {
        CloseHandle(g_tev_h);
        g_tev_h = INVALID_HANDLE_VALUE;
        // SES2: the ".log" -> ".prev.log" derivation is mh_log_rotate.h's, shared with mh_net.log's
        // cap. It used to be four lines here and was the only copy; a second stream rotating meant a
        // second spelling of a name the tools glob, so it moved rather than being duplicated.
        mh_log_rotate(g_tev_path);
        g_tev_bytes = 0;
        return; // this batch lands in the fresh file on the next flush
    }
    if (g_tev_overflow && !g_tev_overflow_logged) { // surface dropped events IN the log
        g_tev_overflow_logged = true;
        const char *w1        = "# WARNING: temporal buffer hit TEV_MAX between drains -- EVENTS WERE DROPPED\n";
        DWORD       w;
        WriteFile(g_tev_h, w1, lstrlenA(w1), &w, nullptr);
    }
    char buf[4096];
    int  off = 0;
    while (g_tev_flushed < g_tev_n) {
        if (off > (int)sizeof(buf) - 96) {
            DWORD w;
            WriteFile(g_tev_h, buf, off, &w, nullptr);
            g_tev_bytes += off;
            off = 0;
        }
        TemporalEvt *e = &g_tev[g_tev_flushed++];
        off += wsprintfA(buf + off, "%I64d %d %ld %ld %ld %ld\n", e->qpc, e->id, e->clk, e->tot, e->com, e->loc);
    }
    if (off) {
        DWORD w;
        WriteFile(g_tev_h, buf, off, &w, nullptr);
        g_tev_bytes += off;
    }
    // Fully drained -> recycle the staging buffer. Capture and flush are BOTH main-thread-only
    // (temporal_capture bails on a foreign tid; flush runs from on_present), so no event can land
    // between the drain loop and this reset. Without it the indices advanced monotonically and the
    // capture died at TEV_MAX even though every row was already safely on disk -- at ~550 fps that
    // was ~24 s of a 90 s run (2026-07-20: the step=101 trace stopped at sim_step 225 of 800).
    if (g_tev_flushed == g_tev_n) {
        g_tev_n       = 0;
        g_tev_flushed = 0;
    }
}

// Cross-module entry (harness on_sim_step calls this -- sim_step is harness-hooked, not tracer-hookable).
extern "C" void MH_Temporal_Event(int id) { temporal_capture(id); }

// Per-EVENT temporal trace config ([trace] temporal=1). The recorder needs the shared present hook
// (TEV_PRESENT + the flush), which net_lockstep.cpp owns -- its present-hook install calls this to
// read the flag + init QPC frequency and the mh_temporal.log path, and sets g_temporal only if the
// hook actually arms. Verbatim from the pre-split MH_Seam_Init block.
int temporal_configure() {
    // Ship default OFF (mp:SES5 decision 5, 2026-09-21; was ON 2026-07-25): this is the per-EVENT
    // trace, a profiling instrument -- no player-report question has ever needed it, and it was
    // 125 MB of a measured 43-minute match. Rig/lane inis set it back to 1 explicitly. When it IS
    // on: the staging buffer is drained + recycled on every present, so run LENGTH is not a limit
    // (only a >TEV_MAX burst between two presents drops events, and that is logged). What IS
    // unbounded is the FILE -- ~40 KB/s, i.e. >100 MB/hour -- so the writer rotates at
    // temporal_max_mb (below) and a session can never fill a disk.
    int temporal  = GetPrivateProfileIntA("trace", "temporal", SHIP_TEMPORAL_LEVEL, g_ini);
    g_temporal_sp = GetPrivateProfileIntA("trace", "temporal_sp", 0, g_ini) != 0;
    // Report it, because "armed" and "recording" were indistinguishable before: with the mode gate
    // shut, the trace logged `temporal=1`, created the file and wrote its header, and emitted nothing.
    if (temporal && g_temporal_sp) seam_log("; [trace] temporal_sp=1 -- recording OUTSIDE SESSION_MODE 3\n");
    // SES2: same MB->bytes derivation (and the same "0 = uncapped") as mh_net.log's [net] log_max_mb.
    g_tev_max_bytes = mh_log_cap_bytes(g_ini, "trace", "temporal_max_mb", 64);
    if (temporal) {
        QueryPerformanceFrequency(&g_qpc_freq);
        // The path itself is (re)composed by temporal_flush's mh_run_path -- SES1 made it per session.
    }
    return temporal;
}

// ==== lockstep-diagnostic loggers (installed from net_seams' MH_Seam_Init, g_ls_log-gated) ========
// U30: both of these were `<byte compare> && install_trampoline(...)`, and the `&&` short-circuits
// past every guard the primitive has. The compare now lives inside it, so the else-branch no longer
// has to name a cause it could not have known.
void install_presence_lost_logger() {
    if (install_trampoline(ADDR_PRESENCE_LOST, (void *)presence_lost_detour, &g_pl_tramp, 8,
                           mh::hook::entry_claim::exclusive, "the presence_lost entry logger"))
        seam_log("; presence_lost logger armed\n");
    else
        seam_log("; presence_lost logger NOT armed -- see the [interlock] line for the reason\n");
}

void install_savegame_err_logger() {
    if (install_trampoline(ADDR_SAVEGAME_ERR_DLG, (void *)savegame_err_detour, &g_se_tramp, 8,
                           mh::hook::entry_claim::exclusive, "the savegame_io_error dlg logger"))
        seam_log("; savegame_io_error dlg logger armed\n");
    else
        seam_log("; savegame_io_error dlg logger NOT armed -- see the [interlock] line for the reason\n");
}

// ==== D15 cause #2: THE EXIT WITNESS =============================================================
//
// Four multi-peer HOST games died at the same instant, ~12 k frames in, at four different script
// positions -- with no WER report, no Application-log event and not a line in any of the game's own
// logs (D15). "They simply stop" is not a diagnosis, and it is not one because
// every exit this binary can take on its own is SILENT, and nothing was watching the door:
//
//   * utils_abort ends in _exit(). No exception is raised, so WER never sees it, and all 38 call
//     sites -- mission-parse errors, gfx/pathfinder init failures, the strategic assert in
//     llm_strat_tile_assert_unit_stack_coords -- vanish identically.
//   * llm_wnd_on_destroy (WM_DESTROY) tears the engine down and calls ExitProcess. A CLEAN exit,
//     equally invisible, and the one that can fire mid-game without anybody typing anything.
//
// Between them they are every self-driven exit in the binary. This is not a fix; it is the
// instrument that lets the next occurrence name itself. Two run-before trampolines, one line each,
// written BEFORE the original runs -- which is the whole point, because after it there is no
// process. seam_log opens/appends/closes per call, so unlike the harness log there is no buffer
// left to lose on the way out.
//
// WITNESSED AT llm_wnd_on_destroy, NOT AT llm_fatal_cleanup, and both reasons were measured rather
// than reasoned. fatal_cleanup looks like the better hook -- every fatal path calls it AND the
// window path does -- but (a) it is also called where the process then keeps running
// (llm_gfx_display_init), so it would report exits that are not happening, and (b) it is an
// [effects] gate target, armed by default, so a second detour on that entry simply loses: the first
// arm attempt reported "NOT armed on llm_fatal_cleanup (unexpected prologue)" on a good build,
// because the prologue it found was the effects layer's own jmp. The C9 interlock names collisions
// with a PROMOTION; this was a detour-vs-detour one, which is why the message could only say what it
// did not recognise. Hence the byte dump below -- an "unexpected prologue" that does not say what it
// actually saw is the same dead end wearing a different hat.
//
// UNCONDITIONAL, deliberately -- not behind g_ls_log like the loggers above. An exit happens at most
// once per run and costs one line; gating it would mean the run that finally reproduces this is the
// one run that was not recording. The ARM banner is unconditional for the same reason: when a death
// leaves no line we have to be able to tell "the witness saw nothing" from "the witness was never
// armed", and in an empty log those read the same.
//
// Note the CALLER return address in each line. utils_abort's 38 sites are the difference between a
// mission-load parse failure and a strategic assert -- one hex number picks the branch out of the
// whole binary.
void *g_ab_tramp = nullptr;
void *g_wd_tramp = nullptr;

// utils_abort does NOT open with the Watcom frame prologue -- it is a 24-byte leaf that starts
// `push edx; mov edx,eax; call [..]`, so its instruction boundaries are 1, 3 and 9, and the usual
// 8-byte steal would cut the call in half. Guarded against the generated 8-byte entry signature
// instead (the same bytes install_export arms on) and stealing 9. The stolen `call dword ptr [abs]`
// carries no rel operand, so it relocates into the trampoline unchanged.
constexpr int STOLEN_ABORT = 9;

void on_utils_abort(unsigned status, unsigned caller) {
    char b[208];
    wsprintfA(b,
              "; EXIT utils_abort(status=%u) caller=0x%08x mode=%d sess=%d gclk=%ld -- the process "
              "is about to _exit(), which raises nothing and leaves no WER report\n",
              status, caller, (int)*(const uint8_t *)ADDR_GAME_MODE,
              (int)*(const uint8_t *)ADDR_SESSION_MODE, ms_of(ADDR_GAME_CLOCK));
    seam_log(b);
}
__declspec(naked) void utils_abort_detour() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp+0x24] // caller return address (pre-pushad [esp])
        push eax
        mov  eax, [esp+0x24] // status (__watcall EAX, saved by pushad; +4 for the one push above)
        push eax
        call on_utils_abort // cdecl(status, caller)
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_ab_tramp] // stolen 9-byte prologue + jmp back to utils_abort+9
    }
}

void on_wnd_destroy(unsigned caller) {
    char b[208];
    wsprintfA(b,
              "; EXIT llm_wnd_on_destroy caller=0x%08x mode=%d sess=%d gclk=%ld -- WM_DESTROY: "
              "engine teardown then ExitProcess, no WER report\n",
              caller, (int)*(const uint8_t *)ADDR_GAME_MODE,
              (int)*(const uint8_t *)ADDR_SESSION_MODE, ms_of(ADDR_GAME_CLOCK));
    seam_log(b);
}
__declspec(naked) void wnd_destroy_detour() {
    __asm {
        pushad
        pushfd
        mov  eax, [esp+0x24] // caller return address (pre-pushad [esp])
        push eax
        call on_wnd_destroy // cdecl(caller)
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_wd_tramp] // stolen 8-byte prologue + jmp back to llm_wnd_on_destroy+8
    }
}

// SAY WHAT WAS THERE. "unexpected prologue" was the whole of the old message, and it is the reason
// the first arm attempt on llm_fatal_cleanup read as a wrong build when it was a detour collision --
// the bytes name the culprit (E9 = somebody's jmp) in the line itself. KEPT after U30 rather than
// folded into the primitive's message: the primitive prints the first FOUR entry bytes, and the
// difference between four and eight is the difference between "not what we expected" and being able
// to see the whole displaced prologue. WHY the entry was unavailable is now the primitive's line.
void witness_not_armed(uintptr_t target, const char *what, const char *consequence) {
    char           b[288];
    const uint8_t *e = (const uint8_t *)target;
    wsprintfA(b,
              "; exit witness NOT armed on %s -- entry %02X%02X%02X%02X%02X%02X%02X%02X is not what "
              "we generated against (E9 = another hook got here first). The WHICH-and-why is on the "
              "[interlock] line for this address. %s\n",
              what, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], consequence);
    seam_log(b);
}

// U30. Both witness sites were `<byte compare> && install_trampoline(...)`, which is the pattern this
// item exists to delete -- the `&&` short-circuits, so the ownership question is never asked and a
// taken entry reads as a wrong build (G68, and this function's own message is a monument to it).
//
// utils_abort cannot simply hand its compare to the primitive: its guard is the exact EIGHT entry
// bytes it was generated against (it steals NINE, so the generic 4-byte Watcom prologue would be both
// wrong and too weak), and the primitive compares four. So the ORDER moves instead: ask the registry
// first, without writing; only then the caller's own stronger byte guard; only then the write.
// Is this entry available AND does it hold what we compiled against? DECISION ONLY -- the install
// stays at the two call sites, with their literal addresses, so tools/lint_dll_patches.py can still
// resolve every detour target to a function. A helper that took the address and did the write would
// make both sites UNRESOLVED, i.e. invisible to the very static check that guards this class.
//
// `expect8 == 0` means "no caller-side guard -- let the primitive apply its default prologue
// expectation instead".
bool witness_entry_ok(uintptr_t target, uint64_t expect8, const char *who) {
    const uint32_t                expect_prologue = expect8 ? 0u : mh::hook::WATCOM_PROLOGUE;
    const mh::hook::refuse_reason why =
        mh::hook::detour_refusal(target, mh::hook::entry_claim::exclusive, expect_prologue);
    if (why != mh::hook::refuse_reason::none) {
        mh::hook::note_entry_refusal(target, who, why);
        return false;
    }
    if (expect8 && *(const uint64_t *)target != expect8) {
        // The caller-side guard, and it has to be FILED as well as printed -- an exit witness that is
        // not armed is precisely the run that will die without a trace.
        mh::hook::note_entry_refusal(target, who, mh::hook::refuse_reason::prologue);
        return false;
    }
    return true;
}

void install_exit_witness() {
    // utils_abort: expect_prologue 0 at the install because the guard above is the exact EIGHT entry
    // bytes it was generated against -- it steals NINE, so the generic 4-byte Watcom prologue would
    // be both the wrong constant and too weak.
    static const char *const WHO_ABORT = "the D15 exit witness (utils_abort)";
    bool                     ok        = witness_entry_ok(ADDR_UTILS_ABORT, mh::exp::entry_utils_abort, WHO_ABORT);
    if (ok)
        ok = install_trampoline(ADDR_UTILS_ABORT, (void *)utils_abort_detour, &g_ab_tramp, STOLEN_ABORT,
                                mh::hook::entry_claim::exclusive, WHO_ABORT, 0);
    if (ok) seam_log("; exit witness armed on utils_abort\n");
    else
        witness_not_armed(ADDR_UTILS_ABORT, "utils_abort",
                          "A silent _exit in this run would leave no trace");

    static const char *const WHO_DESTROY = "the D15 exit witness (llm_wnd_on_destroy)";
    ok                                   = witness_entry_ok(ADDR_WND_ON_DESTROY, 0, WHO_DESTROY);
    if (ok)
        ok = install_trampoline(ADDR_WND_ON_DESTROY, (void *)wnd_destroy_detour, &g_wd_tramp, 8,
                                mh::hook::entry_claim::exclusive, WHO_DESTROY);
    if (ok) seam_log("; exit witness armed on llm_wnd_on_destroy\n");
    else
        witness_not_armed(ADDR_WND_ON_DESTROY, "llm_wnd_on_destroy",
                          "A silent ExitProcess in this run would leave no trace");
}

// GAME_MODE DR0 logger config: read the ini gate + build the mh_gamemode.log path (from MH_Seam_Init;
// g_main_tid is captured there). The arming itself is lazy -- see gm_logger_lazy_arm below.
void gm_logger_configure() {
    g_gm_log = GetPrivateProfileIntA("net", "log_gamemode", 0, g_ini);
    if (g_gm_log) {
        // SES1: PROCESS-scoped, and this is the one stream where that is a safety call rather than a
        // classification. Its writer is a VECTORED EXCEPTION HANDLER on a DR0 data breakpoint -- it
        // runs inside the trap, on the frame thread, with a handle opened once at arm. Swapping that
        // handle at a session boundary would put a CreateFile/CloseHandle pair inside an exception
        // path for a debug knob that ships OFF (`[net] log_gamemode=0`). The lines carry clk_ms, and
        // the session's own logs carry the same clock, so correlating across the two costs nothing.
        unsigned long gen = 0;
        mh_proc_path(g_gm_path, MAX_PATH, "%smh_gamemode.log", &gen);
    }
}

// Arm from a helper thread (needs to SetThreadContext on the main thread). Called from net_seams'
// lazy_start (off loader-lock). One-shot; no-op unless [net] log_gamemode=1 and the tid is captured.
void gm_logger_lazy_arm() {
    static bool gm_armed = false;
    if (g_gm_log && g_main_tid && !gm_armed) {
        gm_armed = true;
        CloseHandle(CreateThread(nullptr, 0, gamemode_arm_thread, nullptr, 0, nullptr));
    }
}

// ==== function-entry tracer (public: install from MH_Seam_Init, dump from the lobby dispatch) ======
// Parse [trace] funcs=0xVA,0xVA,... and install a run-before hook on each (up to 16). PROLOGUE-guarded.
void install_trace_hooks() {
    char list[512];
    // TL-HARN4 EXCEPTION: not routed through mh::config::read_ini_string. `;` is one of this key's
    // OWN token delimiters below (", \t;") alongside comma/space/tab, so a value is allowed to use
    // it as an in-list separator on purpose -- stripping "first `;` onward" would silently truncate
    // a `funcs=0x401000;0x402000` list rather than clean a comment off it. The documented (example
    // ini) form is comma-separated, but the parser already tolerates `;` as an equivalent separator
    // and this is the one key in the audited roster where that is deliberate, not an oversight.
    GetPrivateProfileStringA("trace", "funcs", "", list, sizeof(list), g_ini);
    if (!list[0]) return;
    trace_log("; ==== function-entry trace armed ====\n"); // composes g_trace_path (SES1: per session)
    char *ctx = nullptr;
    // [trace] funcs= VAs are authored EN-canonical (like every ADDR_* in this stack since the EN-only
    // refactor). The tev-id / known-name lookups key on the same EN VA.
    for (char *tok = strtok_s(list, ", \t;", &ctx); tok && g_trace_n < TRACE_MAX;
         tok       = strtok_s(nullptr, ", \t;", &ctx)) {
        uintptr_t va = (uintptr_t)strtoul(tok, nullptr, 16);
        if (va < 0x401000 || va > 0x1000000) continue;
        int  i = g_trace_n;
        char b[160];
        // Never double-hook a function the DLL hooks itself (a double trampoline on one entry corrupts
        // both). install_trace_hooks runs LAST in MH_Seam_Init, so every Init-time self-hook already turned
        // its entry into a jmp (!= PROLOGUE) and is skipped by the guard below. begin_map_load is hooked
        // LATER (U2, at ArmManualLobby) so it still looks like a frame here -> skip it explicitly.
        if (va == mh::addr::llm_lobby_begin_map_load) {
            wsprintfA(b, "; TRACE SKIP %08X (reserved: DLL hooks this itself)\n", (unsigned)va);
            trace_log(b);
            continue;
        }
        if (*(const uint32_t *)va != PROLOGUE) {
            // C9/G68: say WHICH cause, and say it in the log the user is reading. This guard runs
            // LAST in MH_Seam_Init, i.e. AFTER every promotion, so a promoted target is the common
            // case rather than an exotic one -- three of the five canonical trace events
            // (lockstep_pump, commit_horizon, send_lockstep_extend) are in the default `[promote]
            // lockstep` closure, so tracing them printed "already hooked" and the function simply
            // never appeared in the dump. report_if_displaced writes to mh_net.log, which is NOT
            // where someone reading mh_trace.log is looking, so the reason is repeated here.
            if (const char *owner = mh::hook::promoted_owner_of(va)) {
                // ITS OWN BUFFER, and the size is the point: `b` above is char[160] and this message
                // plus a 30-character function name runs past 200. wsprintfA does not bounds-check,
                // so the first version of this line smashed the stack frame and hung the run --
                // caught only because the control arm passed with the identical ini. A longer log
                // line is not a cosmetic change when the buffer is shared.
                char pb[288];
                wsprintfA(pb,
                          "; TRACE SKIP %08X -- it is inside %s, which is PROMOTED in this run, so "
                          "the original entry never executes. Trace it with the matching [promote] "
                          "key off. (NOT a wrong build.)\n",
                          (unsigned)va, owner);
                trace_log(pb);
                continue;
            }
            wsprintfA(b, "; TRACE SKIP %08X (prologue %08X != frame / already hooked)\n",
                      (unsigned)va, *(const uint32_t *)va);
            trace_log(b);
            continue;
        }
        g_trace[i].addr   = va;
        g_trace[i].count  = 0;
        g_trace[i].tev_id = trace_tev_id(va);
        const char *nm    = trace_known_name(va);
        if (nm) lstrcpynA(g_trace[i].name, nm, sizeof(g_trace[i].name));
        else wsprintfA(g_trace[i].name, "%08X", (unsigned)va);
        // U30: named, so an entry another mechanism owns is refused as such in the end-of-arming
        // summary rather than as an anonymous detour. The prologue pre-check above is KEPT: it feeds
        // mh_trace.log, which is not where the [interlock] line goes, and it also carries the
        // reserved-address skip that has nothing to do with byte guards.
        if (!install_trampoline(va, g_trace_thunks[i], &g_ttramp[i], 8,
                                mh::hook::entry_claim::exclusive, "a [trace] funcs= entry tracer")) {
            wsprintfA(b, "; TRACE FAIL %08X (install refused -- see the [interlock] summary)\n",
                      (unsigned)va);
            trace_log(b);
            continue;
        }
        g_trace_n++;
        wsprintfA(b, "; TRACE armed [%d] %s (%08X)\n", i, g_trace[i].name, (unsigned)va);
        trace_log(b);
    }
}

// Dump non-zero call counts (call periodically + at a phase change). Read-only, idempotent.
extern "C" void MH_Seam_TraceDump(const char *when) {
    if (g_trace_n <= 0) return;
    char b[200];
    wsprintfA(b, "; TRACE counts @%s:\n", when ? when : "?");
    trace_log(b);
    for (int i = 0; i < g_trace_n; ++i) {
        wsprintfA(b, ";   [%d] %-30s (%08X) = %ld\n", i, g_trace[i].name,
                  (unsigned)g_trace[i].addr, g_trace[i].count);
        trace_log(b);
    }
}
