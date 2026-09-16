//
// tact/tact_journal.h -- which calls to the two tactical ORDER SEAMS are the PLAYER's (TACT-REC).
//
// Extracted from seams/harness.cpp so it can be asserted offline. The classification is the order
// journal's whole correctness question: both seams are ALSO called by code that runs with no input,
// and journalling one of those issues it TWICE on replay -- once by the game, once by the journal.
// A rule that important should be provable without a rig run, which is what `tacttest` does with it.
//
// THE RULE IS THE CALLER'S EXTENT, not a list of return addresses. Both are derivable from the same
// xref set; the extent survives an instruction changing length and a `call+5` table does not. It
// also happens to be ONE test for BOTH seams, because every player site in both lists falls inside
// one of these two ranges and no engine site does.
//
// Measured from the EN xref set, 2026-08-25 (`find-cross-references` over /eng/mh.exe):
//
//   llm_tact_unit_enqueue_command @0x0042b39d -- 22 call sites
//     PLAYER  6  llm_tact_frame                         0x429f0e 42a403 42a571 42a58a 42a5b8 42a5d1
//     PLAYER  1  llm_tact_ui_sel_panel_multi_mode_tick   0x436722
//     nested  3  llm_tact_group_issue_order              0x42b1ce 42b36c 42b38a -- journalled at the
//                                                        GROUP seam instead, or it lands twice
//     engine  9  llm_tact_unit_owner_tick                the AI / idle-wander arm
//     engine  1  llm_tact_mission_load                   the mission script
//     engine  1  llm_tact_unit_cmd_queue_resubmit_run    the REPEAT resubmit
//     engine  1  llm_tact_fx_update_projectile           the facing correction
//
//   llm_tact_group_issue_order @0x0042b09f -- 13 call sites, and EVERY ONE is a UI function:
//     PLAYER  6  llm_tact_frame
//     PLAYER  7  llm_tact_ui_order_buttons_minimap_tick
//
// Note the two UI functions are ADJACENT in the image -- ui_order_buttons_minimap_tick ends exactly
// where ui_sel_panel_multi_mode_tick begins -- so one range covers both. That is a property of this
// build, not a design, and the offline test pins it: if a future build separates them, the test
// fails rather than the journal silently dropping the sel-panel site.
//
#pragma once
#include <cstdint>

