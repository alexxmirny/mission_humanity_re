//
// seams/ui_net_indicator.cpp -- mp:L1: the player-visible connection indicator.
//
// ---- WHAT IT SHOWS, AND WHY EXACTLY THESE -------------------------------------------------------
//
// Three numbers and one message, and the shortlist is the whole design decision. The latency work
// (mp:L0, the vocabulary; mp:T3, the measurement) settled which quantities a player can act on:
//
//   1. PING -- the per-peer SRTT, RFC 6298 smoothed (alpha = 1/8), in milliseconds. Shown because
//      every player already knows how to read a ping number, NOT because it is the number that
//      decides how the game feels.
//
//   2. A STABILITY BAR, four levels, derived from IPDV (delay VARIATION) and LOSS -- deliberately
//      NOT from the ping. This is the one that needs defending, because the obvious design is to
//      colour the ping digit and be done. The Age of Empires lockstep write-up ("1500 archers on a
//      28.8") measured the opposite of the obvious: players tolerated a HIGH but STEADY delay far
//      better than a lower delay that varied, because a steady delay becomes the game's tempo while
//      a varying one defeats the timing the player has learnt. A link can therefore have a low,
//      calm-looking ping and feel terrible, and a blended single number would hide precisely the
//      half that matters. So the bar is a separate element with separate inputs.
//
//   3. CMD -- command latency, in milliseconds: the wall time from the click to the order taking
//      effect in the sim. This is the number the player actually feels, and it is usually DOMINATED
//      BY THE LOOKAHEAD rather than by the ping, which is why a ping-only readout cannot answer
//      "will my orders feel laggy". Here it is computed as
//
//          cmd_ms = lookahead + step period
//                 = _G_LLM_STRAT_LOCKSTEP_STEP_SIZE  +  _G_LLM_STRAT_SIM_STEP_INTERVAL
//
//      -- the lookahead FLOOR (every order is stamped with an exec_time at or after the horizon we
//      are currently advertising) plus the sub-step quantisation (the order lands on the next sim
//      sub-step boundary at or after that). The lookahead term is read LIVE, so the adaptive
//      controller raising the lookahead on a slow link is visible here as a rising CMD -- which is
//      the honest way a latent 200 ms path reaches the player: not as a scarier ping, but as orders
//      that take longer to happen.
//
//      WHAT THIS NUMBER OMITS, stated rather than hidden: the vocabulary's full definition of
//      command latency is `lookahead + local input queueing + relay hop`, and local input queueing
//      ranges from ~0 up to a whole advertisement window depending on where in that window the
//      click falls. One sub-step is the TYPICAL value of that term, not its worst case; the worst
//      case is closer to two lookaheads. A displayed number that swung by a factor of two between
//      two identical clicks would be worse than useless, so the display takes the stable, typical
//      reading and this comment carries the bound.
//
//   4. On a stall: "WAITING FOR <name>". Not a number -- from the player's seat a stall is binary
//      (the game is frozen or it is not), and the useful information is WHO. Naming the peer turns
//      "the game froze" into "I know why, and who", which is the difference between a bug report
//      and a rage-quit. The name comes from the strategic player table, which is where the lobby
//      slot names end up once the match starts, so it is the same name the player saw in the lobby.
//
// Everything else the transport and the lockstep measure stays in the logs and in the developer
// overlay. This is a shortlist ON PURPOSE.
//
// ---- HOW IT DRAWS -------------------------------------------------------------------------------
//
// Through the GAME's own font path -- llm_gfx_font_select(slot) then llm_gfx_draw_text_blend_clipped
// -- exactly as the `[fonts] probe_text` render probe does (gfx_font_guard.cpp, which is the working
// precedent this reuses rather than a second mechanism). Two consequences worth stating:
//
//   * it looks like the game, because it IS the game's renderer with the game's font, so a player
//     does not see a developer overlay bolted on; and
//   * it inherits the font system's readiness rule: selecting a slot whose data pointer is still
//     null dereferences it for the line height, so the draw waits on _G_LLM_GFX_FONT_DATA_PTRS[slot]
//     being non-null rather than on a game mode. In a live match it always is.
//
// The alternative carriers were considered and rejected for reasons that are structural rather than
// aesthetic. The floating-message queue (llm_ui_print_floating_msg_*) is an EVENT channel -- entries
// expire -- so a standing readout would have to re-post itself forever and would push real messages
// off the screen. The lobby status-line widget (the U23/U42 notice's carrier) is a MENU widget and
// is not on screen during a match at all.
//
// ---- DETERMINISM --------------------------------------------------------------------------------
//
// Render-only, and every input is a read: MH_Net_GetStats (the transport's own snapshot), the two
// pacing doubles (lookahead and sub-step), the lockstep's own already-computed stall state, and a
// name out of the player table. It consumes no RNG, calls no sim body, and writes no game state. It runs from the
// present hook, i.e. after the frame's sim work is already done. The one thing it does write is the
// selected font slot, which the game re-selects before each of its own draws (every retail text site
// does llm_gfx_font_select first), and which is render state in any case.
//
// ---- WHY IT IS OFF IN SOLO ----------------------------------------------------------------------
//
// The draw is gated on _G_LLM_GAME_SESSION_MODE == 3 (MP lockstep) AND a started transport AND at
// least one peer. A single-player game is session mode 1 or 2 and has no transport, so it never
// reaches the first branch -- the indicator is not hidden in solo, it is structurally absent.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "seams/ui_net_indicator.h"
#include "include/mh_net_export.h" // MH_Net_GetStats / IsStarted / PeerCount / LocalPlayerId
#include "addr/mh_addrs.gen.h"     // generated EN VAs
#include "addr/mh_calls.gen.h"     // mh::call::llm_gfx_font_select / llm_gfx_draw_text_blend_clipped
#include "state/region_runtime.h"  // SB-HOSTFREE: a movable region is read where it IS
#include "en_guard.h"              // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA

