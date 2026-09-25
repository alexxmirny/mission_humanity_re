//
// desync/desync_watch.h -- RUNTIME desync detection (D21).
//
// THE PROBLEM. The state hash is an OFFLINE oracle: each peer writes per-step hashes to
// mh_harness.log and tools/mp_analyze.py joins two peers' logs AFTERWARDS. Nothing compares them
// while the match is running, so a broken match plays to the end and nobody is told. That is not a
// hypothetical: the 2026-08-29 internet match was desynced from STEP 1, ran eight minutes, and the
// diagnosis needed a two-machine log pull. Lockstep keeps exchanging orders regardless of whether
// the two sims agree, because agreement is never tested at runtime.
//
// WHAT THIS DOES, AND WHERE IT STOPS. Each peer periodically hashes its own sim state, broadcasts
// the (step, hash) sample, and compares an arriving sample against its OWN entry for that same step.
// On a disagreement it logs loudly, shows ONE on-screen notice, and LEAVES THE MATCH RUNNING.
//   * Ending the match is the player's decision, not the engine's -- a step-1 mismatch and a
//     step-20000 mismatch deserve different human responses, and a false positive that kills a good
//     game is a worse bug than the one being detected.
//   * There is nothing to repair it with. The existing resync path re-aligns the HORIZON; it does
//     not transfer state, so it cannot fix a diverged sim (and D14 records that a forced resync
//     opens its own divergence window). "Detect then resync" is a plausible-sounding mechanism that
//     cannot work.
// So: detect and report. `[desync] action` exists as an opt-in for a future halt; it is default-off
// and this module implements nothing beyond saying that a non-zero value was asked for.
//
// THE COMPARED VALUE IS THE STATE-ONLY HASH, not the combined one. A set of regions (peer_horizon,
// the six clock doubles, frame_ring, fps_estimate, plus order_staging and the ai_econ set) are
// peer-LOCAL by construction and differ on 100% of steps in a healthy game; comparing the combined
// hash would report a desync in every match. The exclusion travels with the manifest entry
// (mh::state::HASH_REGIONS[i].excluded) and is folded into the manifest fingerprint below, so two
// peers that disagree about WHICH regions count are caught as a build mismatch rather than reported
// as a desync.
//
// STRUCTURE. Everything that decides anything lives in this header as pure functions over explicit
// state, so `net_selftest.exe desynctest` runs it with no game, no rig and no socket. The .cpp binds
// that to the live manifest, the transport and the HUD.
//
// KEYED BY STEP, NOT BY ARRIVAL. Each peer keeps a small ring of its own (step -> hash) and looks an
// arriving sample up by ITS step, so the check is immune to the peers being at different wall-clock
// points. A sample for a step we have not reached yet is HELD; one whose step has already been
// evicted is DROPPED and counted, never reported -- a detector that reports "I lost the evidence" as
// a desync is the cry-wolf failure D21 clause (b) exists to prevent.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mh::desync {

// Room for the hash manifest with headroom; the live table is 56 entries (HASH_REGION_COUNT). The
// wire record carries only `region_count` of them, so growing the manifest costs bytes, not a format
// change -- but crossing MAX_REGIONS is a hard stop, asserted at arm time in the .cpp.
inline constexpr int MAX_REGIONS = 96;

inline constexpr uint32_t WIRE_MAGIC   = 0x434e5344u; // 'DSNC' little-endian
inline constexpr uint16_t WIRE_VERSION = 1;

// The on-wire sample. Sent as a FLAG_HASH control frame (never the game queue, never the game's own
// lockstep wire -- see MH_Net_SendHash). Packed and fixed-width: the two peers are the same x86
// build today, but this is a wire format and it is written like one.
//
// D31 clause C adds ONE optional field, the cumulative order digest -- but NOT as a member here.
// See the WIRE COMPATIBILITY note above judge_order() for why: it rides `ORDER_DIGEST_BYTES` of
// trailing bytes after `wire_size(region_count)`, detected by the receiver from frame length, so an
// old (no-digest) build and a new one still agree on this struct's own layout and on region_count/
// manifest_fp -- state comparison never has to know the feature exists.
#pragma pack(push, 1)
struct sample_wire {
    uint32_t magic;            // WIRE_MAGIC
    uint16_t version;          // WIRE_VERSION
    uint16_t region_count;     // how many `per[]` entries follow
    uint64_t manifest_fp;      // fingerprint of the sender's hash manifest (names+lengths+excluded)
    uint32_t step;             // the sim step this sample describes
    uint32_t reserved;         // keep `state_hash` 8-byte aligned; must be 0
    uint64_t state_hash;       // FNV-1a-64 fold of the NON-EXCLUDED per[] entries, in manifest order
    uint64_t per[MAX_REGIONS]; // only the first `region_count` are transmitted
};
#pragma pack(pop)

