//
// world_sync_selftest.cpp -- `net_selftest.exe wstest`: mp:X3c. Every decision of the world-resync
// core (mh/desync/world_sync_core.h) and the wire it rides (desync_wire2.h types 6-9), with no game, no rig
// and no socket. Each gate and each rule has a positive AND a negative arm: a gate that is always shut and
// one that is always open each pass half of the file.
//
#include <stdio.h>
#include <string.h>

#include "desync/desync_wire2.h"
#include "desync/world_sync_core.h"
#include "../mh_net_udp/udp_stats.h"

using namespace mh::desync;
using namespace mh::desync::ws;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

#include <limits>
const double       NAN_D = std::numeric_limits<double>::quiet_NaN();
constexpr uint64_t FP    = 0x1122334455667788ULL;

// ---- A. the wire ---------------------------------------------------------------------------------------
void arm_wire() {
    using namespace v2;
    uint8_t b[MAX_FRAME];
    hdr     h;

    check("ws: BEGIN is 28 bytes", ws_size(T_WS_BEGIN) == 28);
    check("ws: META is 80 bytes", ws_size(T_WS_META) == 80);
    check("ws: DONE is 36 bytes", ws_size(T_WS_DONE) == 36);
    check("ws: ABORT is 28 bytes", ws_size(T_WS_ABORT) == 28);
    check("ws: frame_size agrees for count 1", frame_size(T_WS_META, 1, 63) == 80 && frame_size(T_WS_ABORT, 1, 63) == 28);
    check("ws: a count other than 1 has no size", frame_size(T_WS_BEGIN, 0, 63) < 0 && frame_size(T_WS_BEGIN, 2, 63) < 0);
    check("ws: type 10 is still unknown", frame_size(10, 1, 63) < 0 && ws_size(10) < 0);

    // BEGIN round trip
    int           n = ws_put_begin(b, FP, 4242, 3, 0xA5);
    ws_begin_body bb;
    check("ws: BEGIN round-trips", n == 28 && ws_get_begin(b, n, FP, h, bb) && h.step == 4242 && h.aux == 3 && bb.flags == 0xA5);
    check("ws: BEGIN one byte short refused", !ws_get_begin(b, n - 1, FP, h, bb));
    check("ws: BEGIN one byte long refused", [&] {
        uint8_t c[MAX_FRAME];
        memcpy(c, b, n);
        c[n] = 0;
        return !ws_get_begin(c, n + 1, FP, h, bb);
    }());
    check("ws: BEGIN with a foreign fingerprint refused", !ws_get_begin(b, n, FP ^ 1, h, bb));
    check("ws: BEGIN decoded as META refused (type checked)", [&] {
        ws_meta_body m;
        return !ws_get_meta(b, n, FP, h, m);
    }());
    check("ws: truncated inside the header refused", !ws_get_begin(b, 10, FP, h, bb) && !ws_get_begin(b, 0, FP, h, bb) &&
                                                         !ws_get_begin(nullptr, 28, FP, h, bb));

    // META round trip, every field
    ws_meta_body m = {};
    m.digest_prev  = 0xDEADBEEFCAFEF00DULL;
    for (int i = 0; i < WS_SRC_SLOTS; ++i) m.n_src[i] = 100u + (uint32_t)i;
    m.blob_len = 8123456;
    for (int i = 0; i < WS_ROOT_BYTES; ++i) m.root[i] = (uint8_t)(0xC0 + i);
    n = ws_put_meta(b, FP, 9001, 1, m);
    ws_meta_body m2;
    bool         ok = n == 80 && ws_get_meta(b, n, FP, h, m2) && h.step == 9001 && h.aux == 1 && memcmp(&m, &m2, sizeof(m)) == 0;
    check("ws: META round-trips every field (digest, 9 counters, length, root)", ok);
    check("ws: META short by one refused", !ws_get_meta(b, n - 1, FP, h, m2));
    check("ws: META with count 0 refused", [&] {
        uint8_t c[MAX_FRAME];
        memcpy(c, b, n);
        c[7] = 0; // hdr.count
        return !ws_get_meta(c, n, FP, h, m2);
    }());
    check("ws: META with a bad magic refused", [&] {
        uint8_t c[MAX_FRAME];
        memcpy(c, b, n);
        c[0] ^= 0xFF;
        return !ws_get_meta(c, n, FP, h, m2);
    }());
    check("ws: META with a v1 version word refused", [&] {
        uint8_t c[MAX_FRAME];
        memcpy(c, b, n);
        c[4] = 1;
        return !ws_get_meta(c, n, FP, h, m2);
    }());

    // DONE (negative rc survives) and ABORT
    ws_done_body d = {7777, -30, 15321};
    n              = ws_put_done(b, FP, 9001, 0, d);
    ws_done_body d2;
    check("ws: DONE round-trips incl. a negative rc", n == 36 && ws_get_done(b, n, FP, h, d2) && d2.live_step == 7777 &&
                                                          d2.rc == -30 && d2.ff_ms == 15321 && h.aux == 0);
    n = ws_put_abort(b, FP, 5, 2, WS_ABORT_ROSTER);
    ws_abort_body a;
    check("ws: ABORT round-trips its reason", n == 28 && ws_get_abort(b, n, FP, h, a) && a.reason == WS_ABORT_ROSTER && h.aux == 2);

    // frame_ok (the shared gate the receive loop uses)
    n = ws_put_abort(b, FP, 5, 7, 1);
    check("ws: frame_ok accepts dst 7", frame_ok(b, n, 63, h));
    n = ws_put_abort(b, FP, 5, 8, 1);
    check("ws: frame_ok refuses dst 8 (aux out of range)", !frame_ok(b, n, 63, h));
    put_hdr(b, T_WS_ABORT, 1, FP, 5, 1 /*region*/, 0);
    check("ws: frame_ok refuses a control frame carrying a region", !frame_ok(b, 28, 63, h));
    check("ws: every reason value is below WS_ABORT_COUNT", WS_ABORT_ROSTER < WS_ABORT_COUNT && WS_ABORT_RING_SHORT < WS_ABORT_COUNT);
}

