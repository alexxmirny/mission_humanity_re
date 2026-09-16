//
// tact/tact_calc_dir24.h -- TACT1A/B: the tactical dir24 heading-from-points query.
//
//   llm_tact_calc_dir24 @0x0042e0e2 (0xe7)
//
// Converts the delta between two tile positions into a 1-based 24-way facing index. The tactical
// SIBLING of llm_strat_facing24_from_points (RI-SIM's SIM1A, sim/sim_facing24_from_points.*)
// -- same trig shape, ported here with int (not char/byte) coordinates and its own constant pair. The
// early-out and the wrap/sector step differ in SHAPE from the sim sibling (see the .cpp): this
// function truncates to an INTEGER degree value FIRST, then does every subsequent wrap/sector step in
// plain int arithmetic, rather than staying in the double domain until one final trunc.
//
//   if (x1,y1) == (x2,y2): return 1                                   (0x0042e106-0x0042e11c)
//   dx = x1 - x2, dy = y2 - y1                                        (0x0042e121-0x0042e13c, via double)
//   angle_deg = (int)trunc(atan(dx / dy) * DEG_PER_RAD / PI_APPROX)   (0x0042e13f-0x0042e167)
//   if (dy < 0.0)             angle_deg += 180                        (0x0042e16a-0x0042e17b)
//   if (angle_deg >= 360)     angle_deg += -360                       (0x0042e17b-0x0042e18b)
//   if (angle_deg < 0)        angle_deg += 360                        (0x0042e18b-0x00431198 [sic 0x0042e198])
//   sector = angle_deg / 15 + 1                                       (0x0042e198-0x0042e1ac, IDIV)
//   if (sector > 24) sector = 1                                       (0x0042e1ac-0x0042e1b9)
//   return sector
//
// ---- THE TWO CONSTANTS: SAME GAME-TRUNCATED PI AS THE SIM SIBLING, VERIFIED INDEPENDENTLY --------
//
// 0x0050045a (DEG_PER_RAD, read as `double ptr [EDX+0x0050045a]` at 0x0042e156 -- Ghidra's own label
// is "s__00500452+8", the tail of an unrelated 10-byte string literal "MINE   " at 0x00500452 that a
// separate Ghidra data item happens to overlap by 2 bytes; queued as a non-blocking Ghidra finding,
// tools/data/ghidra_findings.json) = 180.0, bit-confirmed via ReVA read-memory
// (`00 00 00 00 00 80 66 40` LE).
//
// 0x00500462 ("M_PI" per Ghidra's own symbol) = 3.1415926536 EXACTLY (ReVA read-memory:
// `E0 86 44 54 FB 21 09 40` LE), NOT bit-identical to the standard double pi (3.14159265358979323846,
// differs by ~1.02e-11) -- the SAME truncated-pi constant sim_facing24_from_points.h already
// documents at its own address (0x00501504); this is a second, independent occurrence of the game's
// house pi literal, not the same datum re-read.
//
// ---- WHY THIS FILE DECLARES NO SHADOW SITE ---------------------------------------------------------
//
// Reads no tact_view member (only its four scalar arguments) and writes nothing -- 0 direct / 0
// transitive write cells, and non-void but with no game state behind the return value at all, unlike
// llm_tact_calc_approach_dir24_to_tile_stamp (tact_dir24_approach.h), which IS shadowed via return
// value specifically because it reads live tile/table state a shadow arm is worth re-checking. Same
// posture and same reasoning as sim_facing24_from_points.h's "WHY THIS FILE DECLARES NO SHADOW SITE":
// there is no state-layout interpretation to get wrong here, only arithmetic, which the offline
// oracle (tact_calc_dir24_selftest.cpp) covers exhaustively including the atan mock.
//
#pragma once
#include <cstdint>

namespace mh::tact {

// The one outward call, indirected for offline testability -- same shape as
// sim_facing24_from_points.h's facing24_from_points_calls.
struct calc_dir24_calls {
    double (*atan)(double x); // llm_math_atan @0x004daa52
};

const calc_dir24_calls &live_calc_dir24_calls();

namespace detail {

// llm_tact_calc_dir24 @0x0042e0e2. See the header banner for the full derivation.
int32_t calc_dir24(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const calc_dir24_calls &c);

} // namespace detail

int32_t calc_dir24(int32_t x1, int32_t y1, int32_t x2, int32_t y2);


} // namespace mh::tact
