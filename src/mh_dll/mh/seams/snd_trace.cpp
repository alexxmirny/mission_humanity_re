//
// seams/snd_trace.cpp -- [log] snd_trace (a trace of every sound request, mh_snd.log) and the two voice
// fixes [snd] voice_priority / ack_cooldown_on_play.
//
// Full description, format and the findings that motivated it: include/mh_sndtrace_export.h.
//
// Two call-through detours (hook point snd_play on llm_snd_play, snd_order_ack on
// llm_strat_group_order_ack_voice) snapshot the game's own sound state before the original runs, let it
// run, and diff the state after. No register survives differently: the detours save and restore every
// register around the helpers. The trace alone only READS game memory (and calls
// IDirectSoundBuffer::GetStatus, which is a read); the fixes write the sound cfg priority byte for one
// call, the channel records' id field, and the ack voice's LAST_PLAY_TIME -- all sound-only state.
//
// NO FLOATING POINT. The game clock and the ack-voice cooldown are IEEE doubles read as raw bits and
// converted to milliseconds with integer arithmetic, so this file cannot disturb an x87 stack the game
// keeps live across a call (and needs no /arch decision).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "include/mh_sndtrace_export.h"
#include "include/mh_run_context.h" // mh_run_path, mh_log_stamp
#include "include/mh_config_dir.h"  // RL3: <config dir>mh_net.ini
#include "include/mh_log_sink.h"    // LOG1: async log sink (no file I/O on the game thread)
#include "addr/mh_addrs.gen.h"      // generated EN VAs
#include "hook/hookpoint.h"         // D5: the named hook points
#include "en_guard.h"               // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- the sound subsystem's memory (EN /eng/mh.exe; read-only here) -------------------------------------
constexpr uintptr_t A_CFG_TABLE   = 0x0070d5c0; // _G_LLM_SND_CFG_TABLE: llm_snd_cfg_entry[200], 0x2c stride
constexpr uintptr_t A_CFG_COUNT   = 0x0070f82c; // _G_LLM_SND_CFG_TABLE_COUNT
constexpr uintptr_t A_CHAN_COUNT  = 0x00520224; // _G_LLM_SND_CHANNEL_COUNT
constexpr uintptr_t A_CHAN_REC    = 0x0070d330; // per-channel {u8 priority; i32 sound_id; ...}, 0x15 stride
constexpr uintptr_t A_ENABLED     = 0x00ae2ab0; // _G_LLM_SND_ENABLED
constexpr uintptr_t A_MASTER_VOL  = 0x00ae2ab4; // _G_LLM_SND_MASTER_VOLUME
constexpr uintptr_t A_READY       = 0x00fe93fc; // _G_LLM_SND_SUBSYSTEM_READY
constexpr uintptr_t A_DS          = 0x0066991c; // IDirectSound* (non-null = DirectSound backend)
constexpr uintptr_t A_DS_HWND     = 0x00fe93e8; // the window handed to SetCooperativeLevel(level 3)
constexpr uintptr_t A_DS_CHAN     = 0x00fe9090; // per-channel DirectSound record, 0x34 stride
constexpr uintptr_t A_GAME_CLOCK  = 0x005d0198; // _G_LLM_STRAT_GAME_CLOCK (double)
constexpr uintptr_t A_ACK_LAST    = 0x0052029c; // _G_LLM_STRAT_ORDER_ACK_VOICE_LAST_PLAY_TIME (double)
constexpr uintptr_t A_ACK_COOLDWN = 0x0050019e; // _G_LLM_STRAT_ORDER_ACK_VOICE_COOLDOWN_SEC (double)
constexpr uintptr_t A_ACK_SUPP    = 0x005202a4; // _G_LLM_STRAT_ORDER_ACK_VOICE_SUPPRESSED_COUNT (int)
constexpr uintptr_t A_RA_SND_LAST = 0x005202a8; // _G_LLM_STRAT_RACE_ALERT_SOUND_LAST_PLAY_TIME (double)
constexpr uintptr_t A_RA_SND_CD   = 0x005001a6; // _G_LLM_STRAT_RACE_ALERT_SOUND_COOLDOWN_SEC (double)
constexpr uintptr_t A_RA_TXT_LAST = 0x005202b4; // _G_LLM_STRAT_RACE_ALERT_TEXT_LAST_PLAY_TIME (double)
constexpr uintptr_t A_RA_TXT_CD   = 0x005001ae; // _G_LLM_STRAT_RACE_ALERT_TEXT_COOLDOWN_SEC (double)

