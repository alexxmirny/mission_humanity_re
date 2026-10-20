//
// config/gameplay.h -- PT-SET1: apply the host-synced gameplay settings (ini_keys.def F_HOSTSYNC rows).
//
// One entry point, apply(set, source): decode the set against the registry (missing key = default,
// unknown key skipped, both logged), run every row's apply hook (mh/seams/gameplay_hooks.cpp) over a copy
// of the lockstep `reimpl_fixes`, publish that copy with set_fixes() when the run is ours, and write ONE
// log line "; gameplay: <source> <summary>". The data machinery is mh_common/include/mh_gameplay.h.
//
// WHEN (PT-SET1 section 3): boot after lockstep_install_core (LOCAL); host before FLAG_START goes out
// (PUBLISHED); client at the U28 adopt point (ADOPTED); lobby close (LOCAL again).
//
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mh_gameplay.h"
#include "mh_net_proto/gameplay_set.h"

namespace mh::lockstep {
struct reimpl_fixes;
}

namespace mh::gameplay {

enum class Source : uint8_t { LOCAL,
                              PUBLISHED,
                              ADOPTED };

using hook_row = mh_gameplay::hook_row<mh::lockstep::reimpl_fixes>;

// The hook table (mh/seams/gameplay_hooks.cpp). Empty until a row is flagged F_HOSTSYNC.
const hook_row *hooks(int *count);

// This process's own values, read through the ini gate and clamped, as a wire set.
mh_net_proto::GameplaySet local_set();

// Apply `set` (see the file comment). `why` (may be null) is appended to the log line.
void apply(const mh_net_proto::GameplaySet &set, Source src, const char *why = nullptr);

// apply(local_set(), src).
void apply_local(Source src, const char *why = nullptr);

// Digest of the key set this build syncs (JOIN v7); never 0.
uint32_t local_digest();

// "key=value;..." of a set against the registry (for the session record). Empty with no synced rows.
void summary_of(const mh_net_proto::GameplaySet &set, char *out, int cap);

// Re-apply this process's local values WITHOUT touching the session record (the lobby close: the
// finished match's record keeps what actually ran). Logged as "; gameplay: LOCAL ... (<why>)".
void restore_local(const char *why);

// What the session record carries: the summary and source ("local" | "published" | "adopted") of the
// last apply(), excluding restore_local().
void current(char *summary, int summary_cap, char *src, int src_cap);

// GH1: FNV-1a of the last apply()'s summary text -- one short number per peer to compare in the logs.
uint32_t current_hash();

// GH1: one line in mh_net.log (the file the rig checkers read): `; gameplay: <text>`.
void note(const char *text);
// GH1: `; gameplay: applied hash=<current_hash()>` -- written by BOTH peers right after their apply.
void log_applied_hash();

const char *source_name(Source s);

} // namespace mh::gameplay