// ---- B. who diverged -------------------------------------------------------------------------------------
void arm_verdict() {
    uint64_t        s[8];
    diverge_verdict v;

    // 2 peers, host = 0
    s[0] = 0xAA;
    s[1] = 0xBB;
    v    = pick_diverged(s, 0x3, 0, 2);
    check("who: 2 peers differ -> resync the CLIENT (peer 1) only", v.kind == V_RESYNC && v.resync_mask == 0x2);
    s[1] = 0xAA;
    v    = pick_diverged(s, 0x3, 0, 2);
    check("who: 2 peers agree -> AGREE, empty mask", v.kind == V_AGREE && v.resync_mask == 0);
    // 2 peers, host = 1 (the host is not always slot 0)
    s[0] = 0xAA;
    s[1] = 0xBB;
    v    = pick_diverged(s, 0x3, 1, 2);
    check("who: host in slot 1 -> the other slot is the client", v.kind == V_RESYNC && v.resync_mask == 0x1);

    // 3 peers: host + one agree, one out
    s[0] = 1;
    s[1] = 1;
    s[2] = 2;
    v    = pick_diverged(s, 0x7, 0, 3);
    check("who: 3 peers, host in the majority -> resync only the outlier", v.kind == V_RESYNC && v.resync_mask == 0x4 && v.groups == 2);
    // 3 peers: host is the odd one out
    s[0] = 2;
    s[1] = 1;
    s[2] = 1;
    v    = pick_diverged(s, 0x7, 0, 3);
    check("who: 3 peers, host is the minority -> HOST_MINORITY, empty mask (SKIP, row X3g)",
          v.kind == V_HOST_MINORITY && v.resync_mask == 0 && v.host_group == 1 && v.best_group == 2);
    // 4 peers 2-2: ties go to the host's group
    s[0] = 1;
    s[1] = 1;
    s[2] = 2;
    s[3] = 2;
    v    = pick_diverged(s, 0xF, 0, 4);
    check("who: a 2-2 tie favours the host's group -> resync the other two", v.kind == V_RESYNC && v.resync_mask == 0xC);
    // 4 peers 1-3 with host in the 3
    s[0] = 5;
    s[1] = 5;
    s[2] = 5;
    s[3] = 9;
    v    = pick_diverged(s, 0xF, 2, 4);
    check("who: 4 peers, only the outlier is resynced", v.kind == V_RESYNC && v.resync_mask == 0x8);
    // 3 distinct hashes, host alone: group sizes 1,1,1 -> the host's group ties the best
    s[0] = 1;
    s[1] = 2;
    s[2] = 3;
    v    = pick_diverged(s, 0x7, 0, 3);
    check("who: three distinct hashes -> host group ties best -> both others resynced", v.kind == V_RESYNC && v.resync_mask == 0x6 && v.groups == 3);
    // a peer with no hash takes no part
    s[0] = 1;
    s[1] = 2;
    s[2] = 2;
    v    = pick_diverged(s, 0x3, 0, 3); // peer 2 has no hash
    check("who: a peer without a hash is neither counted nor resynced", v.kind == V_RESYNC && v.resync_mask == 0x2);
    // host hash missing / lone reporter
    v = pick_diverged(s, 0x6, 0, 3);
    check("who: the host's own hash missing -> NO_DATA", v.kind == V_NO_DATA);
    v = pick_diverged(s, 0x1, 0, 3);
    check("who: a lone reporter -> NO_DATA", v.kind == V_NO_DATA);
    v = pick_diverged(s, 0x7, 5, 3);
    check("who: host index out of range -> NO_DATA", v.kind == V_NO_DATA);
}

