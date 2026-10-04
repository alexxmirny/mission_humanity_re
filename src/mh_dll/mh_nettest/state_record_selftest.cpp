//
// state_record_selftest.cpp -- `net_selftest.exe staterectest [--write <file>]`: the mp:D40 state
// recording, encoder to file, against an INDEPENDENT decoder written here from docs/state-record.md.
//
// WHAT IS PROVEN, AND AGAINST WHAT.
//   1. ENCODER ROUND TRIP (pure, desync/state_record.h). A synthetic multi-region step sequence --
//      sparse bytes, clusters inside the 8-byte coalescing gap, runs longer than a u16, empty steps --
//      is encoded exactly as the recorder encodes it, then decoded by decode() below, which shares NO
//      code with the encoder (its own CRC is the bitwise form, not the table). Every step's rebuilt
//      state must equal the input byte for byte; the keyframes must sit on the cadence; END must name
//      the last step, the step count and the file size before it.
//   2. CUT FILES. The same file truncated inside every chunk decodes to exactly the last complete
//      step, and that state is still right; a flipped payload byte stops decoding at the chunk before
//      it; header + "every chunk from some KEYF on" (the mp:D43 tail cut) decodes on its own.
//   3. THE LIVE PATH (desync/state_tracker + desync/state_recorder + the writer thread). Every hash
//      region is rebased onto synthetic memory (as inchashtest does), the recorder is driven through
//      the shared tracker for two matches -- with a region REBASED mid-match -- and the files it
//      writes are decoded and compared, step by step, against a hash of the live memory at that step.
//      Steps after match_end must not be recorded, and the second match must get its own file. Every
//      step rebuilt from match 1's file must also hash (VERDICT, per slice + state + combined) to what
//      the live memory hashed at that step -- the machinery of `--hash` below.
//
// `staterectest --hash <file> [--from N] [--to M] [--regions]` is NOT a test: it prints the
// determinism harness's per-step numbers for a REAL recording, for mp:D40 done_when (b).
//
// CROSS-CHECK WITH THE PUBLISHED READER. `staterectest --write <file>` copies match 1's real file
// to <file>; then `python tools/state_record.py info <file>` and `... at <file> <step>` must agree
// with what this suite printed (steps, keyframes, END). The C++ gate does not run Python.
//
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "addr/mh_regions.gen.h"
#include "desync/state_compress.h"
#include "desync/state_record.h"
#include "desync/state_recorder.h"
#include "desync/state_ring.h"
#include "desync/state_ring_core.h"
#include "desync/state_tracker.h"
#include "state/inc_state.h"
#include "state/region_view.h"

#include "desync/state_miniz_config.h"
#pragma warning(push, 0)
#include "../include/miniz/miniz.h"
#pragma warning(pop)

using namespace mh::state;
namespace srec  = mh::desync::srec;
namespace gzmod = mh::desync::gz;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

uint32_t g_rng = 0x6d2b79f5u;
uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ---- the independent decoder -------------------------------------------------------------------
uint32_t crc_bitwise(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xffffffffu;
}

uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

struct decoded {
    bool                              header_ok = false;
    uint16_t                          flags     = 0;
    uint64_t                          fp        = 0;
    uint32_t                          every     = 0;
    std::vector<std::string>          names;
    std::vector<uint32_t>             lens;
    std::vector<uint32_t>             steps;     // every step decoded, in order
    std::vector<std::vector<uint8_t>> states;    // the rebuilt state at steps[k]
    std::vector<uint32_t>             keyframes; // steps that carried a KEYF
    bool                              has_end  = false;
    uint32_t                          end_last = 0, end_count = 0;
    uint64_t                          end_bytes = 0, end_at = 0;
    const char                       *stop      = "eof"; // "eof" | "cut" | "crc" | "bad"
    uint32_t                          step_runs = 0;     // total runs over all STEP chunks
    uint32_t                          max_run   = 0;
};

// keep_states: store every step's full state (small synthetic files only); else the caller hashes.
decoded decode(const std::vector<uint8_t> &f, bool                                              keep_states,
               void (*on_step)(uint32_t, const std::vector<uint8_t> &, void *) = nullptr, void *ctx = nullptr) {
    decoded d;
    if (f.size() < 32 || rd32(&f[0]) != 0x5253484Du || rd16(&f[4]) != 1) return d;
    d.flags              = rd16(&f[6]);
    const uint32_t hlen  = rd32(&f[8]);
    d.fp                 = rd64(&f[12]);
    d.every              = rd32(&f[20]);
    const uint32_t nreg  = rd32(&f[24]);
    size_t         at    = 28;
    uint64_t       total = 0;
    for (uint32_t i = 0; i < nreg; ++i) {
        if (at + 5 > f.size()) return d;
        const uint32_t len = rd32(&f[at]);
        const uint8_t  nl  = f[at + 4];
        if (at + 5 + nl > f.size()) return d;
        d.names.emplace_back((const char *)&f[at + 5], nl);
        d.lens.push_back(len);
        total += len;
        at += 5 + nl;
    }
    if (at != hlen) return d;
    d.header_ok = true;
    std::vector<uint64_t> base(nreg);
    for (uint32_t i = 1; i < nreg; ++i) base[i] = base[i - 1] + d.lens[i - 1];
    std::vector<uint8_t> st;
    bool                 have = false;
    while (at < f.size()) {
        if (at + 12 > f.size()) {
            d.stop = "cut";
            break;
        }
        const uint32_t tag = rd32(&f[at]), n = rd32(&f[at + 4]), crc = rd32(&f[at + 8]);
        if (at + 12 + (uint64_t)n > f.size()) {
            d.stop = "cut";
            break;
        }
        const uint8_t *p = &f[at + 12];
        if (crc_bitwise(p, n) != crc) {
            d.stop = "crc";
            break;
        }
        if (tag == 0x4659454Bu) { // KEYF
            if (n != 4 + total) {
                d.stop = "bad";
                break;
            }
            const uint32_t s = rd32(p);
            st.assign(p + 4, p + 4 + total);
            have = true;
            d.keyframes.push_back(s);
            if (d.steps.empty() || d.steps.back() != s) {
                d.steps.push_back(s);
                if (keep_states) d.states.push_back(st);
                if (on_step) on_step(s, st, ctx);
            } else if (keep_states) {
                if (d.states.back() != st) { // a keyframe after its own STEP chunk must agree with it
                    d.stop = "bad";
                    break;
                }
            }
        } else if (tag == 0x50455453u) { // STEP
            if (!have || n < 8) {
                d.stop = "bad";
                break;
            }
            const uint32_t s = rd32(p), rc = rd32(p + 4);
            size_t         q  = 8;
            bool           ok = true;
            for (uint32_t r = 0; r < rc && ok; ++r) {
                if (q + 8 > n) {
                    ok = false;
                    break;
                }
                const uint16_t reg = rd16(p + q);
                const uint32_t off = rd32(p + q + 2);
                const uint16_t len = rd16(p + q + 6);
                q += 8;
                if (reg >= nreg || (uint64_t)off + len > d.lens[reg] || q + len > n) {
                    ok = false;
                    break;
                }
                memcpy(&st[(size_t)(base[reg] + off)], p + q, len);
                q += len;
                ++d.step_runs;
                if (len > d.max_run) d.max_run = len;
            }
            if (!ok || q != n) {
                d.stop = "bad";
                break;
            }
            d.steps.push_back(s);
            if (keep_states) d.states.push_back(st);
            if (on_step) on_step(s, st, ctx);
        } else if (tag == 0x20444E45u) { // END
            if (n != 16) {
                d.stop = "bad";
                break;
            }
            d.has_end   = true;
            d.end_last  = rd32(p);
            d.end_count = rd32(p + 4);
            d.end_bytes = rd64(p + 8);
            d.end_at    = at;
        } else {
            d.stop = "bad";
            break;
        }
        at += 12 + n;
    }
    return d;
}

