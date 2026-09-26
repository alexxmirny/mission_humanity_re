//
// libref_host/main.cpp -- THE STANDALONE REFERENCE HOST (tracker LIB-REF).
//
// A headless console executable that links ONLY the standalone libmh artifact and drives it over
// the committed order+clock replay fixture (tools/data/fixtures/libref-replay-v1), hashing every
// step and comparing against the stream that fixture carries. It is two things at once and both
// matter: the ACCEPTANCE that libmh does not need mh.exe, and a zero-rig offline determinism
// harness that goes red on a one-byte mutation of any hashed region.
//
// ---- WHAT THE "NO INJECTION LAYER" VERDICT RESTS ON, AND WHAT IT DOES NOT --------------------
//
// NOT on this program linking. A lib holding every VA call site links perfectly well -- the thunk
// primitives are defined inside it (LIB-VA0 clause 5). The verdict comes from
// two instruments that measure the ARTIFACT:
//   * tools/gen_va_census.py   -- the SOURCE census: 2 mh::call:: sites, 0 needing work, 2 printed
//                                 keeps, 0 unrouted (LIB-VA0, closed by measurement).
//   * tools/scan_libmh_vas.py  -- the OBJECT-BYTE scan over the built standalone libmh.lib: 0
//                                 distinct original-image VAs / 0 immediates in code AND initialised
//                                 data, 125 adjudicated occurrences printed (LIB-REF-SPLIT).
// Cite those. Never cite the link for that clause.
//
// WHAT THE LINK **IS** GOOD FOR, measured the day this program first existed: a declared-never-
// defined symbol. Both instruments above are blind to one -- it is not a VA and it emits no bytes --
// and the standalone build had THREE (two rebind rows pointed at arm adapters
// that were either compiled out under MH_LIBMH_BUILD or defined with a different signature, and one
// libmh module called mh::en_build_ok(), which lives outside libmh's module set). That is why this
// program is wired into the build as a permanent gate rather than kept as a demo: it is linked with
// /WHOLEARCHIVE, so every object in libmh.lib must resolve, not merely the ones this main() reaches.
//
// ---- HOW ONE STEP IS DRIVEN, AND WHY IT NEEDS NO NEW ABI ------------------------------------
//
// The fixture's hashes are taken PRE-BODY of llm_strat_sim_step -- the harness's on_sim_step is an
// ENTRY detour, so it runs after the driver has advanced the clock and before the step executes.
// Reproducing that instant looked like it needed a new spine entry to split the driver. It does not,
// because both bodies on the path are translated with every out-edge indirected:
//
//   mh::sim::detail::frame(v, own, lt_frame_calls)      <- llm_strat_frame @0x0043ecfa
//        input_update      -> no-op            (the WALL; a replay reads no device)
//        pump              -> mh::lockstep::pump        (session mode != 3 here: not taken)
//        pace_time_tick    -> no-op            (the C4 pacing hook; no pacing detour standalone)
//        time_tick         -> our time_tick over a PINNED now  (pin_wallclock=1's role)
//        harness_sim_tick  -> the clock-track pin            (the C6 hook, doing exactly what
//                                                             harness.cpp's on_sim_tick does)
//        sim_tick          -> mh::lockstep::detail::sim_tick(state(), replay_calls, fixes())
//             replay_calls.sim_step -> inject this step's orders; hash; compare; libmh_sim_step()
//        render_present    -> no-op            (a NOTIFY record since SIMABI-NOTIFY)
//
// So the observation point is the same function entry the harness detours, reached through the same
// two original bodies, with zero arithmetic reimplemented on this side. The clock-track pin and the
// order injection are byte-for-byte what harness.cpp does at the same two places.
//
// ---- THE COMPARISON CHANNEL IS `state`, NEVER `combined` -------------------------------------
//
// The fixture's README states it with the measurement: `combined` folds the state_excluded()
// wall-clock/pacing regions and is a property of the RUN, so a record and its own replay differ on
// it while `state` is identical across four independent launches. This host compares `state` and the
// per-region R columns. Of the 61 manifest columns 15 are excluded; a headless host runs no
// renderer, no input pump and no lockstep transport, so the excluded clock/pacing columns are not
// expected to track a hosted run and are reported separately rather than folded into the verdict.
//
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "libmh.h"
#include "libmh_host_api.gen.h"
#include "libmh_tact_host_api.gen.h"
#include "libmh_host_in.h" // LIB-REF-IN: libmh_in_open -- the inbound-surface handshake

#include "addr/mh_regions.gen.h"
#include "lockstep/turn_engine.h"
#include "sim/libtrans/sim_lt_frame.h"
#include "sim/rng_trace.h" // C-prime: the RNG draw-sequence trace
#include "sim/sim_state.h"
#include "orders/order_queue.h" // mh::orders::set_suppress_enqueue -- the replay arm's suppression
#include "sim/sim_step.h"
#include "fp/x87.h"        // raw_x87_cw / raw_mxcsr -- the FPENV lines, symmetric with the harness
#include "state/host_in.h" // mh::libmh_in::trap_count/last_trap -- the refusal counter
// LIB-REF-LIVE: the importing host's view of the RECORDING's address space, for re-stamping carried
// pointers INTO bound regions. HOST-ONLY by construction -- the header #errors without this define,
// and lint_libmh_layering.py fails if anything under mh/ names it. libmh's zero-VA guarantee covers
// the ARTIFACT (scan_libmh_vas over libmh.lib) and is untouched: see the generator's banner.
#define MH_LIBREF_HOST_TU 1
#include "host_stock_bases.gen.h"
#include "state/nav_trailer.h"
#include "state/region_view.h"
#include "state/world_snapshot.h"

namespace {

// ================================================================================================
// diagnostics
// ================================================================================================

int g_fails = 0;

void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("  FAIL: ", stdout);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    ++g_fails;
}

// ---- the TRAPS ----------------------------------------------------------------------------
//
// Every host-table entry a strategic headless replay must never reach is bound to a NAMED trap
// rather than to a no-op. A no-op would let a missing service pass as silence; a trap makes the
// first fire a reportable fact with the entry's name on it. Fires are counted and printed, and a
// non-zero count is a RED -- which is the whole content of "the tact table gets trap impls that
// must never fire in a strategic replay".
struct trap_log {
    const char *names[64];
    long        hits[64];
    int         n = 0;

    void hit(const char *name) {
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(names[i], name) == 0) {
                ++hits[i];
                return;
            }
        }
        if (n < 64) {
            names[n] = name;
            hits[n]  = 1;
            ++n;
        }
    }
    long total() const {
        long t = 0;
        for (int i = 0; i < n; ++i) t += hits[i];
        return t;
    }
    void report(const char *table) const {
        if (n == 0) {
            std::printf("  %s traps: none fired\n", table);
            return;
        }
        std::printf("  %s traps: %ld fire(s) across %d entr(ies) -- THIS IS A RED\n", table,
                    total(), n);
        for (int i = 0; i < n; ++i) std::printf("      %-44s %ld\n", names[i], hits[i]);
    }
};

trap_log g_sim_traps;
trap_log g_tact_traps;

#define SIM_TRAP(name)  g_sim_traps.hit(name)
#define TACT_TRAP(name) g_tact_traps.hit(name)

} // namespace

// ================================================================================================
// the SIM host table -- 24 entries, every one of them REQUIRED (the generated table's `notify`
// column is 0 for all 24; the notify classes live in the TACT table, not here).
// ================================================================================================

namespace {

// ---- io: a vfs over stdio ---------------------------------------------------------------------
//
// Real, not a stub: the save driver is compiled into libmh and a host that traps these could not
// run one. Handles are 1-based indices into a small table so 0 stays "failed", which is the
// entry's own contract.
constexpr int MAX_FILES = 16;
FILE         *g_files[MAX_FILES];

int32_t h_vfs_open(const char *path, int32_t mode) {
    for (int i = 0; i < MAX_FILES; ++i) {
        if (g_files[i] != nullptr) continue;
        FILE *f = std::fopen(path, mode == MH_VFS_WRITE ? "wb" : "rb");
        if (f == nullptr) return 0;
        g_files[i] = f;
        return i + 1;
    }
    return 0;
}
FILE *file_of(int32_t h) {
    return (h >= 1 && h <= MAX_FILES) ? g_files[h - 1] : nullptr;
}
void h_vfs_close(int32_t h) {
    FILE *f = file_of(h);
    if (f == nullptr) return;
    std::fclose(f);
    g_files[h - 1] = nullptr;
}
int32_t h_vfs_read(int32_t h, void *dst, uint32_t n) {
    FILE *f = file_of(h);
    if (f == nullptr) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, n, f)); // a short read is REPORTED, not padded
}
int32_t h_vfs_write(int32_t h, const void *src, uint32_t n) {
    FILE *f = file_of(h);
    if (f == nullptr) return -1;
    const size_t w = std::fwrite(src, 1, n, f);
    return w == n ? static_cast<int32_t>(w) : -1; // a partial write is a failure, not a number
}
int32_t h_vfs_seek(int32_t h, int32_t off, int32_t whence) {
    FILE *f = file_of(h);
    if (f == nullptr) return -1;
    const int w = whence == 1 ? SEEK_CUR : (whence == 2 ? SEEK_END : SEEK_SET);
    return std::fseek(f, off, w) == 0 ? 0 : -1;
}
int32_t h_vfs_tell(int32_t h) {
    FILE *f = file_of(h);
    return f == nullptr ? -1 : static_cast<int32_t>(std::ftell(f));
}

// The packed resource banks are mh.rsr/mh.nam content this host does not carry. A replay that
// imports a step-0 world reads no asset; if one is asked for, that is the finding.
// ---- the ASSET VFS (plan doc 3.7: "vfs over stdio, asset lookup") -------------------------------
//
// The packed banks (mh.rsr/mh.nam) are a compressed archive format; this host does not parse one.
// It reads an EXTRACTED tree instead -- `--assets <dir>`, where <dir> holds the bank's contents at
// their bank-relative paths (tools: `python src/formats/unpack.py --src <game> --out <dir> --packs
// mh`). That is a real asset lookup, not a stub: the bytes are the game's own, and where they were
// unpacked from a bank rather than parsed out of one is a property of the HOST, which is exactly the
// kind of thing a host is allowed to decide.
//
// WITHOUT --assets THE TRAP STILL FIRES, deliberately. A replay that asks for an asset this host
// cannot supply is a finding, and silently answering "empty" is how the closed-default class hid for
// 289 steps. No answer is better than an invented one.
const char *g_assets_dir = nullptr;

// ---- --assets-absent: THE ONE ANSWER A BANK-LESS HOST IS ENTITLED TO GIVE (fork F4G) -----------
//
// THE MEASUREMENT THAT FORCED THIS. All three committed fixtures replay 5000/5000 ALL STEPS
// IDENTICAL and then FAIL, on an asset_read trap: the landing sequence asks for `init\H_3100.DMP`
// through `init\H_3107.DMP` (the AI players' scripted base layouts,
// sim/sim_load_base_layout_dmp.cpp). Those files ARE NOT IN THE RETAIL BANK -- mh.nam carries 112
// `init\H_*.DMP` entries and the highest is H_2507 -- so the GAME's own lookup misses them too, and
// the hosted arm answers exactly what an absent asset answers. The trap was firing on a question
// this host cannot answer from nothing ("is the miss mine or the bank's?") rather than on a gap.
//
// THE ANSWER IS DECLARED, NOT INVENTED, AND THE DECLARATION IS FALSIFIABLE. `--assets-absent <file>`
// names a list of bank paths measured to be absent from the retail bank
// (tools/data/libref_asset_dispositions.json, re-verified against a real mh.nam by the gate runner
// on any box that has the game). A name on the list answers a MISS, which is what both arms answer;
// a name NOT on the list still TRAPS, so the fail-closed property is untouched and a NEW asset
// request is still a finding. And the declaration can be caught lying in the one direction that
// matters: with `--assets` also given, a declared-absent name that turns out to EXIST in the tree
// is a refusal, not a read.
constexpr int MAX_ABSENT = 64;
const char   *g_absent[MAX_ABSENT];
int           g_absent_n    = 0;
char         *g_absent_text = nullptr; // owns the loaded file; the pointers above point into it
int           g_absent_hits = 0;

