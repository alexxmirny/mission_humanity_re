//
// udp_rehome_selftest.cpp -- mp:U60 (HM-M2): `Endpoint::rehome`, the id-keeping re-dial and the
// reconcile over the new hub (mh_net_udp/udp_endpoint.cpp, "mp:U60 -- REHOME"). Called from
// `udploopbacktest` (run_rehome_arms) so it rides the existing gate row; the relay half is the
// separate, un-gated `udprehomerelaytest` (run_rehome_relay_role), driven by tools/check_rehome_relay.py
// against the REAL relay because the tunnel is a process-wide singleton: three relayed peers are three
// processes.
//
// WHAT IS PROVED (the done_when of mp:U60, offline half)
//   A 3-endpoint arm: hub + 2 clients at 5 % synthetic loss, the hub killed mid-burst (survivors keep
//     sending into the void, one of them loses the hub's last frames), then ONE SURVIVOR REHOMES AS THE
//     HUB on its own socket and the other RE-DIALS it with its OLD id. Asserted: ids kept; NO WELCOME sent
//     or received (a lifetime counter that rehome must not move); the inbound lanes are NOT cleared (frames
//     queued before the switch are still queued after it); the exactly-once layer's incarnation and own
//     sequence are the same; a FOREIGN id (outside the roster) and the hub's OWN id are refused and told
//     so; frames flow again through the new hub; M1's reconcile completes and the per-origin delivered
//     sequences are gapless, duplicate-free and IDENTICAL across the survivors (313-ish frames disagreed
//     before), including every frame sent while the hub was already dead.
//   B 4-endpoint arm: the survivors re-dial BEFORE the hub has switched (the dial must outlast it).
//   C MUTATIONS, each asserted from inside (a vacuous arm cannot pass):
//     C1 the stop()+start() restart rehome() exists to avoid: lanes cleared, ids re-assigned by a WELCOME,
//        the exactly-once state reset -> the property set does NOT hold.
//     C2 rehome with the reconcile skipped -> the sequences do NOT match.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "../mh_net_udp/udp_endpoint.h"
#include "../mh_net_udp/udp_relay.h"

namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;
namespace osq = mh::netudp::osq;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}
void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[400];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

const unsigned char TEST_PSK[32] = {0x4d, 0x48, 0x74, 0x65, 0x73, 0x74, 0x6b, 0x65, 0x79, 0x20, 0x75, 0x64, 0x70, 0x20, 0x6c, 0x6f,
                                    0x6f, 0x70, 0x62, 0x61, 0x63, 0x6b, 0x20, 0x54, 0x31, 0x20, 0x66, 0x69, 0x78, 0x65, 0x64, 0x21};

void ep_log(void *, const char *line) {
    if (getenv("MH_UDP_LOOPBACK_VERBOSE")) printf("    | %s\n", line);
}

void fill_cfg(Config &c, int role, int port, int player, unsigned short bind_port, bool host_assign) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port          = port;
    c.net.player_id     = player;
    c.net.log           = 1;
    c.net.host_assign   = host_assign ? 1 : 0;
    c.net.ping_ms       = 200;
    c.net.rx_timeout_ms = -1; // a suite that stops to assert must not have a watchdog dropping its peers
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(5);
    }
}

Endpoint g_r[4]; // the old hub + up to 3 survivors; BSS (each is several MB of stream rings plus the layer)
Endpoint g_f[2]; // the foreign dialers

struct Ledger {
    std::vector<uint32_t> seq[osq::ORIGINS]; // per origin, the record counters in DELIVERY order
    void                  clear() {
        for (auto &v : seq) v.clear();
    }
};

void drain(Endpoint &ep, Ledger &led) {
    for (;;) {
        int           sender = -1;
        unsigned char buf[2048];
        int           len = (int)sizeof(buf);
        if (!ep.recv(&sender, buf, &len)) return;
        if (len < 4 || sender < 0 || sender >= osq::ORIGINS) continue;
        led.seq[sender].push_back((uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16));
    }
}