// ---- 1 + 2: the pure encoder ---------------------------------------------------------------------
struct synth {
    std::vector<const char *>         names;
    std::vector<uint32_t>             lens;
    std::vector<std::vector<uint8_t>> mem;
    std::vector<uint8_t>              flat() const {
        std::vector<uint8_t> v;
        for (auto &m : mem) v.insert(v.end(), m.begin(), m.end());
        return v;
    }
};

// Exact changed runs between two images of one region, ascending (what inc::tracker gap 0 emits).
template <class F>
void exact_runs(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, F &&emit) {
    size_t i = 0;
    while (i < a.size()) {
        if (a[i] == b[i]) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < a.size() && a[j] != b[j]) ++j;
        emit((uint32_t)i, (uint32_t)(j - i));
        i = j;
    }
}

void mutate(synth &s, uint32_t step) {
    const int kind = (int)(step % 7);
    if (kind == 3) return; // an empty step, on purpose
    const int nm = 1 + (int)(rnd() % 12);
    for (int k = 0; k < nm; ++k) {
        auto          &m   = s.mem[rnd() % s.mem.size()];
        const uint32_t off = rnd() % (uint32_t)m.size();
        m[off] ^= (uint8_t)(1 + rnd() % 255);
        // A neighbour inside the coalescing gap, to be merged into one run.
        const uint32_t nb = off + 2 + rnd() % 8;
        if (nb < m.size()) m[nb] ^= 0x5a;
    }
    if (step % 37 == 0) { // a run longer than a u16 in the big region
        auto &m = s.mem[4];
        for (uint32_t q = 100; q < 100 + 70000 && q < m.size(); ++q) m[q] = (uint8_t)(m[q] + 1 + (q & 1));
    }
}

// Encode a whole synthetic match; returns the file and the input state at every step.
std::vector<uint8_t> encode_match(synth &s, uint32_t steps, uint32_t every, std::vector<std::vector<uint8_t>> &truth,
                                  uint32_t &exact_run_count) {
    srec::buf o = {};
    srec::put_header(o, 0, 0x1122334455667788ULL, every, s.names.data(), s.lens.data(), (int)s.names.size());
    std::vector<const uint8_t *> parts(s.mem.size());
    auto                         keyf = [&](uint32_t step) {
        for (size_t i = 0; i < s.mem.size(); ++i) parts[i] = s.mem[i].data();
        srec::put_keyf(o, step, parts.data(), s.lens.data(), (int)s.mem.size(), (step % 2) == 0);
        if ((step % 2) != 0) { // exercise the writer thread's late seal
            size_t at = o.len - srec::keyf_chunk_bytes(s.flat().size());
            srec::seal_chunk(o.p + at);
        }
    };
    exact_run_count = 0;
    keyf(1);
    truth.push_back(s.flat());
    for (uint32_t step = 2; step <= steps; ++step) {
        std::vector<std::vector<uint8_t>> prev = s.mem;
        mutate(s, step);
        srec::step_builder sb;
        sb.begin(o, step);
        for (size_t r = 0; r < s.mem.size(); ++r)
            exact_runs(prev[r], s.mem[r], [&](uint32_t off, uint32_t len) {
                ++exact_run_count;
                sb.run((int)r, off, len, s.mem[r].data() + off);
            });
        sb.end();
        if (srec::keyframe_due(step, 1, every)) keyf(step);
        truth.push_back(s.flat());
    }
    srec::put_end(o, steps, steps, o.len);
    std::vector<uint8_t> f(o.p, o.p + o.len);
    check("encoder never ran out of memory", !o.oom);
    o.release();
    return f;
}

bool states_match(const decoded &d, const std::vector<std::vector<uint8_t>> &truth, uint32_t first_step) {
    for (size_t k = 0; k < d.steps.size(); ++k) {
        const uint32_t s = d.steps[k];
        if (s < first_step || s - 1 >= truth.size() || d.states[k] != truth[s - 1]) return false;
    }
    return true;
}

// Chunk start offsets of a well-formed file (header excluded).
std::vector<size_t> chunk_starts(const std::vector<uint8_t> &f) {
    std::vector<size_t> v;
    size_t              at = rd32(&f[8]);
    while (at + 12 <= f.size()) {
        v.push_back(at);
        at += 12 + rd32(&f[at + 4]);
    }
    return v;
}

void test_encoder() {
    printf("--- encoder round trip (pure) ---\n");
    check("crc32(\"123456789\") == 0xCBF43926 (zlib's check value)", srec::crc32("123456789", 9) == 0xCBF43926u);
    check("crc32 chains like zlib.crc32(b, crc32(a))",
          srec::crc32("56789", 5, srec::crc32("1234", 4)) == srec::crc32("123456789", 9));
    check("table crc == bitwise crc", srec::crc32("state record", 12) ==
                                          crc_bitwise(reinterpret_cast<const uint8_t *>("state record"), 12));
    check("keyframe_due: first step", srec::keyframe_due(7, 7, 100));
    check("keyframe_due: first + every", srec::keyframe_due(107, 7, 100) && srec::keyframe_due(207, 7, 100));
    check("keyframe_due: not between", !srec::keyframe_due(100, 7, 100) && !srec::keyframe_due(8, 7, 100));
    check("keyframe_due: every=0 -> only the first", srec::keyframe_due(1, 1, 0) && !srec::keyframe_due(2, 1, 0));

    synth s;
    s.names = {"alpha", "b", "a_region_with_a_longer_name", "tiny", "big"};
    s.lens  = {4096, 3, 1000, 1, 90000};
    for (uint32_t l : s.lens) {
        s.mem.emplace_back(l);
        for (auto &x : s.mem.back()) x = (uint8_t)rnd();
    }
    const uint32_t                    STEPS = 240, EVERY = 50;
    std::vector<std::vector<uint8_t>> truth;
    uint32_t                          exact = 0;
    const std::vector<uint8_t>        f     = encode_match(s, STEPS, EVERY, truth, exact);

    const decoded d = decode(f, true);
    check("header decodes (magic, version, header_len == table end)", d.header_ok);
    check("header: flags 0, fp and keyframe_every as written",
          d.flags == 0 && d.fp == 0x1122334455667788ULL && d.every == EVERY);
    bool names_ok = d.names.size() == s.names.size();
    for (size_t i = 0; names_ok && i < s.names.size(); ++i)
        names_ok = d.names[i] == s.names[i] && d.lens[i] == s.lens[i];
    check("region table: names and lengths as written", names_ok);
    check("decoded to a clean EOF", strcmp(d.stop, "eof") == 0);
    check("every step 1..N decoded exactly once, in order", [&] {
        if (d.steps.size() != STEPS) return false;
        for (uint32_t k = 0; k < STEPS; ++k)
            if (d.steps[k] != k + 1) return false;
        return true;
    }());
    check("EVERY step's rebuilt state is byte-equal to the input", states_match(d, truth, 1));
    check("keyframes at 1, 51, 101, 151, 201 (first, then every 50)",
          d.keyframes == std::vector<uint32_t>({1, 51, 101, 151, 201}));
    check("END: last step, steps recorded, file bytes before END",
          d.has_end && d.end_last == STEPS && d.end_count == STEPS && d.end_bytes == d.end_at);
    check("runs 8 bytes apart were COALESCED (fewer file runs than exact runs)", d.step_runs < exact);
    check("a changed span longer than a u16 was split at 65535", d.max_run == srec::RUN_MAX);
    printf("  %zu bytes, %u STEP runs for %u exact runs, %zu keyframes\n", f.size(), d.step_runs, exact,
           d.keyframes.size());

    // ---- cut files -------------------------------------------------------------------------------
    printf("--- cut files ---\n");
    const std::vector<size_t> cs       = chunk_starts(f);
    int                       bad_cuts = 0, cuts = 0;
    for (size_t c = 0; c < cs.size(); c += 3) {
        const size_t next = (c + 1 < cs.size()) ? cs[c + 1] : f.size();
        for (size_t cut : {cs[c] + 5, cs[c] + 12, (cs[c] + next) / 2, next - 1}) {
            if (cut <= cs[c] || cut >= next) continue;
            ++cuts;
            std::vector<uint8_t> g(f.begin(), f.begin() + (ptrdiff_t)cut);
            const decoded        e     = decode(g, true);
            const decoded        whole = [&] {
                std::vector<uint8_t> h(f.begin(), f.begin() + (ptrdiff_t)cs[c]);
                return decode(h, true);
            }();
            const bool ok = strcmp(e.stop, "cut") == 0 && e.steps == whole.steps && states_match(e, truth, 1);
            if (!ok) ++bad_cuts;
        }
    }
    check("a file cut INSIDE any chunk decodes to exactly the chunks before it, every state still right",
          cuts > 100 && bad_cuts == 0);
    printf("  %d cut positions, %d wrong\n", cuts, bad_cuts);
    {
        std::vector<uint8_t> g   = f;
        const size_t         mid = cs[cs.size() / 2];
        g[mid + 12 + 3] ^= 0x40;
        const decoded e = decode(g, true);
        check("a flipped payload byte stops decoding at that chunk (crc), states before it right",
              strcmp(e.stop, "crc") == 0 && !e.steps.empty() && states_match(e, truth, 1) &&
                  e.steps.size() < STEPS);
    }
    {
        // mp:D43's tail cut: the header + every chunk from the KEYF of step 101 on.
        size_t kf = 0;
        for (size_t at : cs)
            if (rd32(&f[at]) == 0x4659454Bu && rd32(&f[at + 12]) == 101) kf = at;
        std::vector<uint8_t> g(f.begin(), f.begin() + (ptrdiff_t)rd32(&f[8]));
        g.insert(g.end(), f.begin() + (ptrdiff_t)kf, f.end());
        const decoded e = decode(g, true);
        check("header + chunks from a KEYF on (the tail cut) decodes: steps 101..N, states right",
              kf != 0 && strcmp(e.stop, "eof") == 0 && !e.steps.empty() && e.steps.front() == 101 &&
                  e.steps.back() == STEPS && states_match(e, truth, 101));
    }
}

