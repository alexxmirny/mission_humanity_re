//
// Loopback self-test for the MP transport (Phase A0).
// Proves two PROCESSES on one box connect and exchange a framed, sender-tagged datagram through the
// TCP star (host relays). This is the A0 milestone: "prove two processes on one box connect."
//
//   net_selftest.exe selftest [port]  -> spawns itself as host + client, checks a PING/PONG round-trip
//   net_selftest.exe linktest [port]  -> R-live: a connected-but-SILENT peer must be dropped (and a
//                                        quiet one must not be dropped early)
//   net_selftest.exe callstest        -> P0-CALLS: every generated __watcall marshalling thunk
//                                        (mh_calls.gen.cpp) delivers its args to the right register
//                                        / stack offset and returns correctly. No game needed.
//   net_selftest.exe exportstest      -> P0-EXPORT: every generated ENTRY thunk (mh_export.gen.h)
//                                        unmarshals a Watcom-style call into a C++ body and honours
//                                        the caller's stack-cleanup contract. No game needed.
//   net_selftest.exe orderstest       -> O2: the reimplemented order container's logic, including
//   net_selftest.exe issuetest        -> O4-0: the order-ISSUE wrappers, the layer ABOVE the
//                                        container -- each reimplemented wrapper is driven with
//                                        concrete inputs and what it handed the container is
//                                        compared against a golden extracted from the ORIGINAL's
//                                        disassembly. No game needed.
//   net_selftest.exe interlocktest    -> C1/C4: the patch/seam interlock decision + entry ownership
//   net_selftest.exe bindtest         -> SB-BIND T1: the host binds the region registry through
//                                        libmh_bind_regions. Stock bind = provable no-op; the
//                                        teeth are a relocated region where binding `size`
//                                        instead of `reach` nulls an overrunning save block.
//   net_selftest.exe statetest        -> ST2: a state region is REBASED and every consumer follows
//                                        -- mutation-checked both ways (the relocated copy changes
//                                        the hash, the stale .bss VA does not). No game needed.
//   net_selftest.exe simtest          -> SIM0: the strategic sim's logic over heap buffers, the
//                                        aitest of the 307-function sim migration. Reaches the
//                                        branches a rig cannot stage on demand (an unterminated
//                                        cost list, a bit index of 32, a zero-member group).
//                                        the OVERFLOW branches that no rig scenario can reach.
//   net_selftest.exe udpstatstest     -> mp:T3: the latency arithmetic (RFC 6298 SRTT/RTTVAR,
//                                        IPDV, the 256-packet loss window, the arrival-lateness
//                                        percentiles and the adaptive-lookahead decision). No
//                                        socket, no rig -- see the suite for why it exists.
//   net_selftest.exe host   <port>    -> listen as player 0, echo the first datagram back as PONG
//   net_selftest.exe client <port>    -> connect to 127.0.0.1, send PING, expect PONG
//
// Exit code 0 = pass. The `selftest` orchestrator returns 0 only if both children exit 0.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_addr, in the authtest's raw probe
#include <winsock2.h>                   // before windows.h: the authtest opens a raw socket
#include <ws2tcpip.h>
#include <windows.h>
#include <new> // placement-new, for udprelinktest's endpoints
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../mh_net_udp/udp_endpoint.h"     // mp:T1b -- the UDP core, for udprelinktest (see there)
#include "../mh_net_udp/lookahead_start.h"  // mp:P14 -- the seeded start, udpstatstest (n)
#include "../mh_net_udp/udp_ping_cadence.h" // mp:P15 -- the warm-up cadence, udpstatstest (o)
#include "../mh/seams/adaptive_window.h"    // mp:P15 wave 7 -- first-window start + post-spin starved, (p)
#include "mh_net_export.h"
#include "mh_seam_export.h"
#include "selftest_dispatch.h"                // F5I: the suite table mechanism, shared with libmh_test
#include "hostapi_selftest_support.h"         // LIB-ABI: the selftest host table bound in main()
#include "../libmh/state/host_api.h"          // LIB-ABI: libmh_set_host_api
#include "../mh/include/mh_libmh_hook_bind.h" // F4D-PRE: MH_LibMH_BindHookApi
#include "../mh/addr/mh_rebind.gen.h"         // LIB-REBIND R11: load_gates(nullptr) in main()
#include "../mh/addr/mh_addrs.gen.h"          // TL-TEST1: the fixed VAs the seamtest tripwire watches
#include "mh_mpmenu_export.h"
#include "mh_launch_export.h"

#define SEAM_RX_SIZE 0x3f8

// ---- menu-restore surgery test: verify MH_Menu_Apply splices an MP button into a mock widget array
//      (proves the array/clone logic without the game; the real hook + fixed VAs are live-only).
static int run_menutest() {
    // RETIRED at tracker U18 (2026-07-24): this validated the OLD self-render splice (clone cur[0] ->
    // append a 7th text+frame button) against a mock array. U18 replaced it with a game-coupled restore
    // of the real 7-button MENUBCK2 menu (reads live static widget records + calls GetMenuFile), verified
    // LIVE (hand-clicked menu + 2-machine game start), not in this off-target harness.
    printf("=== menutest RETIRED (U18: menu restore is now live-only) ===\n");
    return 0;
#if 0 // -- preserved for reference; the self-render mechanism it tested no longer exists --
    const int            NB = 6;
    static unsigned char widgets[NB][0x30];
    static void         *arr[NB + 2];
    static void         *container; // the container's "+0x00" array-pointer field
    memset(widgets, 0, sizeof(widgets));
    for (int i = 0; i < NB; ++i) {
        *(unsigned int *)(widgets[i] + 0x08) = 0x100;                // flags
        *(unsigned int *)(widgets[i] + 0x0c) = 0x004c0bfc;           // shared tag (must be cloned)
        *(unsigned int *)(widgets[i] + 0x04) = 0x004b7000 + i;       // distinct callback
        *(int *)(widgets[i] + 0x14)          = (i == 0 ? 8 : 9 + i); // ids 8,10,11,12,13,14 (gap at 9)
        *(int *)(widgets[i] + 0x20)          = 200;                  // X
        *(int *)(widgets[i] + 0x28)          = 30;                   // height
        *(int *)(widgets[i] + 0x2c)          = 100 + i * 30;         // Y: 100..250
        arr[i]                               = widgets[i];
    }
    arr[NB]   = nullptr;
    container = arr;
    MH_Menu_SetContainer(&container);
    MH_Menu_SetGeometry(MH_MENU_AUTO, MH_MENU_AUTO);
    MH_Menu_Apply();

    void **na = (void **)container;
    int    n  = 0;
    while (na[n]) ++n;
    int ok = 1;
    if (n != NB + 1) {
        printf("[menutest] FAIL count=%d expected %d\n", n, NB + 1);
        ok = 0;
    }
    unsigned char *mp = (unsigned char *)na[NB];
    if (!mp) {
        printf("[menutest] FAIL no MP widget\n");
        return 1;
    }
    if (*(unsigned int *)(mp + 0x04) != 0x004bd498) {
        printf("[menutest] FAIL cb=0x%08X\n", *(unsigned int *)(mp + 0x04));
        ok = 0;
    }
    if (*(int *)(mp + 0x14) != 9) {
        printf("[menutest] FAIL id=%d\n", *(int *)(mp + 0x14));
        ok = 0;
    }
    if (*(unsigned int *)(mp + 0x0c) != 0x004c0bfc) {
        printf("[menutest] FAIL tag not cloned\n");
        ok = 0;
    }
    if (*(void **)(mp + 0x00) != nullptr) {
        printf("[menutest] FAIL screen-swap not zeroed\n");
        ok = 0;
    }
    if (*(int *)(mp + 0x20) != 200) {
        printf("[menutest] FAIL x=%d\n", *(int *)(mp + 0x20));
        ok = 0;
    }
    if (*(int *)(mp + 0x2c) != 250 + 30) {
        printf("[menutest] FAIL y=%d expected 280\n", *(int *)(mp + 0x2c));
        ok = 0;
    }

    MH_Menu_Apply(); // idempotency: second apply must be a no-op
    void **na2 = (void **)container;
    int    n2  = 0;
    while (na2[n2]) ++n2;
    if (n2 != NB + 1) {
        printf("[menutest] FAIL not idempotent: %d\n", n2);
        ok = 0;
    }

    printf("[menutest] extended %d->%d buttons; MP widget cb=0x004bd498 id=9 x=%d y=%d (tag cloned, idempotent)\n",
           NB, n, *(int *)(mp + 0x20), *(int *)(mp + 0x2c));
    if (ok) {
        printf("=== PASS: MP menu button spliced into the widget array ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
#endif
}

// ---- D17 launch-to-state CLI parser test: prove MH_Launch_ParseCmdline decodes the launch grammar
//      (verb + argument) off-target -- quoted argv0, quoted args with spaces, missing arg, case-
//      insensitivity, verb after unknown flags. The one-shot menu hook + fixed VAs are live-only.
static int run_launchtest() {
    struct Case {
        const char *cl;
        int         verb;
        const char *arg;
        int         skip;
    };
    static const Case cases[] = {
        {"mh.exe", 0, "", 0},
        {"mh.exe --load MyGame", 1, "MyGame", 0},
        {"\"C:\\Program Files\\mh\\mh.exe\" --load \"Late Game 4\"", 1, "Late Game 4", 0},
        {"mh.exe --load", 0, "", 0},                  // missing name
        {"mh.exe --LOAD Foo", 1, "Foo", 0},           // case-insensitive
        {"mh.exe -windowed --load Foo", 1, "Foo", 0}, // skip unknown tok
        {"mh.exe --newgame", 2, "", 0},               // bare -> default race
        {"mh.exe --newgame h", 2, "h", 0},            // race H
        {"mh.exe --newgame a", 2, "a", 0},            // race A
        {"mh.exe --mp-host", 3, "", 0},
        {"mh.exe --mp-join 192.168.0.48:6501", 4, "192.168.0.48:6501", 0},
        {"mh.exe --skip-intro", 0, "", 1},                     // flag only, no verb
        {"mh.exe --newgame h --skip-intro", 2, "h", 1},        // flag after verb
        {"mh.exe --skip-intro --load MyGame", 1, "MyGame", 1}, // flag before verb
        // TACT-PREP: --tactical <save>. Verb 8 -- APPENDED to the enum, never inserted, because
        // these literals are the contract (see the enum's comment in seams/launch.cpp). Same
        // required-argument grammar as --load, so the missing-arg and quoted-arg arms are repeated
        // here rather than assumed to transfer.
        {"mh.exe --tactical MyGame", 8, "MyGame", 0},
        {"mh.exe --tactical", 0, "", 0},                            // missing name -> no verb
        {"mh.exe --TACTICAL \"Late Game 4\"", 8, "Late Game 4", 0}, // case-insensitive + quoted
        {"mh.exe --tactical MyGame --skip-intro", 8, "MyGame", 1},
    };
    const int N  = (int)(sizeof(cases) / sizeof(cases[0]));
    int       ok = 1;
    printf("=== launchtest (D17 CLI parser) ===\n");
    for (int i = 0; i < N; ++i) {
        char arg[128];
        int  skip = -1;
        int  v    = MH_Launch_ParseCmdline(cases[i].cl, arg, (int)sizeof(arg), &skip);
        int  pass = (v == cases[i].verb) && (strcmp(arg, cases[i].arg) == 0) && (skip == cases[i].skip);
        printf("  [%s] cl='%s' -> verb=%d arg='%s' skip=%d (want verb=%d arg='%s' skip=%d)\n",
               pass ? "ok" : "FAIL", cases[i].cl, v, arg, skip, cases[i].verb, cases[i].arg, cases[i].skip);
        if (!pass) ok = 0;
    }
    // mp:U21 -- the --skip-intro teardown must not run while the LOGO.AVI audio thread can still be
    // inside CreateSoundBuffer reading the format block the teardown frees. Row 0 is the state the
    // rig measured on 45/45 boots of peer B at frame 2 (stream + dsound, no buffer yet, thread alive,
    // 0 ms waited): it MUST wait -- the old hook tore down there and crashed dsound.dll+0x1f3a3.
    struct Skip {
        int         stream, ds, dsbuf, pcm, alive;
        unsigned    waited;
        int         want;
        const char *why;
    };
    static const Skip skips[] = {
        {1, 1, 0, 0, 1, 0, MH_INTRO_SKIP_WAIT, "frame 2: thread in CreateSoundBuffer (the U21 race)"},
        {1, 1, 1, 0, 1, 5, MH_INTRO_SKIP_WAIT, "buffer made, PCM block not yet"},
        {1, 1, 0, 1, 1, 5, MH_INTRO_SKIP_WAIT, "PCM without buffer: still not settled"},
        {1, 1, 1, 1, 1, 20, MH_INTRO_SKIP_GO, "buffer + PCM: past the format read"},
        {1, 1, 0, 0, 0, 0, MH_INTRO_SKIP_GO, "thread already gone (create failed)"},
        {0, 1, 0, 0, 1, 0, MH_INTRO_SKIP_GO, "movie has no audio stream"},
        {1, 0, 0, 0, 1, 0, MH_INTRO_SKIP_GO, "no IDirectSound: format never read"},
        {1, 1, 0, 0, 1, 1999, MH_INTRO_SKIP_WAIT, "just under the cap"},
        {1, 1, 0, 0, 1, 2000, MH_INTRO_SKIP_TIMEOUT, "cap hit: never hang the skip"},
    };
    for (int i = 0; i < (int)(sizeof(skips) / sizeof(skips[0])); ++i) {
        const Skip &k   = skips[i];
        int         got = MH_Launch_IntroSkipReady(k.stream, k.ds, k.dsbuf, k.pcm, k.alive, k.waited, 2000u);
        printf("  [%s] intro-skip %s -> %d (want %d)\n", got == k.want ? "ok" : "FAIL", k.why, got, k.want);
        if (got != k.want) ok = 0;
    }
    if (ok) {
        printf("=== PASS: launch CLI grammar decoded correctly ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

static int run_host(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role      = 0;
    c.port      = port;
    c.player_id = 0;
    c.peers     = 1;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[host]   init failed\n");
        return 2;
    }
    printf("[host]   listening on :%d as player 0\n", port);

    for (int t = 0; t < 500; ++t) { // up to ~5 s
        int  sender = -1;
        char buf[128];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len)) {
            if (len < (int)sizeof(buf)) buf[len] = '\0';
            else buf[sizeof(buf) - 1] = '\0';
            printf("[host]   received from player %d: '%s' (%d bytes)\n", sender, buf, len);
            MH_Net_Send(sender, "PONG", 4); // reply to whoever sent it
            printf("[host]   replied PONG to player %d\n", sender);
            Sleep(300); // let the reply flush before we exit
            return 0;
        }
        Sleep(10);
    }
    printf("[host]   TIMEOUT waiting for a datagram\n");
    return 1;
}

// `host` = who to dial. Defaults to loopback (the self-contained selftest), but can be any address:
// pointing it at a relay/tunnel address is how we test that a REMOTE peer completes the handshake
// and exchanges an encrypted datagram over the real internet path, without needing the game's UI on
// either end. See the internet-play notes.
static int run_client(int port, const char *host) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpynA(c.host, (host && *host) ? host : "127.0.0.1", sizeof(c.host));
    c.port      = port;
    c.player_id = 1;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[client] init failed (no host?)\n");
        return 2;
    }

    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[client] never connected\n");
        return 3;
    }
    printf("[client] connected to host as player 1\n");

    MH_Net_Send(0, "PING", 4); // to the host (player 0)
    printf("[client] sent PING to player 0\n");

    for (int t = 0; t < 500; ++t) {
        int  sender = -1;
        char buf[128];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len)) {
            if (len < (int)sizeof(buf)) buf[len] = '\0';
            else buf[sizeof(buf) - 1] = '\0';
            printf("[client] received from player %d: '%s' (%d bytes)\n", sender, buf, len);
            if (len == 4 && memcmp(buf, "PONG", 4) == 0) {
                printf("[client] round-trip OK\n");
                return 0;
            }
            printf("[client] unexpected reply\n");
            return 4;
        }
        Sleep(10);
    }
    printf("[client] TIMEOUT waiting for PONG\n");
    return 5;
}

// ---- 3-node broadcast/relay test: a client broadcasts; the host (local-deliver) AND the OTHER
//      client (relay fan-out) must both receive it. This is what makes it a STAR, not just a pipe.
static int run_recv(int port, int myid) { // a passive client that must get the bcast
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpyA(c.host, "127.0.0.1");
    c.port      = port;
    c.player_id = myid;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[recv%d]  init failed\n", myid);
        return 2;
    }
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[recv%d]  never connected\n", myid);
        return 3;
    }
    printf("[recv%d]  connected, waiting for a relayed broadcast\n", myid);
    for (int t = 0; t < 500; ++t) {
        int  sender = -1;
        char buf[128];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len)) {
            if (len < (int)sizeof(buf)) buf[len] = '\0';
            else buf[sizeof(buf) - 1] = '\0';
            printf("[recv%d]  got relayed broadcast from player %d: '%s'\n", myid, sender, buf);
            return 0;
        }
        Sleep(10);
    }
    printf("[recv%d]  TIMEOUT waiting for broadcast\n", myid);
    return 1;
}

static int run_bcast(int port, int myid) { // the sender: broadcast repeatedly
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpyA(c.host, "127.0.0.1");
    c.port      = port;
    c.player_id = myid;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[bcast%d] init failed\n", myid);
        return 2;
    }
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[bcast%d] never connected\n", myid);
        return 3;
    }
    printf("[bcast%d] connected, broadcasting\n", myid);
    for (int i = 0; i < 12; ++i) {
        MH_Net_Send(MH_NET_BROADCAST, "BCAST", 5);
        Sleep(100);
    } // cover join race
    Sleep(300);
    return 0;
}

static int wait_exit(HANDLE h, DWORD ms); // fwd

static int launch(const char *exe, const char *mode, int port, int extra, PROCESS_INFORMATION *pi) {
    char cmd[MAX_PATH + 96];
    if (extra >= 0) wsprintfA(cmd, "\"%s\" %s %d %d", exe, mode, port, extra);
    else wsprintfA(cmd, "\"%s\" %s %d", exe, mode, port);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    return CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, pi) ? 1 : 0;
}

