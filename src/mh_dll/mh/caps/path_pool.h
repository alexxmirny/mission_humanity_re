//
// caps/path_pool.h -- the relocated path-slot pool (patching:PT-CAP9-U4, v0.2.3).
//
// WHAT IT IS. Each strategic ground unit that walks owns a PATH SLOT: a row of 300 waypoints ({heading, run}, 2 bytes) in a per-player
// pool, a busy flag per slot and a per-player free-slot counter. The shipped game keeps 100 slots per player and a unit record
// field `path_slot_id` (unit +0xaa) that is ONE BYTE (0xff = no path), so at most 100 units per player move at once. The raise lifts
// that to 500 (patching:PT-CAP9-U4, src/patcher/cap_v023_units_{reloc,limits}_EN.mh.patch.json; census tools/data/cap_path_slot_census.json):
//
//   ALWAYS (the relocation layer, applied at boot whatever `[caps] raise` says): buffers [8][501][300 waypoints] and flags [8][501]
//     live in the appended section `.mhcapp`; every access in the exe is re-based; the stock .bss extents (slots 0..99 per player)
//     become the CANONICAL MIRROR the planet save/load block, the registry and the dumps still read. Slot 500 is the SINK: never
//     handed out, so an unguarded reader that indexes the pool with the "no path" value lands in the player's own spare slot.
//   ON THE RAISE (the limit layer): the six scanners walk 0..499, the free count starts at 500, and the unit's id becomes a 16-bit
//     field at unit +0xda (the unit record's unused hole): 500 = no path. The byte at +0xaa stays, as min(id, 0xfe) / 0xff, so the
//     seventeen `cmp byte [..+0xaa], 0xff` checks keep working. Raise OFF never writes the hole.
//
// WHAT THIS FILE DOES (the DLL half):
//   * the geometry of the live pool (buffers(), flags(), waypoint()) -- every shipped DLL reader goes through it;
//   * the canonical mirror (gather/scatter), called from caps::canon_gather / canon_scatter;
//   * the SAVE (patching:PT-CAP9-U7b = U4d): slots 100..500 ride in the planet file's MHOV trailer (ovf.h, KIND_PATH_SLOT, version 2). The stock
//     blocks carry slots 0..99, the flags 0..99 and the free count; save_begin()/save_end() write a VANILLA-SHAPED free count into the file (the
//     blocks are written from the stock address) and put the live one back;
//   * after_load(): a loaded world's ids are re-derived and each player's free count recomputed. A v2 trailer (FEAT_PATH_ID16) vouches for the units'
//     16-bit ids, which are then trusted whenever their slot is busy; without it (a vanilla / v1 file) the id is re-derived from the byte, slots < 100;
//   * the dev check `[caps] path_check=1`: invariants of the pool and the units, logged when broken;
//   * U4e: the raise is REFUSED while any game function that touches the pool is promoted (libmh): libmh pins 100 slots.
//
#pragma once
#include <cstdint>

