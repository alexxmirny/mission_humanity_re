//
// lockstep/lockstep_state.cpp -- WHERE mh::lockstep's STATE IS (RI-STATE / SB-BIND T4).
//
// Every address this closure reads or writes is named HERE and nowhere else. Twelve state structs,
// twelve binders, one translation unit -- the same shape sim/sim_state.cpp, ai/ai_state.cpp,
// tact/tact_state.cpp and orders/order_queue.cpp already have, and the reason
// tools/check_sim_addresses.py can name a single binder for libmh/lockstep/ and refuse an address in
// any other TU of the module.
//
// WHY THE BINDERS MOVED, AND WHY THAT ALONE WOULD HAVE BEEN WORTHLESS
// -------------------------------------------------------------------
// Before SB-BIND these twelve accessors lived beside the code that used them and each spelled its
// own addresses as `reinterpret_cast<T *>(mh::addr::NAME)` -- the STOCK .bss address, baked in. A
// host that binds a relocated copy of a region (SB-HOSTFREE's whole premise) would have moved the
// bytes and left every one of these pointing at the abandoned original, silently: the reads would
// succeed, return stale values, and agree with each other.
//
// So each is now `mh::state::ptr<T>(RID_X)`, which resolves through the live table the host binds.
// That is half the fix. The other half is that NONE of these is a `static` any more and every
// accessor returns BY VALUE.
//
// A `static const X st = {...}` runs its initializer exactly once, on the first call. Converting
// the addresses to ptr<>() while leaving the `static` in place would have resolved them once, at
// whatever the bind happened to be at first call, and cached that forever -- which is the same
// stale pointer with more ceremony, and harder to see. sim_state.h states the rule this follows:
// "NOTHING HERE CACHES AN ADDRESS. state() returns BY VALUE and re-resolves every pointer from the
// region registry on every call. A static bound once would go stale the moment a region is rebased
// under ST2, and it would go stale SILENTLY."
//
// lt_reload_snapshot_resync.cpp carried a comment naming this exact defect and deferring it to
// "SB-BIND T4's lockstep tranche"; this file is that tranche.
//
// The cost is a struct copy per call -- between 2 and 40 pointers. mh::sim::state() and
// mh::ai::state() have returned much larger views by value since SIM0/AI0 and are called from the
// per-step hot path, so the shape is measured, not hoped for.
//
// WHAT DID NOT MOVE. The `*_calls` / `*_ops` binders stay in their own TUs. They bind CODE --
// function pointers, MH_INTERNAL_CALL edges, seam thunks -- and code is not relocated by a state
// bind, so there is nothing for this file to guarantee about them and every reason to keep them
// next to the functions they wire.
//
#include "lockstep/lockstep_state.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_regions.gen.h" // the state region REGISTRY -- where this module's state lives
#include "lockstep/lt_chat_ally_mask.h"
#include "lockstep/lt_reload_snapshot_resync.h"
#include "lockstep/lt_time_query.h"
#include "lockstep/net_session.h"
#include "lockstep/resync.h"
#include "lockstep/turn_engine.h"
#include "lockstep/tx_emit.h"
#include "lockstep/tx_emit_chat.h"
#include "lockstep/tx_emit_order.h"
#include "orders/order_queue.h" // ST1: the order counts come from their OWNER, not a second binding

#include <cstdint>

// COLD BY CONSTRUCTION, so not inline candidates. Each of these builds an aggregate of up to 40
// pointers and is called once on entry to a translated body; `state()` alone has ~1900 call sites,
// and inlining that construction into every one of them is bloat rather than speed.
//
// This is NOT the fix for the C1001 that SB-BIND T4 hit in mh_nettest (see that project's LTCG
// comment). noinline was tried against it FIRST and the crash was unchanged -- recorded here because
// the recorded trap is specifically about attributing a threshold-sensitive LTCG failure to
// whichever edit happened to be in hand, and an unexplained `noinline` sitting next to an ICE would
// read to the next person as the cure.
#if defined(_MSC_VER)
#define MH_BINDER __declspec(noinline)
#else
#define MH_BINDER
#endif