static int run_selftest3(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    PROCESS_INFORMATION ph, prb, psa; // host, recv-B, send-A
    printf("=== selftest3 (broadcast relay fan-out) on port %d ===\n", port);
    if (!launch(exe, "host", port, -1, &ph)) {
        printf("launch host failed\n");
        return 10;
    }
    Sleep(400);
    if (!launch(exe, "recv", port, 2, &prb)) {
        printf("launch recv failed\n");
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    Sleep(400); // let recv-B connect before A broadcasts
    if (!launch(exe, "bcast", port, 1, &psa)) {
        printf("launch bcast failed\n");
        TerminateProcess(ph.hProcess, 99);
        TerminateProcess(prb.hProcess, 99);
        return 12;
    }

    int hc = wait_exit(ph.hProcess, 15000);
    int rc = wait_exit(prb.hProcess, 15000);
    int sc = wait_exit(psa.hProcess, 15000);
    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(prb.hProcess);
    CloseHandle(prb.hThread);
    CloseHandle(psa.hProcess);
    CloseHandle(psa.hThread);

    printf("=== host(local-deliver)=%d  recv-B(relay)=%d  send-A=%d ===\n", hc, rc, sc);
    if (hc == 0 && rc == 0 && sc == 0) {
        printf("=== PASS: broadcast reached host + other client via relay ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

// ---- seam plumbing test: drive MH_Seam_Send / MH_Seam_PollRecv over the real transport, with the
//      RX buffer pointed at a local mock region (so no game / fixed VAs needed). Proves the Phase-A1
//      packet path end-to-end: a client's send lands correctly framed in the host's RX layout, the
//      sender is tagged, the CRC self-check is forced to pass, and the stale tail is zero-filled.
static unsigned char g_mock[SEAM_RX_SIZE + 32];
static int           g_mock_sender = -777;
static unsigned int  g_mock_crc_c  = 0xffffffff;
static unsigned int  g_mock_len    = 0;
static int           g_mock_ishost = 1, g_mock_lpi = 0;

// tooling:TL-TEST1 -- WHY THE CHILDREN CRASHED, and the two rules that keep them from doing it again.
// Every address MH_Seam_Send / MH_Seam_PollRecv touches defaults to a FIXED mh.exe VA (net_seams.cpp
// g_a; _G_LLM_LOBBY_SLOTS for the N2 guard). In this exe those VAs are whatever ASLR put there: inside
// our own 122 MB .data when the image loads low (plain build, base 0x00290000 measured 2026-09-24 --
// the read returns an unrelated global of ours), unmapped or ASan-reserved otherwise (the historical
// "0xC0000005 in one of the two children, which one varies"). Measured 2026-09-24 at 7d2864d5:
//   * the CLIENT never pointed g_a at a mock, so MH_Seam_Send read the real LOCAL_PLAYER_INDEX
//     (0x005d55ac) -- ASan: `access-violation on unknown address 0x005d55ac ... run_seam_client`.
//     RULE 1: BOTH children call seam_point_at_mock() before the first seam call.
//   * the HOST's test packet was a 0x0c slot push, which since mp:N2 goes through the "is the sender
//     seated" guard -- a read of mh::addr::_G_LLM_LOBBY_SLOTS that MH_SeamAddrs cannot redirect. Plain
//     build: it read our own zeroed .data, found nobody seated, DROPPED the packet -> host TIMEOUT 8/8.
//     RULE 2: the test packet is a type PollRecv does not intercept (0x0b/0x0c = N2, 0x0e = host-left,
//     0x17 = R6 hello hold, 0x21 = map), so the path under test is exactly the framing + tagging one.
//     The N2 drop itself is not covered here; it needs a redirectable slot table (see TL-TEST1).
constexpr unsigned char SEAM_TEST_TYPE = 0x10; // not intercepted by MH_Seam_PollRecv

static void seam_point_at_mock(int is_host, int local_player_index) {
    g_mock_ishost = is_host;
    g_mock_lpi    = local_player_index;
    memset(g_mock, 0xAA, sizeof(g_mock)); // poison, so we can see the zero-fill + copy
    MH_SeamAddrs a;
    a.rx_type            = g_mock;
    a.rx_sender_id       = &g_mock_sender;
    a.rx_crc_computed    = &g_mock_crc_c;
    a.rx_crc_embedded    = (unsigned int *)(g_mock + 1);
    a.rx_len             = &g_mock_len;
    a.local_player_index = &g_mock_lpi;
    a.is_host            = &g_mock_ishost;
    MH_Seam_SetAddrs(&a);
}

// THE TRIPWIRE -- what makes RULES 1-2 checkable on every layout. Without it a stray fixed-VA access
// is layout-dependent: an AV when the VA is unmapped, but a SILENT read of one of our own globals when
// ASLR loads the 122 MB image over it (a mutation that dropped RULE 1 passed 3/3 under ASan that way).
// Hardware data breakpoints fire on the ADDRESS whatever is (or is not) mapped there, so the four
// fixed VAs the seam would touch unmocked are watched on the seam child's main thread -- the only
// thread that calls the seam -- and any access fails the child with the VA named.
// Positive control first: a watch that is not honoured (DRs unavailable) fails loudly, never passes.
static volatile LONG  g_trip_hits = 0;
static volatile DWORD g_trip_va   = 0;
static volatile int   g_trip_control; // the positive-control target
static LONG CALLBACK  seam_trip_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c   = ep->ContextRecord;
    DWORD    dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    DWORD va = (dr6 & 1) ? c->Dr0 : (dr6 & 2) ? c->Dr1
                                 : (dr6 & 4)   ? c->Dr2
                                               : c->Dr3;
    if (InterlockedIncrement(&g_trip_hits) == 1) g_trip_va = va;
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
// DR7: local-enable bit 2i; R/W bits 16+4i = 11 (read or write); LEN bits 18+4i (00=1, 01=2, 11=4).
static bool seam_trip_set(const DWORD va[4], const int len[4]) {
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    DWORD dr7      = 0;
    for (int i = 0; i < 4; ++i) {
        if (!va[i]) continue;
        DWORD l = (len[i] == 4) ? 3u : (len[i] == 2) ? 1u
                                                     : 0u;
        dr7 |= (1u << (2 * i)) | (3u << (16 + 4 * i)) | (l << (18 + 4 * i));
    }
    c.Dr0 = va[0];
    c.Dr1 = va[1];
    c.Dr2 = va[2];
    c.Dr3 = va[3];
    c.Dr7 = dr7;
    return SetThreadContext(GetCurrentThread(), &c) != 0;
}
static bool seam_trip_arm(const char *who) {
    static PVOID veh = nullptr;
    if (!veh) veh = AddVectoredExceptionHandler(1, seam_trip_veh);
    const DWORD ctl_va[4]  = {(DWORD)(uintptr_t)&g_trip_control, 0, 0, 0};
    const int   ctl_len[4] = {4, 0, 0, 0};
    g_trip_hits            = 0;
    if (!veh || !seam_trip_set(ctl_va, ctl_len)) {
        printf("%s FAIL tripwire: could not set debug registers (err %lu)\n", who, GetLastError());
        return false;
    }
    int v = g_trip_control; // must trip
    (void)v;
    if (g_trip_hits != 1) {
        printf("%s FAIL tripwire positive control: %ld hits on a watched read (want 1)\n", who, g_trip_hits);
        return false;
    }
    // The four fixed mh.exe VAs an UNMOCKED seam touches first (all naturally aligned for their LEN).
    const DWORD va[4]  = {(DWORD)mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX, // MH_Seam_Send's self index
                          (DWORD)mh::addr::_G_LLM_NET_IS_HOST,            // PollRecv's role gates
                          (DWORD)mh::addr::_G_LLM_LOBBY_SLOTS + 1,        // N2: slot 0's player_id
                          (DWORD)mh::addr::lobby_rx_type};                // PollRecv's copy target
    const int   len[4] = {4, 4, 4, 2};
    g_trip_hits        = 0;
    g_trip_va          = 0;
    if (!seam_trip_set(va, len)) {
        printf("%s FAIL tripwire: could not arm the fixed-VA watches (err %lu)\n", who, GetLastError());
        return false;
    }
    return true;
}
// 0 = clean; otherwise the child's failure code, with the VA named.
static int seam_trip_verdict(const char *who) {
    const DWORD none[4] = {0, 0, 0, 0};
    const int   nl[4]   = {0, 0, 0, 0};
    seam_trip_set(none, nl);
    if (g_trip_hits == 0) return 0;
    printf("%s FAIL touched fixed mh.exe VA 0x%08lx (%ld hit(s)) -- a seam address the mock does not cover\n",
           who, g_trip_va, g_trip_hits);
    return 7;
}

static int run_seam_host(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role      = 0;
    c.port      = port;
    c.player_id = 0;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[seamhost] init failed\n");
        return 2;
    }
    seam_point_at_mock(/*is_host=*/1, /*local_player_index=*/0);
    if (!seam_trip_arm("[seamhost]")) return 6;
    printf("[seamhost] listening; polling seam...\n");
    for (int t = 0; t < 500; ++t) {
        int r = MH_Seam_PollRecv();
        if (r == SEAM_RX_SIZE) {
            int ok = 1;
            if (g_mock[0] != SEAM_TEST_TYPE) {
                printf("[seamhost] FAIL type=%02x\n", g_mock[0]);
                ok = 0;
            }
            if (memcmp(g_mock + 5, "SLOTDATA", 8) != 0) {
                printf("[seamhost] FAIL payload\n");
                ok = 0;
            }
            if (g_mock_sender != 1) {
                printf("[seamhost] FAIL sender=%d\n", g_mock_sender);
                ok = 0;
            }
            if (g_mock_crc_c != 0 || *(unsigned int *)(g_mock + 1) != 0) {
                printf("[seamhost] FAIL crc not equalised\n");
                ok = 0;
            }
            if (g_mock_len != SEAM_RX_SIZE) {
                printf("[seamhost] FAIL len=%u\n", g_mock_len);
                ok = 0;
            }
            if (g_mock[13] != 0 || g_mock[SEAM_RX_SIZE - 1] != 0) {
                printf("[seamhost] FAIL tail not zeroed\n");
                ok = 0;
            }
            printf("[seamhost] recv type=0x%02x sender=%d payload='%.8s' len=%u\n",
                   g_mock[0], g_mock_sender, g_mock + 5, g_mock_len);
            int trip = seam_trip_verdict("[seamhost]");
            if (trip) return trip;
            return ok ? 0 : 1;
        }
        Sleep(10);
    }
    int trip = seam_trip_verdict("[seamhost]");
    if (trip) return trip;
    printf("[seamhost] TIMEOUT\n");
    return 3;
}

static int run_seam_client(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpyA(c.host, "127.0.0.1");
    c.port      = port;
    c.player_id = 1;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[seamcli]  init failed\n");
        return 2;
    }
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[seamcli]  never connected\n");
        return 3;
    }
    seam_point_at_mock(/*is_host=*/0, /*local_player_index=*/1); // RULE 1 (see seam_point_at_mock)
    if (!seam_trip_arm("[seamcli] ")) return 6;
    unsigned char pkt[16];
    pkt[0] = SEAM_TEST_TYPE;                  // RULE 2: a type PollRecv delivers without interception
    pkt[1] = pkt[2] = pkt[3] = pkt[4] = 0xEE; // garbage CRC field -- receiver ignores it
    memcpy(pkt + 5, "SLOTDATA", 8);
    MH_Seam_Send(0, 0, pkt, 13); // mode 0, dest 0 != our index 1 -> unicast to the host
    int trip = seam_trip_verdict("[seamcli] ");
    printf("[seamcli]  sent type 0x%02x to host\n", SEAM_TEST_TYPE);
    Sleep(400);
    return trip;
}

static int run_seamtest(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char hcmd[MAX_PATH + 64], ccmd[MAX_PATH + 64];
    wsprintfA(hcmd, "\"%s\" seam_host %d", exe, port);
    wsprintfA(ccmd, "\"%s\" seam_client %d", exe, port);
    STARTUPINFOA        si;
    PROCESS_INFORMATION ph, pc;
    printf("=== seamtest (lobby send/recv plumbing over TCP) on port %d ===\n", port);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, hcmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &ph)) {
        printf("launch seam_host failed\n");
        return 10;
    }
    Sleep(400);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, ccmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pc)) {
        printf("launch seam_client failed\n");
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    int hc, cc;
    if (WaitForSingleObject(ph.hProcess, 15000) != WAIT_OBJECT_0) {
        TerminateProcess(ph.hProcess, 99);
        hc = 98;
    } else {
        DWORD e = 1;
        GetExitCodeProcess(ph.hProcess, &e);
        hc = (int)e;
    }
    if (WaitForSingleObject(pc.hProcess, 15000) != WAIT_OBJECT_0) {
        TerminateProcess(pc.hProcess, 99);
        cc = 98;
    } else {
        DWORD e = 1;
        GetExitCodeProcess(pc.hProcess, &e);
        cc = (int)e;
    }
    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(pc.hProcess);
    CloseHandle(pc.hThread);
    printf("=== seam_host=%d  seam_client=%d ===\n", hc, cc);
    if (hc == 0 && cc == 0) {
        printf("=== PASS: seam plumbing delivered a framed, sender-tagged, CRC-valid packet ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

// ---- linktest: the link watchdog (R-live) --------------------------------------------------------
// The property under test is NEGATIVE and was the whole 2026-07-26 internet failure: a peer that stops
// talking WITHOUT closing its socket must be noticed. TCP cannot tell that apart from an idle link, so
// the transport pings and times out -- and nothing else in the suite would catch that regressing,
// because every positive test passes just as well with the watchdog deleted.
//
// The mute peer sets ping_ms = -1 ("explicitly off"), which is exactly a blackholed path: connected,
// never erroring, never delivering a byte. The host runs a 2 s timeout and must drop it.
static int run_mute_peer(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpyA(c.host, "127.0.0.1");
    c.port          = port;
    c.player_id     = 1;
    c.log           = 1;
    c.ping_ms       = -1; // say nothing at all
    c.rx_timeout_ms = -1; // and never drop the host on our side, so only the host's verdict is tested
    if (!MH_Net_InitEx(&c)) {
        printf("[mute]   init failed\n");
        return 2;
    }
    printf("[mute]   connected and going silent\n");
    // Must outlive the host's ENTIRE observation window. If this process exits first its socket sends
    // a FIN, the host detects THAT, and the test passes with the watchdog deleted -- which is exactly
    // what happened the first time it was written. run_linktest kills us once the host has decided.
    Sleep(60000);
    return 0;
}

static int run_watch_host(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role          = 0;
    c.port          = port;
    c.player_id     = 0;
    c.peers         = 1;
    c.log           = 1;
    c.ping_ms       = 250;
    c.rx_timeout_ms = 2000;
    if (!MH_Net_InitEx(&c)) {
        printf("[watch]  init failed\n");
        return 2;
    }
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10); // wait for the mute peer
    if (MH_Net_PeerCount() < 1) {
        printf("[watch]  mute peer never connected\n");
        return 3;
    }
    DWORD t0 = GetTickCount();
    printf("[watch]  peer connected; expecting a drop within ~2 s of silence\n");
    // WALL CLOCK, not an iteration count: 700 * Sleep(10) is ~9 s in practice, which outlived the mute
    // peer and let its exit FIN pass for a watchdog detection.
    const DWORD WINDOW_MS = 6000;
    while (MH_Net_PeerCount() > 0 && GetTickCount() - t0 < WINDOW_MS) Sleep(10);
    DWORD dt = GetTickCount() - t0;
    if (MH_Net_PeerCount() > 0) {
        printf("[watch]  FAIL: still 'connected' to a silent peer after %lu ms\n", dt);
        return 4;
    }
    // Guard the other direction too: dropping instantly would mean the timeout is not being honoured
    // and a healthy-but-quiet link would be killed mid-game.
    if (dt < 1500) {
        printf("[watch]  FAIL: dropped after only %lu ms (timeout was 2000 ms -- too eager)\n", dt);
        return 5;
    }
    printf("[watch]  dropped the silent peer after %lu ms\n", dt);
    return 0;
}

static int run_linktest(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    PROCESS_INFORMATION ph, pm;
    printf("=== linktest (R-live: a silent peer is dropped, a quiet one is not) on port %d ===\n", port);
    if (!launch(exe, "watch_host", port, -1, &ph)) {
        printf("launch watch_host failed\n");
        return 10;
    }
    Sleep(400);
    if (!launch(exe, "mute_peer", port, -1, &pm)) {
        printf("launch mute_peer failed\n");
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    int hc = wait_exit(ph.hProcess, 20000);
    TerminateProcess(pm.hProcess, 0); // the verdict is the host's; the mute peer is only scenery
    WaitForSingleObject(pm.hProcess, 2000);
    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(pm.hProcess);
    CloseHandle(pm.hThread);
    printf("=== watch_host=%d ===\n", hc);
    if (hc == 0) {
        printf("=== PASS: a connected-but-silent peer is detected and dropped ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

static int wait_exit(HANDLE h, DWORD ms) {
    if (WaitForSingleObject(h, ms) != WAIT_OBJECT_0) {
        TerminateProcess(h, 99);
        return 98;
    }
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    return (int)code;
}

// ---- relinktest: RE-DIALLING AFTER A MATCH (mp:U40) ----------------------------------------------
// THE PROPERTY, and it is the one the 2026-09-01 internet session did not have: a client whose link
// has died must be able to make a NEW one in the SAME PROCESS. Before U40 it could not, and the
// reason was one latched bool -- `g_started` stayed true for the life of the process (MH_Net_Shutdown
// had been deleted at fork F4B for having no callers), so MH_Net_InitEx early-returned and the
// discovery poll's connect kick, gated on `!MH_Net_IsStarted()`, never fired again. Measured in that
// session's logs: `handshake OK` appears EXACTLY ONCE in 16 minutes, while the host re-advertised its
// new lobby once a second to `peers=0`.
//
// WHY THIS CANNOT BE A RIG TEST AND THE RIG SCENARIO CANNOT BE THIS. `host_rematch` drives the whole
// thing through the real UI and is the user-visible proof; what it cannot stage on demand is the
// FAILED re-dial (step 4 below), because that needs a host that is not there at the moment of the
// second dial. That arm is the one protecting the S8 retry path: a relink that fails must leave the
// transport STOPPED and dialable, not wedged -- otherwise the fix's own failure mode is the bug it
// fixes. MUTATION-CHECKED, not assumed: restoring MH_Net_InitEx's pre-U40 first line
// (`if (g_started || !cfg) return g_started ? 1 : 0;`) fails it on both processes -- the client at
// step 3 with "re-dial never connected -- peers=0 after 8000 ms" (exit 8) and the host with
// "timed out with 1 connections, 1 drops, 0 PONGs" (exit 3), i.e. the second accept never happened.
//
// THE CLIENT IS SILENT BY CONFIGURATION (ping_ms = -1, "explicitly off"), which is linktest's mute
// peer wearing a different hat: it is how a link dies here, deterministically, without either process
// exiting. The HOST's 1.5 s watchdog is what kills it, so "the link is gone" is a fact both ends
// agree on before the re-dial is attempted.
static int relink_wait_peers(int want, DWORD ms, const char *what) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        if (MH_Net_PeerCount() == want) return 1;
        Sleep(10);
    }
    printf("[relinkc] FAIL: %s -- peers=%d after %lu ms (wanted %d)\n", what, MH_Net_PeerCount(),
           (unsigned long)(GetTickCount() - t0), want);
    return 0;
}

static void relink_client_cfg(MH_NetConfig &c, int port) {
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpyA(c.host, "127.0.0.1");
    c.port          = port;
    c.player_id     = 1;
    c.log           = 1;
    c.host_assign   = 1;  // so a SECOND WELCOME is what proves a SECOND connection
    c.ping_ms       = -1; // say nothing: the host's watchdog is what ends the link
    c.rx_timeout_ms = -1; // ...and only the host's verdict is under test
}

static int run_relink_client(int port) {
    MH_NetConfig c;
    relink_client_cfg(c, port);
    if (!MH_Net_InitEx(&c)) {
        printf("[relinkc] first dial failed\n");
        return 2;
    }
    if (!relink_wait_peers(1, 5000, "first connect")) return 3;
    DWORD t0 = GetTickCount();
    while (!MH_Net_IdAssigned() && GetTickCount() - t0 < 5000) Sleep(10);
    if (!MH_Net_IdAssigned()) {
        printf("[relinkc] FAIL: no WELCOME on the first connection\n");
        return 4;
    }
    printf("[relinkc] connected (#1) as player %d\n", MH_Net_LocalPlayerId());

    // 2. go silent -> the host's watchdog retires us. THE PRECONDITION U40 IS ABOUT.
    if (!relink_wait_peers(0, 8000, "host did not drop the silent peer")) return 5;
    // ...and here is the latch that made it unfixable: the transport still calls itself STARTED.
    if (!MH_Net_IsStarted()) {
        printf("[relinkc] FAIL: transport un-started itself -- the test is not testing the U40 path\n");
        return 6;
    }
    printf("[relinkc] link gone, transport still 'started' (peers=0) -- the pre-U40 dead end\n");

    // 3. POSITIVE: re-init an ALREADY-STARTED transport. It must stop the old one and dial again.
    relink_client_cfg(c, port);
    if (!MH_Net_InitEx(&c)) {
        printf("[relinkc] FAIL: re-dial refused\n");
        return 7;
    }
    if (!relink_wait_peers(1, 8000, "re-dial never connected")) return 8;
    t0 = GetTickCount();
    while (!MH_Net_IdAssigned() && GetTickCount() - t0 < 8000) Sleep(10);
    if (!MH_Net_IdAssigned()) {
        printf("[relinkc] FAIL: no SECOND WELCOME -- the old connection was reused, not replaced\n");
        return 9;
    }
    printf("[relinkc] re-connected (#2) as player %d -- a NEW connection, not the old one\n",
           MH_Net_LocalPlayerId());
    MH_Net_Send(0, "PING", 4);
    t0 = GetTickCount();
    for (;;) {
        int  sender = -1;
        char buf[64];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len)) {
            if (len == 4 && memcmp(buf, "PONG", 4) == 0) break;
            printf("[relinkc] FAIL: unexpected reply on the re-dialled link\n");
            return 10;
        }
        if (GetTickCount() - t0 > 8000) {
            printf("[relinkc] FAIL: no PONG over the re-dialled link\n");
            return 11;
        }
        Sleep(10);
    }
    printf("[relinkc] round-trip OK over the RE-DIALLED link\n");

    // 4. NEGATIVE: let it die again, then dial somewhere nothing is listening. The relink must FAIL
    //    CLEANLY -- transport stopped, not wedged -- so the seam's S8 retry can try again.
    if (!relink_wait_peers(0, 8000, "host did not drop the silent peer a second time")) return 12;
    relink_client_cfg(c, port + 1); // nothing listens here
    if (MH_Net_InitEx(&c)) {
        printf("[relinkc] FAIL: a dial to a dead port reported success\n");
        return 13;
    }
    if (MH_Net_IsStarted()) {
        printf("[relinkc] FAIL: a failed relink left the transport 'started' -- wedged, not retryable\n");
        return 14;
    }
    printf("[relinkc] failed relink leaves the transport STOPPED and dialable\n");
    return 0;
}

// The host's own verdict: it must see the peer arrive, GO, and arrive AGAIN. Counting the transitions
// here rather than trusting the client is the point -- "I reconnected" is only true if somebody
// accepted a second connection.
static int run_relink_host(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role          = 0;
    c.port          = port;
    c.player_id     = 0;
    c.peers         = 1;
    c.log           = 1;
    c.host_assign   = 1;
    c.ping_ms       = 250;
    c.rx_timeout_ms = 1500;
    if (!MH_Net_InitEx(&c)) {
        printf("[relinkh] init failed\n");
        return 2;
    }
    printf("[relinkh] listening on :%d, 1.5 s link timeout\n", port);

    int   connects = 0, drops = 0, pongs = 0;
    int   last = 0;
    DWORD t0   = GetTickCount();
    while (GetTickCount() - t0 < 30000) {
        int now = MH_Net_PeerCount();
        if (now > last) printf("[relinkh] connection #%d accepted\n", ++connects);
        if (now < last) printf("[relinkh] connection dropped (#%d)\n", ++drops);
        last = now;

        int  sender = -1;
        char buf[64];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len) && len == 4 && memcmp(buf, "PING", 4) == 0) {
            MH_Net_Send(sender, "PONG", 4);
            ++pongs;
        }
        if (connects >= 2 && drops >= 2 && pongs >= 1) {
            Sleep(200); // let the last reply flush
            printf("[relinkh] saw %d connections, %d drops, answered %d PING(s) -- OK\n", connects,
                   drops, pongs);
            return 0;
        }
        Sleep(10);
    }
    printf("[relinkh] FAIL: timed out with %d connections, %d drops, %d PONGs\n", connects, drops, pongs);
    return 3;
}

static int run_relinktest(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    PROCESS_INFORMATION ph, pc;
    printf("=== relinktest (U40: a client whose link died dials again in the same process) on port %d ===\n",
           port);
    if (!launch(exe, "relink_host", port, -1, &ph)) {
        printf("launch relink_host failed\n");
        return 10;
    }
    Sleep(400); // bind+listen
    if (!launch(exe, "relink_client", port, -1, &pc)) {
        printf("launch relink_client failed\n");
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    int hc = wait_exit(ph.hProcess, 40000);
    int cc = wait_exit(pc.hProcess, 40000);
    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(pc.hProcess);
    CloseHandle(pc.hThread);
    printf("=== relink_host=%d  relink_client=%d ===\n", hc, cc);
    if (hc == 0 && cc == 0) {
        printf("=== PASS: the link died, the client re-dialled, the host accepted a SECOND connection, "
               "and a failed relink stays retryable ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

// ---- qmatchtest: TWO MATCHES OVER ONE LIVE LINK (mp:U41b) ----------------------------------------
// The inbound-queue rollup is per MATCH, but until U41b the counters restarted only at net_reset --
// the TRANSPORT boundary -- which a host_rematch never crosses. This suite keeps ONE TCP link up and
// plays two "matches" over it, separated by MH_Net_QueueMatchBoundary (what mp_session_close calls):
// match 1 queues 20 frames, the boundary lands with 3 still queued, match 2 queues 5. Asserted on
// the host, reading the lanes under their lock: match 1's high-water reaches 20; right after the
// boundary the counters read the carried-over depth (3), not 20 and not 0, the 3 frames are still
// there and the link is still up; match 2's high-water is its own 5. A boundary that did nothing
// fails the second read; one that called reset() (the transport boundary) fails the carry-over.
//
// mp:U41d -- THE EPOCH. `epoch` is a marker ONLY MH_Net_QueueMatchBoundary's call to
// lane_queue::reset_counters() can move (mh_net_queue_policy.h). This suite asserts it strictly
// increases across the two boundaries below AND that it is the reset -- not traffic, not luck --
// that proves it: comment out the `g_lanes.reset_counters()` call in net_transport.cpp (or the
// `++epoch_` inside it) and `epoch_after_b1 == epoch_after_b2`, redding the two QM_CHECKs that name
// "epoch". That mutation is the row's required red arm.
void mh_net_queue_counters_for_test(int *depth, int *high, long *evicted, long *refused,
                                    unsigned *epoch_out); // net_transport.cpp

static bool qm_wait_depth(int want, DWORD budget_ms) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        int  d = 0, h = 0;
        long e = 0, r = 0;
        mh_net_queue_counters_for_test(&d, &h, &e, &r, nullptr);
        if (d >= want) return true;
        if (GetTickCount() - t0 >= budget_ms) return false;
        Sleep(5);
    }
}

static int run_qmatch_host(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role      = 0;
    c.port      = port;
    c.player_id = 0;
    c.peers     = 1;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[qmatchh] init failed\n");
        return 2;
    }
    int fails = 0;
#define QM_CHECK(cond, ...)                         \
    do {                                            \
        if (!(cond)) {                              \
            ++fails;                                \
            printf("[qmatchh] FAIL: " __VA_ARGS__); \
            printf("\n");                           \
        }                                           \
    } while (0)
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[qmatchh] no peer\n");
        return 3;
    }
    int      d = 0, h = 0, sender = 1;
    long     e = 0, r = 0;
    unsigned ep0 = 0, ep1 = 0, ep2 = 0;
    char     buf[64];
    int      len;

    // mp:U41d -- the epoch BEFORE either boundary, so "did it move" has a real baseline (a process
    // that somehow started at a non-zero epoch must not read as "advanced" on its first boundary).
    mh_net_queue_counters_for_test(&d, &h, &e, &r, &ep0);

    // MATCH 1: the client queues 20 frames and nobody drains until they are all there.
    QM_CHECK(qm_wait_depth(20, 5000), "match 1's 20 frames never all arrived");
    mh_net_queue_counters_for_test(&d, &h, &e, &r, nullptr);
    printf("[qmatchh] match 1: depth %d high-water %d evicted %ld refused %ld\n", d, h, e, r);
    QM_CHECK(h >= 20, "match 1's high-water %d < 20", h);
    for (int i = 0; i < 17; ++i) {
        len = (int)sizeof(buf);
        QM_CHECK(MH_Net_Recv(&sender, buf, &len), "drain %d of 17 found nothing", i);
    }

    // THE BOUNDARY, with 3 frames still queued.
    MH_Net_QueueMatchBoundary();
    mh_net_queue_counters_for_test(&d, &h, &e, &r, &ep1);
    printf("[qmatchh] after boundary: depth %d high-water %d evicted %ld refused %ld epoch %u\n", d, h,
           e, r, ep1);
    QM_CHECK(d == 3, "the boundary DROPPED queued frames (depth %d, want 3) -- that is reset(), the "
                     "transport boundary",
             d);
    QM_CHECK(h == 3, "after the boundary the high-water reads %d, want the carried depth 3 (20 = "
                     "inherited from match 1, 0 = below what is queued)",
             h);
    QM_CHECK(e == 0 && r == 0, "evicted/refused not restarted (%ld/%ld)", e, r);
    // mp:U41d -- THE MUTATION TARGET. If MH_Net_QueueMatchBoundary's call to reset_counters() (or the
    // ++epoch_ inside it) is skipped, ep1 stays at ep0 and this reds -- unlike the depth/high-water
    // checks above, nothing about traffic timing or queue depth can make this pass by accident.
    QM_CHECK(ep1 == ep0 + 1, "epoch did not advance across the first boundary (%u -> %u, want +1) -- "
                             "reset_counters() did not run",
             ep0, ep1);
    QM_CHECK(MH_Net_PeerCount() == 1, "the link did not survive the match boundary");
    for (int i = 0; i < 3; ++i) {
        len = (int)sizeof(buf);
        QM_CHECK(MH_Net_Recv(&sender, buf, &len), "the carried frame %d was not deliverable", i);
    }

    // MATCH 2 over the SAME link: 5 frames.
    MH_Net_Send(sender, "NEXT", 4);
    QM_CHECK(qm_wait_depth(5, 5000), "match 2's 5 frames never all arrived");
    mh_net_queue_counters_for_test(&d, &h, &e, &r, nullptr);
    printf("[qmatchh] match 2: depth %d high-water %d evicted %ld refused %ld\n", d, h, e, r);
    QM_CHECK(h == 5, "match 2's rollup reports high-water %d -- it must be its OWN 5, not match 1's 20",
             h);
    MH_Net_QueueMatchBoundary(); // match 2's own rollup line, into mh_net.log
    mh_net_queue_counters_for_test(&d, &h, &e, &r, &ep2);
    QM_CHECK(ep2 == ep1 + 1, "epoch did not advance across the second boundary (%u -> %u, want +1)",
             ep1, ep2);
    MH_Net_Send(sender, "DONE", 4);
    Sleep(300);
#undef QM_CHECK
    printf("[qmatchh] %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}

static bool qm_wait_word(const char *word, DWORD budget_ms) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < budget_ms) {
        int  sender = -1;
        char buf[64];
        int  len = (int)sizeof(buf);
        if (MH_Net_Recv(&sender, buf, &len) && len == 4 && memcmp(buf, word, 4) == 0) return true;
        Sleep(5);
    }
    return false;
}

