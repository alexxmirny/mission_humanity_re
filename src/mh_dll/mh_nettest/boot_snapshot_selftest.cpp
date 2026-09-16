//
// boot_snapshot_selftest.cpp -- the LIB-BOOT post-cfg snapshot, watched to FAIL (tracker LIB-BOOT).
//
// THE CLAIM UNDER TEST: libmh can import a captured post-cfg snapshot and reach the same
// prototype-table state the original binary was in when the snapshot was taken -- and the comparison
// that says so is not vacuous.
//
// WHY THE SECOND HALF IS MOST OF THIS FILE. A snapshot oracle is the easiest kind of test to make
// pass for the wrong reason: hash a subset, compare it to itself, report green. Three things guard
// against that here, and only the first is the one people write:
//
//   1. ROUND TRIP. Poison memory, import, re-hash: the hash must come back to the captured value.
//   2. PER-BLOCK MUTATION -- every block, not one field. For each of the 23 blocks in turn, flip one
//      byte of the blob and assert the imported hash MOVES. A single "corrupt one field" arm proves
//      one block is watched; this proves NO BLOCK IS OUTSIDE THE COMPARISON, which is the property
//      "a pass is not a pass over an empty comparison" actually names.
//   3. `Progress` GETS AN EXPLICIT CONTENT CHECK ON TOP OF ITS HASH ARM. Progress (30,900 B) is
//      MF_VIEW-ONLY -- outside every hashed region -- so the DETERMINISM oracle is structurally
//      blind to it, and an item that leaned on "the hash would catch it"
//      would be leaning on the one hash that cannot. This file's hash is a different hash (it walks
//      the snapshot's own block table, which includes Progress), so arm 2 does cover it -- and it is
//      checked a SECOND time by direct memcmp anyway, because a single mechanism covering the case
//      the whole trap is about is not enough mechanism.
//
// WHAT THIS FILE CANNOT DO, stated so nobody reads more into a green run. It has no INIT.CFG and no
// game: the fixture blob is SYNTHESISED here, not captured from mh.exe. So this proves the
// import path, the schema guards, the ordering refusal and the coverage of the comparison. The
// clause that the imported state EQUALS THE ORIGINAL BINARY'S is an offline oracle run against a
// real capture -- two different claims, two different fixtures.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "state/boot_snapshot.h"
#include "state/host_bind.h" // pulls in libmh/include/libmh.h for libmh_region_bind

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

using namespace mh::state;
using namespace mh::state::boot;

// ---- the arena --------------------------------------------------------------------------------
//
// Every carried region is bound onto memory this process owns. That is not a convenience: the stock
// bases are game .bss addresses this console process cannot materialise (state_selftest.cpp's own
// banner has the measurement -- the loader and the CRT heap own that whole span before main runs).
// Binding is also what the STANDALONE host does, so the fixture exercises the real path rather than
// a hosted shortcut.
struct arena {
    uint8_t *mem = nullptr;
    size_t   len = 0;

    bool bind_blocks() {
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) len += BOOT_SNAPSHOT_BLOCKS[i].len;
        mem = static_cast<uint8_t *>(std::malloc(len));
        if (mem == nullptr) return false;
        std::memset(mem, 0, len);
        libmh_region_bind binds[BOOT_SNAPSHOT_BLOCK_COUNT];
        size_t            off = 0;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) {
            binds[i].region_id = static_cast<uint32_t>(BOOT_SNAPSHOT_BLOCKS[i].rid);
            binds[i].base      = mem + off;
            binds[i].size      = BOOT_SNAPSHOT_BLOCKS[i].len;
            binds[i].count     = 0;
            off += BOOT_SNAPSHOT_BLOCKS[i].len;
        }
        return libmh_bind_regions(binds, BOOT_SNAPSHOT_BLOCK_COUNT) == BOOT_SNAPSHOT_BLOCK_COUNT;
    }

    void fill(uint8_t v) { std::memset(mem, v, len); }
    ~arena() { std::free(mem); }
};

// Deterministic pseudo-content, so a "captured" blob has structure rather than a constant -- a
// constant-filled fixture would pass a mutation arm that a shifted-by-one importer also passes.
uint8_t pattern(size_t i) {
    return static_cast<uint8_t>((i * 131u + (i >> 8) * 17u + 7u) & 0xffu);
}

