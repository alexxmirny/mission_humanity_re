#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hook/tombstone.h"

namespace mh::hook {
namespace {

// Armed extents. 1280 covers the promotion wave's ~306 promoted entries (MAX_PROMOTED is 512) + the
// ~105 generated dead rows + a WHOLE-DOMAIN force-arm over the verified set, which is what the
// per-domain sweep now walks (TACT1-P C4): 936 verified rows program-wide, sim alone 530, so the
// previous 640 would have truncated a sim sweep. Like every table in this layer, overflow is
// REPORTED rather than wrapped -- a silently dropped tombstone is a hole in exactly the claim this
// instrument exists to test.
constexpr int MAX_ARMED = 1280;
struct armed_range {
    const char *name;
    uintptr_t   lo, hi; // the FILLED extent, inclusive -- lo is entry+8 for promoted/forced bodies
    char        kind;   // 'p' promoted-fill, 'd' dead-fill, 'f' force-armed (negative arm)
    const char *domain; // owner_range::domain, or nullptr for an unattributed row (dead table)
};
armed_range g_armed[MAX_ARMED];
int         g_armed_n    = 0;
int         g_armed_lost = 0;

// Skips, so the report can say what was NOT armed and why -- an absent tombstone is unreadable
// evidence otherwise.
constexpr int MAX_SKIPPED = 64;
struct skipped_range {
    const char *name;
    const char *why;
};
skipped_range g_skipped[MAX_SKIPPED];
int           g_skipped_n = 0;

int  g_hits      = 0;
bool g_installed = false;

void (*g_fail)(const char *)  = nullptr;
const tomb_mem_ops *g_mem_ops = nullptr;

void default_fail(const char *line) {
    promotion_log(line);
    // Fail FAST and distinctively. The body is trap-filled; there is nothing to resume into, and a
    // run that entered a tombstoned body has already executed code we claim never executes.
    TerminateProcess(GetCurrentProcess(), 0xDEAD70B5u);
}

void skip(const char *name, const char *why) {
    if (g_skipped_n < MAX_SKIPPED) {
        g_skipped[g_skipped_n].name = name;
        g_skipped[g_skipped_n].why  = why;
        ++g_skipped_n;
    }
}

// Fill [lo, hi] with INT3. The pages are the game image's .text (PAGE_EXECUTE_READ); restore the
// old protection afterwards and flush the icache so a stale decoded original cannot linger. Routed
// through g_mem_ops when a test has installed one, so the arming decision is drivable off-rig.
bool fill_cc(uintptr_t lo, uintptr_t hi) {
    if (g_mem_ops && g_mem_ops->fill) return g_mem_ops->fill(lo, hi);
    const SIZE_T len = (SIZE_T)(hi - lo + 1);
    DWORD        old = 0;
    if (!VirtualProtect((void *)lo, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memset((void *)lo, 0xCC, len);
    VirtualProtect((void *)lo, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)lo, len);
    return true;
}

bool arm(const char *name, uintptr_t lo, uintptr_t hi, char kind, const char *domain) {
    if (lo > hi) {
        skip(name, "body <= 8 bytes -- nothing beyond the redirect to fill");
        return false;
    }
    if (g_armed_n >= MAX_ARMED) {
        ++g_armed_lost;
        return false;
    }
    if (!fill_cc(lo, hi)) {
        skip(name, "VirtualProtect refused the body (U31-class environment failure)");
        return false;
    }
    g_armed[g_armed_n].name   = name;
    g_armed[g_armed_n].lo     = lo;
    g_armed[g_armed_n].hi     = hi;
    g_armed[g_armed_n].kind   = kind;
    g_armed[g_armed_n].domain = domain;
    ++g_armed_n;
    return true;
}

const armed_range *armed_at(uintptr_t addr) {
    for (int i = 0; i < g_armed_n; ++i)
        if (addr >= g_armed[i].lo && addr <= g_armed[i].hi) return &g_armed[i];
    return nullptr;
}

const char *kind_str(char k) {
    switch (k) {
        case 'p': return "promoted";
        case 'd': return "ledger-dead";
        case 'f': return "FORCE-ARMED (negative arm)";
        default: return "?";
    }
}

LONG WINAPI tomb_veh(EXCEPTION_POINTERS *xp) {
    if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t    eip = (uintptr_t)xp->ContextRecord->Eip;
    const armed_range *r   = armed_at(eip);
    if (!r) return EXCEPTION_CONTINUE_SEARCH; // someone else's breakpoint -- not ours to explain

    ++g_hits;
    // Best-effort return address: on entry-via-CALL the caller's return slot is at [esp]; deeper
    // landings make it whatever the stack holds, which is still the single most useful word.
    uintptr_t ret = 0;
    __try {
        ret = *(const uintptr_t *)xp->ContextRecord->Esp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ret = 0;
    }
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [tombstone] HIT -- %s @%08X ENTERED (armed as %s): an original body we claim is "
                "never executed just executed. [esp]=%08X. A caller we missed, a dispatch table "
                "still holding the original, or a wrong `dead` adjudication -- this run is invalid "
                "and terminates now.\n",
                r->name, (unsigned)eip, kind_str(r->kind), (unsigned)ret);
    if (g_fail) {
        g_fail(line);
        // A test handler RETURNS: decline the exception so the test's own SEH catches it.
        return EXCEPTION_CONTINUE_SEARCH;
    }
    default_fail(line); // does not return
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void set_tomb_fail_handler(void (*fn)(const char *)) { g_fail = fn; }
void set_tomb_mem_ops(const tomb_mem_ops *ops) { g_mem_ops = ops; }

int tomb_armed_count() { return g_armed_n; }
int tomb_hit_count() { return g_hits; }

const char *tomb_armed_name(uintptr_t addr) {
    const armed_range *r = armed_at(addr);
    return r ? r->name : nullptr;
}

void tomb_reset_for_test() {
    g_armed_n = g_armed_lost = g_skipped_n = g_hits = 0;
    g_installed                                     = false;
}

// Clamp an arm's high bound to the function's safe end when its flat extent contains bytes that
// are not exclusively its own (tomb_options::tail_caps; the 2026-09-01 shared-tail false hit).
// Is `entry` a row of `tbl`? Used to count a body ONCE when it appears in both the verified set and
// the extent table (the domain sweep walks both).
static bool in_table(const owner_range *tbl, int n, uintptr_t entry) {
    for (int i = 0; i < n; ++i)
        if (tbl[i].entry == entry) return true;
    return false;
}

// The section-5 residue row covering `entry`, or nullptr. Consulted by BOTH force-arm paths.
static const residue_row *residue_of(const tomb_options &opt, uintptr_t entry) {
    for (int i = 0; i < opt.residue_count; ++i)
        if (opt.residue[i].entry == entry) return &opt.residue[i];
    return nullptr;
}

// Announce one exclusion. UNCONDITIONAL and per row, for the same reason the armed set names itself:
// a skip that does not print is indistinguishable from a body nobody thought to arm, and the whole
// value of narrowing the set is that the narrowing is auditable.
static void log_residue_skip(const residue_row *r) {
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [tombstone] EXCLUDED %s -- dispositioned section-5 residue (%s): %s. Its "
                "ORIGINAL is reachable from host code ON PURPOSE, so trapping it would make this "
                "sweep's colour depend on the scenario rather than on the claim.\n",
                r->name, r->cls, r->why);
    promotion_log(line);
}

static uintptr_t cap_hi(const tomb_options &opt, uintptr_t entry, uintptr_t hi) {
    for (int i = 0; i < opt.tail_cap_count; ++i)
        if (opt.tail_caps[i].entry == entry) return opt.tail_caps[i].end < hi ? opt.tail_caps[i].end : hi;
    return hi;
}

int tomb_install(const owner_range *dead_tbl, int dead_n, const tomb_options &opt) {
    if (g_installed) {
        promotion_log("; [tombstone] tomb_install called TWICE -- second call refused\n");
        return g_armed_n;
    }
    if (!opt.arm_promoted && !opt.arm_dead && !opt.force_arm && !opt.force_arm_domain)
        return 0; // disabled; report says so
    g_installed = true;
    AddVectoredExceptionHandler(1, tomb_veh);

    if (opt.arm_promoted) {
        // Sized to hook/promoted.cpp's MAX_PROMOTED (512 since the 2026-09-01 promotion wave: the
        // three whole-domain arms total ~306 entries and the old 256 here would have truncated the
        // fill silently -- a tombstone hole over exactly the newest promotions).
        uintptr_t entries[512];
        const int n = promoted_entries(entries, 512);
        for (int i = 0; i < n; ++i) {
            const owner_range *r = owner_range_of_entry(entries[i]);
            if (!r) continue; // promoted outside the extent table (no MH_EXPORT_REPLACE row) -- the
                              // reconciliation report is where that mismatch belongs, not a fill
            // The first 8 bytes are the live redirect (E9 rel32 + 3 NOPs); everything after is the
            // dead remainder. Verify the redirect is really there -- a noted entry whose first byte
            // is not E9 is a rebind-style promotion this fill must not touch.
            if (*(const unsigned char *)r->entry != 0xE9) {
                skip(r->name, "noted promoted but entry is not an E9 redirect -- not filled");
                continue;
            }
            arm(r->name, r->entry + 8, cap_hi(opt, r->entry, r->end), 'p', r->domain);
        }
    }

    if (opt.arm_dead && dead_tbl) {
        for (int i = 0; i < dead_n; ++i) {
            const owner_range &r = dead_tbl[i];
            if (entry_owner_of(r.entry)) {
                skip(r.name, "a DLL detour owns this dead body's entry -- not filled");
                continue;
            }
            if (promoted_owner_of(r.entry)) {
                skip(r.name, "inside a promoted body -- the promoted fill covers it");
                continue;
            }
            arm(r.name, r.entry, cap_hi(opt, r.entry, r.end), 'd', r.domain);
        }
    }

    if (opt.force_arm && opt.force_arm[0]) {
        // Comma-separated names, parsed destructively into a bounded local copy.
        char buf[512];
        strncpy_s(buf, sizeof(buf), opt.force_arm, _TRUNCATE);
        char *ctx  = nullptr;
        char *name = strtok_s(buf, ", ", &ctx);
        while (name) {
            const owner_range *r = owner_range_by_name(name);
            if (!r) {
                promotion_log("; [tombstone] force_arm names a function NOT in the extent table -- "
                              "ignored (check the spelling against mh_patches.gen.h)\n");
            } else if (armed_at(r->entry + 8)) {
                promotion_log("; [tombstone] force_arm target is already armed -- nothing extra to do\n");
            } else if (const residue_row *res = residue_of(opt, r->entry)) {
                // A BY-NAME force_arm of a residue row is refused too, not just the domain sweep.
                // The negative-arm fragments name rows explicitly, and silently arming one here
                // would reintroduce the scenario-dependent red through the other door.
                log_residue_skip(res);
            } else {
                // Keep the entry bytes LIVE: the unpromoted prologue runs, then walks into the trap.
                if (arm(r->name, r->entry + 8, cap_hi(opt, r->entry, r->end), 'f', r->domain)) {
                    char line[256];
                    _snprintf_s(line, sizeof(line), _TRUNCATE,
                                "; [tombstone] FORCE-ARMED %s -- its ORIGINAL body is live and now "
                                "trapped; this is a negative-arm demonstration run and will "
                                "terminate on the first call\n",
                                r->name);
                    promotion_log(line);
                }
            }
            name = strtok_s(nullptr, ", ", &ctx);
        }
    }

    // The same negative arm, BY DOMAIN. Walks the extent table rather than a name list, so a
    // whole-domain sweep cannot truncate the way a hand-written `force_arm=` of several kilobytes
    // would -- and truncation here is the worst available failure, an under-armed set that reports
    // itself armed. Rows already covered by the promoted fill are LEFT ALONE rather than counted
    // twice: a promoted body's remainder and a force-armed original are different claims, and
    // merging them would make the per-domain force-arm count unreadable.
    if (opt.force_arm_domain && opt.force_arm_domain[0]) {
        char buf[256];
        strncpy_s(buf, sizeof(buf), opt.force_arm_domain, _TRUNCATE);
        char       *ctx   = nullptr;
        char       *dom   = strtok_s(buf, ", ", &ctx);
        int         tbl_n = 0;
        const auto *tbl   = owner_table(&tbl_n);
        while (dom) {
            int matched = 0, armed_here = 0, from_verified = 0, excluded_here = 0;
            // THE VERIFIED SET FIRST, and it is the one that makes the count mean anything: the
            // extent table below holds only rows carrying an MH_EXPORT_REPLACE, so on its own a
            // domain sweep arms the promoted handful and reports itself armed (TACT1-P C4).
            for (int i = 0; i < opt.verified_n; ++i) {
                const owner_range &r = opt.verified_tbl[i];
                if (!r.domain || strcmp(r.domain, dom) != 0) continue;
                ++matched;
                ++from_verified;
                if (armed_at(r.entry + 8)) continue; // the promoted fill already covers it
                if (const residue_row *res = residue_of(opt, r.entry)) {
                    log_residue_skip(res);
                    ++excluded_here;
                    continue;
                }
                if (arm(r.name, r.entry + 8, cap_hi(opt, r.entry, r.end), 'f', r.domain)) ++armed_here;
            }
            for (int i = 0; i < tbl_n; ++i) {
                const owner_range &r = tbl[i];
                if (!r.domain || strcmp(r.domain, dom) != 0) continue;
                // Counted once. A row present in both tables is the same body, and double-counting
                // it would inflate exactly the number this clause reads as coverage.
                if (in_table(opt.verified_tbl, opt.verified_n, r.entry)) continue;
                ++matched;
                if (armed_at(r.entry + 8)) continue;
                if (const residue_row *res = residue_of(opt, r.entry)) {
                    log_residue_skip(res);
                    ++excluded_here;
                    continue;
                }
                if (arm(r.name, r.entry + 8, cap_hi(opt, r.entry, r.end), 'f', r.domain)) ++armed_here;
            }
            char line[256];
            if (!matched)
                // NAME A DOMAIN THAT MATCHES NOTHING AND SAY SO. An empty sweep is silent otherwise,
                // and silence here reads exactly like a clean one.
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "; [tombstone] force_arm_domain=%s matched NO extent rows -- a zero-hit "
                            "claim over this domain proves NOTHING (check the name against "
                            "mh_patches.gen.h's fourth column)\n",
                            dom);
            else
                // `excluded` is broken out of the "already covered" bucket on purpose: those two are
                // opposite facts. A row the promoted fill covers IS trapped; a residue row is
                // deliberately NOT, and folding it into the same number would hide the one thing a
                // reader of this line has to be able to subtract.
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "; [tombstone] force_arm_domain=%s: %d row(s) -- %d from the VERIFIED set, "
                            "%d extent-only; %d newly FORCE-ARMED (%d already covered by the promoted "
                            "fill, %d EXCLUDED as dispositioned section-5 residue)\n",
                            dom, matched, from_verified, matched - from_verified, armed_here,
                            matched - armed_here - excluded_here, excluded_here);
            promotion_log(line);
            dom = strtok_s(nullptr, ", ", &ctx);
        }
    }
    return g_armed_n;
}

