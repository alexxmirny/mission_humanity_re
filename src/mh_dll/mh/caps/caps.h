//
// caps/caps.h -- the roster-geometry table for shipped DLL code (patching:PT-CAP8 v0.2.2, slice B).
//
// WHY THIS EXISTS. mh.dll code that touches the nine per-player arrays used to learn their shape one
// of two ways: a hard-coded vanilla constant (stride 0x6aa4, a stock VA) or `live_roster_caps()` +
// `live().base[RID_*]`. The v0.2.2 cap raise relocates the seven raised arrays into wider ones and
// leaves the stock .bss extents as a vanilla-shaped canonical MIRROR (D1 of the S1 spec), so a hook
// that wants LIVE game memory must not use the registry's stock base any more -- it needs the base,
// the row width in records and the live limit of the array the GAME is using. This table is that one
// source (D3). Every shipped hook and naked stub reads it.
//
//   base    -- address of record [player 0][0] of the array the game addresses
//   elem    -- sizeof(record) (never changes)
//   row     -- records per player row (the STRIDE: 100 vanilla, 500 raised buildings, ...)
//   limit   -- the live index bound (a validity check / loop bound); <= row
//   players -- player rows (8; the projectile pool is ONE row of `row` records)
//
// SLICE B IS BEHAVIOUR-NEUTRAL: the table holds the STOCK values (stock base, vanilla row, vanilla
// limit) and nothing in this slice changes them. configure()/reset_stock() exist so slice C's
// installer (and capstest) can fill the table; the 3 compose stubs and every converted hook then
// follow it without further edits.
//
// PT-CAP9-U1 (v0.2.3): UNITS and SOLDIERS join the relocated set. They live in their own section
// (`.mhcapu`, adopt_units_relocation) and have their own limit layer (units_limits_state); set_limits() flips
// both layers as one (the v0.2.2 layer first, the units layer second, rolled back together on a refusal).
//
// RULE for callers: stride/indexing -> g(X).row (use rec<T>); validity checks and loop bounds ->
// g(X).limit; live game memory -> g(X).base. No shipped file under mh/ may call live_roster_caps()
// (tools/lint_repo.py: no_live_roster_caps) -- libmh runs only on the unrelocated layout.
//
// The same table is exported to naked __asm stubs as three extern "C" dword arrays indexed by the
// MH_CAPS_IDX_* macros (macros, because MSVC inline asm cannot take a scoped enum):
//   mov  edx, dword ptr [mh_caps_base + 4 * MH_CAPS_IDX_TURRETS]
//   imul ecx, dword ptr [mh_caps_row_bytes + 4 * MH_CAPS_IDX_BUILDINGS]   // row * elem, bytes
//
#pragma once
#include <cstdint>

namespace mh::patch {
struct toggle_state;
}

#define MH_CAPS_IDX_BUILDINGS    0
#define MH_CAPS_IDX_UNIT_STORAGE 1
#define MH_CAPS_IDX_TURRETS      2
#define MH_CAPS_IDX_MINES        3
#define MH_CAPS_IDX_LABS         4
#define MH_CAPS_IDX_PRODUCTIONS  5
#define MH_CAPS_IDX_PROJECTILES  6
#define MH_CAPS_IDX_UNITS        7
#define MH_CAPS_IDX_SOLDIERS     8
#define MH_CAPS_COUNT            9

