#include "hook/promoted.h"

#include <cstdio>
#include <cstring>

namespace mh::hook {
namespace {

const owner_range *g_table   = nullptr;
int                g_table_n = 0;

// Bounded so this stays allocation-free and usable from inside an arming path. Raised 64 -> 256 on
// 2026-09-01 (X-TOMB / endgame-plan wave prep), then 256 -> 512 the same day when the promotion
// wave's arithmetic was actually done: the ship set is ~45, and the three whole-domain waves add
// 62 (orders_issue) + 32 (sim_resid) + 167 (ai) = 306 entries with every wave armed together --
// past the 256 cap before SIM1-P adds its roots. 512 costs 2 KB of static table and leaves the
// overflow line as the backstop it was meant to be, not a ceiling the third wave hits. Overflow is
// reported rather than wrapped, because a silently dropped promotion would re-open the exact hole
// this file closes.
constexpr int MAX_PROMOTED = 512;
uintptr_t     g_promoted[MAX_PROMOTED];
int           g_promoted_n = 0;

// Entries claimed by a detour (C4). Small because detours that own an entry are a handful and each
// one is a deliberate decision; a full table would suggest otherwise.
// C9(c) raised this from 16. Every EXCLUSIVE detour now claims its entry, and there are far more of
// those than the ~45 hand-written install sites suggest: the effects gate layer alone arms 79 in one
// table, [trace] can add 16, and the seam/harness/video detours are another ~45. A first attempt at
// 96 still overflowed 17 times on an ordinary run -- MEASURED, not estimated, from the overflow line
// below, which is the reason that line exists. 256 is ~2 KB and leaves real slack; if it ever fills,
// the log says so by address rather than the interlock quietly ceasing to refuse.
// RESIZED 2026-09-05, and the old number is left above because it is the evidence. That sizing was
// measured against a table holding EXCLUSIVE claims only -- `claim_entry` discarded every shared one
// with `if (claim != entry_claim::exclusive) return;`. SIM1-P clause 6 made every claim recorded,
// which multiplied the population by ~5 overnight and nobody re-sized: a promoted ship run attempts
// ~497 claims, so 256 filled and **241 claims per run were dropped**, in the A/B, the determinism
// gate and the whole UI suite alike.
//
// 1024 is sized off the real population (~497) with 2x headroom. The real protection is NOT the
// number, which will go stale again -- it is that overflow now reports itself ONCE with the phrase
// the gates grep for and invalidates the run, so the next promote wave that outgrows this cap fails
// loudly instead of dropping claims into a 241-line murmur.
constexpr int MAX_OWNED = 1024;
struct owned_entry {
    uintptr_t   entry;
    const char *owner;
    // SIM1-P clause 6. `false` for an entry_claim::exclusive claim (the historical contents of this
    // table, which detour_refusal consults to refuse a second patch) and `true` for a ::rebind claim,
    // where sharing the entry IS the design. Recorded in ONE table with a flag rather than a second
    // table so there is one place an entry's claimant is known, and read back through two accessors so
    // that detour_refusal's behaviour is unchanged: it asks entry_owner_of, which still answers only
    // for exclusive claims. A shared claim used to be DISCARDED by claim_entry outright, which is why
    // kHarnessOwned[] had to be hand-kept -- the information the harness needed was thrown away one
    // frame before it was wanted.
    bool shared;
};
owned_entry g_owned[MAX_OWNED];
int         g_owned_n = 0;
// Claims refused for want of a slot. Nonzero means the run is invalid; see the overflow branch.
int g_owned_dropped = 0;

// U30: the refused installs, so the run can state them ONCE instead of leaving them scattered
// through 2 MB of log. Bounded like every other table here. 32 is far above what a healthy run
// produces -- which is ZERO, and that is the design: a refusal is an armed mechanism that is not
// armed, so a run with dozens of them has a configuration problem, not a table-sizing problem. The
// overflow is reported (loudly, and again in the summary) rather than wrapped, because a dropped
// refusal is the exact silence this table exists to end.
constexpr int MAX_REFUSED = 32;
struct refused_entry {
    uintptr_t     target;
    const char   *who;
    refuse_reason why;
};
refused_entry g_refused[MAX_REFUSED];
int           g_refused_n    = 0;
int           g_refused_lost = 0;

// F4C-COMP. Bodies an in-memory static patch wrote into. Bounded like every other table here, and
// sized off the measurement rather than off a feeling: the widest single manifest in src/patcher
// (grand_all_caphike_storagecap) lands its 7,750 sites in 619 DISTINCT bodies, which is the number
// that matters because the unit here is the body, not the site. 1024 leaves ~1.6x over the worst
// manifest the compile list could ever hold and costs 20 KB of static table. Overflow is REPORTED
// once with a run-invalidating verdict, in the shape g_owned_dropped learned the hard way: a dropped
// row does not merely lose a diagnostic, it silently stops refusing the next claimant.
// The value lives in the HEADER since fork F4C-GATE (mh::hook::MAX_PATCHED_BODIES) so the generated
// manifest headers can static_assert against it; this alias keeps the body below unchanged and makes
// a drift between the two impossible by construction.
constexpr int MAX_PATCHED = MAX_PATCHED_BODIES;
struct patched_body {
    uintptr_t   entry;
    uintptr_t   end; // INCLUSIVE
    const char *name;
    const char *manifest;
    bool        window;
};
patched_body g_patched[MAX_PATCHED];
int          g_patched_n       = 0;
int          g_patched_dropped = 0;

// The widest entry steal in the tree: install_jmp writes 8 (E9 rel32 + 3 NOPs), every
// install_trampoline call site passes stolen=8, the byte neuter writes 3.
constexpr int ENTRY_WINDOW = 8;

const char *reason_str(refuse_reason r) {
    switch (r) {
        case refuse_reason::promoted: return "target PROMOTED";
        case refuse_reason::entry_owned: return "entry OWNED";
        case refuse_reason::patched: return "body STATIC-PATCHED";
        case refuse_reason::prologue: return "prologue MISMATCH";
        case refuse_reason::alloc_failed: return "thunk ALLOC FAILED";
        case refuse_reason::protect_failed: return "entry PROTECT FAILED";
        default: return "?";
    }
}

void (*g_log)(const char *) = nullptr;

void say(const char *line) {
    if (g_log) g_log(line);
}

} // namespace

void set_owner_table(const owner_range *tbl, int count) {
    g_table   = tbl;
    g_table_n = count;
}

void set_promotion_logger(void (*fn)(const char *)) { g_log = fn; }

void promotion_log(const char *line) { say(line); }

int promoted_count() { return g_promoted_n; }

void registry_reset_for_test() {
    g_promoted_n      = 0;
    g_owned_n         = 0;
    g_owned_dropped   = 0;
    g_refused_n       = 0;
    g_refused_lost    = 0;
    g_patched_n       = 0;
    g_patched_dropped = 0;
}

// ---- F4C-COMP: the in-memory-static-patch registry ---------------------------------------------

int entry_window_bytes() { return ENTRY_WINDOW; }

int patched_body_count() { return g_patched_n; }

void note_patched_body(uintptr_t entry, uintptr_t end, const char *name, const char *manifest,
                       bool hits_entry_window) {
    for (int i = 0; i < g_patched_n; ++i)
        if (g_patched[i].entry == entry) {
            // A second manifest over the same body: keep the first claimant's name (it wrote first,
            // and the refusal wants the mechanism that already holds the bytes) but OR the window
            // flag -- the question a claimant asks is whether the window is contested at all.
            g_patched[i].window = g_patched[i].window || hits_entry_window;
            return;
        }
    if (g_patched_n >= MAX_PATCHED) {
        ++g_patched_dropped;
        if (g_patched_dropped == 1) {
            char line[256];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [inmem] PATCHED-BODY TABLE FULL (%d slots) at %08X -- PARTIAL, treat this "
                        "run as invalid: further bodies are DROPPED, so detour_refusal stops "
                        "refusing installs over bytes this manifest already wrote\n",
                        MAX_PATCHED, (unsigned)entry);
            say(line);
        }
        return;
    }
    g_patched[g_patched_n].entry    = entry;
    g_patched[g_patched_n].end      = end;
    g_patched[g_patched_n].name     = name;
    g_patched[g_patched_n].manifest = manifest;
    g_patched[g_patched_n].window   = hits_entry_window;
    ++g_patched_n;
}

