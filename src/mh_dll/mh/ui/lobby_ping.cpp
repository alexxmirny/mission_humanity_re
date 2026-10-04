//
// mh/ui/lobby_ping.cpp -- mp:L1b: the lobby's per-slot SRTT column.
//
// L1 shipped only the IN-GAME indicator (seams/ui_net_indicator.cpp). The lobby's slot rows are
// retail's OWN retained-mode widgets, redrawn from scratch every frame by retail's widget-list draw
// pass, which runs strictly AFTER the per-frame lobby-dispatch callback this file is ticked from
// (net_seams.cpp on_lobby_dispatch -> llm_lobby_host_net_dispatch has no draw or present call). So a
// pixel blit issued FROM the lobby tick would sit UNDER that frame's own widget draw and never be
// seen. The technique used here (and by lobby_notice.cpp's status line) is to set a VALUE that
// retail's draw machinery reads every frame and let retail repaint it: whatever this tick last
// wrote is what the next frame draws.
//
// THE PING WIDGET (mp:U50). Each slot row has its own PING widget -- the sixth widget of the row,
// built by the DLL's replacement of llm_lobby_build_slot_widgets (ui/lobby_widgets.cpp), a plain
// label widget (llm_ui_widget_draw, no frame, no action) laid out right of the colour flag sprite
// and left of the panel's inner border (x=349). This file only keeps its label pointer (widget
// +0x38) current: a non-NULL wide string is drawn, NULL draws nothing. It replaces the earlier
// mechanism (mp:L1b/L1e), which borrowed the COLOR widget's optional secondary label and widened
// its width every tick to clear the flag; the color widget is no longer touched.
//
// FORMAT (mp:L1e, the user's ruling 2026-09-22). "R <ms>" (relayed) / "D <ms>" (direct), no " ms"
// suffix; RELAY vs DIRECT UNKNOWN renders as the bare number rather than a guessed letter (see
// MH_NetPeerLatency.relayed in mh_net_export.h). A row with no measurement gets NO label at all,
// not "n/a": the TCP module carries no latency stats and a module=none lobby has no local id, so
// "n/a" would sit on every loopback lobby row and every pinned lobby baseline.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

#include "ui/ui_internal.h"
#include "ui/lobby_ping.h"
#include "ui/lobby_ui.h"               // lobby_slot_ping_widget
#include "addr/mh_addrs.gen.h"         // generated EN VAs (tools/gen_dll_addrs.py)
#include "include/mh_net_export.h"     // MH_Net_GetStats / MH_Net_LocalPlayerId / MH_NET_MAX_PEERS
#include "include/mh_seam_export.h"    // MH_Seam_ClientDialIsRelayed (mp:L1e, the client's own-dial R/D)
#include "mh_net_proto/session_info.h" // mp:L1f -- announce_ping_decode, the host's published table

#pragma comment(lib, "user32.lib") // wsprintfW / lstrcpyW

using mh::ui::detail::ui_log;

