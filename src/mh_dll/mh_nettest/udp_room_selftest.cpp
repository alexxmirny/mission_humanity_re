//
// udp_room_selftest.cpp -- `net_selftest.exe udproomtest` (tracker mp:R6).
//
// THE HOST'S ROOM MINTER, WITH NO RELAY AND NO RNG. mp:R6's acceptance clauses are about two hosts
// on one relay at one moment, and a host relaunching inside the relay's idle window after a crash.
// The rig cannot stage the first at all (one box, one exclusive UDP port per lane, so a second host
// lane cannot bind) and can only stage the second by killing a process and reading a log. Both
// clauses reduce to properties of one small decision function -- src/mh_dll/mh_net_udp/udp_room.h,
// which draws from an INJECTED RNG -- and this suite drives it with RNGs that say exactly what the
// rig could only hope to observe:
//
//   A  a minted room is a HOST room: never 0 (the directory room), never above the 30 bits the
//      browser's sender int routes, and the whole 30-bit range is reachable (the high bit of the
//      draw is masked, not the value clamped).
//   B  two hosts mint DISTINCT rooms from distinct randomness -- the two-hosts clause, in the form
//      the rig cannot stage. And a mint that draws 0, or the room it was told to avoid, REDRAWS
//      rather than returning it.
//   C  room_busy is answered by a fresh room that is not the busy one, re-minted at most
//      BUSY_RETRIES times; the (BUSY_RETRIES+1)-th refusal is left standing with the room
//      untouched -- the mutation check on the bound (a `>` for the `>=` passes every other arm).
//   D  the crash-relaunch clause: a relaunched process has NO memory of its stale room, and still
//      gets a different one from a working RNG with probability 1 - 2^-30; the one deterministic
//      thing the item promises is that a caller who DOES know the stale room never gets it back
//      (`avoid`), even from an RNG that offers it first.
//   E  the RNG failing is a mint of 0 -- the value the transport refuses to host on -- both at
//      init and mid-retry (where the room is left as it was, so the refusal names a real room).
//
#include <stdio.h>
#include <string.h>

#include "../mh_net_udp/udp_room.h"

using namespace mh::udproom;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// A scripted RNG: hands out its 32-bit words in order, then fails. Little-endian, the way mint()
// reads the four bytes, so a script value IS the draw (before the 30-bit mask).
struct Script {
    uint32_t words[16];
    int      n;
    int      at;
    int      calls;
};

int scripted(void *ctx, uint8_t *out, unsigned n) {
    Script *s = (Script *)ctx;
    ++s->calls;
    if (n != 4 || s->at >= s->n) return 0;
    const uint32_t w = s->words[s->at++];
    out[0]           = (uint8_t)(w & 0xff);
    out[1]           = (uint8_t)((w >> 8) & 0xff);
    out[2]           = (uint8_t)((w >> 16) & 0xff);
    out[3]           = (uint8_t)((w >> 24) & 0xff);
    return 1;
}

Script script(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0,
              uint32_t f = 0, uint32_t g = 0, uint32_t h = 0, int n = 1) {
    Script s;
    memset(&s, 0, sizeof(s));
    const uint32_t v[8] = {a, b, c, d, e, f, g, h};
    for (int i = 0; i < n && i < 8; ++i) s.words[i] = v[i];
    s.n = n;
    return s;
}

// An xorshift "working" RNG, seeded: what a real CSPRNG looks like to the minter -- never fails,
// never repeats within any window this suite looks at.
struct Xs {
    uint32_t s;
};
int xorshift(void *ctx, uint8_t *out, unsigned n) {
    Xs *x = (Xs *)ctx;
    for (unsigned i = 0; i < n; ++i) {
        x->s ^= x->s << 13;
        x->s ^= x->s >> 17;
        x->s ^= x->s << 5;
        out[i] = (uint8_t)(x->s & 0xff);
    }
    return 1;
}

// ---- A: a minted room is a host room -------------------------------------------------------------