const char *patched_body_of(uintptr_t addr, const char **fn_out) {
    for (int i = 0; i < g_patched_n; ++i) {
        if (addr < g_patched[i].entry || addr > g_patched[i].end) continue;
        if (fn_out) *fn_out = g_patched[i].name;
        return g_patched[i].manifest;
    }
    return nullptr;
}

const char *patched_entry_window_of(uintptr_t entry, const char **fn_out) {
    for (int i = 0; i < g_patched_n; ++i) {
        if (g_patched[i].entry != entry || !g_patched[i].window) continue;
        if (fn_out) *fn_out = g_patched[i].name;
        return g_patched[i].manifest;
    }
    return nullptr;
}

void report_patched_bodies() {
    // The affirmative case, for report_entry_refusals()'s reason: `[patch] inmem` is default OFF, so
    // "no bodies registered" is the overwhelmingly common state and an absent line would be
    // indistinguishable from a patcher that armed and failed to register.
    if (g_patched_n == 0 && g_patched_dropped == 0) {
        say("; [inmem] 0 body(ies) registered with the interlock -- no in-memory manifest holds "
            "bytes inside a game function in this run\n");
        return;
    }
    char   line[1024];
    size_t n = 0;
    {
        const int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                  "; [inmem] %d body(ies) REGISTERED with the interlock -- a later "
                                  "jmp install anywhere inside one, or a trampoline over a contested "
                                  "entry window, is refused BY NAME:",
                                  g_patched_n);
        n           = (k > 0) ? (size_t)k : 0;
    }
    for (int i = 0; i < g_patched_n; ++i) {
        char one[224];
        _snprintf_s(one, sizeof(one), _TRUNCATE, " [%s %08X-%08X via %s%s]", g_patched[i].name,
                    (unsigned)g_patched[i].entry, (unsigned)g_patched[i].end, g_patched[i].manifest,
                    g_patched[i].window ? ", ENTRY WINDOW contested" : "");
        const size_t need = strlen(one);
        if (n + need + 40 >= sizeof(line)) {
            _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " ... +%d MORE, elided (line cap)",
                        g_patched_n - i);
            n = strlen(line);
            break;
        }
        memcpy(line + n, one, need + 1);
        n += need;
    }
    if (n + 2 < sizeof(line)) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    say(line);
    if (g_patched_dropped) {
        char over[192];
        _snprintf_s(over, sizeof(over), _TRUNCATE,
                    "; [inmem] ...and %d further body(ies) did not fit the table at all -- the line "
                    "above UNDERCOUNTS and the interlock is INCOMPLETE. Raise MAX_PATCHED.\n",
                    g_patched_dropped);
        say(over);
    }
}