// The process's single physical mh_net.log writer (net_seams.cpp). Declared rather than re-derived:
// a second handle on the same file would be a second log nobody reads, and the channel census
// (tools/check_instrument_wiring.py) is written around there being exactly one writer.
void seam_log(const char *s);

namespace {

// ---- the state this reads ----------------------------------------------------------------------
constexpr uintptr_t ADDR_SESSION_MODE = mh::addr::_G_LLM_GAME_SESSION_MODE;        // byte; 3 = lockstep
constexpr uintptr_t ADDR_STEP_SIZE    = mh::addr::_G_LLM_STRAT_LOCKSTEP_STEP_SIZE; // double: lookahead, game-sec
constexpr uintptr_t ADDR_PLAYERS      = mh::addr::_G_LLM_STRAT_PLAYERS;            // llm_strat_player_profile[8]
// _G_LLM_GFX_FONT_DATA_PTRS[7] -- null until boot stage 3 (llm_gfx_font_system_init). Spelled as a
// raw VA with its provenance on the gfx_font_guard.cpp precedent: it is an unlisted data label the
// DLL address manifest does not carry, and it is not in a movable region (checked, same as there).
constexpr uintptr_t ADDR_FONT_DATA_PTRS = 0x005d8198u;

// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE, and a relocating host fills the
// .bss it left with 0xCD, so a VA baked at compile time reads poison.
inline uintptr_t ADDR_SIM_STEP_INT() { // double; the strategic sub-step interval, game-sec
    return mh::state::live_base(mh::state::RID_STRAT_SIM_STEP_INTERVAL);
}
// llm_strat_player_profile: stride 0x740, `name` at +0x714 (the two operands the retail
// "<something> (<player name>)" alert composes its line from -- IMUL EDX,pidx,0x740 / ADD EAX,0x714).
constexpr unsigned PLAYER_STRIDE   = 0x740u;
constexpr unsigned PLAYER_NAME_OFF = 0x714u;

// ---- L1d: placing the indicator by GEOMETRY instead of a fixed 8,y ------------------------------
//
// Retail draws received chat and the "<something> (<player>)" alert line at the frame's TOP-LEFT
// (rows ~8..24) -- see PLAYER_STRIDE/PLAYER_NAME_OFF's comment above, and chat_glyphs' capture_c1_privet
// going red is what proved it. The fix is not a second fixed offset (8,44 was exactly that, and it
// was a STOPGAP): it is anchoring off the SCREEN, so the indicator tracks whatever resolution the
// player is actually running instead of assuming retail's own top-left band stays clear.
//
// TOP-RIGHT OF THE MAP VIEWPORT, not "above the bottom status bar": the bottom HUD chrome
// (llm_ui_hud_widget_icon_blit, docs/symbols.md) blits at FIXED stock-mode widget coordinates that
// -- per that function's own D13 note -- go "too narrow at a custom width", i.e. it does not track a
// custom resolution the way the viewport does, so anchoring off it would need geometry this seam has
// no business knowing. The top-right corner needs only two numbers every frame already carries:
// WindowWidth/WindowHeight (mh::addr::WindowWidth/WindowHeight, the live framebuffer size) and
// G_WIN_W (mh::addr::G_WIN_W, the STRATEGIC VIEWPORT width = WindowWidth - 0xa0, i.e. clear of the
// 160px right side panel; set by llm_gfx_view_metrics_init). All three are plain statics -- read the
// same non-movable way gfx_capture.cpp/gfx_overlay.cpp/video.cpp already read them, NOT through
// live_base (SB-HOSTFREE is about a different set of regions; these three are not in it).
// WindowWidth/WindowHeight (EN 0x005202c8 / 0x005202cc) are the two live-resolution globals; this
// seam only needs the derived viewport width below, but they are the numbers G_WIN_W is built from
// and the ones a future line here (e.g. a bottom-anchored element) would read directly.
constexpr uintptr_t ADDR_VIEW_W = mh::addr::G_WIN_W; // EN 0x00825058; viewport width = WindowWidth-0xa0
constexpr int       GEOM_MARGIN = 8;                 // px clear of the viewport's right edge and of the frame's top

// The RESERVED WIDTH is measured (llm_gfx_font_measure_text, EN 0x004a2be7, mh_calls.gen.h) from a
// FIXED WORST-CASE TEMPLATE, not from the actual live line. Measuring the live text every frame was
// the obvious move and the wrong one: the ping/CMD digits change every sample, and if the anchor
// tracked the LIVE string's width the label+bar prefix -- the part net_hud's `_only` rect actually
// pins a baseline to -- would shift sideways with the digit count instead of sitting still. A
// worst-case template ("P2 PING [####] 9999 ms": the widest peer-index prefix, the label, a full
// bar, and a 4-digit clamp -- see the srtt/cmd clamps below) measured ONCE (the font never changes
// after install, so the result is cached) keeps that column fixed per resolution while the anchor
// itself still moves with the live screen size, which is the placement this item actually asks for.
bool g_reserved_ready = false;
int  g_reserved_w     = 0;

int reserved_width() {
    if (!g_reserved_ready) {
        wchar_t   w[32];
        const int n  = MultiByteToWideChar(CP_ACP, 0, "P2 PING [####] 9999 ms", -1, w, 32);
        g_reserved_w = n > 0 ? (int)mh::call::llm_gfx_font_measure_text((uint16_t *)w) : 0;
        if (g_reserved_w <= 0) g_reserved_w = 160; // defensive: never anchor off a bogus measurement
        g_reserved_ready = true;
    }
    return g_reserved_w;
}

// ---- L1c: naming a peer whose TRANSPORT id the declared-id convention never learnt ----------------
//
// A client's single connection to the host carries `lat[].player_id == -1` by design
// (net_transport.cpp / udp_endpoint.cpp: "the host's id is unknown and irrelevant to a client"), so
// peer_name() fell back to the placeholder "PLAYER 0" on every client sample line. The STRATEGIC
// PEER_HORIZON array, unlike the transport connection table, is broadcast to every player regardless
// of topology -- it is exactly what net_lockstep.cpp's stall binding (MH_Lockstep_StallBindingPeer,
// declared in the header) already resolves a name from, just only once the sim is blocked. This
// reimplements that SAME movement-based liveness test locally (net_lockstep.cpp is mp:T3's file; the
// getter it already exports answers "who is blocking", not "who is my one peer", so this is a second,
// narrower question rather than a second copy of the controller) so a name can resolve unconditionally.
constexpr int   LIVE_PEERS = 8; // the retail PEER_HORIZON array's width (net_lockstep.cpp LS_LATE_PEERS)
constexpr DWORD LIVE_MS =
    5000; // a horizon unmoved this long is not a peer advertising (net_lockstep.cpp LATE_LIVE_MS)
double g_slot_last_h[LIVE_PEERS]    = {0.0};
DWORD  g_slot_last_move[LIVE_PEERS] = {0};

inline uintptr_t ADDR_PEER_HORIZON() { // double[8]; SB-HOSTFREE -- movable, read through live_base
    return mh::state::live_base(mh::state::RID_NET_PEER_HORIZON);
}

// The strategic slot of the `nth` OTHER live peer (0-based), or -1. On a client -- the ONLY caller
// of this, and the star-topology case this item targets -- there is exactly one transport
// connection, so nth=0 is unambiguous: there is only one other live slot to find. Never called for a
// host's multi-client sample (there, `lat[].player_id` is already correct -- HELLO taught it), so the
// "which connection maps to which slot" question this function does NOT answer never has to be asked.
int live_peer_slot(int nth) {
    const DWORD     now  = GetTickCount();
    const int       me   = MH_Net_LocalPlayerId();
    const uintptr_t base = ADDR_PEER_HORIZON();
    int             seen = 0;
    for (int i = 0; i < LIVE_PEERS; ++i) {
        double h;
        memcpy(&h, (const void *)(base + i * 8u), sizeof(double));
        if (h != g_slot_last_h[i]) {
            g_slot_last_h[i]    = h;
            g_slot_last_move[i] = now ? now : 1;
        }
        if (i == me || h <= 0.0 || g_slot_last_move[i] == 0 || (now - g_slot_last_move[i]) > LIVE_MS)
            continue;
        if (seen == nth) return i;
        ++seen;
    }
    return -1;
}

// ---- knobs (mh_net.ini `[hud]`) -----------------------------------------------------------------
bool     g_enabled     = false;       // [hud] net_indicator      -- armed at all
bool     g_visible     = true;        // ...flipped by the hotkey; the indicator is ON but hidden
bool     g_xy_override = false;       // [hud] net_indicator_xy IS SET -- honor it verbatim instead of L1d geometry
int      g_x           = GEOM_MARGIN; // effective draw position: the override value, or the L1d-computed one
int      g_y           = GEOM_MARGIN;
int      g_font        = 0; // [hud] net_indicator_font  -- 0 = FONTY08, the in-game HUD face
uint16_t g_color       = 0xffffu;
bool     g_log_on      = true; // [hud] net_indicator_log
int      g_key_vk      = 'N';  // [hud] net_indicator_key
bool     g_key_ctrl = true, g_key_alt = true, g_key_shift = false;
bool     g_key_prev = false;

// ---- counters + rate limits ---------------------------------------------------------------------
long g_frames     = 0;
long g_stalls     = 0;
bool g_said_shown = false;
// SEEDED AT ARM, not left at 0. The wrap-safe cadence idiom is `(long)(now - deadline) >= 0`, and it
// only works while the two are within 2^31 ticks of each other: a machine up longer than ~24.8 days
// makes `now - 0` read NEGATIVE and the first sample would then wait for the tick counter to wrap.
DWORD           g_next_sample_tick = 0;
int             g_stall_peer_said  = -1;   // the peer the last emitted stall line named; -1 = not stalled
constexpr DWORD SAMPLE_PERIOD_MS   = 2000; // one `; [netind] peer..` line per peer per 2 s
// A block shorter than this is a normal lockstep beat, not something to shout about: at the shipping
// 100 ms lookahead the sim routinely waits a few ms for the next advertisement. The message is for a
// stall a PLAYER notices, and 400 ms is about where a frozen frame stops reading as frame pacing.
constexpr unsigned long STALL_SHOW_MS = 400;
// ...and it stays up this long after the block clears. NOT a test affordance -- a stall is rarely one
// clean episode: a struggling peer produces a run of short blocks, and a line that appeared and
// vanished with each of them would STROBE, which reads as a rendering fault rather than as
// information. Holding it briefly makes "the match is waiting on <name>" a state the player can
// actually read. Measured on the rig at 200 ms RTT with a 3 s one-directional blackhole: the block
// arrived as TWO episodes 400 ms apart (407 ms, then 437 ms), so without the hold the line would
// have flashed twice.
constexpr DWORD STALL_HOLD_MS          = 1000;
DWORD           g_stall_show_until     = 0; // tick the held line expires; 0 = nothing to show
char            g_stall_shown_name[40] = {0};

long ms_of(uintptr_t a) {
    double d;
    memcpy(&d, (const void *)a, sizeof(double));
    return (long)(d * 1000.0);
}

// ---- the stability bar ---------------------------------------------------------------------------
//
// Four levels from IPDV and LOSS, worst-of. The thresholds are a judgement, not a measurement, and
// they are written as one table so they can be re-judged in one place; what is NOT a judgement is
// the choice of inputs (see the head comment). Level 0 is the honest "nothing measured it" state --
// over the TCP transport, which has no channel B to time, that is the permanent answer, and a full
// bar there would be a lie about a link nobody probed.
int stability_level(bool measured, int ipdv_ms, int loss_pm) {
    if (!measured) return 0;
    if (loss_pm < 0) loss_pm = 0;
    if (ipdv_ms > 40 || loss_pm > 50) return 1; // unplayable-ish: orders will arrive in bursts
    if (ipdv_ms > 15 || loss_pm > 20) return 2; // noticeably uneven
    if (ipdv_ms > 5 || loss_pm > 5) return 3;   // slightly uneven
    return 4;                                   // steady
}

void bar_text(char *out, int level) {
    out[0] = '[';
    for (int i = 0; i < 4; ++i) out[1 + i] = (i < level) ? '#' : '.';
    out[5] = ']';
    out[6] = '\0';
}

// The peer's name as the player saw it in the lobby. `idx` is a PEER_HORIZON / strategic-player
// slot. An empty or unprintable name gives "PLAYER <n>" rather than a blank line -- a stall message
// that names nobody is the anonymous stall this item exists to replace.
void peer_name(int idx, char *out, int cap) {
    out[0] = '\0';
    if (idx >= 0 && idx < 8) {
        const char *n = (const char *)(ADDR_PLAYERS + (unsigned)idx * PLAYER_STRIDE + PLAYER_NAME_OFF);
        if ((unsigned char)n[0] >= 0x20u) {
            lstrcpynA(out, n, cap);
            return;
        }
    }
    wsprintfA(out, "PLAYER %d", idx < 0 ? 0 : idx);
}

void draw_line(int y, const char *ascii) {
    wchar_t   w[96];
    const int n = MultiByteToWideChar(CP_ACP, 0, ascii, -1, w, 96);
    if (n <= 0) return;
    mh::call::llm_gfx_draw_text_blend_clipped(g_x, y, (uint16_t *)w, (int16_t)g_color);
}

// ---- ini parsing ---------------------------------------------------------------------------------
int parse_int(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
    return v * sign;
}

unsigned parse_hex(const char *s) {
    unsigned v = 0;
    for (; *s; ++s) {
        const char c = *s;
        if (c >= '0' && c <= '9') v = v * 16u + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16u + (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16u + (unsigned)(c - 'A' + 10);
        else break;
    }
    return v;
}

// `Ctrl+Alt+N`, `Shift+F9`, `N`, or empty/`none` for no hotkey. Deliberately a SMALL grammar: a
// modifier list plus one letter, digit or function key. The debug overlay carries the general
// version; duplicating it here would be a second parser to keep in step with the first.
void parse_key(const char *s) {
    g_key_ctrl = g_key_alt = g_key_shift = false;
    g_key_vk                             = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return;
    if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'o' || s[1] == 'O')) return; // none
    for (;;) {
        if (_strnicmp(s, "ctrl+", 5) == 0) {
            g_key_ctrl = true;
            s += 5;
        } else if (_strnicmp(s, "alt+", 4) == 0) {
            g_key_alt = true;
            s += 4;
        } else if (_strnicmp(s, "shift+", 6) == 0) {
            g_key_shift = true;
            s += 6;
        } else {
            break;
        }
    }
    if ((s[0] == 'f' || s[0] == 'F') && s[1] >= '1' && s[1] <= '9') {
        const int n = parse_int(s + 1);
        if (n >= 1 && n <= 12) g_key_vk = VK_F1 + n - 1;
        return;
    }
    if ((s[0] >= 'a' && s[0] <= 'z')) g_key_vk = s[0] - 'a' + 'A';
    else if ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9')) g_key_vk = s[0];
}