namespace mh::tact {

// Function extents, from Ghidra (entry .. body max + 1).
inline constexpr uint32_t TJ_FRAME_LO = 0x00429b1au; // llm_tact_frame                         4709 B
inline constexpr uint32_t TJ_FRAME_HI = 0x0042ad7fu;
inline constexpr uint32_t TJ_UI_LO    = 0x00435ceau; // llm_tact_ui_order_buttons_minimap_tick 1227 B
inline constexpr uint32_t TJ_UI_HI    = 0x004369c1u; // ..._sel_panel_multi_mode_tick, adjacent 2060 B

// `pc` is the RETURN address the seam's detour captured, i.e. an address inside the caller.
inline constexpr bool tj_is_player(uint32_t pc) {
    return (pc >= TJ_FRAME_LO && pc < TJ_FRAME_HI) || (pc >= TJ_UI_LO && pc < TJ_UI_HI);
}

// ---- the direct-write actions --------------------------------------------------------------
//
// Three player actions have NO OPCODE: the UI writes the unit record directly and nothing passes
// through either seam. An order-only journal is lossy without them.
//
//   active_gun     +0x5eb  the gun swap                (`active_gun ^= 1`)
//   squad_group_id +0x5f0  control-group assign/reset  (sidebar icon, or 0xff to clear)
//   def_stat       +0x006  the sidebar def_stat cycle  (its 3->4 step ALSO enqueues; that half is
//                          journalled at the seam, this is the other half)
//
// They are journalled by WATCHING THE FIELDS rather than by detouring each write site: the writes
// are scattered across several UI functions, and a site list goes stale the moment one moves.
//   SELECTION      +0x002  status BIT 0 -- added 2026-08-25 after a replay of a real 63,563-frame
//                          session fought no battle at all. llm_tact_group_issue_order qualifies its
//                          loop on `status & 1`, so with nothing selected EVERY group order applies
//                          to an empty set and silently does nothing. Selecting a unit is a CLICK,
//                          not an order, so it passed through neither seam -- the most load-bearing
//                          player action in the game was the one the journal did not have. It is a
//                          BIT, not a byte: the same status carries engine bits (8 = FIRE,
//                          0x60 = active/animated), and writing it wholesale on replay would stamp
//                          the recording's engine state over the replay's own.
enum tj_direct_field : uint8_t {
    TJ_F_GUN   = 0,
    TJ_F_GROUP = 1,
    TJ_F_DEF   = 2,
    TJ_F_SEL   = 3, // status bit 0, masked -- see TJ_DIRECT_MASK
    TJ_F_COUNT = 4
};
inline constexpr uint32_t TJ_DIRECT_OFF[TJ_F_COUNT] = {0x5ebu, 0x5f0u, 0x006u, 0x002u};
// 0xff = the whole byte is the player's. Anything else is the bit mask the journal owns; the rest of
// the byte is left exactly as the replay's own engine set it.
inline constexpr uint8_t TJ_DIRECT_MASK[TJ_F_COUNT] = {0xffu, 0xffu, 0xffu, 0x01u};

// ---- the ORDER stream, watched rather than hooked (the `Q` record) ---------------------------
//
// WHY THIS EXISTS, and it is the same lesson as the direct-write watch one level up. The `E`/`G`
// records above come from TRAMPOLINES on the ORIGINAL entries llm_tact_unit_enqueue_command and
// llm_tact_group_issue_order. That is exactly the wrong place to stand once our own bodies start
// running: a promoted llm_tact_frame calls mh::tact::unit_enqueue_command directly -- ours to ours
// -- and never crosses either entry. Measured 2026-09-04 on both recorded sessions: with
// `[promote] tact_frame=1` the recorder sees ZERO E and ZERO G records, while the mission outcome,
// the survivor set, the end frame and the pinned RNG draw series are all IDENTICAL. The orders were
// issued the whole time; the instrument was looking at a door nobody used any more.
//
// So the differential gate had to call its own order diff ADVISORY, which is the worst outcome an
// instrument can reach -- present, printed, and explicitly not to be believed. This closes it.
//
// THE FIX IS TO WATCH STATE, NOT CALLS, which is precisely why the direct-write watch above never
// had the problem: it polls four unit fields at the top of every frame and journals whoever wrote
// the byte -- engine, original, or ours. A command queue is state too. Watching the 128 queue slots
// (plus the two IMMEDIATE records the enqueue writes wholesale on its op==2 / op==6 short-circuits,
// which never reach the queue at all) sees every order that lands, no matter which body put it
// there, and it cannot be bypassed by any future promotion or rebind. It is also indifferent to
// llm_tact_unit_enqueue_command's own entry being claimed by some other mechanism.
//
// WHAT IT IS FOR, stated because the scope is narrower than E/G's. This is a COMPARISON channel,
// not a replay channel: nothing injects a Q record back into a run. E/G stay the replay path
// (they carry the player-vs-engine classification a replay needs, which a state watch cannot
// recover -- a slot does not remember who filled it). Q is what two arms are diffed on.
//
// THE THREE LIMITS, none of them hidden:
//   * PER-FRAME, so an order enqueued and dequeued inside one frame is invisible. Same one-frame
//     convention the direct-write watch documents, and the same reason it is acceptable: both arms
//     of a differential see the same blind spot, so it cancels.
//   * NOT OWNERSHIP-CLASSIFIED. The watch restricts to owner-0 occupied slots exactly as the direct
//     watch does, but llm_tact_unit_owner_tick's idle-wander arm also enqueues onto player units --
//     so a Q record is "an order landed", not "the player ordered it". For an A/B that is a feature:
//     an engine order that differs between arms is a divergence worth failing on.
//   * A DEQUEUE IS NOT RECORDED. Only a slot becoming or changing while LIVE (op != 0) emits. Queue
//     drain is engine bookkeeping and would swamp the file with the sim's own progress.
inline constexpr uint32_t TJ_Q_BASE   = 0x54u; // cmd_queue[0]; see mh_tact_unit_record
inline constexpr uint32_t TJ_Q_STRIDE = 0x0bu; // sizeof(mh_llm_tact_unit_cmd_entry)
inline constexpr int      TJ_Q_SLOTS  = 128;   // 0x80 entries, index wraps at 0x7f
// The two immediate records, same 0xb shape, watched as pseudo-slots 128 and 129. They are the ONLY
// way op 2 (ATTACK/AIM) and op 6 (FACE/TURN) are seen at all: the enqueue short-circuits both into
// these instead of appending (0x0042b79f-0x0042b7f2 and 0x0042b72c-0x0042b77a).
inline constexpr uint32_t TJ_Q_IMM[2] = {0x5d4u, 0x5dfu};
inline constexpr int      TJ_Q_TOTAL  = TJ_Q_SLOTS + 2;

// Byte offset of watched slot `s` within a unit record.
inline constexpr uint32_t tj_q_off(int s) {
    return s < TJ_Q_SLOTS ? TJ_Q_BASE + TJ_Q_STRIDE * (uint32_t)s : TJ_Q_IMM[s - TJ_Q_SLOTS];
}

// Entry field readers. Byte-wise rather than a struct cast: the queue is 0xb bytes at a 0xb stride,
// so every odd slot puts the uint16_t fields on an odd address.
inline constexpr uint16_t tj_q_op(const uint8_t *e) {
    return (uint16_t)((uint16_t)e[1] | (uint16_t)((uint16_t)e[2] << 8));
}
inline constexpr uint16_t tj_q_arg(const uint8_t *e, int i) {
    return (uint16_t)((uint16_t)e[3 + 2 * i] | (uint16_t)((uint16_t)e[4 + 2 * i] << 8));
}

// Record iff the slot is LIVE now and its bytes moved. The op==0 early-out is what keeps a dequeue
// out of the journal, and it is also why interrupt_flag going stale on dequeue (it is NOT cleared by
// llm_tact_unit_cmd_advance -- see the struct comment) cannot produce a phantom record.
inline bool tj_q_record(const uint8_t *prev, const uint8_t *now) {
    if (tj_q_op(now) == 0u) return false;
    for (uint32_t i = 0; i < TJ_Q_STRIDE; ++i)
        if (prev[i] != now[i]) return true;
    return false;
}

// SIGNED, despite the name, and that is not cosmetic. The recorder prints the mouse deltas with
// %ld because they ARE signed -- moving left is dx=-1 -- and this scanner used to reject a leading
// '-', which made the whole record short-count and be discarded as malformed. In the first real
// 4.4-minute recording that is 2,154 of 9,311 mouse events: every leftward and every upward motion
// thrown away, with nothing but a "MALFORMED LINES DROPPED" line to show for it. Two's-complement
// round trip: printed as %ld, re-read here, handed to a field the game reads as int32.
inline constexpr const char *tj_scan_u32(const char *p, const char *end, uint32_t *out) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) neg = (*p++ == '-');
    if (p >= end || *p < '0' || *p > '9') return nullptr;
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') v = v * 10u + (uint32_t)(*p++ - '0');
    *out = neg ? (uint32_t)(-(int32_t)v) : v;
    return p;
}

} // namespace mh::tact