int promoted_entries(uintptr_t *out, int max) {
    const int n = (g_promoted_n < max) ? g_promoted_n : max;
    for (int i = 0; i < n; ++i) out[i] = g_promoted[i];
    return n;
}

const owner_range *owner_range_of_entry(uintptr_t entry) {
    for (int i = 0; i < g_table_n; ++i)
        if (g_table[i].entry == entry) return &g_table[i];
    return nullptr;
}

const owner_range *owner_range_by_name(const char *name) {
    if (!name) return nullptr;
    for (int i = 0; i < g_table_n; ++i)
        if (strcmp(g_table[i].name, name) == 0) return &g_table[i];
    return nullptr;
}

const owner_range *owner_table(int *count) {
    if (count) *count = g_table ? g_table_n : 0;
    return g_table;
}

void note_promoted(uintptr_t entry) {
    for (int i = 0; i < g_promoted_n; ++i)
        if (g_promoted[i] == entry) return;
    if (g_promoted_n >= MAX_PROMOTED) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; [promote] INTERLOCK OVERFLOW -- more than %d promotions; patch suppression is now "
                    "INCOMPLETE for %08X\n",
                    MAX_PROMOTED, (unsigned)entry);
        say(line);
        return;
    }
    g_promoted[g_promoted_n++] = entry;
}

