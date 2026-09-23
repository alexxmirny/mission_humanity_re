//
// config/config.h -- THE CONFIG SELECTOR (fork plan D11, tracker fork F2A).
//
// ONE question, asked from every layer: which implementation does this process run?
//
//   original    the game's own bodies. Nothing of ours is promoted, no rebind row is armed;
//               mh.dll is a shim + patch host + the net restoration, and nothing else.
//   brokered    the reimplemented spine serves the domains it owns. Today's ship configuration.
//   standalone  no game binary at all -- libmh with a host. Everything is ours by definition.
//
// WHY IT IS ONE SELECTOR AND NOT A POLICY EACH CALLER OWNS. Before F2A the answer was spread over
// 23 SHIP_PROMOTE_* constants, SHIP_REBIND_DEFAULT, 47 `[promote]` keys and 719 `[rebind]` rows, and
// "run the original engine" was a 766-line generated ini fragment that had to be re-DERIVED from the
// sources every time it was written (tools/test_ui.py all_original_knobs) precisely because a
// hand-listed one goes stale silently -- and a rollback arm that misses a knob is not a rollback,
// it is a quieter version of the configuration it was meant to disprove. D11 makes the
// configuration a value instead of a list.
//
// F2E CLOSED THE SPLIT. Until F2E this header set the DEFAULT that a per-key `[promote]`/`[rebind]`
// entry could still override, because the oracles' control arms were built out of that vocabulary
// and had to keep working while the selector was proven. F2C retargeted them and F2E deleted the
// vocabulary: the constants, the 57 ini reads, the section headers, the 719-row gate file. There is
// now exactly ONE input that decides which bodies run, and the two functions below are the only way
// to ask it.
//
// STANDALONE AGREES BY CONSTRUCTION, NOT BY CONVENTION. MH_LIBMH_BUILD already selects the
// standalone arm of MH_CRT / MH_PROMOTED / MH_LIBMH_BIND / MH_EXPORT_REPLACE, so
// the selector is COMPILED to standalone in that build rather than reading a file that says so. A
// standalone host has no mh_net.ini and must not need one; this header does not even declare a
// Windows ini call there.
//
// THE READ POINT IS "WHOEVER ASKS FIRST", AND THAT IS WHAT MAKES IT G104-SAFE. The ini path is
// composed HERE, from the module file name, rather than taken from net_internal.h's `g_ini` --
// because `g_ini` is filled in MH_Seam_Init, and two of this selector's consumers run BEFORE it
// (harness.cpp's rebind-gate load and its save-walker relocation gate, both of which already compose
// their own path for exactly this reason; see harness.cpp's note at armed_save_walkers). A selector
// that depended on another module's init would answer "brokered" in every early caller and the
// disagreement would be invisible in a green run -- the G104 shape. Depending on nothing but the
// process's own image path means every call point is correct, so there is no ordering to get right.
// The answer is cached on first use: the file is read once per process.
//
// ONE FILE (F2G, fork plan D12). `mh_net.ini` is now the WHOLE configuration surface: the harness's
// own `mh_harness.ini` merged into it as the `[harness]` section, and the harness arms off an
// explicit `[harness] enable=1` instead of that file's mere existence. Presence-of-a-file and
// presence-of-a-section are the same fragility -- a config whose meaning is "whether this path
// resolves" cannot be read out of the file it is written in, and a stale copy on a test machine
// silently decided the real stop_step of an 800-step gate once. The old file
// is therefore not merely ignored: see refuse_stray_harness_ini below.
//
// A CONFIGURATION THIS BUILD CANNOT PERFORM IS REFUSED, NOT APPROXIMATED (F2E). Two cases, one
// mechanism (`refuse` below): an unrecognised `[config] mode`, and a surviving `[promote]` /
// `[promote_skip]` / `[state_handler_skip]` / `[rebind]` section. Both used to be survivable --
// the mode announced itself and continued, the sections were the live override surface -- and both
// stop being survivable at the same moment and for the same reason: with the per-key surface gone
// the selector is the ONLY control, so `mode=brokerd` silently running brokered is a run whose
// author believed something false about what would execute, and a stale `[promote] lockstep=0`
// fragment is a rollback arm that rolls nothing back. F2A chose announce-and-continue precisely
// because a typo could not brick a lane while the keys still worked; that reason expired with them.
//
// THE REFUSAL HAS NO LOGGER AND CANNOT WAIT FOR ONE. It fires at whoever asks first, which is before
// MH_Seam_Init -- so it states itself through three channels that need no init (a file beside the
// exe, OutputDebugString, stderr) and then kills the process. Terminating is the point: every
// weaker answer leaves a process running a configuration nobody asked for, which is the class of
// failure this whole item exists to remove.
//
#pragma once

