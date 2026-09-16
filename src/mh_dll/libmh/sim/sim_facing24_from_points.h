//
// sim/sim_facing24_from_points.h -- llm_strat_facing24_from_points (RI-SIM / SIM1A).
//
// llm_strat_facing24_from_points @0x004946d2 (0xf0 bytes), `byte __watcall
// llm_strat_facing24_from_points(char from_x, char from_y, char to_x, char to_y)` per the committed
// prototype (sig_llm_strat_facing24_from_points in addr/mh_export.gen.h / addr/mh_calls.gen.h) --
// `char` IS signed here, confirmed by the .asm's own opening MOVSX loads (0x004946f3/0x004946f7/
// 0x00494706/0x0049470a), not left ambiguous.
//
// Converts the delta between two byte tile coordinates into a 1-based 24-way facing index:
//
//   dx = (int)from_x - (int)to_x
//   dy = (int)to_y   - (int)from_y
//   angle_deg = atan(dx / dy) * RAD2DEG_NUM / RAD2DEG_DEN         (radians -> degrees)
//   if (dy < 0.0)            angle_deg += HALF_TURN_DEG           (atan's [-90,90] half-turn fixup)
//   angle_deg += BIAS_DEG                                         (unconditional pre-quantization bias)
//   if (angle_deg < 0.0)     angle_deg += WRAP_ADD_DEG             (wrap up into range)
//   if (angle_deg >= WRAP_LIMIT_DEG) angle_deg += WRAP_SUB_DEG     (wrap back down; WRAP_SUB_DEG < 0)
//   return (byte)trunc(angle_deg / SECTOR_DEG + 1.0)               (1..24)
//
// Non-wrapping (non-toroidal) sibling of llm_strat_dir_sector_to, per the existing Ghidra plate; the
// caller stores the result into llm_strat_crew_soldier.sprite_frame as the soldier's heading (also
// per the existing plate -- not re-derived by this translation, cited as corroboration).
//
// ---- THE EIGHT CONSTANTS: LINK-TIME CONSTANT-POOL DOUBLES, NOT CFG-LOADED STATE -------------------
//
// _G_LLM_STRAT_FACING24_{RAD2DEG_NUM,RAD2DEG_DEN,HALF_TURN_DEG,BIAS_DEG,WRAP_ADD_DEG,WRAP_LIMIT_DEG,
// WRAP_SUB_DEG,SECTOR_DEG} (0x005014fc.. 0x00501534, one 8-byte double each, contiguous) are read
// ONLY as FADD/FMUL/FDIV/FCOMP memory operands throughout this function's body and the identical
// 128-way sibling block at 0x0050157c (llm_strat_dir_sector_to) -- no write site to either block
// anywhere in the image (per the 2026-08-07 derivation, which named and commented both blocks
// after independently confirming this). That is the signature of a compiler-emitted literal constant
// pool, not a cfg-loaded runtime global: cfg tables are loaded once at boot into a table sized by the
// cfg grammar and indexed by an id, whereas this is eight bare doubles at a fixed link address with
// no index, no loader call reaching them, and no cfg keyword named in docs/structs.md that points
// here. So these become plain `constexpr double` below (matching sim_order_dispatch_bldg.cpp's
// TEXT_ID_*/SND_* and sim_unit_refund.cpp's ENERGY_RATIO_SCALE convention for an image-constant that
// is not part of the state view), NOT a sim_view member -- there is no accessor to add.
//
// VALUES, per the already-APPLIED annotation (tools/applied/2026-08-07-204018-annotations.json,
// itself the corrected answer after that same session's own near-miss of misreading this exact block
// as all-zero via a JPype byte-copy bug):
//   RAD2DEG_NUM=180.0  RAD2DEG_DEN=3.1415926536 (truncated pi, NOT std pi -- see below)  HALF_TURN_DEG=180.0  BIAS_DEG=7.0  WRAP_ADD_DEG=360.0
//   WRAP_LIMIT_DEG=360.0  WRAP_SUB_DEG=-360.0  SECTOR_DEG=15.0
// SECTOR_DEG really is 15.0 (checked against the raw pool per the batch context's own warning not to
// assume it) -- 360/15 = 24 sectors, matching the function's name. BIAS_DEG is 7.0, NOT half of 15 --
// the applied comment says so explicitly ("NOT exactly half a sector"), so it is transcribed as 7.0,
// not "improved" to 7.5.
//
// CONDUCTOR-CONFIRMED (2026-08-11), and the translator's caution was justified: ReVA read-memory at
// 0x00501504 returned bytes `E0 86 44 54 FB 21 09 40` (LE) = 3.1415926536 EXACTLY, a truncated
// 11-significant-digit approximation of pi -- NOT bit-identical to the standard double pi
// (3.14159265358979323846), differing by ~1.02e-11. The applied Ghidra comment's "PI" label is
// approximately right but not precise; this is the SAME class of trap the ledger already
// carries a row for (DAT_00504813's 2*pi, from the same 2026-08-07 session, also not bit-identical to
// std 2*pi) -- two of the handful of named doubles in this constant pool block, and both are
// game-truncated rather than library-exact. RAD2DEG_DEN is now the literal `3.1415926536` (verified
// bit-identical to the raw bytes when parsed as a double), not `M_PI` or a hand-typed 3.14159265358979323846.
//
// ---- utils_math_trunc: INLINED, NOT CALLED THROUGH mh_calls.gen.h -------------------------------
//
// utils_math_trunc @0x004d0596 is `MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h (an x87-register-only leaf, ST0 in / ST0 out, no stack-passable signature -- the
// .asm's own CALL at 0x0049479e has no preceding SUB ESP/FSTP [ESP] push, unlike the CALL to
// llm_math_atan five lines above it, which does). Reproduced inline in the .cpp as the same
// FSTCW/RC=11-swap/FRNDINT/FLDCW-restore/FISTP sequence every other translated site that reaches this
// callee already inlines (sim_unit_refund.cpp's refund_amount, sim_unit_update_soldiers.cpp's
// trunc_axis_delta, ai_mine_yield.cpp's x87_scale_and_trunc, ai_group_muster_pick.cpp's
// weapon_power_add_and_trunc, several more in ai/) -- see the .cpp for the exact instruction sequence
// and address citations. Distinct from those precedents in ONE way worth flagging: here the value fed
// to the inlined trunc (angle_deg/SECTOR_DEG + 1.0) is produced ENTIRELY IN THE X87 REGISTER with NO
// intermediate 64-bit memory store between the division, the +1, and the trunc (0x00494791-0x0049479e
// has no FSTP between FADDP and CALL) -- unlike the upstream wrap-chain steps, which each round-trip
// through the SAME 64-bit double slot and are therefore safe to write as separate plain-C++ `double`
// statements (see the .cpp). This final step is reproduced as one hand-written __asm block instead,
// so the extended-precision division-then-add is never forced through a 64-bit store before trunc.
//
// ---- llm_math_atan: CALLED THROUGH AN OUTWARD-CALLS STRUCT, NOT DIRECTLY ---------------------------
//
// llm_math_atan @0x004daa52 IS marshallable (`double llm_math_atan(double x)` in mh_calls.gen.h), but
// this function still takes it as an indirected member (facing24_from_points_calls::atan) rather than
// calling mh::call::llm_math_atan directly from detail::, for the same reason every other translated
// site indirects a marshallable outward call: `net_selftest.exe simtest` is a standalone process ("No
// game needed", per its own usage banner) and mh::call::* thunks jump to a fixed VA that is only valid
// inside the real game image -- calling one directly from detail:: would make the body untestable
// offline. This is, as far as this translation could find, the FIRST sim function whose one outward
// call is a marshallable pure-math leaf rather than a shared game helper with a state effect; the
// shape (a one-member _calls struct, no sim_view/sim_store at all) is new for that reason, not a
// deviation from precedent.
//
// ---- WHY THIS FILE DECLARES NO SHADOW SITE ---------------------------------------------------------
//
// Per the batch context: this function's write set is empty -- it reads no sim_view member (no
// global game state at all, only its four scalar arguments) and writes nothing, only returning a
// value through EAX like any ordinary function. The batch context's own precedent is
// mh::orders::integrity_check (SIM1C), documented as "not shadowable" for the identical reason and
// verified via `simtest` alone. Unlike sim_unit_type_predicates.cpp's five pure predicates (also
// writeless, but STILL shadowed), this function reads no sim_view member either -- a predicate's
// shadow arm is worth arming because it re-checks the reimplementation's STATE-LAYOUT interpretation
// against the live game's return value; there is no state layout here to get wrong, only arithmetic,
// which a scalar-in/scalar-out unit test already covers exhaustively. `detail::` and a thin public
// wrapper are still written below, and the public wrapper is still bound to the REAL llm_math_atan
// (live_facing24_from_points_calls()), so the function is usable by any future C++ caller even though
// nothing hooks the game's own call site.
//
#pragma once
#include <cstdint>

namespace mh::sim {

// ---- the one outward call --------------------------------------------------------------------
//
// See the header banner above on why this is indirected even though llm_math_atan is marshallable.
struct facing24_from_points_calls {
    double (*atan)(double x); // llm_math_atan @0x004daa52
};

const facing24_from_points_calls &live_facing24_from_points_calls();

// The logic over an explicit calls table, matching every other sim TU's split -- here there is no
// sim_view/sim_store parameter at all, because this function reads no game state (see header).
namespace detail {

// llm_strat_facing24_from_points @0x004946d2. See the header banner for the full derivation.
uint8_t facing24_from_points(char from_x, char from_y, char to_x, char to_y,
                             const facing24_from_points_calls &c);

} // namespace detail

// Live wrapper: the logic applied to live_facing24_from_points_calls(). Matches the committed
// __watcall(AL,DL,BL,CL) shape (sig_llm_strat_facing24_from_points).
uint8_t facing24_from_points(char from_x, char from_y, char to_x, char to_y);


} // namespace mh::sim