// ---- C. the roster tracker (D26) ----------------------------------------------------------------------
void arm_roster() {
    uint8_t flags[8] = {0x03, 0x03, 0x02, 0, 0, 0, 0, 0};
    check("roster: word is one ALIVE bit per slot", roster_word(flags) == 0x7);
    flags[1] = 0x01; // HUMAN only, not alive
    check("roster: a non-alive slot is 0 (0x01 is not the ALIVE bit)", roster_word(flags) == 0x5);

    roster_tracker r;
    check("roster: a fresh tracker is quiet", r.quiet(10, 250));
    check("roster: first observation is not a change", !r.observe(1, 0x7));
    check("roster: same word is not a change", !r.observe(2, 0x7));
    check("roster: never changed -> quiet at any step", r.quiet(100000, 250));
    check("roster: a different word is a change", r.observe(1000, 0x3));
    check("roster: quiet is FALSE inside the window (D26 arm: match end)", !r.quiet(1100, 250) && !r.quiet(1249, 250));
    check("roster: quiet is TRUE exactly at the window edge", r.quiet(1250, 250) && r.quiet(5000, 250));
    check("roster: a later change restarts the window", r.observe(3000, 0x1) && !r.quiet(3100, 250) && r.quiet(3250, 250));
}

// ---- D. the gates ---------------------------------------------------------------------------------------
gate_input good() {
    gate_input g{};
    g.is_host          = true;
    g.libmh_ok         = true;
    g.transport_ok     = true;
    g.session_mode     = 3;
    g.peer_human_alive = true;
    g.peer_active      = true;
    g.consecutive      = 50;
    g.step             = 5000;
    g.state            = P_IDLE;
    g.resyncs_done     = 0;
    return g;
}

void arm_gates() {
    config c;
    c.action = 1;
    check("gate: defaults are the design's", config().action == 0 && config().confirm_steps == 50 && config().roster_quiet_steps == 250 &&
                                                 config().resync_max == 3 && config().timeout_ms == 180000 && config().ff_steps == 20 &&
                                                 config().cooldown_steps == 500);
    gate_input g = good();
    check("gate: everything satisfied -> NONE", gate_check(c, g) == SKIP_NONE);

    config off = c;
    off.action = 0;
    check("gate: action=0 (the default) -> report-only", gate_check(off, g) == SKIP_ACTION_OFF);

    g         = good();
    g.is_host = false;
    check("gate: a client never acts", gate_check(c, g) == SKIP_NOT_HOST);
    g          = good();
    g.libmh_ok = false;
    check("gate: libmh / promote not live -> SKIP", gate_check(c, g) == SKIP_NO_LIBMH);
    g              = good();
    g.transport_ok = false;
    check("gate: TCP (snapshot unsupported) -> SKIP", gate_check(c, g) == SKIP_TRANSPORT);
    g              = good();
    g.session_mode = 2;
    check("gate: SESSION_MODE != 3 -> SKIP", gate_check(c, g) == SKIP_MODE);
    g                  = good();
    g.peer_human_alive = false;
    check("gate: peer not ALIVE|HUMAN -> SKIP", gate_check(c, g) == SKIP_PEER);
    g             = good();
    g.peer_active = false;
    check("gate: peer not in the active list -> SKIP", gate_check(c, g) == SKIP_PEER);

    g             = good();
    g.consecutive = 49;
    check("gate: 49 consecutive steps is a blip -> CONFIRM", gate_check(c, g) == SKIP_CONFIRM);
    g             = good();
    g.consecutive = 50;
    check("gate: 50 consecutive steps passes", gate_check(c, g) == SKIP_NONE);
    config c10        = c;
    c10.confirm_steps = 10;
    g                 = good();
    g.consecutive     = 10;
    check("gate: confirm_steps is honoured (10)", gate_check(c10, g) == SKIP_NONE);

    g = good();
    g.roster.observe(1, 0x7);
    g.roster.observe(4900, 0x3); // changed 100 steps ago
    check("gate: roster changed 100 steps ago -> ROSTER (D26)", gate_check(c, g) == SKIP_ROSTER);
    g.step = 5150;
    check("gate: roster changed exactly 250 steps ago passes", gate_check(c, g) == SKIP_NONE);

    g       = good();
    g.state = P_BEGUN;
    check("gate: peer BEGUN -> BUSY", gate_check(c, g) == SKIP_BUSY);
    g.state = P_SENT;
    check("gate: peer SENT -> BUSY", gate_check(c, g) == SKIP_BUSY);
    g.state = P_COOLDOWN;
    check("gate: peer COOLDOWN -> BUSY", gate_check(c, g) == SKIP_BUSY);

    g              = good();
    g.resyncs_done = 2;
    check("gate: 2 of 3 resyncs used -> allowed", gate_check(c, g) == SKIP_NONE);
    g.resyncs_done = 3;
    check("gate: resync_max reached -> MAX", gate_check(c, g) == SKIP_MAX);

    // precedence: the fundamental failure is the one named
    g              = good();
    g.session_mode = 2;
    g.consecutive  = 0;
    check("gate: mode is named before confirm when both fail", gate_check(c, g) == SKIP_MODE);
    check("gate: every skip has a name", skip_name(SKIP_ROSTER)[0] != '?' && skip_name(SKIP_HOST_MINORITY)[0] != '?' &&
                                             skip_name((skip_reason)99)[0] == '?');
}

