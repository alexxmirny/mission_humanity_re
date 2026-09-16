//
// ---- U16: show the "<name> joined/left" line on EVERY lobby peer, not just the host --------------
//
// The host broadcasts a FLAG_ANNOUNCE {is_join, affected player id, name[..]} on each JOIN/LEFT
// delta (launch.cpp mp_sync). The net side's recv handler decides whether the announce concerns us
// -- that is a player-id question and stays there -- and POSTs the rest here.
//
// TWO THREADS, one queue, and that is the reason this is a ring rather than a direct call. The post
// runs on the RECV thread, so it only enqueues; announce_drain() runs on the main/UI thread (from
// the lobby-dispatch seam) and renders each queued line via the retail llm_lobby_announce_line --
// the exact retail announce path, with zero slot/type-dispatch involvement, so it cannot perturb
// slots or determinism. Decoupled from the retail 0x13/0x14 unicast (which clients ignore) by
// design; the host still announces locally via the retail admin loop. (U16)
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/ui_internal.h"
#include "addr/mh_addrs.gen.h" // generated EN VAs (tools/gen_dll_addrs.py)
#include "hook/watcall.h"      // call_watcall3 (Watcom register bridge)

namespace {

struct PendingAnnounce {
    char          name[32];
    unsigned char is_join;
};

CRITICAL_SECTION g_ann_cs;
bool             g_ann_cs_init = false;
PendingAnnounce  g_ann_ring[8];
volatile int     g_ann_head = 0, g_ann_tail = 0; // ring: head = next write, tail = next read

void ann_cs_ensure() {
    if (!g_ann_cs_init) {
        InitializeCriticalSection(&g_ann_cs);
        g_ann_cs_init = true;
    }
}

} // namespace

namespace mh {
namespace ui {

// recv thread: enqueue the announce (drop if the small ring is full -- announces are rare).
void announce_post(const char *name, int is_join) {
    if (!name) return;
    ann_cs_ensure();
    EnterCriticalSection(&g_ann_cs);
    int nxt = (g_ann_head + 1) & 7;
    if (nxt != g_ann_tail) { // not full
        g_ann_ring[g_ann_head].is_join = (unsigned char)is_join;
        lstrcpynA(g_ann_ring[g_ann_head].name, name, 32); // NUL-guaranteed
        g_ann_head = nxt;
    }
    LeaveCriticalSection(&g_ann_cs);
}

// main/UI thread (per lobby frame): render each queued announce via the retail lobby-log path.
void announce_drain() {
    if (!g_ann_cs_init) return;
    for (;;) {
        PendingAnnounce e;
        EnterCriticalSection(&g_ann_cs);
        bool have = (g_ann_tail != g_ann_head);
        if (have) {
            e          = g_ann_ring[g_ann_tail];
            g_ann_tail = (g_ann_tail + 1) & 7;
        }
        LeaveCriticalSection(&g_ann_cs);
        if (!have) break;
        void *verb = (void *)(e.is_join ? mh::addr::lobby_announce_verb_joined
                                        : mh::addr::lobby_announce_verb_left);
        mh::hook::call_watcall3(mh::addr::llm_lobby_announce_line, e.name,
                                (void *)mh::addr::lobby_announce_fmt, verb);
    }
}

} // namespace ui
} // namespace mh