namespace mh::caps {

enum arr : uint8_t {
    BUILDINGS    = MH_CAPS_IDX_BUILDINGS,
    UNIT_STORAGE = MH_CAPS_IDX_UNIT_STORAGE,
    TURRETS      = MH_CAPS_IDX_TURRETS,
    MINES        = MH_CAPS_IDX_MINES,
    LABS         = MH_CAPS_IDX_LABS,
    PRODUCTIONS  = MH_CAPS_IDX_PRODUCTIONS,
    PROJECTILES  = MH_CAPS_IDX_PROJECTILES,
    UNITS        = MH_CAPS_IDX_UNITS,
    SOLDIERS     = MH_CAPS_IDX_SOLDIERS,
    N            = MH_CAPS_COUNT,
};

struct geom {
    uint32_t base;    // VA of record [0][0]
    uint32_t elem;    // sizeof(record)
    uint32_t row;     // records per player row (stride)
    uint32_t limit;   // live index bound (<= row)
    uint32_t players; // player rows
};

// The live geometry of array `a`.
const geom &g(arr a);

// &array[player][idx] -- base + (player * row + idx) * elem. No bounds check: callers test player
// against g(a).players and idx against g(a).limit themselves, exactly as the code they replace did.
template <class T>
inline T *rec(arr a, uint32_t player, uint32_t idx) {
    const geom &x = g(a);
    return reinterpret_cast<T *>(static_cast<uintptr_t>(x.base) +
                                 (static_cast<uintptr_t>(player) * x.row + idx) * x.elem);
}

// Bytes of one player row (row * elem) -- what the game's `imul reg, reg, <stride>` multiplies by.
inline uint32_t row_bytes(arr a) { return g(a).row * g(a).elem; }

// True once the relocation manifest has been applied / the raised limits are on.
bool relocated();
bool raised();

// Fill / restore the table. configure() refuses (returns false, table unchanged) on a base of 0, row 0,
// limit 0 or limit > row. reset_stock() restores EVERY piece of
// state -- the table, relocated(), raised(), the staged limit layer, the canon overrides -- so capstest
// starts each arm from the shipped-without-caps configuration.
bool configure(arr a, uint32_t base, uint32_t row, uint32_t limit);
void reset_stock();

// ---- PT-CAP8 slice C: the relocated layout, the limit layer, the canonical mirror -----------------
//
// The seven raised arrays live at fixed offsets inside the `.mhcapd` section the relocation manifest
// places (src/patcher/cap_v022_reloc_EN.mh.patch.json `_proofs.arrays`; capstest asserts the manifest's
// own refs against these). Each is `players` rows of `RAISED_ROW` records.
inline constexpr unsigned RAISED_COUNT = 7; // BUILDINGS .. PROJECTILES: the v0.2.2 arrays, hosted in `.mhcapd`
inline constexpr unsigned WIDE_COUNT   = 9; // + UNITS, SOLDIERS (v0.2.3): hosted in `.mhcapu`
extern const uint32_t     CAPD_OFFSET[RAISED_COUNT];
extern const uint32_t     CAPU_OFFSET[2];         // UNITS, SOLDIERS inside `.mhcapu`
extern const uint32_t     RAISED_ROW[WIDE_COUNT]; // records per player row when limits are ON (500, 127 x5, 4000, 500, 500)

// The relocation manifest was applied and `.mhcapd` landed at `capd_base`: point the table at the wide
// arrays (row = RAISED_ROW, limit = the vanilla limit). Returns false (table unchanged) on a zero base.
bool adopt_relocation(uint32_t capd_base);
// The units manifest was applied and `.mhcapu` landed at `capu_base`: point UNITS and SOLDIERS at their wide rows
// (row = 500, limit = 100). False (table unchanged) on a zero base. Independent of adopt_relocation() -- either
// may be refused without the other -- but the limit layers only switch ON together (set_limits).
bool adopt_units_relocation(uint32_t capu_base);
bool units_relocated();
// True when array `a` currently lives in a wide (relocated) block: a < 7 -> relocated(), UNITS/SOLDIERS ->
// units_relocated().
bool wide(arr a);

// The limit layer's staged toggle state (filled by patch::apply on the caps_limits manifest). The
// installer applies the manifest into THIS object.
mh::patch::toggle_state &limits_state();
// The units layer's staged toggle state (cap_v023_units_limits_EN); set_limits() flips it with the v0.2.2 layer.
mh::patch::toggle_state &units_limits_state();

// Switch the limit layer ON/OFF (D2: two-state code bytes). Refuses -- logs, returns false, nothing
// changes -- when asked to go ON while !relocated() or without a staged layer, when the layer's bytes
// are foreign (toggle()), or while a strategic world is live and stepping. Asking for the state it is
// already in succeeds without a write. `why` names the caller in the log line.
bool set_limits(bool on, const char *why);

// "A strategic world is live" for set_limits' guard: the sim-step feeders call note_sim_step() each step;
// the world counts as live for 1500 ms after the last one. note_world_ended() clears it (session close).
void note_sim_step();
void note_world_ended();

// PT-CAP9-U8b: the session-close path. The close can run INSIDE a sim step (on_gameover is called from the unit tick), so it must
// not flip the limit sites there. end_world_deferred(revert): if a world has stepped, remember `revert` and return true (nothing
// changes yet); otherwise call it now and return false. quiescent_point() -- called from a place no sim step is on the stack (the
// presents at the main menu) -- ends the world (clears the "live" guard) and runs the remembered revert. Sim steps after the close do
// NOT drop it (the game-over dialog's modal loop keeps stepping). set_limits()'s own live-world guard stays authoritative.
bool end_world_deferred(void (*revert)());
bool world_end_pending();
void quiescent_point();

// D1: the stock .bss extents are the CANONICAL MIRROR of the wide arrays. No-ops unless relocated().
//   canon_gather():  wide rows [p][0..vanilla_cap) -> stock rows       (every wide array, units and soldiers included)
//   canon_scatter(): stock rows -> wide rows [p][0..vanilla_cap), and the rows' tails
//                    [vanilla_cap..raised_cap) are ZEROED (a loaded world has nothing past the window)
void canon_gather();
void canon_scatter();
// Records past the vanilla window that hold any non-zero byte, summed over the wide arrays (the save
// hook's "N records NOT saved" figure; ON only).
uint32_t overflow_records();
// Dev: `[caps] mirror_check=1` -- remember the last gathered/scattered mirror and report (once per array)
// the first byte that changed between two gathers, i.e. a writer that touched a STOCK address.
void set_mirror_check(bool on);
// Test seams: point an array's stock mirror at a buffer of the vanilla extent (null = the real VA).
void set_canon_stock_for_test(arr a, uint8_t *p);
// Test seam: force raised() (the limit layer itself is capstest's subject); reset_stock() clears it.
void set_raised_for_test(bool on);

// ---- PT-CAP11: the AI scratch layer ---------------------------------------------------------------------
//
// With the raise ON an AI player can own more buildings / mines than the vanilla AI scratch arrays hold (eight fixed .bss arrays with no
// bound check: src/patcher/cap_v022_aiscratch_EN.mh.patch.json, tools/gen_cap_ai_manifest.py). The layer is a TOGGLE manifest (class
// caps_ai) staged at boot and flipped by set_limits() together with the limit layer, so raise OFF is byte-identical to vanilla.
// `.mhcapa` holds the enlarged arrays; each is followed by ai_scratch_layout::GUARD_BYTES of GUARD_FILL, checked every sim step.
mh::patch::toggle_state &ai_state();
// The AI manifest was applied and `.mhcapa` landed at `section_base`: remember it and write the guard fill. False on a zero base.
bool ai_adopt(uint32_t section_base);
// True while the AI layer is switched ON (the arrays in use are the enlarged ones).
bool ai_on();
// Bit i set = array i's guard has been overwritten (sticky; 0 = clean or not adopted). Logs `AI GUARD HIT` once per array.
uint32_t ai_guard_check();
// Highest element (1-based, 0 = untouched) ever seen in array i by the census; > ai_scratch_layout::ARRAYS[i].stock_count means a
// vanilla layout would have overflowed. Sampled every few sim steps while the layer is ON.
uint32_t ai_high_water(unsigned i);

// Route diagnostics somewhere (production: seam_log).
void set_logger(void (*fn)(const char *));

// The STOCK geometry (the constants reset_stock() restores) -- exposed for tests and for the parity
// arms that compare "table == today's hard-coded values".
const geom &stock(arr a);

} // namespace mh::caps

// Naked-stub mirrors of the table (kept in step by configure()/reset_stock()).
extern "C" {
extern uint32_t mh_caps_base[MH_CAPS_COUNT];      // g(a).base
extern uint32_t mh_caps_row_bytes[MH_CAPS_COUNT]; // g(a).row * g(a).elem
extern uint32_t mh_caps_limit[MH_CAPS_COUNT];     // g(a).limit
}