namespace mh::caps::path {

inline constexpr uint32_t NO_PATH_U16 = 500; // the 16-bit "no path" value == the sink slot

// The pool landed at host_base (`.mhcapp`); point the geometry at it. False (nothing changed) on a zero base.
bool adopt(uint32_t host_base);
bool relocated();
// Restore the shipped-without-caps state (capstest).
void reset();

// ---- geometry of the live pool ---------------------------------------------------------------------------------------
uint8_t *buffers();             // byte {heading, run}[players][slots][300]: the relocated pool, or the stock array when not relocated
uint8_t *flags();               // byte[players][slots]
uint32_t buffer_player_bytes(); // 60000 stock, 300600 relocated
uint32_t flags_row();           // 100 stock, 501 relocated
uint32_t slot_limit();          // usable slots now: 500 raised, else 100
// &buffers[player][slot][entry] (heading byte; the run byte follows). No bounds check, like the game's own indexing.
uint8_t *waypoint(uint32_t player, uint32_t slot, uint32_t entry);

// ---- the unit record --------------------------------------------------------------------------------------------------
// The unit's path slot id, NO_PATH (0xffffffff) when it has none. `unit` = &units[player][index]. Raised: the 16-bit field.
inline constexpr uint32_t NO_PATH = 0xFFFFFFFFu;
uint32_t                  unit_slot(const uint8_t *unit);
// The id as the BYTE-shaped code (libmh-shared decision headers, seam readers) sees it: the byte when it is exact; 0xff -- "no path" --
// when the raised id does not fit a byte (>= 0xfe), so such a reader fails SAFE instead of indexing another unit's slot.
uint8_t unit_slot_u8(const uint8_t *unit);

// ---- the canonical mirror (called by caps::canon_gather / canon_scatter; no-ops unless relocated) ------------------------
void gather();
void scatter();
// Pure forms over explicit buffers (capstest).
void gather_into(const uint8_t *wbuf, const uint8_t *wflags, uint8_t *sbuf, uint8_t *sflags);
void scatter_from(const uint8_t *sbuf, const uint8_t *sflags, uint8_t *wbuf, uint8_t *wflags);
// Test seam: point the stock mirror at a buffer pair and the free-count array at an int[8] (null = the real VAs).
void set_stock_for_test(uint8_t *buffers, uint8_t *flags, int32_t *count = nullptr);

// After a planet load (the arrays scattered -- AFTER the file's path blocks were read, see caps_install.cpp --, the overflow trailer applied).
// With the raise ON: every unit's 16-bit id is re-derived and each player's free count is recomputed from the busy flags. No-op with the raise OFF.
//   ids16 = false (no trailer / a v1 trailer): the id comes from the byte (0xff -> none; < 100 and the slot busy -> that slot; else none, byte 0xff).
//   ids16 = true  (the trailer carries FEAT_PATH_ID16): the id is the unit's 16-bit word when that slot is busy; else none. The byte is rewritten
//   as min(id, 0xfe) / 0xff either way.
void after_load(bool ids16 = false);

// ---- the save ---------------------------------------------------------------------------------------------------------------
// One path slot as an MHOV record body: byte 0 = the busy flag, bytes 1..600 = the 300 waypoints. Slot 500 is the sink.
inline constexpr uint32_t SLOT_RECORD_BYTES = 601;
// Copy slot (player, slot) out of the live pool; true when any byte is non-zero (a busy flag or stale waypoints). Not relocated: false.
bool slot_record_get(uint32_t player, uint32_t slot, uint8_t *out601);
void slot_record_put(uint32_t player, uint32_t slot, const uint8_t *in601);
// map_SavePlanetToDisk writes the free-count block from the stock array: while raised it holds 500 - busy, which a build without the raise
// (retail, 0.2.1, an OFF build -- a save with nothing past the window is not stamped, so they load it) would trust. save_begin() puts
// 100 - busy(slots 0..99) there; save_end() restores the live counts (idempotent; also called at the next sim step in case the save aborted).
void save_begin();
void save_end();

// ---- U4e: libmh ----------------------------------------------------------------------------------------------------------
// -> the name of a promoted game function that touches the pool, or null.
const char *libmh_blocks();
void        set_promoted_probe_for_test(const char *(*fn)(uintptr_t));

// ---- the dev check -----------------------------------------------------------------------------------------------------
void set_logger(void (*fn)(const char *));
void set_check(bool on);
bool check_enabled();
// Called once per sim step (caps::note_sim_step). Runs the invariants when the check is on; returns the violations found by this call.
uint32_t step();
// The invariants once, now, regardless of the switch (capstest / harness). Violations are logged (first of each kind) and counted.
uint32_t check_now();
uint32_t violations_total();

// PT-CAP9-U7b: FNV-1a-64 of the live pool (waypoints + busy flag, player-major, slot-major). The save round-trip rows compare it before/after.
enum digest_part : unsigned { DIGEST_WINDOW = 0,
                              DIGEST_TAILS  = 1,
                              DIGEST_POOL   = 2 }; // slots 0..99 / 100..500 / all 501
uint64_t pool_digest(unsigned part);

struct census {
    uint32_t holding;    // units of the player holding a path (raised: id != 500; else byte != 0xff)
    uint32_t max_id;     // highest id held
    uint32_t flags_set;  // busy slots
    int32_t  free_count; // the game's per-player counter
};
census take_census(uint32_t player);

} // namespace mh::caps::path