namespace mh::lockstep {

// The `RID_*` enumerators live in `mh::state`, and every binder below names them. Hoisted to file
// scope rather than repeated per function: this TU exists to name addresses, so the region registry
// being in scope throughout is the point rather than a shortcut.
using namespace mh::state;

MH_BINDER engine_state state() {
    // Addresses come from the state region REGISTRY (RI-STATE / ST1), never spelled here.
    // _G_LLM_NET_LOCKSTEP_PEER_TIMING's base was corrected from 0xe58c44 to 0xe58c40 while writing
    // this module -- exactly the drift the generated layer exists for; the registry adds the second
    // half of that guarantee, which is that no OTHER consumer of the same bytes can hold a different
    // answer (its static_asserts pin every entry to mh_addrs.gen.h).
    //
    // THE TWO ORDER COUNTS ARE NOT BOUND HERE ANY MORE. They used to be, off the same two addresses
    // mh::orders binds -- one region with two owners, which is precisely what makes a later island
    // move break a consumer nobody remembered. They come from the module that owns them now.
    using namespace mh::state;
    const engine_state st = {
        ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
        ptr<double>(RID_STRAT_LOCKSTEP_COMMITTED_HORIZON),
        ptr<double>(RID_NET_PEER_HORIZON),
        ptr<double>(RID_NET_PEER_HORIZON_PENDING),
        ptr<peer_timing>(RID_NET_LOCKSTEP_PEER_TIMING),
        ptr<uint8_t>(RID_NET_LOCKSTEP_PEER_STATE),
        ptr<const player_profile>(RID_STRAT_PLAYERS),
        ptr<const uint16_t>(RID_PLAYERSIDE),
        ptr<const double>(RID_STRAT_GAME_CLOCK),
        ptr<const int32_t>(RID_NET_LOCKSTEP_PLAYER_COUNT),
        ptr<const double>(RID_NET_LOCKSTEP_EXTEND_MARGIN_1),
        ptr<const double>(RID_NET_LOCKSTEP_EXTEND_MARGIN_2),
        ptr<const double>(RID_NET_LOCKSTEP_KEEPALIVE_MARGIN_MUL),
        ptr<const double>(RID_NET_LOCKSTEP_KEEPALIVE_STEP_MUL),
        // ---- batch B ----
        ptr<double>(RID_STRAT_GAME_CLOCK),
        ptr<double>(RID_GAME_TIME_DELTA),
        ptr<const double>(RID_TOTAL_GAME_TIME),
        ptr<const double>(RID_STRAT_SIM_STEP_INTERVAL),
        ptr<const int32_t>(RID_GAME_SESSION_MODE),
        mh::orders::pending_count(),
        mh::orders::staging_count(),
        ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        ptr<const double>(RID_NET_LOCKSTEP_PUMP_REFILL_FRAC),
        ptr<int32_t>(RID_STRAT_FLOATING_MSG_QUEUE_ACTIVE),
        ptr<void>(RID_G_TEXT_TMP),
    };
    return st;
}

MH_BINDER dispatch_state dstate() {
    const dispatch_state ds = {
        mh::net::packet_buffer{mh::state::ptr<uint8_t>(RID_NET_SEND_BUF),
                               mh::state::ptr<int32_t>(RID_NET_SEND_BUF_CURSOR)},
        mh::state::ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS),
        // The SAME array engine_state::players points at, deliberately: `participates()` reads through
        // the const alias while these handlers write through this one, and the two must alias or the
        // eliminate sweeps would test stale flags. lockstest binds both to one buffer for that reason.
        mh::state::ptr<player_profile>(RID_STRAT_PLAYERS),
        mh::state::ptr<const int32_t>(RID_NET_LOCAL_PLAYER_INDEX),
        mh::state::ptr<const int32_t>(RID_NET_ACTIVE_PLAYER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_PLAYER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOBBY_SCAN_HOST_COUNT),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_ADAPT_NEXT_TIME),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT),
        mh::state::ptr<const int32_t>(RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_STALL_NAG_COUNT),
        mh::state::ptr<int32_t>(RID_STRAT_LOCKSTEP_STALL_COUNT),
        mh::state::ptr<int32_t>(RID_NET_RESYNC_IN_PROGRESS),
        mh::state::ptr<const int32_t>(RID_DEBUG_TAP_FLAG),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_EMERGENCY_STEP_MUL),
        mh::state::ptr<void>(RID_MESSAGE_QUEUE),
        mh::state::ptr<void *const>(RID_G_TEXT_PTRS),
        &dispatch_gate_audit().evals,
        &dispatch_gate_audit().allowed,
        mh::state::ptr<void>(RID_G_TEXT_TMP),
        mh::state::ptr<char>(RID_S_NETGAMEREAD_005017A8),
    };
    return ds;
}

