//
// lockstep/lockstep_state.h -- the closure's PRODUCTION-ARM binds (RI-STATE / SB-BIND T4).
//
// The twelve translated-logic state structs are declared beside the logic they describe
// (turn_engine.h, tx_emit.h, resync.h, ...) and BOUND in lockstep_state.cpp. This header adds the
// one thing those twelve have no place for: the handful of addresses the module's PRODUCTION arms
// need -- the live_* helpers that format a floating message, or the desync watcher's on-screen
// notice. They are not state a translated function reasons about; they are where the game keeps its
// text scratch buffer, its localized string table, and two things a notice happens to need.
//
// WHY IT IS A BOUND STRUCT RATHER THAN FOUR ptr<>() CALLS AT THE USE SITES. Same reason as every
// other module: tools/check_sim_addresses.py lets exactly one TU per module name an address, so
// that "where does this module's state live" has a single answer a reader can hold. The accessor
// returns BY VALUE and re-resolves through the region registry on every call -- it caches nothing,
// so a host that binds a relocated region is followed here like everywhere else.
//
#pragma once
#include <cstdint>

namespace mh::lockstep {

// The addresses the production arms reach for. Every member is a live pointer resolved from the
// region registry at the moment host_binds() is called; nothing here is stored across a call.
struct host_bind_state {
    // G_TEXT_TMP -- the game's shared wide text scratch. Every floating-message path in this module
    // formats into it and then hands the SAME buffer to the print call, which is what the original
    // does with a folded constant operand.
    void *text_scratch;
    // cfg::G_TEXT_PTRS -- the runtime-loaded localized string pool, indexed by text id.
    void *const *text_ptrs;
    // Planets -- read by the invasion-alert line for a planet's name index. A raw byte pointer plus
    // an explicit stride, exactly as turn_engine.cpp's helper already indexed it.
    const uint8_t *planets;
    // _G_LLM_GAME_SESSION_MODE -- desync_watch gates its whole sampler on this being MP lockstep.
    const uint8_t *session_mode;

    // _G_LLM_STRAT_PLAYERS as RAW BYTES, for the one arm that wants the ANSI `.name` field at a
    // hand-derived offset (`IMUL EDX,pidx,0x740` + `ADD EAX,0x714`) rather than the typed record.
    // The typed binding is engine_state::players / dispatch_state::players_w; this is deliberately
    // not a second one of those -- it is the same region reached the way that helper reaches it.
    char *players_bytes;
};

// Bound in lockstep_state.cpp. By value, re-resolved per call -- see the header comment.
host_bind_state host_binds();

} // namespace mh::lockstep