// ---- E. the state machine -------------------------------------------------------------------------------
event ev(event_kind k, uint32_t a = 0) {
    event e;
    e.kind = k;
    e.a    = a;
    return e;
}
event step_ev(int ms, uint32_t roster, int mode = 3, bool active = true) {
    event e        = ev(E_STEP);
    e.elapsed_ms   = ms;
    e.roster       = roster;
    e.session_mode = mode;
    e.peer_active  = active;
    return e;
}

void arm_fsm() {
    config c;
    c.cooldown_steps    = 5; // small, so the arm can walk it
    c.resync_max        = 2;
    c.capture_retry_max = 3;
    fsm    f;
    action a;

    a = fsm_step(c, f, ev(E_CAPTURE_OK, 10));
    check("fsm: CAPTURE_OK while IDLE is ignored", a.kind == A_NONE && f.state == P_IDLE);
    a = fsm_step(c, f, ev(E_DONE));
    check("fsm: a stray DONE while IDLE is ignored", a.kind == A_NONE && f.state == P_IDLE && f.resyncs == 0);

    // happy path
    event inc  = ev(E_INCIDENT, 4000);
    inc.roster = 0x7;
    a          = fsm_step(c, f, inc);
    check("fsm: INCIDENT from IDLE -> BEGUN, send BEGIN", a.kind == A_SEND_BEGIN && f.state == P_BEGUN && f.resyncs == 1 && f.begin_step == 4000);
    a = fsm_step(c, f, inc);
    check("fsm: INCIDENT while BEGUN is ignored", a.kind == A_NONE && f.resyncs == 1);
    a = fsm_step(c, f, ev(E_DONE));
    check("fsm: DONE before the capture is ignored", a.kind == A_NONE && f.state == P_BEGUN);
    a = fsm_step(c, f, ev(E_CAPTURE_OK, 4003));
    check("fsm: CAPTURE_OK -> SENT, S recorded", a.kind == A_NONE && f.state == P_SENT && f.capture_step == 4003);
    a = fsm_step(c, f, step_ev(1000, 0x7));
    check("fsm: quiet steps while SENT change nothing", a.kind == A_NONE && f.state == P_SENT && f.elapsed_ms == 1000);
    a = fsm_step(c, f, ev(E_DONE, 4200));
    check("fsm: DONE -> COOLDOWN", a.kind == A_LOG_DONE && f.state == P_COOLDOWN && f.cooldown_left == 5);
    for (int i = 0; i < 4; ++i) a = fsm_step(c, f, step_ev(0, 0x7));
    check("fsm: still COOLDOWN one step before the end", f.state == P_COOLDOWN && a.kind == A_NONE);
    a = fsm_step(c, f, step_ev(0, 0x7));
    check("fsm: COOLDOWN ends after cooldown_steps -> IDLE", a.kind == A_COOLDOWN_END && f.state == P_IDLE);

    // second resync then the max
    a = fsm_step(c, f, inc);
    check("fsm: a second resync is allowed (max 2)", a.kind == A_SEND_BEGIN && f.resyncs == 2);
    fsm_step(c, f, ev(E_CAPTURE_OK, 5000));
    fsm_step(c, f, ev(E_DONE, 5100));
    for (int i = 0; i < 5; ++i) fsm_step(c, f, step_ev(0, 0x7));
    a = fsm_step(c, f, inc);
    check("fsm: a third INCIDENT is refused by resync_max", a.kind == A_NONE && f.state == P_IDLE && f.resyncs == 2);

    // ABORT cases (fresh machine each)
    auto begun = [&](fsm &m) {
        m = fsm();
        fsm_step(c, m, inc);
    };
    fsm m;
    begun(m);
    a = fsm_step(c, m, step_ev(10, 0x3));
    check("fsm: roster change after BEGIN -> ABORT(ROSTER), COOLDOWN", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_ROSTER && m.state == P_COOLDOWN);
    begun(m);
    fsm_step(c, m, ev(E_CAPTURE_OK, 4003));
    a = fsm_step(c, m, step_ev(10, 0x3));
    check("fsm: roster change while SENT also aborts", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_ROSTER);
    begun(m);
    a = fsm_step(c, m, step_ev(10, 0x7, /*mode*/ 2));
    check("fsm: SESSION_MODE leaving 3 (R4 local gameover) -> ABORT(GAMEOVER)", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_GAMEOVER);
    begun(m);
    a = fsm_step(c, m, step_ev(10, 0x7, 3, /*active*/ false));
    check("fsm: peer no longer active -> ABORT(PEER_GONE)", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_PEER_GONE);
    begun(m);
    a = fsm_step(c, m, step_ev(c.timeout_ms - 1, 0x7));
    check("fsm: one ms before the timeout -> no abort", a.kind == A_NONE && m.state == P_BEGUN);
    a = fsm_step(c, m, step_ev(c.timeout_ms, 0x7));
    check("fsm: timeout -> ABORT(TIMEOUT)", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_TIMEOUT && m.state == P_COOLDOWN);
    begun(m);
    a = fsm_step(c, m, ev(E_PENDING_OVERFLOW));
    check("fsm: PENDING overflow while BEGUN -> ABORT(PENDING_OVFL) (R5)", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_PENDING_OVFL);
    fsm idle;
    a = fsm_step(c, idle, ev(E_PENDING_OVERFLOW));
    check("fsm: PENDING overflow while IDLE is not an abort", a.kind == A_NONE && idle.state == P_IDLE);
    begun(m);
    check("fsm: capture failure 1 -> retry", fsm_step(c, m, ev(E_CAPTURE_FAIL)).kind == A_RETRY_CAPTURE && m.state == P_BEGUN);
    check("fsm: capture failure 2 -> retry", fsm_step(c, m, ev(E_CAPTURE_FAIL)).kind == A_RETRY_CAPTURE);
    a = fsm_step(c, m, ev(E_CAPTURE_FAIL));
    check("fsm: capture failure 3 (budget) -> ABORT(CAPTURE)", a.kind == A_SEND_ABORT && a.reason == WS_ABORT_CAPTURE && m.state == P_COOLDOWN);
    check("fsm: an abort in COOLDOWN is not repeated", fsm_step(c, m, step_ev(c.timeout_ms, 0x1)).kind == A_NONE);
    check("fsm: roster precedence over timeout", [&] {
        fsm x;
        fsm_step(c, x, inc);
        return fsm_step(c, x, step_ev(c.timeout_ms + 5, 0x1)).reason == WS_ABORT_ROSTER;
    }());
}

