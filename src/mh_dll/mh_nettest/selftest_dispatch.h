//
// selftest_dispatch.h -- the suite-table MECHANISM, shared by both selftest executables.
//
// WHY A SHARED HEADER (fork F5I). F5I S0 replaced net_selftest.cpp's 43-arm `strcmp` chain with one
// table, and S2 gave the spine its own exe (`libmh_selftest.exe`, src/mh_dll/libmh_test/). Two mains
// then need the same five things: the argv bundle a suite receives, the adapters that let a table
// row name a plain `int f()` without a hand-written forwarder, the row type, `--list-suites`, and
// the unknown-mode refusal. Copying them would put the thing S0 exists to prevent -- a second
// uncompared copy of a list mechanism -- straight back into the tree, one level down.
//
// WHAT IS **NOT** HERE, ON PURPOSE. The TABLES themselves, and the reasoning attached to each row.
// net_selftest.cpp's table carries the lessons that produced it (why `selftest` must be a named row,
// why an unrecognised mode is an error rather than a default, why `seamtest` is not gate-flagged);
// libmh_test/libmh_selftest.cpp's carries its own. Those are statements about which suites exist and
// why, which is each exe's own business; this file is the machinery underneath them and says nothing
// about any particular suite.
//
// IT LIVES IN mh_nettest/ RATHER THAN IN A NEUTRAL DIRECTORY for the reason hostapi_selftest_support.h
// and mh/addr/mh_calls.gen.cpp already do: a file shared between build targets stays in the directory
// of the target that owns it and is reached from the other by an include dir, so there is one copy and
// the ownership is legible. libmh_test.vcxproj carries `..\mh_nettest` on its include path.
//
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Everything a suite may be handed. Parsed ONCE in main() so a row cannot disagree with its
// neighbours about which argv slot means what.
struct suite_args {
    int    argc;
    char **argv;
    int    port;  // argv[2] as an int, 39500 when absent or unparseable
    int    extra; // argv[3] as an int, 0 when absent (recv/bcast player id)
};

typedef int (*suite_fn)(const suite_args &);

// The three shapes that cover almost every row. Templates rather than one hand-written forwarder per
// suite, so that adding a suite stays a one-line table edit -- the property the table exists for. A
// suite whose signature fits none of them writes its own adapter next to its own table.
template <int (*F)()>
inline int adapt_void(const suite_args &) {
    return F();
}
template <int (*F)(int)>
inline int adapt_port(const suite_args &a) {
    return F(a.port);
}
template <int (*F)(int, char **)>
inline int adapt_argv(const suite_args &a) {
    return F(a.argc, a.argv);
}

// `gate` marks the suites the offline gate runs (tools/run_selftests.py, via
// tools/data/selftest_roster.json). The rest are modes a driver cannot run unattended -- they take a
// port, expect a peer, spawn or are spawned, and several never return on their own -- which is why
// the flag exists rather than the roster being "every row".
struct suite_row {
    const char *name;
    bool        gate;
    suite_fn    run;
};

// Render the roster for the unknown-mode error. GENERATED FROM THE TABLE, never hand-written: before
// F5I the message was a hand-maintained third copy of the list and had already gone stale.
inline void selftest_print_modes(const suite_row *table, size_t count) {
    printf("modes:");
    int col = 6;
    for (size_t i = 0; i < count; ++i) {
        const int n = (int)strlen(table[i].name) + 1;
        if (col + n > 92) {
            printf("\n     ");
            col = 5;
        }
        printf(" %s", table[i].name);
        col += n;
    }
    printf("\n");
}

// The gate-flagged names, one per line. This is what tools/run_selftests.py reads to assert the
// exe's own roster equals the committed one, so an exe must answer it BEFORE binding anything: a
// failure for an unrelated reason (a host-table version skew) would read as "the exe lists nothing".
inline int selftest_list_suites(const suite_row *table, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (table[i].gate) printf("%s\n", table[i].name);
    return 0;
}

inline suite_args selftest_args(int argc, char **argv) {
    suite_args a;
    a.argc = argc;
    a.argv = argv;
    a.port = (argc > 2) ? atoi(argv[2]) : 39500;
    if (a.port <= 0) a.port = 39500;
    a.extra = (argc > 3) ? atoi(argv[3]) : 0; // per-mode: player id for recv/bcast
    return a;
}

inline const suite_row *selftest_find(const suite_row *table, size_t count, const char *mode) {
    for (size_t i = 0; i < count; ++i)
        if (strcmp(mode, table[i].name) == 0) return &table[i];
    return nullptr;
}

// AN UNRECOGNISED MODE IS AN ERROR, NOT A DEFAULT -- exit 2 with the list. Measured 2026-08-01:
// while the dispatcher still fell through to `run_selftest`, `lockstepstest` (a typo of `lockstest`)
// printed "=== PASS ===" and exited 0, i.e. a green result for a test that does not exist.
inline int selftest_unknown_mode(const char *mode, const suite_row *table, size_t count) {
    printf("unknown mode '%s'\n", mode);
    selftest_print_modes(table, count);
    return 2;
}