MH_BINDER timekeeper_state timekeeper() {
    const timekeeper_state s = {
        mh::state::ptr<double>(RID_LAST_GAME_TIME),
        mh::state::ptr<double>(RID_CURRENT_GAME_TIME),
        mh::state::ptr<const double>(RID_GAME_SPEED),
        mh::state::ptr<double>(RID_GAME_TIME_DELTA),
        mh::state::ptr<double>(RID_TOTAL_GAME_TIME),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_COMMITTED_HORIZON),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
        mh::state::ptr<const double>(RID_STRAT_GAME_CLOCK),
        mh::state::ptr<const double>(RID_STRAT_SIM_STEP_INTERVAL),
        mh::state::ptr<const int32_t>(RID_GAME_SESSION_MODE),

        mh::state::ptr<int32_t>(RID_NET_SYNC_WAIT_ACTIVE),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT),
        mh::state::ptr<double>(RID_NET_LOCKSTEP_SYNC_WAIT_ELAPSED),
        mh::state::ptr<double>(RID_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED),
        mh::state::ptr<const double>(RID_NET_LOCKSTEP_PEER_TIMEOUT_SECS),
        mh::state::ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_PLAYER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOBBY_SCAN_HOST_COUNT),
        mh::state::ptr<const int32_t>(RID_NET_LOCAL_PLAYER_INDEX),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_STALL_NAG_COUNT),
        mh::state::ptr<const uint8_t>(RID_NET_LOCKSTEP_PEER_STATE),
        mh::state::ptr<const uint16_t>(RID_PLAYERSIDE),

        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_ADAPT_NEXT_TIME),
        mh::state::ptr<int32_t>(RID_STRAT_LOCKSTEP_STALL_COUNT),
        mh::state::ptr<const uint32_t>(RID_NET_ACTIVE_PLAYER_COUNT),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_MAX),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_GROW_MUL),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SHRINK_DIV),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_MIN_FPS_NUM_CMP),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_MIN_FPS_NUM_SET),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_ADAPT_INTERVAL_SECS),
        mh::state::ptr<double>(RID_STRAT_FPS_ESTIMATE),
        mh::state::ptr<double>(RID_STRAT_FRAME_TIME_RING),
        mh::state::ptr<int32_t>(RID_STRAT_FRAME_TIME_RING_IDX),
        mh::state::ptr<const double>(RID_STRAT_FPS_WINDOW_NUM),
    };
    return s;
}

MH_BINDER session_init_state live_session_init_state() {
    const session_init_state st = {
        mh::state::ptr<int32_t>(RID_NET_LOCAL_PLAYER_INDEX),
        mh::state::ptr<int32_t>(RID_NET_LOCAL_PLAYER_SLOT),
        mh::state::ptr<int32_t>(RID_NET_IS_HOST),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_PLAYER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOBBY_MAP_RECV_DONE),
        mh::state::ptr<int32_t>(RID_NET_SEND_BUF_CURSOR),
        mh::state::ptr<int32_t>(RID_MP_PROBE_SERVER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_SESSION_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOBBY_SCAN_HOST_COUNT),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_COMMITTED_HORIZON),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_ADAPT_NEXT_TIME),
        mh::state::ptr<int32_t>(RID_GAME_SESSION_MODE),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_STALL_NAG_COUNT),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT),
        mh::state::ptr<int32_t>(RID_STRAT_LOCKSTEP_STALL_COUNT),
        mh::state::ptr<double>(RID_NET_LOCKSTEP_SYNC_WAIT_ELAPSED),
        mh::state::ptr<double>(RID_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED),
        mh::state::ptr<int32_t>(RID_NET_SYNC_WAIT_ACTIVE),
        mh::state::ptr<int32_t>(RID_NET_RESYNC_IN_PROGRESS),
    };
    return st;
}

