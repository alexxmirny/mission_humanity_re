//
// udp_punch_selftest.cpp -- `net_selftest.exe udppunchtest` (tracker mp:R3).
//
// THE PROMOTION STATE MACHINE, WITH NO NETWORK. mp:R3's four acceptance clauses are all rig clauses:
// two VMs, a firewall rule applied over ssh, a relay process, and several minutes per answer. That
// is the right place to prove the item WORKS and the wrong place to find out WHY it did not -- a red
// rig run says a pair stayed relayed and nothing about whether the fault was the candidate codec,
// the probe cadence, the promotion rule or the demotion timer.
//
// So the decision half was written as a pure function of (state, event, time) in
// src/mh_dll/mh_net_udp/udp_punch.cpp, and this suite drives it as a table of milliseconds. Every
// arm here is a question the rig can only ask by waiting for it:
//
//   A  the candidate codec, against the same refusals the Rust half asserts (leg.rs's tests).
//   B  the happy path: candidates in, probes out, echo back, peer's probe seen, PROMOTED.
//   C  the RULE, stated as two negatives -- an echo alone does not promote, and a peer's probe
//      alone does not either. This is the mutation check on the promotion condition: an `||` where
//      the code has `&&` passes every other arm in this file.
//   D  the two probe cadences and the ten-second window they are split by, which is the clause
//      "direct within 10 s of match start" reduced to the only part of it this side controls.
//   E  a probe from an address NOBODY ADVERTISED becomes a candidate -- the arm that makes punching
//      work when one end's NAT maps a different port toward the peer than toward the relay.
//   F  demotion on silence: ANSWERED keepalives keep a path, unanswered ones lose it at DEAD_MS.
//   F2 the ONE-WAY failure, which is the bug this file was rewritten around -- a path whose inbound
//      half is busy and whose outbound half is dead must still demote. On the rig, before the fix, a
//      mid-match cut demoted the client in 3 s and never demoted the host, and the run stalled.
//   G  the pair recovers: after a demotion the same path can be found again.
//   H  `[net] force_relay=1` never promotes, even handed a fully validated path.
//   I  the 32-bit tick WRAP. GetTickCount() wraps every 49.7 days and a comparison of two stamps is
//      wrong across it while a subtraction is right; a match running over midnight-of-the-counter
//      must not demote every peer at once.
//
#include <stdio.h>
#include <string.h>

#include "../mh_net_udp/udp_punch.h"

using namespace mh::udppunch;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

Cand v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port, uint8_t flags = 0) {
    Cand x;
    memset(&x, 0, sizeof(x));
    x.fam     = FAM_V4;
    x.flags   = flags;
    x.port    = port;
    x.addr[0] = a;
    x.addr[1] = b;
    x.addr[2] = c;
    x.addr[3] = d;
    return x;
}

Cand v6(uint16_t tail, uint16_t port) {
    Cand x;
    memset(&x, 0, sizeof(x));
    x.fam      = FAM_V6;
    x.port     = port;
    x.addr[0]  = 0x20;
    x.addr[1]  = 0x01;
    x.addr[2]  = 0x0d;
    x.addr[3]  = 0xb8;
    x.addr[14] = (uint8_t)(tail >> 8);
    x.addr[15] = (uint8_t)(tail & 0xff);
    return x;
}

// Probe once at `now` and, for each candidate the machine chose, immediately hand back the echo.
// The "network" in this file is these three lines: whatever it wanted to probe, answered.
int probe_and_echo(Punch &p, uint32_t now, bool echo) {
    int       idx[MAX_CANDS];
    uint64_t  tok[MAX_CANDS];
    const int n = due_probes(p, now, idx, tok, MAX_CANDS);
    if (echo)
        for (int i = 0; i < n; ++i) on_ack(p, tok[i], now);
    return n;
}

// ---- A: the codec --------------------------------------------------------------------------------