// Bytes actually put on the wire for `n` regions -- the trailing per[] slack is never sent.
inline int wire_size(int n) {
    return (int)(sizeof(sample_wire) - sizeof(uint64_t) * (MAX_REGIONS - (n < 0 ? 0 : n)));
}

// D31 clause C: the ONE optional trailing field, appended after `wire_size(region_count)` bytes --
// see the WIRE COMPATIBILITY note above judge_order(). Not part of `sample_wire` itself (adding a
// struct field ahead of `per[]` would shift every peer's `per[]` offset for a v1 sender; appending
// one after the FULL `per[MAX_REGIONS]` would land past what a smaller `region_count` actually
// transmits). A plain trailing byte count, detected by the receiver from frame LENGTH alone.
inline constexpr int ORDER_DIGEST_BYTES = (int)sizeof(uint64_t);

// ---- the state-only fold ------------------------------------------------------------------------
// Byte-identical to the harness's (harness.cpp on_sim_step): FNV-1a-64 over the eight bytes of each
// NON-EXCLUDED region hash, in manifest order. Same offset basis, same order, same skip rule -- so an
// in-band verdict and tools/mp_analyze.py's offline one are statements about the same number.
inline constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
inline constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

inline uint64_t fnv1a(const void *p, size_t n, uint64_t h = FNV_OFFSET) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= FNV_PRIME;
    }
    return h;
}

inline uint64_t fold_state(const uint64_t *per, const bool *excluded, int n) {
    uint64_t h = FNV_OFFSET;
    for (int i = 0; i < n; ++i)
        if (!excluded[i]) h = fnv1a(&per[i], sizeof(per[i]), h);
    return h;
}

// A fingerprint of WHAT is being hashed -- every region's name, length and excluded flag, in order.
// Two peers whose manifests differ produce different numbers for the same healthy state, so without
// this the first sample of a mixed-build game reads as a desync. Compared before the hashes are.
inline uint64_t manifest_fingerprint(const char *const *names, const uint32_t *lens,
                                     const bool *excluded, int n) {
    uint64_t h = fnv1a(&n, sizeof(n));
    for (int i = 0; i < n; ++i) {
        h               = fnv1a(names[i], strlen(names[i]), h);
        h               = fnv1a(&lens[i], sizeof(lens[i]), h);
        const uint8_t e = excluded[i] ? 1u : 0u;
        h               = fnv1a(&e, 1, h);
    }
    return h;
}

// ---- the local ring -----------------------------------------------------------------------------
// Holds the last RING_CAP SAMPLED steps (not the last RING_CAP steps): at cadence N it spans
// RING_CAP*N steps of history, which is the window a late sample may arrive in.
inline constexpr int RING_CAP = 64;

struct ring_entry {
    uint32_t step;
    bool     used;
    uint64_t state;
    uint64_t per[MAX_REGIONS];
    // D31 clause C: OUR OWN cumulative order digest AT THIS STEP (see order_digest_tick() in the
    // .cpp). Unlike `state`/`per[]`, which are re-derived from a fresh walk every sample, this value
    // is carried forward from every step since session start -- it is what makes an order
    // disagreement that later re-converges still show up: the digest that folded the diverging step
    // never un-folds it.
    uint64_t order_digest;
};

struct ring {
    ring_entry e[RING_CAP];
    int        head;   // next slot to write
    int        count;  // entries held (<= RING_CAP)
    uint32_t   newest; // highest step stored (0 = nothing yet)

    void clear() { memset(this, 0, sizeof(*this)); }

    // `order_digest` defaults to 0 so every EXISTING caller (desynctest's pure ring exercises,
    // written before D31) keeps compiling unchanged; the live caller (sample_and_judge) always
    // passes the real running value.
    void put(uint32_t step, uint64_t state, const uint64_t *per, int n, uint64_t order_digest = 0) {
        ring_entry &s = e[head];
        s.step        = step;
        s.used        = true;
        s.state       = state;
        memcpy(s.per, per, sizeof(uint64_t) * (size_t)(n < 0 ? 0 : n));
        s.order_digest = order_digest;
        head           = (head + 1) % RING_CAP;
        if (count < RING_CAP) ++count;
        if (step > newest) newest = step;
    }