// Bank paths are compared case-insensitively and separator-insensitively, because the two spellings
// in play come from different places: the list is written by a Python tool from a .nam index, and
// the name the sim passes is composed by the game's own code.
bool same_bank_path(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

bool declared_absent(const char *name) {
    if (name == nullptr) return false;
    for (int i = 0; i < g_absent_n; ++i)
        if (same_bank_path(g_absent[i], name)) return true;
    return false;
}

// One name per line; `#` starts a comment; blank lines ignored. Deliberately the dumbest format
// that cannot be mis-parsed: the authority is the JSON the gate runner derives it from, and this is
// only the wire between them.
bool load_absent_list(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) {
        std::fclose(f);
        return false;
    }
    g_absent_text = static_cast<char *>(std::malloc(static_cast<size_t>(n) + 1));
    if (g_absent_text == nullptr) {
        std::fclose(f);
        return false;
    }
    const size_t got   = std::fread(g_absent_text, 1, static_cast<size_t>(n), f);
    g_absent_text[got] = '\0';
    std::fclose(f);
    char *p = g_absent_text;
    while (*p != '\0') {
        char *line = p;
        while (*p != '\0' && *p != '\n') ++p;
        if (*p == '\n') *p++ = '\0';
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
        while (*line == ' ') ++line;
        if (*line == '\0' || *line == '#') continue;
        // A TRUNCATED LIST IS A REFUSAL, NOT A SHORTER LIST. Silently dropping the tail would turn
        // every name past the cap back into a trap, and the run would then fail for a reason that
        // has nothing to do with the fixture -- the same shape as the unreadable-file case above.
        if (g_absent_n >= MAX_ABSENT) {
            std::printf("  [asset] --assets-absent has more than %d entries; this host caps the "
                        "declaration rather than reading part of it\n",
                        MAX_ABSENT);
            g_absent_n = 0;
            return false;
        }
        g_absent[g_absent_n++] = line;
    }
    return true;
}

// The fixture's replay_contract flag, reproduced -- see the arming site in main() for why this host
// carries it and why it was absent until 2026-09-11. Set from the command line before anything runs.
bool g_suppress_enqueue = true;

// ---- LIB-REF-LIVE: THE CARRIED-POINTER RE-STAMP ------------------------------------------------
//
// WHAT IT FIXES, measured rather than reasoned. The blob carries G_TEXT_PTRS -- 806 dwords, MF_VIEW,
// the localized text pool's index -- and every live entry points INTO G_TEXT_BLOCK at the RECORDING
// process's address. This host binds G_TEXT_BLOCK somewhere else, so those pointers name unmapped
// memory. The replay fixtures never noticed: the text pool is read when the sim FORMATS a message
// (building completion, advisor, damage notices), and a replay with enqueue suppression on generates
// none. The live loop generates them, and the first one faulted -- 0xC0000005 in
// mh::crt::detail::format_core<wchar_t> reading 0x0058696C, which is literally entry [n] of the
// carried table, during step 2001 of a run whose first 2000 steps were bit-identical.
//
// WHY THE BLOB'S OWN CENSUS DID NOT CATCH IT. `region_head_ptrs` counts dwords whose value EQUALS
// some carried region's live base. These point at 720 different INTERIOR offsets, so the census was
// never going to see one, and the eleven it does count are a different population.
//
// FOUR OUTCOMES PER ENTRY AND NO FIFTH. Anything this adjudication does not recognise REFUSES the
// import and names the entry -- it does not skip, clamp or zero it. A re-stamp that quietly left an
// entry pointing at the recording's address space would restore exactly the failure it exists to
// remove, with the fault moved to whenever that particular string is next formatted.
struct restamp_stats {
    int inside = 0, rdata = 0, zero = 0, bad = 0;
};

// THE POINTEE SIDE IS REGISTRY-DRIVEN, not one hardcoded region -- and that is a correction the
// instrument forced rather than a generalisation chosen up front. The first draft adjudicated only
// "inside G_TEXT_BLOCK", and its refusal immediately named G_TEXT_PTRS[168] = 0x00E589C0: entry 0xa8
// is the SCENARIO-PLANET NAME slot, which llm_strat_scenario_planet_clone publishes as a pointer to
// the UTF-16 scratch buffer STRAT_SCENARIO_PLANET_NAME_W (rid 758) -- a different carried region,
// at offset +0. So a carried text pointer may name ANY bound region, and the honest rule is "find
// the region whose ORIGINAL span contains this address, and rebase onto that region's binding".
//
// AMBIGUITY REFUSES. The registry has overlapping spans (gen_world_snapshot --check reports four
// overlapping block pairs), so an address can fall inside two regions' extents. Picking one would be
// a guess with a 50% failure rate that no oracle here could attribute, so a second match is a
// refusal like any other unadjudicated entry.
//
// Returns: 1 match -> *out_rid set; 0 -> no region; >1 -> ambiguous (out_rid untouched).
int stock_region_containing(uint32_t va, int *out_rid) {
    using namespace mh::state;
    int hits = 0;
    for (int rid = 0; rid < (int)RID_COUNT; ++rid) {
        const uint32_t base = libref::STOCK_BASE[rid];
        if (base == 0) continue;
        const uint32_t span = reach_of(static_cast<region_id>(rid));
        if (span == 0) continue;
        if (va >= base && va < base + span) {
            if (hits == 0) *out_rid = rid;
            ++hits;
        }
    }
    return hits;
}

// The host's own copies of the .rdata constants carried pointers name. Static storage duration, so
// the pointers handed to the sim outlive every step. The CONTENT is the real bytes read out of the
// frozen EN image at generation time, not an invented empty string -- if the pool's default ever
// reaches hashed state we are faithful by construction (measured: this one IS empty, and the raw
// bytes are recorded beside it in the generated header as the evidence for that).
const wchar_t *rdata_copy_for(uint32_t va) {
    for (int i = 0; i < libref::RDATA_CONSTANT_COUNT; ++i)
        if (libref::RDATA_CONSTANTS[i].va == va) return libref::RDATA_CONSTANTS[i].text;
    return nullptr;
}

bool g_restamp_text_ptrs = true; // --no-restamp-text-ptrs is the red arm; see the flag's help

bool restamp_text_ptrs() {
    using namespace mh::state;
    if (!g_restamp_text_ptrs) {
        std::printf("  [restamp] G_TEXT_PTRS re-stamp DISABLED (--no-restamp-text-ptrs) -- the "
                    "carried pointers still name the RECORDING's address space\n");
        return true;
    }
    uint32_t *const p       = ptr<uint32_t>(RID_G_TEXT_PTRS);
    const uint32_t  n       = reach_of(RID_G_TEXT_PTRS) / 4u;
    const uint32_t  stock_p = libref::STOCK_BASE[RID_G_TEXT_PTRS];
    if (p == nullptr || stock_p == 0 || n == 0) {
        fail("the G_TEXT_PTRS re-stamp has no table to work on (live %p, stock %08X, %u entries)",
             (void *)p, stock_p, n);
        return false;
    }

    restamp_stats st;
    int           pointees                   = 0; // distinct regions pointed into -- reported, not assumed to be 1
    bool          seen[mh::state::RID_COUNT] = {};
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v   = p[i];
        int            rid = -1;
        int            hits;
        if (v == 0) { // (iii) an unassigned slot -- carried as zero, kept as zero
            ++st.zero;
        } else if ((hits = stock_region_containing(v, &rid)) == 1) { // (i) interior of a bound region
            const uint8_t *const live = ptr<const uint8_t>(static_cast<region_id>(rid));
            if (live == nullptr) {
                ++st.bad;
                std::printf("  [restamp] G_TEXT_PTRS[%u] = %08X is inside rid %d, which this host "
                            "did not bind\n",
                            i, v, rid);
                continue;
            }
            p[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(live) +
                                         (v - libref::STOCK_BASE[rid]));
            ++st.inside;
            if (!seen[rid]) {
                seen[rid] = true;
                ++pointees;
            }
        } else if (hits > 1) { // ambiguous -- see stock_region_containing's note
            ++st.bad;
            if (st.bad <= 8)
                std::printf("  [restamp] G_TEXT_PTRS[%u] = %08X falls inside %d overlapping region "
                            "spans -- ambiguous, not guessed\n",
                            i, v, hits);
        } else if (const wchar_t *txt = rdata_copy_for(v)) { // (ii) an .rdata constant, by content
            p[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(txt));
            ++st.rdata;
        } else { // no fifth outcome
            ++st.bad;
            if (st.bad <= 8)
                std::printf("  [restamp] G_TEXT_PTRS[%u] = %08X is inside no carried region, is no "
                            "adjudicated .rdata constant, and is not zero\n",
                            i, v);
        }
    }
    std::printf("  [restamp] G_TEXT_PTRS: %u entries -- %d rebased into %d carried region(s), %d to "
                "a host .rdata copy, %d zero kept, %d UNADJUDICATED\n",
                n, st.inside, pointees, st.rdata, st.zero, st.bad);
    if (st.bad != 0) {
        fail("%d G_TEXT_PTRS entr(ies) fall outside every adjudicated class -- refusing the import "
             "rather than leaving a pointer into the recording's address space (B6.4: never "
             "silently shrink)",
             st.bad);
        return false;
    }
    // ANTI-VACUITY. A re-stamp that touched nothing and reported success is indistinguishable from a
    // correct one until the first format, which is exactly how this bug lived through every replay.
    if (st.inside == 0) {
        fail("the G_TEXT_PTRS re-stamp changed NOTHING -- 0 entries pointed into any carried "
             "region, so either the table is not what this host thinks it is or the blob did not "
             "carry it");
        return false;
    }
    return true;
}

// Bank paths use backslashes ("init\H_3101.DMP"); the host's filesystem takes either, but building
// the path by hand keeps the separator explicit rather than trusting the CRT to be lenient.
bool asset_path(const char *name, char *out, size_t cap) {
    if (g_assets_dir == nullptr || name == nullptr) return false;
    size_t k = 0;
    for (const char *p = g_assets_dir; *p && k + 1 < cap; ++p) out[k++] = *p;
    if (k + 1 < cap) out[k++] = '/';
    for (const char *p = name; *p && k + 1 < cap; ++p) out[k++] = (*p == '\\') ? '/' : *p;
    out[k] = '\0';
    return true;
}

// -> 1 the fixture declared this name absent from the bank, 0 it did not. Prints the verdict, and
// REFUSES the declaration if the asset tree contradicts it (see the --assets-absent block above).
int absent_by_declaration(const char *entry, const char *name, const char *resolved) {
    if (!declared_absent(name)) return 0;
    if (resolved != nullptr) {
        FILE *probe = std::fopen(resolved, "rb");
        if (probe != nullptr) {
            std::fclose(probe);
            SIM_TRAP(entry);
            std::printf("  [asset] %s(\"%s\") -- DECLARATION IS FALSE: --assets-absent says the bank "
                        "does not have it, and %s EXISTS. Re-derive the dispositions file.\n",
                        entry, name, resolved);
            return 0;
        }
    }
    ++g_absent_hits;
    std::printf("  [asset] %s(\"%s\") -- MISS, DECLARED ABSENT FROM THE BANK (the hosted arm misses "
                "it too; not a gap in this host)\n",
                entry, name);
    return 1;
}

