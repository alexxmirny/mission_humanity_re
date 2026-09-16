//
// state/rebind_arming.cpp -- R11's runtime: the rebind's arm bitmap, its yields, its report, trap_hit.
//
// WHAT THIS IS AFTER THE F2E COLLAPSE. It used to be a per-row GATE: a ship default constant
// (SHIP_REBIND_DEFAULT) plus a `[rebind] <callee>=0|1` section that overrode it row by row, read
// once into a bitmap. The section and the constant are gone with the rest of the per-key
// vocabulary -- which row binds ours is `[config] mode` and nothing else, exactly as for every
// promotion.
//
// THE BITMAP SURVIVED THE GATE, AND DELETING IT WOULD HAVE BEEN THE BUG. `armed(row)` looks like it
// could collapse to `mh::config::ours_run()` at each call site, and it cannot, because arming is not
// uniform across rows for a reason that has nothing to do with configuration: an entry some
// INSTRUMENT owns must not also be rebound. A rebound caller calls our body directly and never
// reaches the entry hook, so the instrument silently stops seeing the calls -- the wall-clock pin
// under `pin_wallclock`, and every row a harness detour claimed (whose detour carries the per-step
// trajectory hash, so a bypass VOIDS a determinism run rather than failing it). Both were found by
// bisecting all 676 rows against a red gate, not by reading the code. harness.cpp derives that yield
// set from the claim table and clears those rows through set_armed(); the bitmap is what it clears.
//
// SO THE TWO ENTRY POINTS NAME WHAT THEY DO. arm_from_config() is the game's: every row follows the
// selector. arm_none() is the offline oracle's: net_selftest drives module bodies through recording
// stubs and must arm nothing, and it says so in one call rather than passing a 0 that used to mean
// "a ship default of zero". Neither is optional -- armed() distinguishes "loaded, nothing armed"
// from "never loaded", and the second COMPLAINS by name rather than defaulting quietly, because a
// wrong answer that announces itself is recoverable and a quiet one is not (the G104 shape: a bind
// placed "at arm time" ran after arm-path code that already needed it).
//
// THE STANDALONE BUILD HAS NO INI AND NEEDS NONE: MH_LIBMH_BIND's standalone arm binds the target
// unconditionally and never calls armed(), so the whole thing collapses to a stub there. It is still
// compiled into libmh so report_arming() and trap_hit() exist for a standalone host to call.
//
#include "addr/mh_rebind.gen.h"

