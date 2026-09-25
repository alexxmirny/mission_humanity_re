//
// mh_harness/config1.h -- the harness in CONFIGURATION (1), i.e. with no libmh.dll (mp:D29).
//
// Ruling Q4 used to refuse the whole instrument without libmh's spine, so the build players run had
// no determinism oracle at all. D29 measured that the per-step region hash reaches exactly three
// spine symbols -- mh::state::live(), owner_table(), owner_count() -- and that mh.dll already
// defines all three with the configuration (1) answer (seams/libmh_bind.cpp: its own table seeded
// from REGIONS[], an empty owner table). So in configuration (1) the harness binds those three OUT
// OF mh.dll into their spine slots (the generated mh_harness_bind_config1) and arms "spine-free".
// Every other spine slot stays null; the call sites that could reach one are guarded in harness.cpp
// or their `[harness]` key is refused BY NAME at arm (the table below).
//
// HEADER-ONLY AND FREE OF THE MODULE, deliberately: net_selftest.exe's bindtest drives these exact
// functions with planted inputs (a resolver that hands back a PRIVATE copy of the registry, a
// set_armed that counts), which is the only way to prove the configuration (1) path offline -- the
// rig cannot be asked to hold a mutant.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mh::harness_cfg1 {

// ---- the fallback bind ---------------------------------------------------------------------------
//
// ALL OR NOTHING, the same rule both sibling binds follow: resolve every row into a local table
// first and adopt it only when it is whole. A partial adoption would arm an instrument that reads the
// registry through one image and the owner table through another -- which is exactly the two-table
// failure G179 records, wearing a harness. Returns the number of rows resolved (== n on success,
// nothing written otherwise), or -1 for a malformed request.
template <class Resolve>
inline int bind_fallback(void **spine_slots, int spine_count, const int *slot,
                         const char *const *name, int n, Resolve resolve) {
    constexpr int CAP = 8;
    if (spine_slots == nullptr || n <= 0 || n > CAP) return -1;
    void *t[CAP] = {};
    int   got    = 0;
    for (int k = 0; k < n; ++k) {
        if (slot[k] < 0 || slot[k] >= spine_count) return -1;
        t[k] = resolve(name[k]);
        if (t[k] != nullptr) ++got;
    }
    if (got != n) return got; // adopt the table only when it is WHOLE
    for (int k = 0; k < n; ++k) spine_slots[slot[k]] = t[k];
    return got;
}

// ---- the spine-only keys (ruling Q4 as amended by D29) --------------------------------------------
//
// A `[harness]` key whose FEATURE needs a spine row the fallback does not bind. In configuration (1)
// each one that is set is REFUSED BY NAME (a line in mh_harness.log, OutputDebugString, stderr) and
// switched off, and the instrument keeps hashing -- a refusal of the key, not of the harness. The
// `needs` column is the spine row, so the refusal line says WHY rather than only WHAT.
//
// What is NOT here, and why: the call sites that have an honest spine-free answer are GUARDED instead
// of refused, so their keys keep working -- rng_trace_set_step (no trace to step), the order-count
// ledger note (no ledger), the step-1 inbound census (no inbound surface -> "n/a"), the clause-6
// rebind yield (no rebind rows exist, and set_armed would trap), pin_strat_seed's libmh session-seed
// push (no promoted session_begin to reach), replay_suppress_enqueue's libmh sink gate (the byte
// neuter still goes in) and rdump's RNGD/NOTE flush (the region bytes still dump).
struct spine_key {
    const char *key;
    const char *needs;
};

inline constexpr spine_key SPINE_ONLY_KEYS[] = {
    {"boot_snapshot", "mh::state::boot::blob_size/capture"},
    {"world_capture", "mh::state::world::capture/capture_capacity"},
    {"snap_bench", "mh::state::world::capture/capture_capacity"},
    {"snapshot_at", "mh::state::world::capture/capture_capacity"},
    {"snapshot_import", "libmh_import_world + world::lockstep_hash"},
    {"world_import_at", "libmh_import_world + boot::reset_session_latch_for_test"},
    {"save_at", "mh::save::promotion_active/verify_active/last_save_path"},
    {"savegame_at", "mh::save::container_promotion_active"},
    {"loadgame_at", "mh::save::container_load_promotion_active"},
    {"load_at", "mh::save::promotion_active"},
    {"tact_synth", "mh::tact::unit_enqueue_command/group_issue_order"},
    {"tact_journal", "mh::tact::unit_enqueue_command/group_issue_order (the replay injector)"},
    {"skip_pace_hook", "mh::sim::set_time_resync_pace_disabled"},
    {"rng_trace", "mh::sim::rng_trace_window"},
    {"skip_input_update", "mh::sim::set_lt_frame_input_override"},
    {"pin_menu_clock", "mh::hosthook::install_export_ok (the MH_EXPORT_REPLACE install)"},
};
inline constexpr int SPINE_ONLY_KEY_COUNT = (int)(sizeof(SPINE_ONLY_KEYS) / sizeof(SPINE_ONLY_KEYS[0]));