// ---- F. the minority side ------------------------------------------------------------------------------
void arm_minority() {
    uint32_t have[9]   = {10, 20, 0, 0, 0, 0, 0, 0, 3};
    uint32_t oldest[9] = {1, 5, 0, 0, 0, 0, 0, 0, 1};
    uint32_t nsrc[9]   = {4, 20, 0, 0, 0, 0, 0, 0, 3};
    check("ring: covered (oldest <= n_src+1, or nothing to replay)", ring_covers(have, oldest, nsrc) == 0);
    oldest[0] = 5; // holds 5.., needs 5 (n_src 4 + 1): still exactly covered
    check("ring: oldest == n_src+1 is covered", ring_covers(have, oldest, nsrc) == 0);
    oldest[0] = 6;
    check("ring: oldest > n_src+1 -> -30 (ring short)", ring_covers(have, oldest, nsrc) == RC_RING_SHORT);
    oldest[0] = 5;
    nsrc[1]   = 10; // source 1 holds 5.., needs 11: covered
    check("ring: a later n_src needs less", ring_covers(have, oldest, nsrc) == 0);
    nsrc[1] = 2; // needs 3, ring starts at 5
    check("ring: a lower n_src than the ring reaches -> -30", ring_covers(have, oldest, nsrc) == RC_RING_SHORT);
    nsrc[1]   = 20;
    oldest[1] = 0;
    have[1]   = 20; // nothing to replay for source 1 -> ring emptiness irrelevant
    check("ring: nothing to replay tolerates an empty ring", ring_covers(have, oldest, nsrc) == 0);
    nsrc[1] = 5; // something to replay but nothing held
    check("ring: something to replay with an empty ring -> -30", ring_covers(have, oldest, nsrc) == RC_RING_SHORT);

    import_input i{};
    i.blob_ready = true;
    i.meta_seen  = true;
    i.blob_step  = 900;
    i.meta_step  = 900;
    i.blob_len   = 100;
    i.meta_len   = 100;
    i.root_match = true;
    i.ring_rc    = 0;
    check("import: everything agrees -> GO", import_check(i) == IMP_GO);
    import_input j = i;
    j.blob_ready   = false;
    check("import: blob not ready -> WAIT", import_check(j) == IMP_WAIT);
    j           = i;
    j.meta_seen = false;
    check("import: META not seen -> WAIT", import_check(j) == IMP_WAIT);
    j           = i;
    j.blob_step = 901;
    check("import: blob step != META.S -> BAD_STEP", import_check(j) == IMP_BAD_STEP);
    j          = i;
    j.meta_len = 99;
    check("import: length mismatch -> BAD_LEN", import_check(j) == IMP_BAD_LEN);
    j            = i;
    j.root_match = false;
    check("import: root mismatch -> BAD_ROOT", import_check(j) == IMP_BAD_ROOT);
    j         = i;
    j.ring_rc = RC_RING_SHORT;
    check("import: short ring -> RING_SHORT (DONE rc -30)", import_check(j) == IMP_RING_SHORT);
    j            = i;
    j.blob_ready = false;
    j.blob_step  = 1;
    check("import: WAIT wins over a not-yet-comparable step", import_check(j) == IMP_WAIT);

    check("minority: mode 3 and no abort -> continue", !minority_must_abort(3, false));
    check("minority: ABORT received -> stop", minority_must_abort(3, true));
    check("minority: local gameover (mode 2, R4) -> stop", minority_must_abort(2, false));
}