int32_t h_asset_read(const char *name, void *dst, uint32_t cap) {
    char path[1024];
    if (!asset_path(name, path, sizeof(path))) {
        // The declaration is consulted BEFORE the refusal, and only when there is no tree to ask.
        if (absent_by_declaration("asset_read", name, nullptr)) return 0;
        SIM_TRAP("asset_read");
        std::printf("  [asset] asset_read(\"%s\", cap=%u) -- REFUSED (no --assets dir, and the name "
                    "is not in --assets-absent)\n",
                    name ? name : "?", cap);
        return -1;
    }
    if (absent_by_declaration("asset_read", name, path)) return 0;
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        // A bank MISS, not a host failure: the asset genuinely is not in mh.rsr (measured for
        // init/H_3101.DMP, which the AI asks for and the bank does not contain -- the highest H_
        // script shipped is H_2507). The hosted arm's lookup misses too, so answering 0 bytes read
        // is the same answer, not an invented one. No trap: a miss both arms share is not a gap.
        std::printf("  [asset] asset_read(\"%s\", cap=%u) -- MISS (not in the bank)\n", name, cap);
        return 0;
    }
    const size_t got = std::fread(dst, 1, cap, f);
    std::fclose(f);
    std::printf("  [asset] asset_read(\"%s\", cap=%u) = %zu byte(s)\n", name, cap, got);
    return static_cast<int32_t>(got);
}
int32_t h_asset_size(const char *name) {
    char path[1024];
    // The declaration is about THE BANK, not about an entry point, so it answers both queries. No
    // committed fixture reaches this one (measured: the size query the DMP loader makes is
    // asset_read(name, null, 0)), and the arm is here so that a fixture which does reach it is not
    // red for a reason the tool already knows the answer to.
    if (!asset_path(name, path, sizeof(path))) {
        if (absent_by_declaration("asset_size", name, nullptr)) return -1;
        SIM_TRAP("asset_size");
        return -1;
    }
    if (absent_by_declaration("asset_size", name, path)) return -1;
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        SIM_TRAP("asset_size");
        std::printf("  [asset] asset_size(\"%s\") -- NOT FOUND at %s\n", name, path);
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    std::printf("  [asset] asset_size(\"%s\") = %ld\n", name, n);
    return static_cast<int32_t>(n);
}

// ---- map-io: the six PRE-FORK (DEFER-BLOB) entries --------------------------------------------
//
// These are the entries the LIB-WORLD blob exists to retire: a host that imports a step-0 world has
// already been handed everything they produce. Trapping them is therefore not a gap -- it is the
// assertion that the blob really did replace them, and a fire would say the replay reached a parse
// the fixture was supposed to make unnecessary.
uint32_t h_cfg_ReadMapFile(void *) {
    SIM_TRAP("cfg_ReadMapFile");
    return 0;
}
void    h_map_ReadMap_pre(uint32_t) { SIM_TRAP("map_ReadMap_pre"); }
void    h_map_load_regions(int32_t) { SIM_TRAP("map_load_regions"); }
void    h_map_save_regions(int32_t) { SIM_TRAP("map_save_regions"); }
int32_t h_planet_tlo_load(uint32_t) {
    SIM_TRAP("planet_tlo_load");
    return 0;
}
void h_save_player_data(void *) { SIM_TRAP("save_player_data"); }

// ---- net: single peer, session mode != 3 -------------------------------------------------------
//
// recv answers "nothing yet" honestly rather than trapping: a non-blocking poll that reports empty
// is a CORRECT implementation for a host with no peers, and the turn engine is entitled to call it.
// send is a trap: with no peer there is nothing a send could mean, and a fire would say the replay
// entered a lockstep path this fixture never recorded.
int32_t h_transport_recv(int32_t *, void *, int32_t *len) {
    if (len) *len = 0;
    return 0;
}
void h_transport_send(void *, int32_t) { SIM_TRAP("transport_send"); }

// ---- string: THE REAL CP_ACP CODECS, and this is the item's own recorded clause ----------------
//
// Two of these feed HASHED sim state -- wide_to_local writes player_desc[].name inside the `players`
// slice and wide_to_local_bytes' result reaches _G_LLM_STRAT_PLAYERS[].name inside `strat_players`,
// both mutation-proven (simtest T10 / T12). So they are bound to the REAL
// WideCharToMultiByte(CP_ACP, ...) rather than to a portable equivalent: any other correct-looking
// conversion moves those two slices and fails this program's own hash comparison. Whether libmh
// should instead vendor a fixed codec is a DECLARED DIVERGENCE owned by LIB-FORK, and it is not
// decided here by picking one.
//
// CALL COUNTS ARE PRINTED. A replay that imports a step-0 world has already been handed both of
// those names inside the blob, so the expectation is zero calls -- and a claim that this host "binds
// the strict codecs correctly" is worth exactly as much as the number of times they ran. Printing it
// is the difference between a proven binding and a decorative one.
long g_codec_calls[4];

void *h_ansi_to_wide(void *dst, char *src) {
    ++g_codec_calls[0];
    if (dst == nullptr || src == nullptr) return dst;
    MultiByteToWideChar(CP_ACP, 0, src, -1, static_cast<LPWSTR>(dst), 1024);
    return dst;
}
void *h_ansi_to_wide_scratch(char *src) {
    ++g_codec_calls[1];
    static wchar_t scratch[512]; // the host's OWN shared buffer -- valid until the next call
    scratch[0] = 0;
    if (src) MultiByteToWideChar(CP_ACP, 0, src, -1, scratch, 512);
    return scratch;
}
char *h_wide_to_local(void *src, char *dst) {
    ++g_codec_calls[2];
    if (dst == nullptr || src == nullptr) return dst;
    const wchar_t *w = static_cast<const wchar_t *>(src);
    int            n = 0;
    while (w[n] != 0) ++n;
    const int got          = WideCharToMultiByte(CP_ACP, 0, w, n, dst, n, nullptr, nullptr);
    dst[got < 0 ? 0 : got] = '\0';
    return dst;
}
uint32_t h_wide_to_local_bytes(void *str) {
    ++g_codec_calls[3];
    if (str == nullptr) return 0;
    const wchar_t *w = static_cast<const wchar_t *>(str);
    int            n = 0;
    while (w[n] != 0) ++n;
    static char buf[1024];
    const int   got        = WideCharToMultiByte(CP_ACP, 0, w, n, buf, sizeof(buf) - 1, nullptr, nullptr);
    buf[got < 0 ? 0 : got] = '\0';
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf));
}

// ---- time / fatal / platform -------------------------------------------------------------------

// MONOTONIC is the whole contract. PACING ONLY (R8): nothing this returns may reach hashed state,
// which is why a plain counter is a legal implementation and a wall clock is not required.
uint32_t g_ticks;
uint32_t h_ticks_ms(void) {
    return ++g_ticks;
}

// IT MUST NOT RETURN -- the code after every call site assumes it is unreachable.
void h_fatal(int32_t status) {
    std::printf("  FAIL: libmh called fatal(%d) -- the replay cannot continue\n", (int)status);
    std::fflush(stdout);
    std::exit(3);
}

// 0x400 inert bytes: the media/CD diagnostic block a .sav's last member carries. The contents are
// measured inert (llm_game_load stops at its last wanted member), so a fixed placeholder satisfies
// the format -- and this entry staying REQUIRED is what lets mh.dll keep emitting the real bytes.
void *h_media_diag_block(void) {
    static unsigned char block[0x400];
    return block;
}

// ---- the input WALL ----------------------------------------------------------------------------
//
// "A LIB-REF replay host implements it by applying the frame's RECORDED orders and reading no device
// at all" -- the generated table's own words. This host goes one step further and applies them at
// the SIM_STEP entry instead, which is the instant harness.cpp's order_replay_inject uses, so the
// injected queue is identical at the hash rather than merely equivalent. Nothing is left for this
// entry to do, and it reads no device, which is the property that matters.
void h_apply_frame_input(void) {}

const libmh_host_api &sim_host_table() {
    static const libmh_host_api api = {
        h_apply_frame_input,
        h_asset_read,
        h_asset_size,
        h_vfs_close,
        h_vfs_open,
        h_vfs_read,
        h_vfs_seek,
        h_vfs_tell,
        h_vfs_write,
        h_cfg_ReadMapFile,
        h_map_ReadMap_pre,
        h_map_load_regions,
        h_map_save_regions,
        h_planet_tlo_load,
        h_save_player_data,
        h_transport_recv,
        h_transport_send,
        h_ansi_to_wide,
        h_ansi_to_wide_scratch,
        h_wide_to_local,
        h_wide_to_local_bytes,
        h_ticks_ms,
        h_fatal,
        h_media_diag_block,
    };
    return api;
}

// ================================================================================================
// the TACT host table -- 16 entries, ALL TRAPS. None may fire in a strategic replay.
// ================================================================================================

void t_convert_565_555(int16_t *) { TACT_TRAP("llm_gfx_convert_pixels_565_to_555"); }
void t_palette_565_555(void) { TACT_TRAP("llm_tlo_palette_convert_565_to_555"); }
void t_shade_table(void) { TACT_TRAP("llm_tlo_shade_table_build_tact"); }
void t_play_sample(uint32_t, void *, int32_t, int32_t, int32_t) {
    TACT_TRAP("llm_snd_play_matching_sample");
}
void    t_res_change(void) { TACT_TRAP("llm_gfx_apply_resolution_change"); }
void    t_window_res(int32_t, int32_t) { TACT_TRAP("llm_gfx_apply_window_resolution"); }
void    t_render_view(void) { TACT_TRAP("llm_tact_render_view"); }
int32_t t_view_size_mode(int32_t) {
    TACT_TRAP("llm_view_set_size_mode");
    return 0;
}
void    t_key_dequeue(uint32_t) { TACT_TRAP("llm_input_key_dequeue"); }
int32_t t_key_queue_empty(void) {
    TACT_TRAP("llm_input_key_queue_empty");
    return 1;
}
int32_t t_mouse_buttons(void) {
    TACT_TRAP("llm_input_mouse_buttons_get");
    return 0;
}
void     t_mouse_delta(void) { TACT_TRAP("llm_input_mouse_delta_pump"); }
uint8_t *t_res_ptr(char *) {
    TACT_TRAP("GetResourseFilePtr");
    return nullptr;
}
uint32_t t_res_size(char *) {
    TACT_TRAP("rsr_GetFileRealSize");
    return 0;
}
void t_fatal_cleanup(void) { TACT_TRAP("llm_fatal_cleanup"); }
void t_abort(int32_t status) {
    TACT_TRAP("utils_abort");
    h_fatal(status);
}

const libmh_tact_host_api &tact_host_table() {
    static const libmh_tact_host_api api = {
        t_convert_565_555,
        t_palette_565_555,
        t_shade_table,
        t_play_sample,
        t_res_change,
        t_window_res,
        t_render_view,
        t_view_size_mode,
        t_key_dequeue,
        t_key_queue_empty,
        t_mouse_buttons,
        t_mouse_delta,
        t_res_ptr,
        t_res_size,
        t_fatal_cleanup,
        t_abort,
    };
    return api;
}

void on_unbound(const char *name) {
    fail("host table entry is unbound: %s", name);
}

// ================================================================================================
// the arena -- every region bound onto memory THIS process owns
// ================================================================================================
//
// All RID_COUNT of them, not only the ones the blob carries: libmh_in_open refuses until
// libmh_bound_count() reaches libmh_region_count(), and "did every region get an answer?" is the
// question the bind count exists to answer. A zero-extent region is answered with (nullptr, 0),
// which bind_all accepts and which still marks the region BOUND -- an honest "there is nothing
// here", as opposed to leaving it looking forgotten.
//
// POISONED, on worldtest's precedent: importing over zeroes lets a block that is never written
// agree with a zero-filled expectation, and 0xCD agrees with nothing. Every byte this host ends up
// hashing therefore arrived from the blob or from a re-derive, never from a lucky initial value.
// THE MOVE_MICROSTEPS OVER-ALLOCATION PROBE (LIB-DISPATCH-SA / finding A, 2026-09-11).
//
// `_G_LLM_STRAT_MOVE_MICROSTEPS` is claimed at 96 bytes in the registry -- ONE heading row -- while
// the sim indexes it `[move_heading][MICROSTEPS_PER_HEADING]` with heading taken `% 24`, so the true
// extent is 24 * 96 = 2304. mh/addr/mh_addrs.gen.h's own comment on the symbol already warns that
// Ghidra's `[1][32]` typing is SHORT. Hosted, the over-index lands in mh.exe's real .bss where the
// rest of the table exists, so nothing is visibly wrong. In this host every region is a separate
// slice of one arena, so the over-index reads WHATEVER REGION WAS PLACED NEXT.
//
// These two knobs make that testable without touching libmh: `--microsteps-extent N` gives the
// region a larger allocation (so the over-index lands in memory THIS host controls), and
// `--microsteps-fill B` fills everything past the carried 96 bytes with B after the import. If the
// step-3 result moves when B moves, the sim is reading past byte 96 -- measured, not argued.
uint32_t g_microsteps_extent = 0;
int      g_microsteps_fill   = -1;