MH_BINDER wait_screen_state live_wait_screen_state() {
    const wait_screen_state st = {
        mh::state::ptr<const uint32_t>(RID_NET_LOCKSTEP_RESYNC_DEADLINE_MS),
        {
            mh::state::ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS),
            mh::state::ptr<int32_t>(RID_NET_RESYNC_IN_PROGRESS),
        },
    };
    return st;
}

MH_BINDER emit_state estate() {
    // Every address here already has a same-value twin bound somewhere else in this module (see the
    // field comments in tx_emit.h); gen_dll_addrs.py still verifies each one independently against
    // docs/symbols.md, so a Ghidra rename/move is a build failure here too, same as turn_engine.cpp.
    const emit_state es = {
        mh::net::packet_buffer{mh::state::ptr<uint8_t>(RID_NET_SEND_BUF),
                               mh::state::ptr<int32_t>(RID_NET_SEND_BUF_CURSOR)},
        mh::state::ptr<const double>(RID_NET_PEER_HORIZON),
        mh::state::ptr<double>(RID_NET_PEER_HORIZON_PENDING),
        mh::state::ptr<uint8_t>(RID_NET_LOCKSTEP_PEER_STATE),
        mh::state::ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS),
        mh::state::ptr<double>(RID_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED),
        mh::state::ptr<const uint16_t>(RID_PLAYERSIDE),
        mh::state::ptr<player_profile>(RID_STRAT_PLAYERS),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT),
        mh::state::ptr<const int32_t>(RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN),
        mh::state::ptr<const int32_t>(RID_NET_ACTIVE_PLAYER_COUNT),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_HORIZON),
        mh::state::ptr<const double>(RID_STRAT_GAME_CLOCK),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
    };
    return es;
}

MH_BINDER emit_chat_state chat_estate() {
    const emit_chat_state es = {
        mh::net::packet_buffer{mh::state::ptr<uint8_t>(RID_NET_SEND_BUF),
                               mh::state::ptr<int32_t>(RID_NET_SEND_BUF_CURSOR)},

        // _G_LLM_CHAT_TARGET_MASK @ 0x00e5898b. This was a bare `reinterpret_cast<const uint8_t *>
        // (0x00e5898bu)` from 2026-07-29 until SB-BIND T4, with a comment explaining that the symbol
        // was absent from mh_addrs.gen.h and the addr manifest and that a follow-up had to add it.
        // The follow-up landed at some point in between -- the symbol and RID_CHAT_TARGET_MASK both
        // exist now -- but nothing pointed the literal at them, because a resolved TODO in a comment
        // is not something any gate re-reads. This is what tools/check_sim_addresses.py's raw-VA arm
        // is for, and the literal is why the arm predates the named-VA one.
        mh::state::ptr<const uint8_t>(RID_CHAT_TARGET_MASK),
    };
    return es;
}

MH_BINDER order_tx_state live_order_tx_state() {
    const order_tx_state st = {
        mh::state::ptr<uint8_t>(RID_NET_SEND_BUF),
        mh::state::ptr<int32_t>(RID_NET_SEND_BUF_CURSOR),
    };
    return st;
}