static int run_qmatch_client(int port) {
    MH_NetConfig c;
    memset(&c, 0, sizeof(c));
    c.role = 1;
    lstrcpynA(c.host, "127.0.0.1", sizeof(c.host));
    c.port      = port;
    c.player_id = 1;
    c.log       = 1;
    if (!MH_Net_InitEx(&c)) {
        printf("[qmatchc] init failed\n");
        return 2;
    }
    for (int t = 0; t < 500 && MH_Net_PeerCount() < 1; ++t) Sleep(10);
    if (MH_Net_PeerCount() < 1) {
        printf("[qmatchc] never connected\n");
        return 3;
    }
    char f[4] = {'Q', '1', 0, 0};
    for (int i = 0; i < 20; ++i) {
        f[2] = (char)i;
        MH_Net_Send(0, f, 4);
    }
    if (!qm_wait_word("NEXT", 10000)) {
        printf("[qmatchc] never told to start match 2\n");
        return 4;
    }
    f[1] = '2';
    for (int i = 0; i < 5; ++i) {
        f[2] = (char)i;
        MH_Net_Send(0, f, 4);
    }
    if (!qm_wait_word("DONE", 10000)) {
        printf("[qmatchc] host never finished\n");
        return 5;
    }
    return 0;
}

static int run_qmatchtest(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    PROCESS_INFORMATION ph, pc;
    printf("=== qmatchtest (U41b: two matches over one live link, separate queue rollups) on port %d ===\n",
           port);
    if (!launch(exe, "qmatch_host", port, -1, &ph)) {
        printf("launch qmatch_host failed\n");
        return 10;
    }
    Sleep(400);
    if (!launch(exe, "qmatch_client", port, -1, &pc)) {
        printf("launch qmatch_client failed\n");
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    int hc = wait_exit(ph.hProcess, 30000);
    int cc = wait_exit(pc.hProcess, 30000);
    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(pc.hProcess);
    CloseHandle(pc.hThread);
    printf("=== qmatch_host=%d  qmatch_client=%d ===\n", hc, cc);
    if (hc == 0 && cc == 0) {
        printf("=== PASS: the second match's rollup did not inherit the first's, and the boundary kept "
               "the link and the queued frames ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

// ---- udprelinktest: THE SAME RULE, ON THE OTHER MODULE (mp:T1b) ----------------------------------
// U40's fix lived in mh_net.dll. `[net] transport=udp` is a whole second transport with its own
// InitEx, and its `if (g_started ...) return` was still there -- so the defect U40 measured (a
// client that finished a match can never dial the re-created lobby, because the transport calls
// itself started for the life of the process) came straight back on a UDP lane. T1b is that rule
// mirrored, and this suite is its oracle.
//
// WHY THIS IS NOT AN ARM OF `relinktest`, which tests the identical property. relinktest drives the
// MH_Net_* EXPORTS, and in net_selftest.exe those are mh_net/net_transport.cpp's -- the TCP module's
// -- because udp_transport.cpp defines the same 23 symbols and two definitions do not link. So the
// UDP side is reached the way `udploopbacktest` reaches it: through mh::netudp::Endpoint, the object
// udp_transport.cpp owns exactly one of. The restart therefore lives IN the endpoint (start() on a
// started endpoint restarts it) and the module's InitEx is a thin g_started bookkeeper over it --
// which is what makes the module's behaviour testable here at all, and is worth preserving if this
// ever moves.
//
// HOW THE LINK IS KILLED, in one process and without either endpoint exiting. The same mute-peer
// trick relinktest/linktest use, but the mute is harder to arrange over UDP than over TCP and the
// configuration below is load-bearing in two non-obvious ways:
//   * THE HOST'S PING IS OFF. A ping is answered by a PONG from inside the receive path, with no
//     regard for whether the answering peer has pings of its own -- so a pinging host keeps its own
//     victim alive and the watchdog never fires.
//   * host_assign IS OFF. A host-assigned WELCOME travels on the reliable stream, and a peer that
//     has received stream bytes publishes its acknowledgement frontier every ACK_MS forever. That
//     traffic is the client speaking, so the host would never see silence. With no WELCOME the
//     client's only transmission is its own FLAG_HELLO, which stops being retransmitted as soon as
//     the host acks it -- and after that it is genuinely mute.
// The observable for "a SECOND connection" is therefore the host's own `hs_done` counter rather than
// a second WELCOME (relinktest's proof, unavailable here for the reason above): a completed
// handshake is counted where it completes, and nothing else increments it.
namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;

// ~4 MB of stream rings apiece (8 peer slots x a 1024-segment reorder window x two directions), so
// file scope and not a local, exactly as udp_loopback_selftest's three endpoints are.
Endpoint g_ur_host, g_ur_client;

int g_ur_checks = 0, g_ur_fails = 0;

void ur_check(bool ok, const char *fmt, ...) {
    ++g_ur_checks;
    if (ok) return;
    ++g_ur_fails;
    char    b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("  FAIL: %s\n", b);
}

void ur_log(void * /*ctx*/, const char *line) {
    if (getenv("MH_UDP_RELINK_VERBOSE")) printf("    | %s\n", line);
}

// The suite's own key, for udploopbacktest's reason: a suite must not depend on, or create, an
// mh_key.txt beside whatever directory it happens to run in.
const unsigned char UR_PSK[32] = {
    0x4d,
    0x48,
    0x74,
    0x65,
    0x73,
    0x74,
    0x6b,
    0x65,
    0x79,
    0x20,
    0x75,
    0x64,
    0x70,
    0x20,
    0x72,
    0x65,
    0x6c,
    0x69,
    0x6e,
    0x6b,
    0x20,
    0x54,
    0x31,
    0x62,
    0x20,
    0x66,
    0x69,
    0x78,
    0x65,
    0x64,
    0x21,
    0x21,
};

void ur_cfg(Config &c, int role, int port, unsigned short bind_port, int ping_ms, int rx_timeout_ms) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port          = port;
    c.net.player_id     = (role == 0) ? 0 : 1;
    c.net.log           = 1;
    c.net.host_assign   = 0; // see the note above -- a WELCOME would make the client ack forever
    c.net.ping_ms       = ping_ms;
    c.net.rx_timeout_ms = rx_timeout_ms;
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <class Pred>
bool ur_wait(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(5);
    }
}

long ur_hs_done(Endpoint &ep) {
    Counters k;
    ep.counters(k);
    return k.hs_done;
}

} // namespace

static int run_udprelinktest(int port) {
    // A base of its own: udploopbacktest holds 39560..39591 and the TCP suites take the argument
    // itself, so a suite that binds four ports in one process takes the argument plus 100 rather
    // than sharing a range with either.
    const int base = port + 100;
    printf("=== udprelinktest (T1b: U40's restart rule on mh_net_udp.dll) on ports %d.. ===\n", base);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Endpoint &host = g_ur_host, &cl = g_ur_client;
    new (&host) Endpoint(); // placement-new: a zeroed endpoint, as the loopback suite does
    new (&cl) Endpoint();
    host.set_log(ur_log, nullptr);
    cl.set_log(ur_log, nullptr);

    Config ch, cc;
    ur_cfg(ch, 0, base, (unsigned short)base, -1, 1500);     // no pings; a 1.5 s link timeout
    ur_cfg(cc, 1, base, (unsigned short)(base + 1), -1, -1); // say nothing, and never give up

    ur_check(host.start(ch, UR_PSK, true), "the host started");
    ur_check(cl.start(cc, UR_PSK, true), "the client started (first dial)");

    // ---- 1. the first connection -----------------------------------------------------------------
    const bool up1 = ur_wait([&] { return host.peer_count() == 1; }, 8000);
    ur_check(up1, "the host admitted the client (peers=%d)", host.peer_count());
    ur_check(ur_hs_done(host) == 1, "exactly one completed handshake so far (%ld)", ur_hs_done(host));
    if (!up1) {
        host.stop();
        cl.stop();
        printf("=== udprelinktest: %d checks, %d failures ===\n", g_ur_checks, g_ur_fails);
        return 1;
    }

    // ---- 2. the link dies, and the client does not notice -- the pre-T1b dead end -----------------
    const bool gone = ur_wait([&] { return host.peer_count() == 0; }, 8000);
    ur_check(gone, "the host's watchdog retired the silent peer (peers=%d)", host.peer_count());
    ur_check(cl.started() && cl.peer_count() == 1,
             "...while the client still calls itself started with a live peer -- the dead end U40 "
             "measured (started=%d, peers=%d)",
             (int)cl.started(), cl.peer_count());

    // ---- 3. THE RULE: re-init on a started transport RESTARTS it ----------------------------------
    ur_cfg(cc, 1, base, (unsigned short)(base + 1), -1, -1);
    const bool re = cl.start(cc, UR_PSK, true);
    ur_check(re, "the re-dial was accepted on an ALREADY-STARTED transport (the T1b rule)");
    ur_check(cl.peer_count() == 0, "...and the restart dropped the old connection table (peers=%d)",
             cl.peer_count());
    const bool up2 = ur_wait([&] { return host.peer_count() == 1; }, 10000);
    ur_check(up2, "the host accepted a SECOND connection from the same client (peers=%d)",
             host.peer_count());
    ur_check(ur_hs_done(host) == 2,
             "...counted as a second completed handshake, not the old one resurrected (%ld)",
             ur_hs_done(host));

    // ---- 4. and it carries traffic ----------------------------------------------------------------
    // The link is only re-established if it MOVES something: a re-dial that completes a handshake
    // and then cannot deliver a byte would pass every count above.
    bool pinged = false, ponged = false;
    if (up2) {
        cl.send(MH_NET_BROADCAST, "PING", 4);
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 8000 && !ponged) {
            int           sender = -1;
            unsigned char buf[64];
            int           len = (int)sizeof(buf);
            if (!pinged && host.recv(&sender, buf, &len) && len == 4 && memcmp(buf, "PING", 4) == 0) {
                host.send(sender, "PONG", 4);
                pinged = true;
            }
            len = (int)sizeof(buf);
            if (pinged && cl.recv(&sender, buf, &len) && len == 4 && memcmp(buf, "PONG", 4) == 0)
                ponged = true;
            Sleep(5);
        }
    }
    ur_check(pinged, "the host received game traffic over the RE-DIALLED link");
    ur_check(ponged, "...and the client received the reply");

    // ---- 5. NEGATIVE: a dial that finds nobody must leave the transport dialable -------------------
    // The shape differs from the TCP suite's and the difference is the protocol's, not the test's:
    // UDP has no connect(), so start() cannot report "nothing is listening" -- it binds a socket,
    // sends a HELLO into the dark and gives up HS_BUDGET_MS later. What must hold is the same
    // property the S8 retry path depends on: the failure is not a wedge. After the dead dial the
    // endpoint has no peers, still calls itself started, and A THIRD DIAL AT THE REAL HOST WORKS.
    ur_cfg(cc, 1, base + 7, (unsigned short)(base + 2), -1, -1); // nothing listens on base+7
    ur_check(cl.start(cc, UR_PSK, true), "a dial into the dark is accepted (UDP cannot refuse it)");
    // The restart also released the second connection, which the host notices on its own timer.
    ur_check(ur_wait([&] { return host.peer_count() == 0; }, 8000),
             "the restart tore the second connection down on the HOST side too (peers=%d)",
             host.peer_count());
    Sleep(4200); // > HS_BUDGET_MS: the handshake gives up
    ur_check(cl.peer_count() == 0 && cl.started(),
             "a dial that found nobody leaves the transport started and peerless, not wedged "
             "(peers=%d, started=%d)",
             cl.peer_count(), (int)cl.started());
    ur_cfg(cc, 1, base, (unsigned short)(base + 1), -1, -1);
    ur_check(cl.start(cc, UR_PSK, true), "...and a THIRD dial, at the real host, is accepted");
    ur_check(ur_wait([&] { return host.peer_count() == 1; }, 10000),
             "the host accepted the third connection (peers=%d)", host.peer_count());
    ur_check(ur_hs_done(host) == 3, "three completed handshakes from one client process (%ld)",
             ur_hs_done(host));

    Counters kh;
    host.counters(kh);
    printf("     host: handshakes started %ld done %ld | mac-fail %ld malformed %ld wrong-conn %ld\n",
           kh.hs_started, kh.hs_done, kh.mac_fail, kh.malformed, kh.wrong_conn);
    ur_check(kh.mac_fail == 0, "no authentication failure across the three links (%ld)", kh.mac_fail);

    host.stop();
    cl.stop();
    printf("=== udprelinktest: %d checks, %d failures ===\n", g_ur_checks, g_ur_fails);
    return g_ur_fails ? 1 : 0;
}

static int run_selftest(int port) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char host_cmd[MAX_PATH + 64], cli_cmd[MAX_PATH + 64];
    wsprintfA(host_cmd, "\"%s\" host %d", exe, port);
    wsprintfA(cli_cmd, "\"%s\" client %d", exe, port);

    STARTUPINFOA        si;
    PROCESS_INFORMATION pih, pic;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    printf("=== selftest on port %d ===\n", port);
    if (!CreateProcessA(nullptr, host_cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pih)) {
        printf("failed to launch host (%lu)\n", GetLastError());
        return 10;
    }
    Sleep(400); // give the host time to bind+listen
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, cli_cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pic)) {
        printf("failed to launch client (%lu)\n", GetLastError());
        TerminateProcess(pih.hProcess, 99);
        return 11;
    }
    int hc = wait_exit(pih.hProcess, 15000);
    int cc = wait_exit(pic.hProcess, 15000);
    CloseHandle(pih.hProcess);
    CloseHandle(pih.hThread);
    CloseHandle(pic.hProcess);
    CloseHandle(pic.hThread);

    printf("=== host exit=%d  client exit=%d ===\n", hc, cc);
    if (hc == 0 && cc == 0) {
        printf("=== PASS: transport loopback round-trip succeeded ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}


// ---- netcfgtest: MH_NetConfig's relay fields (mp:R7a) --------------------------------------------
//
// WHAT R7a CHANGED, and what this suite pins. Before R7a a peer with `[net] relay` set had no direct
// mode: mh_net_udp.dll read the key itself and tunnelled EVERY connection. R7a moved the decision to
// mh.dll and made it travel in MH_NetConfig -- an appended `relay_addr` (empty = a direct dial) plus
// `relay_room`. Two properties of that contract are testable with no rig, and both are the failure a
// silent regression would be:
//
//   (1) THE CONFIG FIELD'S DEFAULT IS "NO RELAY". A zero-initialised MH_NetConfig -- what every
//       self-test, the force-entry path and a caller that forgot the field all get -- must carry an
//       EMPTY relay_addr, i.e. a direct dial. If the default ever became "relay", the whole point of
//       R7a (a direct mode exists again) would be undone for every caller that does not set the field.
//
//   (2) THE TWO FIELDS ARE APPENDED. The TCP module (mh_net.dll) and any reader built before R7a must
//       address every field ABOVE relay_addr at unchanged offsets and ignore the trailing bytes -- so
//       relay_addr must sit after rx_timeout_ms and relay_room must be the last member. An insertion
//       in the middle would move a field an older reader still reads by offset.
//
//   (3) THE TCP MODULE IS INDIFFERENT TO IT. net_selftest links the TCP transport (net_transport.cpp)
//       as its MH_Net_*; it has no relay and never reads relay_addr. Proven at runtime: a TCP HOST
//       init with relay_addr SET starts exactly as a plain direct host would (binds its port, no
//       relay, no crash). A build that started reading the field would have to do SOMETHING with a
//       relay address on a transport that cannot relay; doing nothing is the contract.
static int run_netcfgtest(int port) {
    int checks = 0, fails = 0;
#define NC_CK(cond, msg)                 \
    do {                                 \
        ++checks;                        \
        if (!(cond)) {                   \
            ++fails;                     \
            printf("  FAIL: %s\n", msg); \
        }                                \
    } while (0)

    // (1) absent = no relay.
    MH_NetConfig z;
    memset(&z, 0, sizeof(z));
    NC_CK(z.relay_addr[0] == '\0', "zero-init MH_NetConfig has an empty relay_addr (absent = no relay)");
    NC_CK(z.relay_room == 0, "zero-init MH_NetConfig has relay_room 0");

    // (2) ABI append: relay_addr after rx_timeout_ms, relay_room the last member.
    NC_CK(offsetof(MH_NetConfig, relay_addr) > offsetof(MH_NetConfig, rx_timeout_ms),
          "relay_addr is appended after rx_timeout_ms");
    NC_CK(offsetof(MH_NetConfig, relay_room) > offsetof(MH_NetConfig, relay_addr),
          "relay_room is appended after relay_addr");
    NC_CK(offsetof(MH_NetConfig, relay_room) + sizeof(z.relay_room) == sizeof(MH_NetConfig),
          "relay_room is the LAST member of MH_NetConfig (nothing trails it)");

    // (3) TCP module indifference: a host init carrying a relay address starts as a plain TCP host.
    MH_NetConfig h;
    memset(&h, 0, sizeof(h));
    h.role      = 0; // host
    h.port      = port;
    h.player_id = 0;
    h.peers     = 1;
    h.log       = 0;
    lstrcpynA(h.relay_addr, "203.0.113.9:7100", (int)sizeof(h.relay_addr)); // TEST-NET-3, never dialled
    h.relay_room      = 12345;
    const int started = MH_Net_InitEx(&h);
    NC_CK(started == 1, "TCP host init with relay_addr SET still starts (the TCP module ignores it)");
    NC_CK(MH_Net_IsStarted() == 1, "TCP host is listening after a relay_addr-bearing init");
    // The TCP module has no relay: PeerCount is 0 (no clients) and there is no tunnel concept at all.
    // The single positive above is the whole claim -- it accepted a relay-shaped config and behaved
    // like the direct host it is. (No MH_Net_Shutdown since fork F4B; the process exits here.)

#undef NC_CK
    printf("=== netcfgtest: %d checks, %d failures ===\n", checks, fails);
    if (fails == 0) {
        printf("=== PASS: MH_NetConfig relay fields -- default is no relay, appended, TCP-indifferent (mp:R7a) ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}


// ---- udpstatstest: the latency ARITHMETIC (mp:T3) ------------------------------------------------
//
// WHY AN OFFLINE SUITE FOR THIS AT ALL. Everything mp:T3 adds is measured on a link, and a link is
// the one thing this machine cannot stage: the rig clauses ("SRTT within 10% of 80 with the shim at
// 80 +/- 20", "the controller raises the lookahead within 2 s at 200 ms") each need two VMs, a
// middlebox and several minutes, and a run that comes back wrong cannot say WHICH of the estimator,
// the sampler or the controller was wrong. So the arithmetic under all three is an I/O-free header
// (mh_net_udp/udp_stats.h) and this suite asserts it against RFC 6298's own worked recurrence, a
// synthetic 80 +/- 20 stream, and the controller's three decisions -- in milliseconds, with no
// socket. The rig then proves the WIRING, which is the part only a rig can prove.
//
// The RFC 6298 vectors are computed by hand in the comments so a reader can check them against
// §2 rules 2.2/2.3 without running anything.
namespace {

int g_us_checks = 0, g_us_fails = 0;

void us_check(bool ok, const char *what, ...) {
    char    b[256];
    va_list ap;
    va_start(ap, what);
    vsnprintf(b, sizeof(b), what, ap);
    va_end(ap);
    ++g_us_checks;
    if (!ok) {
        ++g_us_fails;
        printf("  FAIL: %s\n", b);
    }
}

bool near_ms(double a, double b, double tol) {
    double d = a - b;
    if (d < 0.0) d = -d;
    return d <= tol;
}

// A deterministic generator, so a failing run is replayable and a passing one is not a lucky seed.
// (Numerical Recipes' LCG; the same shape udp_endpoint's synthetic loss uses.)
uint32_t us_rand(uint32_t &st) {
    st = st * 1664525u + 1013904223u;
    return st;
}

// ---- mp:P10: the two-peer coupling, offline ------------------------------------------------------
//
// EVERY OTHER ARM IN THIS SUITE DRIVES ONE CONTROLLER. That is what let the ratchet ship: the defect
// is not in any single decision -- each of them is locally correct -- it is in what two correct
// controllers do TO EACH OTHER through `committed = min(own horizon, the peer's ARRIVED horizon)`.
// So this drives two instances against each other with the coupling the field has, and the property
// asserted is about the PAIR's endpoint, not about one verdict.
//
// THE MODEL, and each piece is a line of the real system rather than a knob:
//   * each peer advertises `horizon = own clock + own lookahead` every frame
//     (turn_engine.cpp:438-441, the pump);
//   * an advertisement ARRIVES one one-way delay later, so what the receiver mins against is always
//     stale -- this is the whole reason the raw slack overstates the idle horizon;
//   * `committed = min(own horizon, the arrived peer horizon)` (turn_engine.cpp:205-213);
//   * the sim advances toward `committed`, but no faster than the machine can run it (`rate`), and
//     that per-peer rate asymmetry is what makes one peer sit ON its horizon while the other has
//     margin -- the field's host/joiner split;
//   * lateness and slack are sampled exactly as lateness_tick does (net_lockstep.cpp:860-915), into
//     the same LatenessWindow, and reduced by the same percentiles;
//   * a decision every AD_WINDOW_MS, through the real lookahead_decide.
//
// It is calibrated, not invented: at owd = 100 ms it reproduces the 2026-09-20 field corpus's own
// numbers -- the opening `slack 100 ms` with a starved tail on BOTH peers, and the walk to
// (400, 60) inside ~15 s.
struct TwoPeerSim {
    double cur_ms[2];   // the settled lookahead of each peer
    double rate_eff[2]; // game ms advanced per wall ms over the whole run
    int    n_grow[2];
    int    n_shrink[2];
    // mp:P12 -- the opening, measured the way the rig's `[adaptive]` line reports it: the starved
    // fraction of each of the first four DECIDED windows (window k is the one decision k was made on,
    // so window 1 is measured entirely at the start value), the first decision itself, and the starved
    // fraction of the first 10 s of wall time.
    double starved_win[2][4];
    int    first_verdict[2];
    double first_want[2];
    bool   first_full[2];
    double starved_10s[2];
};

struct SimPeerState {
    double                       c, S, rate, peer_h, t0, warm_t0;
    bool                         warm_seen;
    int                          clean;
    mh::netstats::LatenessWindow late, slack;
    int                          win_frames, win_starved, decided, frames_10s, starved_10s;
};

// `owd_ms` is the LINK. `feed_owd_ms` is what the controller is TOLD about it -- negative means
// "this transport cannot measure its link", which is bit-for-bit the pre-P10 decision, so the SAME
// code drives both arms and the regression arm is not a tautology about a #ifdef. The two being
// separate parameters is what lets an arm ask what a WRONG estimate costs.
TwoPeerSim two_peer_run(double start_a, double start_b, double rate_a, double rate_b, double owd_ms,
                        double feed_owd_ms, double secs = 120.0, double sim_ms = 20.0,
                        double floor_ms = 60.0, double ceil_ms = 400.0, bool p12 = false) {
    using namespace mh::netstats;
    const double DT = 10.0, WINDOW = 2000.0, WARMUP = 1500.0;
    enum { RING = 128 };
    SimPeerState p[2];
    double       q_at[2][RING], q_v[2][RING];
    int          q_head[2], q_tail[2];
    for (int i = 0; i < 2; ++i) {
        p[i].c         = 0.0;
        p[i].S         = i ? start_b : start_a;
        p[i].rate      = i ? rate_b : rate_a;
        p[i].peer_h    = 0.0;
        p[i].t0        = 0.0;
        p[i].warm_t0   = 0.0;
        p[i].warm_seen = false;
        p[i].clean     = 0;
        p[i].late.reset();
        p[i].slack.reset();
        p[i].win_frames = p[i].win_starved = p[i].decided = p[i].frames_10s = p[i].starved_10s = 0;
        q_head[i] = q_tail[i] = 0;
    }
    TwoPeerSim out;
    for (int i = 0; i < 2; ++i) {
        out.n_grow[i]        = 0;
        out.n_shrink[i]      = 0;
        out.first_verdict[i] = -1;
        out.first_want[i]    = 0.0;
        out.first_full[i]    = false;
        for (int k = 0; k < 4; ++k) out.starved_win[i][k] = -1.0;
    }
    for (double t = 0.0; t < secs * 1000.0; t += DT) {
        for (int i = 0; i < 2; ++i) { // the pump: advertise toward the other side
            const double h   = p[i].c + p[i].S;
            const int    o   = 1 - i;
            const int    nxt = (q_tail[o] + 1) % RING;
            if (nxt != q_head[o]) {
                q_at[o][q_tail[o]] = t + owd_ms;
                q_v[o][q_tail[o]]  = h;
                q_tail[o]          = nxt;
            }
        }
        for (int i = 0; i < 2; ++i) {
            while (q_head[i] != q_tail[i] && q_at[i][q_head[i]] <= t) {
                p[i].peer_h = q_v[i][q_head[i]];
                q_head[i]   = (q_head[i] + 1) % RING;
            }
            const double own_h     = p[i].c + p[i].S;
            const double committed = (p[i].peer_h > 0.0 && p[i].peer_h < own_h) ? p[i].peer_h : own_h;
            const double required  = p[i].c + sim_ms;
            ++p[i].win_frames;
            if (t < 10000.0) ++p[i].frames_10s;
            if (committed < required) {
                ++p[i].win_starved;
                if (t < 10000.0) ++p[i].starved_10s;
                p[i].late.push(-(int)sim_ms); // blocked this frame
            } else {
                p[i].late.push((int)(p[i].peer_h - required));
                double adv = committed - p[i].c;
                if (adv > p[i].rate * DT) adv = p[i].rate * DT;
                if (adv > 0.0) p[i].c += adv;
            }
            const int sl = (int)(own_h - committed);
            p[i].slack.push(sl > 0 ? sl : 0);
            if (!p[i].warm_seen && p[i].peer_h > 0.0) {
                p[i].warm_seen = true;
                p[i].warm_t0   = t;
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (t - p[i].t0 < WINDOW) continue;
            int         lp50 = 0, lt95 = 0, lt99 = 0, sp50 = 0, s95 = 0, s99 = 0;
            LookaheadIn in;
            in.cur_ms   = p[i].S;
            in.sim_ms   = sim_ms;
            in.floor_ms = floor_ms;
            in.ceil_ms  = ceil_ms;
            in.have     = p[i].late.count() >= AD_LATE_MIN_SAMPLES &&
                      p[i].late.percentiles(lp50, lt95, lt99);
            in.tail95_ms   = (double)lt95;
            in.clean_in    = p[i].clean;
            in.warm        = p[i].warm_seen && (t - p[i].warm_t0) >= WARMUP;
            in.slack_ms    = (p[i].slack.count() >= AD_LATE_MIN_SAMPLES &&
                           p[i].slack.percentiles(sp50, s95, s99))
                                 ? (double)sp50
                                 : 0.0;
            in.link_owd_ms = feed_owd_ms;
            // mp:P12 -- off unless the arm asks, so every pre-P12 arm drives the unchanged decision.
            in.samples           = p12 ? p[i].late.count() : 0;
            in.first_warm        = p12 && in.warm && p[i].decided == 0;
            const LookaheadOut d = lookahead_decide(in);
            p[i].clean           = d.clean_out;
            if (d.verdict == LA_GROW) ++out.n_grow[i];
            if (d.verdict == LA_SHRINK) ++out.n_shrink[i];
            if (in.warm && d.verdict != LA_NO_SAMPLES) {
                if (p[i].decided == 0) {
                    out.first_verdict[i] = d.verdict;
                    out.first_want[i]    = d.want_ms;
                    out.first_full[i]    = d.first_full;
                }
                if (p[i].decided < 4)
                    out.starved_win[i][p[i].decided] =
                        p[i].win_frames ? (double)p[i].win_starved / (double)p[i].win_frames : 0.0;
                ++p[i].decided;
            }
            p[i].win_frames = p[i].win_starved = 0;
            p[i].S                             = d.want_ms;
            p[i].late.reset();
            p[i].slack.reset();
            p[i].t0 = t;
        }
    }
    for (int i = 0; i < 2; ++i) {
        out.cur_ms[i]      = p[i].S;
        out.rate_eff[i]    = p[i].c / (secs * 1000.0);
        out.starved_10s[i] = p[i].frames_10s ? (double)p[i].starved_10s / (double)p[i].frames_10s : 0.0;
    }
    return out;
}

} // namespace

static int run_udpstatstest() {
    using namespace mh::netstats;
    printf("=== udpstatstest (T3: RFC 6298 / 3393 / 7680 + the lookahead decision) ===\n");
    g_us_checks = g_us_fails = 0;

    // ---- 1. RFC 6298 §2 rule 2.2 -- the FIRST measurement ----------------------------------------
    //   SRTT = R, RTTVAR = R/2.
    {
        RttEstimator e;
        e.reset();
        e.sample(1000.0);
        us_check(near_ms(e.srtt_ms, 1000.0, 1e-9), "rule 2.2: SRTT = R (%f)", e.srtt_ms);
        us_check(near_ms(e.rttvar_ms, 500.0, 1e-9), "rule 2.2: RTTVAR = R/2 (%f)", e.rttvar_ms);
        us_check(e.samples == 1, "one sample counted (%ld)", e.samples);
    }

    // ---- 2. RFC 6298 §2 rule 2.3 -- worked by hand, and the ORDER of the two assignments ---------
    //   start R  = 100  -> SRTT 100, RTTVAR 50            (rule 2.2)
    //   then  R' = 140:
    //       RTTVAR = 3/4 * 50  + 1/4 * |100 - 140| = 37.5 + 10  = 47.5   <- uses the OLD SRTT
    //       SRTT   = 7/8 * 100 + 1/8 * 140         = 87.5 + 17.5 = 105
    //   Computing SRTT first would give RTTVAR = 37.5 + 1/4*|105-140| = 46.25, so this pair of
    //   assertions is also the mutation check on the order: 47.5 passes, 46.25 does not.
    {
        RttEstimator e;
        e.reset();
        e.sample(100.0);
        e.sample(140.0);
        us_check(near_ms(e.rttvar_ms, 47.5, 1e-9),
                 "rule 2.3: RTTVAR uses the OLD SRTT -> 47.5, not 46.25 (%f)", e.rttvar_ms);
        us_check(near_ms(e.srtt_ms, 105.0, 1e-9), "rule 2.3: SRTT = 105 (%f)", e.srtt_ms);
        //   then R'' = 105 (== SRTT): RTTVAR = 3/4 * 47.5 = 35.625, SRTT unchanged at 105.
        e.sample(105.0);
        us_check(near_ms(e.rttvar_ms, 35.625, 1e-9), "rule 2.3: a zero-deviation sample decays "
                                                     "RTTVAR by 1/4 (%f)",
                 e.rttvar_ms);
        us_check(near_ms(e.srtt_ms, 105.0, 1e-9), "rule 2.3: SRTT stays at 105 (%f)", e.srtt_ms);
    }

    // ---- 3. THE RIG CLAUSE, OFFLINE: 80 ms +/- 20 ms -> SRTT within 10% of 80, RTTVAR non-zero ----
    // This is the same assertion tools/net_shim.py's `--delay 40 --jitter 20` run has to produce on
    // the rig; here the stream is synthetic so the claim is about the estimator alone.
    {
        RttEstimator e;
        e.reset();
        uint32_t st = 0x5EEDu;
        for (int i = 0; i < 400; ++i) e.sample(60.0 + (double)(us_rand(st) % 41u)); // 60..100, mean 80
        us_check(near_ms(e.srtt_ms, 80.0, 8.0), "80 +/- 20 stream: SRTT within 10%% of 80 (%f)",
                 e.srtt_ms);
        us_check(e.rttvar_ms > 1.0, "80 +/- 20 stream: RTTVAR is non-zero (%f)", e.rttvar_ms);
        us_check(e.ipdv_ms > 1.0, "80 +/- 20 stream: IPDV is non-zero (%f)", e.ipdv_ms);
    }

    // ---- 4. a PERFECTLY steady link: SRTT exact, RTTVAR and IPDV collapse ------------------------
    // The negative control for arm 3. If RTTVAR were, say, accumulating |R| instead of |SRTT - R|,
    // arm 3 would still pass and this one would not.
    {
        RttEstimator e;
        e.reset();
        for (int i = 0; i < 200; ++i) e.sample(80.0);
        us_check(near_ms(e.srtt_ms, 80.0, 1e-6), "steady link: SRTT == 80 (%f)", e.srtt_ms);
        us_check(e.rttvar_ms < 0.001, "steady link: RTTVAR collapses to ~0 (%f)", e.rttvar_ms);
        us_check(e.ipdv_ms < 0.001, "steady link: IPDV collapses to ~0 (%f)", e.ipdv_ms);
    }

    // ---- 5. IPDV is a PAIR statistic, RTTVAR is a deviation-from-mean one -------------------------
    // A 60/100 alternation has a constant pair delta of 40 and a mean of 80, so IPDV -> 40 while
    // RTTVAR -> 20. A single smoothed number could not tell those two links apart; the point of
    // carrying both is that it can.
    {
        RttEstimator e;
        e.reset();
        for (int i = 0; i < 400; ++i) e.sample((i % 2) ? 100.0 : 60.0);
        us_check(near_ms(e.ipdv_ms, 40.0, 1.0), "60/100 alternation: IPDV -> 40 (%f)", e.ipdv_ms);
        us_check(near_ms(e.rttvar_ms, 20.0, 2.0), "60/100 alternation: RTTVAR -> 20 (%f)",
                 e.rttvar_ms);
    }

    // ---- 6. the probe table: a matched echo measures, an unmatched one does NOT -------------------
    {
        PingTracker t;
        t.reset();
        const int64_t FREQ = 1000000; // a 1 MHz counter: 1 tick = 1 us
        t.on_sent(1000u, 0);
        us_check(near_ms(t.on_echo(1000u, 80000, FREQ), 80.0, 1e-9), "a matched echo measures 80 ms");
        us_check(t.on_echo(1000u, 90000, FREQ) < 0.0, "the SAME echo a second time is not a sample");
        us_check(t.on_echo(4242u, 90000, FREQ) < 0.0, "an echo we never sent is not a sample");
        // ...and a probe that aged out of the table is unmatchable rather than wrong.
        t.reset();
        for (uint32_t i = 0; i < (uint32_t)PING_TRACK + 2u; ++i) t.on_sent(i, (int64_t)i * 1000);
        us_check(t.on_echo(0u, 99999, FREQ) < 0.0, "a probe wrapped out of the table is not a sample");
        us_check(t.on_echo((uint32_t)PING_TRACK + 1u, 99999, FREQ) >= 0.0,
                 "...while the newest probe still matches");
    }

    // ---- 6b. the ECHO MATCH report (mp:T3b) -------------------------------------------------------
    // `on_echo` can say WHICH table entry it matched and how many probes are still outstanding, and
    // the endpoint prints both on every sample. That exists because the ~19 ms under-read was chased
    // for a session on the theory that an echo could be matched against a LATER probe's stamp -- the
    // only arithmetic by which a stopwatch started BEFORE a send can report less than the path
    // delivers. It cannot: the origin key advances once per ping interval and the scan takes the
    // first live entry carrying it. These checks are what makes that a tested property rather than a
    // reading of the loop, so a future edit to the scan cannot quietly reintroduce the theory.
    {
        PingTracker   t;
        EchoMatch     m;
        const int64_t FREQ = 1000000;
        t.reset();
        t.on_sent(10u, 0);
        t.on_sent(20u, 5000);
        t.on_sent(30u, 9000);
        us_check(near_ms(t.on_echo(20u, 45000, FREQ, &m), 40.0, 1e-9),
                 "the MIDDLE probe is measured against ITS OWN stamp, not a neighbour's");
        us_check(m.slot == 1, "...and the match reports slot 1 (%d)", m.slot);
        us_check(m.outstanding == 2, "...with the other two still outstanding (%d)", m.outstanding);
        us_check(t.on_echo(99u, 50000, FREQ, &m) < 0.0, "an unknown origin is not a sample");
        us_check(m.slot == -1, "...and reports no slot (%d)", m.slot);
        us_check(m.outstanding == 2, "...without consuming one (%d)", m.outstanding);
        // The out-param is OPTIONAL and the arithmetic must not depend on it: same table, no report.
        us_check(near_ms(t.on_echo(30u, 29000, FREQ), 20.0, 1e-9),
                 "the measurement is the same with no EchoMatch asked for");
    }

    // ---- 7. RFC 7680 loss over the 256-packet sequence window --------------------------------------
    {
        LossWindow w;
        w.reset();
        for (uint64_t i = 1; i <= 400; ++i) w.on_rx(i);
        us_check(w.loss_pm() == 0, "a complete sequence -> 0 pm (%d)", w.loss_pm());

        w.reset();
        for (uint64_t i = 1; i <= 400; ++i)
            if (i % 20 != 0) w.on_rx(i); // drop every 20th: 5% == 50 pm
        us_check(w.loss_pm() >= 40 && w.loss_pm() <= 60, "1-in-20 dropped -> ~50 pm (%d)",
                 w.loss_pm());

        // A REORDER IS NOT A LOSS. Deliver 1..40, then 45..50, then the missing 41..44 late: every
        // datagram arrived, so the ratio must be 0 even though the window saw holes on the way.
        w.reset();
        for (uint64_t i = 1; i <= 40; ++i) w.on_rx(i);
        for (uint64_t i = 45; i <= 120; ++i) w.on_rx(i);
        for (uint64_t i = 41; i <= 44; ++i) w.on_rx(i);
        us_check(w.loss_pm() == 0, "an in-window reorder is not loss (%d)", w.loss_pm());

        // Too little settled to say -> REFUSE with -1 rather than answer 0 pm off three packets.
        w.reset();
        for (uint64_t i = 1; i <= 5; ++i) w.on_rx(i);
        us_check(w.loss_pm() == -1, "too few packets -> -1, not a confident 0 (%d)", w.loss_pm());
    }

    // ---- 8. the lateness percentiles, and which end is the dangerous one ---------------------------
    {
        LatenessWindow lw;
        lw.reset();
        int p50 = 0, t95 = 0, t99 = 0;
        us_check(!lw.percentiles(p50, t95, t99), "an empty window reduces to nothing, not to 0");
        for (int i = 0; i < 100; ++i) lw.push(i); // 0..99 ms of margin
        us_check(lw.percentiles(p50, t95, t99), "a populated window reduces");
        us_check(p50 >= 48 && p50 <= 52, "p50 of 0..99 is ~50 (%d)", p50);
        us_check(t95 >= 3 && t95 <= 7, "tail95 is the LOW end (~5), not the high one (%d)", t95);
        us_check(t99 >= 0 && t99 <= 2, "tail99 is lower still (~1) (%d)", t99);
        // The ring is bounded: the oldest samples fall out, so a link that RECOVERED stops being
        // judged on the stall it had two minutes ago.
        lw.reset();
        for (int i = 0; i < LATE_WINDOW; ++i) lw.push(-500); // a stalled era
        for (int i = 0; i < LATE_WINDOW; ++i) lw.push(120);  // ...entirely overwritten
        us_check(lw.percentiles(p50, t95, t99) && t95 == 120,
                 "the window is bounded: an overwritten stall no longer binds (%d)", t95);
    }

    // ---- 9. the controller decision, which is the whole of mp:T3's behaviour change ----------------
    {
        LookaheadIn in;
        in.sim_ms   = 20.0; // the shipping sim_step_ms
        in.floor_ms = 60.0; // == AD_SIM_FLOOR_MULT * sim_ms, the effective floor at sim_step 20
        in.ceil_ms  = 400.0;
        in.clean_in = 0;
        // mp:T3c's two new inputs. The arms below (a)..(f) are T3's and are asserted UNCHANGED, which
        // is the point of setting these to "join is over, our horizon is the binding one": the T3c
        // behaviour has to be additional, not a retune of what the rig already accepted.
        in.warm     = true;
        in.slack_ms = 0.0;
        // mp:P10's new input, set to "this transport cannot measure its link" for every arm (a)..(j)
        // below, which is exactly what makes them assertions about the UNCHANGED decision: with no
        // one-way delay the grow gate stands down and `slack_ms` is read raw, i.e. the pre-P10 rule
        // bit for bit. Arms (k) and (l) are the ones that supply a link.
        in.link_owd_ms = -1.0;

        // (a) NOTHING MEASURED -> hold. Not "assume the worst", not "assume the best".
        in.cur_ms      = 100.0;
        in.have        = false;
        in.tail95_ms   = 0.0;
        LookaheadOut d = lookahead_decide(in);
        us_check(d.verdict == LA_NO_SAMPLES && near_ms(d.want_ms, 100.0, 1e-9),
                 "no samples -> hold at 100 (%f, verdict %d)", d.want_ms, d.verdict);

        // (b) THE 200 ms CLAUSE. The peer is arriving 200 ms past its deadline; the controller must
        // move on the FIRST decision, and the per-window cap is a doubling.
        in.cur_ms    = 100.0;
        in.have      = true;
        in.tail95_ms = -200.0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && near_ms(d.want_ms, 200.0, 1e-9),
                 "tail95 -200 -> grow, capped at one doubling (%f)", d.want_ms);
        in.cur_ms = d.want_ms;
        d         = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && near_ms(d.want_ms, 400.0, 1e-9),
                 "...and again, to the configured ceiling (%f)", d.want_ms);
        in.cur_ms = d.want_ms;
        d         = lookahead_decide(in);
        us_check(near_ms(d.want_ms, 400.0, 1e-9), "the ceiling holds (%f)", d.want_ms);

        // (c) a mild deficit grows by the MISSING MARGIN, not by a flat step -- with the 25% floor
        // still underneath it. grow_at = 0.5 * 20 = 10 ms; tail95 = 4 -> missing 6 ms, which is less
        // than 25% of 100, so the floor wins: 125.
        in.cur_ms    = 100.0;
        in.tail95_ms = 4.0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && near_ms(d.want_ms, 125.0, 1e-9),
                 "a 6 ms deficit still gets the 25%% minimum step (%f)", d.want_ms);
        //   ...and a 60 ms deficit gets 60 ms, not 25.
        in.tail95_ms = -50.0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && near_ms(d.want_ms, 160.0, 1e-9),
                 "a 60 ms deficit gets 60 ms (%f)", d.want_ms);

        // (d) THE HYSTERESIS BAND: between 0.5 and 1.5 sub-steps of margin -> hold, and bank no
        // credit toward shrinking. mp:T3c made the credit DECAY here rather than reset (the band is
        // one sub-step wide and the error term is on that grid, so a healthy link lands inside it
        // about half the time); a GROW is what still wipes it, asserted below.
        in.cur_ms    = 100.0;
        in.tail95_ms = 20.0; // 1.0 sub-step: inside [10, 30]
        in.clean_in  = 2;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_HOLD && near_ms(d.want_ms, 100.0, 1e-9) && d.clean_out == 1,
                 "inside the band -> hold, and the shrink credit decays by one (%f, clean %d)",
                 d.want_ms, d.clean_out);
        in.clean_in = 0;
        d           = lookahead_decide(in);
        us_check(d.clean_out == 0, "...and does not go negative (%d)", d.clean_out);
        in.clean_in  = 2; // a GROW, by contrast, still wipes it outright -- that is mp:P1's guard
        in.tail95_ms = -40.0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && d.clean_out == 0,
                 "...but starvation still zeroes it outright (%d)", d.clean_out);

        // (e) SHRINK SLOWLY: three consecutive comfortable windows before the first 4% step.
        in.cur_ms    = 100.0;
        in.tail95_ms = 80.0;
        in.clean_in  = 0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_HOLD && d.clean_out == 1, "comfortable window 1 -> hold");
        in.clean_in = d.clean_out;
        d           = lookahead_decide(in);
        us_check(d.verdict == LA_HOLD && d.clean_out == 2, "comfortable window 2 -> hold");
        in.clean_in = d.clean_out;
        d           = lookahead_decide(in);
        // mp:T3c changed the SIZE of that first step (50 ms of surplus buys back 25, not 4) but not
        // the hysteresis this arm exists for: it is still the THIRD consecutive comfortable window
        // that unlocks it, and the two before it moved nothing.
        us_check(d.verdict == LA_SHRINK && near_ms(d.want_ms, 75.0, 1e-9),
                 "comfortable window 3 -> the first step, half the surplus (%f)", d.want_ms);

        // (f) THE 0 ms CLAUSE: a clean link walks down and SETTLES ON THE CONFIGURED MINIMUM rather
        // than oscillating below it or stopping short of it -- and, since mp:T3c, arrives there in a
        // handful of windows instead of fifty. Under T3's flat 4% this loop took 47 shrink steps to
        // cross 400 -> 60 (0.96^47), i.e. ~98 s of real time at one 2 s window each; the whole point
        // of T3c is that the same clean link is done inside the 30 s the rig clause allows.
        in.cur_ms         = 400.0;
        in.clean_in       = 0;
        int    moves      = 0;
        int    first_at   = -1;
        int    settled_at = -1;
        double worst_cut  = 0.0;
        for (int w = 0; w < 500; ++w) {
            // A zero-delay link: the peer's horizon is a whole lookahead ahead of the sim, so the
            // margin IS the lookahead less the sub-step the deadline is measured at.
            in.tail95_ms = in.cur_ms - in.sim_ms;
            d            = lookahead_decide(in);
            in.clean_in  = d.clean_out;
            if (!near_ms(d.want_ms, in.cur_ms, 1e-9)) {
                if (first_at < 0) first_at = w;
                ++moves;
                const double cut = 1.0 - d.want_ms / in.cur_ms;
                if (cut > worst_cut) worst_cut = cut;
            }
            us_check(d.verdict != LA_GROW, "a clean link never grows (window %d)", w);
            in.cur_ms = d.want_ms;
            if (settled_at < 0 && near_ms(in.cur_ms, 60.0, 1e-9)) settled_at = w;
        }
        us_check(near_ms(in.cur_ms, 60.0, 1e-9),
                 "a clean link settles exactly on the configured minimum, and STAYS there over 500 "
                 "windows (%f)",
                 in.cur_ms);
        us_check(first_at >= AD_LATE_SHRINK_AFTER - 1,
                 "...and could not start before AD_LATE_SHRINK_AFTER clean windows (first move at "
                 "%d)",
                 first_at);
        us_check(settled_at >= 0 && settled_at <= 10,
                 "mp:T3c -- and reaches it inside 10 windows from the 400 ms ceiling, not 49 (%d)",
                 settled_at);
        us_check(worst_cut <= 1.0 - AD_LATE_SHRINK_MAX + 1e-9,
                 "...with no single window giving back more than the bound (worst %.3f vs %.3f)",
                 worst_cut, 1.0 - AD_LATE_SHRINK_MAX);

        // ---- (g) mp:T3c: THE JOIN WARM-UP -----------------------------------------------------
        // The measured defect: a client is genuinely starved for its first half-second because the
        // host has not advertised yet, so the samples are honest and the verdict off them (GROW) is
        // correct -- about a regime that is over. While `warm` is false NOTHING moves, whatever the
        // samples say, and no shrink credit is banked either.
        in.cur_ms    = 100.0;
        in.have      = true;
        in.warm      = false;
        in.clean_in  = 2;      // ...even with two comfortable windows already behind it
        in.tail95_ms = -260.0; // the real join reading: tail99 -265 ms on the rig, 2026-09-18
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_WARMUP && near_ms(d.want_ms, 100.0, 1e-9) && d.clean_out == 0,
                 "inside the warm-up a -260 ms tail moves nothing and banks nothing (%f, %d, %d)",
                 d.want_ms, d.verdict, d.clean_out);
        in.tail95_ms = 400.0;                // ...and it is not a "hold high" either: a comfortable reading is
        d            = lookahead_decide(in); // equally not acted on
        us_check(d.verdict == LA_WARMUP && near_ms(d.want_ms, 100.0, 1e-9),
                 "...and a comfortable one does not shrink inside it either (%f)", d.want_ms);
        // The warm-up does NOT disable the floor/ceiling clamp: a value out of band is still illegal.
        in.cur_ms = 10.0;
        d         = lookahead_decide(in);
        us_check(near_ms(d.want_ms, 60.0, 1e-9),
                 "...but the floor still applies inside the warm-up (%f)", d.want_ms);
        // Once warm, the SAME starved reading is acted on -- the warm-up delays the first decision,
        // it does not suppress growth.
        in.cur_ms    = 100.0;
        in.warm      = true;
        in.tail95_ms = -260.0;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && near_ms(d.want_ms, 200.0, 1e-9),
                 "...and the moment it ends the same reading grows (%f)", d.want_ms);

        // ---- (h) mp:T3c: THE PROPORTIONAL SHRINK, AND WHY IT CANNOT OVERSHOOT ------------------
        // Half the surplus over the shrink threshold. The 4% step is the FLOOR, so a small surplus
        // still behaves exactly as it did under T3 -- the two forms need no threshold between them.
        in.clean_in  = AD_LATE_SHRINK_AFTER - 1; // credit already banked; this window shrinks
        in.cur_ms    = 100.0;
        in.tail95_ms = 105.0; // the rig's own reading at cur=100 on a clean LAN, 2026-09-18
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_SHRINK && near_ms(d.want_ms, 62.5, 1e-9),
                 "a 75 ms surplus gives back half of it in one window, not 4 ms (%f)", d.want_ms);
        in.clean_in  = AD_LATE_SHRINK_AFTER - 1;
        in.cur_ms    = 100.0;
        in.tail95_ms = 33.0; // ...and a 3 ms surplus is still the flat 4%, unchanged from T3
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_SHRINK && near_ms(d.want_ms, 96.0, 1e-9),
                 "a 3 ms surplus is still the 4%% step -- the proportional form is self-gating (%f)",
                 d.want_ms);
        // THE NO-OVERSHOOT PROPERTY, asserted rather than argued, and asserted about the RIGHT term.
        // The flat 4% step has never had this property (4% of a 400 ms lookahead is 16 ms, which is
        // more than a 1 ms surplus) and T3c does not change it -- what T3c must not do is INTRODUCE
        // an overshoot. So: for every surplus, the step taken is at most the larger of the two
        // candidates, and whenever the proportional one is the larger, the worst case it can leave
        // behind -- a margin that tracks the lookahead one for one -- is still above the band.
        {
            int bad = 0;
            for (int t = 31; t <= 4000; ++t) {
                in.clean_in        = AD_LATE_SHRINK_AFTER - 1;
                in.cur_ms          = 400.0;
                in.tail95_ms       = (double)t;
                d                  = lookahead_decide(in);
                const double moved = in.cur_ms - d.want_ms;
                const double flat  = in.cur_ms * (1.0 - AD_LATE_SHRINK);
                const double prop  = AD_LATE_SHRINK_FRAC * (in.tail95_ms - AD_LATE_SHRINK_MULT * in.sim_ms);
                const double most  = (flat > prop) ? flat : prop;
                if (moved > most + 1e-9) ++bad;
                if (prop > flat && !(in.tail95_ms - moved > AD_LATE_SHRINK_MULT * in.sim_ms - 1e-9))
                    ++bad;
            }
            us_check(bad == 0,
                     "over surpluses 31..4000 ms the proportional term never overshoots the band "
                     "and never exceeds half the surplus (%d violations)",
                     bad);
        }

        // ---- (i) mp:T3c: UNUSED HORIZON, the surplus the lateness tail cannot see --------------
        // The rig's client: its own horizon is 120 ms while the match runs on the host's, so it
        // measures the HOST's margin (a quantised 20/40 ms, straddling the 30 ms shrink threshold)
        // and reads its own 60 ms of waste as nothing at all.
        in.warm      = true;
        in.cur_ms    = 120.0;
        in.tail95_ms = 20.0; // INSIDE the band -- under T3 this zeroed the shrink credit
        in.slack_ms  = 0.0;
        in.clean_in  = 2;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_HOLD && d.clean_out == 1,
                 "with no slack an in-band tail is not evidence of surplus (%d)", d.clean_out);
        in.slack_ms = 60.0; // ...and with 60 ms of our own horizon going unused, it does not
        d           = lookahead_decide(in);
        us_check(d.verdict == LA_SHRINK && near_ms(d.want_ms, 90.0, 1e-9),
                 "unused horizon is its own evidence of surplus: shrink by half of it (%f, %d)",
                 d.want_ms, d.verdict);
        // Slack never outranks real starvation: a grow reading still grows, and still resets.
        in.tail95_ms = -100.0;
        in.clean_in  = 2;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_GROW && d.clean_out == 0,
                 "...but starvation still outranks it (%d)", d.verdict);
        // Below the AD_SLACK_MULT threshold a sliver of slack is not "we are not binding".
        in.tail95_ms = 20.0;
        in.slack_ms  = 5.0; // < 0.5 sub-step
        in.clean_in  = 2;
        d            = lookahead_decide(in);
        us_check(d.verdict == LA_HOLD && d.clean_out == 1,
                 "a sliver of slack is not evidence (%d, clean %d)", d.verdict, d.clean_out);

        // ---- (j) mp:T3c: NO OSCILLATION ON A STEP-UP / STEP-DOWN SEQUENCE ----------------------
        // The synthetic link the rig arm mirrors: clean, then 200 ms injected, then clean again. The
        // properties that must hold across the whole sequence are (1) it ends on the floor, (2) it
        // covers the bad era, and (3) it never reverses direction inside one era -- a shrink
        // immediately followed by a grow (or the reverse) is the oscillation P1 measured and the
        // bound exists to prevent.
        in.warm       = true;
        in.slack_ms   = 0.0;
        in.cur_ms     = 100.0;
        in.clean_in   = 0;
        int    flips  = 0;
        int    lastv  = LA_HOLD;
        double peak   = 0.0;
        double inject = 0.0;
        for (int w = 0; w < 120; ++w) {
            inject = (w >= 20 && w < 60) ? 200.0 : 0.0;
            // margin = what this lookahead buys, less the injected one-way delay
            in.tail95_ms = in.cur_ms - in.sim_ms - inject;
            d            = lookahead_decide(in);
            in.clean_in  = d.clean_out;
            if ((d.verdict == LA_GROW && lastv == LA_SHRINK) ||
                (d.verdict == LA_SHRINK && lastv == LA_GROW))
                ++flips;
            if (d.verdict == LA_GROW || d.verdict == LA_SHRINK) lastv = d.verdict;
            in.cur_ms = d.want_ms;
            if (w >= 20 && w < 60 && in.cur_ms > peak) peak = in.cur_ms;
        }
        us_check(flips <= 2,
                 "a step up and a step down are two direction changes, not a wobble (%d)", flips);
        us_check(peak >= 200.0, "...the injected era is actually covered (peak %f)", peak);
        us_check(near_ms(in.cur_ms, 60.0, 1e-9),
                 "...and the link is handed back to the floor afterwards (%f)", in.cur_ms);

        // ---- (k) mp:P10: THE GROW GATE, and that it reads a DELAY-CORRECTED slack ---------------
        // The state the field's host sat in for every one of its 25 sessions: starved tail, and an
        // own horizon far above the one the match is running on. Growing there cannot raise our own
        // COMMITTED (it is a min and we are not the min), so the decision must not be a grow.
        in.warm        = true;
        in.clean_in    = 0;
        in.cur_ms      = 344.0;
        in.tail95_ms   = -20.0; // blocked: the pre-P10 rule grows on this, hard
        in.slack_ms    = 320.0;
        in.link_owd_ms = -1.0; // ...and with no link measurement it still does, unchanged
        d              = lookahead_decide(in);
        us_check(d.verdict == LA_GROW,
                 "with no link measurement a starved window still grows -- pre-P10, bit for bit "
                 "(%d)",
                 d.verdict);
        in.link_owd_ms = 104.0; // the field's link: SRTT ~208, RTTVAR ~1..4
        d              = lookahead_decide(in);
        us_check(d.verdict != LA_GROW,
                 "mp:P10 -- 320 ms of slack against a 104 ms link is 216 ms of horizon nobody can "
                 "use: not a grow (%d, %f)",
                 d.verdict, d.want_ms);
        us_check(d.want_ms <= in.cur_ms + 1e-9,
                 "...and the gate falls THROUGH to the shrink arm rather than stranding the value "
                 "(%f, was %f)",
                 d.want_ms, in.cur_ms);
        // THE CORRECTION IS THE WHOLE POINT, so assert the case it exists for: the pair's opening
        // window on a slow link, where BOTH peers read a slack of a full one-way delay while both
        // are genuinely starved. A `&& !slack` on the RAW slack refuses here and deadlocks the pair.
        in.cur_ms      = 100.0;
        in.tail95_ms   = -31.0; // the field's own first-window reading, both peers
        in.slack_ms    = 100.0; // ...and its own first-window slack, both peers
        in.link_owd_ms = 104.0;
        d              = lookahead_decide(in);
        us_check(d.verdict == LA_GROW,
                 "mp:P10 -- a slack that is only the flight time is NOT idle horizon: the opening "
                 "window on a 205 ms link still grows (%d, %f)",
                 d.verdict, d.want_ms);
        // The dead-band still applies, on the corrected quantity.
        in.slack_ms = 104.0 + 0.4 * in.sim_ms; // under AD_SLACK_MULT of idle horizon
        d           = lookahead_decide(in);
        us_check(d.verdict == LA_GROW, "...and a sliver over the delay is still not evidence (%d)",
                 d.verdict);
        in.slack_ms = 104.0 + 0.6 * in.sim_ms; // over it
        d           = lookahead_decide(in);
        us_check(d.verdict != LA_GROW, "...while half a sub-step over it is (%d)", d.verdict);
        // The SHRINK arm reads the same corrected quantity -- one signal, not two. 60 ms of idle
        // horizon over a 104 ms link buys back half of the 60, not half of the 164.
        in.cur_ms      = 200.0;
        in.tail95_ms   = 20.0; // inside the band, so only the slack term can move this
        in.slack_ms    = 164.0;
        in.link_owd_ms = 104.0;
        in.clean_in    = AD_LATE_SHRINK_AFTER - 1;
        d              = lookahead_decide(in);
        us_check(d.verdict == LA_SHRINK && near_ms(d.want_ms, 170.0, 1e-9),
                 "mp:P10 -- the shrink gives back half the IDLE horizon, not half the raw slack "
                 "(%f, expected 170)",
                 d.want_ms);

        // ---- (l) mp:P10: THE TWO-PEER RATCHET ---------------------------------------------------
        // The regression arm proper. Two controllers, coupled, from the same start on the field's
        // own link. Asserted BOTH ways so it cannot pass by accident: the pre-P10 decision must
        // reach (ceiling, floor), and the corrected one must not. A mutation that removes the grow
        // gate turns the second half red; a mutation that makes the gate unconditional turns the
        // throughput clause red.
        {
            const double OWD  = 100.0;
            TwoPeerSim   old_ = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, -1.0);
            us_check(near_ms(old_.cur_ms[0], 400.0, 1e-9) && near_ms(old_.cur_ms[1], 60.0, 1e-9),
                     "mp:P10 -- the PRE-P10 rule ratchets two equal peers to (ceiling, floor): "
                     "%.0f / %.0f (expected 400 / 60)",
                     old_.cur_ms[0], old_.cur_ms[1]);
            TwoPeerSim fixed_ = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD);
            // The pair's endpoint is the whole finding, so it goes on stdout whether or not the
            // arm passes -- a green run that cannot say WHAT it settled on is not evidence.
            printf("  [P10] two-peer, owd %.0f ms, start 100/100: pre-P10 %.0f/%.0f (%.3f/%.3f x) "
                   "-> corrected %.0f/%.0f (%.3f/%.3f x)\n",
                   OWD, old_.cur_ms[0], old_.cur_ms[1], old_.rate_eff[0], old_.rate_eff[1],
                   fixed_.cur_ms[0], fixed_.cur_ms[1], fixed_.rate_eff[0], fixed_.rate_eff[1]);
            us_check(fixed_.cur_ms[0] < 400.0 - 1e-9 && fixed_.cur_ms[1] > 60.0 + 1e-9,
                     "...and the corrected one does not: %.0f / %.0f", fixed_.cur_ms[0],
                     fixed_.cur_ms[1]);
            // "Within one shrink bound of each other" -- the row's own acceptance clause. One
            // window may take at most (1 - AD_LATE_SHRINK_MAX) off a value, so two peers further
            // apart than that are on different trajectories, not on one.
            const double hi    = (fixed_.cur_ms[0] > fixed_.cur_ms[1]) ? fixed_.cur_ms[0] : fixed_.cur_ms[1];
            const double lo    = (fixed_.cur_ms[0] > fixed_.cur_ms[1]) ? fixed_.cur_ms[1] : fixed_.cur_ms[0];
            const double bound = hi * (1.0 - AD_LATE_SHRINK_MAX);
            us_check(hi - lo <= bound + 1e-9,
                     "...and they settle within one shrink bound of each other (%.0f - %.0f = %.0f, "
                     "bound %.0f)",
                     hi, lo, hi - lo, bound);
            // AND IT COSTS NOTHING. This is the clause that stops the gate being made unconditional:
            // a refusal on the RAW slack passes every assertion above and deadlocks the pair at its
            // start value, which only a throughput clause can see.
            us_check(fixed_.rate_eff[0] >= old_.rate_eff[0] - 0.01 &&
                         fixed_.rate_eff[1] >= old_.rate_eff[1] - 0.01,
                     "...at no cost in sim rate (%.3f / %.3f vs %.3f / %.3f)", fixed_.rate_eff[0],
                     fixed_.rate_eff[1], old_.rate_eff[0], old_.rate_eff[1]);
            // A VALUE CARRIED IN FROM A PREVIOUS MATCH IS GIVEN BACK. The per-match reset in
            // lateness_tick clears the lateness windows but not g_lockstep_step, which is why the
            // field's 60s and 400s only ever appear in a process's SECOND match. The gate must not
            // strand one: this is the clause the `hold` version of the fix failed.
            TwoPeerSim carried = two_peer_run(400.0, 60.0, 1.25, 1.00, OWD, OWD);
            us_check(carried.cur_ms[0] < 400.0 - 1e-9 && carried.cur_ms[1] > 60.0 + 1e-9,
                     "mp:P10 -- a carried (400, 60) is walked back, not stranded (%.0f / %.0f)",
                     carried.cur_ms[0], carried.cur_ms[1]);
            // A CLEAN LAN IS UNTOUCHED: the correction is a subtraction of a delay that is not
            // there, so a 5 ms link still walks down to the floor.
            TwoPeerSim lan = two_peer_run(100.0, 100.0, 1.25, 1.00, 5.0, 5.0);
            us_check(near_ms(lan.cur_ms[0], 60.0, 1e-9) && near_ms(lan.cur_ms[1], 60.0, 1e-9),
                     "...and a 5 ms link still settles both peers on the floor (%.0f / %.0f)",
                     lan.cur_ms[0], lan.cur_ms[1]);
            // THE ESTIMATE IS BIASED HIGH ON PURPOSE, and this is the arm that says why. The two
            // ways of being wrong are not symmetric: told HALF the true delay the pair loses a
            // quarter of its throughput (the correction then reads flight time as idle horizon and
            // the gate fires on a link that genuinely needs the lookahead); told TWICE it simply
            // returns some of the latency win and degrades toward the pre-P10 behaviour. That
            // asymmetry is the entire reason the caller passes RFC 6298's upper bound halved,
            // (SRTT + 4*RTTVAR)/2, rather than a centred estimate -- so assert it here, where a
            // later "tidy the bias away" would have to red a check to land.
            TwoPeerSim under = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD * 0.5);
            TwoPeerSim over  = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD * 2.0);
            us_check(under.rate_eff[0] < fixed_.rate_eff[0] - 0.05,
                     "mp:P10 -- HALF the true delay costs real throughput (%.3f vs %.3f): the "
                     "estimate must be biased high",
                     under.rate_eff[0], fixed_.rate_eff[0]);
            us_check(over.rate_eff[0] >= old_.rate_eff[0] - 0.01,
                     "...while TWICE it costs none (%.3f vs %.3f) -- it only gives latency back",
                     over.rate_eff[0], old_.rate_eff[0]);
            us_check(over.cur_ms[0] > fixed_.cur_ms[0],
                     "...and that is what 'gives latency back' means: %.0f vs %.0f ms of lookahead",
                     over.cur_ms[0], fixed_.cur_ms[0]);
        }

        // ---- (m) mp:P12: THE FIRST POST-WARM-UP DECISION MAY TAKE THE WHOLE PROPORTIONAL STEP -----
        // The decision first, with the rig's own numbers (--shim-delay 200: tail95 -297 host / -359
        // client, n 47+), then the two-peer opening it buys, then the LAN arm it must not touch.
        {
            LookaheadIn f;
            f.cur_ms       = 100.0;
            f.sim_ms       = 20.0;
            f.floor_ms     = 60.0;
            f.ceil_ms      = 400.0;
            f.have         = true;
            f.clean_in     = 0;
            f.warm         = true;
            f.slack_ms     = 0.0;
            f.link_owd_ms  = -1.0;
            f.samples      = 47;
            f.first_warm   = true;
            f.tail95_ms    = -200.0; // want = 100 + (10 + 200) = 310: over one doubling, under the ceiling
            LookaheadOut m = lookahead_decide(f);
            us_check(m.verdict == LA_GROW && near_ms(m.want_ms, 310.0, 1e-9) && m.first_full,
                     "mp:P12 -- the first decision takes the whole proportional step (%f, full %d)",
                     m.want_ms, (int)m.first_full);
            f.tail95_ms = -297.0; // the rig's host: want 407, and the ceiling is what bounds it
            m           = lookahead_decide(f);
            us_check(m.verdict == LA_GROW && near_ms(m.want_ms, 400.0, 1e-9),
                     "...the rig's tail95 -297 lands on the 400 ms ceiling, not on 200 (%f)", m.want_ms);
            f.tail95_ms  = -200.0;
            f.first_warm = false; // every later window keeps the doubling cap
            m            = lookahead_decide(f);
            us_check(m.verdict == LA_GROW && near_ms(m.want_ms, 200.0, 1e-9) && !m.first_full,
                     "...a LATER window with the same tail is still capped at one doubling (%f)", m.want_ms);
            f.first_warm = true;
            f.samples    = AD_FIRST_GROW_MIN_SAMPLES - 1; // a thin tail keeps the cap
            m            = lookahead_decide(f);
            us_check(near_ms(m.want_ms, 200.0, 1e-9) && !m.first_full,
                     "...and so is a first window with %d samples (%f)", f.samples, m.want_ms);
            f.samples = AD_FIRST_GROW_MIN_SAMPLES;
            m         = lookahead_decide(f);
            us_check(near_ms(m.want_ms, 310.0, 1e-9), "...while %d samples is enough (%f)", f.samples,
                     m.want_ms);
            // INERT WHERE THE CAP DOES NOT BIND, i.e. on a LAN: a first window with a mild deficit, or
            // with margin to spare, decides exactly what the pre-P12 rule decides.
            const double lan_tails[] = {4.0, 20.0, 40.0, -40.0};
            bool         same        = true;
            for (double tl : lan_tails) {
                f.tail95_ms          = tl;
                f.first_warm         = true;
                const LookaheadOut a = lookahead_decide(f);
                f.first_warm         = false;
                const LookaheadOut b = lookahead_decide(f);
                if (a.verdict != b.verdict || a.want_ms != b.want_ms || a.clean_out != b.clean_out || a.first_full)
                    same = false;
            }
            us_check(same, "...and where the doubling cap does not bind the first decision is the pre-P12 one "
                           "bit for bit (tails 4 / 20 / 40 / -40)");

            // THE OPENING, two coupled peers on the rig's link (one-way 200 ms), P10's gate armed as
            // shipped. Window 1 is measured at the start value BEFORE any decision, so P12 cannot move
            // it (asserted: identical) -- what it moves is every window after the first decision.
            const double OWD = 200.0;
            TwoPeerSim   off = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD, 120.0, 20.0, 60.0, 400.0, false);
            TwoPeerSim   on  = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD, 120.0, 20.0, 60.0, 400.0, true);
            for (int i = 0; i < 2; ++i) {
                printf("  [P12] owd %.0f peer %d: first %.0f -> %.0f (full %d); starved win1..4 "
                       "%.0f/%.0f/%.0f/%.0f%% -> %.0f/%.0f/%.0f/%.0f%%, first 10 s %.1f%% -> %.1f%%, "
                       "settled %.0f -> %.0f, rate %.3f -> %.3f x\n",
                       OWD, i, 100.0, on.first_want[i], (int)on.first_full[i], 100 * off.starved_win[i][0],
                       100 * off.starved_win[i][1], 100 * off.starved_win[i][2], 100 * off.starved_win[i][3],
                       100 * on.starved_win[i][0], 100 * on.starved_win[i][1], 100 * on.starved_win[i][2],
                       100 * on.starved_win[i][3], 100 * off.starved_10s[i], 100 * on.starved_10s[i],
                       off.cur_ms[i], on.cur_ms[i], off.rate_eff[i], on.rate_eff[i]);
            }
            const TwoPeerSim lan_off = two_peer_run(100.0, 100.0, 1.25, 1.00, 0.5, 0.5, 120.0, 20.0, 60.0, 400.0, false);
            const TwoPeerSim lan_on  = two_peer_run(100.0, 100.0, 1.25, 1.00, 0.5, 0.5, 120.0, 20.0, 60.0, 400.0, true);
            for (int i = 0; i < 2; ++i)
                printf("  [P12] LAN peer %d: first verdict %d want %.0f (full %d); settled %.0f -> %.0f\n", i,
                       lan_on.first_verdict[i], lan_on.first_want[i], (int)lan_on.first_full[i],
                       lan_off.cur_ms[i], lan_on.cur_ms[i]);
            // THE WINDOW THE ROW CALLS "THE FIRST" IS OUT OF P12'S REACH, and this says so in code:
            // it is measured at the start value, before the decision this change touches.
            us_check(off.starved_win[0][0] == on.starved_win[0][0] && off.starved_win[1][0] == on.starved_win[1][0],
                     "mp:P12 -- window 1 (measured at the start value, BEFORE the first decision) is "
                     "unchanged: %.3f/%.3f vs %.3f/%.3f",
                     off.starved_win[0][0], off.starved_win[1][0], on.starved_win[0][0], on.starved_win[1][0]);
            us_check(on.first_full[0] && on.first_full[1],
                     "...the first decision on the 200 ms link took the uncapped step on both peers");
            us_check(on.starved_win[0][1] <= off.starved_win[0][1] && on.starved_win[1][1] <= off.starved_win[1][1] &&
                         (on.starved_win[0][1] < off.starved_win[0][1] || on.starved_win[1][1] < off.starved_win[1][1]),
                     "...and the window AFTER it starves less (%.3f/%.3f vs %.3f/%.3f)", on.starved_win[0][1],
                     on.starved_win[1][1], off.starved_win[0][1], off.starved_win[1][1]);
            us_check(on.starved_10s[0] <= off.starved_10s[0] && on.starved_10s[1] <= off.starved_10s[1],
                     "...so the first 10 s starve no more on either peer (%.3f/%.3f vs %.3f/%.3f)",
                     on.starved_10s[0], on.starved_10s[1], off.starved_10s[0], off.starved_10s[1]);
            us_check(on.rate_eff[0] >= off.rate_eff[0] - 0.005 && on.rate_eff[1] >= off.rate_eff[1] - 0.005,
                     "...at no cost in sim rate (%.3f/%.3f vs %.3f/%.3f)", on.rate_eff[0], on.rate_eff[1],
                     off.rate_eff[0], off.rate_eff[1]);
            // The pair's SUM, not each peer: under P10's coupling WHICH peer ends up carrying the
            // lookahead is path-dependent (the printout above shows the two swapping), and the latency
            // the pair pays is the sum.
            us_check(on.cur_ms[0] + on.cur_ms[1] <= 1.05 * (off.cur_ms[0] + off.cur_ms[1]),
                     "...and the pair settles on no more total lookahead (%.0f vs %.0f ms, +5%% allowed)",
                     on.cur_ms[0] + on.cur_ms[1], off.cur_ms[0] + off.cur_ms[1]);
            // THE LAN ARM: the first decision there does not grow, and the whole run is bit-identical.
            us_check(lan_on.first_verdict[0] != LA_GROW && lan_on.first_verdict[1] != LA_GROW,
                     "mp:P12 LAN -- the first decision on a 0.5 ms link does not grow (verdicts %d / %d)",
                     lan_on.first_verdict[0], lan_on.first_verdict[1]);
            us_check(lan_on.cur_ms[0] == lan_off.cur_ms[0] && lan_on.cur_ms[1] == lan_off.cur_ms[1] &&
                         lan_on.rate_eff[0] == lan_off.rate_eff[0] && lan_on.rate_eff[1] == lan_off.rate_eff[1] &&
                         lan_on.n_grow[0] == lan_off.n_grow[0] && lan_on.n_grow[1] == lan_off.n_grow[1],
                     "...and the whole LAN run is bit-identical with and without P12");
        }

        // ---- (n) mp:P14: THE START LOOKAHEAD, SEEDED FROM THE LOBBY RTT ----------------------------
        // lookahead_start (mh_net_udp/lookahead_start.h) against the three links the row names, then
        // the two-peer opening it buys at the rig's 200 ms one-way, then P12's guard re-checked under
        // the seeded start. Floor 60 / ceiling 400 / fallback 100 are the shipping values.
        {
            const double FL = 60.0, CE = 400.0, FB = 100.0;
            double       rtt[2];
            int          smp[2];
            // LAN: the rig's clean link reads SRTT ~1 ms -> the floor band (P11: both peers settle 60).
            rtt[0]              = 1.0;
            smp[0]              = 5;
            LookaheadStartOut o = lookahead_start(rtt, smp, 1, true, FL, CE, FB);
            us_check(o.reason == LS_START_SEEDED && o.start_ms >= FL && o.start_ms <= FL + 2.0,
                     "mp:P14 -- a LAN RTT (1 ms) seeds the floor band, not 100 (%f)", o.start_ms);
            // 400 ms RTT, the rig's --shim-delay 200 at 4 lobby pings (shim200 host log: srtt 405.6 ms
            // at sample 4). Settled band there 223-313, the host's tail95 reaching the hold band at
            // ~300-312 -- the start must land in the top of it, not at 100 and not at the ceiling.
            rtt[0] = 405.6;
            smp[0] = 4;
            o      = lookahead_start(rtt, smp, 1, true, FL, CE, FB);
            us_check(o.reason == LS_START_SEEDED && o.start_ms >= 280.0 && o.start_ms <= 320.0 &&
                         near_ms(o.rtt_ms, 405.6, 1e-9) && o.samples == 4,
                     "mp:P14 -- a 400 ms RTT seeds near the link's settled band [280, 320] (%f)", o.start_ms);
            // No samples, too few, or a transport that cannot measure: the configured start, unclamped.
            smp[0] = 0;
            o      = lookahead_start(rtt, smp, 1, true, FL, CE, FB);
            us_check(o.reason == LS_START_NO_RTT && o.start_ms == FB && o.peer == -1,
                     "mp:P14 -- no RTT samples -> the 100 ms fallback (%f)", o.start_ms);
            smp[0] = AD_START_MIN_RTT_SAMPLES - 1;
            o      = lookahead_start(rtt, smp, 1, true, FL, CE, FB);
            us_check(o.reason == LS_START_NO_RTT && o.start_ms == FB,
                     "...and so does %d samples, under the floor of %d (%f)", smp[0], AD_START_MIN_RTT_SAMPLES,
                     o.start_ms);
            smp[0] = 30;
            o      = lookahead_start(rtt, smp, 1, false, FL, CE, FB);
            us_check(o.reason == LS_START_NO_RTT && o.start_ms == FB,
                     "...and so does a transport that cannot measure (TCP, lat_supported 0) (%f)", o.start_ms);
            o = lookahead_start(rtt, smp, 0, true, FL, CE, FB);
            us_check(o.reason == LS_START_NO_RTT && o.start_ms == FB, "...and so does an empty peer list");
            // The WORST link binds, not the first or the average.
            rtt[0] = 50.0;
            smp[0] = 10;
            rtt[1] = 405.6;
            smp[1] = 10;
            o      = lookahead_start(rtt, smp, 2, true, FL, CE, FB);
            us_check(o.peer == 1 && o.start_ms >= 280.0, "mp:P14 -- the slowest peer sets the start (%d, %f)",
                     o.peer, o.start_ms);
            // A partly measured peer set can only RAISE the fallback.
            rtt[0] = 1.0;
            smp[0] = 10;
            smp[1] = 1;
            o      = lookahead_start(rtt, smp, 2, true, FL, CE, FB);
            us_check(o.reason == LS_START_PARTIAL && o.start_ms == FB,
                     "mp:P14 -- a LAN peer beside an unmeasured one keeps the 100 fallback (%f)", o.start_ms);
            rtt[0] = 405.6;
            o      = lookahead_start(rtt, smp, 2, true, FL, CE, FB);
            us_check(o.reason == LS_START_PARTIAL && o.start_ms >= 280.0,
                     "...while a slow measured peer still raises it (%f)", o.start_ms);
            // The ceiling bounds it (a 900 ms RTT seeds the ceiling, not 622).
            rtt[0] = 900.0;
            o      = lookahead_start(rtt, smp, 1, true, FL, CE, FB);
            us_check(o.start_ms == CE, "mp:P14 -- the ceiling bounds the seed (%f)", o.start_ms);

            // THE OPENING, two coupled peers at the rig's one-way 200 ms, P10 + P12 armed as shipped:
            // the seeded start against the 100 ms start. Window 1 is the one the row's rig clause reads
            // (<= 10% starved); in the model it is measured exactly as the `first-window` line reports it.
            //
            // TWO PAIRS, because the model's "starved" means two things. With EQUAL machine rates (`sym`,
            // both 1.00 -- the game clock paces both peers at realtime) every starved frame is the LINK's:
            // that is the pair the <= 10% clause is asserted on. P12's calibrated pair runs peer 0 at 1.25x,
            // and a faster machine bound by a realtime peer waits on it ~20% of frames at ANY lookahead
            // (measured while fitting this arm: settled windows 18-20% from starts 100 / 262 / 310 / 350 /
            // 400) -- that is waiting on the peer, not on the link, so on that pair window 1 is asserted
            // only against the peer's own settled windows.
            const double OWD = 200.0;
            rtt[0]           = 2.0 * OWD;
            smp[0]           = 4;
            const double S   = lookahead_start(rtt, smp, 1, true, FL, CE, FB).start_ms;
            TwoPeerSim   old = two_peer_run(100.0, 100.0, 1.25, 1.00, OWD, OWD, 120.0, 20.0, FL, CE, true);
            TwoPeerSim   sd  = two_peer_run(S, S, 1.25, 1.00, OWD, OWD, 120.0, 20.0, FL, CE, true);
            TwoPeerSim   so  = two_peer_run(100.0, 100.0, 1.00, 1.00, OWD, OWD, 120.0, 20.0, FL, CE, true);
            TwoPeerSim   sym = two_peer_run(S, S, 1.00, 1.00, OWD, OWD, 120.0, 20.0, FL, CE, true);
            for (int i = 0; i < 2; ++i)
                printf("  [P14] owd %.0f peer %d: start 100 -> %.0f; win1 %.0f%% -> %.0f%% (equal rates %.0f%% -> "
                       "%.0f%%; first verdict %d -> %d, uncapped %d -> %d), first 10 s %.1f%% -> %.1f%%, settled "
                       "%.0f -> %.0f, rate %.3f -> %.3f x\n",
                       OWD, i, S, 100 * old.starved_win[i][0], 100 * sd.starved_win[i][0], 100 * so.starved_win[i][0],
                       100 * sym.starved_win[i][0], old.first_verdict[i], sd.first_verdict[i], (int)old.first_full[i],
                       (int)sd.first_full[i], 100 * old.starved_10s[i], 100 * sd.starved_10s[i], old.cur_ms[i],
                       sd.cur_ms[i], old.rate_eff[i], sd.rate_eff[i]);
            us_check(sym.starved_win[0][0] <= 0.10 && sym.starved_win[1][0] <= 0.10 && so.starved_win[0][0] > 0.10 &&
                         so.starved_win[1][0] > 0.10,
                     "mp:P14 -- seeded, the FIRST decided window starves <= 10%% on both peers (%.3f/%.3f; from "
                     "the 100 start %.3f/%.3f)",
                     sym.starved_win[0][0], sym.starved_win[1][0], so.starved_win[0][0], so.starved_win[1][0]);
            us_check(sd.starved_win[0][0] <= sd.starved_win[0][3] + 0.01 &&
                         sd.starved_win[1][0] <= sd.starved_win[1][3] + 0.01,
                     "...and on the unequal-rate pair window 1 is no worse than the peer's own 4th window "
                     "(%.3f/%.3f vs %.3f/%.3f; from 100: %.3f/%.3f)",
                     sd.starved_win[0][0], sd.starved_win[1][0], sd.starved_win[0][3], sd.starved_win[1][3],
                     old.starved_win[0][0], old.starved_win[1][0]);
            us_check(sd.starved_10s[0] < old.starved_10s[0] && sd.starved_10s[1] < old.starved_10s[1],
                     "...and the first 10 s starve less on both (%.3f/%.3f vs %.3f/%.3f)", sd.starved_10s[0],
                     sd.starved_10s[1], old.starved_10s[0], old.starved_10s[1]);
            us_check(sd.rate_eff[0] >= old.rate_eff[0] - 0.005 && sd.rate_eff[1] >= old.rate_eff[1] - 0.005,
                     "...at no cost in sim rate (%.3f/%.3f vs %.3f/%.3f)", sd.rate_eff[0], sd.rate_eff[1],
                     old.rate_eff[0], old.rate_eff[1]);
            us_check(sd.cur_ms[0] + sd.cur_ms[1] <= 1.10 * (old.cur_ms[0] + old.cur_ms[1]),
                     "...and the pair settles on no more total lookahead (%.0f vs %.0f ms, +10%% allowed)",
                     sd.cur_ms[0] + sd.cur_ms[1], old.cur_ms[0] + old.cur_ms[1]);
            // P12 RE-CHECKED UNDER THE SEED: its uncapped first grow binds only when the first window's
            // deficit exceeds the value in force, and a seeded start removes that deficit -- so on the
            // link that motivated P12 the guard is now a no-op (it still fires from the 100 fallback,
            // which is (m) above and the `old` run here).
            us_check(old.first_full[0] || old.first_full[1],
                     "mp:P12 under P14 -- from the 100 fallback the uncapped first grow still fires");
            us_check(!sd.first_full[0] && !sd.first_full[1],
                     "mp:P12 under P14 -- from the seeded start it does not (the cap cannot bind)");

            // THE LAN ARM: the seed is the floor band, so the pair starts where P11's 100-start run
            // took 7-9 s to arrive, and never grows.
            rtt[0]                = 1.0;
            smp[0]                = 5;
            const double     SL   = lookahead_start(rtt, smp, 1, true, FL, CE, FB).start_ms;
            const TwoPeerSim lan0 = two_peer_run(100.0, 100.0, 1.25, 1.00, 0.5, 0.5, 120.0, 20.0, FL, CE, true);
            const TwoPeerSim lanS = two_peer_run(SL, SL, 1.25, 1.00, 0.5, 0.5, 120.0, 20.0, FL, CE, true);
            for (int i = 0; i < 2; ++i)
                printf("  [P14] LAN peer %d: start 100 -> %.1f; first verdict %d -> %d, settled %.0f -> %.0f, "
                       "grows %d -> %d, rate %.3f -> %.3f x\n",
                       i, SL, lan0.first_verdict[i], lanS.first_verdict[i], lan0.cur_ms[i], lanS.cur_ms[i],
                       lan0.n_grow[i], lanS.n_grow[i], lan0.rate_eff[i], lanS.rate_eff[i]);
            us_check(lanS.cur_ms[0] <= lan0.cur_ms[0] && lanS.cur_ms[1] <= lan0.cur_ms[1] && lanS.n_grow[0] == 0 &&
                         lanS.n_grow[1] == 0,
                     "mp:P14 LAN -- seeded at the floor band, the pair settles no higher (%.0f/%.0f vs %.0f/%.0f) "
                     "and never grows",
                     lanS.cur_ms[0], lanS.cur_ms[1], lan0.cur_ms[0], lan0.cur_ms[1]);
            us_check(lanS.rate_eff[0] >= lan0.rate_eff[0] - 0.005 && lanS.rate_eff[1] >= lan0.rate_eff[1] - 0.005,
                     "...at no cost in sim rate (%.3f/%.3f vs %.3f/%.3f)", lanS.rate_eff[0], lanS.rate_eff[1],
                     lan0.rate_eff[0], lan0.rate_eff[1]);
        }
    }

    // ---- mp:T4 -- the eager advert's send rule (advert_should_send) -----------------------------
    // A model of the frame loop the rule lives in, with the two properties it must keep at once:
    //   RATE       one send per frame at an uncapped frame rate filled the UDP transport's 1024-segment
    //              window at a 360 ms round trip (dead-ends G294). Measured as the busiest 400 ms
    //              (the round trip + the 40 ms ack cadence) against that window.
    //   INVARIANT  after the eager block, the last horizon on the wire equals HORIZON. The first
    //              fix broke it: the order scheduler pulls HORIZON forward (the order itself
    //              carries it), the eager write lowers it back, and a rule keyed on "the last value
    //              this path sent" skipped the lowered value.
    // Three policies run through the same loop: the REAL rule, the pre-T4 SEND-EVERY-FRAME code, and
    // the first fix, LAST-SENT-BY-THIS-PATH. The two mutants must each red one property.
    {
        enum Policy { REAL,
                      EVERY_FRAME,
                      LAST_SENT };
        struct Sim {
            long sends;
            long worst_window; // most eager sends inside any 400 ms span
            long broken;       // frames that ended with wire != HORIZON
        };
        static long sent_at[20000]; // prefix count of eager sends, per frame
        auto        run = [](Policy pol, double fps, double secs) {
            Sim        r{0, 0, 0};
            const long frames  = (long)(fps * secs);
            const long win     = (long)(fps * 0.400);
            double     horizon = -1.0, wire = -1.0, last_sent = -1.0;
            bool       have_last = false;
            long       n         = 0;
            for (long f = 0; f < frames && f < 20000; ++f) {
                const double t     = (double)f / fps;
                const double clock = 0.020 * (double)(long)(t / 0.020); // whole 20 ms sim steps
                // Every 97th frame an order is scheduled with an exec_time past the horizon: the
                // scheduler PULLS HORIZON forward and sends the order, not an EXTEND -- and the
                // receiver takes an order's exec_time as the sender's horizon (rx_dispatch.cpp
                // MSG_ORDER), so the wire now says E.
                if (f % 97 == 50 && clock + 0.300 > horizon) {
                    horizon = clock + 0.300;
                    wire    = horizon;
                }
                // Every 150th frame the heartbeat writes and SENDS clock + lookahead.
                if (f % 150 == 7) {
                    horizon = clock + 0.100;
                    wire    = horizon;
                }
                // The eager block.
                const double before = horizon;
                horizon             = clock + 0.100;
                bool send           = false;
                if (pol == REAL) send = advert_should_send(before, horizon);
                else if (pol == EVERY_FRAME) send = true;
                else send = !(have_last && horizon == last_sent);
                if (send) {
                    wire      = horizon;
                    last_sent = horizon;
                    have_last = true;
                    ++n;
                }
                if (!(wire == horizon)) ++r.broken;
                sent_at[f] = n;
                if (f >= win && n - sent_at[f - win] > r.worst_window) r.worst_window = n - sent_at[f - win];
            }
            r.sends = n;
            return r;
        };
        const long SEG_WINDOW_T4 = 1024; // mh_net_udp/udp_endpoint.h SEG_WINDOW

        // (1) ~3000 calls at an UNCHANGED horizon -> at most one send.
        double h = -1.0;
        long   n = 0;
        for (int i = 0; i < 3000; ++i) {
            const double before = h;
            h                   = 0.1;
            if (advert_should_send(before, h)) ++n;
        }
        us_check(n == 1, "mp:T4 -- 3000 frames at an unchanged horizon send it once (%ld)", n);
        // (2) a LOWER horizon is a change (the adaptive shrink, and the eager write after an order
        //     pulled HORIZON forward).
        us_check(advert_should_send(0.3, 0.1), "mp:T4 -- a lowered horizon goes out");
        us_check(advert_should_send(0.1, 0.12), "mp:T4 -- ...and so does a raised one");

        // (3) the rig shape: 3000 fps for 2 s, orders and heartbeat interleaved.
        const Sim real  = run(REAL, 3000.0, 2.0);
        const Sim every = run(EVERY_FRAME, 3000.0, 2.0);
        const Sim last  = run(LAST_SENT, 3000.0, 2.0);
        printf("  mp:T4 3000 fps x 2 s: real %ld sends (worst 400 ms %ld, broken %ld) | every-frame %ld "
               "(%ld, %ld) | last-sent %ld (%ld, %ld)\n",
               real.sends, real.worst_window, real.broken, every.sends, every.worst_window, every.broken,
               last.sends, last.worst_window, last.broken);
        us_check(real.worst_window < SEG_WINDOW_T4 / 4,
                 "mp:T4 -- RATE: the busiest 400 ms stays far under the 1024-segment window (%ld)",
                 real.worst_window);
        us_check(real.broken == 0, "mp:T4 -- INVARIANT: after every eager block the wire holds HORIZON "
                                   "(%ld frames broke it)",
                 real.broken);
        // (4) THE MUTANTS MUST RED, each on the property it breaks, or the checks prove nothing.
        us_check(every.worst_window >= SEG_WINDOW_T4,
                 "mp:T4 mutant EVERY-FRAME (pre-T4) fills the window: %ld in 400 ms", every.worst_window);
        us_check(last.broken > 0,
                 "mp:T4 mutant LAST-SENT (the first fix) breaks the invariant: %ld frames", last.broken);
    }

    // ---- mp:D30 -- the advertised horizon never goes DOWN (monotone_step / monotone_horizon) ------
    // (1) the helper's contract, over a sweep; (2) the TWO-PEER MODEL of the rig shape that desynced
    // (tmp/o5_rig/D30/r1_DESYNC): A shrinks its lookahead 315 -> 253 ms at clock 10.439 s, B runs
    // 100 ms ahead of A, 180 ms one-way delivery, A stamps an order every step. RED before (the
    // target pinned as-is: B releases A's orders a step late), GREEN after; the helper mutants red;
    // (3) PACING: with the fix the shrink still lands, within the size of the cut.
    {
        // (1) the contract
        long bad_floor = 0, bad_target = 0, bad_idle = 0;
        for (int i = 0; i < 20000; ++i) {
            const double clk    = 0.02 * (double)(i % 5000) + 0.001 * (double)(i % 7);
            const double target = 0.060 + 0.001 * (double)(i % 341);
            const double sent   = clk + 0.001 * (double)((i * 37) % 700); // 0..0.7 s ahead
            const double st     = monotone_step(target, clk, sent);
            if (sent > 0.0 && clk + st < sent) ++bad_floor;
            if (st < target) ++bad_target;
            if (sent <= clk + target - 1e-9 && st != target) ++bad_idle;
        }
        us_check(bad_floor == 0, "mp:D30 -- clock + step never lands under the sent max (%ld of 20000)", bad_floor);
        us_check(bad_target == 0, "mp:D30 -- the step never goes under the target (%ld)", bad_target);
        us_check(bad_idle == 0, "mp:D30 -- with nothing sent above clock + target the target passes through (%ld)",
                 bad_idle);
        us_check(monotone_step(0.25, 10.0, 12.5) == 0.25, "mp:D30 -- a max > 2 s ahead is a restarted clock, ignored");
        us_check(monotone_step(0.25, 10.0, 0.0) == 0.25, "mp:D30 -- no max yet -> the target");
        us_check(monotone_horizon(10.0, 0.25, 10.4) == 10.4 && monotone_horizon(10.0, 0.25, 10.1) == 10.25,
                 "mp:D30 -- monotone_horizon floors at the held horizon and otherwise adds the step");
        us_check(order_is_late(10.713, 10.72) && order_is_late(10.72, 10.72) && !order_is_late(10.73, 10.72),
                 "mp:D30 -- order_is_late is release_due's !(exec > clock)");
        volatile double zero = 0.0;
        const double    nan  = zero / zero;
        us_check(order_is_late(nan, 10.0), "mp:D30 -- a NaN exec_time counts as late, as release_due releases it");

        // (2) the two-peer model
        enum Pol { FIXED,
                   PREFIX,       // the target pinned as-is -- the shipped bug
                   MUT_STEP,     // mutant: holds one sub-step short (max(target, need - SUB))
                   MUT_NEEDONLY, // mutant: ignores the target (never adds new lookahead)
                   MUT_FRAMEONLY // mutant: the time_tick pin without the per-step re-pin
        };
        struct Msg {
            double arrive; // real seconds
            int    kind;   // 1 = order, 2 = horizon
            double v;      // exec_time or horizon
            int    id;     // order id
        };
        struct R {
            long   orders, late, mismatched, horizon_drops;
            double a_clock_end, eff_min_after, released_at;
        };
        auto model = [](Pol pol) {
            const double  SUB = 0.020, D = 0.180, T0 = 10.0, SHRINK_AT = 10.439, OLD = 0.315, NEW = 0.253;
            static Msg    q_ab[40000], q_ba[40000]; // A->B, B->A (FIFO: one delay, send order)
            static double rel_a[4000], rel_b[4000], pend_exec[4000], own_exec[4000];
            static int    pend_id[4000], own_id[4000];
            long          n_ab = 0, h_ab = 0, n_ba = 0, h_ba = 0, n_pend = 0, n_own = 0;
            R             r{0, 0, 0, 0, 0.0, 1e9, -1.0};
            // Both clocks start at T0 (one sim grid, as in a real match); B runs AHEAD in wall time,
            // which is how the rig's host sat 100 ms ahead of the client that shrank.
            double clk[2] = {T0, T0}, hz[2], heard[2], maxs[2], step[2] = {OLD, OLD};
            hz[0]             = clk[0] + OLD;
            hz[1]             = clk[1] + OLD;
            heard[0]          = hz[1];
            heard[1]          = hz[0];
            maxs[0]           = hz[0];
            maxs[1]           = hz[1];
            int    next_id    = 0;
            double last_adv_a = 0.0;
            for (int i = 0; i < 4000; ++i) rel_a[i] = rel_b[i] = -1.0;
            for (int ms = 0; ms < 3000; ++ms) {
                const double t = 0.001 * (double)ms;
                for (int p = 0; p < 2; ++p) {
                    // deliveries due by now, processed before the sim (the pump's RX drain)
                    if (p == 1) {
                        while (h_ab < n_ab && q_ab[h_ab].arrive <= t) {
                            const Msg &m = q_ab[h_ab++];
                            heard[1]     = m.v; // MSG_HORIZON and MSG_ORDER's exec: a straight overwrite
                            if (m.kind == 1) {
                                if (order_is_late(m.v, clk[1])) ++r.late;
                                pend_exec[n_pend] = m.v;
                                pend_id[n_pend++] = m.id;
                            }
                        }
                    } else {
                        while (h_ba < n_ba && q_ba[h_ba].arrive <= t) heard[0] = q_ba[h_ba++].v;
                    }
                    // time_tick: the pin, then the advertise (clock + STEP_SIZE)
                    const double target = (p == 0 && clk[0] >= SHRINK_AT) ? NEW : OLD;
                    const double need   = maxs[p] - clk[p];
                    double       st;
                    if (pol == FIXED || pol == MUT_FRAMEONLY) st = monotone_step(target, clk[p], maxs[p]);
                    else if (pol == PREFIX) st = target;
                    else if (pol == MUT_STEP) st = need - SUB > target ? need - SUB : target;
                    else st = need;
                    step[p]         = st;
                    const double hv = clk[p] + st;
                    if (p == 0) {
                        if (hv < last_adv_a - 1e-12) ++r.horizon_drops;
                        last_adv_a = hv;
                        if (clk[0] >= SHRINK_AT) {
                            if (st < r.eff_min_after) r.eff_min_after = st;
                            if (r.released_at < 0.0 && st <= NEW + 1e-6) r.released_at = clk[0];
                        }
                    }
                    hz[p] = hv;
                    if (hv > maxs[p]) maxs[p] = hv;
                    if (p == 0) q_ab[n_ab++] = Msg{t + D, 2, hv, -1};
                    else q_ba[n_ba++] = Msg{t + D, 2, hv, -1};
                    // sim_tick: total = min(wall, committed)
                    const double wall      = T0 + t + (p == 1 ? 0.100 : 0.0);
                    const double committed = hz[p] < heard[p] ? hz[p] : heard[p];
                    const double total     = wall < committed ? wall : committed;
                    while (clk[p] + SUB <= total) {
                        clk[p] += SUB;
                        long o = 0;
                        if (p == 0) {
                            for (long k = 0; k < n_own; ++k) {
                                if (!(own_exec[k] > clk[0])) {
                                    rel_a[own_id[k]] = clk[0];
                                } else {
                                    own_exec[o] = own_exec[k];
                                    own_id[o++] = own_id[k];
                                }
                            }
                            n_own = o;
                            // A stamps one order per step: max(clock + STEP_SIZE, HORIZON); schedule
                            // raises HORIZON to it if it is past it.
                            if (next_id < 4000 && clk[0] >= T0 + 0.2) {
                                // the per-step re-pin (MH_Lockstep_StepPin) at this step's clock
                                const double tg = clk[0] >= SHRINK_AT ? NEW : OLD;
                                const double nd = maxs[0] - clk[0];
                                if (pol == FIXED) step[0] = monotone_step(tg, clk[0], maxs[0]);
                                else if (pol == PREFIX) step[0] = tg;
                                else if (pol == MUT_STEP) step[0] = nd - SUB > tg ? nd - SUB : tg;
                                else if (pol == MUT_NEEDONLY) step[0] = nd;
                                // MUT_FRAMEONLY: keeps the frame-start pin
                                double e = clk[0] + step[0];
                                if (hz[0] > e) e = hz[0];
                                if (!(hz[0] >= e)) hz[0] = e;
                                if (e > maxs[0]) maxs[0] = e;
                                own_exec[n_own] = e;
                                own_id[n_own++] = next_id;
                                q_ab[n_ab++]    = Msg{t + D, 1, e, next_id};
                                ++next_id;
                            }
                        } else {
                            for (long k = 0; k < n_pend; ++k) {
                                if (!(pend_exec[k] > clk[1])) {
                                    rel_b[pend_id[k]] = clk[1];
                                } else {
                                    pend_exec[o] = pend_exec[k];
                                    pend_id[o++] = pend_id[k];
                                }
                            }
                            n_pend = o;
                        }
                    }
                }
                if (n_ab > 39000 || n_ba > 39000) break;
            }
            r.orders = next_id;
            for (int i = 0; i < next_id; ++i)
                if (rel_a[i] >= 0.0 && rel_b[i] >= 0.0 && (long)((rel_a[i] - T0) / SUB + 0.5) != (long)((rel_b[i] - T0) / SUB + 0.5))
                    ++r.mismatched;
            r.a_clock_end = clk[0];
            return r;
        };
        const R fixed = model(FIXED), prefix = model(PREFIX), mstep = model(MUT_STEP), mneed = model(MUT_NEEDONLY),
                frameonly = model(MUT_FRAMEONLY);
        printf("  mp:D30 model: fixed %ld orders, late %ld, mismatched %ld, drops %ld, end %.3f | pre-fix late %ld "
               "mismatched %ld drops %ld | step-short late %ld | need-only end %.3f | frame-only landed %.3f | shrink "
               "landed at clock %.3f (eff min %.3f)\n",
               fixed.orders, fixed.late, fixed.mismatched, fixed.horizon_drops, fixed.a_clock_end, prefix.late,
               prefix.mismatched, prefix.horizon_drops, mstep.late, mneed.a_clock_end, frameonly.released_at, fixed.released_at,
               fixed.eff_min_after);
        us_check(prefix.late > 0 && prefix.mismatched > 0 && prefix.horizon_drops > 0,
                 "mp:D30 RED before: the pre-fix pin lowers A's horizon (%ld drops) and B releases %ld of A's "
                 "orders a step late (%ld late at receipt)",
                 prefix.horizon_drops, prefix.mismatched, prefix.late);
        us_check(fixed.orders > 100 && fixed.late == 0 && fixed.mismatched == 0 && fixed.horizon_drops == 0,
                 "mp:D30 GREEN after: %ld orders, 0 late (%ld), 0 released at different steps (%ld), horizon "
                 "never lowered (%ld)",
                 fixed.orders, fixed.late, fixed.mismatched, fixed.horizon_drops);
        us_check(mstep.late > 0, "mp:D30 mutant step-short reds: %ld late", mstep.late);
        // (3) pacing: the shrink lands, bounded by the size of the cut, and the sim keeps real time
        us_check(fixed.released_at > 0.0 && fixed.released_at <= 10.439 + (0.315 - 0.253) + 3 * 0.020,
                 "mp:D30 PACING: the shrink takes effect by clock %.3f (a 62 ms cut at 10.439)", fixed.released_at);
        us_check(fixed.eff_min_after >= 0.253 - 1e-9, "mp:D30 PACING: never under the target (%.4f)",
                 fixed.eff_min_after);
        us_check(fixed.a_clock_end > 12.5, "mp:D30 PACING: A's sim keeps real time (clock %.3f after 3 s)",
                 fixed.a_clock_end);
        us_check(mneed.a_clock_end < 11.0, "mp:D30 mutant need-only reds the pacing arm: A stalls at %.3f",
                 mneed.a_clock_end);
        us_check(frameonly.late == 0 && (frameonly.released_at < 0.0 || frameonly.released_at > 10.7),
                 "mp:D30 mutant frame-only (no per-step re-pin) is SAFE but reds PACING: the shrink lands at "
                 "%.3f (-1 = never) against an order every step",
                 frameonly.released_at);
    }

    // ---- 10. (o) mp:P15: THE WARM-UP PING CADENCE'S OWN PRECONDITION --------------------------------
    // P14's seed (section n above) needs AD_START_MIN_RTT_SAMPLES (3) pings before it can fire at all.
    // This drives the SAME pure function timer_loop() calls (udp_ping_cadence.h's `ping_due`) through
    // a synthetic tick loop at TICK_MS (udp_endpoint.h, the real timer thread's period) from admission,
    // and counts how many ticks it says to ping by the 1 s mark -- proving the WIRING (the real
    // constants, the real per-conn clocks), not a re-derivation of the formula.
    {
        using namespace mh::netudp;
        // (o1) the shipped cadence: >= 3 samples within 1 s of admission (mp:P15's done_when).
        {
            uint32_t admitted = 0, last_ping = 0;
            int      samples = 0;
            for (uint32_t now = TICK_MS; now <= 1000; now += TICK_MS) {
                if (ping_due(PING_MS_DEFAULT, now - admitted, now - last_ping, FAST_PING_MS,
                             FAST_PING_WINDOW_MS)) {
                    ++samples;
                    last_ping = now;
                }
            }
            us_check(samples >= mh::netstats::AD_START_MIN_RTT_SAMPLES,
                     "mp:P15 -- the warm-up cadence delivers >= %d ping(s) within 1 s of admission "
                     "(got %d)",
                     mh::netstats::AD_START_MIN_RTT_SAMPLES, samples);
            // MUTATION-CHECKED (2026-09-24): with FAST_PING_WINDOW_MS forced to 0 -- the warm-up
            // window collapsing to nothing, which is what a caller that forgot to pass it (or a
            // constant regressed to 0) would look like -- this ticks at the steady PING_MS_DEFAULT
            // (1000 ms) throughout and `samples` reads 1, redding this check; reverted after.
        }
        // (o2) NEVER FASTER than a configured `ping_ms` slower than FAST_PING_MS: the warm-up can
        // only ADD samples early, never override a deliberately slow steady rate.
        {
            const int SLOW = 500; // < FAST_PING_MS's implicit 250, > the fast rate would be if unclamped
            uint32_t  now = 100, admitted = 0, last_ping = 0;
            us_check(!ping_due(SLOW, now - admitted, now - last_ping, FAST_PING_MS, FAST_PING_WINDOW_MS),
                     "mp:P15 -- a configured ping_ms slower than FAST_PING_MS is not sped up at t=100 "
                     "(cadence must stay >= %d)",
                     SLOW);
        }
        // (o3) pings OFF (`ping_ms <= 0`) stays off during the warm-up window too -- the R-live
        // three-state convention is not bypassed by admission.
        us_check(!ping_due(0, 0, 1000000u, FAST_PING_MS, FAST_PING_WINDOW_MS),
                 "mp:P15 -- ping_ms <= 0 never pings, warm-up window or not");
        // (o4) past the warm-up window, the cadence is exactly the steady rate -- not stuck fast and
        // not reset by every call (the SAME conn's last_ping_ms must carry across ticks).
        {
            const uint32_t past = FAST_PING_WINDOW_MS + 10;
            us_check(!ping_due(PING_MS_DEFAULT, past, 1u, FAST_PING_MS, FAST_PING_WINDOW_MS),
                     "mp:P15 -- 1 ms after the last ping, past the warm-up window, is not due yet "
                     "(steady cadence is %d ms)",
                     PING_MS_DEFAULT);
            us_check(ping_due(PING_MS_DEFAULT, past, (uint32_t)PING_MS_DEFAULT, FAST_PING_MS,
                              FAST_PING_WINDOW_MS),
                     "mp:P15 -- a full steady interval past the warm-up window IS due");
        }
    }

    // ---- 11. (p) mp:P15 wave 7: WHERE THE FIRST WINDOW STARTS AND WHAT ITS STARVED FIGURE COUNTS ------
    // mh/seams/adaptive_window.h -- the same pure functions net_lockstep.cpp's lateness_tick /
    // adaptive_tick / adaptive_post_spin_tick call. (p1) replays wave 6's P15 run-3 host shape frame by
    // frame: every frame ENDS 16 ms of COMMITTED ahead of the clock (< one 20 ms sub-step, so the next
    // frame OPENS blocked) and rx_spin lands the peer's EXTEND inside the frame (36 ms ahead after the
    // spin). The pre-spin diag read that as 1015/1610 ms starved; the sim never missed a step.
    {
        using namespace mh::adwin;
        // (p1) the phase-locked host: pre-spin blocked on every frame, post-spin never.
        {
            PostSpinWindow w;
            uint32_t       pre_ms = 0;
            double         clk    = 0.760;
            for (int f = 0; f < 80; ++f) { // 80 x 20 ms = 1.6 s, the run-3 window
                const double com_pre  = clk + 0.016;
                const double com_post = clk + 0.036;
                if (blocked(clk, com_pre, 0.020)) pre_ms += 20;
                w.add(20, blocked(clk, com_post, 0.020));
                clk += 0.020; // the sim funds exactly one sub-step per frame
            }
            us_check(pre_ms == 1600, "mp:P15 (p1) -- the pre-spin sample reads the phase-locked host as "
                                     "starved every frame (got %u/1600 ms)",
                     pre_ms);
            us_check(w.time_ms == 1600 && w.starved_ms == 0,
                     "mp:P15 (p1) -- post-spin, the phase-locked host is starved 0 ms (got %u/%u ms)",
                     w.starved_ms, w.time_ms);
            // MUTATION-CHECKED (2026-09-24, one build, reverted after): in adaptive_window.h, `blocked`
            // with `<=` for `<` reds (p2b); all_live_latch latching on ANY live peer (the pre-wave-7
            // start) reds (p3c) and (p3f); warm_anchor_ready ignoring all_live reds (p4). 4 FAIL.
        }
        // (p2) a genuinely silent peer: the spin times out and COMMITTED is still short -> counted.
        {
            PostSpinWindow w;
            for (int f = 0; f < 10; ++f) w.add(40, blocked(5.000, 5.004, 0.020));
            us_check(w.starved_ms == 400 && w.time_ms == 400,
                     "mp:P15 (p2) -- a peer that never advertises is starved 100%% post-spin (got %u/%u)",
                     w.starved_ms, w.time_ms);
            // (p2b) exactly funded is NOT blocked (the sim can take the step).
            // (binary-exact values: 4.5 + 0.25 == 4.75 with no rounding, so this pins `<`, not an ulp)
            us_check(!blocked(4.5, 4.75, 0.25), "mp:P15 (p2b) -- COMMITTED == clock + sub-step is funded");
            w.reset();
            us_check(w.starved_ms == 0 && w.time_ms == 0, "mp:P15 (p2c) -- reset empties the window");
        }
        // (p3) the all-peers-live latch.
        us_check(live_peers_expected(2) == 1 && live_peers_expected(3) == 2,
                 "mp:P15 (p3a) -- expected peers = active humans minus ourselves");
        us_check(live_peers_expected(0) == 1 && live_peers_expected(1) == 1,
                 "mp:P15 (p3b) -- a roster that counts <= 1 still waits for one live peer");
        us_check(all_live_latch(1, 2, true, 100, 5000) == AL_NOT_YET,
                 "mp:P15 (p3c) -- 3-peer, one live: the first window does not start yet");
        us_check(all_live_latch(2, 2, true, 100, 5000) == AL_ALL_LIVE,
                 "mp:P15 (p3d) -- 3-peer, both live: latched");
        us_check(all_live_latch(0, 1, false, 0, 5000) == AL_NOT_YET,
                 "mp:P15 (p3e) -- nobody live yet: not latched, and no timeout without a first live peer");
        us_check(all_live_latch(1, 2, true, 5000, 5000) == AL_TIMEOUT,
                 "mp:P15 (p3f) -- a roster miscount cannot hold the controller off past the timeout");
        // (p4) the warm-up anchor needs BOTH a tail and every peer live.
        us_check(!warm_anchor_ready(true, false) && !warm_anchor_ready(false, true) && warm_anchor_ready(true, true),
                 "mp:P15 (p4) -- T3c's warm-up is anchored only once a tail exists AND all peers are live");
        // (p5) wave 7 rig run 1's bug: an 8-slot PEER_HORIZON table, slot 0 ours, slots 1-7 holding
        // retail's 10 s sentinel from the first tick; slot 1 (the real peer) starts advertising at
        // tick 50. Driven through the same horizon_observe / slot_live / all_live_latch lateness_tick
        // calls. The latch must stay shut until slot 1 moves, then report 1/1.
        {
            bool     seen[8]    = {false};
            double   last[8]    = {0.0};
            uint32_t moved[8]   = {0};
            int      fired_tick = -1, fired_live = -1;
            for (int tick = 0; tick < 100 && fired_tick < 0; ++tick) {
                const uint32_t now = 1000u + (uint32_t)tick * 20u;
                int            n   = 0;
                for (int i = 0; i < 8; ++i) {
                    double h = 10.0;                                        // the sentinel
                    if (i == 0) h = 0.1 + tick * 0.02;                      // our own slot moves
                    if (i == 1 && tick >= 50) h = 0.3 + (tick - 50) * 0.02; // the peer starts advertising
                    if (horizon_observe(seen[i], last[i], h)) moved[i] = now;
                    if (slot_live(i == 0, h, moved[i], now, 5000)) ++n;
                }
                if (all_live_latch(n, live_peers_expected(2), false, 0, 5000) != AL_NOT_YET) {
                    fired_tick = tick;
                    fired_live = n;
                }
            }
            us_check(fired_tick == 50 && fired_live == 1,
                     "mp:P15 (p5) -- sentinel slots are not live: all-peers-live fires when the real peer "
                     "first moves (tick 50) and reports 1/1 (got tick %d, %d live)",
                     fired_tick, fired_live);
            // (p6) the blocked EPISODE (adaptive_window.h (3)), through the same two functions
            // lateness_tick and lateness_post_spin_tick call. (p6a) wave 6 run 3's host: 4 s of 20 ms
            // frames, each OPENS blocked and rx_spin funds it 16 ms later -> 0 synthetic splits, one
            // -16 per frame. (p6b) a genuinely unfunded block: the spin times out (40 ms) unfunded,
            // frame after frame, for 2.5 s -> the split still fires, and nothing else is charged.
            {
                uint32_t since  = 0;
                int      splits = 0, ends = 0, worst = 0, s = 0;
                for (uint32_t t = 1000; t < 5000; t += 20) {
                    if (episode_blocked_top(since, t, 2000, &s)) ++splits;
                    if (episode_end_post_spin(since, true, t + 16, &s)) {
                        ++ends;
                        if (s < worst) worst = s;
                    }
                }
                us_check(splits == 0 && ends == 200 && worst == -16,
                         "mp:P15 (p6a) -- a phase-locked peer whose spin funds every frame yields 0 synthetic "
                         "-2000 samples over 4 s and 200 real -16 ms ones (got %d splits, %d ends, worst %d)",
                         splits, ends, worst);
                since  = 0;
                splits = ends = 0;
                int split_s   = 0;
                for (uint32_t t = 1000; t < 3500; t += 60) {
                    if (episode_blocked_top(since, t, 2000, &s)) {
                        ++splits;
                        split_s = s;
                    }
                    if (episode_end_post_spin(since, false, t + 40, &s)) ++ends;
                }
                us_check(splits == 1 && split_s <= -2000 && ends == 0,
                         "mp:P15 (p6b) -- a block the spin cannot fund still splits at 2 s and ends nowhere "
                         "else (got %d splits, sample %d, %d ends)",
                         splits, split_s, ends);
                // MUTATION-CHECKED (2026-09-25, reverted after): episode_end_post_spin always false
                // (the pre-fix behaviour) reds (p6a) with 1 split; ignoring `funded` reds (p6b) with 0.
            }
            // MUTATION-CHECKED (2026-09-25): horizon_observe returning true on the first read (the
            // pre-fix "0.0 -> sentinel is a move") reds this with tick 0, 7 live. Reverted after.
        }
    }

    printf("=== udpstatstest: %d checks, %d failures ===\n", g_us_checks, g_us_fails);
    return g_us_fails ? 1 : 0;
}