    // Linear scan, deliberately -- NOT slot = step % RING_CAP. Sampled steps are multiples of the
    // cadence, so a modulo index aliases: at cadence 20 the multiples of 20 only ever hit 16 of the
    // 64 slots (64/gcd(20,64)), silently shrinking the history to a quarter of what the size says.
    // The scan runs once per RECEIVED sample -- a few per second over <=64 entries.
    const ring_entry *find(uint32_t step) const {
        for (int i = 0; i < count; ++i)
            if (e[i].used && e[i].step == step) return &e[i];
        return nullptr;
    }
};

// ---- the verdict --------------------------------------------------------------------------------
enum class outcome : int {
    ok = 0,            // compared, hashes agree
    mismatch,          // compared, hashes DISAGREE -- the thing this module exists to say
    not_yet,           // their step is ahead of ours: hold the sample and retry
    too_old,           // their step fell out of our ring before we could compare: dropped, counted
    bad_frame,         // malformed / wrong version / wrong length
    manifest_mismatch, // the peers are not hashing the same thing (different builds)
};

struct verdict {
    outcome  kind;
    uint32_t step;
    uint64_t mine;
    uint64_t theirs;
    int      first_region; // index of the first DIVERGING non-excluded region, or -1
};

// Validate a received frame in isolation: magic, version, declared region count, and that the byte
// count the transport handed us is exactly the count the header claims. `len` is the payload length.
inline bool frame_is_sane(const sample_wire &s, int len) {
    if (len < wire_size(0)) return false;
    if (s.magic != WIRE_MAGIC || s.version != WIRE_VERSION) return false;
    if (s.region_count == 0 || s.region_count > MAX_REGIONS) return false;
    if (s.reserved != 0) return false;
    return len == wire_size((int)s.region_count);
}

// Compare one received sample against our own ring. `our_newest` is the highest step we have SAMPLED
// (not the current step) -- a sample above it is future, one below it that we cannot find is past.
inline verdict judge(const ring &r, const sample_wire &s, const bool *excluded, int n,
                     uint64_t our_manifest_fp, uint32_t our_newest) {
    verdict v = {outcome::bad_frame, s.step, 0, s.state_hash, -1};
    if ((int)s.region_count != n || s.manifest_fp != our_manifest_fp) {
        v.kind = outcome::manifest_mismatch;
        return v;
    }
    const ring_entry *mine = r.find(s.step);
    if (!mine) {
        v.kind = (s.step > our_newest) ? outcome::not_yet : outcome::too_old;
        return v;
    }
    v.mine = mine->state;
    if (mine->state == s.state_hash) {
        v.kind = outcome::ok;
        return v;
    }
    v.kind = outcome::mismatch;
    for (int i = 0; i < n; ++i) {
        if (excluded[i]) continue; // peer-local by construction; never the culprit named
        if (mine->per[i] != s.per[i]) {
            v.first_region = i;
            break;
        }
    }
    return v;
}

// ---- D31 clause C: the cumulative order digest --------------------------------------------------
// A SEPARATE, weaker channel from the state verdict above. The state hash re-converges when a short
// order-region divergence heals on its own (D30: 7-18 steps, then agreement) -- which is exactly the
// case the shipped detector was blind to, because it only ever compares the CURRENT state at a
// sampled step. The order digest never re-converges by construction: it is an FNV-1a-64 fold, taken
// EVERY sim step (not just on the sampling cadence), of what BOTH peers apply that step, carried
// forward from session start. A past disagreement stays folded into it forever, so it shows at the
// very next sample even after the state has healed.
//
// WIRE COMPATIBILITY. The digest rides the SAME sample_wire frame as an OPTIONAL trailing 8 bytes,
// appended AFTER the existing `wire_size(region_count)` bytes -- it does NOT become a new struct
// field or a new `region_count` entry, and WIRE_VERSION does not change. Two consequences, both
// deliberate (mp:D31 R2 ruling):
//   * region_count and manifest_fp are UNCHANGED, so an old (no-digest) build and a new build still
//     agree on the STATE manifest and keep comparing state normally -- see judge() above, which never
//     sees this feature exist.
//   * an OLD peer receiving a NEW peer's longer frame sees a length that does not equal its own
//     wire_size(region_count) and drops it as a bad_frame (counted, not crashed, not misread) --
//     "old peer ignores". A NEW peer receiving an OLD peer's frame (no trailing bytes) parses the
//     base fields exactly as today and simply has no digest to compare -- `judge_order` below
//     returns `absent`, never `mismatch` -- "new peer compares only state when the digest is
//     absent". Proven by the udpstatstest arm mixing a v1-shaped and a v2-shaped frame stream.
enum class order_outcome : int {
    absent = 0, // no digest on the incoming sample (an rc2 peer, or ours/theirs not sampled yet)
    ok,         // both peers' cumulative digests agree at this step
    mismatch,   // they disagree -- some order, at some step up to and including this one, differed
};