void arm_codec() {
    printf("-- A: the candidate codec\n");
    Cand      list[3] = {v4(192, 168, 0, 38, 6501), v6(0x1234, 40000), v4(203, 0, 113, 9, 54321, FLAG_OBSERVED)};
    uint8_t   buf[256];
    const int n = cands_encode(list, 3, buf, (int)sizeof(buf));
    check("three candidates encode", n == 1 + (4 + 4) + (4 + 16) + (4 + 4));

    Cand got[MAX_CANDS];
    check("...and decode to the same three", cands_decode(buf, n, got, MAX_CANDS) == 3);
    check("v4 survives", cand_eq(got[0], list[0]) && got[0].port == 6501);
    check("v6 survives whole", cand_eq(got[1], list[1]) && memcmp(got[1].addr, list[1].addr, 16) == 0);
    check("the OBSERVED flag survives", (got[2].flags & FLAG_OBSERVED) != 0);

    // EVERY truncation is a refusal, never a shorter list -- the property the Rust half asserts too.
    bool all_refused = true;
    for (int cut = 0; cut < n; ++cut)
        if (cands_decode(buf, cut, got, MAX_CANDS) >= 0) all_refused = false;
    check("every truncation is refused", all_refused);

    uint8_t trailing[256];
    memcpy(trailing, buf, (size_t)n);
    trailing[n] = 0;
    check("a trailing byte is refused", cands_decode(trailing, n + 1, got, MAX_CANDS) < 0);

    uint8_t lying[256];
    memcpy(lying, buf, (size_t)n);
    lying[0] = MAX_CANDS + 1;
    check("a count over the ceiling is refused", cands_decode(lying, n, got, MAX_CANDS) < 0);

    uint8_t badfam[256];
    memcpy(badfam, buf, (size_t)n);
    badfam[1] = 7;
    check("an unknown family is refused, not skipped", cands_decode(badfam, n, got, MAX_CANDS) < 0);

    // An EMPTY payload is malformed; an empty LIST is a legal answer.
    check("an empty payload is refused", cands_decode(buf, 0, got, MAX_CANDS) < 0);
    uint8_t   empty[4];
    const int en = cands_encode(list, 0, empty, (int)sizeof(empty));
    check("an empty list round-trips as zero", en == 1 && cands_decode(empty, en, got, MAX_CANDS) == 0);

    // A destination smaller than the list is a refusal at the DECODER, not a partial fill.
    check("a cap under the count is refused", cands_decode(buf, n, got, 2) < 0);
    check("an output buffer too small encodes nothing", cands_encode(list, 3, buf, 8) == 0);
}

// ---- B, C: the promotion rule ---------------------------------------------------------------------

void arm_promotion() {
    printf("-- B/C: the promotion rule\n");
    const Cand reflexive = v4(203, 0, 113, 7, 41000, FLAG_OBSERVED);
    const Cand local     = v4(192, 168, 0, 38, 41000);

    // B: the happy path.
    {
        Punch p;
        reset(p, 0x1234);
        check("nothing to send before any candidate", probe_and_echo(p, 1000, false) == 0);
        check("relayed while idle", direct_target(p) == nullptr);

        Cand offered[2] = {local, reflexive};
        check("two new candidates", add_cands(p, offered, 2, 1000) == 2);
        check("the same two again are not new", add_cands(p, offered, 2, 1000) == 0);

        check("both are probed at once", probe_and_echo(p, 1000, true) == 2);
        check("an echo alone is not a promotion", tick(p, 1010) == EV_NONE);
        on_peer_probe(p, reflexive, 1020);
        check("PROMOTED once the peer's own probe lands", tick(p, 1020) == EV_PROMOTED);
        const Cand *t = direct_target(p);
        check("and the target is the path that validated", t != nullptr && cand_eq(*t, reflexive));
        check("counted", p.promotions == 1 && p.demotions == 0);
        check("the elapsed punch is readable", (uint32_t)(p.promoted_ms - p.started_ms) == 20);
    }

    // C: the two negatives. THE mutation check -- an `||` in the promotion condition passes every
    // other arm of this file and fails exactly these two.
    {
        Punch p;
        reset(p, 1);
        Cand one = local;
        add_cands(p, &one, 1, 0);
        probe_and_echo(p, 0, true); // acked, never heard
        bool stayed = true;
        for (uint32_t t = 0; t < 30000; t += 250)
            if (tick(p, t) != EV_NONE) stayed = false;
        check("acked but never heard stays relayed", stayed && direct_target(p) == nullptr);
    }
    {
        Punch p;
        reset(p, 2);
        Cand one = local;
        add_cands(p, &one, 1, 0);
        probe_and_echo(p, 0, false); // probed, never echoed
        on_peer_probe(p, one, 10);   // heard, never acked
        bool stayed = true;
        for (uint32_t t = 0; t < 30000; t += 250)
            if (tick(p, t) != EV_NONE) stayed = false;
        check("heard but never acked stays relayed", stayed && direct_target(p) == nullptr);
    }
    {
        // A STALE ECHO credits nothing: a token already consumed, and a token never sent.
        Punch p;
        reset(p, 3);
        Cand one = local;
        add_cands(p, &one, 1, 0);
        int      idx[MAX_CANDS];
        uint64_t tok[MAX_CANDS];
        due_probes(p, 0, idx, tok, MAX_CANDS);
        check("the echo lands", on_ack(p, tok[0], 0));
        check("the same echo again lands nowhere", !on_ack(p, tok[0], 0));
        check("a nonce nobody sent lands nowhere", !on_ack(p, tok[0] ^ 0xa5a5, 0));
        check("and zero is never a valid nonce", !on_ack(p, 0, 0));
    }
}