namespace {

constexpr uintptr_t ADDR_SLOTS = mh::addr::_G_LLM_LOBBY_SLOTS;         // llm_lobby_player_slot[8], stride 0x39
constexpr uintptr_t ADDR_BUILT = mh::addr::_G_LLM_LOBBY_WIDGETS_BUILT; // 0 until the slot rows exist

constexpr unsigned SLOT_STRIDE      = 0x39u;
constexpr unsigned OFF_PLAYERID     = 0x01u; // llm_lobby_player_slot.player_id
constexpr unsigned OFF_STATUS       = 0x0bu; // llm_lobby_player_slot.slot_status (llm_lobby_slot_status)
constexpr uint8_t  STATUS_HUMAN     = 1u;    // OPEN=0, HUMAN=1, AI=2, CLOSED=3 (docs/structs.md)
constexpr int      MAX_SLOTS        = 8;
constexpr unsigned OFF_LABEL        = 0x38u; // llm_ui_widget.label (wchar_t*; 0 = none)
constexpr DWORD    SAMPLE_PERIOD_MS = 2000;  // one log line per refresh pass per 2s, like ui_net_indicator

wchar_t g_label[MAX_SLOTS][16]; // one persistent buffer per slot; the widget only keeps the pointer
DWORD   g_next_sample_tick = 0;

// ---- mp:L1f -- THE HOST'S PUBLISHED TABLE, as a client holds it --------------------------------
//
// ONE ALIGNED LONG PER PLAYER ID, and that is the whole of the thread safety. The summary arrives
// on the RECEIVE thread (net_seams.cpp on_announce_recv) and is rendered on the MAIN thread, so
// something has to bridge them; a critical section on this path would be a lock taken from a
// datagram handler for a lobby decoration, and a multi-field struct would tear. A single 32-bit
// scalar per row, written with InterlockedExchange and read with InterlockedCompareExchange, cannot
// tear and needs no lock on either side. Packed:
//
//     bit 0      1 = this row carries a published measurement, 0 = nothing (or cleared)
//     bits 1-2   the relay class as the wire spells it (0 direct, 1 relayed, 2 unknown)
//     bits 8-23  srtt_ms, already clamped by the encoder to ANNOUNCE_PING_SRTT_CLAMP (9999)
//
// A summary REPLACES the table rather than merging into it: every id the new one does not mention
// is cleared, so a peer that left stops showing a frozen number on everyone else's screen the next
// time the host speaks.
volatile LONG g_pub[MAX_SLOTS];
volatile LONG g_pub_at; // GetTickCount of the last summary that arrived; 0 = none this session

// mp:L1f -- how many rows the LAST tick actually painted a number on. Read by the `lobbyping`
// script predicate (seams/ui_drive.cpp) so a scenario waits for the mechanism rather than for a
// frame budget. Written once per tick on the main thread, read from the same thread; volatile
// rather than plain only because the reader is in another translation unit.
volatile LONG g_measured_rows;

// HOW LONG A PUBLISHED NUMBER IS WORTH SHOWING. The host publishes at ~1 Hz, so five missed
// summaries is a host that has stopped talking to us (it left, the link died, or it is running a
// module with no latency stats at all) -- and a stale ping is worse than no ping, because it looks
// live. After this the rows the client cannot measure go blank again, which is exactly the state
// they were in before this item.
constexpr DWORD PUB_STALE_MS = 5000;

constexpr LONG PUB_PRESENT  = 1;
constexpr int  PUB_RELAY_SH = 1;
constexpr LONG PUB_RELAY_MS = 3;
constexpr int  PUB_SRTT_SH  = 8;
constexpr LONG PUB_SRTT_MS  = 0xffff;

// The wire's relay class (0/1/2) -> MH_NetPeerLatency.relayed's spelling (0/1/-1), which is what the
// renderer below already speaks. Two spellings because the wire has no room for a negative and the
// struct has no reason to invent a third positive value.
int relay_from_wire(LONG w) {
    if (w == mh_net_proto::ANNOUNCE_PING_RELAY_DIRECT) return 0;
    if (w == mh_net_proto::ANNOUNCE_PING_RELAY_RELAYED) return 1;
    return -1;
}

// Read the published row for `pid`. False when nothing is published for it, or when the whole table
// has aged out. Safe from the main thread against a concurrent publish: one LONG, one read.
bool published_for(int pid, int &srtt_ms, int &relay) {
    if (pid < 0 || pid >= MAX_SLOTS) return false;
    const LONG at = InterlockedCompareExchange(&g_pub_at, 0, 0);
    if (at == 0 || (DWORD)(GetTickCount() - (DWORD)at) > PUB_STALE_MS) return false;
    const LONG v = InterlockedCompareExchange(&g_pub[pid], 0, 0);
    if ((v & PUB_PRESENT) == 0) return false;
    srtt_ms = (int)((v >> PUB_SRTT_SH) & PUB_SRTT_MS);
    relay   = relay_from_wire((v >> PUB_RELAY_SH) & PUB_RELAY_MS);
    return true;
}

// Set slot's PING label (nullptr clears). A row that was never built has no widget: nothing to do.
void set_slot_label(int slot, const wchar_t *text) {
    void *w = mh::ui::lobby_slot_ping_widget(slot);
    if (w == nullptr) return;
    *(const wchar_t **)((uintptr_t)w + OFF_LABEL) = text;
}

} // namespace