#include "config/config.h" // the arm report names the mode -- the log IS the evidence channel

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef MH_LIBMH_BUILD
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mh::rebind {
namespace {

constexpr int kRowCount  = static_cast<int>(sizeof(row_names) / sizeof(row_names[0]));
constexpr int kTrapCount = TRAP_DEFERRED_COUNT + TRAP_UNBINDABLE_COUNT;

static_assert(kRowCount == ROW_COUNT, "the row-name table and the row enum disagree");

bool g_armed[ROW_COUNT ? ROW_COUNT : 1];
bool g_loaded     = false;
bool g_complained = false;
int  g_armed_n    = 0;

void (*g_sink)(const char *) = nullptr;

void say(const char *line) {
    if (g_sink)
        g_sink(line);
    else
        std::fputs(line, stderr);
}

int row_of(const char *name) {
    for (int i = 0; i < ROW_COUNT; ++i)
        if (std::strcmp(row_names[i], name) == 0) return i;
    return -1;
}

} // namespace

namespace {

// The one place the bitmap is filled. `on` is the whole configuration question, already answered.
void arm_all(bool on) {
    std::memset(g_armed, on ? 1 : 0, sizeof(g_armed));
    g_armed_n = on ? ROW_COUNT : 0;
    g_loaded  = true;
}

} // namespace

void arm_from_config(bool ours) { arm_all(ours); }

// The offline oracle's arm: NOTHING, on purpose. net_selftest is a host of libmh too, and its suites
// drive module bodies through recording stubs -- a configured-armed gate would rebind callees
// underneath them and change what the stubs observe. It is a separate call rather than
// `arm_from_config(false)` because "this host arms nothing, by design" and "this run selected the
// original engine" are different statements, and the second one a reader would go looking for an
// ini to explain.
void arm_none() { arm_all(false); }

bool armed(int row) {
#ifdef MH_LIBMH_BUILD
    (void)row;
    return true; // standalone binds unconditionally; this path exists only for completeness
#else
    if (!g_loaded) {
        // Once, loudly. The alternative -- returning false quietly -- is the failure this whole
        // named-init-point design exists to prevent, and it is invisible in a green run.
        if (!g_complained) {
            g_complained = true;
            say("; [rebind] BUG: armed() consulted BEFORE arm_from_config()/arm_none() -- every row "
                "reads UNARMED, so a run that believes it armed the rebind did not. Arm earlier "
                "(the libmh rebind notes R11).\n");
        }
        return false;
    }
    return row >= 0 && row < ROW_COUNT && g_armed[row];
#endif
}

bool set_armed(const char *name, bool on) {
    const int row = row_of(name);
    if (row < 0) return false;
    if (g_armed[row] != on) {
        g_armed[row] = on;
        g_armed_n += on ? 1 : -1;
    }
    return true;
}

int armed_count() { return g_armed_n; }

const char *row_name(int row) {
    return (row >= 0 && row < ROW_COUNT) ? row_names[row] : "?";
}

int trap_count() { return kTrapCount; }

const char *trap_name(int i) { return (i >= 0 && i < kTrapCount) ? trap_names[i] : "?"; }

void report_arming(void (*sink)(const char *)) {
    g_sink = sink;
    char line[256];
    // AFFIRMATIVE AT ZERO. An unreported arm set is how a 0.8% live set passed for a promoted
    // closure, so the zero case says so in words rather than printing
    // nothing and letting a reader assume the line was simply not reached.
    if (!g_loaded) {
        say("; [rebind] NOT ARMED -- neither arm_from_config() nor arm_none() ran; no row is armed\n");
        return;
    }
    // THE MODE RIDES ON BOTH LINES (fork F2E). The count used to be readable against a ship default
    // and a section of overrides; with the section gone the count IS the configuration, so a reader
    // who sees "armed 0" must be able to tell "this run selected the original engine" from "the
    // arming is broken" without a second line to correlate. Same information, one fewer inference.
    if (g_armed_n == 0) {
        std::snprintf(line, sizeof(line),
                      "; [rebind] armed 0 of %d row(s) [config mode=%s] -- the hosted binder is "
                      "entirely ORIGINAL\n",
                      ROW_COUNT, mh::config::mode_name());
        say(line);
    } else {
        std::snprintf(line, sizeof(line), "; [rebind] armed %d of %d row(s) [config mode=%s]:\n",
                      g_armed_n, ROW_COUNT, mh::config::mode_name());
        say(line);
        for (int i = 0; i < ROW_COUNT; ++i) {
            if (!g_armed[i]) continue;
            std::snprintf(line, sizeof(line), ";   [rebind] %s -> OURS\n", row_names[i]);
            say(line);
        }
    }
    std::snprintf(line, sizeof(line),
                  "; [rebind] %d trap(s) in the standalone arm (%d deferred, %d unbindable); "
                  "hosted binds the original for all of them\n",
                  kTrapCount, TRAP_DEFERRED_COUNT, TRAP_UNBINDABLE_COUNT);
    say(line);
}

void trap_hit(const char *callee) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "; [rebind] TRAP: %s has no bound body in this build -- it is deferred "
                  "(LIB-REBIND-UI) or unbindable. Failing fast.\n",
                  callee);
    say(line);
    std::fflush(nullptr);
    std::abort();
}

} // namespace mh::rebind