// THE ARENA POISON, as a knob rather than a constant -- an A/B against the whole class of
// "something reads arena bytes the blob never wrote". Every byte this host ends up reading that the
// fixture did not carry is this value: an uncarried region, or the tail of a region whose registered
// size is shorter than what the sim actually indexes (the MOVE_MICROSTEPS class). If the replay's
// first divergence MOVES when this changes, such a read exists and matters; if it does not move, the
// entire class is excluded in one measurement. 0xCD is the default because it agrees with nothing.
int g_arena_poison = 0xCD;

// INTER-REGION PADDING, the other half of the same A/B. Poison alone only catches an over-read that
// lands in the 0-15 bytes of alignment slack after a region; one that runs further lands in the NEXT
// REGION'S CARRIED BYTES, which no poison value changes. Padding every region apart moves what a
// long over-read finds without moving anything the fixture carries, so pad + poison together cover
// short claims of any length. If the divergence is unmoved under both, no cross-region read is in it.
uint32_t g_arena_pad = 0;

struct arena {
    uint8_t *mem = nullptr;
    size_t   len = 0;
    int      n   = 0;

    static uint32_t extent_of(int rid) {
        const uint32_t r = mh::state::reach_of(static_cast<mh::state::region_id>(rid));
        if (rid == static_cast<int>(mh::state::RID_STRAT_MOVE_MICROSTEPS) &&
            g_microsteps_extent > r)
            return g_microsteps_extent;
        return r;
    }

    bool bind_all_regions() {
        const int count = static_cast<int>(libmh_region_count());
        for (int i = 0; i < count; ++i)
            len += ((extent_of(i) + 15u) & ~15u) + g_arena_pad;
        mem = static_cast<uint8_t *>(std::malloc(len));
        if (mem == nullptr) return false;
        std::memset(mem, g_arena_poison, len);

        libmh_region_bind *binds =
            static_cast<libmh_region_bind *>(std::malloc(sizeof(libmh_region_bind) * count));
        if (binds == nullptr) return false;
        size_t off = 0;
        for (int i = 0; i < count; ++i) {
            const uint32_t sz  = extent_of(i);
            binds[i].region_id = static_cast<uint32_t>(i);
            binds[i].base      = sz ? (mem + off) : nullptr;
            binds[i].size      = sz;
            binds[i].count     = 0;
            off += ((sz + 15u) & ~15u) + g_arena_pad;
        }
        const int rc = libmh_bind_regions(binds, static_cast<size_t>(count));
        std::free(binds);
        n = count;
        return rc == count;
    }
    ~arena() { std::free(mem); }
};

// ================================================================================================
// the fixture
// ================================================================================================

struct blob {
    uint8_t *p = nullptr;
    size_t   n = 0;
    ~blob() { std::free(p); }
};

bool slurp(const char *path, blob *out) {
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        fail("cannot open %s", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->p         = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(n) + 1));
    const size_t r = std::fread(out->p, 1, static_cast<size_t>(n), f);
    std::fclose(f);
    out->p[r] = 0;
    out->n    = r;
    return r == static_cast<size_t>(n);
}

// The recording: a 4-byte header (magic 'MHOR' + version) then N records of {uint32 step, 68-byte
// order}. Read as raw bytes -- see libmh_submit_order's note on why this is a copy and not a decode.
constexpr uint32_t ORDERS_MAGIC = 0x524f484du; // 'MHOR'
constexpr size_t   ORDER_BYTES  = 0x44;
constexpr size_t   REC_BYTES    = 4 + ORDER_BYTES;
constexpr int      QUEUE_CAP_   = 300; // llm_strat_order[300] -- CMP [QUEUE_COUNT],0x12c

// THE TAIL-CLEAR, hoisted out of the injector for LIB-REF-LIVE (2026-09-11).
//
// NOW REDUNDANT (2026-09-25, mp:D33 follow-up): the hash treats the dead slots as zeros itself
// (mh::orders::emit_region via local()), and harness.cpp's clear is gone. Kept only because this
// host owns its own memory, not the game's, so it is harmless; the history below is why it existed.
//
// It is harness.cpp's `order_queue_tail_clear()`, and the reason it is a free function here is the
// reason it is an UNCONDITIONAL step-level call there: the clear runs in BOTH arms of every harnessed
// run, not only the ones that inject. `order_queue` is hashed whole -- 300 slots of 0x44 -- while only
// [0, count) is live, so leaving the dead slots as process history let two arms that agreed on every
// live order disagree on the hash the moment `count` dipped below an earlier high-water mark. A LIVE
// replay injects nothing at all, so had this stayed inside inject() the standalone live mode would
// have been the ONLY arm in either host that never cleared -- exactly the asymmetry that made the
// fourth fixture's record-vs-replay residue go broad.
//
// FROM THE EFFECTIVE COUNT, not from `injected`: the injector sets the count only when it wrote
// something, so a step that injects nothing still owes dispatch the previous records. CLAMPED,
// because the count has been MEASURED above 300 and an unclamped length would be negative.
// HASH-INPUT BEGIN libref_prehash_writes (tools/data/hash_input_epoch.json)
void order_queue_tail_clear(int injected) {
    uint8_t *const q = mh::state::ptr<uint8_t>(mh::state::RID_STRAT_ORDER_QUEUE);
    int32_t *const c = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
    if (q == nullptr || c == nullptr) return;
    int live = *c;
    if (live < 0) live = 0;
    if (live > QUEUE_CAP_) live = QUEUE_CAP_;
    std::memset(q + static_cast<size_t>(live) * ORDER_BYTES, 0,
                (static_cast<size_t>(QUEUE_CAP_) - static_cast<size_t>(live)) * ORDER_BYTES);
    // LIB-REF step-5000, count-ledger tag 16: the count this step BEGINS with, at the same point in
    // the step as the hosted arm's note (after inject + tail-clear, before the hash). It is the
    // ledger's per-step opening balance -- everything the body then does to the count is a tag 10..15
    // entry, and the two must add up to the next step's opening balance. In LIVE mode `injected` is
    // always 0, which is itself the reading: every order in that queue was put there by the sim.
    mh::sim::rng_trace_add_note(16u, (uint32_t)injected, 0u, 0u, 0u, (uint32_t)*c, 0u);
}

struct recording {
    const uint8_t *recs = nullptr;
    uint32_t       n    = 0;
    uint32_t       cur  = 0;
    // LIB-REF-SOAK: the injector's own evidence -- see the readback in inject().
    int arrivals = 0, set_ok = 0, no_set = 0;

    bool open(const blob &b) {
        if (b.n < 8) return false;
        uint32_t magic = 0;
        std::memcpy(&magic, b.p, 4);
        if (magic != ORDERS_MAGIC) return false;
        recs = b.p + 8;
        n    = static_cast<uint32_t>((b.n - 8) / REC_BYTES);
        return true;
    }
    uint32_t step_of(uint32_t i) const {
        uint32_t s = 0;
        std::memcpy(&s, recs + static_cast<size_t>(i) * REC_BYTES, 4);
        return s;
    }
    // harness.cpp's order_replay_inject, reproduced STATEMENT FOR STATEMENT over the host's own
    // bound pointers. Three details are load-bearing and all three come from reading that function,
    // not from reasoning about it:
    //
    //   1. IT OVERWRITES FROM INDEX 0 AND SETS THE COUNT. It does not append. `order_queue` is a
    //      NON-EXCLUDED hashed region, 20400 bytes, hashed whole -- so the queue's tail matters, and
    //      the tail is whatever earlier steps left there. The step-0 blob seeds it.
    //   2. THE COUNT IS WRITTEN ONLY WHEN n > 0. A step with no records leaves the count exactly as
    //      the previous step's dispatch left it.
    //   3. IT CAPS AT QUEUE_CAP records and drops the rest.
    //
    // THIS REPLACED AN APPEND-AT-THE-COUNT VERSION, and the reason is worth keeping. That version
    // was justified by "the dispatcher is count-gated and zeroes the count, so a step always begins
    // at 0, so appending writes the same bytes". MEASURED FALSE: from step ~392 the count climbed
    // past QUEUE_CAP and every further submit was refused. The premise was never checked, and the
    // fix is not to check it -- it is to stop depending on it, because the harness does not.
    //
    // libmh_submit_order is deliberately NOT used here. Its declared contract is "feed one record
    // into the pending set", which is a different operation from what the recording replays; the
    // entry stays implemented and exercised by the selftest, and the replay reproduces the
    // instrument it is replaying.
    static constexpr int QUEUE_CAP = 300; // llm_strat_order[300] -- CMP [QUEUE_COUNT],0x12c

    int inject(uint32_t step) {
        while (cur < n && step_of(cur) < step) ++cur;
        uint8_t *const q = mh::state::ptr<uint8_t>(mh::state::RID_STRAT_ORDER_QUEUE);
        int32_t *const c = mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
        if (q == nullptr || c == nullptr) {
            fail("step %u: the order queue regions are unbound", step);
            return 0;
        }
        int k = 0;
        for (uint32_t j = cur; j < n && step_of(j) == step && k < QUEUE_CAP; ++j, ++k)
            std::memcpy(q + static_cast<size_t>(k) * ORDER_BYTES,
                        recs + static_cast<size_t>(j) * REC_BYTES + 4, ORDER_BYTES);
        if (k > 0) {
            *c = k;
            ++arrivals;
            // THE READBACK, the same measurement harness.cpp's injector makes and for the same
            // reason: fixture_replay's recorder guard admits a `0 -> n` fixture only against
            // evidence that the count was actually SET, never against a comment claiming it. A
            // tally of intentions would not notice a write to the wrong address or an unbound
            // region; reading the count back does.
            if (*c == k)
                ++set_ok;
            else
                ++no_set;
        }
        order_queue_tail_clear(k);
        return k;
    }
};
// HASH-INPUT END libref_prehash_writes

// The committed hash stream: "<step> <clock> <combined> <state>" lines, and "R <step> <h0..hN>".
// Parsed into flat arrays indexed by step-1.
struct stream {
    uint64_t *state = nullptr;
    uint64_t *clock = nullptr;
    uint64_t *per   = nullptr; // [steps][ncols], only for steps that carry an R line
    bool     *has_r = nullptr;
    uint32_t  steps = 0;
    int       ncols = 0;

    ~stream() {
        std::free(state);
        std::free(clock);
        std::free(per);
        std::free(has_r);
    }
};

uint64_t hex64(const char *s) {
    uint64_t v = 0;
    for (int i = 0; i < 16 && s[i]; ++i) {
        const char c = s[i];
        const int  d = (c >= '0' && c <= '9')   ? c - '0'
                       : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                       : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                : -1;
        if (d < 0) break;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    return v;
}

bool parse_stream(const blob &steps_txt, const blob &regions_txt, uint32_t want_steps, stream *out) {
    out->steps = want_steps;
    out->ncols = mh::state::HASH_REGION_COUNT;
    out->state = static_cast<uint64_t *>(std::calloc(want_steps, sizeof(uint64_t)));
    out->clock = static_cast<uint64_t *>(std::calloc(want_steps, sizeof(uint64_t)));
    out->has_r = static_cast<bool *>(std::calloc(want_steps, sizeof(bool)));
    out->per   = static_cast<uint64_t *>(
        std::calloc(static_cast<size_t>(want_steps) * out->ncols, sizeof(uint64_t)));
    if (!out->state || !out->clock || !out->has_r || !out->per) return false;

    uint32_t seen = 0;
    for (const char *p = reinterpret_cast<const char *>(steps_txt.p); p && *p;) {
        const char *nl = std::strchr(p, '\n');
        if (*p >= '0' && *p <= '9') {
            const uint32_t st = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
            const char    *q  = std::strchr(p, ' ');
            if (q && st >= 1 && st <= want_steps) {
                ++q;
                out->clock[st - 1] = hex64(q);
                q                  = std::strchr(q, ' ');
                if (q) q = std::strchr(q + 1, ' '); // skip `combined`
                if (q) out->state[st - 1] = hex64(q + 1);
                ++seen;
            }
        }
        p = nl ? nl + 1 : nullptr;
    }
    if (seen != want_steps) {
        fail("hash_steps.txt carried %u usable lines, expected %u", seen, want_steps);
        return false;
    }

    for (const char *p = reinterpret_cast<const char *>(regions_txt.p); p && *p;) {
        const char *nl = std::strchr(p, '\n');
        if (p[0] == 'R' && p[1] == ' ') {
            const char    *q  = p + 2;
            const uint32_t st = static_cast<uint32_t>(std::strtoul(q, nullptr, 10));
            if (st >= 1 && st <= want_steps) {
                q = std::strchr(q, ' ');
                for (int c = 0; c < out->ncols && q; ++c) {
                    out->per[static_cast<size_t>(st - 1) * out->ncols + c] = hex64(q + 1);
                    q                                                      = std::strchr(q + 1, ' ');
                }
                out->has_r[st - 1] = true;
            }
        }
        p = nl ? nl + 1 : nullptr;
    }
    return true;
}

} // namespace