// ---- 3: the live path ----------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> g_bufs;
std::vector<int>                  g_rids;

void bind_synthetic() {
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const int rid  = (int)HASH_REGIONS[i].rid;
        bool      seen = false;
        for (int r : g_rids) seen = seen || r == rid;
        if (seen) continue;
        uint32_t need = REGIONS[rid].size;
        for (int j = 0; j < HASH_REGION_COUNT; ++j)
            if ((int)HASH_REGIONS[j].rid == rid && HASH_REGIONS[j].offset + HASH_REGIONS[j].len > need)
                need = HASH_REGIONS[j].offset + HASH_REGIONS[j].len;
        g_rids.push_back(rid);
        g_bufs.emplace_back(need + 64u);
        for (auto &x : g_bufs.back()) x = (uint8_t)rnd();
        rebase((region_id)rid, (uint32_t)(uintptr_t)g_bufs.back().data(), need);
    }
}

void unbind_synthetic() {
    for (int rid : g_rids) unrebase((region_id)rid);
}

uint8_t *slice(int i) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(hash_base(i))); }

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ULL) {
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t live_hash() {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) h = fnv(slice(i), HASH_REGIONS[i].len, h);
    return h;
}

void live_mutate(uint32_t step) {
    if (step % 5 == 0) return; // unchanged steps must still get (empty) STEP chunks
    const int nm = 1 + (int)(rnd() % 20);
    for (int k = 0; k < nm; ++k) {
        const int      i = (int)(rnd() % HASH_REGION_COUNT);
        const uint32_t L = HASH_REGIONS[i].len;
        uint8_t       *p = slice(i);
        const uint32_t o = rnd() % L;
        p[o] ^= (uint8_t)(1 + rnd() % 255);
        if (o + 5 < L) p[o + 5] ^= 0x33;
    }
}

std::vector<std::string> g_log_paths; // every "STATE RECORD -> <path>:" the recorder logged
int                      g_log_lines = 0;
const char              *g_write_gz  = nullptr; // --write-gz <file>: copy match 3's .gz out for the Python cross-check
char                     g_gz_line[1024];       // the last "STATE RECORD compressed" / "compress FAILED" line (mp:D46)

void test_log(const char *fmt, ...) {
    char    line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    ++g_log_lines;
    fputs(line, stdout);
    if (strstr(line, "STATE RECORD compress")) memcpy(g_gz_line, line, sizeof(g_gz_line));
    for (const char *tag : {"STATE RECORD -> ", "STATE RING -> "}) {
        const char *k = strstr(line, tag);
        if (k) {
            k += strlen(tag);
            const char *e = strstr(k, ": ");
            if (e) g_log_paths.emplace_back(k, e);
        }
    }
}

std::vector<uint8_t> read_raw(const std::string &path) {
    std::vector<uint8_t> v;
    FILE                *fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return v;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    v.resize(n > 0 ? (size_t)n : 0);
    if (n > 0) fread(v.data(), 1, (size_t)n, fp);
    fclose(fp);
    return v;
}

// mp:D46: gunzip a whole gzip member (RFC 1952, no optional header fields, as the recorder writes it) with
// miniz's INFLATER -- a different code path from the deflate that wrote it. Empty on any malformation: bad
// magic, a trailer whose length or CRC-32 (bitwise, below) disagrees with the bytes that came out.
std::vector<uint8_t> gunzip(const std::vector<uint8_t> &z) {
    std::vector<uint8_t> out;
    if (z.size() < 18 || z[0] != 0x1f || z[1] != 0x8b || z[2] != 8 || z[3] != 0) return out;
    const uint8_t *t    = z.data() + z.size() - 8;
    const uint32_t crc  = t[0] | (t[1] << 8) | (t[2] << 16) | ((uint32_t)t[3] << 24);
    const uint32_t size = t[4] | (t[5] << 8) | (t[6] << 16) | ((uint32_t)t[7] << 24);
    out.resize(size);
    const size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), z.data() + 10, z.size() - 18, 0);
    if (got != size || crc_bitwise(out.data(), out.size()) != crc) out.clear();
    return out;
}

// Every reader below takes a path that may be a `.gz` (the recorder's compressed form) or a raw file.
std::vector<uint8_t> read_file(const std::string &path) {
    std::vector<uint8_t> v = read_raw(path);
    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) return gunzip(v);
    return v;
}

struct live_ctx {
    const std::vector<uint64_t> *truth; // hash of the live state at step k is truth[k-1]
    int                          bad = 0, seen = 0;
};

void on_live_step(uint32_t s, const std::vector<uint8_t> &st, void *vctx) {
    live_ctx *c = static_cast<live_ctx *>(vctx);
    ++c->seen;
    if (s == 0 || s > c->truth->size() || fnv(st.data(), st.size()) != (*c->truth)[s - 1]) ++c->bad;
}

