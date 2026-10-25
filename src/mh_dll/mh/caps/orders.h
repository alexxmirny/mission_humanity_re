//
// caps/orders.h -- the ORDER layer of the cap raise (patching:PT-CAP9-U5, v0.2.3): the three order arrays, their live geometry,
// the stock-mirror copy, the high-water / refusal census and the sender quota.
//
// WHY. One click of a raised roster can order hundreds of units; the queue [300], the staging list [300] and the pending list
// [1000] were small and silent (a full queue WIPED every order of the frame; a full REMOTE pending list reset its count = a desync).
// src/patcher/cap_v023_orders_EN.mh.patch.json (tools/gen_cap_orders_manifest.py) is a TOGGLE manifest, flipped with the limit
// layer by caps::set_limits: it points every operand at the enlarged arrays in `.mhcapo`, widens the bounds, turns the two wipes into
// COUNTED REFUSALS and routes the sender-side staging bound through a quota stub in this file.
//
// THE MIRROR. Like every raised roster array the stock extents stay the CANONICAL MIRROR: the first 300 / 1000 / 300 records are
// gathered into them before the hash and before a planet save (the save block keeps its stock addresses) and scattered back after a
// load. The QUEUE tail past the stock window is hashed (an overflow slice, PT-CAP9-U7d); the pending / staging tails are NOT (peer-local in-flight state, like their stock windows, which the manifest excludes). All three are carried past the file / the world blob by the MHOV codec
// (PT-CAP9-U7c, kinds 10 / 11 / 12 in ovf.h): scatter() keeps the counts the stock blocks brought in and finish_scatter() gives them back
// once the codec has restored the tail records, or clamps them (logged) when it did not.
//
// THE GEOMETRY. base(w) / cap(w) are what the GAME is using right now: the stock array and its vanilla bound while the layer is OFF
// (or never staged), the wide array and the raised bound once it is ON. Every shipped reader of the queue goes through them.
//
#pragma once
#include <cstdint>

namespace mh::patch {
struct toggle_state;
}

namespace mh::caps::orders {

enum which : unsigned { QUEUE   = 0,
                        PENDING = 1,
                        STAGING = 2,
                        N       = 3 };

inline constexpr uint32_t REC_BYTES = 0x44;
// Sender quota: the most records (staging + pending) one player may have in flight (user decision 2026-10-21).
inline constexpr uint32_t QUOTA = 2048;

// Route diagnostics somewhere (caps::set_logger forwards its sink here).
void set_log_sink(void (*fn)(const char *));

// ---- the live geometry -------------------------------------------------------------------------------------------
uint8_t *base(which w);      // address of record [0]
uint32_t cap(which w);       // records the game's bound allows right now
uint32_t stock_cap(which w); // 300 / 1000 / 300
uint32_t wide_cap(which w);  // 4096 / 32768 / 2048
bool     on();               // the layer is staged and switched ON
bool     staged();           // the manifest was applied and `.mhcapo` adopted

// ---- the boot / toggle arm (patch/caps_install.cpp, caps::set_limits) --------------------------------------------
mh::patch::toggle_state &state();
// `.mhcapo` landed at `o_base`: remember it, write the guards. False on a zero base.
bool adopt(uint32_t o_base);
// Flip the layer (manifest sites, then the stock<->wide copy, then the O1c carrier rebuild), all at a quiescent point. A failure puts
// the manifest back and returns false with nothing changed. Asking for the state it is already in succeeds.
bool toggle(bool on, const char *why);

// ---- the canonical mirror (called from caps::canon_gather / canon_scatter) ---------------------------------------
void gather();  // wide[0..stock) -> stock; logs once when the queue count is past the stock window
void scatter(); // stock -> wide[0..stock), the wide tails zeroed, the counts clamped to the window; the pre-clamp counts are KEPT for finish_scatter()
// PT-CAP9-U7c: call after the MHOV apply that follows a scatter (load hook, world import, scene import) -- with the apply's result or not at all.
// A count that was past the stock window is RESTORED when the codec brought back exactly the tail records [stock, count) of that array; otherwise it
// stays clamped and the drop is logged. Idempotent; a following scatter() flushes a forgotten one.
void finish_scatter();
// The MHOV codec's view of the arrays (kinds 10 / 11 / 12 = QUEUE / PENDING / STAGING, one array, no player rows):
uint8_t *wide_record(which w, uint32_t idx);                       // record `idx` of the WIDE array; null unless the layer is ON and idx < wide_cap
uint32_t live_count(which w);                                      // the game's live count of w, clamped to [0, cap(w)]
void     import_record(which w, uint32_t idx, const uint8_t *rec); // ovf::apply: write one tail record and note it for finish_scatter()

// ---- census ------------------------------------------------------------------------------------------------------
// One call per sim step (caps::note_sim_step): the guard check, the high-water scan, and a log line when a figure changed.
void     step();
uint32_t high_water(which w); // records ever seen in the array since the layer went ON (the highest non-empty record, 1-based)
uint32_t refused(which w);    // records refused at a full array (queue / pending: the counted ex-wipes; staging: the stub's refusals)
uint32_t refused_quota();     // ... of the staging refusals, the per-player quota ones
uint32_t guard_hits();        // bit i = array i's guard was overwritten (sticky)

// ---- the sender quota (U5c) -------------------------------------------------------------------------------------
// Pure decision, exposed for the selftest: may a record of `owner` (4-bit player) be staged now? `echo` = the 0xfb/0xfb MP echo of an
// order whose primary was accepted. `staging_count` = records already staged; `own_in_flight` = this owner's staging + pending records.
// An order and its echo are accepted or refused TOGETHER: a primary needs room for itself AND its echo.
struct quota_state {
    bool orphan[16]; // the owner's last primary was refused: its echo must be refused too
};
bool quota_accept(quota_state &qs, unsigned owner, bool echo, uint32_t staging_count, uint32_t own_in_flight);
void quota_reset_for_test();

// The player notice for a refused click (one call per sim step at most). Set by the HUD layer; null = log only.
void set_notice_hook(void (*fn)(unsigned owner, bool quota));
// TEST ONLY (`[caps] orders_release_cap`): lower the release_due queue bound to N (0 = off) the next time the layer goes ON.
void set_release_cap_for_test(uint32_t n);

// The O1c carrier owns the two pending disp32 inside its 27-byte local-decode patch (seams/order_wide_index.cpp): toggle() asks it to
// re-aim them from the pending array `from` to `to`. Returns false if it cannot (toggle() then puts the manifest back).
void set_local_rebind(bool (*fn)(uint32_t from_pending_base, uint32_t to_pending_base));

// Test seams: point an array at buffers (null = the real game addresses); reset_for_test() forgets all state.
void set_stock_for_test(which w, uint8_t *stock, int32_t *count_var);
void set_on_for_test(bool on); // pretend the layer is ON over an adopted section (no manifest involved)
void reset_for_test();

// Dev/test: read the figures the way the harness line prints them.
void format_line(char *out, unsigned cap_bytes);

} // namespace mh::caps::orders