void tomb_report() {
    // The affirmative case is not optional -- report_entry_refusals' discipline, same reason.
    if (!g_installed) {
        promotion_log("; [tombstone] not armed ([tombstone] enable=0) -- no dead-body claim is being "
                      "checked this run\n");
        return;
    }
    int np = 0, nd = 0, nf = 0;
    for (int i = 0; i < g_armed_n; ++i) {
        if (g_armed[i].kind == 'p') ++np;
        else if (g_armed[i].kind == 'd') ++nd;
        else ++nf;
    }
    char head[256];
    _snprintf_s(head, sizeof(head), _TRUNCATE,
                "; [tombstone] %d bodies armed (%d promoted-fill, %d ledger-dead, %d force-armed), "
                "%d skipped, %d hits so far%s\n",
                g_armed_n, np, nd, nf, g_skipped_n, g_hits,
                g_armed_n == 0 ? " -- ARMED SET EMPTY: a zero-hit claim over it proves NOTHING" : "");
    promotion_log(head);
    // PER DOMAIN, and it is not decoration. The total above reads identically whether two domains
    // contributed or one contributed everything and the other nothing -- and "this domain armed
    // nothing" is the coverage hole the instrument exists to find, not a detail. So the breakout is
    // emitted unconditionally, and a domain at zero is stated in the same words the empty-set case
    // uses rather than being absent from the list.
    {
        const char *doms[16];
        int         dn = 0, dp[16] = {0}, df[16] = {0}, dd[16] = {0};
        int         unattributed = 0;
        for (int i = 0; i < g_armed_n; ++i) {
            const char *d = g_armed[i].domain;
            if (!d || !d[0]) {
                ++unattributed;
                continue;
            }
            int j = 0;
            for (; j < dn; ++j)
                if (strcmp(doms[j], d) == 0) break;
            if (j == dn) {
                if (dn >= 16) continue; // more domains than the breakout can hold; total still stands
                doms[dn++] = d;
            }
            if (g_armed[i].kind == 'p') ++dp[j];
            else if (g_armed[i].kind == 'd') ++dd[j];
            else ++df[j];
        }
        for (int j = 0; j < dn; ++j) {
            char line[224];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [tombstone] domain %s: %d armed (%d promoted-fill, %d ledger-dead, %d "
                        "force-armed)%s\n",
                        doms[j], dp[j] + dd[j] + df[j], dp[j], dd[j], df[j],
                        (dp[j] + dd[j] + df[j]) == 0
                            ? " -- ZERO for this domain: a no-hit claim over it proves NOTHING"
                            : "");
            promotion_log(line);
        }
        if (unattributed) {
            char line[192];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "; [tombstone] domain (unattributed): %d armed -- extent rows with no domain "
                        "column, i.e. the generated dead-body table; NOT evidence about any domain\n",
                        unattributed);
            promotion_log(line);
        }
    }
    if (g_armed_lost) {
        char over[160];
        _snprintf_s(over, sizeof(over), _TRUNCATE,
                    "; [tombstone] ...and %d further bodies did not fit the table -- the armed set "
                    "UNDERCOUNTS. Raise MAX_ARMED.\n",
                    g_armed_lost);
        promotion_log(over);
    }
    // Names, chunked -- the done_when wants the armed set NAMED, and a 100+-name line would be
    // truncated into exactly the silence the summary exists to end.
    char   line[1024];
    size_t n = 0;
    for (int i = 0; i < g_armed_n; ++i) {
        if (n == 0) {
            const int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "; [tombstone] armed:");
            n           = (k > 0) ? (size_t)k : 0;
        }
        char one[96];
        _snprintf_s(one, sizeof(one), _TRUNCATE, " %s(%c)", g_armed[i].name, g_armed[i].kind);
        const size_t need = strlen(one);
        if (n + need + 2 >= sizeof(line)) {
            line[n]     = '\n';
            line[n + 1] = 0;
            promotion_log(line);
            n = 0;
            --i; // re-emit this name on the next line
            continue;
        }
        memcpy(line + n, one, need + 1);
        n += need;
    }
    if (n) {
        line[n]     = '\n';
        line[n + 1] = 0;
        promotion_log(line);
    }
    for (int i = 0; i < g_skipped_n; ++i) {
        char one[256];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "; [tombstone] skipped %s -- %s\n", g_skipped[i].name,
                    g_skipped[i].why);
        promotion_log(one);
    }
}

} // namespace mh::hook