// ================================================================================================
// the replay
// ================================================================================================

namespace {

struct replay_ctx {
    recording    *rec        = nullptr;
    const stream *want       = nullptr;
    const double *clocks     = nullptr;
    uint32_t      step       = 0; // steps COMPLETED, mirroring harness.cpp's g_step
    uint32_t      limit      = 0;
    int           mismatches = 0;
    int           first_bad  = 0;
    uint32_t      poke_step  = 0;
    int           poke_col   = -1;

    // ---- THE RESUME, and it is a property of WHERE the snapshot was taken -----------------------
    //
    // The world blob is captured INSIDE frame 1, at llm_strat_sim_step's entry -- so frame 1's clock
    // advance has ALREADY HAPPENED in it: the imported `game_clock` is clocks[0], the value the
    // driver would compute. Replaying frame 1 from that state makes the driver's own gate
    // (`delta = total - clock; if (0 < delta)`) see delta == 0 and take no step, which is correct
    // arithmetic over a state that is one statement further along than the driver expects.
    //
    // So frame 1 is RESUMED, not restarted: game_clock is rolled back to a value below clocks[0] so
    // the gate fires, and the two doubles the driver then recomputes are RESTORED to the imported
    // ones at the observation point, before anything reads them. Restored rather than recomputed
    // because the snapshot IS their true value -- reconstructing the pre-frame clock as
    // `clocks[0] - delta` and letting the subtraction run again would make step 1 depend on a
    // round trip through two floating-point operations for no gain. `game_time_delta` is
    // state_excluded() but it is NOT inert: sim_step passes it straight into ai_players_tick, so a
    // delta that is merely close would change the AI's frame and diverge later, quietly.
    //
    // Nothing observes the rolled-back value: the driver overwrites game_clock before calling
    // sim_step, and the hook below restores both before the hash. From frame 2 on the pin is the
    // harness's own and no fixup runs.
    // LIB-REF-LIVE: no order injection and no enqueue suppression -- the in-sim AI is the sole order
    // source. Set from --live, never inferred from the fixture's shape: a mode that switched itself
    // on because a file was absent would read a MISSING input as a deliberate one, which is the
    // closed-default class this project keeps paying for (G175).
    bool live  = false;
    bool trace = false;
    // The divergence LOCALIZER: dump one hashed region's live bytes at the hash instant of one
    // step, in the harness's own `RX <step> <col> <off> <hex>` format, so a standalone dump and a
    // hosted `rdump_rid` dump of the same step are diffable without either side re-formatting.
    int         dump_col       = -1;
    int         dump_rid       = -1;
    uint32_t    dump_step      = 0;
    const char *dump_to        = nullptr;
    bool        resume_pending = false;
    double      resume_clock   = 0.0;
    double      resume_delta   = 0.0;
};

replay_ctx g_rp;

// The C6 hook's job, verbatim: pin TOTAL_GAME_TIME to the RECORDED clock for the step about to run.
// `g_step` is steps COMPLETED, so clocks[g_step] is the next step's clock and clocks[0] is step 1's
// -- the same indexing harness.cpp's on_sim_tick uses, and cross-checked against the fixture (the
// committed clock track's entry N-1 equals the hash line N's clock column exactly).
void pin_clock_track() {
    if (g_rp.step >= g_rp.limit) return;
    double *total = mh::state::ptr<double>(mh::state::RID_TOTAL_GAME_TIME);
    if (total) std::memcpy(total, &g_rp.clocks[g_rp.step], sizeof(double));
}

// THE OBSERVATION POINT: llm_strat_sim_step's entry, which is exactly where harness.cpp's
// on_sim_step detour sits. Everything here happens in the harness's own order -- inject, then hash,
// then the body.
void replay_sim_step() {
    const uint32_t step = ++g_rp.step;
    // C-prime: the draws that follow belong to THIS step -- the same statement harness.cpp makes
    // right after its own ++g_step, at the same point in the step (this IS the same observation
    // point: llm_strat_sim_step's entry, where the harness's on_sim_step detour sits).
    mh::sim::rng_trace_set_step(step);
    if (step > g_rp.limit) {
        mh::sim::sim_step();
        return;
    }

    if (g_rp.resume_pending) { // frame 1 only -- see replay_ctx's resume note
        g_rp.resume_pending = false;
        double *clk         = mh::state::ptr<double>(mh::state::RID_STRAT_GAME_CLOCK);
        double *dlt         = mh::state::ptr<double>(mh::state::RID_GAME_TIME_DELTA);
        if (clk) std::memcpy(clk, &g_rp.resume_clock, sizeof(double));
        if (dlt) std::memcpy(dlt, &g_rp.resume_delta, sizeof(double));
    }

    // LIB-REF-LIVE: the ONE structural difference between the two modes, and it is a subtraction.
    // A replay injects the recording's orders at the top of every step; a LIVE run injects nothing,
    // because the in-sim AI is the sole order source and reproducing that loop is the whole proof.
    // The tail-clear still runs -- it is unconditional in BOTH arms of the hosted harness, so a live
    // mode that skipped it would be the only arm anywhere that never cleared.
    if (g_rp.live)
        order_queue_tail_clear(0);
    else
        g_rp.rec->inject(step);

    // The one-byte mutation arm. Poked AFTER the injection and BEFORE the hash, inside the region's
    // own live extent, so the poke is in exactly the bytes the comparison walks. A described
    // mutation is not a run one: this is armed from the command line and the run's verdict is what
    // reports it.
    if (g_rp.poke_step == step && g_rp.poke_col >= 0) {
        const mh::state::hash_region &r = mh::state::HASH_REGIONS[g_rp.poke_col];
        uint8_t *const                b = mh::state::ptr<uint8_t>(r.rid);
        if (b) {
            b[r.offset] = static_cast<uint8_t>(b[r.offset] ^ 0xffu);
            std::printf("  [poke] step %u region %s +0 flipped\n", step, r.name);
        }
    }

    // The FP environment at the dumped instant, same format and same sample point as the harness's
    // FPENV line, so the two arms diff line-for-line.
    if (g_rp.dump_step == step)
        std::printf("  FPENV %u cw=%04X mxcsr=%08X\n", step, (unsigned)mh::fp::raw_x87_cw(),
                    (unsigned)mh::fp::raw_mxcsr());

    // --dump-rid: any region BY RID, hashed or not. The hashed-column dump above cannot reach the
    // ones that matter most for a divergence like this -- an MF_VIEW-only region (the nav-region
    // decomposition, the microsteps table) can differ silently between the arms and then DRIVE a
    // decision that shows up in a hashed region a step later. Same RX format.
    if (g_rp.dump_rid >= 0 && g_rp.dump_step == step && g_rp.dump_to != nullptr) {
        const auto     r   = static_cast<mh::state::region_id>(g_rp.dump_rid);
        const uint32_t len = mh::state::reach_of(r);
        const uint8_t *b   = mh::state::ptr<const uint8_t>(r);
        FILE          *f   = std::fopen(g_rp.dump_to, "wb");
        if (f != nullptr && b != nullptr) {
            static const char HEX[] = "0123456789ABCDEF";
            for (uint32_t off = 0; off < len; off += 64) {
                const uint32_t m = (len - off) < 64u ? (len - off) : 64u;
                std::fprintf(f, "RX %u %d %u ", step, g_rp.dump_rid, off);
                for (uint32_t j = 0; j < m; ++j) {
                    std::fputc(HEX[b[off + j] >> 4], f);
                    std::fputc(HEX[b[off + j] & 0x0f], f);
                }
                std::fputc(0x0a, f);
            }
            std::fclose(f);
            std::printf("  [dump] step %u rid %d -> %s (%u bytes)\n", step, g_rp.dump_rid,
                        g_rp.dump_to, len);
        }
    }

    if (g_rp.dump_col >= 0 && g_rp.dump_step == step && g_rp.dump_to != nullptr) {
        const mh::state::hash_region &r = mh::state::HASH_REGIONS[g_rp.dump_col];
        const uint8_t *const          b = mh::state::ptr<const uint8_t>(r.rid);
        FILE                         *f = std::fopen(g_rp.dump_to, "wb");
        if (f != nullptr && b != nullptr) {
            static const char HEX[] = "0123456789ABCDEF";
            for (uint32_t off = 0; off < r.len; off += 64) {
                const uint32_t m = (r.len - off) < 64u ? (r.len - off) : 64u;
                std::fprintf(f, "RX %u %d %u ", step, g_rp.dump_col, off);
                for (uint32_t j = 0; j < m; ++j) {
                    std::fputc(HEX[b[off + r.offset + j] >> 4], f);
                    std::fputc(HEX[b[off + r.offset + j] & 0x0f], f);
                }
                std::fputc(0x0a, f);
            }
            std::fclose(f);
            std::printf("  [dump] step %u region %s -> %s (%u bytes)\n", step, r.name,
                        g_rp.dump_to, r.len);
        }
    }

    const uint64_t got = libmh_state_hash();
    const uint64_t exp = g_rp.want->state[step - 1];
    if (got != exp) {
        ++g_rp.mismatches;
        // Every mismatching step, not only the first. A divergence that RE-CONVERGES between
        // periodic instants looks completely different from one that persists, and only the full
        // step list can tell them apart -- which is exactly the question the recording-format
        // phase-infidelity attribution turns on.
        std::printf("  MM %u\n", step);
        if (g_rp.first_bad == 0) {
            g_rp.first_bad = static_cast<int>(step);
            std::printf("  MISMATCH at step %u: state=%08lX%08lX expected %08lX%08lX\n", step,
                        (unsigned long)(got >> 32), (unsigned long)(got & 0xffffffffu),
                        (unsigned long)(exp >> 32), (unsigned long)(exp & 0xffffffffu));
            // LOCALIZE, which is what the R columns are for: name the regions that differ rather
            // than reporting that a 2.79 MB fold changed.
            if (g_rp.want->has_r[step - 1]) {
                int named = 0;
                for (int c = 0; c < mh::state::HASH_REGION_COUNT; ++c) {
                    const uint64_t a = mh::state::hash_slice(c, true, true, true);
                    const uint64_t w =
                        g_rp.want->per[static_cast<size_t>(step - 1) * g_rp.want->ncols + c];
                    if (a == w) continue;
                    if (std::strcmp(mh::state::HASH_REGIONS[c].name, "order_queue_count") == 0) {
                        int32_t *qc =
                            mh::state::ptr<int32_t>(mh::state::RID_STRAT_ORDER_QUEUE_COUNT);
                        std::printf("      [probe] order_queue_count = %d (ours)\n", qc ? *qc : -999);
                        // Brute-force what value the HOSTED arm held: order_queue_count is a 4-byte
                        // region, so its slice hash is a pure function of the integer. Restore ours.
                        if (qc != nullptr) {
                            const int32_t save = *qc;
                            for (int32_t v = 0; v <= 320; ++v) {
                                *qc = v;
                                if (mh::state::hash_slice(c, true, true, true) == w) {
                                    std::printf("      [probe] hosted order_queue_count = %d\n", v);
                                    break;
                                }
                            }
                            *qc = save;
                        }
                    }
                    std::printf("      region %-22s %s\n", mh::state::HASH_REGIONS[c].name,
                                mh::state::HASH_REGIONS[c].excluded ? "(EXCLUDED from the verdict)"
                                                                    : "*** DIFFERS");
                    ++named;
                }
                if (named == 0)
                    std::printf("      (no R column differs -- the fold disagrees with its own "
                                "columns; that is an instrument fault, not a divergence)\n");
            }
        }
    }

    if (g_rp.trace) std::printf("  [trace] step %u: entering the body\n", step);
    mh::sim::sim_step();
    if (g_rp.trace) std::printf("  [trace] step %u: body returned\n", step);
}

mh::lockstep::game_calls g_sim_tick_calls;
mh::sim::lt_frame_calls  g_frame_calls;

void replay_sim_tick() {
    mh::lockstep::detail::sim_tick(mh::lockstep::state(), g_sim_tick_calls, mh::lockstep::fixes());
}

void noop() {}

bool    g_no_time_tick = false;
int32_t replay_no_time_tick() { return 0; }

int32_t replay_time_tick() {
    // The pinned clock: pin_wallclock=1's role. Every value time_tick writes (current/last/total
    // game time, the frame ring, the fps estimate) is state_excluded(), so this cannot move the
    // comparison channel -- it runs for real anyway, because running the original body is cheaper to
    // justify than arguing that skipping it is safe.
    static double now = 0.0;
    now += 0.016667;
    return mh::lockstep::detail::time_tick(mh::lockstep::timekeeper(),
                                           mh::lockstep::timekeeper_live_calls(),
                                           mh::lockstep::fixes(), now);
}

} // namespace

