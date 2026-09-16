//
// patch/inmem_install.cpp -- the F1E arm point: read the opt-in, apply the compiled-in manifest to
// the loaded image, optionally dump what landed for the parity checker.
//
// SILENT WHEN DISARMED, deliberately. Every other install in MH_Seam_Init reports itself, and that
// is right for a shipped mechanism; this is a spike that defaults OFF, and a line in every run's
// arm log is a change to the arm log that the parity gates read.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

#include "hook/promoted.h"
#include "include/mh_inmem_patch_export.h"
#include "patch/inmem_patch.h"
#include "patch/manifests.gen.h"

// net_seams.cpp; declared rather than pulling in net_internal.h for one log sink.
void seam_log(const char *s);

namespace {
const char *g_refusal;

// WHICH compiled-in manifest this run applies (fork F4C-GATE). The compile list derives from the
// manifests' own `class` field and holds three alternatives, so there is no "the" manifest any
// more and the selection has to be stated. Rules, in the order they are applied:
//
//   * `[patch] manifest=<name>` names one. An unknown name REFUSES and lists what is compiled in --
//     a typo must not silently arm a different patch, and it must not silently arm none either.
//   * no key, one manifest compiled in: that one. (The F1E shape, kept working.)
//   * no key, several compiled in: REFUSE and list them. Picking a default here would mean the ini
//     that says `inmem=1` and nothing else quietly gets whichever manifest sorts first -- which for
//     this corpus is the 7,770-site grand limit raise, the single largest change in the tree.
const mh::patch::inmem_manifest *select(const char *ini_path) {
    char want[128] = {};
    GetPrivateProfileStringA("patch", "manifest", "", want, sizeof(want), ini_path);

    // BOUNDED, because the compile list is derived and therefore not a number this file controls:
    // a corpus reclassification could put twenty names here, and a refusal that overran its own
    // buffer would be a crash in the code path whose whole job is to fail clearly. Truncation is
    // stated rather than silent -- the count in the message is the real one either way.
    char   names[512] = {};
    size_t used       = 0;
    for (int i = 0; i < mh::patch::COMPILED_COUNT; ++i) {
        const char  *sep  = used ? ", " : "";
        const size_t need = strlen(sep) + strlen(mh::patch::COMPILED[i].name);
        if (used + need + 5 >= sizeof(names)) {
            memcpy(names + used, ", ...", 6);
            break;
        }
        memcpy(names + used, sep, strlen(sep));
        used += strlen(sep);
        memcpy(names + used, mh::patch::COMPILED[i].name, strlen(mh::patch::COMPILED[i].name));
        used += strlen(mh::patch::COMPILED[i].name);
        names[used] = '\0';
    }

    if (!want[0]) {
        if (mh::patch::COMPILED_COUNT == 1) return mh::patch::COMPILED[0].m;
        char b[768];
        wsprintfA(b,
                  "; [inmem] NOT applied: %d manifests are compiled in and `[patch] manifest=` names "
                  "none of them. They are ALTERNATIVES (each bakes a cave at the image's next-free "
                  "VA), so there is no default. Compiled in: %s\n",
                  mh::patch::COMPILED_COUNT, names);
        seam_log(b);
        g_refusal = "no [patch] manifest= selected";
        return nullptr;
    }
    for (int i = 0; i < mh::patch::COMPILED_COUNT; ++i)
        if (strcmp(want, mh::patch::COMPILED[i].name) == 0) return mh::patch::COMPILED[i].m;

    char b[1024];
    wsprintfA(b, "; [inmem] NOT applied: `[patch] manifest=%s` is not compiled into this build. Compiled in: %s\n",
              want, names);
    seam_log(b);
    g_refusal = "[patch] manifest= names a manifest this build does not carry";
    return nullptr;
}

// dump_exit is honoured on EVERY path, including the ones that apply nothing. The parity runner
// launches the lane and WAITS for a dump; a refusal that left the process booting would spend the
// runner's whole timeout to report a fact the log already had.
void maybe_exit(const char *ini_path) {
    if (!GetPrivateProfileIntA("patch", "dump_exit", 0, ini_path)) return;
    // The headless parity run: the only thing this process was started for is the state of its
    // own image one instruction after the apply. Leaving it to boot would mean a window, a rig
    // lease and a kill, for evidence that is already on disk.
    seam_log("; [inmem] [patch] dump_exit=1 -- terminating after the dump\n");
    TerminateProcess(GetCurrentProcess(), 0);
}
} // namespace

extern "C" const char *MH_InMemPatch_Refusal(void) { return g_refusal; }

extern "C" int MH_InMemPatch_Install(const char *ini_path) {
    if (!ini_path || !GetPrivateProfileIntA("patch", "inmem", 0, ini_path)) return 0;

    mh::patch::set_logger(&seam_log);
    const mh::patch::inmem_manifest *sel = select(ini_path);
    if (!sel) {
        mh::hook::report_patched_bodies();
        maybe_exit(ini_path);
        return MH_INMEM_ARMED;
    }
    const mh::patch::inmem_manifest &m = *sel;

    int                           armed = MH_INMEM_ARMED;
    const mh::patch::apply_result r     = mh::patch::apply(m);
    g_refusal                           = r.refusal;
    if (r.applied) armed |= MH_INMEM_APPLIED;

    // F4C-COMP. Stated HERE and not from the arm sequence, so the line exists only in a run that
    // actually asked for the patcher: `[patch] inmem` is default OFF, and an unconditional line
    // would be a change to every arm-order baseline in the tree for a mechanism that did nothing.
    // Inside an armed run it is not optional -- a refused manifest prints "0 body(ies) registered",
    // which is the difference between "nothing to register" and "registration is broken".
    mh::hook::report_patched_bodies();

    char dump[MAX_PATH] = {};
    GetPrivateProfileStringA("patch", "dump", "", dump, sizeof(dump), ini_path);
    if (dump[0]) {
        // The dump is written whether or not the apply succeeded: a refusal is a result the parity
        // run has to be able to read, and an absent file reads as "the DLL never ran", which is a
        // different fact.
        if (mh::patch::write_parity_dump(m, r, dump)) armed |= MH_INMEM_DUMPED;
        char b[MAX_PATH + 64];
        wsprintfA(b, "; [inmem] parity dump %s -> %s\n", (armed & MH_INMEM_DUMPED) ? "written" : "FAILED", dump);
        seam_log(b);
    }

    maybe_exit(ini_path);
    return armed;
}
