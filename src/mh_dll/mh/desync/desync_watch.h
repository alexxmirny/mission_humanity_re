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
};

struct ring {
    ring_entry e[RING_CAP];
    int        head;   // next slot to write
    int        count;  // entries held (<= RING_CAP)
    uint32_t   newest; // highest step stored (0 = nothing yet)

    void clear() { memset(this, 0, sizeof(*this)); }

    void put(uint32_t step, uint64_t state, const uint64_t *per, int n) {
        ring_entry &s = e[head];
        s.step        = step;
        s.used        = true;
        s.state       = state;
        memcpy(s.per, per, sizeof(uint64_t) * (size_t)(n < 0 ? 0 : n));
        head = (head + 1) % RING_CAP;
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

// ---- notice / log throttling --------------------------------------------------------------------
// D21 (f): 21428 consecutive mismatching steps must produce ONE user-visible notice, not a storm.
// The log is throttled separately and more generously -- the FIRST mismatch is written in full (it
// carries the step, both hashes and the diverging region), then one summary per LOG_EVERY.
inline constexpr int LOG_FIRST = 4;   // the first N mismatches are written in full
inline constexpr int LOG_EVERY = 200; // after that, one rollup line every N

inline bool should_notify(int mismatches_so_far) { return mismatches_so_far == 1; }

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
    int         head, count;
    int         dropped;

    void clear() { memset(this, 0, sizeof(*this)); }

    void push(int from, const sample_wire &s) {
        if (count == PENDING_CAP) {
            head = (head + 1) % PENDING_CAP;
            --count;
            ++dropped;
        }
        const int t = (head + count) % PENDING_CAP;
        q[t]        = s;
        sender[t]   = from;
        ++count;
    }

    bool pop(int &from, sample_wire &out) {
        if (count == 0) return false;
        out  = q[head];
        from = sender[head];
        head = (head + 1) % PENDING_CAP;
        --count;
        return true;
    }
};

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

// One sim step, at the step's PRE-BODY boundary. Counts the step and, on the cadence, hashes,
// broadcasts and judges whatever has arrived. MAIN THREAD ONLY.
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