// ---- authtest: the link-security gate ------------------------------------------------------------
// The property that matters for a host published on the internet is NEGATIVE -- a peer WITHOUT the
// key must not get in -- and a negative property is exactly the kind that rots silently (disable the
// handshake and every positive test still passes). So this drives three peers against one host:
// wrong key (must be refused), right key (must play), and a raw connect that speaks no handshake at
// all (must be refused). It uses MH_KEY_FILE to give each child its own key in one directory.
static void write_key_file(const char *path, const char *hex) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, hex, lstrlenA(hex), &w, nullptr);
    WriteFile(h, "\r\n; test key\r\n", 14, &w, nullptr); // trailing comment: the real file has one
    CloseHandle(h);
}

static int spawn_with_key(const char *exe, const char *mode, int port, const char *keyfile,
                          PROCESS_INFORMATION *pi) {
    SetEnvironmentVariableA("MH_KEY_FILE", keyfile); // children inherit the current environment
    char cmd[MAX_PATH + 64];
    wsprintfA(cmd, "\"%s\" %s %d", exe, mode, port);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    return CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, pi) ? 1 : 0;
}

// A peer that connects and says nothing: the pre-handshake denial-of-service shape (hold a slot,
// send garbage into the game's parser). It must be dropped without ever reaching a peer slot.
static int run_mute_probe(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 2;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(s);
        return 0; // refused outright is also a pass
    }
    send(s, "MH\x00\x00garbage-not-a-handshake", 28, 0); // junk where the HELLO should be
    char buf[64];
    int  n = recv(s, buf, sizeof(buf), 0); // expect the host to close on us
    closesocket(s);
    printf("[probe]  host %s\n", n <= 0 ? "closed the connection (good)" : "REPLIED (bad)");
    return n <= 0 ? 0 : 1;
}