// ---- D: the cadence -------------------------------------------------------------------------------

void arm_cadence() {
    printf("-- D: the probe cadence and the ten-second window\n");
    Punch p;
    reset(p, 7);
    Cand two[2] = {v4(10, 0, 0, 1, 5000), v4(10, 0, 0, 2, 5000)};
    add_cands(p, two, 2, 0);

    // Inside the fast window: a probe per candidate every FAST_MS and none in between.
    check("the first pump probes at once", probe_and_echo(p, 0, false) == 2);
    check("nothing until FAST_MS", probe_and_echo(p, FAST_MS - 1, false) == 0);
    check("then both again", probe_and_echo(p, FAST_MS, false) == 2);

    int fast = 0;
    for (uint32_t t = FAST_MS; t < FAST_WINDOW_MS; t += 50) fast += probe_and_echo(p, t, false);
    // (FAST_WINDOW_MS - FAST_MS) / FAST_MS rounds of 2 probes, give or take the first.
    check("the fast window probes ~4x a second", fast >= 2 * 38 && fast <= 2 * 40);

    // Past it, the slow cadence -- the job stops being "find a path fast" and becomes "notice if
    // one becomes possible", which is what a lifted firewall rule looks like.
    probe_and_echo(p, FAST_WINDOW_MS + 1, false);
    check("nothing at the fast rate any more", probe_and_echo(p, FAST_WINDOW_MS + 1 + FAST_MS, false) == 0);
    check("but still probing at the slow rate",
          probe_and_echo(p, FAST_WINDOW_MS + 1 + SLOW_MS, false) == 2);
}

// ---- E: an address nobody advertised --------------------------------------------------------------

void arm_unadvertised() {
    printf("-- E: a probe from an address nobody advertised\n");
    Punch p;
    reset(p, 11);
    const Cand advertised = v4(203, 0, 113, 7, 41000, FLAG_OBSERVED);
    Cand       a          = advertised;
    add_cands(p, &a, 1, 0);

    // The peer is behind a NAT that maps a DIFFERENT port toward us than toward the relay, so the
    // advertised candidate is wrong for everybody and only the inbound probe carries the truth.
    const Cand real = v4(203, 0, 113, 7, 49999);
    const int  at   = on_peer_probe(p, real, 100);
    check("the inbound address became a candidate", at == 1 && p.n == 2);
    check("and it is HEARD already", p.path[at].heard);

    probe_and_echo(p, 250, true); // now we probe it too, and it answers
    check("PROMOTED onto the address that actually reached us", tick(p, 260) == EV_PROMOTED);
    const Cand *t = direct_target(p);
    check("not onto the advertised one", t != nullptr && cand_eq(*t, real) && !cand_eq(*t, advertised));

    // A repeat of a KNOWN address is not a second entry.
    const int again = on_peer_probe(p, real, 300);
    check("a repeat does not grow the table", again == at && p.n == 2);

    // And the table is a ceiling, not a suggestion.
    Punch q;
    reset(q, 12);
    for (int i = 0; i < MAX_CANDS + 4; ++i) on_peer_probe(q, v4(10, 0, 1, (uint8_t)i, 1), 0);
    check("the table stops at MAX_CANDS", q.n == MAX_CANDS);
    check("and refuses the overflow by name", on_peer_probe(q, v4(10, 9, 9, 9, 1), 0) == -1);
}