// ---- the fault reporter -------------------------------------------------------------------------
//
// A standalone replay that faults is the RIGHT failure mode for a re-derive obligation the host has
// not discharged (world_snapshot_dispositions.json says so in as many words: "a wild read, not a
// drift"). But a fault that prints nothing is indistinguishable from a hang, so the handler turns it
// into a located fact: the exception code, the faulting address, and that address as an offset from
// the image base, which the linker .map resolves to a function.
LONG CALLBACK fault_report(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uintptr_t at   = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    std::printf("\n  FAULT: code 0x%08lX at 0x%08lX (image+0x%08lX)\n", (unsigned long)code,
                (unsigned long)at, (unsigned long)(at - base));
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        std::printf("         %s 0x%08lX\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

// LIB-DISPATCH-SA's selftest arm (dispatch_selftest.cpp). Declared rather than headered: one
// function, one caller, and a header for it would outweigh it.
int run_dispatch_selftest();

int main(int argc, char **argv) {
    // --dispatch-selftest: LIB-DISPATCH-SA's arm. Handled before ANY other argument because it
    // proves the fill FUNCTION on local tables -- it needs no fixture, no bound world, and no
    // arguments, and making it wait behind the fixture parsing would tie a table test to a replay.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--dispatch-selftest") == 0) return run_dispatch_selftest();

    const char *dir          = nullptr;
    uint32_t    steps        = 0;
    uint32_t    poke_at      = 0;
    const char *poke_reg     = nullptr;
    const char *dump_reg     = nullptr;
    const char *nav_report   = nullptr;
    const char *rng_trace_to = nullptr;
    const char *absent_list  = nullptr;
    const char *input_epoch  = nullptr; // TL-GATE8: the fixture's stamped hash-input epoch
    uint32_t    rng_lo = 1u, rng_hi = 0u;
    // Default ON: the fixture's replay_contract says replay_suppress_enqueue=1, and reproducing the
    // contract is this host's job. See the arming site for the negative arm's purpose.
    g_suppress_enqueue = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) dir = argv[++i];
        else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc)
            steps = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--poke-step") == 0 && i + 1 < argc)
            poke_at = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--poke-region") == 0 && i + 1 < argc)
            poke_reg = argv[++i];
        else if (std::strcmp(argv[i], "--microsteps-extent") == 0 && i + 1 < argc)
            g_microsteps_extent = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (std::strcmp(argv[i], "--microsteps-fill") == 0 && i + 1 < argc)
            g_microsteps_fill = static_cast<int>(std::strtoul(argv[++i], nullptr, 0));
        else if (std::strcmp(argv[i], "--no-time-tick") == 0)
            g_no_time_tick = true;
        else if (std::strcmp(argv[i], "--no-suppress-enqueue") == 0)
            g_suppress_enqueue = false;
        else if (std::strcmp(argv[i], "--no-restamp-text-ptrs") == 0)
            g_restamp_text_ptrs = false;
        else if (std::strcmp(argv[i], "--live") == 0) {
            // LIB-REF-LIVE. The two halves of the live contract are set TOGETHER, from one flag,
            // because they are one claim: no injection AND no suppression is what "the in-sim AI is
            // the sole order source" means. Letting them be armed separately would allow a run that
            // injects nothing yet still suppresses -- a sim with no orders at all, which would look
            // deterministic and prove nothing.
            g_rp.live          = true;
            g_suppress_enqueue = false;
        } else if (std::strcmp(argv[i], "--trace") == 0)
            g_rp.trace = true;
        else if (std::strcmp(argv[i], "--dump-rid") == 0 && i + 1 < argc)
            g_rp.dump_rid = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        else if (std::strcmp(argv[i], "--dump-region") == 0 && i + 1 < argc)
            dump_reg = argv[++i];
        else if (std::strcmp(argv[i], "--dump-step") == 0 && i + 1 < argc)
            g_rp.dump_step = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--dump-to") == 0 && i + 1 < argc)
            g_rp.dump_to = argv[++i];
        else if (std::strcmp(argv[i], "--nav-report") == 0 && i + 1 < argc)
            nav_report = argv[++i];
        else if (std::strcmp(argv[i], "--arena-poison") == 0 && i + 1 < argc)
            g_arena_poison = (int)std::strtoul(argv[++i], nullptr, 0) & 0xff;
        else if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            g_assets_dir = argv[++i];
        else if (std::strcmp(argv[i], "--assets-absent") == 0 && i + 1 < argc)
            absent_list = argv[++i];
        else if (std::strcmp(argv[i], "--input-epoch") == 0 && i + 1 < argc)
            input_epoch = argv[++i];
        else if (std::strcmp(argv[i], "--arena-pad") == 0 && i + 1 < argc)
            g_arena_pad = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--rng-trace") == 0 && i + 3 < argc) {
            rng_lo       = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            rng_hi       = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            rng_trace_to = argv[++i];
        }
    }
    if (dir == nullptr) {
        std::printf("usage: libref_host --fixture <unpacked dir> --input-epoch <E> [--steps N]\n"
                    "                   [--poke-step N --poke-region <name>]\n"
                    "                   [--dump-rid N | --dump-region <name>] [--dump-step N]\n"
                    "                   [--dump-to <file>] [--nav-report <file>]\n"
                    "                   [--arena-poison 0xNN] [--arena-pad N]\n"
                    "                   [--assets <dir>] [--assets-absent <file>]\n"
                    "                   [--no-suppress-enqueue] [--live]\n"
                    "                   [--no-restamp-text-ptrs]\n"
                    "\n"
                    "  --no-restamp-text-ptrs\n"
                    "                 leave the carried G_TEXT_PTRS pointing at the RECORDING's\n"
                    "                 address space. THE RED ARM for the re-stamp: a live run then\n"
                    "                 faults in format_core<wchar_t> the first time the sim formats\n"
                    "                 a message (measured: step 2001 of the ai-soak fixture). Keep\n"
                    "                 it, because a fixup nobody can switch off is a fixup nobody\n"
                    "                 can show is doing anything.\n"
                    "\n"
                    "  --input-epoch  the fixture's step0.hash_input_epoch (manifest.json). REQUIRED:\n"
                    "                 a fixture cut under another epoch is refused, never compared.\n"
                    "  --live         THE LIVE-LOOP MODE (LIB-REF-LIVE): no order injection and no\n"
                    "                 enqueue suppression, so the in-sim AI is the sole order source\n"
                    "                 and the enqueue/issue/dispatch loop runs for real. Needs a lane\n"
                    "                 unpacked from libref-live-aisoak-v1 (clock + world + streams,\n"
                    "                 NO mh_orders.bin); refuses a lane that has an order stream.\n"
                    "  --no-suppress-enqueue\n"
                    "                 let the sim's OWN enqueues reach the order queue. The fixture's\n"
                    "                 replay_contract sets replay_suppress_enqueue=1, so this host\n"
                    "                 suppresses by default and this flag is the negative arm: against\n"
                    "                 the committed fixture it is EXPECTED to diverge.\n"
                    "  --nav-report   after import, write the rebuilt map-region pool POINTER-FREE:\n"
                    "                 both list orders by region index, BY_INDEX, and every grid cell\n"
                    "                 as a region index + its terrain_flags. Directly comparable with\n"
                    "                 the blob's nav trailer; --dump-rid cannot do this comparison at\n"
                    "                 all, because the two pools live in different processes.\n"
                    "  --assets       an EXTRACTED bank tree (src/formats/unpack.py's\n"
                    "                 data/uncompressed/mh). Without it an asset request is a TRAP,\n"
                    "                 deliberately: an invented answer is worse than no answer.\n"
                    "  --assets-absent\n"
                    "                 a list of bank paths MEASURED to be absent from the retail\n"
                    "                 bank (one per line; # comments). Those answer a MISS instead\n"
                    "                 of trapping, because a miss is what the hosted arm answers\n"
                    "                 too. Everything not on the list still traps, and a listed name\n"
                    "                 that turns out to EXIST under --assets refuses the run.\n"
                    "  --arena-poison the byte every un-carried arena address holds (default 0xCD).\n"
                    "  --arena-pad    bytes of padding between every bound region (default 0).\n"
                    "                 The pair is an A/B against the whole class of 'the sim reads\n"
                    "                 arena bytes the fixture never wrote': poison catches a read\n"
                    "                 landing in alignment slack, padding catches one long enough to\n"
                    "                 reach the next region. If a divergence is unmoved under both,\n"
                    "                 no cross-region or uncarried read is in it.\n");
        return 2;
    }

    // UNBUFFERED. A replay that faults loses a buffered log entirely, and the last line before the
    // fault is the whole diagnosis -- exactly the "a crashed run prints NOTHING" trap the selftest
    // gate's own banner records.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, fault_report);

    std::printf("=== libref_host: the standalone libmh reference replay ===\n");
    // TL-GATE8: refuse, before anything is compared, a fixture cut under another hash-input epoch.
    std::printf("  build hash-input epoch %lu\n", (unsigned long)mh::state::HASH_INPUT_EPOCH);
    if (input_epoch == nullptr) {
        fail("hash-input epoch mismatch: artifact E=unstamped, build E=%lu -- pass --input-epoch "
             "<step0.hash_input_epoch>",
             (unsigned long)mh::state::HASH_INPUT_EPOCH);
        return 2;
    }
    if (std::strtoul(input_epoch, nullptr, 10) != mh::state::HASH_INPUT_EPOCH) {
        fail("hash-input epoch mismatch: artifact E=%s, build E=%lu -- stale fixture, re-capture it",
             input_epoch, (unsigned long)mh::state::HASH_INPUT_EPOCH);
        return 2;
    }
    // A GIVEN-BUT-UNREADABLE LIST IS A REFUSAL, NOT A WARNING: silently proceeding with zero
    // declarations turns every declared miss back into a trap, and the run would then fail for a
    // reason that has nothing to do with the replay.
    if (absent_list != nullptr && !load_absent_list(absent_list)) {
        fail("--assets-absent %s could not be read", absent_list);
        return 1;
    }
    std::printf("  asset tree: %s; declared-absent bank paths: %d\n",
                g_assets_dir ? g_assets_dir : "NONE (a request traps unless declared absent)",
                g_absent_n);
    std::printf("  libmh ABI version %u (header %u)\n", libmh_abi_version(), LIBMH_ABI_VERSION);
    if (libmh_abi_version() != LIBMH_ABI_VERSION) {
        fail("ABI version mismatch between this host's header and the linked libmh");
        return 1;
    }

    // ---- the host tables, BEFORE the bind: libmh_set_host_api is where the standalone build
    // installs the PC=53 guarantee (CRT-X87-CPP), and three of the C++ helpers it covers are WRONG
    // at PC=64, so nothing that computes may run ahead of it.
    if (libmh_set_host_api(&sim_host_table(), LIBMH_HOST_API_VERSION) != 0)
        fail("libmh_set_host_api refused the sim table");
    if (libmh_set_tact_host_api(&tact_host_table(), LIBMH_TACT_HOST_API_VERSION) != 0)
        fail("libmh_set_tact_host_api refused the tact table");
    if (libmh_host_api_unbound(on_unbound) != 0) fail("the sim host table has unbound entries");
    if (libmh_tact_host_api_unbound(on_unbound) != 0) fail("the tact host table has unbound entries");
    if (libmh_install_fp_precision() != 1) fail("could not install the PC=53 guarantee");
    // The RAW control words, printed in the SAME format the harness prints them in its rdump window
    // (seams/harness.cpp, "FPENV <step> cw= mxcsr="), so the two arms diff line-for-line. The PC=53
    // guarantee covers the precision FIELD only -- rounding, the exception masks and all of MXCSR are
    // whatever the host left, and the host is a different program in each arm.
    std::printf("  FPENV 0 cw=%04X mxcsr=%08X\n", (unsigned)mh::fp::raw_x87_cw(),
                (unsigned)mh::fp::raw_mxcsr());

    // ---- the arena
    arena a;
    if (!a.bind_all_regions()) {
        fail("could not bind all %d regions", (int)libmh_region_count());
        return 1;
    }
    std::printf("  bound %d/%d region(s), %.2f MB of arena (poisoned 0x%02X)\n", libmh_bound_count(),
                (int)libmh_region_count(), (double)a.len / (1024.0 * 1024.0), g_arena_poison);
    if (libmh_bound_count() != (int)libmh_region_count()) fail("not every region got an answer");

    // OPEN THE INBOUND SURFACE. Every MH_IN_GUARD'd C entry in state/host_in.cpp REFUSES until this
    // handshake happens, and a refusal is not an error -- it RETURNS THE CLOSED DEFAULT. For
    // libmh_bldg_footprint_is_clear that default is 0, i.e. "the footprint is NOT clear", which is a
    // perfectly ordinary answer that no oracle can distinguish from a real one. This host never
    // called it, so for 289 steps every guarded entry silently answered its default; the first one
    // whose default differed from the truth was that footprint check, and it sent an AI unit to
    // idle_scatter instead of deploy_to_building. See the report -- this is the step-290 root cause.
    if (libmh_in_open(LIBMH_HOST_IN_VERSION) != 0)
        fail("libmh_in_open refused (host 0x%08X)", (unsigned)LIBMH_HOST_IN_VERSION);

    // THE REPLAY CONTRACT'S ENQUEUE SUPPRESSION, reproduced (2026-09-11). manifest.json's
    // replay_contract sets `replay_suppress_enqueue=1`, and a standalone host that replays the
    // fixture must reproduce that flag's SEMANTICS, not merely read the same bytes: with it set,
    // nothing the sim itself enqueues reaches the order queue, so the injected recording is the only
    // thing that does. Until today this line was deliberately absent, and correctly so -- the check
    // then lived in the PROMOTION WRAPPER behind the game's entry, which this host does not have, so
    // setting the flag here changed nothing and would have been dead code implying a reproduction it
    // did not provide. The check now lives in the SINK (mh::orders::detail::enqueue), which IS the
    // path this host's sim_step takes, so the flag is live here and the two arms suppress the same
    // calls at the same point.
    //
    // THE NEGATIVE ARM IS REAL, not decorative: `--no-suppress-enqueue` leaves the sim's own enqueues
    // running. Against the fixture that is expected to DIVERGE (the recording was made under
    // suppression), which is what makes it a falsifier for this line rather than a switch nobody
    // turns; it is also the configuration LIB-REF-LIVE's no-injection live-loop proof needs.
    if (g_suppress_enqueue) mh::orders::set_suppress_enqueue(true);
    std::printf("  enqueue suppression: %s\n",
                g_suppress_enqueue ? "ON (the replay contract's replay_suppress_enqueue=1)"
                                   : "OFF (--no-suppress-enqueue: the sim's own enqueues RUN)");

    // The BASELINE for the refusal counter below. mh::libmh_in::trap() has counted closed-default
    // refusals since the module loaded and nothing has ever ASKED it -- which is precisely how the
    // step-290 bug stayed invisible. Baselining here rather than resetting inside libmh_in_open
    // keeps a shipping function's behaviour unchanged: what this host cares about is refusals AFTER
    // its own open, and that is a subtraction, not a state change.
    const int in_traps_at_open = mh::libmh_in::trap_count();

    // ---- the fixture
    char path[1024];
    blob world, orders, clocks, steps_txt, regions_txt;
    std::snprintf(path, sizeof(path), "%s/mh_world.bin", dir);
    if (!slurp(path, &world)) return 1;
    // THE ORDER STREAM IS OPTIONAL, AND ONLY IN LIVE MODE -- checked BOTH WAYS rather than merely
    // skipped. A live fixture has no mh_orders.bin by construction (its absence is the artifact's
    // claim), but "the file is missing" and "this is a live fixture" must never be the same
    // observation: a replay lane whose orders half failed to unpack would otherwise silently become
    // a live run against a replay stream and diverge at the first order. So --live REFUSES a lane
    // that has one, and a lane without one REFUSES to run as a replay.
    std::snprintf(path, sizeof(path), "%s/mh_orders.bin", dir);
    {
        FILE *probe = std::fopen(path, "rb");
        if (probe != nullptr) std::fclose(probe);
        const bool have_orders = probe != nullptr;
        if (g_rp.live && have_orders)
            fail("--live, but %s HAS an mh_orders.bin. A live fixture carries no order stream; a "
                 "lane that has one was unpacked from a REPLAY fixture and the two contracts are "
                 "not interchangeable",
                 dir);
        if (!g_rp.live && !have_orders)
            fail("%s has no mh_orders.bin. If this is the LIVE-LOOP fixture, say --live -- an "
                 "absent input is never read as a mode",
                 dir);
        if (have_orders && !slurp(path, &orders)) return 1;
    }
    std::snprintf(path, sizeof(path), "%s/mh_clock.bin", dir);
    if (!slurp(path, &clocks)) return 1;
    std::snprintf(path, sizeof(path), "%s/hash_steps.txt", dir);
    if (!slurp(path, &steps_txt)) return 1;
    std::snprintf(path, sizeof(path), "%s/hash_regions.txt", dir);
    if (!slurp(path, &regions_txt)) return 1;

    mh::state::world::blob_header h;
    std::memcpy(&h, world.p, sizeof(h));
    std::printf("  world blob: %zu bytes, %lu blocks, step %lu, masks %lu, head_ptrs %lu\n", world.n,
                (unsigned long)h.base.block_count, (unsigned long)h.step,
                (unsigned long)h.mask_flags, (unsigned long)h.region_head_ptrs);

    // A HASH IS ONLY COMPARABLE UNDER ITS OWN IMPLEMENTATION AND MANIFEST -- checked first, because
    // a mismatch here makes every number below incomparable rather than wrong.
    if (h.hash_sink_fp != mh::state::world::hash_sink_fingerprint())
        fail("the blob was captured under a DIFFERENT hash_sink implementation -- stale fixture");
    if (h.hash_manifest_fp != mh::state::world::hash_manifest_fingerprint())
        fail("the blob was captured under a DIFFERENT hash manifest -- stale fixture");
    if (h.mask_flags != (mh::state::world::MASK_CTRL_GROUP | mh::state::world::MASK_SOLDIER_ANIM |
                         mh::state::world::MASK_PLANETS_GFX))
        fail("the blob's mask flags (%lu) are not the three libmh_state_hash applies",
             (unsigned long)h.mask_flags);

    const uint32_t total = static_cast<uint32_t>(clocks.n / sizeof(double));
    if (steps == 0 || steps > total) steps = total;

    stream want;
    if (!parse_stream(steps_txt, regions_txt, total, &want)) return 1;

    recording rec;
    if (g_rp.live) {
        std::printf("  LIVE-LOOP mode: NO order stream, NO injection, enqueue suppression OFF -- "
                    "the in-sim AI is the sole order source\n");
        std::printf("  clock track: %u step(s); running %u\n", total, steps);
    } else {
        if (!rec.open(orders)) {
            fail("mh_orders.bin is not a recording (bad magic)");
            return 1;
        }
        std::printf("  recording: %u order record(s); clock track: %u step(s); replaying %u\n",
                    rec.n, total, steps);
    }

    // ---- the poison must not already agree ------------------------------------------------------
    const uint64_t poisoned = libmh_state_hash();
    if (poisoned == want.state[0])
        fail("the poisoned arena already reproduces step 1 -- the comparison would prove nothing");

    // ---- import: bytes + the re-derives (state/spine.cpp) ---------------------------------------
    const int irc = libmh_import_world(world.p, world.n);
    if (irc != 0) {
        fail("libmh_import_world refused the fixture (rc=%d)", irc);
        return 1;
    }

    // THE IMPORTING HOST'S OWN OBLIGATION, discharged here rather than inside libmh: the carried
    // pointers INTO bound regions, which are wrong the moment a host binds the pointee anywhere but
    // its stock .bss. world_snapshot_dispositions.json has always named this class and named its
    // owner ("obligations on the importing host, not gaps"); G_TEXT_PTRS is the third member and the
    // first one a headless strategic run reaches. Immediately after the import and before the first
    // hash, so nothing has read a stale pointer yet.
    if (!restamp_text_ptrs()) return 1;
    if (g_microsteps_fill >= 0 && g_microsteps_extent > 96u) {
        uint8_t *const b = mh::state::ptr<uint8_t>(mh::state::RID_STRAT_MOVE_MICROSTEPS);
        if (b) {
            std::memset(b + 96, g_microsteps_fill, g_microsteps_extent - 96u);
            std::printf("  [probe] MOVE_MICROSTEPS [96, %u) filled with 0x%02X\n",
                        g_microsteps_extent, g_microsteps_fill);
        }
    }

    // ---- --nav-report: the nav decomposition, rendered POINTER-FREE ------------------------------
    //
    // The B5.1 acceptance has to compare the pool this host rebuilt against the one the recording
    // captured, and a raw `--dump-rid` cannot do it: the two live in different processes, so every
    // dword differs by construction, and two dumps from two RUNS of THIS host differ from each other
    // too (the nodes are individual mallocs and the heap moves). That is not a hypothetical -- it
    // produced a confident "180 label mismatches" against a pool that was in fact exact.
    //
    // So the report names regions by their own `index`, which is carried data rather than an address,
    // and it includes the two list ORDERS, which no region dump can reach at all because the lists
    // live in the heap. Every line is directly comparable with the blob's nav trailer.
    if (nav_report) {
        FILE *nf = std::fopen(nav_report, "wb");
        if (nf == nullptr) {
            fail("could not open %s for the nav report", nav_report);
            return 1;
        }
        const mh::state::nav::import_stats &ns = mh::state::nav::last_import();
        std::fprintf(nf, "nodes %u active %u free %u grid_set %u\n", ns.nodes, ns.active, ns.free,
                     ns.grid_set);
        mh::sim::sim_state  nst  = mh::sim::state();
        mh::sim::sim_store &nown = nst.own;
        std::fprintf(nf, "active");
        for (const mh::sim::llm_map_region *p = nown.region_list_head_mut(); p != nullptr; p = p->next)
            std::fprintf(nf, " %u", (unsigned)p->index);
        std::fprintf(nf, "\nfree");
        for (const mh::sim::llm_map_region *p = nown.region_pool_free_head(); p != nullptr; p = p->next)
            std::fprintf(nf, " %u", (unsigned)p->index);
        std::fprintf(nf, "\n");
        for (uint32_t i = 0; i < 4096u; ++i) {
            const mh::sim::llm_map_region *r = nown.region_by_index()[i];
            if (r != nullptr) std::fprintf(nf, "bi %u %u\n", i, (unsigned)r->index);
        }
        for (int32_t x = 0; x < mh::sim::MAP_GRID_DIM; ++x)
            for (int32_t y = 0; y < mh::sim::MAP_GRID_DIM; ++y) {
                const mh::sim::llm_map_region_cell &c   = nown.region_cell_at(x, y);
                const uintptr_t                     v   = reinterpret_cast<uintptr_t>(c.region);
                const long                          lab = v == 0 ? -1L : (v == 0xffffffffu ? -2L : (long)c.region->index);
                std::fprintf(nf, "g %d %d %ld %lu\n", x, y, lab, (unsigned long)c.terrain_flags);
            }
        std::fclose(nf);
        std::printf("  [nav] report -> %s (%u nodes, %u active, %u free, %u assigned cells)\n",
                    nav_report, ns.nodes, ns.active, ns.free, ns.grid_set);
    }

    // C-prime: arm BEFORE the first step, so the window's own first draw is captured.
    if (rng_trace_to != nullptr) {
        mh::sim::rng_trace_window(rng_lo, rng_hi);
        // THE IMAGE BASE IS PART OF THE MEASUREMENT, not decoration: this exe is ASLR'd, so a
        // recorded return address is meaningless without the base it was recorded under. RVA =
        // ra - base, and the linker .map resolves the RVA to a function.
        std::printf("  [rng] tracing draws over steps %u..%u -> %s (image base %p)\n", rng_lo, rng_hi,
                    rng_trace_to, (const void *)GetModuleHandleA(nullptr));
    }

    // ---- THE STEP-0 CLAUSE, IN TWO PARTS AND AT THE RIGHT PHASE ---------------------------------
    //
    // WHAT IT USED TO DO, AND WHY THAT WAS WRONG (corrected 2026-09-12, LIB-REF-SOAK). It compared
    // the imported world against the committed stream's STEP-1 line. Those are two different phases:
    // the imported world is PRE-INJECTION, while the stream is a replay's, so its step-1 line was
    // sampled AFTER that step's orders were injected. The two agree only when the recording happens
    // to carry no step-1 records -- true of the sp_det fixture, and the reason this held for months.
    // The all-AI soak fixture's order stream starts AT step 1, and the clause failed a world that
    // was byte-perfect: `order_queue` and `order_queue_count` differed and nothing else did, while
    // the ordinary per-step comparison -- which runs after injection and DOES cover step 1 --
    // reported 0 mismatches across all 5000 steps.
    //
    // (1) THE IMPORT CLAUSE is against the blob's OWN carried lockstep_state. That value was
    // captured at the same instant as the bytes it ships with, so "the imported world reproduces the
    // world the capture recorded" is exactly what this compares -- phase-correct by construction,
    // and applicable to every fixture regardless of what its first step injects.
    const uint64_t at_step0 = libmh_state_hash();
    std::printf("  after import: state=%08lX%08lX expected %08lX%08lX (the blob's own "
                "lockstep_state)\n",
                (unsigned long)(at_step0 >> 32), (unsigned long)(at_step0 & 0xffffffffu),
                (unsigned long)(h.lockstep_state >> 32),
                (unsigned long)(h.lockstep_state & 0xffffffffu));
    if (at_step0 != h.lockstep_state) {
        fail("THE STEP-0 CLAUSE: the imported world does not reproduce the state hash the CAPTURE "
             "recorded in this very blob");
        for (int c = 0; c < mh::state::HASH_REGION_COUNT; ++c) {
            const uint64_t a = mh::state::hash_slice(c, true, true, true);
            if (a != want.per[c])
                std::printf("      region %-22s %s\n", mh::state::HASH_REGIONS[c].name,
                            mh::state::HASH_REGIONS[c].excluded ? "(excluded)" : "*** DIFFERS");
        }
    }

    // (2) THE SAME-RUN CROSS-CHECK, kept, but only where it is valid -- and it PRINTS WHICH ARM RAN
    // rather than skipping in silence, because a check that quietly declines to run is the shape
    // this project keeps paying for. Valid exactly when step 1 injects nothing: then the replay's
    // step-1 sample is the pre-injection world, which is what the blob holds.
    {
        const bool step1_injects = !g_rp.live && rec.n > 0 && rec.step_of(0) == 1u;
        if (step1_injects) {
            std::printf("  same-run cross-check: SKIPPED ON A MEASURED PRECONDITION -- the "
                        "recording carries record(s) at step 1, so the stream's step-1 line was "
                        "sampled AFTER injection and is not comparable with the pre-injection "
                        "blob. Step 1 is covered by the ordinary per-step comparison below.\n");
        } else if (h.lockstep_state != want.state[0]) {
            fail("THE SAME-RUN CROSS-CHECK: the blob's lockstep_state %08lX%08lX is not the "
                 "stream's step-1 state %08lX%08lX, and step 1 injects nothing -- the blob and the "
                 "hash stream are from DIFFERENT runs",
                 (unsigned long)(h.lockstep_state >> 32),
                 (unsigned long)(h.lockstep_state & 0xffffffffu),
                 (unsigned long)(want.state[0] >> 32), (unsigned long)(want.state[0] & 0xffffffffu));
        } else {
            std::printf("  same-run cross-check: RAN -- step 1 injects nothing and the blob's "
                        "lockstep_state matches the stream's step-1 state\n");
        }
    }

    // ---- the drive ------------------------------------------------------------------------------
    g_rp.rec    = &rec;
    g_rp.want   = &want;
    g_rp.clocks = reinterpret_cast<const double *>(clocks.p);
    g_rp.limit  = steps;

    // Arm the frame-1 resume: stash the imported pair, then roll the clock back so the driver's
    // gate fires. See replay_ctx.
    {
        double *clk = mh::state::ptr<double>(mh::state::RID_STRAT_GAME_CLOCK);
        double *dlt = mh::state::ptr<double>(mh::state::RID_GAME_TIME_DELTA);
        if (clk == nullptr || dlt == nullptr) {
            fail("the clock regions are unbound after import");
            return 1;
        }
        std::memcpy(&g_rp.resume_clock, clk, sizeof(double));
        std::memcpy(&g_rp.resume_delta, dlt, sizeof(double));
        const double rolled = g_rp.resume_clock - 1.0;
        std::memcpy(clk, &rolled, sizeof(double));
        g_rp.resume_pending = true;
    }
    if (poke_reg) {
        for (int c = 0; c < mh::state::HASH_REGION_COUNT; ++c)
            if (std::strcmp(mh::state::HASH_REGIONS[c].name, poke_reg) == 0) g_rp.poke_col = c;
        if (g_rp.poke_col < 0) {
            fail("--poke-region %s is not a hash manifest column", poke_reg);
            return 1;
        }
        g_rp.poke_step = poke_at ? poke_at : 1;
    }

    if (g_rp.dump_rid >= 0 && g_rp.dump_step == 0) g_rp.dump_step = 1;
    if (dump_reg) {
        for (int c = 0; c < mh::state::HASH_REGION_COUNT; ++c)
            if (std::strcmp(mh::state::HASH_REGIONS[c].name, dump_reg) == 0) g_rp.dump_col = c;
        if (g_rp.dump_col < 0) {
            fail("--dump-region %s is not a hash manifest column", dump_reg);
            return 1;
        }
        if (g_rp.dump_step == 0) g_rp.dump_step = 1;
    }

    g_sim_tick_calls               = mh::lockstep::live_calls();
    g_sim_tick_calls.sim_step      = replay_sim_step;
    g_frame_calls                  = mh::sim::live_lt_frame_calls();
    g_frame_calls.input_update     = noop;
    g_frame_calls.pace_time_tick   = noop;
    g_frame_calls.time_tick        = g_no_time_tick ? replay_no_time_tick : replay_time_tick;
    g_frame_calls.harness_sim_tick = pin_clock_track;
    g_frame_calls.sim_tick         = replay_sim_tick;
    g_frame_calls.render_present   = noop;
    g_frame_calls.render_view      = noop;

    const DWORD t0 = GetTickCount();
    while (g_rp.step < steps) {
        const uint32_t     before = g_rp.step;
        mh::sim::sim_state st     = mh::sim::state();
        mh::sim::detail::frame(st.read, st.own, g_frame_calls);
        if (g_rp.step == before) {
            fail("frame %u produced no sim step -- the clock track did not advance the sim",
                 before + 1);
            break;
        }
    }
    const double secs = (GetTickCount() - t0) / 1000.0;

    // ---- the verdict ----------------------------------------------------------------------------
    if (rng_trace_to != nullptr) {
        FILE *rf = std::fopen(rng_trace_to, "wb");
        if (rf != nullptr) {
            const int n = mh::sim::rng_trace_count();
            for (int i = 0; i < n; ++i) {
                const mh::sim::rng_trace_entry &e = mh::sim::rng_trace_at(i);
                std::fprintf(rf, "RNGD %d %u %d %04X %p %p\n", i, e.step, e.channel,
                             (unsigned)e.after, e.ra, e.site);
            }
            const int nn = mh::sim::rng_trace_note_count();
            for (int i = 0; i < nn; ++i) {
                const mh::sim::rng_trace_note &n = mh::sim::rng_trace_note_at(i);
                std::fprintf(rf, "NOTE %d %u %u %08X %08X %08X %08X %08X %08X\n", i, n.step, n.tag,
                             n.a, n.b, n.c, n.d, n.e, n.f);
            }
            std::fclose(rf);
            std::printf("  [rng] %d draw(s) written%s\n", n,
                        mh::sim::rng_trace_overflowed() ? "  -- OVERFLOWED, sequence INVALID" : "");
        }
    }

    std::printf("\n  replayed %u step(s) in %.2f s (%.0f steps/s)\n", g_rp.step, secs,
                secs > 0.0 ? g_rp.step / secs : 0.0);
    std::printf("  state-hash mismatches: %d%s\n", g_rp.mismatches,
                g_rp.first_bad ? "" : " (ALL STEPS IDENTICAL)");
    if (g_rp.first_bad) std::printf("  first divergence at step %d\n", g_rp.first_bad);
    // The injector's evidence line, in the same wording the hosted harness emits so one parser reads
    // both and the two arms are comparable at a glance. Unconditional in replay mode.
    if (!g_rp.live)
        std::printf("  injector: count SET at %d of %d arrival step(s), %d append-without-set\n",
                    rec.set_ok, rec.arrivals, rec.no_set);
    std::printf("  CP_ACP codec calls: ansi_to_wide %ld, scratch %ld, wide_to_local %ld, bytes %ld\n",
                g_codec_calls[0], g_codec_calls[1], g_codec_calls[2], g_codec_calls[3]);
    // Reported unconditionally, including the 0/0 case: "the declaration was never needed" and "the
    // declaration was never loaded" are different facts and only a printed number separates them.
    std::printf("  declared-absent asset requests answered: %d (of %d declaration(s))\n",
                g_absent_hits, g_absent_n);
    g_sim_traps.report("sim");
    g_tact_traps.report("tact");

    // THE INBOUND REFUSAL COUNT, and it is a HARD FAILURE rather than a warning.
    //
    // Every MH_IN_GUARD'd entry in state/host_in.cpp answers its CLOSED DEFAULT when the inbound
    // surface is not open -- 0, or nothing, depending on the entry. For a replay host no such answer
    // is ever legitimate: a closed default is indistinguishable from a real one at every oracle this
    // project owns, which is exactly how it hid. THE COUNTER ALREADY EXISTED (mh::libmh_in::trap()
    // has incremented it since the module was written, and last_trap() has always named the first
    // one); what did not exist was anybody asking it. This host went 289 steps taking
    // libmh_bldg_footprint_is_clear's closed 0 for "the footprint is not clear", sent an AI unit to
    // idle_scatter instead of deploy_to_building, and diverged -- with the count sitting there,
    // unread, the whole time. An instrument nobody reads is not an instrument.
    const int in_refused = mh::libmh_in::trap_count() - in_traps_at_open;
    if (in_refused > 0) {
        std::printf("  inbound refusals since open: %d -- THIS IS A RED (first: %s)\n", in_refused,
                    mh::libmh_in::last_trap());
    } else {
        std::printf("  inbound refusals since open: 0\n");
    }

    if (g_sim_traps.total() != 0 || g_tact_traps.total() != 0)
        fail("a host-table trap fired -- see the counts above");
    if (in_refused > 0)
        fail("%d inbound entr(y/ies) answered a CLOSED DEFAULT instead of running (first: %s) -- the "
             "replay ran on invented answers",
             in_refused, mh::libmh_in::last_trap());
    if (g_rp.mismatches != 0 && g_rp.poke_col < 0) fail("the replay diverged from the fixture");
    if (g_rp.poke_col >= 0 && g_rp.mismatches == 0)
        fail("THE RED ARM DID NOT GO RED: a one-byte poke of %s at step %u left the hash unchanged",
             poke_reg, g_rp.poke_step);

    const bool ok = (g_fails == 0);
    std::printf("=== libref_host: %s ===\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
