//
// desync_selftest.cpp -- `net_selftest.exe desynctest`: every decision the runtime desync detector
// (D21) makes, with no game, no rig and no socket.
//
// WHY IT IS THE RIGHT PLACE FOR MOST OF D21's EVIDENCE. Three of the item's clauses are about things
// that must NOT happen, and a rig run cannot demonstrate the absence of an event -- a clean 2-peer
// determinism run and a detector that never looked produce the same silence. So the rig proves the
// two arms that need a real match (a deliberately desynced run IS caught; a clean run reports
// nothing), and the decisions underneath them are asserted here:
//
//   (c) the compared value drops the peer-local regions   -> fold_state / first-diverging-region
//   (f) 21428 mismatching steps produce ONE notice        -> should_notify over the real count
//   (b) a lost or future sample is never called a desync  -> judge()'s too_old / not_yet arms
//
// The last one is the cry-wolf failure mode the item singles out, and it is entirely a matter of
// what judge() returns for a step it cannot find -- which is testable here and nowhere else.
//
#include <stdio.h>
#include <string.h>

#include "desync/desync_watch.h"

using namespace mh::desync;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// A small stand-in manifest: 8 regions, two of them excluded -- the same shape as the live one
// (56 regions, 9 excluded), which is what the assertions are about.
constexpr int NR       = 8;
constexpr int EX_A     = 3; // "peer_horizon"-like: peer-local, differs on 100% of steps
constexpr int EX_B     = 6; // "frame_ring"-like
bool          g_ex[NR] = {false, false, false, true, false, false, true, false};

void fill(uint64_t *per, uint64_t seed) {
    for (int i = 0; i < NR; ++i) per[i] = seed + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
}

// Build a wire record the way the live sender does.
sample_wire make_sample(uint32_t step, const uint64_t *per, uint64_t fp) {
    sample_wire s;
    memset(&s, 0, sizeof(s));
    s.magic        = WIRE_MAGIC;
    s.version      = WIRE_VERSION;
    s.region_count = (uint16_t)NR;
    s.manifest_fp  = fp;
    s.step         = step;
    s.state_hash   = fold_state(per, g_ex, NR);
    memcpy(s.per, per, sizeof(uint64_t) * NR);
    return s;
}

} // namespace