void note_entry_owner(uintptr_t entry, const char *owner) { note_entry_claim(entry, owner, false); }

void note_entry_claim(uintptr_t entry, const char *owner, bool shared) {
    for (int i = 0; i < g_owned_n; ++i)
        if (g_owned[i].entry == entry) {
            g_owned[i].owner = owner; // last claim wins; the log below makes a re-claim visible
            // An EXCLUSIVE claim over a shared one downgrades the row to exclusive, never the reverse:
            // once some site has demanded the entry outright, that is the stronger fact about it.
            if (!shared) g_owned[i].shared = false;
            return;
        }
    if (g_owned_n >= MAX_OWNED) {
        // ONCE, AND AS A RUN-INVALIDATING VERDICT. This used to log per dropped claim, and the
        // mitigation the sizing comment relied on ("if it ever fills, the log says so") is exactly what
        // failed: after clause 6 made shared claims recordable the table overflowed on EVERY run and
        // emitted 241 identical lines, which is indistinguishable from noise and went unread for hours
        // across the A/B, the determinism gate and the UI suite. Two consequences were live the whole
        // time and neither failed anything: detour_refusal stopped refusing a second patch on the
        // dropped entries, and clause 6's derived yield could not see them -- reintroducing, as a
        // capacity limit, precisely the hand-list fragility that clause exists to remove.
        //
        // So: one unmistakable line carrying the PHRASE THE GATES GREP FOR, then silence, and a count
        // callers can read. A dropped claim is not a diagnostic, it is an invalid run.
        ++g_owned_dropped;
        if (g_owned_dropped == 1) {
            char line[256];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [owner] TABLE FULL (%d slots) at %08X -- PARTIAL, treat this run as "
                        "invalid: further claims are DROPPED, so detour_refusal stops refusing a "
                        "second patch on them and clause 6's derived yield cannot see them\n",
                        MAX_OWNED, (unsigned)entry);
            say(line);
        }
        return;
    }
    g_owned[g_owned_n].entry  = entry;
    g_owned[g_owned_n].owner  = owner;
    g_owned[g_owned_n].shared = shared;
    ++g_owned_n;
}

const char *entry_owner_of(uintptr_t entry) {
    for (int i = 0; i < g_owned_n; ++i)
        if (g_owned[i].entry == entry && !g_owned[i].shared) return g_owned[i].owner;
    return nullptr;
}

int entry_claim_count() { return g_owned_n; }

bool entry_claim_at(int i, uintptr_t *entry, const char **owner, bool *shared) {
    if (i < 0 || i >= g_owned_n) return false;
    if (entry) *entry = g_owned[i].entry;
    if (owner) *owner = g_owned[i].owner;
    if (shared) *shared = g_owned[i].shared;
    return true;
}

// ANY claimant, exclusive or shared. This is the one clause 6 needs: what disqualifies a row from the
// libmh rebind is that SOME instrument sits on its entry, and whether that instrument was willing to
// share is beside the point -- a rebound caller calls our body directly and reaches neither kind.
const char *entry_claimant_of(uintptr_t entry) {
    for (int i = 0; i < g_owned_n; ++i)
        if (g_owned[i].entry == entry) return g_owned[i].owner;
    return nullptr;
}

int entry_refusal_count() { return g_refused_n + g_refused_lost; }