bool hotkey_edge() {
    if (!g_key_vk) return false;
    const bool down = (GetAsyncKeyState(g_key_vk) & 0x8000) != 0 &&
                      (!g_key_ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) &&
                      (!g_key_alt || (GetAsyncKeyState(VK_MENU) & 0x8000)) &&
                      (!g_key_shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    const bool edge = down && !g_key_prev;
    g_key_prev      = down;
    return edge;
}

void ind_log(const char *fmt, ...) {
    if (!g_log_on) return;
    char    line[288];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    const int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n]     = '\n';
        line[n + 1] = '\0';
    }
    seam_log(line);
}

bool in_live_match() {
    return *(const uint8_t *)ADDR_SESSION_MODE == 3 && MH_Net_IsStarted() && MH_Net_PeerCount() > 0;
}

} // namespace

// ---- install ------------------------------------------------------------------------------------

extern "C" int MH_NetIndicator_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    char ini[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *s = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    s[1] = 0;
    wsprintfA(ini, "%smh_net.ini", exe);

    // DEFAULT ON, with no ini at all. This is a player-facing readout for a mode that cannot be
    // entered by accident -- you are in a network game -- and a connection indicator nobody turns on
    // is a connection indicator nobody has when the connection goes wrong.
    g_enabled = GetPrivateProfileIntA("hud", "net_indicator", 1, ini) != 0;
    if (!g_enabled) return 0;

    char buf[96];
    // L1d: EMPTY default, not "8,44" -- the key's PRESENCE is what makes it an explicit override
    // (Keep `[hud] net_indicator_xy` as an explicit override). Absent, the position is computed by
    // geometry every drawn frame instead (see reserved_width()/ADDR_VIEW_W above); present, it wins
    // verbatim, exactly as before.
    GetPrivateProfileStringA("hud", "net_indicator_xy", "", buf, sizeof(buf), ini);
    g_xy_override = buf[0] != 0;
    if (g_xy_override) {
        char *comma = buf;
        while (*comma && *comma != ',') ++comma;
        if (*comma) {
            *comma = 0;
            g_y    = parse_int(comma + 1);
        }
        g_x = parse_int(buf);
    }
    g_font = GetPrivateProfileIntA("hud", "net_indicator_font", 0, ini);
    if (g_font < 0 || g_font > 6) g_font = 0;
    GetPrivateProfileStringA("hud", "net_indicator_color", "ffff", buf, sizeof(buf), ini);
    g_color  = (uint16_t)parse_hex(buf);
    g_log_on = GetPrivateProfileIntA("hud", "net_indicator_log", 1, ini) != 0;
    GetPrivateProfileStringA("hud", "net_indicator_key", "Ctrl+Alt+N", buf, sizeof(buf), ini);
    parse_key(buf);
    g_visible          = true;
    g_next_sample_tick = GetTickCount();
    return 1;
}