namespace mh {
namespace ui {

// mp:L1f -- see lobby_ping.h. RECEIVE THREAD.
void lobby_ping_on_published(const unsigned char *buf, int len) {
    mh_net_proto::AnnouncePingEntry e[mh_net_proto::ANNOUNCE_PING_MAX_ENTRIES];
    size_t                          n = 0;
    // A malformed or truncated summary changes NOTHING -- not even the arrival stamp. Letting a bad
    // frame refresh the clock would keep a dead table alive past PUB_STALE_MS.
    if (!mh_net_proto::announce_ping_decode(buf, (size_t)(len < 0 ? 0 : len), e,
                                            mh_net_proto::ANNOUNCE_PING_MAX_ENTRIES, &n))
        return;
    LONG next[MAX_SLOTS];
    for (int i = 0; i < MAX_SLOTS; ++i) next[i] = 0; // REPLACE, never merge -- see g_pub's comment
    for (size_t i = 0; i < n; ++i) {
        const int pid = (int)e[i].player_id;
        if (pid < 0 || pid >= MAX_SLOTS) continue;
        LONG v = PUB_PRESENT;
        v |= ((LONG)e[i].relay & PUB_RELAY_MS) << PUB_RELAY_SH;
        v |= ((LONG)e[i].srtt_ms & PUB_SRTT_MS) << PUB_SRTT_SH;
        next[pid] = v;
    }
    for (int i = 0; i < MAX_SLOTS; ++i) InterlockedExchange(&g_pub[i], next[i]);
    // The stamp LAST: a reader that gets between the rows and the clock sees the PREVIOUS stamp
    // with the new rows, which is at worst one publish period pessimistic about their age -- the
    // opposite order would let it see the new stamp over the old rows, which is optimistic and
    // therefore the wrong way round for a staleness test.
    InterlockedExchange(&g_pub_at, (LONG)GetTickCount());
}

// mp:P16 -- see lobby_ping.h.
bool lobby_ping_published_srtt(int pid, unsigned max_age_ms, int *srtt_ms) {
    if (!srtt_ms || pid < 0 || pid >= MAX_SLOTS) return false;
    const LONG at = InterlockedCompareExchange(&g_pub_at, 0, 0);
    if (at == 0 || (DWORD)(GetTickCount() - (DWORD)at) > (DWORD)max_age_ms) return false;
    const LONG v = InterlockedCompareExchange(&g_pub[pid], 0, 0);
    if ((v & PUB_PRESENT) == 0) return false;
    *srtt_ms = (int)((v >> PUB_SRTT_SH) & PUB_SRTT_MS);
    return true;
}

void lobby_ping_tick() {
    // Screen gate: only the lobby owns these widget records at all (the browsers/map-picker share
    // other parts of this same widget family, but never this array).
    if (*(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != (void *)mh::addr::lobby_widget_origin) return;
    if (*(const int *)ADDR_BUILT == 0) return; // rows not (yet) built this lobby visit

    MH_NetStats st;
    MH_Net_GetStats(&st);
    const int  local_id = MH_Net_LocalPlayerId();
    const bool am_host  = local_id == 0; // llm_lobby_player_slot doc: "Host = 0"

    const DWORD now    = GetTickCount();
    const bool  sample = (long)(now - g_next_sample_tick) >= 0;
    if (sample) g_next_sample_tick = now + SAMPLE_PERIOD_MS;
    int painted = 0; // mp:L1f -- rows this tick put a real number on; published to g_measured_rows

    for (int slot = 0; slot < MAX_SLOTS; ++slot) {
        const uintptr_t rec    = ADDR_SLOTS + (uintptr_t)slot * SLOT_STRIDE;
        const uint8_t   status = *(const uint8_t *)(rec + OFF_STATUS);
        const int       pid    = *(const int *)(rec + OFF_PLAYERID);
        if (status != STATUS_HUMAN || pid == local_id) {
            set_slot_label(slot, nullptr); // clear stale text: slot changed kind, or it is our own row
            continue;
        }
        bool measured        = false;
        int  srtt_ms         = -1;
        int  relay           = -1;    // mp:L1e: -1 UNKNOWN, 0 direct, 1 relayed -- see mh_net_export.h
        bool from_host_table = false; // mp:L1f: this row is the HOST's number, not our own
        if (st.lat_supported) {
            if (am_host) {
                // Host: one lat[] entry per connected client, with a real (HELLO-taught) player_id.
                for (int j = 0; j < st.lat_count && j < MH_NET_MAX_PEERS; ++j) {
                    if (st.lat[j].player_id == pid && st.lat[j].samples > 0) {
                        srtt_ms = (st.lat[j].srtt_us + 500) / 1000;
                        relay   = st.lat[j].relayed; // mp:L1f wired this on the UDP module (the
                                                     // relay's per-remote latch, keyed by the
                                                     // loopback address the two layers already
                                                     // share); -1 still means "nobody can say"
                                                     // and still renders as a bare number.
                        measured = true;
                        break;
                    }
                }
            } else if (pid == 0 && st.lat_count > 0 && st.lat[0].samples > 0) {
                // Client: the ONE transport connection is to the host (lobby slot pid 0), and the
                // declared-id convention leaves lat[0].player_id == -1 on that connection
                // (mh_net_export.h) -- so this is the only slot a client can ever measure, matched
                // positionally, the same fallback ui_net_indicator.cpp uses for its own npeer==1 case.
                srtt_ms = (st.lat[0].srtt_us + 500) / 1000;
                // mp:L1h (2026-09-26) -- THE LIVE PATH FIRST, the dial only as a fallback. L1e read
                // the mp:R7a kick-site latch here because the transport field was still unwired; L1f
                // wired it (the tunnel's per-remote class, keyed by the loopback port a client's one
                // conn also uses) but this arm kept the latch. The latch says how we DIALLED, and a
                // relay-directory join that the punch then promotes to direct kept showing "R" while
                // the host's screen showed "D" for the same link (rc3 field lobby: client
                // `relayed=1` for the whole lobby, `udp path DIRECT` 12 s before its first sample).
                relay = st.lat[0].relayed;
                if (relay < 0) relay = MH_Seam_ClientDialIsRelayed() ? 1 : 0;
                measured = true;
            }
        }
        // mp:L1f -- EVERY OTHER ROW ON A CLIENT'S SCREEN. The transport is a client-server star, so
        // a client's lat[] holds exactly one row (the host's, handled above) and it can never
        // measure another client -- those rows were blank on its screen, always, by construction.
        // The host measures them all and publishes them (session_info.h ANNOUNCE_PING); this is
        // where a client renders what it cannot measure.
        //
        // OUR OWN MEASUREMENT ALWAYS WINS where we have one -- `!measured` guards this whole arm --
        // because the client's own RTT to the host is the truthful number for that row, while the
        // published one would be the host's view of a link we are an endpoint of. The host never
        // publishes its own row for the same reason (see the wire block), so the two never collide;
        // the guard is belt and braces against a host that someday does.
        //
        // NO SECOND LETTER AND NO WIDER CELL (the user's ruling, mp:L1e): a published row renders
        // exactly like a measured one. The provenance is carried in the LOG line below -- which is
        // free, is where check_lobby_ping.py reads it, and costs the cell no pixels.
        if (!measured && !am_host) {
            int psrtt = -1, prelay = -1;
            if (published_for(pid, psrtt, prelay)) {
                srtt_ms         = psrtt;
                relay           = prelay;
                measured        = true;
                from_host_table = true;
            }
        }
        // No measurement -> NO cell, not "n/a": the TCP module carries no latency stats and a
        // module=none lobby has no local id at all, so "n/a" would sit on every loopback lobby row
        // and every pinned lobby baseline (four suite rows went red on a 12x7 px "n/a" at the
        // first gate, 2026-09-22). Only a real number earns pixels.
        if (!measured) {
            set_slot_label(slot, nullptr);
        } else {
            const int clamped = srtt_ms > 9999 ? 9999 : srtt_ms; // must not overflow the row either
            wchar_t  *buf     = g_label[slot];
            // mp:L1e format ("R <n>" / "D <n>", no " ms" -- see the file header block for the pixel
            // measurement behind this choice). relay < 0 (UNKNOWN) degrades to the bare number: no
            // letter, no space, rather than a guessed one.
            if (relay == 1) {
                wsprintfW(buf, L"R %d", clamped);
            } else if (relay == 0) {
                wsprintfW(buf, L"D %d", clamped);
            } else {
                wsprintfW(buf, L"%d", clamped);
            }
            set_slot_label(slot, buf);
            ++painted;
        }
        if (sample) {
            // mp:L1f -- `src` is the provenance the CELL deliberately does not show: `self` = this
            // peer measured it on its own connection, `host` = the host published it because this
            // peer physically cannot measure that link (the star topology), `none` = nothing to
            // show. It is the field check_lobby_ping.py asserts the L1f claim on.
            char b[144];
            wsprintfA(b, "; [lobbyping] slot=%d pid=%d measured=%d srtt_ms=%d relayed=%d src=%s\n",
                      slot, pid, (int)measured, srtt_ms, measured ? relay : -2,
                      !measured ? "none" : (from_host_table ? "host" : "self"));
            ui_log(b);
        }
    }
    InterlockedExchange(&g_measured_rows, (LONG)painted);
}

// mp:L1f -- see lobby_ping.h.
int lobby_ping_measured_rows() { return (int)InterlockedCompareExchange(&g_measured_rows, 0, 0); }

} // namespace ui
} // namespace mh