// `theirs_has` is false for an old-build sample (no trailing bytes) or one this build could not
// parse a digest out of; `mine` is looked up in the SAME ring the state verdict already found an
// entry in, so this is only meaningful when the caller already has a `judge()` verdict of `ok` or
// `mismatch` for the same step.
inline order_outcome judge_order(const ring &r, uint32_t step, uint64_t theirs_digest, bool theirs_has) {
    if (!theirs_has) return order_outcome::absent;
    const ring_entry *mine = r.find(step);
    if (!mine) return order_outcome::absent; // defensive: the state verdict should already guarantee this
    return (mine->order_digest == theirs_digest) ? order_outcome::ok : order_outcome::mismatch;
}

// ---- notice / log throttling --------------------------------------------------------------------
// D21 (f): 21428 consecutive mismatching steps must produce ONE user-visible notice, not a storm.
// The log is throttled separately and more generously -- the FIRST mismatch is written in full (it
// carries the step, both hashes and the diverging region), then one summary per LOG_EVERY.
inline constexpr int LOG_FIRST = 4;   // the first N mismatches are written in full
inline constexpr int LOG_EVERY = 200; // after that, one rollup line every N

// D31 R2 (user ruling, 2026-09-24): the on-screen notice is gated on PERSISTENCE, not on the first
// mismatching sample. D30 showed order-region divergences that last 7-18 steps and then re-converge
// on their own -- at the shipped every=50 cadence that is well under one sample interval, so the
// FIRST mismatching sample is frequently one the very next sample will contradict. `n` here is
// CONSECUTIVE mismatching samples (reset to 0 by an intervening `ok`), and the bar is the SECOND one
// in a row: one interval of standing disagreement, not one sample of it. Still exactly ONE notice
// per match (fires only at n==2, never again for the same unbroken run, and a fresh incident after a
// reconvergence restarts its own count from 0) -- D21 clause (f)'s 21428-consecutive-mismatch case is
// unaffected, it still produces exactly one notice, just on sample #2 instead of #1.
inline bool should_notify(int consecutive_mismatches_so_far) { return consecutive_mismatches_so_far == 2; }

// D31 clause C: the order-digest channel is ALWAYS log+rollup only (R2) -- it never reaches
// `should_notify`, so it has no persistence gate of its own. It reuses the SAME full/rollup
// throttle as the state channel (LOG_FIRST/LOG_EVERY), keyed by its own occurrence count.

inline bool should_log_full(int mismatches_so_far) { return mismatches_so_far <= LOG_FIRST; }

inline bool should_log_rollup(int mismatches_so_far) {
    return mismatches_so_far > LOG_FIRST && (mismatches_so_far % LOG_EVERY) == 0;
}

// ---- snapshot scheduling (D25) ------------------------------------------------------------------
// On a mismatch each peer ARMS, and thereafter dumps its full state on an ABSOLUTE step grid. The
// dumps are only worth anything if every peer writes one for the SAME lockstep step -- a dump at
// "N steps after I noticed" compares two different instants and names nothing -- and an absolute
// grid is what guarantees that WITHOUT the peers agreeing on when they noticed.
//
// THE FIRST DESIGN TIED THE TARGET TO THE MISMATCHING SAMPLE'S STEP (`mismatch_step + 4*every`),
// which looks equivalent and is not: peer A judges B's sample for step 1450 while B may still be
// judging A's for 1400, so the two derive 1650 and 1600 and neither file has a partner. The grid
// removes the assumption instead of tightening it.
//
// GRID = 8 cadences (400 steps, ~8 s at the shipped every=50 and 50 steps/s): far enough out that
// every peer has armed by the next tick, rare enough that a match cannot fill a disk. A peer that
// arms late simply misses the first grid step and joins at the next -- which still pairs, because
// the budget allows several.
inline uint32_t snapshot_grid(int every) { return every > 0 ? (uint32_t)(8 * every) : 0u; }