// ---- G. fast-forward arithmetic ------------------------------------------------------------------------
void arm_ff() {
    const double sub = 0.02; // 50 steps/s
    check("ff: cap is clock + ff_steps*sub", ff_cap(10.0, 20, sub) > 10.39 && ff_cap(10.0, 20, sub) < 10.41);
    check("ff: TOTAL is capped when the backlog is large", ff_total(30.0, 10.0, 20, sub) == ff_cap(10.0, 20, sub));
    check("ff: TOTAL never exceeds the live target", ff_total(10.1, 10.0, 20, sub) == 10.1);
    check("ff: the live target advances by dt*speed", ff_live_advance(30.0, 0.1, 2.0, 0.0) > 30.19 && ff_live_advance(30.0, 0.1, 2.0, 0.0) < 30.21);
    check("ff: the live target is clamped to committed", ff_live_advance(30.0, 5.0, 1.0, 31.0) == 31.0);
    check("ff: the live target never moves backwards", ff_live_advance(30.0, -1.0, 1.0, 0.0) == 30.0 &&
                                                           ff_live_advance(30.0, 0.1, 1.0, 20.0) == 30.0);
    check("ff: NOT done while the backlog exceeds two caps", !ff_done(30.0, 10.0, 20, sub));
    check("ff: done when the backlog is within two caps", ff_done(10.7, 10.0, 20, sub));
    check("ff: done exactly at two caps", ff_done(10.0 + 2.0 * 20 * sub, 10.0, 20, sub));
    check("ff: a 1000-step backlog needs (1000-40)/20 = 48 capped frames",
          [&] {
              double clock = 0.0, live = 1000 * sub;
              int    frames = 0;
              while (!ff_done(live, clock, 20, sub) && frames < 1000) {
                  clock = ff_total(live, clock, 20, sub);
                  ++frames;
              }
              return frames == 48;
          }());
}