void rm_tree(const std::string &dir) {
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

// ---- the VERDICT hash of whatever the slices hold (mp:D40 done_when (b)) --------------------------
// The determinism harness's per-step numbers, recomputed over the CURRENT contents of the (synthetic)
// slices: per-slice hash_slice with the harness's default masks, `combined` = FNV fold of every
// per-slice hash, `state` = the same fold over the non-excluded ones (harness.cpp on_sim_step). The
// two owned slices go through the raw emitters, as inchashtest's ref_stream does: the owner's stream
// is the same bytes, but its pointers are cached at first use and never point at a test buffer.
struct verdict {
    uint64_t per[HASH_REGION_COUNT];
    uint64_t combined, state;
};

// Which harness hash kind `--hash` reproduces: 1 = the FNV VERDICT walk (default), 2 = the
// incremental core's per-slice sums (tooling:TL-HARN-INCHASH), recomputed from scratch over the
// rebuilt slices -- what a `hash_kind=2` harness logs, folded the same way.
int g_hash_kind = 1;

void verdict_now(verdict &v) {
    uint64_t c = 1469598103934665603ULL, s = c;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const hash_region &r = HASH_REGIONS[i];
        if (g_hash_kind == 2) {
            v.per[i] = mh::state::inc::recompute_live(i, mh::state::inc::knobs{});
        } else if (owner_serves(r.rid, r.offset, r.len)) {
            hash_sink h(sink_mode::VERDICT);
            if (i == HIDX_ORDER_QUEUE) emit_order_queue(slice(i), r.len, h);
            else h.bytes(slice(i), r.len);
            v.per[i] = h.finish();
        } else {
            v.per[i] = hash_slice(i, true, true, true);
        }
        c = fnv(reinterpret_cast<const uint8_t *>(&v.per[i]), 8, c);
        if (!r.excluded) s = fnv(reinterpret_cast<const uint8_t *>(&v.per[i]), 8, s);
    }
    v.combined = c;
    v.state    = s;
}

// Rebuild every step of a recording INTO the bound slices and hash it. The file's region table must
// be this build's manifest (names + lengths), or nothing is hashed.
struct replay_ctx {
    uint32_t from, to;
    void (*emit)(uint32_t step, const verdict &v, void *ctx);
    void    *ctx;
    uint64_t hashed = 0;
    verdict  v;
};

void replay_step(uint32_t s, const std::vector<uint8_t> &st, void *vctx) {
    replay_ctx *c = static_cast<replay_ctx *>(vctx);
    if (s < c->from || s > c->to) return;
    size_t at = 0;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        memcpy(slice(i), st.data() + at, HASH_REGIONS[i].len);
        at += HASH_REGIONS[i].len;
    }
    verdict_now(c->v);
    ++c->hashed;
    c->emit(s, c->v, c->ctx);
}

bool table_is_manifest(const decoded &d) {
    if (!d.header_ok || (int)d.names.size() != HASH_REGION_COUNT) return false;
    for (int i = 0; i < HASH_REGION_COUNT; ++i)
        if (d.names[i] != HASH_REGIONS[i].name || d.lens[i] != HASH_REGIONS[i].len) return false;
    return true;
}