inline bool snapshot_due(uint32_t step, int every) {
    const uint32_t g = snapshot_grid(every);
    return g != 0u && step != 0u && (step % g) == 0u;
}

// ---- pending inbound ----------------------------------------------------------------------------
// Samples arrive on the RECV THREAD and are judged on the main thread, so they queue. A sample for a
// step we have not reached is put back; the queue is small and DROPS THE OLDEST when full, which is
// the same choice the transport's own inbound queue makes and for the same reason (fresh evidence
// beats stale).
inline constexpr int PENDING_CAP = 48;

struct pending_queue {
    sample_wire q[PENDING_CAP];
    int         sender[PENDING_CAP];
    // D31 clause C: the PEER's order digest (if the frame carried one) rides alongside its
    // sample_wire rather than inside it -- see the WIRE COMPATIBILITY note above judge_order(). Kept
    // as parallel arrays, not a new struct, so the queue's existing FIFO/overflow mechanics (and the
    // selftest section that exercises them) do not have to change shape.
    uint64_t order_digest[PENDING_CAP];
    bool     has_order_digest[PENDING_CAP];
    int      head, count;
    int      dropped;

    void clear() { memset(this, 0, sizeof(*this)); }

    // `order_digest`/`has_order_digest` default (0 / false) so every EXISTING caller (desynctest's
    // pure queue exercises) keeps compiling unchanged.
    void push(int from, const sample_wire &s, uint64_t od = 0, bool has_od = false) {
        if (count == PENDING_CAP) {
            head = (head + 1) % PENDING_CAP;
            --count;
            ++dropped;
        }
        const int t         = (head + count) % PENDING_CAP;
        q[t]                = s;
        sender[t]           = from;
        order_digest[t]     = od;
        has_order_digest[t] = has_od;
        ++count;
    }

    bool pop(int &from, sample_wire &out, uint64_t *od = nullptr, bool *has_od = nullptr) {
        if (count == 0) return false;
        out  = q[head];
        from = sender[head];
        if (od) *od = order_digest[head];
        if (has_od) *has_od = has_order_digest[head];
        head = (head + 1) % PENDING_CAP;
        --count;
        return true;
    }
};

// ---- D31 clause A: STATUS-line proof-of-life cadence ---------------------------------------------
// Deliberately a SEPARATE knob from the sampling cadence (`[desync] every`, in sim STEPS): this
// decides how often "compared=N mismatching=0" gets WRITTEN, not how often a sample is taken.
//
// THE 2026-09-24 O4 RIG PROVED THE TWO MUST NOT SHARE A THRESHOLD. mp:D31 was opened because 5 of 6
// configuration-(1) shim runs read "armed, no sample reached a comparison" in mp_analyze.py -- which
// LOOKED like the detector had failed silently, but was a misdiagnosis of a REPORTING gap, not a
// comparison one:
//   * the 1 run that DID desync proved judge() was comparing correctly the entire time -- its
//     `*** DESYNC` line is unthrottled at the first mismatch (should_log_full, LOG_FIRST=4) and fired
//     at sample #11 (step 550), so a real mismatch was never missed;
//   * the other 5 runs never diverged (mp_analyze's silence read as "never compared" was actually
//     "compared cleanly, said nothing"), and the ONLY two channels that would have proven that are a
//     MATCH log line (silent unless `[desync] verbose=1`, by design -- a clean 20-minute match would
//     otherwise write ~3000 lines saying nothing happened) and this STATUS line;
//   * the pre-D31 threshold was 50 SAMPLES. A 2000-STEP harness/rig run at the shipped every=50
//     produces only 40 samples per peer -- below the threshold in every one of the 6 runs -- and the
//     match never reached a CLEAN mp_session_close either (the harness stops the process at
//     stop_step, which is not one of net_discovery.cpp's MATCH_END_REASONS), so match_end()'s own
//     rollup never fired either. Two independent proof-of-life paths, both silent, for a reason that
//     has nothing to do with whether the detector worked.
// DLL_PROCESS_DETACH was considered and REJECTED as the other place to flush a final rollup: mh.c's
// own detach arm is deliberately empty beyond the crash-handler unregister, on the record that
// teardown added there is how a detach path acquires a deadlock (a process TERMINATING is the
// common case, and EnterCriticalSection/WriteFile from under the loader lock is exactly that risk).
// So the fix is entirely in this cadence, lowered so a run an order of magnitude shorter than a full
// match still gets proof of life without depending on how the process exits.
inline constexpr int64_t SAMPLES_PER_STATUS_LINE = 10; // was 50 -- see above