MH_BINDER resync_state live_resync_state() {
    const resync_state st = {
        mh::state::ptr<const player_profile_r>(RID_STRAT_PLAYERS),
        mh::state::ptr<int32_t>(RID_NET_ACTIVE_PLAYER_COUNT),
        mh::state::ptr<const int32_t>(RID_NET_LOCAL_PLAYER_SLOT),
        mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT),
        mh::state::ptr<int32_t>(RID_NET_RESYNC_IN_PROGRESS),
        mh::state::ptr<uint32_t>(RID_NET_LOCKSTEP_RESYNC_DEADLINE_MS),
        mh::state::ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
        mh::state::ptr<const double>(RID_STRAT_GAME_CLOCK),
        mh::state::ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SIZE),
        mh::state::ptr<const double>(RID_NET_LOCKSTEP_RESYNC_TIMEOUT_MS),
        mh::state::ptr<const double>(RID_NET_LOCKSTEP_RESYNC_DEADLINE_PAD_MS),
    };
    return st;
}

MH_BINDER chat_ally_mask_state live_chat_ally_mask_state() {
    const chat_ally_mask_state st = {
        mh::state::ptr<const uint8_t>(RID_CHAT_TARGET_MODE),
        mh::state::ptr<uint8_t>(RID_CHAT_TARGET_MASK),
        mh::state::ptr<const mh::game::mh_llm_strat_player_profile>(RID_STRAT_PLAYERS),
        mh::state::ptr<const uint16_t>(RID_PLAYERSIDE),
        mh::state::ptr<const mh::game::mh_llm_strat_player_desc>(RID_PLAYERS),
    };
    return st;
}

// SIMABI-CHAT (2026-09-10). The two internalized chat-target entries. Same two bytes as the binder
// above, but MODE is WRITABLE here (recalc is its only writer image-wide) and there is no
// `player_desc` row -- the relation reads in the original feed a local nothing reads back.
MH_BINDER chat_target_state live_chat_target_state() {
    const chat_target_state st = {
        mh::state::ptr<uint8_t>(RID_CHAT_TARGET_MODE),
        mh::state::ptr<uint8_t>(RID_CHAT_TARGET_MASK),
        mh::state::ptr<const mh::game::mh_llm_strat_player_profile>(RID_STRAT_PLAYERS),
        mh::state::ptr<const uint16_t>(RID_PLAYERSIDE),
    };
    return st;
}

MH_BINDER reload_state live_reload_state() {
    const reload_state st = {
        mh::state::ptr<int32_t>(RID_SND_ENABLED),
        mh::state::ptr<double>(RID_STRAT_GAME_CLOCK),
        mh::state::ptr<int32_t>(RID_STRAT_SIM_ACTIVE),
        mh::state::ptr<mh::game::mh_map_object_unit>(RID_UNITS),
        mh::state::ptr<mh::game::mh_map_object_building>(RID_BUILDINGS),
        mh::state::ptr<const double>(RID_STRAT_RELOAD_TICK_BACKDATE),
        mh::state::ptr<const double>(RID_STRAT_RELOAD_CYCLE_BACKDATE),
        // SB-BIND T2: the derived row capacities, re-derived on every call like every pointer above.
        // This carried a note that the struct was a `static const` which did not follow a rebase and
        // that T4's lockstep tranche would fix it; T4 is this file, and it did.
        mh::state::live_roster_caps(),
    };
    return st;
}

MH_BINDER lt_time_query_state time_query_state() {
    // Both constants are registry-pinned (RID_GAME_CLOCK_RAW_TICKS / RID_GAME_CLOCK_TICKS_PER_SECOND
    // static_asserts in mh_regions.gen.h), registered LT1E 2026-09-02.
    const lt_time_query_state st = {
        mh::state::ptr<const uint32_t>(RID_GAME_CLOCK_RAW_TICKS),
        mh::state::ptr<const double>(RID_GAME_CLOCK_TICKS_PER_SECOND),
    };
    return st;
}

// ---- the production-arm binds (lockstep_state.h) -------------------------------------------------
MH_BINDER host_bind_state host_binds() {
    using namespace mh::state;
    return host_bind_state{
        ptr<void>(RID_G_TEXT_TMP),
        ptr<void *const>(RID_G_TEXT_PTRS),
        ptr<const uint8_t>(RID_PLANETS),
        ptr<const uint8_t>(RID_GAME_SESSION_MODE),
        ptr<char>(RID_STRAT_PLAYERS),
    };
}

} // namespace mh::lockstep