constexpr int CFG_STRIDE    = 0x2c;
constexpr int CFG_PRIORITY  = 0x21; // u8
constexpr int CFG_WAVE      = 0x28; // pointer; null = the sample is missing
constexpr int REC_STRIDE    = 0x15;
constexpr int DS_STRIDE     = 0x34;
constexpr int DS_FLAGS      = 0x2c; // bit 0 set = the channel record is unconfigured
constexpr int DS_BUF        = 0x30; // IDirectSoundBuffer* of the last sound started on the channel
constexpr int MAX_CH        = 16;
constexpr int SAME_ID_LIMIT = 3; // llm_snd_play drops when this many channels already play the id

constexpr DWORD DSBSTATUS_PLAYING    = 0x1;
constexpr DWORD DSBSTATUS_BUFFERLOST = 0x2;

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const volatile T *>(a); }

} // namespace
void seam_log(const char *s); // net_seams.cpp: one line to mh_net.log
namespace {

// ---- output --------------------------------------------------------------------------------------------
char          g_path[MAX_PATH];
unsigned long g_gen   = 0;
long          g_lines = 0;

void put(const char *fmt, ...) {
    mh_run_path(g_path, MAX_PATH, "%smh_snd.log", &g_gen);
    char    line[640];
    int     n = mh_log_stamp(line); // "[hh:mm:ss.mmm] ": the same wall stamp mh_net.log carries
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(line + n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n++] = '\n';
    line[n]   = 0;
    ++g_lines;
    mh_logq_write(g_path, line, n, 32ll * 1024 * 1024); // one-generation rotation at 32 MB (~1.4 MB per combat minute)
}

// The features this seam carries (read once at install).
bool g_trace    = false; // [log] snd_trace
bool g_fix_prio = false; // [snd] voice_priority
bool g_fix_ack  = false; // [snd] ack_cooldown_on_play
bool g_fix_load = false; // [snd] cooldown_load_fix

// [snd] cooldown_load_fix (player report 2026-10-14, rc6): the move-ack voice and the two race-alert throttles keep
// their LAST_PLAY_TIME in plain statics that nothing resets (only their own function reads or writes them, xrefs
// checked). Load a save whose game clock is EARLIER than the last voice and every ack/alert stays suppressed until the
// clock catches up -- after a second load in one sitting the move voices went silent for the rest of the session
// (ACK gap=-1144523ms). A stamp ahead of the clock can only be a stale one, so put it one cooldown back: the next
// request plays. Sound-only state (the alert's voice pick draws the cosmetic fx PRNG channel, not the sim's).
void clamp_stale_stamp(uintptr_t last_va, uintptr_t cd_va, double clock) {
    volatile double *last = reinterpret_cast<volatile double *>(last_va);
    if (*last > clock) *last = clock - 2.0 * *reinterpret_cast<const volatile double *>(cd_va);
}
void clamp_stale_stamps() {
    if (!g_fix_load) return;
    __try {
        const double clock = *reinterpret_cast<const volatile double *>(A_GAME_CLOCK);
        clamp_stale_stamp(A_ACK_LAST, A_ACK_COOLDWN, clock);
        clamp_stale_stamp(A_RA_SND_LAST, A_RA_SND_CD, clock);
        clamp_stale_stamp(A_RA_TXT_LAST, A_RA_TXT_CD, clock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

constexpr int      PRIO_BOOST   = 100;
constexpr int      SENTINEL_ID  = 0x7fff5eed; // an idle channel's id while a request is in flight
constexpr uint32_t RA_ACK_VOICE = 0x00425a24; // llm_snd_play's return address for the ack's race voice

// IEEE-754 double (raw bits, non-negative, finite) -> whole milliseconds. Integer arithmetic only.
long long dbl_ms(uint64_t bits) {
    const int      e    = (int)((bits >> 52) & 0x7ff);
    const uint64_t frac = bits & ((1ull << 52) - 1);
    if (e == 0) return 0; // zero / denormal
    if (e == 0x7ff) return -1;
    const uint64_t m  = (1ull << 52) | frac; // value = m * 2^(e-1075)
    const int      sh = e - 1075;
    const uint64_t m1 = m * 1000ull; // < 2^63
    if (sh >= 0) return sh > 10 ? -1 : (long long)(m1 << sh);
    if (-sh >= 63) return 0;
    return (long long)(m1 >> (-sh));
}
// "%u.%03u" pieces of a double read at `a`.
struct secs {
    long long ms;
};
secs dbl_at(uintptr_t a) { return secs{dbl_ms(rd<uint64_t>(a))}; }
#define SEC_FMT    "%lu.%03u" // wsprintf has no %I64d; game seconds fit an unsigned long for any real session
#define SEC_ARG(s) ((unsigned long)((s).ms / 1000)), (unsigned)((s).ms % 1000)

// `va` is the RETURN address llm_snd_play was called with, i.e. the call site + 5.
const char *caller_name(uint32_t va) {
    switch (va) {
        case 0x00425a24:
        case 0x00425a5a: return "order_ack_voice";
        case 0x00425943: return "select_voice_single";
        case 0x004259a8: return "select_voice_group";
        case 0x0049a519: return "bldg_select(sound_click)";
        case 0x0049a54d: return "bldg_select(offline 0x7d/0x7e)";
        case 0x0049a55e: return "bldg_select(unbuilt 0x7b)";
        case 0x0049a613: return "bldg_select(energy-deficit 0x7c)";
        case 0x00443ade: return "input_update(unit sound_move)";
        case 0x00425ae0:
        case 0x00425b16: return "race_alert_sound_emit";
        case 0x00425bb9: return "race_alert_text_emit";
        case 0x004420e6:
        case 0x00442eaa: return "input_update(0xb3)";
        case 0x00442e47: return "input_update(0x44)";
        case 0x00441297: return "projectile_tick";
        case 0x0048c501:
        case 0x0048c123:
        case 0x0048c163: return "unit_fire_weapon";
        case 0x0044db5d: return "ambient_tick";
        default: break;
    }
    if (va >= 0x00411800 && va < 0x00413b00) return "ui_hud";
    if (va >= 0x70000000u) return "(injected module)";
    return "";
}

// [snd] voice_priority: the call sites whose sound is a voice or UI feedback (everything but combat and
// ambience). Keyed by return address, like caller_name.
bool is_voice_or_ui(uint32_t va) {
    switch (va) {
        case 0x00425a24: // order_ack_voice (race voice)
        case 0x00425a5a: // order_ack_voice (generic 0x44)
        case 0x00425943: // select_voice_single
        case 0x004259a8: // select_voice_group
        case 0x0049a519: // bldg_select(sound_click)
        case 0x0049a54d: // bldg_select(offline)
        case 0x0049a55e: // bldg_select(unbuilt)
        case 0x0049a613: // bldg_select(energy-deficit)
        case 0x00443ade: // input_update(unit sound_move)
        case 0x00425ae0: // race_alert_sound_emit
        case 0x00425b16:
        case 0x00425bb9: // race_alert_text_emit
        case 0x004420e6: // input_update(0xb3)
        case 0x00442eaa:
        case 0x00442e47: // input_update(0x44)
            return true;
        default: break;
    }
    return va >= 0x00411800 && va < 0x00413b00; // ui_hud
}

// ---- the game's channel state -------------------------------------------------------------------------
struct chan {
    bool    busy; // a DirectSound buffer is attached and DSBSTATUS_PLAYING is set (the game's state==0)
    uint8_t prio;
    int     id;
    void   *buf;
    DWORD   status; // GetStatus result (valid when buf)
    bool    status_ok;
};

DWORD ds_status(void *buf, bool *ok) {
    DWORD st = 0;
    *ok      = false;
    if (!buf) return 0;
    __try {
        typedef long(__stdcall * get_status_fn)(void *, DWORD *);
        void **vt = *reinterpret_cast<void ***>(buf);
        if (((get_status_fn)vt[9])(buf, &st) >= 0) *ok = true; // IDirectSoundBuffer::GetStatus
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return st;
}

int channel_count() {
    int n = rd<int>(A_CHAN_COUNT);
    return n < 0 ? 0 : (n > MAX_CH ? MAX_CH : n);
}

void read_channels(chan *c, int n) {
    const bool ds = rd<uint32_t>(A_DS) != 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t rec = A_CHAN_REC + (uintptr_t)i * REC_STRIDE;
        c[i].prio           = rd<uint8_t>(rec);
        c[i].id             = *reinterpret_cast<const int *>(rec + 1);
        c[i].buf            = nullptr;
        c[i].status         = 0;
        c[i].status_ok      = false;
        c[i].busy           = false;
        if (ds) {
            c[i].buf = reinterpret_cast<void *>(rd<uint32_t>(A_DS_CHAN + (uintptr_t)i * DS_STRIDE + DS_BUF));
            if (c[i].buf) {
                c[i].status = ds_status(c[i].buf, &c[i].status_ok);
                c[i].busy   = c[i].status_ok && (c[i].status & DSBSTATUS_PLAYING) != 0;
            }
        }
    }
}

// ---- llm_snd_play -------------------------------------------------------------------------------------
struct snd_pre {
    int      id, vol, eff;
    int      prio;    // the priority the request runs with (the cfg byte, raised by voice_priority)
    int      boosted; // 1 = the cfg byte was raised and must be put back
    int      cfg_prio;
    uint32_t caller;
    int      n;
    bool     sent[MAX_CH]; // idle channel whose id holds SENTINEL_ID for the call
    chan     ch[MAX_CH];
};
snd_pre       g_pre;
volatile long g_requests         = 0; // llm_snd_play calls seen (the state poller reports it)
void         *g_snd_tramp        = nullptr;
bool          g_live             = false; // a request is in flight (a re-entrant call would clobber g_pre; none exists)
bool          g_in_ack           = false; // llm_strat_group_order_ack_voice is on the stack
bool          g_ack_voice_played = false;

inline uint8_t *rec_prio(int i) { return reinterpret_cast<uint8_t *>(A_CHAN_REC + (uintptr_t)i * REC_STRIDE); }
inline int     *rec_id(int i) { return reinterpret_cast<int *>(A_CHAN_REC + (uintptr_t)i * REC_STRIDE + 1); }

void __cdecl snd_enter(int id, int vol, uint32_t caller) {
    ++g_requests;
    clamp_stale_stamps(); // the race-alert stamps are only reachable here (any sound request runs before the next alert)
    __try {
        g_pre.id       = id;
        g_pre.vol      = vol;
        g_pre.caller   = caller;
        const int cnt  = rd<int>(A_CFG_COUNT);
        g_pre.eff      = (cnt <= id) ? 0 : id; // the game's own fallback to entry 0
        g_pre.n        = channel_count();
        uint8_t *cfgp  = reinterpret_cast<uint8_t *>(A_CFG_TABLE + (uintptr_t)g_pre.eff * CFG_STRIDE + CFG_PRIORITY);
        g_pre.cfg_prio = *cfgp;
        g_pre.prio     = g_pre.cfg_prio;
        g_pre.boosted  = 0;
        read_channels(g_pre.ch, g_pre.n);
        if (g_fix_prio && is_voice_or_ui(caller)) {
            const int b   = g_pre.cfg_prio + PRIO_BOOST;
            g_pre.prio    = b > 255 ? 255 : b;
            *cfgp         = (uint8_t)g_pre.prio;
            g_pre.boosted = 1;
        }
        // Make a play on an idle channel always visible: an idle channel's id is never read by
        // llm_snd_play (the same-id count only counts playing channels; an idle channel is taken at once).
        const bool ds = rd<uint32_t>(A_DS) != 0;
        for (int i = 0; i < g_pre.n; ++i) {
            g_pre.sent[i] = (g_fix_prio || g_fix_ack) && ds && !g_pre.ch[i].busy;
            if (g_pre.sent[i]) *rec_id(i) = SENTINEL_ID;
        }
        g_live = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_live = false;
        put("SND request id=%d: snapshot faulted (seam swallowed it)", id);
    }
}

void chan_list(char *out, int cap, const chan *c, int n) {
    int o  = 0;
    out[0] = 0;
    for (int i = 0; i < n && o < cap - 40; ++i)
        o += wsprintfA(out + o, "%s%d:%s p%u id%d", i ? " | " : "", i, c[i].busy ? "busy" : "free", (unsigned)c[i].prio,
                       c[i].id);
}

// The fixes' half of the leave: put the cfg byte and the sentinels back, find the channel the request
// played on (-1 = dropped), write the id the steal path forgets. Runs whether or not the trace is on.
int fix_leave() {
    const snd_pre &p = g_pre;
    if (p.boosted)
        *reinterpret_cast<uint8_t *>(A_CFG_TABLE + (uintptr_t)p.eff * CFG_STRIDE + CFG_PRIORITY) = (uint8_t)p.cfg_prio;
    int played = -1;
    for (int i = 0; i < p.n; ++i) {
        if (p.sent[i]) {
            if (*rec_id(i) == SENTINEL_ID) *rec_id(i) = p.ch[i].id; // untouched: put the old id back
            else if (played < 0) played = i;                        // llm_snd_play took this idle channel
        } else if (*rec_prio(i) != p.ch[i].prio || *rec_id(i) != p.ch[i].id) {
            if (played < 0) played = i;                                     // stolen
            if (g_fix_prio && *rec_id(i) == p.ch[i].id) *rec_id(i) = p.eff; // the steal path's missing id write
        }
    }
    return played;
}

void __cdecl snd_leave() {
    if (!g_live) return;
    g_live = false;
    __try {
        const snd_pre &p      = g_pre;
        const int      played = (g_fix_prio || g_fix_ack) ? fix_leave() : -2;
        if (g_in_ack && p.caller == RA_ACK_VOICE) g_ack_voice_played = played >= 0;
        if (!g_trace) return;
        chan post[MAX_CH];
        read_channels(post, p.n);

        const uintptr_t cfg    = A_CFG_TABLE + (uintptr_t)p.eff * CFG_STRIDE;
        const bool      wave   = rd<uint32_t>(cfg + CFG_WAVE) != 0;
        const int       prio   = p.prio;
        const int       enable = rd<int>(A_ENABLED);
        const int       master = rd<int>(A_MASTER_VOL);
        const bool      ds     = rd<uint32_t>(A_DS) != 0;

        // What the game's branch ladder would decide from the pre-state (integer replica of llm_snd_play).
        int same = 0, first_free = -1, low_ch = -1, low_pr = 1000;
        for (int i = 0; i < p.n; ++i) {
            if (p.ch[i].busy) {
                if (p.ch[i].id == p.eff) ++same;
                if ((int)p.ch[i].prio < low_pr) low_pr = p.ch[i].prio, low_ch = i;
            } else if (first_free < 0) {
                first_free = i;
            }
        }
        const char *why = nullptr; // set for a predicted DROP
        if (!wave) why = "dropped: no sample (cfg wave_ptr null)";
        else if (!enable) why = "dropped: sound disabled (SND_ENABLED=0)";
        else if (!master) why = "dropped: master volume 0";
        else if (same >= SAME_ID_LIMIT) why = "dropped: same-id limit (3 channels already play it)";
        else if (first_free < 0 && !(low_ch >= 0 && low_pr < prio)) why = "dropped: no free channel (all busy with priority >= request)";

        // What actually happened: which channel record the game rewrote.
        int changed = -1;
        for (int i = 0; i < p.n; ++i)
            if (post[i].id != p.ch[i].id || post[i].prio != p.ch[i].prio || post[i].buf != p.ch[i].buf) {
                changed = i;
                break;
            }
        // A request that rewrote a record with identical id+prio+buffer pointer is indistinguishable; the
        // predicted channel is then the answer.
        int pred = first_free >= 0 ? first_free : low_ch;
        if (played != -2) changed = played; // the fixes' sentinel decided it exactly
        else if (changed < 0 && !why) changed = pred;

        char outcome[200];
        if (why) {
            wsprintfA(outcome, "%s", why);
        } else if (changed < 0) {
            wsprintfA(outcome, "ANOMALY: predicted play on ch%d but no channel record changed", pred);
        } else {
            const chan &c   = post[changed];
            const char *how = (changed == first_free) ? "free" : "STOLEN (lowest priority)";
            const char *dsx;
            if (!ds) dsx = "raw-backend";
            else if (!c.buf) dsx = "DS-FAIL: buffer null (CreateSoundBuffer/Lock failed)";
            else if (!c.status_ok) dsx = "DS-FAIL: GetStatus failed";
            else if (c.status & DSBSTATUS_BUFFERLOST) dsx = "DS-FAIL: BUFFERLOST";
            else if (c.status & DSBSTATUS_PLAYING) dsx = "playing";
            else dsx = "NOT-PLAYING right after Play (stale/finished buffer or Play failed)";
            wsprintfA(outcome, "played ch%d (%s) ds=%s st=0x%x", changed, how, dsx, (unsigned)c.status);
        }

        const secs clk = dbl_at(A_GAME_CLOCK);
        char       extra[400];
        extra[0] = 0;
        // The channel list only when it explains something (a drop on channels, or an anomaly/DS fault).
        char lst[360];
        lst[0] = 0;
        if (why && (first_free < 0 || same >= SAME_ID_LIMIT)) chan_list(lst, sizeof(lst), p.ch, p.n);
        else if (!why && (changed < 0 || (outcome[0] == 'p' && !strstr(outcome, "ds=playing"))))
            chan_list(lst, sizeof(lst), post, p.n);
        if (lst[0]) wsprintfA(extra, " chans=[%s]", lst);

        char        who[48];
        const char *nm = caller_name(p.caller);
        wsprintfA(who, "%s%s", nm[0] ? " " : "", nm);
        put("SND id=%d%s vol=%d prio=%d clk=" SEC_FMT " tick=%lu caller=0x%08x%s fg=%d -> %s%s", p.id,
            p.eff != p.id ? "(->0 table-count fallback)" : "", p.vol, prio, SEC_ARG(clk), (unsigned long)GetTickCount(),
            (unsigned)p.caller, who, (int)(GetForegroundWindow() == (HWND)(uintptr_t)rd<uint32_t>(A_DS_HWND)),
            outcome, extra);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        put("SND trace helper faulted (observer swallowed it; the game was not affected)");
    }
}

// Watcom __watcall: the request is in EAX (id) and EDX (volume); nothing is on the stack. The detour
// saves ALL registers, hands (id, vol, return-address) to snd_enter, restores them, CALLS the original
// through the trampoline, then saves everything again for snd_leave and returns to the real caller with
// the registers exactly as the original left them. (pushad/popad is symmetric around the second helper.)
// clang-format off
__declspec(naked) void snd_play_detour() {
    __asm {
        pushad
        pushfd
        mov  ecx, [esp + 36]        // the caller's return address (pushad 32 + pushfd 4)
        push ecx
        push edx
        push eax
        call snd_enter
        add  esp, 12
        popfd
        popad
        call dword ptr [g_snd_tramp] // stolen prologue + the rest of llm_snd_play; returns here
        pushad
        pushfd
        call snd_leave
        popfd
        popad
        ret
    }
}
// clang-format on

// ---- llm_strat_group_order_ack_voice ----------------------------------------------------------------------
struct ack_pre {
    uint64_t last, clock, cooldown;
    int      supp;
    uint32_t caller;
};
ack_pre g_ack;
void   *g_ack_tramp = nullptr;
bool    g_ack_live  = false;

void __cdecl ack_enter(uint32_t caller) {
    clamp_stale_stamps(); // before the snapshot: the ack fix's rearm restores the clamped stamp, not the stale one
    __try {
        g_ack.last         = rd<uint64_t>(A_ACK_LAST);
        g_ack.clock        = rd<uint64_t>(A_GAME_CLOCK);
        g_ack.cooldown     = rd<uint64_t>(A_ACK_COOLDWN);
        g_ack.supp         = rd<int>(A_ACK_SUPP);
        g_ack.caller       = caller;
        g_ack_live         = true;
        g_in_ack           = true;
        g_ack_voice_played = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ack_live = false;
    }
}

void __cdecl ack_leave() {
    g_in_ack = false;
    if (!g_ack_live) return;
    g_ack_live = false;
    __try {
        const uint64_t last2 = rd<uint64_t>(A_ACK_LAST);
        // [snd] ack_cooldown_on_play: the voice was requested (the stamp moved) but did not play -- put the
        // stamp back so the next order is not silenced by a cooldown nobody heard.
        bool rearmed = false;
        if (g_fix_ack && last2 != g_ack.last && !g_ack_voice_played) {
            *reinterpret_cast<volatile uint64_t *>(A_ACK_LAST) = g_ack.last;
            rearmed                                            = true;
        }
        if (!g_trace) return;
        const int   supp2 = rd<int>(A_ACK_SUPP);
        const char *br;
        char        b[96];
        if (rearmed) {
            br = "EMIT, voice DROPPED -> cooldown not armed (ack_cooldown_on_play)";
        } else if (last2 != g_ack.last) {
            br = "EMIT (cooldown elapsed; race voice requested -- see the SND line above)";
        } else if (supp2 == g_ack.supp + 1) {
            wsprintfA(b, "SUPPRESSED (silent) n=%d of 6", supp2);
            br = b;
        } else if (supp2 == 0 && g_ack.supp >= 5) {
            br = "GENERIC 0x44 (6th refusal plays the generic cue; see the SND line above)";
        } else if (supp2 == 0 && g_ack.supp == 0) {
            br = "no change (snapshot-resync reset or an early out)";
        } else {
            br = "unclassified state change";
        }
        const secs clk = secs{dbl_ms(g_ack.clock)}, lp = secs{dbl_ms(g_ack.last)}, cd = secs{dbl_ms(g_ack.cooldown)};
        put("ACK clk=" SEC_FMT " last_play=" SEC_FMT " cooldown=" SEC_FMT " gap=%ldms supp_before=%d caller=0x%08x -> %s",
            SEC_ARG(clk), SEC_ARG(lp), SEC_ARG(cd), (long)(clk.ms - lp.ms), g_ack.supp, (unsigned)g_ack.caller, br);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        put("ACK trace helper faulted (observer swallowed it; the game was not affected)");
    }
}

// clang-format off
__declspec(naked) void ack_detour() {
    __asm {
        pushad
        pushfd
        mov  ecx, [esp + 36]
        push ecx
        call ack_enter
        add  esp, 4
        popfd
        popad
        call dword ptr [g_ack_tramp]
        pushad
        pushfd
        call ack_leave
        popfd
        popad
        ret
    }
}
// clang-format on

// ---- focus tap ----------------------------------------------------------------------------------------------
HHOOK g_hook = nullptr;

LRESULT CALLBACK focus_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && l) {
        const CWPSTRUCT *c    = (const CWPSTRUCT *)l;
        const char      *what = nullptr;
        char             det[48];
        det[0] = 0;
        switch (c->message) {
            case WM_ACTIVATEAPP:
                what = "WM_ACTIVATEAPP";
                wsprintfA(det, "active=%d", c->wParam ? 1 : 0);
                break;
            case WM_ACTIVATE:
                what = "WM_ACTIVATE";
                wsprintfA(det, "state=%s", LOWORD(c->wParam) == WA_INACTIVE ? "inactive" : "active");
                break;
            case WM_KILLFOCUS: what = "WM_KILLFOCUS"; break;
            case WM_SETFOCUS: what = "WM_SETFOCUS"; break;
            case WM_SIZE:
                if (c->wParam == SIZE_MINIMIZED || c->wParam == SIZE_RESTORED) {
                    what = "WM_SIZE";
                    wsprintfA(det, "%s", c->wParam == SIZE_MINIMIZED ? "MINIMIZED" : "restored");
                }
                break;
            default: break;
        }
        if (what && !(GetWindowLongA(c->hwnd, GWL_STYLE) & WS_CHILD)) {
            const HWND dsw = (HWND)(uintptr_t)rd<uint32_t>(A_DS_HWND);
            const secs clk = dbl_at(A_GAME_CLOCK);
            put("FOCUS %s hwnd=%p %s is_ds_hwnd=%d fg_is_this=%d clk=" SEC_FMT " tick=%lu", what, (void *)c->hwnd, det,
                (int)(c->hwnd == dsw), (int)(GetForegroundWindow() == c->hwnd), SEC_ARG(clk), (unsigned long)GetTickCount());
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// ---- the sound subsystem's own state, polled (read-only) ---------------------------------------------------
// A player whose DirectSound device never opened (or whose volume is 0) hears nothing at all; that is the
// first thing a flaky-sound report needs to rule in or out, and it is invisible in every other log.
// One line whenever any of these change, so a quiet run costs a handful of lines.
struct poll_state {
    int      enabled, master, ready, chans, cfgs;
    uint32_t ds, hwnd;
};

DWORD WINAPI state_poll(LPVOID) {
    poll_state last     = {-1, -1, -1, -1, -1, 0xffffffffu, 0xffffffffu};
    long       last_req = 0;
    int        quiet    = 0;
    for (int iter = 0; iter < 0x7fffffff; ++iter) {
        Sleep(500);
        poll_state now = {};
        __try {
            now.enabled = rd<int>(A_ENABLED);
            now.master  = rd<int>(A_MASTER_VOL);
            now.ready   = rd<int>(A_READY);
            now.chans   = rd<int>(A_CHAN_COUNT);
            now.cfgs    = rd<int>(A_CFG_COUNT);
            now.ds      = rd<uint32_t>(A_DS);
            now.hwnd    = rd<uint32_t>(A_DS_HWND);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (memcmp(&now, &last, sizeof(now)) != 0) {
            put("STATE enabled=%d master_volume=%d subsystem_ready=%d directsound=%s ds_hwnd=%08x channels=%d cfg_entries=%d "
                "requests_so_far=%ld",
                now.enabled, now.master, now.ready, now.ds ? "open" : "NONE", (unsigned)now.hwnd, now.chans, now.cfgs,
                (long)g_requests);
            last     = now;
            last_req = g_requests;
        } else if (g_requests != last_req && ++quiet >= 60) { // activity heartbeat, at most every ~30 s
            quiet = 0;
            put("STATE requests_so_far=%ld (llm_snd_play calls seen since arming)", (long)g_requests);
            last_req = g_requests;
        }
    }
    return 0;
}

} // namespace

extern "C" int MH_SndTrace_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only (the VAs above are EN)

    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);                                               // RL3: <config dir>mh_net.ini
    g_trace    = mh_ini_get_int("log", "snd_trace", 0, ini) != 0;            // default off
    g_fix_prio = mh_ini_get_int("snd", "voice_priority", 1, ini) != 0;       // default ON
    g_fix_ack  = mh_ini_get_int("snd", "ack_cooldown_on_play", 1, ini) != 0; // default ON
    g_fix_load = mh_ini_get_int("snd", "cooldown_load_fix", 1, ini) != 0;    // default ON
    if (!g_trace && !g_fix_prio && !g_fix_ack && !g_fix_load) return 0;

    // Both points are exclusive: the trace and the fixes share the two detours. The ack detour is only
    // needed by the trace and the ack fix; the ack fix also needs the snd_play detour (its "did it play").
    const bool a = mh::hook::arm_observer(mh::hook::point::snd_play, (void *)snd_play_detour, &g_snd_tramp);
    const bool b = (g_trace || g_fix_ack || g_fix_load) &&
                   mh::hook::arm_observer(mh::hook::point::snd_order_ack, (void *)ack_detour, &g_ack_tramp);
    {
        char l[240];
        if (g_fix_prio) {
            wsprintfA(l, "; [snd] voice_priority=1 %s: voices/UI sounds outrank combat sounds for a channel\n",
                      a ? "active" : "NOT ACTIVE (llm_snd_play hook refused)");
            seam_log(l);
        }
        if (g_fix_ack) {
            wsprintfA(l, "; [snd] ack_cooldown_on_play=1 %s: a dropped move-ack voice does not start the 2 s cooldown\n",
                      (a && b) ? "active" : "NOT ACTIVE (llm_snd_play / order-ack hook refused or displaced)");
            seam_log(l);
        }
        if (g_fix_load) {
            wsprintfA(l, "; [snd] cooldown_load_fix=1 %s: a voice/alert cooldown stamp ahead of a loaded save's clock is reset\n",
                      a ? "active" : "NOT ACTIVE (llm_snd_play hook refused)");
            seam_log(l);
        }
        if (!a) g_fix_prio = false;
        if (!(a && b)) g_fix_ack = false;
    }
    if (!g_trace) return (a || b) ? 1 : 0;

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&MH_SndTrace_Install, &self);
    g_hook = SetWindowsHookExA(WH_CALLWNDPROC, focus_proc, self, GetCurrentThreadId());
    CreateThread(nullptr, 0, state_poll, nullptr, 0, nullptr);
    put("; snd_trace armed: llm_snd_play=%s order_ack_voice=%s focus_tap=%s "
        "(columns: stamp, SND|ACK|FOCUS, ids, game clock s, GetTickCount, caller VA, fg=1 when the "
        "foreground window is the DirectSound cooperative-level window, outcome)",
        a ? "armed" : "REFUSED", b ? "armed" : "REFUSED", g_hook ? "armed" : "FAILED");
    return (a || b) ? 1 : 0;
}