void test_live(const char *write_to) {
    printf("--- live path: shared tracker -> recorder -> writer thread -> file ---\n");
    namespace hub = mh::desync::state_hub;
    namespace rec = mh::desync::recorder;
    bind_synthetic();
    const uint32_t EVERY = 25;
    rec::set_compress(false); // matches 1 and 2 are read back RAW; match 3 below turns it on (mp:D46)
    check("recorder configure(state_record=1) says on", rec::configure(1, EVERY, 0xabcdef0123456789ULL, &test_log));
    check("the shared tracker allocates for it", hub::enable("staterectest", &test_log));
    hub::add_listener(rec::listener());

    // Before any session: nobody wants a step, the tracker is not touched.
    check("no session armed -> step() skips", !hub::step(1));

    uint32_t total = 0;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) total += HASH_REGIONS[i].len;

    for (int match = 1; match <= 2; ++match) {
        const uint32_t        STEPS = match == 1 ? 130 : 40;
        std::vector<uint64_t> truth;
        std::vector<verdict>  vtruth; // match 1: the VERDICT hashes of live memory, per step
        hub::reset();
        rec::axis_info ax = {false, 0};
        rec::session_start(ax);
        std::vector<uint8_t> moved;
        for (uint32_t s = 1; s <= STEPS; ++s) {
            if (s > 1) live_mutate(s);
            if (match == 1 && s == 60) { // rebase one region to a copy with a few different bytes
                const int      I   = HIDX_STRAT_PLAYERS;
                const uint32_t L   = HASH_REGIONS[I].len;
                const auto     rid = HASH_REGIONS[I].rid;
                moved.assign(slice(I), slice(I) + L);
                moved[3] ^= 0x77;
                moved[L - 2] ^= 0x01;
                rebase(rid, (uint32_t)(uintptr_t)moved.data(), L);
            }
            hub::step(s);
            truth.push_back(live_hash());
            if (match == 1) {
                vtruth.emplace_back();
                verdict_now(vtruth.back());
            }
        }
        const size_t paths_before = g_log_paths.size();
        rec::match_end();
        (void)paths_before;
        // After match_end nothing is recorded (a "Continue game"): the tracker is skipped.
        live_mutate(STEPS + 1);
        check("after match_end the recorder no longer wants steps (Continue game is not recorded)",
              !hub::step(STEPS + 1));
        if (match == 1) { // back to the real buffer for match 2
            for (size_t r = 0; r < g_rids.size(); ++r)
                if (g_rids[r] == (int)HASH_REGIONS[HIDX_STRAT_PLAYERS].rid)
                    rebase((region_id)g_rids[r], (uint32_t)(uintptr_t)g_bufs[r].data(),
                           (uint32_t)g_bufs[r].size() - 64u);
        }
        check("the recorder logged the file it opened", (int)g_log_paths.size() == match);
        if ((int)g_log_paths.size() != match) break;
        const std::string          path = g_log_paths.back();
        const std::vector<uint8_t> f    = read_file(path);
        live_ctx                   ctx;
        ctx.truth       = &truth;
        const decoded d = decode(f, false, on_live_step, &ctx);
        char          what[256];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "match %d: header decodes, %d regions, fp and cadence as configured",
                    match, HASH_REGION_COUNT);
        check(what, d.header_ok && (int)d.names.size() == HASH_REGION_COUNT && d.fp == 0xabcdef0123456789ULL &&
                        d.every == EVERY);
        bool tab = d.header_ok;
        for (int i = 0; tab && i < HASH_REGION_COUNT; ++i)
            tab = d.names[i] == HASH_REGIONS[i].name && d.lens[i] == HASH_REGIONS[i].len;
        _snprintf_s(what, sizeof(what), _TRUNCATE, "match %d: region table == HASH_REGIONS (names + lens)", match);
        check(what, tab);
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "match %d: steps 1..%u decoded, EVERY rebuilt state == the live memory at that step (%d bad)",
                    match, STEPS, ctx.bad);
        check(what, strcmp(d.stop, "eof") == 0 && ctx.seen == (int)STEPS && ctx.bad == 0 && d.steps.size() == STEPS &&
                        d.steps.front() == 1 && d.steps.back() == STEPS);
        std::vector<uint32_t> want_kf;
        for (uint32_t s = 1; s <= STEPS; s += EVERY) want_kf.push_back(s);
        _snprintf_s(what, sizeof(what), _TRUNCATE, "match %d: keyframes at 1 and every %u after", match, EVERY);
        check(what, d.keyframes == want_kf);
        _snprintf_s(what, sizeof(what), _TRUNCATE, "match %d: END names the last step, the count and the size", match);
        check(what, d.has_end && d.end_last == STEPS && d.end_count == STEPS && d.end_bytes == d.end_at &&
                        d.end_at + 28 == f.size());
        printf("  match %d: %s, %zu bytes (%u-byte state x %zu keyframes)\n", match, path.c_str(), f.size(), total,
               d.keyframes.size());
        if (match == 2)
            check("the second match in one folder got its OWN file (mh_match_state_2.bin)",
                  path.find("mh_match_state_2.bin") != std::string::npos);
        if (match == 1) {
            // done_when (b)'s machinery (`--hash`): rebuild each step INTO the slices and take the
            // harness's VERDICT hashes -- they must equal the ones taken over live memory then.
            struct cmp {
                const std::vector<verdict> *t;
                int                         bad = 0, n = 0;
            } cc{&vtruth};
            replay_ctx rc;
            rc.from = 1;
            rc.to   = STEPS;
            rc.ctx  = &cc;
            rc.emit = [](uint32_t s, const verdict &v, void *vc) {
                cmp *k = static_cast<cmp *>(vc);
                ++k->n;
                const verdict &w = (*k->t)[s - 1];
                if (v.state != w.state || v.combined != w.combined ||
                    memcmp(v.per, w.per, sizeof(v.per)) != 0)
                    ++k->bad;
            };
            check("match 1: the file's region table is this build's manifest", table_is_manifest(d));
            decode(f, false, replay_step, &rc);
            _snprintf_s(what, sizeof(what), _TRUNCATE,
                        "match 1: every step rebuilt from the file hashes (VERDICT, per slice + state + "
                        "combined) to what the live memory hashed then (%d of %d differ)",
                        cc.bad, cc.n);
            check(what, cc.n == (int)STEPS && cc.bad == 0);
        }
        if (match == 1 && write_to) {
            const bool ok = CopyFileA(path.c_str(), write_to, FALSE) != 0;
            check("--write: copied match 1's file", ok);
            if (ok)
                printf("  wrote %s -- cross-check: python tools/state_record.py info %s ; python "
                       "tools/state_record.py at %s 60\n",
                       write_to, write_to, write_to);
        }
    }

    // ---- mp:D46: match 3 with compression ON: only the .gz is left, and it decodes ----------------------
    {
        const uint32_t        STEPS = 60;
        std::vector<uint64_t> truth;
        hub::reset();
        rec::set_compress(true);
        g_gz_line[0] = 0;
        rec::session_start(rec::axis_info{false, 0});
        for (uint32_t s = 1; s <= STEPS; ++s) {
            if (s > 1) live_mutate(s);
            hub::step(s);
            truth.push_back(live_hash());
        }
        const size_t n_paths = g_log_paths.size();
        rec::match_end();
        check("match 3: the recorder logged its file", g_log_paths.size() == 3);
        check("match 3: the background compression finished", gzmod::wait_idle(60000));
        if (n_paths == 3 || g_log_paths.size() == 3) {
            const std::string raw = g_log_paths.back();
            const std::string gz  = raw + ".gz";
            check("match 3: the raw file is gone and only the .gz is left",
                  GetFileAttributesA(raw.c_str()) == INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesA(gz.c_str()) != INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesA((gz + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
            check("match 3: the log line reports raw bytes, gzip bytes and ms",
                  strstr(g_gz_line, "compressed ") && strstr(g_gz_line, " raw bytes -> ") && strstr(g_gz_line, " ms ") &&
                      strstr(g_gz_line, "removed"));
            if (g_write_gz && CopyFileA(gz.c_str(), g_write_gz, FALSE))
                printf("  wrote %s -- cross-check: python tools/state_record.py info %s\n", g_write_gz, g_write_gz);
            live_ctx ctx;
            ctx.truth       = &truth;
            const decoded d = decode(read_file(gz), false, on_live_step, &ctx);
            check("match 3: the gunzipped file decodes, every step equals the live memory, END exact",
                  strcmp(d.stop, "eof") == 0 && ctx.seen == (int)STEPS && ctx.bad == 0 && d.has_end &&
                      d.end_last == STEPS);
        }
        // A finished match leaves only <name>.gz; the next match in the folder must not reuse <name>.
        rec::set_compress(false);
        if (g_log_paths.size() >= 2) {
            DeleteFileA(g_log_paths[0].c_str()); // match 1's raw file (mh_match_state.bin)
            DeleteFileA(g_log_paths[1].c_str()); // match 2's raw file (mh_match_state_2.bin)
            // Stands in for match 1's finished .gz.
            HANDLE d1 = CreateFileA((g_log_paths[0] + ".gz").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (d1 != INVALID_HANDLE_VALUE) CloseHandle(d1);
        }
        hub::reset();
        rec::session_start(rec::axis_info{false, 0});
        for (uint32_t s = 1; s <= 3; ++s) {
            if (s > 1) live_mutate(s);
            hub::step(s);
        }
        rec::match_end();
        check("match 4: a name whose .gz exists is taken (mh_match_state.bin skipped -> _2)",
              g_log_paths.size() == 4 && g_log_paths.back().find("mh_match_state_2.bin") != std::string::npos);
    }
    unbind_synthetic();
}

// ---- mp:D46: the compress step itself ---------------------------------------------------------------
std::string tmp_dir() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char d[MAX_PATH];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "%smh_staterec_gz_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(d, nullptr);
    return d;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &v) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t at = 0;
    bool   ok = true;
    while (ok && at < v.size()) {
        const DWORD k = (DWORD)((v.size() - at) > (1u << 24) ? (1u << 24) : (v.size() - at));
        DWORD       w = 0;
        ok            = WriteFile(h, v.data() + at, k, &w, nullptr) && w == k;
        at += k;
    }
    CloseHandle(h);
    return ok;
}

bool exists(const std::string &p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

uint64_t file_size(const std::string &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// Half structured (what a recording looks like), half noise (what deflate cannot shrink), `n` bytes.
std::vector<uint8_t> mixed_bytes(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (i / 4096) % 2 ? (uint8_t)rnd() : (uint8_t)((i * 7) >> 3);
    return v;
}

// `staterectest --compress-child <raw>`: the process the interrupted-compress test kills.
int compress_child(const char *raw) {
    gzmod::result r;
    gzmod::compress_file(raw, &r);
    return r.ok ? 0 : 1;
}

void test_compress() {
    printf("--- mp:D46: gzip of a finished recording (state_compress) ---\n");
    const std::string dir = tmp_dir();

    // 1. Round trip over sizes that straddle the 1 MiB read chunk and the 256 KiB write buffer.
    for (size_t n : {(size_t)1, (size_t)4096, (size_t)(1u << 20), (size_t)(1u << 20) + 1, (size_t)(5u << 20) + 12345}) {
        const std::string          raw  = dir + "\\rt_" + std::to_string(n) + ".bin";
        const std::vector<uint8_t> data = mixed_bytes(n);
        char                       what[200];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "round trip %zu bytes: .gz gunzips to the original", n);
        check("test file written", write_file(raw, data));
        gzmod::result r;
        gzmod::compress_file(raw.c_str(), &r);
        check(what, r.ok && r.raw == n && r.comp == file_size(raw + ".gz") && read_file(raw + ".gz") == data);
        check("  the raw file is deleted, no .tmp is left", !exists(raw) && !exists(raw + ".gz.tmp"));
        DeleteFileA((raw + ".gz").c_str());
    }

    // 2. A compressible file shrinks.
    {
        const std::string    raw = dir + "\\structured.bin";
        std::vector<uint8_t> data(3u << 20);
        for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)((i % 977) < 40 ? i : 0);
        check("structured file written", write_file(raw, data));
        gzmod::result r;
        gzmod::compress_file(raw.c_str(), &r);
        check("a compressible file shrinks by 10x or more and round-trips",
              r.ok && r.comp * 10 < r.raw && read_file(raw + ".gz") == data);
        DeleteFileA((raw + ".gz").c_str());
    }

    // 3. Failure leaves nothing behind; a stale .gz.tmp from a killed run is replaced.
    {
        gzmod::result r;
        gzmod::compress_file((dir + "\\missing.bin").c_str(), &r);
        check("missing raw file -> not ok, no .gz and no .tmp made",
              !r.ok && !exists(dir + "\\missing.bin.gz") && !exists(dir + "\\missing.bin.gz.tmp"));
        const std::string raw  = dir + "\\stale.bin";
        const auto        data = mixed_bytes(300000);
        write_file(raw, data);
        write_file(raw + ".gz.tmp", std::vector<uint8_t>(777, 0xEE)); // what a kill leaves
        gzmod::compress_file(raw.c_str(), &r);
        check("a stale .gz.tmp is overwritten and the result is the right file",
              r.ok && !exists(raw + ".gz.tmp") && read_file(raw + ".gz") == data);
        DeleteFileA((raw + ".gz").c_str());
    }

    // 4. mp:D46 done_when (b): a process killed MID-compress leaves the raw file whole and no final .gz.
    {
        const std::string raw = dir + "\\kill.bin";
        // 48 MB, not more: the rerun check below holds the gzip AND the inflated copy in memory, and the
        // 32-bit ASan pass ran out of address space at 192 MB (light gate 2026-10-04). Noise at level 6
        // still takes long enough for the 4 MB .tmp probe to catch the child mid-compress.
        const size_t         N = 48u << 20;
        std::vector<uint8_t> data(N);
        for (auto &b : data) b = (uint8_t)rnd(); // noise: deflate cannot shrink it and runs at its slowest
        check("48 MB raw file written", write_file(raw, data));
        const uint32_t want = crc_bitwise(data.data(), data.size());
        data.clear();
        data.shrink_to_fit();

        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        char cmd[2 * MAX_PATH + 64];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "\"%s\" staterectest --compress-child \"%s\"", self, raw.c_str());
        STARTUPINFOA        si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        const bool          up = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                                &pi) != 0;
        check("compress child started", up);
        if (up) {
            bool        mid = false;
            const DWORD t0  = GetTickCount();
            while (GetTickCount() - t0 < 60000) {
                if (file_size(raw + ".gz.tmp") >= (4u << 20)) {
                    mid = WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
                    break;
                }
                if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT) break;
                Sleep(2);
            }
            TerminateProcess(pi.hProcess, 99); // the exit: no cleanup runs
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            check("the child was killed while the .gz.tmp was growing (still compressing)", mid);
            const std::vector<uint8_t> after = read_raw(raw);
            check("after the kill: the raw file is whole (size and CRC-32 unchanged)",
                  after.size() == N && crc_bitwise(after.data(), after.size()) == want);
            check("after the kill: no final .gz exists (a truncated .tmp is not a recording)", !exists(raw + ".gz"));
            // The next run recovers: it overwrites the stale temp, finishes, and removes the raw file.
            gzmod::result r;
            gzmod::compress_file(raw.c_str(), &r);
            const std::vector<uint8_t> back = read_file(raw + ".gz");
            check("a rerun after the kill completes: .gz decodes to the original, raw removed, no .tmp",
                  r.ok && !exists(raw) && !exists(raw + ".gz.tmp") && back.size() == N &&
                      crc_bitwise(back.data(), back.size()) == want);
        }
    }
    rm_tree(dir);
}