#ifndef MH_LIBMH_BUILD
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdio> // the refusal's console channel -- it fires before any logger exists
#include <windows.h>
#include "config/ini_read.h" // TL-HARN4: read_ini_string -- strips a trailing `;comment` off `[config] mode`
#endif

namespace mh::config {

enum class mode_t {
    original,   // the game's own bodies; mh.dll is shim + patch host + the net restoration
    brokered,   // the reimplemented spine serves the domains it owns -- today's ship configuration
    standalone, // no game binary at all: libmh + a host. Compile-time (MH_LIBMH_BUILD), never an ini
};

// THE SAVE CLOSURE IS NOT OURS YET, and that fact is a constant rather than a knob (fork ruling Q1).
// Every other domain's brokered answer is "ours"; the four save-block walkers -- SavePlanetToDisk,
// LoadPlanetFromDisk, game_SaveGame, llm_game_load -- are the one domain where a translated body
// exists and is deliberately NOT installed, because the in-call A/B has not produced byte-identity
// evidence over a real container. Before F2E that was four `SHIP_PROMOTE_*` constants reading 0;
// the constants are gone and this is what is left of them.
//
// IT IS DECLARED HERE, BESIDE THE SELECTOR, because it is the second half of one derivation:
// `brokered AND save-closure-owned => ours`. Two consumers read it -- the install decision in
// reimpl_probe and the D6 relocation mask in harness -- and they must never disagree about whether
// a save walker is ours, since the mask decides whether 29 regions may move out from under it.
// Flipping this one `false` to `true` is the whole of arming the save closure.
inline constexpr bool kSaveClosureOwned = false;

#ifdef MH_LIBMH_BUILD

constexpr mode_t      mode() { return mode_t::standalone; }
constexpr const char *mode_name() { return "standalone"; }

#else

namespace detail {

// The three channels a refusal has before any logger exists. A file beside the exe is what an
// automated lane can read back after the process is gone; OutputDebugString is what a debugger or
// DebugView sees; stderr is what a console host (net_selftest) shows. All three, because the
// refusal's whole job is to be found -- a process that died silently is indistinguishable from one
// that crashed, and the operator would go looking in the wrong place.
inline void refuse(const char *dir, const char *what, const char *why) {
    // 1 KB, because `what` carries a full path (MAX_PATH) plus its sentence and the buffer has to
    // hold that alongside the fixed frame below. wsprintfA does not bound-check.
    char line[1024];
    wsprintfA(line,
              "mh.dll REFUSED THIS RUN: %s\r\n%s\r\n"
              "Nothing was installed and the process is being terminated, because every weaker "
              "answer leaves a game running a configuration nobody asked for.\r\n",
              what, why);
    std::fputs(line, stderr);
    OutputDebugStringA(line);
    char path[MAX_PATH];
    wsprintfA(path, "%smh_config_refused.log", dir);
    const HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
        CloseHandle(h);
    }
    // TerminateProcess, not ExitProcess: this runs from DllMain's call tree, where an orderly exit
    // would run DLL_PROCESS_DETACH under the loader lock we are already holding.
    TerminateProcess(GetCurrentProcess(), 3);
}

// The retired configuration vocabulary (F2E). A section here is not one stale line -- it is a
// config file that describes a control surface this build no longer has, so every statement it
// makes about what will execute is false. Refused BY NAME so the operator knows which fragment to
// delete rather than being told only that something is wrong.
//
// F2G ADDED THREE MORE, and they are retired for the same reason one level down -- the section no
// longer exists, so every key under it is a statement about a control surface this build does not
// have. `[pacing]` folded into `[video]` (fps_cap), `[test]` into `[uitest]` (lane), and `[probe]`
// went away with the LT1C cell-grid diagnostic it gated. A surviving `[test] lane=7` is the worst of
// the three: the lane it names is the single-instance mutex rename that keeps parallel lanes apart,
// so a silently-ignored one puts two game instances on one mutex and the failure surfaces as a
// launch that never happens.
// EACH ROW CARRIES ITS OWN SUCCESSOR, because "this is retired" is the useless half of the message.
// The operator is holding a fragment that says something; what they need is where that statement
// went, and the two generations retire for different reasons -- F2E's four described a per-key
// control surface that no longer exists, F2G's three simply MOVED.
struct retired_section {
    const char *name;
    const char *why;
};
inline void refuse_retired_sections(const char *dir, const char *ini) {
    static const char *const kF2E =
        "Which bodies run is now decided by `[config] mode` alone (original | brokered). A "
        "surviving per-key section is a rollback arm that rolls nothing back: delete the "
        "fragment, or express what it meant as `[config] mode=original`.";
    static const retired_section kRetired[] = {
        {"promote", kF2E},
        {"promote_skip", kF2E},
        {"state_handler_skip", kF2E},
        {"rebind", kF2E},
        {"pacing", "`fps_cap` moved into `[video]` at fork F2G, beside the `no_present` it already "
                   "requires. Rename the section header to `[video]` (merging it with any `[video]` "
                   "block the file already has -- a SECOND one would be unreachable)."},
        {"test", "`lane` moved into `[uitest]` at fork F2G. It is the single-instance mutex rename "
                 "that keeps parallel lanes apart, so an ignored one would put two game instances "
                 "on one mutex: move the key under `[uitest]`."},
        {"probe", "the LT1C cell-grid dead-store probe this section gated was deleted at fork F2G "
                  "(its negative claim is settled). There is no successor key -- "
                  "delete the section."},
    };
    // Two bytes is enough to distinguish present-and-non-empty from absent: GetPrivateProfileSection
    // returns 0 for both an absent section and an empty one, and an empty `[promote]` header states
    // nothing about what runs. It is the KEYS that lie.
    char buf[4];
    for (const retired_section &r : kRetired) {
        if (GetPrivateProfileSectionA(r.name, buf, sizeof(buf), ini) == 0) continue;
        char what[512];
        wsprintfA(what, "%s carries a `[%s]` section, and that section was retired by the fork.",
                  ini, r.name);
        refuse(dir, what, r.why);
    }
}

// THE OLD HARNESS FILE IS REFUSED, NOT IGNORED (F2G). Until the merge, `mh_harness.ini` beside the
// exe WAS the arming signal -- so a copy left behind on a rig machine used to arm a harness and
// decide a run's stop_step. After the merge the same file arms nothing and configures nothing, which
// is strictly worse: the operator who put it there believes the run is instrumented, the run is not,
// and nothing in the log says so. That is the quiet-wrong-answer shape this whole refusal mechanism
// exists to remove, and it is the one case where the mere EXISTENCE of a path is still the signal --
// deliberately, because that is exactly what the old file meant.
inline void refuse_stray_harness_ini(const char *dir) {
    char stray[MAX_PATH];
    wsprintfA(stray, "%smh_harness.ini", dir);
    if (GetFileAttributesA(stray) == INVALID_FILE_ATTRIBUTES) return;
    char what[512];
    wsprintfA(what, "%s still exists, and fork F2G merged that file into mh_net.ini.", stray);
    refuse(dir, what,
           "The harness now reads its `[harness]` section out of mh_net.ini and arms only on an "
           "explicit `[harness] enable=1` -- presence of a file is no longer a configuration. Move "
           "the block into mh_net.ini (adding `enable=1` if you wanted it armed) and delete this "
           "file. It is refused rather than ignored because a leftover that used to arm the harness "
           "and now does nothing is a run whose author believes it is instrumented.");
}

// The exe's directory, with a trailing separator. Composed from the process image path and nothing
// else -- that is the whole of the G104 argument in this header's banner.
inline void exe_dir(char *out) {
    GetModuleFileNameA(nullptr, out, MAX_PATH);
    char *slash = nullptr;
    for (char *p = out; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (slash != nullptr) slash[1] = '\0';
}

inline mode_t resolve() {
    char dir[MAX_PATH];
    char ini[MAX_PATH];
    exe_dir(dir);
    wsprintfA(ini, "%smh_net.ini", dir);

    refuse_stray_harness_ini(dir);
    refuse_retired_sections(dir, ini);

    char v[32] = {0};
    // BROKERED IS THE DEFAULT, so an install with no ini at all runs what ships today. Same
    // reasoning C8-f used for promotion and the 2026-09-04 call used for the rebind gate: a
    // configuration that ships off is a mechanism exercised in no configuration anyone plays.
    read_ini_string("config", "mode", "brokered", v, sizeof(v), ini); // TL-HARN4: strips a trailing `;comment`
    if (lstrcmpiA(v, "original") == 0) return mode_t::original;
    if (lstrcmpiA(v, "brokered") == 0) return mode_t::brokered;
    // `standalone` is not selectable from an ini: it is a property of the BUILD, and a hosted
    // process claiming it would be describing a binary layout it does not have -- so it lands here,
    // with every typo, rather than being silently honoured.
    char what[256];
    wsprintfA(what, "`[config] mode=%s` in %s is not a mode this build implements.", v, ini);
    refuse(dir, what,
           "The modes are `original` (the game's own bodies) and `brokered` (the reimplemented "
           "spine -- the default when the key is absent). `standalone` is a property of the BUILD, "
           "never of an ini. Until F2E a misspelling here fell back to brokered and announced "
           "itself; with the per-key override surface deleted that fallback would be the only "
           "control silently disagreeing with the run it names.");
    return mode_t::brokered; // unreachable; refuse() terminated
}

} // namespace detail

inline mode_t mode() {
    static const mode_t cached = detail::resolve();
    return cached;
}

inline const char *mode_name() { return mode() == mode_t::original ? "original" : "brokered"; }

#endif

// THE D11 DERIVATION, and the only one: every "does OUR body run here" question in the tree goes
// through this. Under `original` nothing of ours runs; under `brokered` and `standalone` ours does.
//
// It takes no argument since F2E. It used to take the mechanism's `SHIP_PROMOTE_*` constant, which
// was the migration era's way of saying "this domain is translated but not yet trusted" -- with
// those constants deleted the only surviving instance of that idea is the save closure above, and
// it has its own named question rather than being one value in a list of 23.
// (`inline`, not `constexpr`: in the hosted build the answer comes from a file, so there is no
// constant expression to be had. In the standalone build the compiler folds it anyway.)
inline bool ours_run() { return mode() != mode_t::original; }

// `brokered AND save-closure-owned => ours` -- the Q1 derivation, in one place so its two consumers
// cannot drift apart. False in every configuration today, and that is the behaviour-preserving
// answer the ruling requires, not an oversight: see kSaveClosureOwned.
inline bool save_walkers_ours() { return ours_run() && kSaveClosureOwned; }

} // namespace mh::config