int run_desynctest() {
    printf("=== desynctest (D21: a live match is told when the two sims stop agreeing) ===\n");

    static const char *const NAMES[NR] = {"a", "b", "c", "peer_horizon", "e", "f", "frame_ring", "h"};
    static const uint32_t    LENS[NR]  = {16, 32, 48, 64, 80, 96, 112, 128};
    const uint64_t           FP        = manifest_fingerprint(NAMES, LENS, g_ex, NR);

    // ---- 1. the wire record -----------------------------------------------------------------
    {
        check("wire_size(0) is the header alone", wire_size(0) == (int)(sizeof(sample_wire) - 8 * MAX_REGIONS));
        check("wire_size(n) grows 8 bytes per region", wire_size(NR) == wire_size(0) + 8 * NR);
        check("the live manifest fits the record", 56 <= MAX_REGIONS);

        uint64_t per[NR];
        fill(per, 0x1111);
        sample_wire s = make_sample(7, per, FP);
        check("a well-formed frame is sane", frame_is_sane(s, wire_size(NR)));

        // Every rejection arm, one at a time -- a validator that only ever sees good input is not one.
        sample_wire b = s;
        b.magic       = 0xdeadbeefu;
        check("a wrong magic is rejected", !frame_is_sane(b, wire_size(NR)));
        b         = s;
        b.version = WIRE_VERSION + 1;
        check("a wrong version is rejected", !frame_is_sane(b, wire_size(NR)));
        b              = s;
        b.region_count = 0;
        check("a zero region count is rejected", !frame_is_sane(b, wire_size(0)));
        b              = s;
        b.region_count = MAX_REGIONS + 1;
        check("an over-large region count is rejected", !frame_is_sane(b, wire_size(NR)));
        b          = s;
        b.reserved = 1;
        check("a non-zero reserved word is rejected", !frame_is_sane(b, wire_size(NR)));
        // THE ONE THAT MATTERS: a truncated frame whose header still claims NR regions. Accepting it
        // would compare against uninitialised bytes and call the difference a desync.
        check("a frame shorter than its own header claims is rejected",
              !frame_is_sane(s, wire_size(NR) - 8));
        check("a frame longer than its own header claims is rejected",
              !frame_is_sane(s, wire_size(NR) + 8));
    }

    // ---- 2. clause (c): the compared value drops the peer-local regions ----------------------
    {
        uint64_t a[NR], b[NR];
        fill(a, 0x2222);
        memcpy(b, a, sizeof(a));
        const uint64_t base = fold_state(a, g_ex, NR);

        // In the 2026-08-28 run the peer-local regions differ on 100% of steps BY CONSTRUCTION.
        // Comparing a hash that counted them would report a desync in every healthy game.
        b[EX_A] ^= 0xffffffffffffffffULL;
        b[EX_B] ^= 0x1ULL;
        check("changing an EXCLUDED region does not move the state hash", fold_state(b, g_ex, NR) == base);

        // ...and the mask is not vacuous: everything else still counts.
        for (int i = 0; i < NR; ++i) {
            if (g_ex[i]) continue;
            uint64_t c[NR];
            memcpy(c, a, sizeof(a));
            c[i] ^= 1ULL;
            check("changing an INCLUDED region moves the state hash", fold_state(c, g_ex, NR) != base);
        }
    }

    // ---- 3. the ring: no modulo aliasing at a cadence ----------------------------------------
    {
        // Sampled steps are multiples of the cadence. A `slot = step % RING_CAP` index would keep only
        // 64/gcd(20,64) = 16 of the 64 entries and silently drop the rest -- the ring would claim a
        // 1280-step history and hold 320. Store a full ring at cadence 20 and demand all of it back.
        ring r;
        r.clear();
        uint64_t per[NR];
        for (int k = 1; k <= RING_CAP; ++k) {
            fill(per, (uint64_t)k);
            r.put((uint32_t)k * 20u, fold_state(per, g_ex, NR), per, NR);
        }
        int found = 0;
        for (int k = 1; k <= RING_CAP; ++k)
            if (r.find((uint32_t)k * 20u)) ++found;
        check("a full ring of cadence-spaced steps is entirely retrievable", found == RING_CAP);
        check("the ring reports its newest step", r.newest == (uint32_t)RING_CAP * 20u);
        check("an unsampled step is not found", r.find(21u) == nullptr);

        // One more put evicts the oldest -- and the evicted step must MISS, not return the newer
        // entry that now owns its slot.
        fill(per, 999);
        r.put((uint32_t)(RING_CAP + 1) * 20u, fold_state(per, g_ex, NR), per, NR);
        check("the oldest entry is evicted", r.find(20u) == nullptr);
        check("the newest entry is present", r.find((uint32_t)(RING_CAP + 1) * 20u) != nullptr);
    }

    // ---- 4. judge(): agreement, disagreement, and the two NON-verdicts ------------------------
    {
        ring r;
        r.clear();
        uint64_t mine[NR];
        fill(mine, 0x3333);
        r.put(100, fold_state(mine, g_ex, NR), mine, NR);
        r.put(120, fold_state(mine, g_ex, NR), mine, NR);

        // agreement
        verdict v = judge(r, make_sample(100, mine, FP), g_ex, NR, FP, r.newest);
        check("identical state at the same step is OK", v.kind == outcome::ok);
        check("an OK verdict names no region", v.first_region == -1);

        // disagreement, localized
        uint64_t theirs[NR];
        memcpy(theirs, mine, sizeof(mine));
        theirs[4] ^= 0x55ULL;
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("differing state at the same step is a MISMATCH", v.kind == outcome::mismatch);
        check("the mismatch names the first diverging region", v.first_region == 4);
        check("the mismatch carries both hashes", v.mine != v.theirs && v.mine == fold_state(mine, g_ex, NR));

        // THE CLAUSE (c) TRAP AT THE LOCALIZATION LEVEL: a peer-local region differing must never be
        // NAMED as the culprit -- it differs in every healthy game, so naming it would send every
        // future reader to the wrong region.
        memcpy(theirs, mine, sizeof(mine));
        theirs[EX_A] ^= 0xffULL; // excluded, index BELOW the real culprit
        theirs[5] ^= 0x77ULL;    // the real one
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("a mismatch skips excluded regions when naming the culprit",
              v.kind == outcome::mismatch && v.first_region == 5);

        // ...and a sample differing ONLY in excluded regions is not a mismatch at all.
        memcpy(theirs, mine, sizeof(mine));
        theirs[EX_A] ^= 0xffULL;
        theirs[EX_B] ^= 0xffULL;
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("a sample differing only in excluded regions is OK", v.kind == outcome::ok);

        // THE CRY-WOLF ARMS. Neither of these is a desync, and reporting either as one is the
        // failure D21 clause (b) exists to prevent.
        v = judge(r, make_sample(140, mine, FP), g_ex, NR, FP, r.newest);
        check("a sample for a step we have not reached is HELD, not reported",
              v.kind == outcome::not_yet);
        v = judge(r, make_sample(60, mine, FP), g_ex, NR, FP, r.newest);
        check("a sample whose ring entry is gone is DROPPED, not reported", v.kind == outcome::too_old);

        // A peer hashing a different manifest is a BUILD mismatch. Without this it would read as a
        // desync on the very first sample of a mixed-build game.
        v = judge(r, make_sample(100, mine, FP ^ 1ULL), g_ex, NR, FP, r.newest);
        check("a different manifest fingerprint is reported as a manifest mismatch",
              v.kind == outcome::manifest_mismatch);
        sample_wire wrong_n  = make_sample(100, mine, FP);
        wrong_n.region_count = NR - 1;
        v                    = judge(r, wrong_n, g_ex, NR, FP, r.newest);
        check("a different region count is reported as a manifest mismatch",
              v.kind == outcome::manifest_mismatch);
    }

    // ---- 5. clause (f): 21428 mismatching steps produce ONE notice ---------------------------
    {
        // The number is the real one -- the 2026-08-29 match was desynced from step 1 and ran 21428
        // steps. A per-step notice would have drawn it 21428 times.
        int notices = 0, full = 0, rollups = 0;
        for (int n = 1; n <= 21428; ++n) {
            if (should_notify(n)) ++notices;
            if (should_log_full(n)) ++full;
            if (should_log_rollup(n)) ++rollups;
        }
        check("21428 consecutive mismatches produce exactly ONE user-visible notice", notices == 1);
        check("the first mismatches are logged in full", full == LOG_FIRST);
        check("the rest are rolled up, not one line each", rollups == (21428 - LOG_FIRST) / LOG_EVERY);
        check("the log volume is bounded well under the mismatch count", full + rollups < 200);
    }

    // ---- 6. the pending queue: FIFO, and full means drop the OLDEST --------------------------
    {
        pending_queue q;
        q.clear();
        uint64_t per[NR];
        fill(per, 0x4444);
        for (int i = 0; i < PENDING_CAP; ++i) q.push(i % 8, make_sample((uint32_t)i, per, FP));
        check("the queue holds its capacity", q.count == PENDING_CAP && q.dropped == 0);

        q.push(1, make_sample(9999, per, FP)); // one too many
        check("an overflowing push drops exactly one", q.dropped == 1 && q.count == PENDING_CAP);

        int         from = -1;
        sample_wire s;
        check("pop returns a sample", q.pop(from, s));
        // The OLDEST (step 0) was the one dropped, so the head is now step 1 -- fresh evidence beats
        // stale, the same choice the transport's own inbound queue makes.
        check("the dropped sample was the oldest", s.step == 1);

        q.clear();
        check("a cleared queue pops nothing", !q.pop(from, s) && q.count == 0);
    }

    // ---- 7. snapshot scheduling (D25): the dump steps are ABSOLUTE, not per-peer ---------------
    {
        // The dumps are worth something only if every peer writes one for the SAME lockstep step.
        // An absolute grid delivers that WITHOUT the peers agreeing on when they noticed -- which
        // they do not: peer A may judge B's sample for step 1450 while B is still judging A's for
        // 1400. A schedule of the form `mismatch_step + k*every` gives those two 1650 and 1600 and
        // neither file has a partner, which is the bug these checks exist to keep out.
        check("the grid is 8 cadences", snapshot_grid(50) == 400);
        check("a degenerate cadence has no grid", snapshot_grid(0) == 0);

        check("a grid step is due", snapshot_due(1600, 50));
        check("a non-grid step is not", !snapshot_due(1650, 50));
        check("a cadence multiple that is not a grid multiple is not due", !snapshot_due(1450, 50));
        check("step 0 is never due (nothing has happened yet)", !snapshot_due(0, 50));
        check("a degenerate cadence is never due", !snapshot_due(1600, 0));

        // THE PROPERTY THAT MATTERS: two peers that armed at DIFFERENT steps still produce files
        // for common steps. Simulated over a 3000-step match with the shipped budget of 3 -- peer A
        // arms at 1450 (its first dump 1600), peer B at 1650 (its first 2000). Every step B writes
        // must be one A also writes, and at least one must be shared or the pair is undiffable.
        const int BUDGET = 3;
        uint32_t  a_steps[BUDGET], b_steps[BUDGET];
        int       na = 0, nb = 0;
        for (uint32_t s = 1; s <= 3000; ++s) {
            if (!snapshot_due(s, 50)) continue;
            if (s >= 1450 && na < BUDGET) a_steps[na++] = s;
            if (s >= 1650 && nb < BUDGET) b_steps[nb++] = s;
        }
        check("both peers spend their budget", na == BUDGET && nb == BUDGET);
        int shared = 0;
        for (int i = 0; i < nb; ++i)
            for (int j = 0; j < na; ++j)
                if (b_steps[i] == a_steps[j]) ++shared;
        check("peers that armed 200 steps apart still share dump steps", shared >= 2);
        check("the later peer's first dump is a grid step the earlier one also hit",
              a_steps[0] == 1600 && b_steps[0] == 2000);
    }

    printf("=== desynctest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