// The offset of a block's PAYLOAD inside a blob, and its length. Read out of the BLOB'S OWN header
// words rather than recomputed: canonical_len() is private to boot_snapshot.cpp, and reading the
// blob is what any external consumer has to do anyway -- so the arms below walk it the same way a
// third-party importer would, rather than through a second copy of the layout rule.
bool block_span(const uint8_t *blob, int idx, size_t *off, uint32_t *len) {
    size_t o = sizeof(blob_header);
    for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) {
        uint32_t l = 0;
        std::memcpy(&l, blob + o + 4, 4);
        if (i == idx) {
            *off = o + 8u;
            *len = l;
            return true;
        }
        o += 8u + l;
    }
    return false;
}

} // namespace

int run_boottest(int argc, char **argv);

// ---- THE OFFLINE ORACLE (done_when clause 1) ---------------------------------------------------
//
// `net_selftest boottest <blob>` imports a REAL capture -- one the game wrote at boot stage 9, whose
// bytes the original cfg parser produced -- into an arena this process allocated, and re-derives the
// canonical hash. The blob's header carries the value the SAME hash function produced in-binary over
// the ORIGINAL's live memory, so the comparison is: one hash function, two processes, two different
// memories, one of them filled by the parser and one by the import path.
//
// It is NOT circular, and the distinction is worth being precise about. The header hash is not a
// checksum of the file -- `import()` never validates against it, and mutating a payload byte does not
// make the import fail. It is a statement about the ORIGINAL BINARY'S STATE, recorded at the moment
// that state existed. Reproducing it from the blob in another process is the claim under test.
static int run_blob_oracle(const char *path) {
    printf("=== boottest --blob (LIB-BOOT: import a REAL capture and reproduce the original's "
           "hash) ===\n");
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        printf("  FAIL: cannot open %s\n", path);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint8_t     *blob = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(n)));
    const size_t rd   = std::fread(blob, 1, static_cast<size_t>(n), f);
    std::fclose(f);
    if (rd != static_cast<size_t>(n)) {
        printf("  FAIL: short read of %s\n", path);
        return 1;
    }

    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    printf("  %s: %ld bytes, %lu blocks, schema=%08lX, recorded hash=%08lX%08lX, text_ptrs "
           "in_arena=%lu out_arena=%lu\n",
           path, n, (unsigned long)h.base.block_count, (unsigned long)h.base.schema,
           (unsigned long)(h.base.content_hash >> 32), (unsigned long)(h.base.content_hash & 0xffffffffu),
           (unsigned long)h.text_ptrs_in_arena, (unsigned long)h.text_ptrs_out_arena);

    arena a;
    if (!a.bind_blocks()) {
        printf("  FAIL: could not bind the carried regions\n");
        return 1;
    }
    // POISON FIRST. Importing over zeroes would let a block that is never written still agree with a
    // zero-filled expectation; 0xCD agrees with nothing.
    a.fill(0xCD);
    const uint64_t poisoned = canonical_hash();
    ck(poisoned != h.base.content_hash, "the poisoned arena does not already hash to the captured value");

    const int rc = import(blob, static_cast<size_t>(n));
    ck(rc == BOOT_OK, "libmh imports the real capture");
    if (rc != BOOT_OK) printf("        import rc=%d\n", rc);

    const uint64_t mine = canonical_hash();
    printf("  libmh hash after import: %08lX%08lX\n", (unsigned long)(mine >> 32),
           (unsigned long)(mine & 0xffffffffu));
    ck(mine == h.base.content_hash,
       "libmh's prototype-table state hashes EQUAL to the original binary's at stage 9");

    // THE CAPTURE'S OWN RESTORE SELF-CHECK, asserted here rather than trusted. Both halves matter:
    // `changed > 0` says the deferred parse really wrote something (so the restore was not a no-op
    // over untouched memory -- the vacuous shape), and `mismatch == 0` says every one of those
    // regions came back to its pre-call bytes. Together they are the item's evidence that arming
    // the capture leaves the running game unperturbed.
    ck(h.restore_changed > 0,
       "the deferred parse actually modified the restore set (the check is not vacuous)");
    ck(h.restore_mismatch == 0, "every restored region came back to its pre-call bytes");

    // THE NEGATIVE ARM ON A REAL BLOB, per block. Same shape as the synthetic fixture's, run against
    // the capture that actually matters.
    {
        uint8_t *mut  = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(n)));
        int      seen = 0, missed = 0;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) {
            size_t   off = 0;
            uint32_t len = 0;
            if (!block_span(blob, i, &off, &len) || len == 0) {
                ++missed;
                continue;
            }
            std::memcpy(mut, blob, static_cast<size_t>(n));
            mut[off + len / 2u] ^= 0x5Au;
            a.fill(0xCD);
            if (import(mut, static_cast<size_t>(n)) != BOOT_OK) {
                ++missed;
                continue;
            }
            if (canonical_hash() != h.base.content_hash) {
                ++seen;
            } else {
                ++missed;
                printf("  FAIL: block %d (%s) -- a flipped byte did NOT move the hash\n", i,
                       BOOT_SNAPSHOT_BLOCKS[i].name);
            }
        }
        std::free(mut);
        ck(seen == BOOT_SNAPSHOT_BLOCK_COUNT && missed == 0,
           "every block of the REAL blob is inside the comparison");
        printf("  per-block mutation on the real blob: %d/%d caught\n", seen,
               BOOT_SNAPSHOT_BLOCK_COUNT);
    }

    std::free(blob);
    printf("=== boottest --blob: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

int run_boottest(int argc, char **argv) {
    // `boottest <blob>` runs the offline oracle against a real capture; bare `boottest` runs the
    // synthetic fixture that the gate carries (no game, no INIT.CFG, no rig).
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-' && std::strcmp(argv[i], "boottest") != 0) return run_blob_oracle(argv[i]);

    printf("=== boottest (LIB-BOOT: the post-cfg snapshot, and the arms that make it mean "
           "something) ===\n");

    arena a;
    if (!a.bind_blocks()) {
        printf("  FAIL: could not bind the %d carried regions\n", BOOT_SNAPSHOT_BLOCK_COUNT);
        return 1;
    }
    printf("  bound %d block(s), %lu bytes of arena\n", BOOT_SNAPSHOT_BLOCK_COUNT,
           (unsigned long)a.len);

    // ---- 1. a synthetic "capture" ---------------------------------------------------------------
    //
    // Fill the arena with structured content, then capture it. G_TEXT_PTRS is given entries that
    // really do point into the bound G_TEXT_BLOCK, because that is the property the normalisation
    // and the `pointed-into` capture guard are about -- a fixture of random bytes there would make
    // capture() refuse, correctly, and prove nothing.
    for (size_t i = 0; i < a.len; ++i) a.mem[i] = pattern(i);
    {
        uint8_t       *ptrs       = reinterpret_cast<uint8_t *>(live_base(RID_G_TEXT_PTRS));
        const uint32_t arena_base = live_base(RID_G_TEXT_BLOCK);
        const uint32_t arena_len  = live_size(RID_G_TEXT_BLOCK);
        const uint32_t n          = live_size(RID_G_TEXT_PTRS) / 4u;
        for (uint32_t i = 0; i < n; ++i) {
            // Two thirds inside the arena, one third deliberately outside -- both tallies are part
            // of what the capture reports and both paths need exercising.
            const uint32_t v =
                (i % 3u == 2u) ? 0x00401000u + i : (arena_base + (i * 37u) % arena_len);
            std::memcpy(ptrs + i * 4u, &v, 4);
        }
    }

    const size_t need = blob_size();
    uint8_t     *blob = static_cast<uint8_t *>(std::malloc(need));
    size_t       got  = 0;
    ck(capture(blob, need, &got) == BOOT_OK, "capture succeeds over a bound arena");
    ck(got == need, "capture writes exactly blob_size() bytes");

    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    ck(h.base.schema == schema_fingerprint(), "the blob carries this build's schema fingerprint");
    ck(h.base.block_count == static_cast<uint32_t>(BOOT_SNAPSHOT_BLOCK_COUNT), "block count recorded");
    ck(h.text_ptrs_in_arena > 0 && h.text_ptrs_out_arena > 0,
       "both text-pointer tallies are non-zero -- the fixture exercised both paths");
    printf("  capture: %lu bytes, hash=%08lX%08lX, text_ptrs in=%lu out=%lu\n", (unsigned long)got,
           (unsigned long)(h.base.content_hash >> 32), (unsigned long)(h.base.content_hash & 0xffffffffu),
           (unsigned long)h.text_ptrs_in_arena, (unsigned long)h.text_ptrs_out_arena);

    const uint64_t captured = h.base.content_hash;
    ck(canonical_hash() == captured, "the live hash equals the one recorded in the header");

    // ---- 2. THE ROUND TRIP -----------------------------------------------------------------------
    //
    // Poison first. Importing over memory that already holds the right answer proves nothing -- it is
    // the "compare a thing to itself" failure this file exists to avoid, and it would pass with an
    // import that wrote zero bytes.
    a.fill(0xCD);
    ck(canonical_hash() != captured, "poisoning the arena moves the hash (the compare is live)");
    ck(import(blob, got) == BOOT_OK, "import accepts the blob");
    ck(canonical_hash() == captured, "after import the hash is back to the captured value");

    // ---- 3. PER-BLOCK MUTATION: no block is outside the comparison -------------------------------
    //
    // For every block in turn: flip one byte of its payload in a COPY of the blob, import, and
    // require the hash to move. A block that survives this is a block the oracle is not watching.
    {
        uint8_t *mut  = static_cast<uint8_t *>(std::malloc(got));
        int      seen = 0, missed = 0;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) {
            size_t   off = 0;
            uint32_t len = 0;
            if (!block_span(blob, i, &off, &len) || len == 0) {
                ++missed;
                printf("  FAIL: block %d (%s) has no payload span\n", i,
                       BOOT_SNAPSHOT_BLOCKS[i].name);
                continue;
            }
            std::memcpy(mut, blob, got);
            // Mid-block rather than byte 0: byte 0 of a table is the first record's first field and
            // is the byte a lazy importer is most likely to get right by accident.
            mut[off + len / 2u] ^= 0x5Au;
            a.fill(0xCD);
            const int rc = import(mut, got);
            if (rc != BOOT_OK) {
                ++missed;
                printf("  FAIL: block %d (%s) mutation was REJECTED (rc=%d) rather than imported --"
                       " the arm cannot say whether the hash watches it\n",
                       i, BOOT_SNAPSHOT_BLOCKS[i].name, rc);
                continue;
            }
            if (canonical_hash() != captured) {
                ++seen;
            } else {
                ++missed;
                printf("  FAIL: block %d (%s, %u B) -- one flipped byte did NOT move the hash. This"
                       " block is not in the comparison.\n",
                       i, BOOT_SNAPSHOT_BLOCKS[i].name, len);
            }
        }
        std::free(mut);
        ck(missed == 0 && seen == BOOT_SNAPSHOT_BLOCK_COUNT,
           "EVERY block's one-byte mutation is caught (no block outside the comparison)");
        printf("  per-block mutation: %d/%d blocks caught\n", seen, BOOT_SNAPSHOT_BLOCK_COUNT);
    }

    // ---- 3b. PER-BLOCK CONTENT: every block is actually WRITTEN ----------------------------------
    //
    // THIS ARM EXISTS BECAUSE ARM 3 WAS MEASURED AND FOUND SHORT. Mutating the implementation so
    // import() silently SKIPPED the Progress block left arm 3 still reporting 23/23 caught: with the
    // block never written the arena keeps its poison, and poison-vs-mutated-blob is a hash
    // difference too. So arm 3 proves "this block is IN the hash", which is NOT the same claim as
    // "this block was IMPORTED" -- and a block quietly not applied is exactly the failure mode a
    // snapshot has. It gets its own arm rather than a footnote.
    //
    // Compare the LIVE bytes against the blob's payload for every RAW block. Two blocks are not raw
    // and are NAMED rather than silently skipped:
    //   G_TEXT_PTRS   -- stored as offsets and re-absolutised on import, so the live bytes are
    //                    deliberately not the blob's bytes. Covered by the hash arm (which walks the
    //                    same normalisation) plus the in/out-arena tallies.
    //   TLO_REGISTRY  -- a DERIVED block; the imported table lives in the module rather than being
    //                    written back as fabricated `char *`s. Covered by arm 7's lookups.
    {
        a.fill(0xCD);
        ck(import(blob, got) == BOOT_OK, "import (for the per-block content check)");
        int checked = 0, wrong = 0, excused = 0;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i) {
            const boot_snapshot_block &b = BOOT_SNAPSHOT_BLOCKS[i];
            if (b.rid == RID_G_TEXT_PTRS || b.rid == RID_TLO_REGISTRY) {
                ++excused;
                continue;
            }
            size_t   off = 0;
            uint32_t len = 0;
            block_span(blob, i, &off, &len);
            const uint8_t *live = reinterpret_cast<const uint8_t *>(live_base(b.rid));
            ++checked;
            if (std::memcmp(live, blob + off, len) != 0) {
                ++wrong;
                printf("  FAIL: block %d (%s, %u B) was NOT imported byte-for-byte\n", i, b.name,
                       len);
            }
        }
        ck(wrong == 0, "every RAW block's live bytes equal the blob's after import");
        ck(checked + excused == BOOT_SNAPSHOT_BLOCK_COUNT && excused == 2,
           "exactly the two non-raw blocks are excused, and both are named");
        printf("  per-block content: %d checked, %d excused (G_TEXT_PTRS, TLO_REGISTRY), %d wrong\n",
               checked, excused, wrong);
    }

    // ---- 4. `Progress` -- the explicit content check (G128) ---------------------------------------
    //
    // Progress is MF_VIEW-only, so no determinism oracle can see it. Arm 3 above already covers it
    // through THIS hash, which is a different hash; this is the independent second mechanism, and it
    // is deliberately a byte compare rather than another hash.
    {
        int pidx = -1;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i)
            if (BOOT_SNAPSHOT_BLOCKS[i].rid == RID_PROGRESS_00E162E4) pidx = i;
        ck(pidx >= 0, "Progress is a carried block at all");
        if (pidx >= 0) {
            size_t   off = 0;
            uint32_t len = 0;
            block_span(blob, pidx, &off, &len);
            a.fill(0xCD);
            ck(import(blob, got) == BOOT_OK, "import (for the Progress content check)");
            const uint8_t *live = reinterpret_cast<const uint8_t *>(live_base(RID_PROGRESS_00E162E4));
            ck(len == BOOT_SNAPSHOT_BLOCKS[pidx].len, "the Progress block is the whole region");
            ck(std::memcmp(live, blob + off, len) == 0,
               "Progress' imported bytes are byte-identical to the blob's -- checked directly, not "
               "through any hash");

            // And the check has to be able to FAIL, or it is decoration.
            uint8_t *mut = static_cast<uint8_t *>(std::malloc(got));
            std::memcpy(mut, blob, got);
            mut[off + len / 2u] ^= 0xFFu;
            a.fill(0xCD);
            ck(import(mut, got) == BOOT_OK, "import (mutated Progress)");
            ck(std::memcmp(live, blob + off, len) != 0,
               "a mutated Progress block IS caught by the direct content check");
            std::free(mut);
            a.fill(0xCD);
            ck(import(blob, got) == BOOT_OK, "re-import the clean blob");
        }
    }

    // ---- 5. the refusals -------------------------------------------------------------------------
    {
        ck(import(nullptr, 16) == BOOT_ERR_ARG, "a null blob is refused");
        ck(import(blob, sizeof(blob_header) - 1) == BOOT_ERR_ARG, "a blob shorter than the header");

        uint8_t *mut = static_cast<uint8_t *>(std::malloc(got));

        std::memcpy(mut, blob, got);
        mut[0] ^= 0xffu;
        ck(import(mut, got) == BOOT_ERR_MAGIC, "a wrong magic is refused");

        std::memcpy(mut, blob, got);
        blob_header bh;
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.format += 1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == BOOT_ERR_MAGIC, "an unknown format is refused");

        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.schema ^= 0x1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == BOOT_ERR_SCHEMA,
           "a blob captured against a different block table is refused");

        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.block_count += 1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == BOOT_ERR_SCHEMA, "a disagreeing block count is refused");

        std::memcpy(mut, blob, got);
        ck(import(mut, got - 1u) == BOOT_ERR_TRUNCATED, "a truncated blob is refused");

        // A block header that names a different region than the schema expects.
        std::memcpy(mut, blob, got);
        {
            uint32_t bogus = 0xffffffffu;
            std::memcpy(mut + sizeof(blob_header), &bogus, 4);
            ck(import(mut, got) == BOOT_ERR_UNKNOWN_RID, "a block naming no known region is refused");
        }

        // AND THE REFUSALS MUST NOT HAVE WRITTEN ANYTHING. A rejected import that had already
        // half-applied would leave the world part boot values and part poison with nothing able to
        // tell -- the same rule host_bind.cpp's bind_all follows.
        a.fill(0xCD);
        const uint64_t poisoned = canonical_hash();
        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.schema ^= 0x1u;
        std::memcpy(mut, &bh, sizeof(bh));
        import(mut, got);
        ck(canonical_hash() == poisoned, "a REFUSED import writes nothing at all");

        std::free(mut);
        a.fill(0xCD);
        ck(import(blob, got) == BOOT_OK, "re-import the clean blob");
    }

    // ---- 6. THE ORDERING GATE (trap a) -----------------------------------------------------------
    //
    // `Planets` is one region with a boot writer (slots 1..N, once) and a SESSION writer
    // (llm_strat_scenario_planet_clone, slot 0x1f, every session -- and it READS Planets[1].icon_index
    // it does not overwrite). G_TEXT_PTRS has the same split. So an import after a session has begun
    // would overwrite live state with boot values, and the refusal is a hard gate rather than a note
    // in a doc.
    {
        ck(!session_begun(), "no session has begun in this fixture yet");
        note_session_begun();
        ck(session_begun(), "the latch is set");
        a.fill(0xCD);
        const uint64_t poisoned = canonical_hash();
        ck(import(blob, got) == BOOT_ERR_SESSION_BEGUN,
           "an import AFTER a session has begun is refused (-4)");
        ck(canonical_hash() == poisoned, "and it wrote nothing");
        reset_session_latch_for_test();
        ck(import(blob, got) == BOOT_OK, "with the latch cleared the same blob imports");
    }

    // ---- 7. the tlo lookup, over the IMPORTED registry --------------------------------------------
    //
    // This is what closes sim/libtrans/sim_lt_cfg_planet.cpp's census site: the lookup was always
    // ours to do, it just had no data of its own. The original @0x004b9582 walks 8 entries with a
    // case-insensitive compare and RETURNS 1 for anything it does not find -- that fallthrough is
    // load-bearing (every unknown map gets Jungle), so it is asserted rather than assumed.
    {
        // The synthetic arena gave the registry pattern bytes, so the imported names are whatever
        // the pattern produced; what matters here is the SHAPE. Import a blob whose tlo block we
        // author directly.
        uint8_t *mut = static_cast<uint8_t *>(std::malloc(got));
        std::memcpy(mut, blob, got);
        int tidx = -1;
        for (int i = 0; i < BOOT_SNAPSHOT_BLOCK_COUNT; ++i)
            if (BOOT_SNAPSHOT_BLOCKS[i].rid == RID_TLO_REGISTRY) tidx = i;
        ck(tidx >= 0, "the tlo registry is a carried block");
        if (tidx >= 0) {
            size_t   off = 0;
            uint32_t len = 0;
            block_span(blob, tidx, &off, &len);
            ck(len == TLO_ENTRIES * TLO_ENTRY_LEN, "the tlo block is the DERIVED form, not 72 raw bytes");
            static const char *kNames[TLO_ENTRIES] = {"Jungle.tlo", "Kam256.tlo", "Metal256.tlo",
                                                      "Ruins256.tlo", "Ice.tlo", "Lava.tlo",
                                                      "Sand.tlo", "Snow.tlo"};
            // INDICES START AT 2, and that is the arm rather than a detail. The original's
            // fallthrough returns 1, and in the REAL table Jungle's index is also 1 -- so a fixture
            // that mirrored the real indices could not tell "matched Jungle" from "found nothing",
            // and the default arm below would pass on an implementation that never matches anything.
            for (uint32_t i = 0; i < TLO_ENTRIES; ++i) {
                uint8_t *rec = mut + off + i * TLO_ENTRY_LEN;
                std::memset(rec, 0, TLO_ENTRY_LEN);
                std::memcpy(rec, kNames[i], std::strlen(kNames[i]));
                rec[TLO_NAME_CAP] = static_cast<uint8_t>(i + 2u);
            }
            // The header's hash no longer describes this blob; import does not check it (the schema
            // does), which is deliberate -- content_hash is the ORACLE's value, not an integrity seal.
            ck(import(mut, got) == BOOT_OK, "import the authored tlo block");
            ck(tlo_index_for("Jungle.tlo") == 2, "an exact name resolves");
            ck(tlo_index_for("METAL256.TLO") == 4, "the compare is case-insensitive (upper)");
            ck(tlo_index_for("snow.tlo") == 9, "the compare is case-insensitive (lower)");
            ck(tlo_index_for("no_such_map.tlo") == 1,
               "an unknown name falls through to 1 (Jungle) -- the original's own default");
            ck(tlo_index_for(nullptr) == 1, "a null name does not crash and answers the default");
            // A prefix must NOT match: the original compares whole strings.
            ck(tlo_index_for("Jungle") == 1,
               "a PREFIX of a real name is not a match (it falls through to the default)");
        }
        std::free(mut);
    }

    std::free(blob);
    printf("=== boottest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