void note_entry_refusal(uintptr_t target, const char *who, refuse_reason why) {
    if (why == refuse_reason::none) return; // not a refusal; recording it would inflate the count
    for (int i = 0; i < g_refused_n; ++i)
        if (g_refused[i].target == target && g_refused[i].why == why) return; // idempotent
    if (g_refused_n >= MAX_REFUSED) {
        ++g_refused_lost;
        char line[192];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; [interlock] REFUSAL TABLE FULL (%d) -- %08X (%s, %s) is NOT in the summary "
                    "below, which is therefore INCOMPLETE. Raise MAX_REFUSED.\n",
                    MAX_REFUSED,
                    (unsigned)target, who ? who : "an unnamed DLL detour",
                    reason_str(why));
        say(line);
        return;
    }
    g_refused[g_refused_n].target = target;
    g_refused[g_refused_n].who    = who;
    g_refused[g_refused_n].why    = why;
    ++g_refused_n;
}

void report_entry_refusals() {
    // THE AFFIRMATIVE CASE IS NOT OPTIONAL. U30 was a mechanism that disarmed itself and said so in
    // one line nobody read; a report that only speaks when something is wrong has the same failure
    // mode one level up -- an absent line is unreadable evidence. So a clean run states it.
    if (g_refused_n == 0 && g_refused_lost == 0) {
        say("; [interlock] 0 detour install(s) refused -- every install_jmp/install_trampoline site "
            "that asked for an entry in this run got it\n");
        return;
    }
    char   line[1024];
    size_t n = 0;
    {
        const int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                  "; [interlock] %d detour install(s) REFUSED -- whatever fix each "
                                  "carried IS NOT IN THIS RUN:",
                                  g_refused_n);
        n           = (k > 0) ? (size_t)k : 0;
    }
    for (int i = 0; i < g_refused_n; ++i) {
        char one[192];
        _snprintf_s(one, sizeof(one), _TRUNCATE, " [%s @%08X: %s]",
                    g_refused[i].who ? g_refused[i].who : "an unnamed DLL detour",
                    (unsigned)g_refused[i].target, reason_str(g_refused[i].why));
        const size_t need = strlen(one);
        // Never truncate silently -- an elided refusal is the bug, not the cure for it.
        if (n + need + 32 >= sizeof(line)) {
            _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " ... +%d MORE, elided (line cap)",
                        g_refused_n - i);
            n = strlen(line);
            break;
        }
        memcpy(line + n, one, need + 1);
        n += need;
    }
    if (n + 2 < sizeof(line)) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    say(line);
    if (g_refused_lost) {
        char over[160];
        _snprintf_s(over, sizeof(over), _TRUNCATE,
                    "; [interlock] ...and %d further refusal(s) did not fit the table at all -- the "
                    "line above UNDERCOUNTS. Raise MAX_REFUSED.\n",
                    g_refused_lost);
        say(over);
    }
}

void report_interlock(const patch_decl *tbl, int count) {
    if (g_promoted_n == 0) {
        say("; [interlock] no promotions in this run -- every registered patch owns its bytes\n");
        return;
    }
    int collisions = 0;
    for (int i = 0; i < count; ++i) {
        const patch_decl &p = tbl[i];
        if (!promoted_owner_of(p.addr)) continue;
        ++collisions;
        char line[288];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; [interlock] fix '%s' at %08X is inside PROMOTED %s -- the byte patch was NOT "
                    "written; carrier = %s\n",
                    p.id, (unsigned)p.addr, p.owner, p.carrier ? p.carrier : "NONE DECLARED");
        say(line);
    }
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [interlock] %d promotion(s) live, %d registered fix(es) displaced by them\n", g_promoted_n,
                collisions);
    say(line);
}

const char *promoted_owner_of(uintptr_t addr) {
    if (!g_table || g_promoted_n == 0) return nullptr;
    for (int i = 0; i < g_table_n; ++i) {
        const owner_range &r = g_table[i];
        if (addr < r.entry || addr > r.end) continue;
        for (int k = 0; k < g_promoted_n; ++k)
            if (g_promoted[k] == r.entry) return r.name;
        return nullptr; // owned, but that owner is not promoted in this run -- the write is fine
    }
    return nullptr;
}

} // namespace mh::hook
