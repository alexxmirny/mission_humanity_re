//
// tombstone_selftest.cpp -- X-TOMB's arming DECISION, off the rig (the endgame plan D-E2).
//
// WHY A UNIT TEST AND NOT ONLY A RIG RUN. The rig IS the acceptance for the instrument as a whole:
// a full session with tombstones armed and zero hits. But a zero-hit run cannot demonstrate the
// arming decision -- which bodies got armed, over what extent, with which boundaries, and which were
// correctly SKIPPED -- and a decision that armed nothing, or armed the wrong extent, would also
// produce a green run. That is the interlock_selftest lesson applied to the mirror instrument: the
// half that can be wrong is the decision, so the decision is exercised here directly.
//
// The trap fill is injected (set_tomb_mem_ops) so no real byte is written; the extent table is
// synthetic (set_owner_table) so the test states its own premises instead of inheriting whatever the
// build currently promotes; and the report is captured (set_promotion_logger) because a summary that
// is silent or miscounts looks identical to a healthy one from outside.
//
#include "hook/promoted.h"
#include "hook/tombstone.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using mh::hook::note_promoted;
using mh::hook::owner_range;
using mh::hook::set_owner_table;

namespace {

int  g_checks = 0, g_fails = 0;
void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// Synthetic promotable bodies. Each `entry` points at a real test-owned buffer so the promoted-fill
// path's live-redirect check (`*(uint8_t*)entry == 0xE9`) reads a byte the test controls; `end` is a
// fabricated VA past it (the fill is injected, so end need not be a real address). fn_a carries a
// real E9 at its entry (a promoted body); fn_c does NOT (a noted-but-not-redirected body, which the
// fill must refuse to touch). fn_b is only ever a force_arm / dead target.
uint8_t g_a_entry[16], g_b_entry[16], g_c_entry[16];

owner_range g_ranges[3];
void        build_ranges() {
    memset(g_a_entry, 0xCC, sizeof(g_a_entry));
    g_a_entry[0] = 0xE9; // fn_a is genuinely redirected
    memset(g_b_entry, 0xCC, sizeof(g_b_entry));
    g_b_entry[0] = 0xE9;
    memset(g_c_entry, 0x55, sizeof(g_c_entry)); // fn_c: a Watcom prologue, NOT a redirect
    g_ranges[0] = {"fn_a", (uintptr_t)g_a_entry, (uintptr_t)g_a_entry + 0x100};
    g_ranges[1] = {"fn_b", (uintptr_t)g_b_entry, (uintptr_t)g_b_entry + 0x100};
    g_ranges[2] = {"fn_c", (uintptr_t)g_c_entry, (uintptr_t)g_c_entry + 0x100};
}

// The injected fill: record (lo,hi) rather than write, and report success. A separate "refuse" mode
// drives the VirtualProtect-declined skip arm.
struct fill_rec {
    uintptr_t lo, hi;
};
fill_rec g_fills[16];
int      g_fill_n      = 0;
bool     g_fill_refuse = false;
bool     rec_fill(uintptr_t lo, uintptr_t hi) {
    if (g_fill_refuse) return false;
    if (g_fill_n < 16) {
        g_fills[g_fill_n].lo = lo;
        g_fills[g_fill_n].hi = hi;
        ++g_fill_n;
    }
    return true;
}
const mh::hook::tomb_mem_ops REC_OPS = {&rec_fill};
void                         fills_reset() { g_fill_n = 0; }
bool                         filled(uintptr_t lo, uintptr_t hi) {
    for (int i = 0; i < g_fill_n; ++i)
        if (g_fills[i].lo == lo && g_fills[i].hi == hi) return true;
    return false;
}

char g_cap[8192];
void cap_log(const char *line) {
    const size_t used = strlen(g_cap), n = strlen(line);
    if (used + n + 1 < sizeof(g_cap)) memcpy(g_cap + used, line, n + 1);
}
void cap_reset() { g_cap[0] = 0; }
bool cap_has(const char *needle) { return strstr(g_cap, needle) != nullptr; }

// A capturing fail handler that RETURNS -- the VEH then declines the exception. Used to prove the
// hit path names the right body without terminating the test process.
char g_hitline[512];
void cap_fail(const char *line) { strncpy_s(g_hitline, sizeof(g_hitline), line, _TRUNCATE); }

void reset_all() {
    mh::hook::tomb_reset_for_test();
    mh::hook::registry_reset_for_test(); // clear the process-global promoted/owned/refusal registries
    set_owner_table(g_ranges, 3);        // registry reset does not touch the extent table, but a
                                         // reset for symmetry keeps each scenario self-contained
    fills_reset();
    cap_reset();
    g_hitline[0]  = 0;
    g_fill_refuse = false;
}

} // namespace