bool is_run(const std::vector<uint32_t> &v) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != (uint32_t)i) return false;
    return true;
}

int queue_depth(Endpoint &ep) {
    int      d = 0, h = 0;
    long     e = 0, r = 0;
    unsigned epoch = 0;
    ep.queue_counters_for_test(&d, &h, &e, &r, &epoch);
    return d;
}

// mode 0 = the real thing; 1 = MUTATION C1 (stop()+start() instead of rehome); 2 = MUTATION C2 (rehome, no reconcile)
bool rehome_arm(const char *name, int clients, int base, int mode, bool dial_first) {
    const int  ROUNDS = 260, KILL_AT = 120, LOSS_PM = 50, TAIL = 12, HOLD = 30, POST = 60;
    const bool quiet = (mode != 0);
    if (!quiet)
        printf("  -- %s (hub + %d clients, %d rounds, hub killed at %d, loss %d/1000, %s)\n", name, clients, ROUNDS,
               KILL_AT, LOSS_PM, dial_first ? "survivors dial BEFORE the hub switches" : "hub switches first");
    Endpoint &hub = g_r[0];
    for (int i = 0; i <= clients; ++i) new (&g_r[i]) Endpoint();
    for (int i = 0; i < 2; ++i) new (&g_f[i]) Endpoint();
    Ledger led[3];
    for (auto &l : led) l.clear();

    Config ch, cc[3];
    fill_cfg(ch, 0, base, 0, (unsigned short)base, true);
    for (int i = 0; i < clients; ++i) fill_cfg(cc[i], 1, base, 1, (unsigned short)(base + 1 + i), true);
    for (int i = 0; i <= clients; ++i) {
        g_r[i].set_log(ep_log, nullptr);
        g_r[i].set_rx_loss(LOSS_PM, 0x5678abcdu + 0x9e3779b9u * (unsigned)i);
    }
    bool ok = hub.start(ch, TEST_PSK, true);
    for (int i = 0; i < clients; ++i) ok &= g_r[1 + i].start(cc[i], TEST_PSK, true);
    check("endpoints started", ok);
    const bool joined = wait_for([&] { return hub.peer_count() == clients; }, 8000);
    const bool ids_ok = joined && wait_for(
                                      [&] {
                                          for (int i = 0; i < clients; ++i)
                                              if (!g_r[1 + i].id_assigned()) return false;
                                          return true;
                                      },
                                      4000);
    checkf(joined && ids_ok, "%s: all clients admitted and assigned ids", name);
    if (!(joined && ids_ok)) {
        for (int i = 0; i <= clients; ++i) g_r[i].stop();
        return false;
    }
    int id[3];
    for (int i = 0; i < clients; ++i) id[i] = g_r[1 + i].local_player_id();

    // ---- the burst: identical shape to M1's hub-kill arm, plus an undrained HOLD at the end ----------
    int      hub_sent = 0, sent[3] = {0, 0, 0};
    uint32_t next_r[4] = {0, 0, 0, 0}; // per sender: the record counter (the run the ledgers must show)
    auto     send_one  = [&](int who_idx) -> bool {
        unsigned char rec[40];
        memset(rec, 0xA0, sizeof(rec));
        const uint32_t r = next_r[who_idx];
        rec[0]           = (unsigned char)(who_idx == 0 ? 0 : id[who_idx - 1]);
        rec[1]           = (unsigned char)(r & 0xff);
        rec[2]           = (unsigned char)((r >> 8) & 0xff);
        rec[3]           = 0;
        if (!g_r[who_idx].send(MH_NET_BROADCAST, rec, sizeof(rec))) return false;
        ++next_r[who_idx];
        return true;
    };
    for (int r = 0; r < ROUNDS; ++r) {
        if (r == KILL_AT - TAIL) g_r[clients].set_rx_loss(900, 0x7a11u); // the last survivor misses the hub's tail
        if (r == KILL_AT) {
            hub.stop(); // abrupt: nothing flushed, nothing forwarded after this line
            g_r[clients].set_rx_loss(LOSS_PM, 0x7a12u);
        }
        if (r < KILL_AT && send_one(0)) ++hub_sent;
        for (int i = 0; i < clients; ++i) {
            if (send_one(1 + i)) ++sent[i];
            if (r < KILL_AT - HOLD) drain(g_r[1 + i], led[i]); // the last HOLD rounds before the kill stay QUEUED for the game
        }
        if ((r % 8) == 7) Sleep(1);
    }
    for (int k = 0; k < 40; ++k) Sleep(10); // whatever is still in flight to a survivor has arrived

    // ---- BEFORE: the state rehome must keep -------------------------------------------------------
    osq::Report rep0[3];
    uint32_t    target0[osq::ORIGINS] = {0};
    long        missing_before = 0, hub_missing_before = 0;
    int         qd_before[3];
    for (int i = 0; i < clients; ++i) {
        g_r[1 + i].osq_report(rep0[i]);
        qd_before[i] = queue_depth(g_r[1 + i]);
        for (int o = 0; o < osq::ORIGINS; ++o)
            if (rep0[i].front[o] > target0[o]) target0[o] = rep0[i].front[o];
    }
    for (int i = 0; i < clients; ++i) {
        for (int o = 0; o < osq::ORIGINS; ++o) missing_before += (long)(target0[o] - rep0[i].front[o]);
        hub_missing_before += (long)(target0[0] - rep0[i].front[0]);
    }
    Endpoint::RehomeStatus st0[3];
    for (int i = 0; i < clients; ++i) g_r[1 + i].rehome_status(st0[i]);
    long simlost = 0;
    for (int i = 0; i < clients; ++i) {
        Counters k;
        g_r[1 + i].counters(k);
        simlost += k.dgram_dropped_sim;
    }

    // ---- THE SWITCH: survivor 0 becomes the hub, the others re-dial it with their old ids ----------
    const int      new_hub = 0; // index into survivors
    Endpoint      &nh      = g_r[1 + new_hub];
    const unsigned nh_port = nh.bound_port();
    checkf(nh_port == (unsigned)(base + 1 + new_hub), "%s: bound_port() reports the socket's port (%u)", name, nh_port);
    bool sw_ok = true;
    if (mode == 1) {
        // MUTATION C1 -- what rehome() exists to AVOID. A restart: new hub = stop()+start() as a HOST, the
        // other survivors stop()+start() as host-assign clients. Everything the properties name is lost.
        for (int i = 0; i < clients; ++i) g_r[1 + i].stop();
        Config nhc, ncc[3];
        fill_cfg(nhc, 0, base + 40, 0, (unsigned short)(base + 40), true);
        sw_ok &= nh.start(nhc, TEST_PSK, true);
        for (int i = 1; i < clients; ++i) {
            fill_cfg(ncc[i], 1, base + 40, 1, (unsigned short)(base + 41 + i), true);
            sw_ok &= g_r[1 + i].start(ncc[i], TEST_PSK, true);
        }
    } else {
        Endpoint::RehomeSpec hs, cs;
        memset(&hs, 0, sizeof(hs));
        memset(&cs, 0, sizeof(cs));
        hs.as_hub = true;
        for (int i = 1; i < clients; ++i) hs.roster[hs.roster_n++] = id[i];
        hs.test_skip_reconcile = (mode == 2);
        cs.as_hub              = false;
        lstrcpynA(cs.host, "127.0.0.1", sizeof(cs.host));
        cs.port                = (int)nh_port;
        cs.dial_budget_ms      = 15000;
        cs.test_skip_reconcile = (mode == 2);
        if (dial_first) {
            for (int i = 1; i < clients; ++i) sw_ok &= g_r[1 + i].rehome(cs);
            Sleep(700); // the dial retries (HS_RETRY_MS 300 ms) against an endpoint that is not a hub yet
            sw_ok &= nh.rehome(hs);
        } else {
            sw_ok &= nh.rehome(hs);
            for (int i = 1; i < clients; ++i) sw_ok &= g_r[1 + i].rehome(cs);
        }
    }
    if (!quiet) checkf(sw_ok, "%s: rehome() accepted by the new hub and every survivor", name);
    // The lanes, sampled the instant the switch returned and BEFORE anything drains them.
    int qd_after[3];
    for (int i = 0; i < clients; ++i) qd_after[i] = queue_depth(g_r[1 + i]);

    // ---- wait for the topology + the reconcile ------------------------------------------------------
    bool seated = wait_for(
        [&] {
            for (int i = 0; i < clients; ++i) drain(g_r[1 + i], led[i]);
            return nh.peer_count() == clients - 1;
        },
        mode == 1 ? 6000 : 15000);
    bool reconciled = true;
    if (mode == 0) {
        reconciled = wait_for(
            [&] {
                for (int i = 0; i < clients; ++i) drain(g_r[1 + i], led[i]);
                for (int i = 0; i < clients; ++i) {
                    Endpoint::RehomeStatus s;
                    g_r[1 + i].rehome_status(s);
                    if (!s.done) return false;
                }
                return true;
            },
            20000);
    } else {
        Sleep(1500); // give a mutated arm the same time the real one would have needed
    }

    // ---- the foreign dialers (mode 0 only): a roster miss and the hub's own id ----------------------
    long foreign_refused = 0;
    if (mode == 0) {
        Config f1, f2;
        fill_cfg(f1, 1, (int)nh_port, 5, (unsigned short)(base + 20), false);           // an id nobody told the hub about
        fill_cfg(f2, 1, (int)nh_port, id[new_hub], (unsigned short)(base + 21), false); // the hub's OWN id
        g_f[0].set_log(ep_log, nullptr);
        g_f[1].set_log(ep_log, nullptr);
        g_f[0].start(f1, TEST_PSK, true);
        g_f[1].start(f2, TEST_PSK, true);
        const bool refused = wait_for(
            [&] {
                Endpoint::RehomeStatus s;
                nh.rehome_status(s);
                return s.foreign_refused >= 2;
            },
            8000);
        Endpoint::RehomeStatus s;
        nh.rehome_status(s);
        foreign_refused = s.foreign_refused;
        checkf(refused, "%s: the hub REFUSED both foreign HELLOs (refused %ld)", name, s.foreign_refused);
        const bool told = wait_for([&] { return g_f[0].peer_count() == 0 && g_f[1].peer_count() == 0; }, 4000);
        Counters   fk0, fk1;
        g_f[0].counters(fk0);
        g_f[1].counters(fk1);
        checkf(fk0.hs_done >= 1 && fk1.hs_done >= 1, "%s: NON-VACUOUS -- both foreign dialers really completed the handshake first", name);
        checkf(told, "%s: ...and each foreign dialer was told (its conn dropped)", name);
        check("...and neither holds a seat on the hub", nh.peer_count() == clients - 1);
        int       ids[8];
        const int n    = nh.active_peer_ids(ids, 8);
        bool      has5 = false, has_own = false;
        for (int k = 0; k < n; ++k) {
            if (ids[k] == 5) has5 = true;
            if (ids[k] == id[new_hub]) has_own = true;
        }
        check("...the seated ids are exactly the roster (no 5, not the hub's own)", n == clients - 1 && !has5 && !has_own);
        g_f[0].stop();
        g_f[1].stop();
    }

    // ---- frames flow again through the new hub -----------------------------------------------------
    for (int k = 0; k < POST; ++k) {
        for (int i = 0; i < clients; ++i)
            if (send_one(1 + i)) ++sent[i];
        for (int i = 0; i < clients; ++i) drain(g_r[1 + i], led[i]);
        if ((k % 8) == 7) Sleep(2);
    }
    for (int k = 0; k < 120; ++k) {
        for (int i = 0; i < clients; ++i) drain(g_r[1 + i], led[i]);
        Sleep(10);
    }
    for (int i = 0; i < clients; ++i) drain(g_r[1 + i], led[i]);

    // ---- AFTER: the properties ---------------------------------------------------------------------
    Endpoint::RehomeStatus st[3];
    osq::Report            rep1[3];
    for (int i = 0; i < clients; ++i) {
        g_r[1 + i].rehome_status(st[i]);
        g_r[1 + i].osq_report(rep1[i]);
    }
    bool ids_kept = true, no_welcome = true, lanes_kept = true, layer_kept = true, run_ok = true, same_ok = true,
         all_in = true, done_ok = true;
    for (int i = 0; i < clients; ++i) {
        if (g_r[1 + i].local_player_id() != id[i] || !g_r[1 + i].id_assigned()) ids_kept = false;
        if (st[i].welcome_tx != st0[i].welcome_tx || st[i].welcome_rx != st0[i].welcome_rx) no_welcome = false;
        if (qd_after[i] < qd_before[i]) lanes_kept = false;
        if (rep1[i].inc[id[i]] != rep0[i].inc[id[i]] || rep1[i].front[id[i]] < rep0[i].front[id[i]]) layer_kept = false;
        if (!st[i].done || st[i].unrecoverable || st[i].aborted || st[i].ranges_noroom != 0) done_ok = false;
        for (int o = 0; o < osq::ORIGINS; ++o) {
            if (o == id[i]) continue;
            if (!is_run(led[i].seq[o])) run_ok = false;
        }
        for (int j = 0; j < clients; ++j)
            if (j != i && (int)led[i].seq[id[j]].size() != sent[j]) all_in = false;
    }
    for (int a = 0; a < clients; ++a)
        for (int b = a + 1; b < clients; ++b)
            for (int o = 0; o < osq::ORIGINS; ++o)
                if (o != id[a] && o != id[b] && led[a].seq[o] != led[b].seq[o]) same_ok = false;
    const bool hub_len_ok = (long)led[0].seq[0].size() == (long)target0[0] && (long)led[1].seq[0].size() == (long)target0[0];

    const bool held = seated && reconciled && ids_kept && no_welcome && lanes_kept && layer_kept && run_ok && same_ok &&
                      all_in && done_ok && hub_len_ok;
    if (quiet) {
        printf("  -- %s (the probe): seated %d reconciled %d ids_kept %d no_welcome %d lanes_kept %d layer_kept %d run %d "
               "same %d all_in %d done %d hub_len %d  => properties %s\n",
               name, (int)seated, (int)reconciled, (int)ids_kept, (int)no_welcome, (int)lanes_kept, (int)layer_kept,
               (int)run_ok, (int)same_ok, (int)all_in, (int)done_ok, (int)hub_len_ok, held ? "HELD (vacuous!)" : "do not hold");
        for (int i = 0; i <= clients; ++i) g_r[i].stop();
        return held;
    }
    long served = 0, ingested = 0;
    for (int i = 0; i < clients; ++i) {
        served += st[i].ranges_served;
        ingested += st[i].ranges_ingested;
    }
    Endpoint::RehomeStatus hs_;
    nh.rehome_status(hs_);
    printf("     hub sent %d, survivors sent", hub_sent);
    for (int i = 0; i < clients; ++i) printf(" %d", sent[i]);
    printf(" | synthetic loss %ld | frames missing across survivors BEFORE %ld (the dead hub's own: %ld) | lanes %d->%d | "
           "reconcile: reports %d plans %d fetches %d, ranges served %ld ingested %ld | foreign refused %ld\n",
           simlost, missing_before, hub_missing_before, qd_before[1 < clients ? 1 : 0], qd_after[1 < clients ? 1 : 0],
           hs_.reports_in, hs_.plans, hs_.fetches_issued, served, ingested, foreign_refused);
    checkf(simlost > 0, "%s: the 5%% loss injection actually fired (%ld datagrams)", name, simlost);
    checkf(missing_before > 0, "%s: NON-VACUOUS -- the survivors really disagreed before the switch (%ld frames missing)", name,
           missing_before);
    checkf(hub_missing_before > 0, "%s: NON-VACUOUS -- survivors held different parts of the DEAD HUB's stream (%ld missing)", name,
           hub_missing_before);
    checkf(qd_before[0] > 0 && qd_before[clients - 1] > 0, "%s: NON-VACUOUS -- frames were QUEUED for the game at the switch (%d, %d)",
           name, qd_before[0], qd_before[clients - 1]);
    checkf(seated, "%s: the survivors re-dialled and are seated on the new hub (peer_count %d)", name, nh.peer_count());
    checkf(ids_kept, "%s: every survivor kept its player id and id_assigned", name);
    checkf(no_welcome, "%s: NO WELCOME was sent or received across the rehome (lifetime counters unmoved)", name);
    checkf(lanes_kept, "%s: the inbound lanes were NOT cleared (queued frames: before %d/%d, after %d/%d)", name, qd_before[0],
           qd_before[clients - 1], qd_after[0], qd_after[clients - 1]);
    checkf(layer_kept, "%s: the exactly-once layer's incarnation and own sequence were kept", name);
    checkf(reconciled && done_ok, "%s: every survivor reached the hub's TARGET frontier (done, nothing unrecoverable/aborted, no range without headroom)",
           name);
    checkf(served > 0 && ingested > 0, "%s: ranges really crossed the new hub (served %ld, ingested %ld)", name, served, ingested);
    checkf(run_ok, "%s: every origin's delivered sequence is a gapless, duplicate-free run at every survivor", name);
    checkf(same_ok, "%s: per-origin delivered sequences are IDENTICAL across the survivors", name);
    checkf(all_in, "%s: every frame a surviving origin sent -- including those sent into the dead hub and after the switch -- reached every survivor", name);
    checkf(hub_len_ok, "%s: the dead hub's stream ends at the max frontier any survivor held (%u)", name, target0[0]);
    for (int i = 0; i < clients; ++i) {
        const osq::Counters oc = g_r[1 + i].osq_counters();
        checkf(oc.gap_skipped == 0, "%s: survivor %d never skipped a hole", name, id[i]);
    }
    for (int i = 0; i <= clients; ++i) g_r[i].stop();
    return held;
}

} // namespace