// ---- per-present -------------------------------------------------------------------------------

extern "C" void MH_NetIndicator_OnPresent(void) {
    // SELF-ARMING, on the first present rather than from MH_Core_Arm. Two reasons, and neither is
    // laziness: (a) this seam installs no hook, so it has nothing to contribute to the arm-log
    // SEQUENCE that tools/check_arm_order.py diffs against a committed template -- adding a line
    // there would be a gate change dressed up as a feature; (b) the first present is off the loader
    // lock, which is where every other "read some ini and decide" step in this file already lives
    // (ensure_key_once, net_lockstep.cpp).
    static bool armed = false;
    if (!armed) {
        armed = true;
        MH_NetIndicator_Install();
    }
    if (!g_enabled) return;
    // SOLO DRAWS NOTHING, and it is this branch rather than a hidden element: a single-player game
    // is not session mode 3 and has no started transport.
    if (!in_live_match()) {
        g_stall_peer_said  = -1;
        g_stall_show_until = 0; // a match that ENDED is not a match that is waiting for anyone
        return;
    }
    if (hotkey_edge()) g_visible = !g_visible;

    // The stall message is a MEASUREMENT, not a drawing: keep taking it while hidden so the log (and
    // a player's bug report) still carries who the match was waiting for.
    unsigned long blocked_ms = 0;
    const int     bind       = MH_Lockstep_StallBindingPeer(&blocked_ms);
    const bool    stalled    = (bind >= 0 && blocked_ms >= STALL_SHOW_MS);
    char          bind_name[40];
    bind_name[0] = '\0';
    if (stalled) peer_name(bind, bind_name, sizeof(bind_name));

    if (stalled) {
        // The DISPLAY is held (see STALL_HOLD_MS); the LOG is not. A log line per episode edge is
        // what a reader afterwards needs -- how many blocks there were and how long each lasted --
        // and smoothing that would destroy the very structure the hold exists to hide from the eye.
        lstrcpynA(g_stall_shown_name, bind_name, sizeof(g_stall_shown_name));
        const DWORD now    = GetTickCount();
        g_stall_show_until = (now + STALL_HOLD_MS) | 1u; // never 0: 0 is the "nothing held" sentinel
    }
    if (stalled && bind != g_stall_peer_said) {
        g_stall_peer_said = bind;
        ++g_stalls;
        ind_log("; [netind] stall waiting_for=%s peer=%d ms=%lu", bind_name, bind, blocked_ms);
    } else if (!stalled && g_stall_peer_said >= 0) {
        ind_log("; [netind] stall_end peer=%d", g_stall_peer_said);
        g_stall_peer_said = -1;
    }
    const bool show_stall =
        g_stall_show_until != 0 && (long)(GetTickCount() - g_stall_show_until) < 0;
    if (!show_stall) g_stall_show_until = 0;

    // COMMAND LATENCY -- see the head comment. Both terms are read live, so the adaptive
    // controller's moves show up here without this file knowing the controller exists.
    long look_ms = ms_of(ADDR_STEP_SIZE);
    long step_ms = ms_of(ADDR_SIM_STEP_INT());
    if (look_ms < 0) look_ms = 0;
    if (step_ms < 0) step_ms = 0;
    long cmd_ms = look_ms + step_ms;
    if (cmd_ms > 9999) cmd_ms = 9999;

    MH_NetStats st;
    MH_Net_GetStats(&st);
    const int npeer = (st.lat_supported && st.lat_count > 0)
                          ? (st.lat_count > 3 ? 3 : st.lat_count)
                          : 1; // unmeasurable transport: one line that says so

    const bool draw = g_visible && *(const uint8_t *const *)ADDR_FONT_DATA_PTRS != nullptr &&
                      ((const uint8_t *const *)ADDR_FONT_DATA_PTRS)[g_font] != nullptr;
    int line_h = 10;
    if (draw) {
        mh::call::llm_gfx_font_select(g_font);
        const int32_t h = mh::call::llm_gfx_font_get_line_height();
        if (h > 0 && h < 64) line_h = (int)h;
        ++g_frames;
        if (!g_xy_override) {
            // L1d: from the LIVE screen size every drawn frame (resolution can change at runtime --
            // gfx_overlay.cpp's ADDR_W/H comment documents the same re-read-per-frame posture for
            // these globals), not baked in once at install.
            const int32_t view_w = *(const int32_t *)ADDR_VIEW_W;
            g_x                  = (int)view_w - GEOM_MARGIN - reserved_width();
            if (g_x < GEOM_MARGIN) g_x = GEOM_MARGIN; // defensive: a viewport too narrow to reserve for
            g_y = GEOM_MARGIN;
        }
    }

    const bool sample = g_log_on && (long)(GetTickCount() - g_next_sample_tick) >= 0;
    if (sample) g_next_sample_tick = GetTickCount() + SAMPLE_PERIOD_MS;

    int y = g_y;
    for (int i = 0; i < npeer; ++i) {
        const bool meas =
            st.lat_supported != 0 && i < st.lat_count && st.lat[i].samples > 0;
        const int srtt_ms = meas ? (st.lat[i].srtt_us + 500) / 1000 : -1;
        const int ipdv_ms = meas ? (st.lat[i].ipdv_us + 500) / 1000 : -1;
        const int loss_pm = (st.lat_supported != 0 && i < st.lat_count) ? st.lat[i].loss_pm : -1;
        const int level   = stability_level(meas, ipdv_ms < 0 ? 0 : ipdv_ms, loss_pm);
        char      bar[8];
        bar_text(bar, level);

        // THE VARYING NUMBER GOES LAST, and that is a test affordance rather than typography. The
        // ping digits differ on every run, so a committed capture baseline has to mask them -- and
        // the game's fonts are PROPORTIONAL, so a number padded to a fixed character width still
        // moves everything after it by the difference between a space and a digit. Putting the
        // digits at the end of the line means the mask is a rectangle running to the right edge and
        // the label and the bar stay strictly compared, which is where the regression would be.
        // `n/a` rather than 0 is what an unmeasurable transport prints: a 0 ms ping would read as a
        // perfect link, which is the one answer worse than no answer.
        char msrtt[12];
        if (srtt_ms < 0) lstrcpyA(msrtt, "n/a");
        else wsprintfA(msrtt, "%d ms", srtt_ms > 9999 ? 9999 : srtt_ms);

        char line[96];
        if (npeer > 1) wsprintfA(line, "P%d PING %s %s", i, bar, msrtt);
        else wsprintfA(line, "PING %s %s", bar, msrtt);
        if (draw) draw_line(y, line);
        y += line_h;

        if (sample) {
            int pid = (st.lat_supported && i < st.lat_count) ? st.lat[i].player_id : -1;
            // L1c: a client's one connection to the host never learns a transport player_id (the
            // declared-id convention -- see live_peer_slot()'s head comment). Restricted to npeer==1
            // (the client's single-connection case): with more than one lat[] entry (a host with
            // multiple clients) the transport id is already correct, and "which connection is which
            // slot" would be genuinely ambiguous for this simple an nth-live-slot lookup.
            if (pid < 0 && npeer == 1) pid = live_peer_slot(0);
            char nm[40];
            peer_name(pid, nm, sizeof(nm));
            ind_log("; [netind] peer%d name=%s srtt_ms=%d ipdv_ms=%d loss_pm=%d bar=%d cmd_ms=%d "
                    "look_ms=%d step_ms=%d",
                    i, nm, srtt_ms, ipdv_ms, loss_pm, level, (int)cmd_ms, (int)look_ms, (int)step_ms);
        }
    }

    if (draw) {
        char line[64];
        wsprintfA(line, "CMD %d ms", (int)cmd_ms);
        draw_line(y, line);
        y += line_h;
        if (show_stall) {
            char line2[80];
            wsprintfA(line2, "WAITING FOR %s", g_stall_shown_name);
            draw_line(y, line2);
        }
        if (!g_said_shown) {
            g_said_shown = true;
            // The arm report, deliberately emitted HERE and not at install time: this seam installs
            // no hook, so it has no place in the arm-log sequence the arm-order gate diffs, and a
            // line saying "armed" would only claim the ini was read. A line written from inside the
            // first drawn frame claims the thing worth claiming -- that it is on a player's screen.
            ind_log("; [netind] shown at %d,%d font=%d peers=%d lat_supported=%d", g_x, g_y, g_font,
                    MH_Net_PeerCount(), st.lat_supported);
        }
    }
}

extern "C" void MH_NetIndicator_Stats(long *frames_drawn, long *stalls_named) {
    if (frames_drawn) *frames_drawn = g_frames;
    if (stalls_named) *stalls_named = g_stalls;
}