int run_tombstonetest() {
    printf("== tombstone selftest (X-TOMB arming decision) ==\n");
    build_ranges();
    set_owner_table(g_ranges, 3);
    mh::hook::set_promotion_logger(cap_log);
    mh::hook::set_tomb_mem_ops(&REC_OPS);

    const uintptr_t A = (uintptr_t)g_a_entry, B = (uintptr_t)g_b_entry, C = (uintptr_t)g_c_entry;

    // ---- scenario 1: DISABLED. enable=0 arms nothing and says so affirmatively (a silent
    // instrument is the U30 shape).
    reset_all();
    {
        mh::hook::tomb_options opt{}; // all false
        ck(mh::hook::tomb_install(nullptr, 0, opt) == 0, "disabled: nothing armed");
        mh::hook::tomb_report();
        ck(cap_has("not armed"), "disabled: report states the instrument is off");
        ck(g_fill_n == 0, "disabled: no fill attempted");
    }

    // ---- scenario 2: PROMOTED fill. Promote fn_a only; arm_promoted fills entry+8..end and leaves
    // the 8-byte live redirect untouched. fn_b is not promoted -> not armed.
    reset_all();
    note_promoted(A); // fn_a is the only promotion in this run
    {
        mh::hook::tomb_options opt{};
        opt.arm_promoted = true;
        const int n      = mh::hook::tomb_install(nullptr, 0, opt);
        ck(n == 1, "promoted: exactly fn_a armed");
        ck(filled(A + 8, A + 0x100), "promoted: fill starts at entry+8 (redirect preserved) to end");
        ck(!filled(A, A + 0x100), "promoted: the live entry bytes were NOT filled");
        // The armed-range membership the VEH depends on: inside the fill is fn_a, the 8-byte redirect
        // gap is NOT armed, and fn_b (un-promoted) is nowhere.
        ck(mh::hook::tomb_armed_name(A + 8) != nullptr, "promoted: entry+8 is armed");
        ck(strcmp(mh::hook::tomb_armed_name(A + 8), "fn_a") == 0, "promoted: names fn_a");
        ck(mh::hook::tomb_armed_name(A + 0x100) != nullptr, "promoted: end byte is armed (inclusive)");
        ck(mh::hook::tomb_armed_name(A + 0x101) == nullptr, "promoted: end+1 is NOT armed");
        ck(mh::hook::tomb_armed_name(A + 4) == nullptr, "promoted: the redirect gap is NOT armed");
        ck(mh::hook::tomb_armed_name(B + 8) == nullptr, "promoted: un-promoted fn_b is NOT armed");
    }

    // ---- scenario 3: the HIT path names the entered body, and a returning handler lets the VEH
    // decline (so a real INT3 in a filled range would reach the app's own SEH). Driven by the range
    // lookup the VEH uses, plus the handler seam -- not by raising a real breakpoint, which the
    // injected fill deliberately never wrote.
    {
        mh::hook::set_tomb_fail_handler(cap_fail);
        // The name lookup is what the VEH formats its line from; assert it resolves mid-body.
        ck(mh::hook::tomb_armed_name(A + 0x40) != nullptr && strcmp(mh::hook::tomb_armed_name(A + 0x40), "fn_a") == 0,
           "hit: a mid-body address resolves to fn_a for the report line");
        ck(mh::hook::tomb_armed_name(C) == nullptr, "hit: an unarmed address resolves to nothing");
        mh::hook::set_tomb_fail_handler(nullptr);
    }

    // ---- scenario 4: a NOTED body whose entry is NOT an E9 redirect is refused (a rebind-style
    // promotion this fill must not touch). fn_c's entry byte is 0x55.
    reset_all();
    note_promoted(C);
    {
        mh::hook::tomb_options opt{};
        opt.arm_promoted = true;
        const int n      = mh::hook::tomb_install(nullptr, 0, opt);
        ck(n == 0, "not-redirected: a noted body with no E9 entry is NOT filled");
        mh::hook::tomb_report(); // skips are surfaced at report time, not install time
        ck(cap_has("not an E9 redirect"), "not-redirected: the skip is reported by name");
        ck(g_fill_n == 0, "not-redirected: no fill attempted");
    }

    // ---- scenario 5: DEAD table. Filled whole (entry..end, no redirect gap), and skipped when a
    // detour owns the entry or a promotion covers it.
    reset_all();
    {
        const owner_range dead[] = {{"dead_x", B, B + 0x100}};
        mh::hook::note_entry_owner(B, "some diagnostic detour"); // B's entry is owned
        mh::hook::tomb_options opt{};
        opt.arm_dead = true;
        const int n  = mh::hook::tomb_install(dead, 1, opt);
        ck(n == 0, "dead: a body whose entry a detour owns is skipped, not filled");
        mh::hook::tomb_report(); // skips are surfaced at report time, not install time
        ck(cap_has("a DLL detour owns"), "dead: the owned-entry skip is named");
    }
    reset_all();
    {
        const owner_range      dead[] = {{"dead_y", C, C + 0x80}};
        mh::hook::tomb_options opt{};
        opt.arm_dead = true;
        const int n  = mh::hook::tomb_install(dead, 1, opt);
        ck(n == 1, "dead: a clean dead body is armed whole");
        ck(filled(C, C + 0x80), "dead: fill covers entry..end (no redirect gap)");
        ck(mh::hook::tomb_armed_name(C) != nullptr, "dead: the entry byte itself is armed");
    }
    // ---- scenario 5b: the SAFE-END cap. A dead body whose flat extent ends in a shared epilogue
    // tail (the 2026-09-01 llm_strat_ai_build_target_list false hit: two LIVE siblings JMP into
    // its last bytes) is armed only up to the cap -- the shared bytes are neither filled nor
    // claimed, so a live sibling's epilogue run cannot fire a false HIT.
    reset_all();
    {
        const owner_range      dead[] = {{"dead_tail", C, C + 0x80}};
        const owner_range      caps[] = {{"dead_tail", C, C + 0x70}}; // last 0x10 bytes are shared
        mh::hook::tomb_options opt{};
        opt.arm_dead       = true;
        opt.tail_caps      = caps;
        opt.tail_cap_count = 1;
        const int n        = mh::hook::tomb_install(dead, 1, opt);
        ck(n == 1, "cap: the clamped dead body still arms");
        ck(filled(C, C + 0x70), "cap: fill covers entry..safe_end");
        ck(!filled(C + 0x71, C + 0x80), "cap: the shared tail bytes are NOT filled");
        ck(mh::hook::tomb_armed_name(C + 0x70) != nullptr, "cap: the last safe byte is armed");
        ck(mh::hook::tomb_armed_name(C + 0x71) == nullptr,
           "cap: the first shared byte is UNARMED -- a sibling's epilogue run cannot false-hit");
    }

    // ---- scenario 6: FORCE-ARM (the NEGATIVE ARM). fn_b is NOT promoted; force_arm arms its
    // original body anyway, from entry+8, and the report says so loudly.
    reset_all();
    {
        mh::hook::tomb_options opt{};
        opt.force_arm = "fn_b";
        const int n   = mh::hook::tomb_install(nullptr, 0, opt);
        ck(n == 1, "force-arm: fn_b armed despite not being promoted");
        ck(filled(B + 8, B + 0x100), "force-arm: fills the ORIGINAL body from entry+8");
        ck(cap_has("FORCE-ARMED fn_b"), "force-arm: reported as a negative-arm demonstration");
        ck(mh::hook::tomb_armed_name(B + 0x10) != nullptr, "force-arm: fn_b body is armed");
    }
    // an unknown force_arm name is ignored with a line, not silently
    reset_all();
    {
        mh::hook::tomb_options opt{};
        opt.force_arm = "no_such_function";
        const int n   = mh::hook::tomb_install(nullptr, 0, opt);
        ck(n == 0, "force-arm: an unknown name arms nothing");
        ck(cap_has("NOT in the extent table"), "force-arm: the unknown name is reported");
    }

    // ---- scenario 6b: the DOMAIN sweep walks the VERIFIED SET, not just the extent table
    // (TACT1-P C4, 2026-09-04). The extent table is exactly the MH_EXPORT_REPLACE rows -- eleven of
    // tact's 98 -- so a sweep that only walked it armed a tenth of the domain and reported itself
    // armed. Here fn_c is verified-only (no extent row in the domain) and fn_b is in BOTH, which is
    // the double-count trap: the count is what a coverage clause reads, so the same body appearing
    // twice would inflate exactly the number being trusted.
    reset_all();
    {
        static owner_range verified[2] = {{"fn_b", B, B + 0x100, "tst"}, {"fn_c", C, C + 0x100, "tst"}};
        static owner_range extent[3]   = {{"fn_a", A, A + 0x100, nullptr},
                                          {"fn_b", B, B + 0x100, "tst"}, // also in `verified`
                                          {"fn_c", C, C + 0x100, nullptr}};
        set_owner_table(extent, 3);
        mh::hook::tomb_options opt{};
        opt.force_arm_domain = "tst";
        opt.verified_tbl     = verified;
        opt.verified_n       = 2;
        const int n          = mh::hook::tomb_install(nullptr, 0, opt);
        ck(n == 2, "domain sweep: both VERIFIED rows armed (fn_c has no domain extent row at all)");
        ck(cap_has("2 row(s) -- 2 from the VERIFIED set, 0 extent-only"),
           "domain sweep: fn_b is counted ONCE despite being in both tables");
        ck(mh::hook::tomb_armed_name(C + 0x10) != nullptr,
           "domain sweep: a verified-but-unpromoted body is armed -- the coverage claim C4 needs");
        set_owner_table(g_ranges, 3);
    }
    // A domain nobody owns must SAY it matched nothing -- an empty sweep reads exactly like a clean
    // one otherwise, which is the pass that proves nothing.
    reset_all();
    {
        mh::hook::tomb_options opt{};
        opt.force_arm_domain = "no_such_domain";
        ck(mh::hook::tomb_install(nullptr, 0, opt) == 0, "domain sweep: an unknown domain arms nothing");
        ck(cap_has("matched NO extent rows"), "domain sweep: the empty match is reported");
    }

    // ---- scenario 7: the REPORT is the output, so read it. An empty armed set must say so (a
    // zero-hit claim over nothing proves nothing); a non-empty one must state its size and names.
    reset_all();
    {
        mh::hook::tomb_options opt{};
        opt.arm_dead = true;
        mh::hook::tomb_install(nullptr, 0, opt); // arm_dead with an empty dead table -> 0 armed
        mh::hook::tomb_report();
        ck(cap_has("ARMED SET EMPTY"), "report: an empty armed set is called out, not passed");
    }
    reset_all();
    note_promoted(A);
    {
        mh::hook::tomb_options opt{};
        opt.arm_promoted = true;
        mh::hook::tomb_install(nullptr, 0, opt);
        mh::hook::tomb_report();
        ck(cap_has("1 bodies armed"), "report: states the armed count");
        ck(cap_has("fn_a"), "report: names the armed body");
    }

    // Leave production seams as production expects them.
    mh::hook::set_tomb_mem_ops(nullptr);
    mh::hook::set_tomb_fail_handler(nullptr);

    printf("tombstone selftest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