// ---- F, G: demotion and recovery ------------------------------------------------------------------

void promote(Punch &p, const Cand &c, uint32_t at) {
    Cand one = c;
    add_cands(p, &one, 1, at);
    probe_and_echo(p, at, true);
    on_peer_probe(p, c, at);
    tick(p, at);
}

void arm_demotion() {
    printf("-- F/G: demotion on silence, and recovery\n");
    const Cand path = v4(198, 51, 100, 4, 6000);
    Punch      p;
    reset(p, 21);
    promote(p, path, 1000);
    check("direct", direct_target(p) != nullptr);

    // ANSWERED keepalives keep it direct, and nothing else does. Nineteen seconds of them.
    for (uint32_t t = 1000; t < 20000; t += 500) {
        probe_and_echo(p, t, true); // due at KEEP_MS; the echo is what counts
        if (tick(p, t) != EV_NONE) check("answered keepalives kept it direct", false);
    }
    check("nineteen seconds of answered keepalives, still direct",
          direct_target(p) != nullptr && p.demotions == 0);

    // Then the path goes dark: the keepalives still GO, nothing comes back. Not a fault -- a NAT
    // rebinding, a Wi-Fi roam -- so the cost of being wrong is one relayed second, timer is short.
    // The keepalive is due every KEEP_MS, so the last one the loop above echoed was at 19000.
    const uint32_t last = 19000;
    for (uint32_t t = 20000; t < last + DEAD_MS; t += 250) probe_and_echo(p, t, false);
    check("still direct just under the timer", tick(p, last + DEAD_MS - 1) == EV_NONE);
    check("DEMOTED at it", tick(p, last + DEAD_MS) == EV_DEMOTED);
    check("and relayed again", direct_target(p) == nullptr);
    check("counted", p.demotions == 1 && p.promotions == 1);

    // G: the search restarts, INCLUDING on the path that just died -- the commonest cause is a
    // rebinding, which the next candidate exchange repairs.
    check("probing again at the fast rate", probe_and_echo(p, last + DEAD_MS, true) == 1);
    on_peer_probe(p, path, last + DEAD_MS);
    check("PROMOTED a second time", tick(p, last + DEAD_MS + 1) == EV_PROMOTED);
    check("counted separately", p.promotions == 2 && p.demotions == 1);

    // While direct, only the chosen path is kept warm -- not every loser the pair ever collected.
    Punch q;
    reset(q, 22);
    Cand many[3] = {v4(10, 0, 0, 1, 1), v4(10, 0, 0, 2, 2), v4(10, 0, 0, 3, 3)};
    add_cands(q, many, 3, 0);
    probe_and_echo(q, 0, true);
    on_peer_probe(q, many[0], 0);
    tick(q, 0);
    check("direct on the first", direct_target(q) != nullptr);
    check("one keepalive, not three", probe_and_echo(q, KEEP_MS, false) == 1);
    check("and not before KEEP_MS", probe_and_echo(q, KEEP_MS + 1, false) == 0);
}

// ---- F2: the ONE-WAY path failure ------------------------------------------------------------------