void arm_range() {
    printf("-- A: the range\n");
    check("ROOM_MAX is the browser's 30-bit sender field", ROOM_MAX == 0x3FFFFFFFu);
    check("the bound on busy retries is what the log lines promise", BUSY_RETRIES == 4);

    // Every draw the RNG can produce lands inside (0, ROOM_MAX].
    Xs   x        = {0x9E3779B9u};
    bool in_range = true, saw_high_bit = false;
    for (int i = 0; i < 20000; ++i) {
        const uint32_t r = mint(xorshift, &x);
        if (r == 0 || r > ROOM_MAX) in_range = false;
        if (r & 0x20000000u) saw_high_bit = true;
    }
    check("20000 mints: never 0, never above ROOM_MAX", in_range);
    check("...and bit 29 is reachable (masked, not clamped)", saw_high_bit);

    // The mask is a MASK: a draw with bits 30-31 set is the same room as the draw without them.
    Script s1 = script(0xC0001234u);
    Script s2 = script(0x00001234u);
    check("bits above 30 are dropped, the rest is the room",
          mint(scripted, &s1) == 0x1234u && mint(scripted, &s2) == 0x1234u);
    Script s3 = script(0xFFFFFFFFu);
    check("an all-ones draw is ROOM_MAX itself", mint(scripted, &s3) == ROOM_MAX);
}

// ---- B: two hosts, and the redraw --------------------------------------------------------------

void arm_two_hosts() {
    printf("-- B: two hosts mint distinct rooms; 0 and `avoid` are redrawn\n");
    // THE TWO-HOSTS CLAUSE, offline: two hosts with the same `[net] port` (the number that used
    // to BE the room) and their own randomness get their own rooms. The port is not an input to
    // mint() at all, which is the whole fix; this arm says so by never mentioning it.
    Xs     a = {0x12345678u}, b = {0x0BADF00Du};
    Minter ha, hb;
    check("host A minted", minter_init(ha, xorshift, &a) && ha.room != 0);
    check("host B minted", minter_init(hb, xorshift, &b) && hb.room != 0);
    check("distinct rooms", ha.room != hb.room);
    check("both start with the retry budget untouched", ha.busy == 0 && hb.busy == 0);

    // A run of many hosts on one relay: no two alike in 512 mints from one working RNG. Not a
    // proof of the 2^-30 figure (that is arithmetic), a proof that the minter does not collapse
    // the draw into a smaller space than it was handed.
    Xs       x = {0xDEADBEEFu};
    uint32_t rooms[512];
    bool     unique = true;
    for (int i = 0; i < 512; ++i) {
        rooms[i] = mint(xorshift, &x);
        for (int j = 0; j < i; ++j)
            if (rooms[j] == rooms[i]) unique = false;
    }
    check("512 hosts, 512 rooms", unique);

    // A draw of 0 is the directory room and is REDRAWN, not returned: the RNG's first word is 0
    // (and 0x40000000 masks to 0 too -- both are the same failure), the second is the room.
    Script z = script(0x00000000u, 0x40000000u, 0x00000777u, 0, 0, 0, 0, 0, 3);
    check("0 and a masked-to-0 draw are skipped; the third draw is the room",
          mint(scripted, &z) == 0x777u && z.calls == 3);
    // ...and a draw equal to `avoid` likewise.
    Script av = script(0x00000777u, 0x00000777u, 0x00000778u, 0, 0, 0, 0, 0, 3);
    check("the avoided room is skipped however often it is drawn",
          mint(scripted, &av, 0x777u) == 0x778u && av.calls == 3);
    // MINT_DRAWS bounds the redraw: an RNG that only ever offers 0 gives up as a mint of 0.
    Script zz = script(0, 0, 0, 0, 0, 0, 0, 0, 8);
    check("eight zero draws -> 0 (the RNG has stopped being one)",
          mint(scripted, &zz) == 0 && zz.calls == MINT_DRAWS);
}

// ---- C: room_busy and its bound ------------------------------------------------------------------