static int run_authtest(int port) {
    char exe[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    lstrcpynA(dir, exe, MAX_PATH);
    char *slash = dir;
    for (char *p = dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    char keyA[MAX_PATH], keyB[MAX_PATH];
    wsprintfA(keyA, "%smh_key_test_a.txt", dir);
    wsprintfA(keyB, "%smh_key_test_b.txt", dir);
    write_key_file(keyA, "1111111111111111111111111111111111111111111111111111111111111111");
    write_key_file(keyB, "2222222222222222222222222222222222222222222222222222222222222222");

    printf("=== authtest (PSK gate) on port %d ===\n", port);
    PROCESS_INFORMATION ph, pbad, pgood, pprobe;
    if (!spawn_with_key(exe, "host", port, keyA, &ph)) {
        printf("failed to launch host\n");
        return 10;
    }
    Sleep(400);

    if (!spawn_with_key(exe, "client", port, keyB, &pbad)) { // WRONG key
        TerminateProcess(ph.hProcess, 99);
        return 11;
    }
    int bad = wait_exit(pbad.hProcess, 15000);

    if (!spawn_with_key(exe, "probe", port, keyA, &pprobe)) { // no handshake at all
        TerminateProcess(ph.hProcess, 99);
        return 12;
    }
    int probe = wait_exit(pprobe.hProcess, 15000);

    if (!spawn_with_key(exe, "client", port, keyA, &pgood)) { // RIGHT key
        TerminateProcess(ph.hProcess, 99);
        return 13;
    }
    int good = wait_exit(pgood.hProcess, 15000);
    int host = wait_exit(ph.hProcess, 15000);

    CloseHandle(ph.hProcess);
    CloseHandle(ph.hThread);
    CloseHandle(pbad.hProcess);
    CloseHandle(pbad.hThread);
    CloseHandle(pgood.hProcess);
    CloseHandle(pgood.hThread);
    CloseHandle(pprobe.hProcess);
    CloseHandle(pprobe.hThread);
    DeleteFileA(keyA);
    DeleteFileA(keyB);

    printf("=== wrong-key client=%d (want !=0)  mute probe=%d (want 0)  right-key client=%d (want 0)  host=%d ===\n",
           bad, probe, good, host);
    if (bad != 0 && probe == 0 && good == 0 && host == 0) {
        printf("=== PASS: only the key holder got in ===\n");
        return 0;
    }
    printf("=== FAIL ===\n");
    return 1;
}

// mh_calls_selftest.gen.cpp -- generated with the marshalling thunks it verifies (gen_dll_calls.py)
int run_callstest();
// mh_export_selftest.gen.cpp -- generated with the entry thunks it verifies (gen_dll_exports.py)
int run_exportstest();
int run_watchdogtest();
int run_desynctest();
// net_queue_selftest.cpp -- D24: which inbound frame a FULL transport queue may destroy.
int run_queuetest();
// session_id_selftest.cpp -- SES0: the UUIDv7 match_id, the v3 SESSION_INFO/JOIN records, and the
// host's admit decision. Carries the two acceptance clauses the rig cannot reach -- there is no
// pre-SES0 client to run, so both refusals only exist offline.
int run_sessionidtest();
// session_dir_selftest.cpp -- SES1: the per-SESSION run directory. The NAME, the ROLLOVER state
// machine and the session.json / SESSION_BEGIN / SESSION_END record. Three of SES1's five acceptance
// clauses are claims about a name and a transition table, and on the rig each of them costs a
// two-VM run per match; the other two (the directories really appear, every stream lands in them)
// stay rig work because they are claims about CreateDirectory and about a dozen writers.
int run_sessiondirtest();
// log_rotate_selftest.cpp -- SES2: the log-stream SIZE CAP and its one-generation rotation. Its
// centre is SES2's acceptance clause driven verbatim -- 70 MB emitted through the REAL seam_log()
// into the REAL run directory at the REAL default cap, leaving a capped file plus exactly one
// .prev -- which on the rig would be a two-VM session long enough to produce 70 MB of DIAG lines.
// The surrounding arms are the refusals (a non-.log path, a too-small buffer, "0 means uncapped")
// that a gameplay run reaches never.
int run_logrottest();
// ini_read_selftest.cpp -- TL-HARN4: the shared ini STRING-read helper (config/ini_read.h) that
// strips a trailing same-line `;comment` from a value -- GetPrivateProfileStringA's own return
// includes it verbatim, which used to TERMINATE the process on two strict-compare keys ([config]
// mode, [net] module) and silently corrupt two others ([net] relay, [fonts] probe_text) before each
// was hand-patched at its own call site. This suite is the durable fix's own proof, against a
// fixture ini shaped exactly like the trap (mh_net.example.ini's documented style).
int run_inireadtest();
int run_runctxtest(int argc, char **argv); // LA13: where the logs root is (spawns itself)
// udp_wire_selftest.cpp -- T0: the UDP packet format (plan D2). Its centre is a directory of
// FIXTURE FILES that this suite and `cargo test -p relay` both read, because an encoder agreeing
// with its own decoder is one implementation agreeing with itself; the claim worth making is that
// two independent ones accept and refuse the same committed bytes. It takes argv because of the two
// side modes -- `--emit <dir>` regenerates the fixtures from this encoder, and `--fuzz <seconds>`
// runs the seeded decoder fuzz that the ASan build turns into the out-of-bounds proof.
int run_udpwiretest(int argc, char **argv);
// udp_loopback_selftest.cpp -- mp:T1: the UDP TRANSPORT above that format. A host and two clients
// IN THIS PROCESS on 127.0.0.1 -- which the TCP suites cannot do, because their transport is a file
// of globals and one process can only be one peer. The object form is what buys the two things this
// item is judged on: a synthetic loss rate applied where the network would apply it, and counters
// that say WHY a lossy run completed (redundancy covered N, the retransmit covered M, the
// reassembler stalled K times) rather than only that it did.
int run_udploopbacktest(int argc, char **argv);
int run_uqmatchtest(); // mp:U41e, udp_loopback_selftest.cpp
// udp_bulk_selftest.cpp -- mp:T2: CHANNEL C, the bulk reliable chunk transfer above that transport.
// A mebibyte crossing hash-verified at 5% injected loss, a receiver KILLED mid-transfer resuming
// from its last acknowledged chunk (mp:T1b's restart), and the never-evictable chunk lane refusing
// rather than destroying when the application stops draining -- which is the D24 question asked of
// the one queue whose contents are by definition not supersedable.
int run_udpbulktest(int port);
// udp_snapshot_selftest.cpp -- mp:X1: the CHUNKED SNAPSHOT PIPELINE above channel C. A real
// mh::state::world::capture() blob -- 829 bound regions, the RNG channels among them -- manifested,
// hashed, moved across two real endpoints at 5% loss, verified against the hash vector its sender
// committed to BEFORE the transfer, and imported into a poisoned world that must then reproduce the
// capturing peer's content and lockstep hashes. It lives here rather than in libmh_selftest because
// it needs both halves in one process: the world spine AND a real socket with a loss dial.
int run_udpsnaptest(int port);
// udp_punch_selftest.cpp -- mp:R3: the HOLE-PUNCH promotion state machine, with no network at all.
// Candidates in, probes out, an echo back, the peer's own probe seen, PROMOTED -- then the path goes
// dark and the pair falls back to the relay. Every one of R3's acceptance clauses is a rig clause
// (two VMs, a firewall rule over ssh, minutes per answer), and a red rig run cannot say whether the
// codec, the cadence, the promotion rule or the demotion timer was wrong. So the decision half is a
// pure function of (state, event, time) and this drives it as a table of milliseconds.
int run_udppunchtest();
// udp_room_selftest.cpp -- mp:R6: the HOST'S RELAY ROOM MINTER, with no relay and no RNG. A host's
// room used to be its `[net] port`, so on a shared relay the second host at any moment was refused
// `room_busy`; now it is a random 30-bit code minted per tunnel start and re-minted, a bounded
// number of times, on `room_busy`. The rig cannot stage two hosts on one relay (one box, one
// exclusive UDP port per lane), so the two-hosts clause, the retry bound and the "a relaunch after a
// crash gets a fresh room" clause are all proved here, over an injected RNG that says exactly what a
// rig run could only hope to observe.
int run_udproomtest();
// udp_relay_selftest.cpp -- mp:R3e: the HOST TUNNEL'S PER-PEER SLOTS ARE RELEASED. udp_relay.cpp
// holds one loopback socket, one punch and one pair-key set per remote peer, eight of each, and
// until R3e nothing ever freed one: the ninth distinct joiner of a lobby's life got no socket and
// no punch. Nine sequential harness joins is nine lobby walks on a lane pool with no headroom, so
// the real tunnel is driven here against a STAND-IN relay and a stand-in endpoint: eight join, a
// ninth is refused (the cap), the eight leave, the ninth and tenth then get both, and a punch
// nobody is behind is released by the orphan rule.
int run_udprelaytest();
// map_transfer_selftest.cpp -- mp:X2: the MAP DOWNLOAD. The content hash that makes a map name an
// identity, the `<stem>.<hex>.<ext>` stored name, the resolver that answers by HASHING candidates
// rather than by trusting their names, the open-redirect that serves a downloaded file under the
// base name WITHOUT renaming the map in game memory, the host's Start gate, and one real map-sized
// file across two real endpoints through X1's pipeline. Three of X2's clauses are things a rig can
// only show by ABSENCE (nothing transferred, nothing overwritten, nothing accepted), and an absence
// on a rig is equally consistent with a mechanism that never ran.
int run_maptest(int port);
int run_interlocktest();
// inmem_patch_selftest.cpp -- F1E: the in-memory static-patch applier and, above all, its REFUSALS
// (a moved guarded byte, an already-patched image, a site inside a promoted body, an unavailable
// cave VA). Same argument as interlocktest: every one of them is about a write that must not happen.
int run_patchtest();
// gone_peer_guard_selftest.cpp -- mp:U19i: gone_peer_frame_guard's byte-patch carrier, the REAL naked
// thunk driven against a fake buffer/emitter/frame. Its rig half (a config-(1) clean quit) cannot
// show the damage the thunk prevents without a second, unguarded run; this arm shows both.
int run_gpfgtest();
// tombstone_selftest.cpp -- X-TOMB: the arming DECISION (which bodies, what extent, what skips).
// A zero-hit rig run cannot demonstrate the decision, only the absence of a hit, so it is here.
int run_tombstonetest();
// hostapi_selftest.cpp -- LIB-ABI: the host-callback table's BINDING CONTRACT (version handshake,
// unbound-walk names the gap, a required entry reached through the selftest host traps by name).
int run_hostapitest();
// hostin_selftest.cpp -- LIB-REF-IN: the INBOUND (host -> libmh) ABI's binding contract. The
// sibling of the line above, and the same three arms in the other direction: the version handshake
// (a stale host refused WITHOUT clobbering a working open), the unbound-walk (a holed entry AND a
// holed order id reported BY NAME), and the trap (calling one names the slot it refused rather
// than returning a quiet zero that reads like an entry which did nothing).
int run_hostintest();
// state_selftest.cpp -- ST2's synthetic rebase fixture: a region moves and the hash, the save
// resolver and the shadow region sets all follow, mutation-checked in BOTH directions.
int run_statetest();
// state_selftest.cpp (same TU) -- SB-BIND T1: the HOST answers where the state is, through the C
// ABI a standalone host will use. The stock answer is a no-op BY CONSTRUCTION; the arm with teeth
// relocates one region and shows the size-vs-reach choice decide whether an overrunning save block
// resolves or comes back null.
int run_bindtest();


// ---- THE SUITE TABLE ----------------------------------------------------------------------------
//
// One row per mode. It replaced a 43-arm `else if (strcmp(...))` chain at fork F5I, for one reason:
// the chain was the ONLY statement of what suites exist, and nothing outside this file could read
// it. `tools/run_selftests.py` carried a second, hand-maintained copy of the gate's 32 names, the
// unknown-mode error message carried a third, and the three drifted independently -- a suite could
// be added here and never run by the gate, or listed in the error text and not exist.
//
// Now there is one list. `--list-suites` prints the `gate` rows, the error message is rendered FROM
// the table, and tools/data/selftest_roster.json is checked against both (by
// tools/check_selftest_roster.py against this source, and by run_selftests.py against the LIVE
// `--list-suites` output of the exe it is about to run).
//
// `gate` marks the suites the offline gate runs. The rest are the TRANSPORT and rig modes: they
// take a port, expect a peer, spawn or are spawned, and several deliberately never return on their
// own -- run_selftests.py cannot run them, which is why the flag exists rather than the roster
// being "every row". `seamtest` is a third case: self-contained, a known failure until 2026-09-24
// (tooling:TL-TEST1), and not gate-flagged yet.

// `suite_args`, `suite_fn`, `suite_row` and the three adapt_<shape> templates live in
// selftest_dispatch.h, shared with libmh_test/libmh_selftest.cpp since F5I S2 -- two mains needing
// the same dispatch mechanism is exactly the second-uncompared-copy shape the table replaced.
// WHICH suites exist, and why, stays here: that is this exe's own business.

// The three rows that do not fit one of the shared shapes.
static int adapt_client(const suite_args &a) {
    return run_client(a.port, (a.argc > 3) ? a.argv[3] : NULL);
}
static int adapt_recv(const suite_args &a) {
    return run_recv(a.port, a.extra ? a.extra : 2);
}
static int adapt_bcast(const suite_args &a) {
    return run_bcast(a.port, a.extra ? a.extra : 1);
}
// clang-format off
static const suite_row SUITE_TABLE[] = {
    // ---- transport / rig modes: a peer, a port, and usually another process --------------------
    {"host",        false, adapt_port<run_host>},
    {"client",      false, adapt_client},
    {"recv",        false, adapt_recv},
    {"bcast",       false, adapt_bcast},
    {"seam_host",   false, adapt_port<run_seam_host>},
    {"seam_client", false, adapt_port<run_seam_client>},
    // NOT gate-flagged. It was a known failure from 2026-07-25 (children reading fixed mh.exe VAs;
    // fixed 2026-09-24, tooling:TL-TEST1 -- see seam_point_at_mock). Gating it is a roster edit
    // (tools/data/selftest_roster.json + `true` here) and costs ~1 s per mode; left to the conductor.
    {"seamtest",    false, adapt_port<run_seamtest>},
    {"watch_host",  false, adapt_port<run_watch_host>},
    {"mute_peer",   false, adapt_port<run_mute_peer>},
    // U40 relinktest's two children (spawned, never run by hand -- the client blocks on the host's
    // watchdog and the host counts the client's connections).
    {"relink_host",   false, adapt_port<run_relink_host>},
    {"relink_client", false, adapt_port<run_relink_client>},
    // U41b qmatchtest's two children (spawned, never run by hand).
    {"qmatch_host",   false, adapt_port<run_qmatch_host>},
    {"qmatch_client", false, adapt_port<run_qmatch_client>},
    {"probe",       false, adapt_port<run_mute_probe>},
    // RETIRED at tracker U18 (2026-07-24) -- the self-render splice it validated no longer exists,
    // and the replacement is a game-coupled restore verified live. The row stays so the name still
    // resolves to its own explanation instead of to "unknown mode".
    {"menutest",    false, adapt_void<run_menutest>},

    // ---- the gate's 17 -- the OTHER 15 now live in libmh_selftest.exe ----------------------------
    // F5I S3 CUT THEM. The 15 SPINE suites (aitest, simtest, tacttest, orderstest, issuetest,
    // lockstest, resynctest, netsessiontest, libtranstest, savetest, boottest, worldtest, navtest,
    // crttest, fptest) are gone from this table, their declarations are gone, and their 338 TUs are
    // gone from mh_nettest.vcxproj. They are gate-flagged in libmh_test/libmh_selftest.cpp instead,
    // which builds the same TUs in the STANDALONE arm (MH_LIBMH_BUILD);
    // tools/data/selftest_roster.json's `exe` column routes each name to one exe and
    // tools/check_selftest_roster.py asserts the partition.
    //
    // S2 kept those rows here as `gate: false` so that the arm was the ONLY variable in the
    // identity measurement (the same build still answered `net_selftest.exe simtest`, so the new
    // exe's transcript could be compared against both the recorded baseline and this exe running
    // the same suite). That measurement is done -- 32/32 -- and the rows are now DELETED rather
    // than left un-gated on purpose: a stale consumer that still says `net_selftest.exe aitest`
    // must exit 2 with the mode list, not run a second copy of the suite in the wrong arm and
    // print green. That is the same argument as the unknown-mode error below, one level up.
    //
    // THE SPLIT IS BY SUBJECT, NOT BY SUITE (rulings R6 and R8), which is why `statetest`,
    // `bindtest` and `hostintest` stayed here with their TUs while the rest of the spine's oracles
    // left. All three have the same precondition: the STOCK bind. statetest's subject is ST2 -- a
    // region moving off its STOCK VA; bindtest asserts the HOSTED answer to "where does each region
    // live"; hostintest's first arm is an inbound open being REFUSED over an unanswered registry.
    // Standalone, every stock base is 0 by design (mh_regions.gen.h's MH_STOCK_BASE, so that no
    // original VA reaches libmh.lib's data), so all three would be asserting over a table of zeros
    // and printing green. The standalone side of that same question is proven instead by run_gate's
    // libref unit, which binds a real arena, opens the inbound surface for real, and replays.
    //
    // Nothing gate-flagged below is reachable from libmh_selftest.exe and nothing gate-flagged
    // there is reachable here; tools/check_selftest_roster.py asserts both directions against the
    // two tables, so the pair cannot drift into overlapping or into a gap.
    // `selftest` MUST BE NAMED. It used to reach run_selftest only via the fall-through at the
    // bottom, and when that fall-through became an error (2026-08-01, so a mistyped mode stops
    // printing PASS) this branch was not added -- so the gate's own documented first step,
    // `net_selftest.exe selftest`, started exiting 2 with the mode list. Found 2026-08-01 by
    // running the gate; the hardening fix had broken the thing it was hardening. The BARE
    // invocation still defaults to this row, which is a documented convenience and cannot be a typo.
    {"selftest",       true, adapt_port<run_selftest>},
    {"selftest3",      true, adapt_port<run_selftest3>},
    {"authtest",       true, adapt_port<run_authtest>},
    {"linktest",       true, adapt_port<run_linktest>},
    // U40. Deliberately next to linktest: both stage a link DEATH with the same mute-peer trick,
    // and where linktest asks "is the corpse noticed", this one asks "can the survivor dial again".
    {"relinktest",     true, adapt_port<run_relinktest>},
    {"qmatchtest",     true, adapt_port<run_qmatchtest>},
    // mp:T1b -- the same question asked of the OTHER transport module. It cannot be an arm of the
    // row above: relinktest drives the MH_Net_* exports, which in this exe are the TCP module's, so
    // the UDP side is reached through mh::netudp::Endpoint the way udploopbacktest reaches it.
    {"udprelinktest",  true, adapt_port<run_udprelinktest>},
    // mp:R7a. The MH_NetConfig relay fields: default = no relay, appended, TCP-indifferent. Binds a
    // TCP host port for the indifference arm, so it takes the port argument.
    {"netcfgtest",     true, adapt_port<run_netcfgtest>},
    {"callstest",      true, adapt_void<run_callstest>},
    {"exportstest",    true, adapt_void<run_exportstest>},
    {"launchtest",     true, adapt_void<run_launchtest>},
    {"interlocktest",  true, adapt_void<run_interlocktest>},
    {"patchtest",      true, adapt_void<run_patchtest>},
    {"gpfgtest",       true, adapt_void<run_gpfgtest>},
    {"tombstonetest",  true, adapt_void<run_tombstonetest>},
    {"hostapitest",    true, adapt_void<run_hostapitest>},
    // It must run BEFORE anything binds the regions -- its first arm is the open being refused over
    // an unanswered registry -- which the gate ordering happens to give it; if that stops holding
    // the suite says so rather than silently skipping the arm.
    {"hostintest",     true, adapt_void<run_hostintest>},
    {"statetest",      true, adapt_void<run_statetest>},
    {"bindtest",       true, adapt_void<run_bindtest>},
    {"watchdogtest",   true, adapt_void<run_watchdogtest>},
    {"desynctest",     true, adapt_void<run_desynctest>},
    {"queuetest",      true, adapt_void<run_queuetest>},
    {"sessionidtest",  true, adapt_void<run_sessionidtest>},
    {"sessiondirtest", true, adapt_void<run_sessiondirtest>},
    {"logrottest",     true, adapt_void<run_logrottest>},
    {"inireadtest",    true, adapt_void<run_inireadtest>},
    // dist LA13. Beside logrottest, its neighbour on the same object: that suite writes THROUGH the
    // run directory, this one asks WHERE it is -- <exedir>\logs\ by default, MH_LOG_ROOT when the
    // launcher sets it (a Program Files game is UAC-virtualized beside its exe). The variable-set
    // arms run in a spawned copy of this exe, because run_context decides once per process.
    {"runctxtest",     true, adapt_argv<run_runctxtest>},
    {"udpwiretest",    true, adapt_argv<run_udpwiretest>},
    {"udploopbacktest",true, adapt_argv<run_udploopbacktest>},
    // mp:U41e. Beside udploopbacktest because it drives the same object the same way, one concern
    // over: T1 proves the byte stream, this proves the inbound-queue rollup + match-boundary epoch
    // mh_net.dll's TCP transport has carried since mp:U41/U41d. Fixed ports (39710..), clear of
    // udploopbacktest's own 39560..39697 range and udprelinktest/udpbulktest/udpsnaptest's.
    {"uqmatchtest",    true, adapt_void<run_uqmatchtest>},
    // mp:T2. Next to the loopback suite because it drives the same object on the same loopback with
    // the same loss dial, one channel over: T1 owns the byte stream on channel A, this owns the
    // chunk transfer on channel C. It takes the port argument plus 200, so the two never collide.
    {"udpbulktest",    true, adapt_port<run_udpbulktest>},
    // mp:X1. Directly after T2 because it is T2's first consumer: the transport proves bytes cross,
    // this proves a WORLD crosses. Port argument plus 300.
    {"udpsnaptest",    true, adapt_port<run_udpsnaptest>},
    // mp:T3. The link-measurement ARITHMETIC, with no link: RFC 6298's own worked recurrence,
    // a synthetic 80 +/- 20 stream standing in for the shim, the 256-packet loss window, the
    // lateness percentiles, and the adaptive controller's decision function -- which is where
    // this suite earns its place, because the three rig clauses in T3's done_when are each a
    // claim about that function and a red rig run could not say which layer was wrong.
    {"udpstatstest",   true, adapt_void<run_udpstatstest>},
    // mp:R3. Beside udpstatstest and for the SAME reason, one layer over: both are the offline half
    // of an item whose done_when is entirely rig clauses, and in both the thing under test is a
    // decision function that a red rig run could not have named.
    {"udppunchtest",   true, adapt_void<run_udppunchtest>},
    // mp:R6. Beside udppunchtest for the same reason it sits beside udpstatstest: the offline half of
    // a relay item whose done_when is entirely about a deployment (two hosts on the VPS relay at
    // once), reduced to the decision function a red live run could not have named.
    {"udproomtest",    true, adapt_void<run_udproomtest>},
    // mp:R3e. Beside the two above: the third relay item proved offline, and the first to link the
    // tunnel itself (udp_relay.cpp) -- the relay and the endpoint it talks to are both stand-ins
    // the suite owns on loopback. Ephemeral ports throughout; no port argument.
    {"udprelaytest",   true, adapt_void<run_udprelaytest>},
    // mp:X2. After the snapshot suite because its wire arm is that pipeline carrying a different
    // payload: X1 proves a WORLD crosses, this proves a MAP file does and that what lands is written
    // under a content-addressed name the loader then resolves. Port argument plus 400.
    {"maptest",        true, adapt_port<run_maptest>},
};
// clang-format on

static const size_t SUITE_COUNT = sizeof(SUITE_TABLE) / sizeof(SUITE_TABLE[0]);

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "selftest";

    // ANSWERED BEFORE ANYTHING IS BOUND. `--list-suites` is what tools/run_selftests.py reads to
    // assert the exe's roster equals the committed one, so it must not be able to fail for a reason
    // unrelated to the roster (a host-table version skew below would exit 3, and the assertion would
    // read that as "the exe lists nothing").
    if (strcmp(mode, "--list-suites") == 0) return selftest_list_suites(SUITE_TABLE, SUITE_COUNT);

    const suite_args a = selftest_args(argc, argv);

    // LIB-REBIND R11: net_selftest is a host too, and it arms NOTHING -- the offline oracle drives
    // module bodies through their recording stubs, not through the rebind. Arming an empty set is
    // still required rather than skippable: armed() distinguishes "armed for nothing" from "never
    // armed", and the second complains once per process by design (a run that believed it armed rows
    // and did not is the failure R11 exists to make visible). Without this call every suite would
    // carry that complaint in its output.
    //
    // arm_none(), NOT arm_from_config(). Since fork F2E the game's arming follows `[config] mode`,
    // whose default is brokered -- so a selector-driven call here would arm every row the moment
    // this exe ran beside an mh_net.ini, or indeed beside none, and rebind callees underneath the
    // recording stubs the suites measure. "This host arms nothing, by design" is a different
    // statement from "this run selected the original engine" and gets its own entry point.
    mh::rebind::arm_none();

    // LIB-ABI stage C/D: net_selftest IS a host of libmh. Bind the selftest host table
    // (notify entries -> no-ops, required entries -> named FATAL traps) before any suite
    // runs module code -- module code reaches host callbacks only through mh::host(), which
    // fail-fast aborts if nothing is bound. This binding is the no-op-host arm: every suite
    // below runs with all notify callbacks doing nothing.
    if (libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION) != 0) {
        printf("FATAL: selftest host table refused by libmh_set_host_api (version skew?)\n");
        return 3;
    }
    if (libmh_set_tact_host_api(&mh_hostapi_selftest_tact_table(), LIBMH_TACT_HOST_API_VERSION) !=
        0) {
        printf("FATAL: selftest tact host table refused by libmh_set_tact_host_api "
               "(version skew?)\n");
        return 3;
    }

    // F4D-PRE: net_selftest is a host of libmh's HOOK services too, and it binds mh.dll's REAL
    // table rather than a selftest one -- it compiles hook/detour.cpp, hook/export.cpp,
    // hook/promoted.cpp and seams/harness.cpp, so the five forwarders reach exactly the bodies
    // every suite reached before the table existed. That is the point: the indirection has to be
    // behaviour-neutral here, and binding a stub table would hide it if it were not. The UNBOUND
    // answers are asserted by `hostapitest`, on a table it saves and restores.
    if (MH_LibMH_BindHookApi() != 0) {
        printf("FATAL: mh.dll hook table refused by libmh_set_hook_api (version skew?)\n");
        return 3;
    }

    if (const suite_row *row = selftest_find(SUITE_TABLE, SUITE_COUNT, mode)) return row->run(a);

    // AN UNRECOGNISED MODE IS AN ERROR, NOT A DEFAULT. This used to fall through to run_selftest, so
    // a mistyped name printed "=== PASS: transport loopback round-trip succeeded ===" and exited 0
    // -- a green result for a test that does not exist. Measured 2026-08-01 while running the gate
    // by hand: `lockstepstest` (a typo of `lockstest`) reported PASS. Every check in this file
    // exists because a green result that could not go red is worth less than no check, and the
    // dispatcher was quietly manufacturing them. The BARE invocation still defaults to `selftest`
    // via the table row above -- that is a documented convenience, and it cannot be a typo.
    return selftest_unknown_mode(mode, SUITE_TABLE, SUITE_COUNT);
}