void arm_one_way() {
    printf("-- F2: a path that dies in ONE direction only\n");
    // THE REGRESSION THIS ARM EXISTS FOR, measured on the rig 2026-09-18 before it was fixed. A
    // firewall rule cut the direct path mid-match; the client demoted in 3 s and the HOST never
    // demoted at all, because the client -- correctly back on the relay and correctly still probing
    // the direct path it wanted back -- kept the host's "last datagram from the peer" timer fresh.
    // The host went on sending the whole match into a hole and the run stalled at step 555.
    //
    // The rule is now: only an echo of OUR OWN nonce is liveness. This arm is the host's half of
    // that afternoon -- inbound traffic of every kind, outbound answered by nothing.
    const Cand path = v4(192, 168, 0, 38, 41000);
    Punch      p;
    reset(p, 51);
    promote(p, path, 0);
    check("direct", direct_target(p) != nullptr);

    for (uint32_t t = 250; t <= 30000; t += 250) {
        probe_and_echo(p, t, false); // our keepalives go out; nothing comes back
        on_peer_probe(p, path, t);   // ...while THEIRS keep arriving, every quarter second
        if (t < DEAD_MS) {
            if (tick(p, t) != EV_NONE) check("not before the timer", false);
            continue;
        }
        if (t == DEAD_MS) {
            check("DEMOTED despite a busy inbound half", tick(p, t) == EV_DEMOTED);
            break;
        }
    }
    check("relayed", direct_target(p) == nullptr && p.demotions == 1);
}

// ---- H: force_relay -------------------------------------------------------------------------------

void arm_forced() {
    printf("-- H: [net] force_relay=1\n");
    const Cand path = v4(198, 51, 100, 9, 7000);
    {
        Punch p;
        reset(p, 31);
        set_forced(p, true, 0);
        check("forced", forced(p));
        Cand one = path;
        check("candidates are not even taken", add_cands(p, &one, 1, 0) == 0);
        check("nothing is probed", probe_and_echo(p, 0, true) == 0);
        // Even handed a fully validated path by hand, it must not promote.
        on_peer_probe(p, path, 0);
        bool stayed = true;
        for (uint32_t t = 0; t < 60000; t += 250)
            if (tick(p, t) != EV_NONE) stayed = false;
        check("never promotes", stayed && direct_target(p) == nullptr);
    }
    {
        // Pinning a pair that is ALREADY direct takes effect at once: the knob exists to be believed.
        Punch p;
        reset(p, 32);
        promote(p, path, 500);
        check("direct first", direct_target(p) != nullptr);
        set_forced(p, true, 600);
        check("relayed the moment it is set", direct_target(p) == nullptr);
        check("and the drop is counted as a demotion", p.demotions == 1);
    }
}

// ---- I: the 32-bit tick wrap ----------------------------------------------------------------------

void arm_wrap() {
    printf("-- I: the GetTickCount wrap\n");
    // 49.7 days after a machine booted, GetTickCount() goes from 0xFFFFFFFF to 0. A comparison of
    // two stamps is wrong across that boundary and an unsigned subtraction is right; every elapsed
    // test in udp_punch.cpp is a subtraction, and this arm is what says so.
    const uint32_t before = 0xFFFFF000u;
    const Cand     path   = v4(198, 51, 100, 22, 8000);
    Punch          p;
    reset(p, 41);
    promote(p, path, before);
    check("direct just before the wrap", direct_target(p) != nullptr);

    uint32_t t = before;
    for (int i = 0; i < 40; ++i) { // 20 s of answered keepalives, straight across 0
        t += 500;
        probe_and_echo(p, t, true);
        if (tick(p, t) != EV_NONE) check("survived the wrap without a spurious demotion", false);
    }
    check("past the wrap and still direct", t < before && direct_target(p) != nullptr);
    check("no demotion at all", p.demotions == 0);

    // And the timer still FIRES across it, which a clamped-to-zero subtraction would have broken
    // in the other direction. `t` is past the wrap, so the last echo is at a stamp NUMERICALLY
    // larger than `now` -- which is the whole point of the arm.
    check("silence across the wrap still demotes", tick(p, t + DEAD_MS) == EV_DEMOTED);
}

} // namespace

int run_udppunchtest() {
    printf("=== udppunchtest (mp:R3: the hole-punch promotion state machine, no sockets) ===\n");
    arm_codec();
    arm_promotion();
    arm_cadence();
    arm_unadvertised();
    arm_demotion();
    arm_one_way();
    arm_forced();
    arm_wrap();
    printf("=== udppunchtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