// ---- 4: the mp:D41 ring, pure core (desync/state_ring_core.h) --------------------------------------
// A long synthetic run over a small manifest: the byte bound holds on EVERY step (run-up budget while
// rolling, the whole cap during the tail, and the allocation never grows past base + cap + scratch),
// an outsized step re-keys instead of storing, and the serialised RING file decodes -- independently
// -- to exactly the window, every state equal to the input at that step.
namespace sring = mh::desync::sring;

struct ring_run {
    uint32_t              keep, tail, cap;
    uint32_t              steps, trigger_at, mismatch_at;
    int                   bad_bound  = 0; // steps whose held bytes exceeded their budget
    int                   bad_ram    = 0; // steps whose allocation exceeded base + cap + 2 scratch
    uint32_t              rekeys     = 0;
    uint32_t              budget_pop = 0;
    uint32_t              dump_last  = 0;
    std::vector<uint8_t>  file;
    std::vector<uint64_t> truth; // fnv of the flat state at step s is truth[s - 1]
};

bool bool_true() { return true; }

void ring_core_run(ring_run &rr) {
    synth s;
    s.names = {"r0", "r1_big", "r2"};
    s.lens  = {3000, 14000, 64};
    for (uint32_t l : s.lens) {
        s.mem.emplace_back(l);
        for (auto &x : s.mem.back()) x = (uint8_t)rnd();
    }
    uint64_t total = 0;
    for (uint32_t l : s.lens) total += l;
    sring::core   c;
    sring::config cfg = {rr.keep, rr.tail, rr.cap, rr.cap / 4};
    check("ring core: alloc", c.alloc(s.lens.data(), 3, cfg));
    std::vector<const uint8_t *> parts(3);
    auto                         key = [&](uint32_t step) {
        for (int i = 0; i < 3; ++i) parts[i] = s.mem[i].data();
        c.rekey(step, parts.data());
    };
    auto flat_hash = [&] {
        uint64_t h = 1469598103934665603ULL;
        for (auto &m : s.mem) h = fnv(m.data(), m.size(), h);
        return h;
    };
    key(1);
    rr.truth.push_back(flat_hash());
    bool done = false;
    for (uint32_t step = 2; step <= rr.steps && !done; ++step) {
        std::vector<std::vector<uint8_t>> prev = s.mem;
        const int                         nm   = (step % 11 == 0) ? 0 : 1 + (int)(rnd() % 30);
        for (int k = 0; k < nm; ++k) {
            auto &m = s.mem[rnd() % 3];
            m[rnd() % m.size()] ^= (uint8_t)(1 + rnd() % 255);
        }
        if (step % 997 == 0) // an outsized step: the whole big region rewritten (> the run-up budget)
            for (auto &x : s.mem[1]) x = (uint8_t)(x + 1);
        c.begin_step(step);
        for (int r = 0; r < 3; ++r)
            exact_runs(prev[r], s.mem[r], [&](uint32_t off, uint32_t len) { c.run(r, off, len, s.mem[r].data() + off); });
        const sring::push p = c.end_step();
        rr.truth.push_back(flat_hash());
        if (p == sring::push::rekey || p == sring::push::gap) key(step);
        else if (p == sring::push::tail_full) done = true;
        else if (p != sring::push::ok) check("ring core: no oom", false);
        const size_t budget = c.ph() == sring::phase::tail ? rr.cap : rr.cap - rr.cap / 4;
        if (c.used() > budget) ++rr.bad_bound;
        if (c.ram_bytes() > total + rr.cap + 2 * sring::SCRATCH_KEEP) ++rr.bad_ram;
        if (step == rr.trigger_at) {
            check("ring core: trigger from rolling", c.trigger(rr.mismatch_at));
            check("ring core: a second trigger is refused", !c.trigger(rr.mismatch_at));
        }
        if (c.tail_complete()) done = true;
    }
    rr.rekeys     = c.rekeys();
    rr.budget_pop = c.budget_pops();
    rr.dump_last  = c.last_step();
    srec::buf hdr = {};
    srec::put_header(hdr, srec::FLAG_RING, 0x5151515151515151ULL, 0, s.names.data(), s.lens.data(), 3);
    sring::frozen f = c.detach();
    check("ring core: detach leaves the core empty", !c.allocated() && c.ram_bytes() == 0);
    struct sinkv {
        static bool put(void *ctx, const void *p, size_t n) {
            auto *v = static_cast<std::vector<uint8_t> *>(ctx);
            v->insert(v->end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
            return true;
        }
    };
    check("ring core: serialize", sring::serialize(hdr, f, &sinkv::put, &rr.file));
    check("ring core: file_bytes() == the bytes serialised", sring::file_bytes(hdr, f) == rr.file.size());
    hdr.release();
    f.release();
}

struct hash_ctx {
    const std::vector<uint64_t> *truth;
    int                          bad = 0, seen = 0;
    uint32_t                     first = 0, last = 0, prev = 0;
    bool                         contiguous = true;
};

void on_hash_step(uint32_t s, const std::vector<uint8_t> &st, void *vctx) {
    hash_ctx *c = static_cast<hash_ctx *>(vctx);
    if (c->seen == 0) c->first = s;
    else if (s != c->prev + 1) c->contiguous = false;
    c->prev = c->last = s;
    ++c->seen;
    if (s == 0 || s > c->truth->size() || fnv(st.data(), st.size()) != (*c->truth)[s - 1]) ++c->bad;
}

void test_ring_core() {
    printf("--- mp:D41 ring core (pure): bounds + round trip ---\n");
    {
        // Generous cap: the window is exactly keep_steps before the trigger, then tail_steps after it.
        ring_run rr = {400, 120, 1u << 20, 20000, 19500, 19480};
        ring_core_run(rr);
        hash_ctx hc;
        hc.truth        = &rr.truth;
        const decoded d = decode(rr.file, false, on_hash_step, &hc);
        check("ring core (wide cap): run-up never above its budget, tail never above the cap", rr.bad_bound == 0);
        check("ring core (wide cap): RAM never above base + cap + scratch", rr.bad_ram == 0);
        check("ring core (wide cap): header decodes with flags RING", d.header_ok && d.flags == srec::FLAG_RING);
        check("ring core (wide cap): first chunk KEYF at the window's oldest step = trigger - keep",
              !d.keyframes.empty() && d.keyframes.size() == 1 && d.keyframes[0] == 19500 - 400);
        check("ring core (wide cap): steps contiguous, trigger - keep .. trigger + tail",
              hc.contiguous && hc.first == 19100 && hc.last == 19620 && hc.seen == 521);
        check("ring core (wide cap): EVERY rebuilt state == the input at that step", hc.bad == 0);
        check("ring core (wide cap): END: last step, count, bytes",
              d.has_end && d.end_last == 19620 && d.end_count == 521 && d.end_bytes == d.end_at &&
                  strcmp(d.stop, "eof") == 0);
        printf("  wide cap: %zu-byte file, steps %u..%u, %u re-key(s)\n", rr.file.size(), hc.first, hc.last, rr.rekeys);
    }
    {
        // Tight cap: the BYTES bite before the age does; still bounded, still exact, tail cut by the cap.
        ring_run rr = {5000, 5000, 16384, 20000, 19500, 19490};
        ring_core_run(rr);
        hash_ctx hc;
        hc.truth        = &rr.truth;
        const decoded d = decode(rr.file, false, on_hash_step, &hc);
        check("ring core (tight cap): held bytes never above the budget on any step", rr.bad_bound == 0);
        check("ring core (tight cap): RAM never above base + cap + scratch", rr.bad_ram == 0);
        check("ring core (tight cap): the byte budget evicted chunks before their age", rr.budget_pop > 0);
        check("ring core (tight cap): every outsized step (> the run-up budget) RE-KEYED instead of storing",
              rr.rekeys >= 20);
        check("ring core (tight cap): the tail stopped at the cap (short of 5000 steps)",
              rr.dump_last > 19500 && rr.dump_last < 19500 + 5000);
        check("ring core (tight cap): decodes, contiguous, every state exact",
              d.header_ok && strcmp(d.stop, "eof") == 0 && hc.contiguous && hc.bad == 0 && hc.seen > 1 &&
                  hc.last == rr.dump_last && d.has_end);
        printf("  tight cap: %zu-byte file, steps %u..%u, %u chunk(s) evicted by bytes\n", rr.file.size(), hc.first,
               hc.last, rr.budget_pop);
    }
}

// ---- 5: the mp:D41 ring, live (shared tracker -> state_ring -> background writer -> file) ---------
bool g_ring_live = false;
bool ring_live() { return g_ring_live; }

void test_ring_live(const char *write_to) {
    printf("--- mp:D41 ring live path: shared tracker -> state_ring -> writer thread -> file ---\n");
    namespace hub = mh::desync::state_hub;
    namespace rng = mh::desync::state_ring;
    g_rids.clear();
    g_bufs.clear();
    bind_synthetic();
    rng::settings st;
    check("state_ring=0 -> off", !rng::configure({0, 30, 10, 6144}, false, 1, &ring_live, &test_log));
    check("state_record=1 -> the ring stays off (the whole-match file already holds it)",
          !rng::configure(st, true, 1, &ring_live, &test_log));
    st.seconds = 1; // 50 steps + 250 slack = a 300-step window
    st.tail_s  = 1; // 50 steps of tail
    st.max_kb  = 1024;
    check("ring configure(state_ring=1, 1 s, tail 1 s) says on",
          rng::configure(st, false, 0x0123456789abcdefULL, &ring_live, &test_log));
    check("the shared tracker is there for it", hub::enable("staterectest ring", &test_log));
    hub::add_listener(rng::listener());
    const uint32_t KEEP = 300, TAIL = 50, BUDGET = 1024u * 1024u * 3u / 4u;

    auto wait_writer = [] {
        for (int i = 0; i < 500 && !rng::writer_idle(); ++i) Sleep(10);
        return rng::writer_idle();
    };

    // ---- match A: a desync; live gaps before it re-key the window ----------------------------------
    std::vector<uint64_t> truth;
    hub::reset();
    rng::session_start();
    int          bad_bound = 0, wanted_before_live = 0, dumped_at = 0;
    bool         second = true, after_dump_idle = true;
    const size_t paths0 = g_log_paths.size();
    for (uint32_t s = 1; s <= 800; ++s) {
        g_ring_live = !(s <= 20 || (s >= 300 && s <= 310));
        if (s > 1) live_mutate(s);
        const bool stepped = hub::step(s);
        truth.push_back(live_hash());
        if (s <= 20 && stepped) ++wanted_before_live;
        const auto &c = rng::core_view();
        if (c.used() > (c.ph() == mh::desync::sring::phase::tail ? 1024u * 1024u : BUDGET)) ++bad_bound;
        if (s == 695) check("the first mismatch triggers the ring", rng::on_first_mismatch(690));
        if (s == 700) second = rng::on_first_mismatch(699);
        if (!dumped_at && s > 695 && c.ph() == mh::desync::sring::phase::empty) dumped_at = (int)s;
        if (s == 760) after_dump_idle = !stepped;
    }
    check("not live (solo / lobby): the ring asks for no tracker update", wanted_before_live == 0);
    check("the ring's held bytes never exceeded its budget", bad_bound == 0);
    check("a second mismatch does NOT dump again (on_first_mismatch refuses)", !second);
    check("the dump happened when the tail was complete (trigger + 50 steps)", dumped_at == 695 + (int)TAIL);
    check("after the dump the ring asks for no more steps", after_dump_idle);
    rng::match_end();
    check("the background writer finished", wait_writer());
    check("match A: exactly ONE ring file logged", g_log_paths.size() == paths0 + 1);
    if (g_log_paths.size() == paths0 + 1) {
        const std::string          path = g_log_paths.back();
        const std::vector<uint8_t> f    = read_file(path);
        hash_ctx                   hc;
        hc.truth        = &truth;
        const decoded d = decode(f, false, on_hash_step, &hc);
        check("match A: file is mh_desync_state.bin", path.find("mh_desync_state.bin") != std::string::npos);
        check("match A: header decodes, flags RING, the configured fp, keyframe_every 0",
              d.header_ok && d.flags == srec::FLAG_RING && d.fp == 0x0123456789abcdefULL && d.every == 0);
        check("match A: region table == this build's manifest", table_is_manifest(d));
        check("match A: one KEYF, at the window's oldest step (trigger - 300)",
              d.keyframes.size() == 1 && d.keyframes[0] == 695 - KEEP);
        check("match A: steps contiguous from trigger - 300 to trigger + 50",
              hc.contiguous && hc.first == 695 - KEEP && hc.last == 695 + TAIL);
        check("match A: covers >= state_ring_s (50 steps) BEFORE the mismatching step 690 and the tail after it",
              hc.first + 50 <= 690 && hc.last >= 695 + TAIL);
        check("match A: EVERY rebuilt state == the live memory at that step", hc.bad == 0 && hc.seen == (int)(KEEP + TAIL + 1));
        check("match A: END names the last step, the count and the size",
              d.has_end && d.end_last == 695 + TAIL && d.end_count == KEEP + TAIL + 1 && d.end_bytes == d.end_at &&
                  d.end_at + 28 == f.size() && strcmp(d.stop, "eof") == 0);
        printf("  match A: %s, %zu bytes, steps %u..%u\n", path.c_str(), f.size(), hc.first, hc.last);
        if (write_to) {
            const bool ok = CopyFileA(path.c_str(), write_to, FALSE) != 0;
            check("--write-ring: copied match A's ring file", ok);
            if (ok)
                printf("  wrote %s -- cross-check: python tools/state_record.py info %s ; ... at %s 600\n", write_to,
                       write_to, write_to);
        }
    }

    // ---- match B: a clean match writes nothing --------------------------------------------------------
    hub::reset();
    rng::session_start();
    g_ring_live = true;
    for (uint32_t s = 1; s <= 400; ++s) {
        if (s > 1) live_mutate(s);
        hub::step(s);
    }
    check("match B (clean): the window slid to exactly 300 steps",
          rng::core_view().chunks() == KEEP && rng::core_view().base_step() == 400 - KEEP);
    rng::match_end();
    check("match B (clean): nothing written", wait_writer() && g_log_paths.size() == paths0 + 1);

    // ---- match C: the match ends inside the tail -> dumped there -------------------------------------
    truth.clear();
    hub::reset();
    rng::session_start();
    for (uint32_t s = 1; s <= 220; ++s) {
        if (s > 1) live_mutate(s);
        hub::step(s);
        truth.push_back(live_hash());
        if (s == 200) rng::on_first_mismatch(198);
    }
    rng::match_end();
    check("match C: the tail cut by match end was dumped", wait_writer() && g_log_paths.size() == paths0 + 2);
    if (g_log_paths.size() == paths0 + 2) {
        const std::string path = g_log_paths.back();
        hash_ctx          hc;
        hc.truth        = &truth;
        const decoded d = decode(read_file(path), false, on_hash_step, &hc);
        check("match C: its own file (mh_desync_state_2.bin), steps 1..220, every state exact",
              path.find("mh_desync_state_2.bin") != std::string::npos && hc.first == 1 && hc.last == 220 &&
                  hc.contiguous && hc.bad == 0 && d.has_end);
    }
    g_ring_live = false;
    unbind_synthetic();
}

// `staterectest --hash <file> [--from N] [--to M] [--regions] [--kind 1|2]`: NOT a test. Rebuild every step of a
// real recording and print the determinism harness's numbers for it, one line per step:
//     <step> <combined> <state>        (16 hex digits each; the file's own step axis)
//     R <step> <h0> <h1> ...           (--regions: the per-slice hashes, manifest order)
// For mp:D40 done_when (b): join to mh_harness.log's `<step> <clock> <combined> <state>` rows at
// process step = step_base + file step + the offset the recorder's `step axis` line names. Harness
// default masks only (mask_ctrl_group = mask_soldier_anim = mask_planets_gfx = 1).
int hash_mode(const char *path, uint32_t from, uint32_t to, bool regions) {
    const std::vector<uint8_t> f = read_file(path);
    const decoded              h = decode(std::vector<uint8_t>(f.begin(), f.begin() + (f.size() < 4096 ? f.size() : 4096)), false);
    if (!table_is_manifest(h)) {
        printf("staterectest --hash: %s is not a recording of THIS build's %d-slice manifest (header %s) -- refused\n",
               path, HASH_REGION_COUNT, h.header_ok ? "decodes, table differs" : "does not decode");
        return 2;
    }
    bind_synthetic();
    struct out {
        bool regions;
    } o{regions};
    replay_ctx rc;
    rc.from = from;
    rc.to   = to;
    rc.ctx  = &o;
    rc.emit = [](uint32_t s, const verdict &v, void *vc) {
        printf("%lu %08lX%08lX %08lX%08lX\n", (unsigned long)s, (unsigned long)(v.combined >> 32),
               (unsigned long)(v.combined & 0xffffffffu), (unsigned long)(v.state >> 32),
               (unsigned long)(v.state & 0xffffffffu));
        if (static_cast<out *>(vc)->regions) {
            printf("R %lu", (unsigned long)s);
            for (int i = 0; i < HASH_REGION_COUNT; ++i)
                printf(" %08lX%08lX", (unsigned long)(v.per[i] >> 32), (unsigned long)(v.per[i] & 0xffffffffu));
            printf("\n");
        }
    };
    const decoded d = decode(f, false, replay_step, &rc);
    unbind_synthetic();
    fprintf(stderr, "staterectest --hash: %s: %llu step(s) hashed, steps %lu..%lu decoded, stop=%s%s\n", path,
            (unsigned long long)rc.hashed, d.steps.empty() ? 0ul : (unsigned long)d.steps.front(),
            d.steps.empty() ? 0ul : (unsigned long)d.steps.back(), d.stop, d.has_end ? " (END present)" : " (no END)");
    return rc.hashed ? 0 : 1;
}

} // namespace