// ================================================================================================
// the relay half: ONE peer of a three-process arm (tools/check_rehome_relay.py is the conductor)
// ================================================================================================
//
//   udprehomerelaytest <role> <relay host:port> <dir> <old_room> <new_room>
//     role h  = the OLD HOST: hosts <old_room>, sends numbered frames, is killed by the conductor
//     role c1 = survivor that BECOMES THE HUB: re-homes its tunnel into <new_room> as the host, then its endpoint
//     role c2 = the other survivor: moves its tunnel to <new_room> FIRST (the new host has not registered yet,
//               so the relay answers no_host and the HELLO retries), then re-dials with its old id
// Files in <dir> are the whole control channel: `<role>.up` (admitted), `go_c1` / `go_c2` (conductor), `stop_send`
// (conductor), `<role>.ledger` (the result). Fixed ids: h=0, c1=1, c2=2 (host_assign off), open key.
namespace {

void write_file(const char *dir, const char *name, const char *text) {
    char path[400];
    wsprintfA(path, "%s\\%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}
bool file_exists(const char *dir, const char *name) {
    char path[400];
    wsprintfA(path, "%s\\%s", dir, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
void rlog(const char *role, const char *fmt, ...) {
    char    b[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", role, b);
    fflush(stdout);
}
void relay_ep_log(void *ctx, const char *line) {
    if (getenv("MH_UDP_LOOPBACK_VERBOSE") || strstr(line, "REHOME") || strstr(line, "RE-HOM") || strstr(line, "relay") ||
        strstr(line, "reconcile") || strstr(line, "refus"))
        printf("    | [%s] %s\n", (const char *)ctx, line);
}

} // namespace

int run_rehome_relay_role(int argc, char **argv) {
    // argv[0] = the exe, argv[1] = the suite name (adapt_argv hands the whole line)
    if (argc < 8) {
        printf("usage: net_selftest.exe udprehomerelaytest <h|c1|c2> <relayhost:port> <dir> <old_room> <new_room> <basePort>\n");
        return 2;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const char *role = argv[2];
    char        rhost[64];
    int         rport = 0;
    {
        const char *colon = strrchr(argv[3], ':');
        if (!colon) return 2;
        lstrcpynA(rhost, argv[3], (int)(colon - argv[3] + 1) < 64 ? (int)(colon - argv[3] + 1) : 64);
        rport = atoi(colon + 1);
    }
    const char    *dir      = argv[4];
    const uint32_t old_room = (uint32_t)strtoul(argv[5], nullptr, 10);
    const uint32_t new_room = (uint32_t)strtoul(argv[6], nullptr, 10);
    const int      base     = atoi(argv[7]);
    const int      who      = !strcmp(role, "h") ? 0 : (!strcmp(role, "c1") ? 1 : 2);
    uint8_t        psk[32];
    memset(psk, 0, sizeof(psk)); // the OPEN key: the relay runs without --key

    Endpoint &ep = g_r[0];
    new (&ep) Endpoint();
    ep.set_log(relay_ep_log, (void *)role);

    Config c;
    fill_cfg(c, who == 0 ? 0 : 1, base, who, (unsigned short)(base + who), false);
    c.net.rx_timeout_ms = -1;
    c.leg_overhead      = mh::udprelay::LEG_OVERHEAD;

    mh::udprelay::Config rc;
    memset(&rc, 0, sizeof(rc));
    lstrcpynA(rc.host, rhost, sizeof(rc.host));
    rc.port        = (unsigned short)rport;
    rc.room        = old_room;
    rc.role        = who == 0 ? 0 : 1;
    rc.game_port   = (unsigned short)(base + who);
    rc.force_relay = 1;
    if (!mh::udprelay::start(rc, psk, relay_ep_log, (void *)role)) {
        rlog(role, "tunnel failed to start");
        return 1;
    }
    if (who != 0) {
        c.net.port = (int)mh::udprelay::client_dial_port();
        lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    }
    if (!ep.start(c, psk, false)) {
        rlog(role, "endpoint failed to start");
        return 1;
    }
    // admitted?
    const bool up = wait_for([&] { return ep.peer_count() >= (who == 0 ? 2 : 1); }, 30000);
    if (!up) {
        rlog(role, "never admitted (peer_count %d)", ep.peer_count());
        return 1;
    }
    char nm[64];
    wsprintfA(nm, "%s.up", role);
    write_file(dir, nm, "1");
    rlog(role, "admitted; sending numbered frames");

    Ledger   led;
    uint32_t next_r  = 0;
    int      sent    = 0;
    bool     rehomed = false, sending = true;
    DWORD    t_rehome = 0;
    bool     ok       = true;
    for (;;) {
        if (sending) {
            unsigned char rec[40];
            memset(rec, 0xA0, sizeof(rec));
            rec[0] = (unsigned char)who;
            rec[1] = (unsigned char)(next_r & 0xff);
            rec[2] = (unsigned char)((next_r >> 8) & 0xff);
            rec[3] = (unsigned char)((next_r >> 16) & 0xff);
            if (ep.send(MH_NET_BROADCAST, rec, sizeof(rec))) {
                ++next_r;
                ++sent;
            }
        }
        drain(ep, led);
        if (!rehomed && who != 0 && file_exists(dir, who == 1 ? "go_c1" : "go_c2")) {
            rehomed  = true;
            t_rehome = GetTickCount();
            if (who == 1) {
                // the NEW HUB: tunnel first (fresh host registration in the pre-minted room), then the endpoint
                mh::udprelay::Rehome rh;
                rh.role                = 0;
                rh.room                = new_room;
                rh.game_port           = ep.bound_port();
                const bool           t = mh::udprelay::rehome(rh);
                Endpoint::RehomeSpec hs;
                memset(&hs, 0, sizeof(hs));
                hs.as_hub            = true;
                hs.roster[0]         = 2;
                hs.roster_n          = 1;
                hs.reconcile_wait_ms = 20000;
                const bool e         = ep.rehome(hs);
                rlog(role, "rehome as HUB into room %u: tunnel %d endpoint %d", new_room, (int)t, (int)e);
                ok &= t && e;
            } else {
                mh::udprelay::Rehome rh;
                rh.role                = 1;
                rh.room                = new_room;
                rh.game_port           = 0;
                const bool           t = mh::udprelay::rehome(rh);
                Endpoint::RehomeSpec cs;
                memset(&cs, 0, sizeof(cs));
                cs.as_hub = false;
                lstrcpynA(cs.host, "127.0.0.1", sizeof(cs.host));
                cs.port           = (int)mh::udprelay::client_dial_port();
                cs.dial_budget_ms = 40000;
                const bool e      = ep.rehome(cs);
                rlog(role, "rehome as CLIENT into room %u: tunnel %d endpoint %d", new_room, (int)t, (int)e);
                ok &= t && e;
            }
        }
        if (sending && file_exists(dir, "stop_send")) {
            sending = false;
            rlog(role, "stopped sending at %u frames; settling", next_r);
            DWORD until = GetTickCount() + 6000;
            while ((long)(until - GetTickCount()) > 0) {
                drain(ep, led);
                Sleep(10);
            }
            break;
        }
        if (who == 0 && file_exists(dir, "exit_now")) break;
        Sleep(5);
    }
    Endpoint::RehomeStatus st;
    ep.rehome_status(st);
    char path[400];
    wsprintfA(path, "%s\\%s.ledger", dir, role);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "role=%s id=%d sent=%u done=%d unrec=%d aborted=%d welcome_tx=%ld welcome_rx=%ld served=%ld ingested=%ld "
                   "noroom=%ld foreign=%ld rehomed=%d peers=%d\n",
                role, who, next_r, (int)st.done, (int)st.unrecoverable, (int)st.aborted, st.welcome_tx, st.welcome_rx,
                st.ranges_served, st.ranges_ingested, st.ranges_noroom, st.foreign_refused, (int)rehomed, ep.peer_count());
        for (int o = 0; o < osq::ORIGINS; ++o) {
            fprintf(f, "origin %d n=%zu run=%d", o, led.seq[o].size(), (int)is_run(led.seq[o]));
            fprintf(f, " last=%u\n", led.seq[o].empty() ? 0u : led.seq[o].back());
        }
        fclose(f);
    }
    (void)t_rehome;
    (void)sent;
    rlog(role, "ledger written (%s)", ok ? "ok" : "REHOME CALL FAILED");
    ep.stop();
    mh::udprelay::stop();
    return ok ? 0 : 1;
}

int run_rehome_arms() {
    const int fails0 = g_fails;
    printf("  -- mp:U60 rehome: the id-keeping hub change + reconcile\n");
    rehome_arm("3-endpoint rehome", 2, 39840, 0, false);
    rehome_arm("4-endpoint rehome (survivors dial first)", 3, 39870, 0, true);
    const bool restart_held = rehome_arm("mutation C1", 2, 39910, 1, false);
    check("MUTATION C1: a stop()+start() restart is DETECTED (lanes/ids/layer/sequences do not survive it)", !restart_held);
    const bool noreconcile_held = rehome_arm("mutation C2", 2, 39930, 2, false);
    check("MUTATION C2: a rehome with the reconcile skipped is DETECTED (the sequences differ)", !noreconcile_held);
    printf("     rehome: %d checks so far, %d failures in this block\n", g_checks, g_fails - fails0);
    return g_fails - fails0;
}