inline bool status_line_due(int64_t samples_reported) {
    return samples_reported > 0 && (samples_reported % SAMPLES_PER_STATUS_LINE) == 0;
}

// ---- the live module (desync_watch.cpp) ---------------------------------------------------------
// Nothing above this line touches the game, a socket or Windows; everything below is the binding.

// Wire the module's log sink (the seam layer passes seam_log -> mh_net.log), before install().
void set_logger(void (*fn)(const char *));

// Read [desync] from `ini_path` (mh_net.ini), bind the manifest, register the FLAG_HASH handler.
// Returns 1 if armed. Idempotent-ish; call once from the seam arm.
int install(const char *ini_path);

// Start (or restart) sampling: zero the step counter, the ring, the pending queue and the one-shot
// notice, and report the outgoing match's rollup. Called from the session_begin_multi hook -- the
// step key is only meaningful relative to a session both peers entered together.
void session_reset();

// The outgoing match's rollup, WITHOUT restarting sampling. Called from the session boundary
// (mp_session_close, net_discovery.cpp) BEFORE the session directory closes, so a match's own
// `; [desync] match end: N mismatching / M compared` line lands in ITS directory. Until mp:RM1 the
// rollup was written only by session_reset(), i.e. at the NEXT session_begin_multi -- into the next
// match's directory, and never at all for the last match of a process. A rematch gate that asserts
// game 2's verdict needs game 2's rollup to exist (tools/check_rematch_residue.py). Clears the
// counters it reported, so the reset that follows at the following session_begin_multi has nothing to repeat;
// leaves g_running alone (a `Continue game` after an outcome keeps stepping and is not a new match).
void match_end();

// TL-HARN-CLEANCLOSE: stop sampling for the rest of this match, WITHOUT a rollup (the caller has just
// written one with match_end). Called only by the determinism harness's stop (MH_Session_HarnessStop):
// the process plays on until the runner kills it, and every STATUS line after the rollup would describe
// those post-match seconds -- with the counters match_end() zeroed -- while mp_analyze reads the LAST
// STATUS line as the peer's verdict. session_reset() (the next session_begin_multi) re-arms sampling.
void stop_sampling();

// One sim step, at the step's PRE-BODY boundary. Counts the step, folds this step's due-now order
// queue into the running order digest (D31 clause C -- UNCONDITIONALLY, every step, not gated by the
// sampling cadence: that is what lets a short order divergence that later re-converges still show up
// at the next sample) and, on the cadence, hashes, broadcasts and judges whatever has arrived. MAIN
// THREAD ONLY.
//
// TWO CALLERS, because no single per-step hook is live in both configurations that matter:
//   * a trampoline the seam layer installs on llm_strat_sim_step when the determinism harness did
//     NOT arm -- i.e. a shipped game, which is the case this feature exists for;
//   * the harness's own sim_step detour when it did, via on_sim_step_hashed below.
// The turn engine's `calls.sim_step` edge is NOT the hook: `llm_strat_sim_tick`, the mode-3
// catch-up loop that drives it, is not in the default promotion closure ("sim_tick is ORIGINAL"),
// so at ship that edge never executes.
void on_sim_step();

// The same boundary, with the per-region hashes and the state-only fold ALREADY COMPUTED by the
// caller in manifest order (`n` must be the live HASH_REGION_COUNT). Reuses them verbatim, so a run
// that was hashing anyway pays for one walk instead of two. Falls back to its own hash, once and
// loudly, if the offered manifest is not ours.
void on_sim_step_hashed(const uint64_t *per, int n, uint64_t state);

// True once the current match has seen at least one mismatching sample.
bool detected();

} // namespace mh::desync