void arm_busy() {
    printf("-- C: room_busy -> re-mint, at most BUSY_RETRIES times\n");
    // The script is the sequence of rooms this host will name: the first at init, then one per
    // room_busy. Each must differ from the busy one it moves away from.
    Script s = script(0x1001u, 0x1002u, 0x1003u, 0x1004u, 0x1005u, 0x1006u, 0x1007u, 0x1008u, 8);
    Minter m;
    check("init", minter_init(m, scripted, &s) && m.room == 0x1001u);
    uint32_t prev = m.room;
    for (int i = 1; i <= BUSY_RETRIES; ++i) {
        const bool moved = minter_on_busy(m);
        char       what[96];
        sprintf(what, "room_busy #%d: re-minted to a room that is not the busy one (busy=%d)", i,
                m.busy);
        check(what, moved && m.room != prev && m.room != 0 && m.busy == i);
        prev = m.room;
    }
    check("the retry count is exactly the bound", m.busy == BUSY_RETRIES);
    // THE (BUSY_RETRIES+1)-TH REFUSAL STANDS. Randomness is still available (three words left in
    // the script), so the only thing that can stop the re-mint is the bound -- which is the
    // mutation this arm exists to catch.
    const uint32_t last  = m.room;
    const int      calls = s.calls;
    check("a further room_busy is NOT answered with a fresh room", !minter_on_busy(m));
    check("...the room is left as it was (the refusal names a real room)", m.room == last);
    check("...the retry count does not grow past the bound", m.busy == BUSY_RETRIES);
    check("...and the RNG was not even asked", s.calls == calls);

    // A re-mint that draws the busy room again redraws (the `avoid` path, through on_busy).
    Script r = script(0x2001u, 0x2001u, 0x2001u, 0x2002u, 0, 0, 0, 0, 4);
    Minter n;
    minter_init(n, scripted, &r);
    check("busy room re-drawn twice -> skipped both times", minter_on_busy(n) && n.room == 0x2002u);
}

// ---- D: the crash relaunch -----------------------------------------------------------------------

void arm_relaunch() {
    printf("-- D: a relaunch inside idle_secs gets a fresh room, not the stale one\n");
    // The stale slot on the relay names the room the crashed process minted. The relaunched
    // process is a FRESH minter with its own draw: with the port no longer an input there is
    // nothing that makes the two agree, so the relay sees a new room beside the stale one rather
    // than a second host at the same code. Two fresh minters over a working RNG stand in for the
    // two processes.
    Xs     first = {0x5EED0001u}, again = {0x5EED0002u};
    Minter crashed, relaunched;
    minter_init(crashed, xorshift, &first);
    minter_init(relaunched, xorshift, &again);
    check("the relaunched host's room is not the stale slot's", relaunched.room != crashed.room);
    check("...and it is a host room in its own right", relaunched.room != 0 &&
                                                           relaunched.room <= ROOM_MAX);

    // The deterministic half: a caller that knows the stale room (a restart inside one process,
    // which udp_transport's re-host path is) never gets it back, even from an RNG that offers it.
    const uint32_t stale = 0x0ABCDEFu;
    Script         s     = script(stale, stale, 0x0ABCDF0u, 0, 0, 0, 0, 0, 3);
    Minter         m;
    check("init with `avoid` = the stale room skips it however often it is drawn",
          minter_init(m, scripted, &s, stale) && m.room == 0x0ABCDF0u && s.calls == 3);
}

// ---- E: no randomness ----------------------------------------------------------------------------

void arm_rng_failure() {
    printf("-- E: the RNG failing is a mint of 0, which the transport refuses to host on\n");
    Script none = script(0, 0, 0, 0, 0, 0, 0, 0, 0); // n = 0: every call fails
    Minter m;
    check("init over a dead RNG mints 0 and says so", !minter_init(m, scripted, &none) &&
                                                          m.room == 0);
    check("...after exactly one ask (a failure is not retried as if it were a bad draw)",
          none.calls == 1);
    check("a null RNG mints 0 too", mint(nullptr, nullptr) == 0);

    // The RNG dying MID-RETRY leaves the room where it was: the refusal that then stands names
    // the room the relay actually refused, not 0.
    Script one = script(0x3001u, 0, 0, 0, 0, 0, 0, 0, 1);
    Minter n;
    minter_init(n, scripted, &one);
    check("room_busy with no randomness left: no re-mint, room untouched, count untouched",
          !minter_on_busy(n) && n.room == 0x3001u && n.busy == 0);
}

} // namespace

int run_udproomtest() {
    printf("=== udproomtest (mp:R6: the host's relay room is minted, never the port; no sockets) ===\n");
    arm_range();
    arm_two_hosts();
    arm_busy();
    arm_relaunch();
    arm_rng_failure();
    printf("=== udproomtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