int run_staterectest(int argc, char **argv) {
    const char *write_to = nullptr, *hash_file = nullptr, *write_ring = nullptr;
    uint32_t    from = 0, to = 0xffffffffu;
    bool        regions = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--write") == 0 && i + 1 < argc) write_to = argv[++i];
        else if (strcmp(argv[i], "--write-ring") == 0 && i + 1 < argc) write_ring = argv[++i];
        else if (strcmp(argv[i], "--write-gz") == 0 && i + 1 < argc) g_write_gz = argv[++i];
        else if (strcmp(argv[i], "--hash") == 0 && i + 1 < argc) hash_file = argv[++i];
        else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) from = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) to = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--regions") == 0) regions = true;
        else if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) g_hash_kind = atoi(argv[++i]) == 2 ? 2 : 1;
    }
    for (int i = 2; i + 1 < argc; ++i)
        if (strcmp(argv[i], "--compress-child") == 0) return compress_child(argv[i + 1]);
    if (hash_file) return hash_mode(hash_file, from, to, regions);
    printf("=== staterectest (mp:D40 whole-match recording + mp:D41 ring round-trip) ===\n");
    // The recorder writes into MH_RunDir(); point the logs root at a private temp folder so the suite
    // leaves nothing behind (it must be set before anything in this process asks for MH_RunDir).
    char tmp[MAX_PATH], root[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%smh_staterectest_%lu", tmp, (unsigned long)GetCurrentProcessId());
    SetEnvironmentVariableA("MH_LOG_ROOT", root);

    test_encoder();
    test_live(write_to);
    test_compress();
    test_ring_core();
    test_ring_live(write_ring);

    rm_tree(root);
    printf("=== staterectest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