// ---- H. mirror_horizon ---------------------------------------------------------------------------------
void arm_mirror() {
    using mh::netstats::mirror_horizon;
    double p[4] = {50.0, 40.0, 60.0, 0.0};
    check("mirror: never below what was already sent", mirror_horizon(100.0, p, 0xF, 4, 1, 10.0) == 100.0);
    check("mirror: the max of the OTHER peers when larger", mirror_horizon(10.0, p, 0xF, 4, 1, 5.0) == 60.0);
    check("mirror: ignores its own slot (G276)", mirror_horizon(10.0, p, 0xF, 4, 2, 5.0) == 50.0);
    check("mirror: ignores peers outside the active mask", mirror_horizon(10.0, p, 0x3, 4, 1, 5.0) == 50.0);
    check("mirror: the local HORIZON counts", mirror_horizon(10.0, p, 0x0, 4, 1, 77.0) == 77.0);
    p[0] = NAN_D;
    check("mirror: NaN peer entries are ignored", mirror_horizon(10.0, p, 0xF, 4, 1, 5.0) == 60.0);
    double q[2] = {-5.0, 0.0};
    check("mirror: non-positive entries are ignored", mirror_horizon(12.0, q, 0x3, 2, 5, 0.0) == 12.0);
    check("mirror: a NaN or negative sent starts from zero", mirror_horizon(NAN_D, q, 0x0, 2, 0, 3.0) == 3.0 &&
                                                                 mirror_horizon(-1.0, q, 0x0, 2, 0, 0.0) == 0.0);
    // monotone under a clock that drops by a backlog: successive calls never decrease
    double sent = 100.0, last = 0.0;
    bool   mono  = true;
    double hz[3] = {0, 99.0, 101.0};
    for (int k = 0; k < 20; ++k) {
        hz[1]          = 90.0 + (k * 7 % 13);                           // a wobbling peer horizon, sometimes below sent
        const double h = mirror_horizon(sent, hz, 0x6, 3, 0, 20.0 - k); // a local horizon far behind
        if (h < last || h < sent) mono = false;
        last = sent = h;
    }
    check("mirror: 20 wobbling calls never move the advertised horizon backwards", mono);
}

// ---- X3c-FIX: the game-side horizon floor + the client map-drain disposition ---------------------------
void arm_x3c_fix() {
    using mh::netstats::client_poll_disposition;
    using mh::netstats::mirror_extend_floor;
    check("floor: a writer below the mirror floor is raised to it (5300 -> 23660)", mirror_extend_floor(5.3, 23.66) == 23.66);
    check("floor: a writer above the floor is untouched", mirror_extend_floor(30.0, 23.66) == 30.0);
    check("floor: a NaN writer value yields the floor", mirror_extend_floor(NAN_D, 23.66) == 23.66);
    check("floor: a NaN floor leaves the writer alone", mirror_extend_floor(5.3, NAN_D) == 5.3);
    // a clock rewound by the backlog: 20 advertise_horizon-style writes (clock + 30 ms) never go under the floor
    double clk = 5.0, floor_s = 23.66, last = 0.0;
    bool   mono = true;
    for (int k = 0; k < 20; ++k) {
        clk += 0.05;
        const double w = mirror_extend_floor(clk + 0.03, floor_s);
        if (w < floor_s || w < last) mono = false;
        last = w;
    }
    check("floor: 20 catch-up writes from a rewound clock never advertise below the mirror floor", mono);
    // the disposition: the map wins, in-game 1..5 is parked, everything else is discarded
    check("poll: the map packet is applied", client_poll_disposition(0x21, 400, 0x21, 300) == 0);
    check("poll: a short 0x21 is not the map", client_poll_disposition(0x21, 10, 0x21, 300) == 2);
    bool parked = true;
    for (int t = 1; t <= 5; ++t) parked = parked && client_poll_disposition((unsigned char)t, 9, 0x21, 300) == 1;
    check("poll: in-game types 1..5 (horizon/order/META...) are parked, never discarded", parked);
    check("poll: type 0 and 6..0x20 lobby datagrams are discarded",
          client_poll_disposition(0, 9, 0x21, 300) == 2 && client_poll_disposition(6, 9, 0x21, 300) == 2 &&
              client_poll_disposition(0x0e, 5, 0x21, 300) == 2);
    check("poll: an empty datagram is discarded", client_poll_disposition(2, 0, 0x21, 300) == 2);
}

