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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mh_net_export.h"
#include "mh_seam_export.h"
#include "hostapi_selftest_support.h"         // LIB-ABI: the selftest host table bound in main()
#include "../libmh/state/host_api.h"          // LIB-ABI: libmh_set_host_api
#include "../mh/include/mh_libmh_hook_bind.h" // F4D-PRE: MH_LibMH_BindHookApi
#include "../mh/addr/mh_rebind.gen.h"         // LIB-REBIND R11: load_gates(nullptr) in main()
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

static void seam_point_at_mock() {
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
    seam_point_at_mock();
    printf("[seamhost] listening; polling seam...\n");
    for (int t = 0; t < 500; ++t) {
        int r = MH_Seam_PollRecv();
        if (r == SEAM_RX_SIZE) {
            int ok = 1;
            if (g_mock[0] != 0x0c) {
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
            return ok ? 0 : 1;
        }
        Sleep(10);
    }
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
    unsigned char pkt[16];
    pkt[0] = 0x0c;                            // type: client slot-state push
    pkt[1] = pkt[2] = pkt[3] = pkt[4] = 0xEE; // garbage CRC field -- receiver ignores it
    memcpy(pkt + 5, "SLOTDATA", 8);
    MH_Seam_Send(0, 0, pkt, 13); // mode 0 = unicast to dest player 0 (host)
    printf("[seamcli]  sent slot-push (0x0c) to host\n");
    Sleep(400);
    return 0;
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
// orders_selftest.cpp -- the order container's logic over heap buffers (branches no rig can reach)
int run_orderstest();
// issue_selftest.cpp -- O4-0: the order-ISSUE wrappers, one layer ABOVE the container. Drives each
// reimplemented wrapper with concrete inputs and compares what it handed the container against a
// golden EXTRACTED FROM THE ORIGINAL's disassembly (order_issue_golden.gen.h), so the expectations
// are not authored by the same reading that authored the translation.
int run_issuetest();
// lockstep_selftest.cpp -- the turn engine's horizon/barrier logic, same arrangement (L1)
int run_lockstest();
// save_selftest.cpp -- the save FORMAT over buffers: batch A's version gate and batch B's block
// layer + LZW codec. Optional argv[2] = a real .sav to push every block of through the layer.
int run_savetest(int argc, char **argv);
int run_resynctest();
// net_session_selftest.cpp -- NET-SESSION: the process-level lockstep bootstrap
// (llm_net_session_globals_reset) and the mode-8 leader resync frame (llm_wait_screen_frame).
// Neither is armable -- one runs ONCE per process at WM_CREATE, the other SENDS a packet and drains
// the socket -- so this plus the live `[netprobe]` read-back is their whole evidence.
int run_netsessiontest();
// lib_trans_selftest.cpp -- LT0: the lib_trans domain oracle's expectation layer (RNG-family golden
// vectors + batch-A wrapper contracts, pinned against the verified rng_next body).
int run_libtranstest();
int run_watchdogtest();
int run_desynctest();
// net_queue_selftest.cpp -- D24: which inbound frame a FULL transport queue may destroy.
int run_queuetest();
int run_interlocktest();
// inmem_patch_selftest.cpp -- F1E: the in-memory static-patch applier and, above all, its REFUSALS
// (a moved guarded byte, an already-patched image, a site inside a promoted body, an unavailable
// cave VA). Same argument as interlocktest: every one of them is about a write that must not happen.
int run_patchtest();
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
int run_boottest(int argc, char **argv);
// world_snapshot_selftest.cpp -- LIB-WORLD: the step-0 world fixture. `worldtest` runs a
// synthesised blob through the format, the refusals and all three coverage arms; `worldtest <blob>`
// imports a REAL step-0 capture into a poisoned arena and reproduces the recording peer's lockstep
// hash -- the done_when's oracle, in a process with no game to reproduce it from.
int run_worldtest(int argc, char **argv);
int run_navtest();
// state_selftest.cpp (same TU) -- SB-BIND T1: the HOST answers where the state is, through the C
// ABI a standalone host will use. The stock answer is a no-op BY CONSTRUCTION; the arm with teeth
// relocates one region and shows the size-vs-reach choice decide whether an overrunning save block
// resolves or comes back null.
int run_bindtest();
// ai_selftest.cpp -- AI0: the strategic AI's decision logic over heap buffers. Most of the ~215-
// function AI cluster is pure over its state, so this is the oracle it is verified with; a rig run
// cannot reach an empty candidate list, a negative damage tally, or the player-7 record overrun.
int run_aitest();
// sim_selftest.cpp -- SIM0: the strategic sim's logic over heap buffers, the sibling of aitest and
// the lever that keeps the 307-function sim migration off the rig. The sim's rig runs are the
// expensive ones, so per-function iteration happening here instead is the largest saving RI-SIM has.
int run_simtest();
// crt_sprintf_selftest.cpp -- LIB-CRT: the vendored sprintf family. A standalone libmh cannot reach
// the binary's Watcom CRT at a VA, so crt/crt_sprintf.h supplies the 12 shapes and this is the oracle
// that says they mean the same thing. The half that earns it is the six NARROW sites: they build
// filenames, nothing else compares them, and a wrong one loads a different file rather than
// misdrawing a pixel.
int run_crttest();
// crt_vendor_selftest.cpp -- LIB-CRT, the other five vendored headers (crt_string / crt_math /
// crt_heap / crt_rand / crt_qsort). Same suite name, separate TU: its reference arm is the ORIGINAL
// MACHINE CODE transcribed into naked functions, so `crttest` runs both halves and the failure
// counts are summed.
int run_crt_vendor_test();
// fp_x87_selftest.cpp -- CRT-X87 step 2: mh/fp/x87.h's helpers were assembly and are now C++, and
// this keeps a verbatim copy of the assembly as the reference arm so the equality is re-proved every
// run rather than trusted from a note. Runs at BOTH x87 precision settings, because the harness pins
// PC=53 and the bare default is PC=64 and a helper correct at only one would look fine for months.
int run_fptest();
// tact_selftest.cpp -- TACT-DOMAIN: the OFFLINE half of the tactical oracle. The trajectory oracle
// (test_ui.py --tact-determinism) compares a COMBINED hash over 14 slices and therefore cannot
// audit its own coverage: a wrong slice length, or a mask that dropped everything, makes both arms
// identically wrong and prints green. This is where all 14 slices are proven to be read, and the
// arena's strides proven to match the manifest -- in a process with no game, no rig, no mission.
int run_tacttest();

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "selftest";
    int         port = (argc > 2) ? atoi(argv[2]) : 39500;
    if (port <= 0) port = 39500;

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

    int extra = (argc > 3) ? atoi(argv[3]) : 0; // per-mode: player id for recv/bcast
    if (strcmp(mode, "host") == 0) return run_host(port);
    else if (strcmp(mode, "client") == 0) return run_client(port, (argc > 3) ? argv[3] : NULL);
    else if (strcmp(mode, "recv") == 0) return run_recv(port, extra ? extra : 2);
    // `selftest` NAMED EXPLICITLY. It used to reach run_selftest only via the fall-through below,
    // and when that fall-through became an error (2026-08-01, so a mistyped mode stops printing
    // PASS) this branch was not added -- so the gate's own documented first step,
    // `net_selftest.exe selftest`, started exiting 2 with the mode list. Found 2026-08-01 by running
    // the gate; the hardening fix had broken the thing it was hardening.
    else if (strcmp(mode, "selftest") == 0) return run_selftest(port);
    else if (strcmp(mode, "bcast") == 0) return run_bcast(port, extra ? extra : 1);
    else if (strcmp(mode, "selftest3") == 0) return run_selftest3(port);
    else if (strcmp(mode, "seam_host") == 0) return run_seam_host(port);
    else if (strcmp(mode, "seam_client") == 0) return run_seam_client(port);
    else if (strcmp(mode, "seamtest") == 0) return run_seamtest(port);
    else if (strcmp(mode, "watch_host") == 0) return run_watch_host(port);
    else if (strcmp(mode, "mute_peer") == 0) return run_mute_peer(port);
    else if (strcmp(mode, "linktest") == 0) return run_linktest(port);
    else if (strcmp(mode, "probe") == 0) return run_mute_probe(port);
    else if (strcmp(mode, "authtest") == 0) return run_authtest(port);
    else if (strcmp(mode, "menutest") == 0) return run_menutest();
    else if (strcmp(mode, "launchtest") == 0) return run_launchtest();
    else if (strcmp(mode, "callstest") == 0) return run_callstest();
    else if (strcmp(mode, "exportstest") == 0) return run_exportstest();
    else if (strcmp(mode, "orderstest") == 0) return run_orderstest();
    else if (strcmp(mode, "issuetest") == 0) return run_issuetest();
    else if (strcmp(mode, "lockstest") == 0) return run_lockstest();
    else if (strcmp(mode, "savetest") == 0) return run_savetest(argc, argv);
    else if (strcmp(mode, "resynctest") == 0) return run_resynctest();
    else if (strcmp(mode, "netsessiontest") == 0) return run_netsessiontest();
    else if (strcmp(mode, "libtranstest") == 0) return run_libtranstest();
    else if (strcmp(mode, "watchdogtest") == 0) return run_watchdogtest();
    else if (strcmp(mode, "desynctest") == 0) return run_desynctest();
    else if (strcmp(mode, "queuetest") == 0) return run_queuetest();
    else if (strcmp(mode, "interlocktest") == 0) return run_interlocktest();
    else if (strcmp(mode, "patchtest") == 0) return run_patchtest();
    else if (strcmp(mode, "tombstonetest") == 0) return run_tombstonetest();
    else if (strcmp(mode, "hostapitest") == 0) return run_hostapitest();
    else if (strcmp(mode, "hostintest") == 0) return run_hostintest();
    else if (strcmp(mode, "statetest") == 0) return run_statetest();
    else if (strcmp(mode, "boottest") == 0) return run_boottest(argc, argv);
    else if (strcmp(mode, "worldtest") == 0) return run_worldtest(argc, argv);
    else if (strcmp(mode, "navtest") == 0) return run_navtest();
    else if (strcmp(mode, "bindtest") == 0) return run_bindtest();
    else if (strcmp(mode, "aitest") == 0) return run_aitest();
    else if (strcmp(mode, "simtest") == 0) return run_simtest();
    else if (strcmp(mode, "tacttest") == 0) return run_tacttest();
    else if (strcmp(mode, "crttest") == 0) {
        // LIB-CRT has TWO halves and both must run: the sprintf family (expectations written out
        // from the standard) and the other five vendored headers (expectations transcribed from the
        // original machine code). Run both and SUM -- returning the first non-zero would hide a
        // failure in the second.
        const int a = run_crttest();
        const int b = run_crt_vendor_test();
        return (a != 0 || b != 0) ? 1 : 0;
    } else if (strcmp(mode, "fptest") == 0) return run_fptest();
    else if (argc > 1) {
        // AN UNRECOGNISED MODE IS AN ERROR, NOT A DEFAULT. This used to fall through to
        // run_selftest, so a mistyped name printed "=== PASS: transport loopback round-trip
        // succeeded ===" and exited 0 -- a green result for a test that does not exist. Measured
        // 2026-08-01 while running the gate by hand: `lockstepstest` (a typo of `lockstest`)
        // reported PASS. Every check in this file exists because a green result that could not go
        // red is worth less than no check, and the dispatcher was quietly manufacturing them.
        // The BARE invocation still defaults to selftest -- that is a documented convenience, and
        // it cannot be a typo.
        printf("unknown mode '%s'\n", mode);
        printf("modes: host client recv bcast selftest selftest3 seam_host seam_client seamtest\n"
               "       watch_host mute_peer linktest probe authtest menutest launchtest callstest\n"
               "       exportstest orderstest issuetest lockstest savetest resynctest interlocktest patchtest desynctest\n"
               "       queuetest\n"
               "       statetest boottest worldtest aitest simtest tacttest tombstonetest hostapitest\n"
               "       hostintest\n"
               "       crttest fptest\n");
        return 2;
    } else return run_selftest(port);
}