inline const spine_key *find_spine_key(const char *key) {
    for (const spine_key &k : SPINE_ONLY_KEYS) {
        const char *a = k.key, *b = key;
        while (*a && *a == *b) ++a, ++b;
        if (*a == '\0' && *b == '\0') return &k;
    }
    return nullptr;
}

inline int format_key_refusal(char *out, size_t cap, const spine_key &k) {
    return std::snprintf(out, cap,
                         "; [harness] configuration (1): key `%s` REFUSED -- it needs %s, a libmh.dll "
                         "spine row with no spine-free answer. The key is OFF for this run; the "
                         "per-step region hash is unaffected and keeps running.\n",
                         k.key, k.needs);
}

// ---- the clause-6 rebind yield, gated on the spine ------------------------------------------------
//
// mh::rebind::set_armed is a SPINE row and the rows it flips are libmh's: with no spine there is no
// rebind table to yield from, and the call would reach a null slot and TRAP (kill the process). So
// with spine == false this returns -1 WITHOUT CALLING ANYTHING. Otherwise it is the derivation
// harness.cpp always ran: every rebindable row whose original entry an instrument has claimed is
// disarmed, and on_yield(row, claimant) reports each one.
template <class Claimant, class SetArmed, class OnYield>
inline int yield_claimed_rows(bool spine, int rows, const uintptr_t *row_addr,
                              const char *const *row_names, Claimant who, SetArmed set_armed,
                              OnYield on_yield, int *matched_out) {
    if (matched_out) *matched_out = 0;
    if (!spine) return -1;
    int yielded = 0, matched = 0;
    for (int i = 0; i < rows; ++i) {
        const uintptr_t a = row_addr[i];
        if (!a) continue; // no exported address -> matches nothing; never yielded on a guess
        const char *w = who(a);
        if (!w) continue;
        ++matched;
        if (set_armed(row_names[i], false)) {
            ++yielded;
            on_yield(row_names[i], w);
        }
    }
    if (matched_out) *matched_out = matched;
    return yielded;
}

// ---- the two report lines --------------------------------------------------------------------------
//
// THE STEP-1 INBOUND CENSUS keeps its prefix in both configurations: it is check_arm_order's end
// marker for the harness channel ("; [libmh_in] inbound refusals since open:"), so a configuration
// (1) run must still END its arm window on it. Only the body differs -- there is no inbound surface to
// count without libmh.dll, and a 0 there would read as "counted, none", which is not what happened.
inline int format_census(char *out, size_t cap, bool spine, int refusals, const char *first) {
    if (!spine)
        return std::snprintf(out, cap,
                             "; [libmh_in] inbound refusals since open: n/a, no libmh "
                             "(configuration (1) -- there is no inbound surface to count)\n");
    return std::snprintf(out, cap, "; [libmh_in] inbound refusals since open: %d (first: %s)\n",
                         refusals, first ? first : "");
}

// THE CONFIGURATION LINE: which registry the hash reads, what moved, and the manifest fingerprint.
// Written in BOTH configurations so a MIXED pair (configuration (1) vs mode=original) carries the
// same statement on both sides, and tools/mp_analyze.py refuses a pair whose `manifest fp=` differ.
// It is written AFTER the step-1 census, i.e. outside check_arm_order's arm window, so no recorded
// arm-order baseline moves because of it.
inline int format_config_line(char *out, size_t cap, bool spine, int rebased, int owned,
                              int uncovered, uint32_t manifest_fp) {
    char unc[24];
    if (uncovered == 0)
        std::snprintf(unc, sizeof(unc), "none");
    else
        std::snprintf(unc, sizeof(unc), "%d", uncovered);
    return std::snprintf(out, cap,
                         "; [harness] configuration %s: spine %s, registry=%s, rebased=%d owned=%d, "
                         "uncovered=%s, manifest fp=%08X\n",
                         spine ? "(2)" : "(1)", spine ? "PRESENT" : "ABSENT",
                         spine ? "libmh.dll" : "mh.dll", rebased, owned, unc,
                         (unsigned)manifest_fp);
}

} // namespace mh::harness_cfg1