// ---- H. the wide roster word (risk R2) ----------------------------------------------------------
// The wiring folds ALIVE|HUMAN|GONE|DEFEATED, because a clean quit runs mark_player_gone and leaves ALIVE alone.
void arm_wide_roster() {
    uint8_t        f[8] = {0x07, 0x07, 0, 0, 0, 0, 0, 0}; // two ALIVE|HUMAN slots
    const uint32_t base = roster_word_wide(f);
    check("wide: two live humans fold to 0x33", base == 0x33);
    check("wide: the ALIVE-only word sees the same pair", roster_word(f) == 0x3);

    // mark_player_gone on slot 1: HUMAN cleared, GONE and DEFEATED set, ALIVE untouched.
    uint8_t g[8];
    memcpy(g, f, 8);
    g[1] = (uint8_t)((g[1] & ~0x04u) | 0x08u | 0x10u);
    check("wide: mark_player_gone (ALIVE untouched) MOVES the wide word", roster_word_wide(g) != base);
    check("wide: ...and the ALIVE-only word would NOT have moved (why the wide one exists)",
          roster_word(g) == roster_word(f));

    // bits above DEFEATED and the low bit of the flags byte are not roster state
    uint8_t h[8];
    memcpy(h, f, 8);
    h[0] |= 0x01u | 0x20u | 0x80u;
    check("wide: flag bits outside ALIVE|HUMAN|GONE|DEFEATED do not move the word", roster_word_wide(h) == base);

    // the four bits are per slot: slot 7 lands in the top nibble
    uint8_t t[8] = {};
    t[7]         = 0x1E;
    check("wide: slot 7 occupies the top nibble", roster_word_wide(t) == 0xF0000000u);

    // through the tracker: a quiet window opens on the change
    roster_tracker r;
    r.observe(10, roster_word_wide(f));
    check("wide: tracker sees a quit as a change", r.observe(2000, roster_word_wide(g)) && !r.quiet(2100, 250));
}

// ---- I. the hash history the host groups N peers from -----------------------------------------------------
void arm_hash_hist() {
    hash_hist h;
    h.clear();
    uint64_t v = 0;
    check("hist: empty reads as unknown", !h.get(1, 100, &v));
    h.put(1, 100, 0xAAAA);
    h.put(8, 100, 0xBBBB);
    check("hist: a sender's value round-trips", h.get(1, 100, &v) && v == 0xAAAA);
    check("hist: the host's own slot (8) is independent", h.get(8, 100, &v) && v == 0xBBBB);
    check("hist: another sender at the same step is unknown", !h.get(2, 100, &v));
    check("hist: another step of the same sender is unknown", !h.get(1, 101, &v));

    // eviction: step 100 + 128 shares the slot; the older step must read UNKNOWN, not the newer value
    h.put(1, 100 + hash_hist::N, 0xCCCC);
    check("hist: an evicted step reads as unknown (never as the newer value)", !h.get(1, 100, &v));
    check("hist: the newer step is there", h.get(1, 100 + hash_hist::N, &v) && v == 0xCCCC);

    // out-of-range senders / step 0 are ignored, not written
    h.put(9, 5, 1);
    h.put(-1, 5, 1);
    h.put(2, 0, 1);
    check("hist: out-of-range sender and step 0 are refused", !h.get(9, 5, &v) && !h.get(-1, 5, &v) && !h.get(2, 0, &v));

    // it feeds pick_diverged: 3 peers, the host (slot 0) with peer 1 against peer 2
    hash_hist g;
    g.clear();
    g.put(8, 500, 0x11); // host's own
    g.put(1, 500, 0x11);
    g.put(2, 500, 0x22);
    uint64_t st[8] = {};
    uint32_t have  = 0;
    if (g.get(8, 500, &st[0])) have |= 1u;
    for (int s = 1; s < 3; ++s)
        if (g.get(s, 500, &st[s])) have |= 1u << s;
    const diverge_verdict dv = pick_diverged(st, have, 0, 8);
    check("hist: 3 peers, the host in the majority -> resync peer 2 only",
          dv.kind == V_RESYNC && dv.resync_mask == 0x4 && dv.groups == 2 && dv.host_group == 2 && dv.best_group == 2);
    // a peer whose hash was never seen at that step takes no part
    have &= ~(1u << 2);
    check("hist: a peer with no hash at the incident step is not resynced",
          pick_diverged(st, have, 0, 8).kind == V_AGREE);
}

} // namespace

int run_wstest() {
    arm_wire();
    arm_verdict();
    arm_roster();
    arm_gates();
    arm_fsm();
    arm_minority();
    arm_ff();
    arm_mirror();
    arm_x3c_fix();
    arm_wide_roster();
    arm_hash_hist();
    printf("=== wstest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
