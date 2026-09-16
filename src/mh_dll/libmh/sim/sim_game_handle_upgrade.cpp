//
// sim/sim_game_handle_upgrade.cpp -- see sim_game_handle_upgrade.h. Translated from the DISASSEMBLY
// (tmp/decomp/game_HandleUpgrade_0044062a.asm), NOT from Ghidra's C draft
// (tmp/decomp/game_HandleUpgrade_0044062a.c) -- the draft's field math cross-checks clean against the
// raw bytes, but it is silent or misleading on several points this file states explicitly:
//
//   1. TYPE DISPATCH. The draft renders `if (cVar1 != UNIT) { if (cVar1 < 2) {...} else if
//      (cVar1==2) {...} }`, which reads as "type<2 is the unit arm" only because UNIT==0 is also
//      excluded by the outer guard -- and that is exactly how the batch context doc's prose
//      summarises it too. The RAW assembly (0x00440654-0x0044066a) is three separate unsigned
//      compares: `CMP type,1; JC <return>` (type==0 -> immediate return, does NOTHING -- no unit_mut
//      write, no message), `CMP type,1; JBE <unit arm>` (type==1 -> unit arm), `CMP type,2; JZ
//      <weapon arm>` (type==2 -> weapon arm), else return. So despite cfg_enum_E_UPGRADE_TYPE's UNIT
//      member being 0, ONLY type==1 ever runs the unit arm; type==0 is a silent no-op. Reproduced
//      exactly below (apply_upgrade's `if (type == 1) ... if (type == 2) ...`, no `type < 2`) -- this
//      is the asm's own behaviour, not a bug this translation introduces or "fixes". Flagged in the
//      structured report for review since it visibly contradicts the batch context doc's shorthand.
//
//   2. SEVEN VALUES THIS FILE NEEDS THAT ARE NOT YET IN THE VIEW/DTM. Used below AS IF sim_view /
//      sim_state.h already carried them (same posture sim_population_change.cpp takes with its not-
//      yet-generated shadow site) -- see the structured report's declared_needs for the full list:
//        - v.upgrade_unit_speed_pct_divisor: DOUBLE_00500a70, already named
//          _G_LLM_STRAT_UPGRADE_SPEED_PERCENT_DIVISOR in addr/mh_addrs.gen.h this same slice
//          (read-memory-confirmed 100.0 per that entry's own comment), just not yet wired into
//          sim_view as a struct member.
//        - v.upgrade_weapon_pct_divisor: DOUBLE_00500a96. Ghidra's OWN decompiler prints this as
//          `const::_100f` in the draft, i.e. it already infers 100.0, but that is Ghidra's inference
//          surfacing through the draft, not a read-memory confirmation performed by this translation
//          -- the address has NO Ghidra symbol at all yet. Distinct storage from divisor #1 above per
//          the batch context doc's explicit instruction not to merge the two.
//        - v.upgrade_msg_sep_before_category / _sep_before_name / _clause_sep_first /
//          _clause_sep_next / _trailer: five DAT_00500a78/0x7e/0x84/0x8c/0x92 wide-string-literal
//          POINTERS this function concatenates directly (`MOV EDX,<addr>` then utils_concat -- an
//          immediate address, not a dereference through a variable, so the constant IS the string's
//          address). NOT indices into G_TEXT_PTRS. This translation has no memory-read access, so
//          only each one's ROLE is known (derived from control flow, see the per-site comments
//          below and the structured report) -- not its actual characters. Proposed names are
//          provisional; the conductor should rename them once the bytes are read.
//        - cfg_upgrade_object_id(): Upgrades[].objects is `uint8_t objects[64]` in the generated
//          header (the applied cfg_t_upgrade_object_index[16] sub-type does not survive flattening,
//          the same situation sim_state.h's cfg_unit_weapon_id()/cfg_unit_weapon_enabled() document
//          for cfg_unit::weapons) -- but each entry here is a 4-byte dword (`SHL EAX,0x2` stride in
//          the asm, at 0x0044069c and 0x0044092a), not a byte pair, so cfg_unit_weapon_id's exact
//          pattern does not transfer; a proper dword accessor is needed instead of a reinterpret_cast
//          in this body. Called below as if declared alongside cfg_unit_weapon_id in sim_state.h.
//
#include "sim/sim_game_handle_upgrade.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const upgrade_calls &live_upgrade_calls() {
    static const upgrade_calls c = {
        MH_CRT(utils_w_str_copy),
        MH_CRT(utils_concat),
        mh::state::evt::text_print_u32,
    };
    return c;
}

namespace {

// PlayerSide is a 16-bit field the original MOVZX-widens before comparing it against `player` --
// same idiom sim_order_dispatch_bldg.cpp's own is_local_player() documents, at this function's two
// analogous sites (0x004407d1, 0x00440aeb).
inline bool is_local_player(const sim_view &v, uint32_t player) {
    return (uint32_t)(uint16_t)*v.player_side == player;
}

// G_TEXT_PTRS indices this function reads as fixed literals (not per-object data) -- named the same
// way sim_order_dispatch_bldg.cpp names TEXT_ID_INSUFFICIENT_ENERGY. Values read off the raw
// [G_TEXT_PTRS + id*4] address arithmetic at each site (e.g. 0x005845f0 = G_TEXT_PTRS(0x58440c) +
// 0x79*4), not off the draft's `[0x79]`-style indices, though they agree.
inline constexpr int32_t TEXT_ID_MSG_PREFIX              = 0x79; // 121: shared "<prefix>" header (both arms)
inline constexpr int32_t TEXT_ID_UNIT_CATEGORY           = 0x18; // 24: unit arm's category label
inline constexpr int32_t TEXT_ID_WEAPON_CATEGORY         = 0x1a; // 26: weapon arm's category label
inline constexpr int32_t TEXT_ID_UNIT_SPEED_DELTA        = 0x8e; // 142: unit arm's speed-changed clause
inline constexpr int32_t TEXT_ID_UNIT_ARMOR_DELTA        = 0x8f; // 143: unit arm's armor-changed clause
inline constexpr int32_t TEXT_ID_WEAPON_RANGE_DELTA      = 0x90; // 144
inline constexpr int32_t TEXT_ID_WEAPON_MISSING_DELTA    = 0x91; // 145
inline constexpr int32_t TEXT_ID_WEAPON_POWER_DELTA      = 0x92; // 146
inline constexpr int32_t TEXT_ID_WEAPON_FIRE_RANGE_DELTA = 0x93; // 147

inline constexpr int32_t UPGRADE_OBJECT_SLOTS = 16; // Upgrades[].objects[16], both arms loop 0..15

// The shared "<prefix>: <category>: <name>" message opener (0x004407e1-0x0044083e for the unit arm,
// 0x00440afb-0x00440b58 for the weapon arm -- byte-identical shape, different text ids): copy the
// prefix into text_scratch, then concat sep/category/sep/name.
inline void begin_upgrade_message(const sim_view &v, sim_store &own, const upgrade_calls &c,
                                  int32_t category_text_id, int32_t name_text_id) {
    c.utils_w_str_copy((void *)v.text_ptrs[TEXT_ID_MSG_PREFIX], own.text_scratch());
    c.utils_concat(own.text_scratch(), (void *)v.upgrade_msg_sep_before_category);
    c.utils_concat(own.text_scratch(), (void *)v.text_ptrs[category_text_id]);
    c.utils_concat(own.text_scratch(), (void *)v.upgrade_msg_sep_before_name);
    c.utils_concat(own.text_scratch(), (void *)v.text_ptrs[name_text_id]);
}

// One "<sep><clause text>" append. `has_prior_clause` starts false (the original's `bVar3`) and is
// set true after the first append; the separator is DAT_00500a84 on the FIRST clause appended and
// DAT_00500a8c on every clause after (0x00440858-0x0044088e and its three weapon-arm siblings all
// share this exact three-line if/else-then-concat shape).
inline void append_delta_clause(const sim_view &v, sim_store &own, const upgrade_calls &c,
                                bool &has_prior_clause, int32_t text_id) {
    c.utils_concat(own.text_scratch(), (void *)(has_prior_clause ? v.upgrade_msg_clause_sep_next
                                                                 : v.upgrade_msg_clause_sep_first));
    c.utils_concat(own.text_scratch(), (void *)v.text_ptrs[text_id]);
    has_prior_clause = true;
}

// The trailing DAT_00500a92 + PrintTextMessage every arm ends with (0x004408df-0x004408f3 for the
// unit arm, 0x00440c8e-0x00440ca2 for the weapon arm).
inline void finish_upgrade_message(const sim_view &v, sim_store &own, const upgrade_calls &c) {
    c.utils_concat(own.text_scratch(), (void *)v.upgrade_msg_trailer);
    c.game_ui_PrintTextMessage(own.text_scratch());
}

} // namespace

namespace detail {

void apply_upgrade(const sim_view &v, sim_store &own, const upgrade_calls &c, uint32_t player,
                   int32_t upgrade_id) {
    const cfg_upgrade &up = v.cfg_upgrades[upgrade_id];

    // 0x00440647-0x0044066a: dispatch on Upgrades[upgrade_id].type. See the file banner point 1 --
    // type==0 returns without doing anything; only type==1 runs the "unit" arm below.
    const uint32_t type = up.type;

    if (type == 1) {
        for (int32_t i = 0; i < UPGRADE_OBJECT_SLOTS; ++i) {
            // DECLARED NEED (see banner point 2): cfg_upgrade_object_id() does not exist yet.
            const uint32_t obj_id = cfg_upgrade_object_id(up, i);
            if (obj_id == 0) continue; // 0x004406aa: skip an unused object slot

            cfg_unit &u                = own.unit_mut(obj_id);
            bool      has_prior_clause = false;

            // 0x004406d2-0x004406ee: new = old - (old * Upgrades[id].step_speed) / DIVISOR. Order
            // matters for x87 bit-exactness: multiply, then divide, then subtract from the ORIGINAL
            // value (re-read, not a not-yet-updated new value) -- the original never caches
            // old_step_speed across the FSUBR, and neither does this (u.step_speed[player] is read
            // twice below, same as the asm's two independent [EAX+..]/[EBX+..] address computations
            // both landing on the same cell).
            u.step_speed[player] -=
                (u.step_speed[player] * up.step_speed) / *v.upgrade_unit_speed_pct_divisor;
            // 0x00440712-0x0044072e: same shape, same Upgrades[id].step_speed SOURCE field -- there
            // is no separate cfg "turn_speed delta" field; both unit fields share this one (verified:
            // both FMULs read the identical Upgrades offset 0xbcf91c).
            u.turn_speed[player] -=
                (u.turn_speed[player] * up.step_speed) / *v.upgrade_unit_speed_pct_divisor;
            // 0x00440743-0x0044074d: flat int subtract, no clamp. ENERGY here is the upgrade's armor
            // delta (an HP-like stat), not the power-generation resource -- see
            // ENERGY/POWER note.
            u.armor_prob[player] -= up.energy;

            // 0x00440762-0x0044078e / 0x004407a1-0x004407c7: `FLDZ; FCOMP val; SAHF; JC skip` clamps
            // `val` to 0.0 when val<=0.0 (ordered) and leaves it alone when val>0.0 OR val is NaN.
            // Plain `val <= 0.0` reproduces this bit-for-bit (IEEE `<=` is also false on NaN) --
            // unlike the INVERTED-polarity `energy_gate()` idiom sim_order_dispatch_bldg.cpp needs a
            // named helper for, this polarity happens to match naive C. FP comparison, see
            // uncertainties[].
            if (u.step_speed[player] <= 0.0) u.step_speed[player] = 0.0;
            if (u.turn_speed[player] <= 0.0) u.turn_speed[player] = 0.0;

            // 0x004407d1-0x004407db: only the LOCAL player sees the summary toast.
            if (is_local_player(v, player)) {
                begin_upgrade_message(v, own, c, TEXT_ID_UNIT_CATEGORY, u.name);

                // 0x0044083f-0x00440856: `(bits(step_speed) & 0x7fffffff00000000) != 0 ||
                // low32(step_speed) != 0` -- a sign-masked nonzero-double test done in integer bits
                // (avoids an FP compare entirely). Plain `up.step_speed != 0.0` is bit-identical here
                // (both are false only for +/-0.0, both are true for any NaN). FP comparison, see
                // uncertainties[].
                if (up.step_speed != 0.0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_UNIT_SPEED_DELTA);
                }
                if (up.energy != 0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_UNIT_ARMOR_DELTA);
                }
                finish_upgrade_message(v, own, c);
            }
        }
        return;
    }

    if (type == 2) {
        for (int32_t i = 0; i < UPGRADE_OBJECT_SLOTS; ++i) {
            const uint32_t obj_id = cfg_upgrade_object_id(up, i);
            if (obj_id == 0) continue; // 0x0044093c: skip an unused object slot

            cfg_weapon &w                = own.weapon_mut(obj_id);
            bool        has_prior_clause = false;

            // 0x00440964-0x0044097f: integer IMUL then IDIV, truncating toward zero exactly like
            // C's `/` -- a real hardware divide, not the shift-form division rule 8 warns about.
            w.range_min[player] -= (w.range_min[player] * up.range_min) / 100;
            w.range_max[player] += (w.range_max[player] * up.range_max) / 100;
            w.missing[player] -= (w.missing[player] * up.missing) / 100;
            // 0x00440a2b-0x00440a83: FILD-converted int cfg percent * old DOUBLE weapon value /
            // DIVISOR, ADDED to the old value -- power/fire_range are `double[9]` fields (see
            // mh_structs.gen.h), not int32 like range_min/range_max/missing above, hence the
            // different arithmetic shape and the *8 (not *4) player stride in the asm. FP
            // expression, see uncertainties[].
            w.power[player] =
                (up.power * w.power[player]) / *v.upgrade_weapon_pct_divisor + w.power[player];
            w.fire_range[player] =
                (up.fire_range * w.fire_range[player]) / *v.upgrade_weapon_pct_divisor +
                w.fire_range[player];

            // 0x00440a98-0x00440ae1: plain SIGNED INT compare (not FP) -- clamp to 0 when <=0.
            // range_max/power/fire_range get NO clamp of any kind (verified: no matching CMP/MOV
            // pair follows either of them in the assembly, unlike range_min/missing here).
            if (w.range_min[player] <= 0) w.range_min[player] = 0;
            if (w.missing[player] <= 0) w.missing[player] = 0;

            if (is_local_player(v, player)) {
                begin_upgrade_message(v, own, c, TEXT_ID_WEAPON_CATEGORY, w.name);

                // 0x00440b59-0x00440b73: a combined range_min/range_max clause -- fires if EITHER
                // cfg delta is nonzero (short-circuit OR, range_min checked first, matching the
                // asm's own JNZ-then-fallback order).
                if (up.range_min != 0 || up.range_max != 0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_WEAPON_RANGE_DELTA);
                }
                if (up.missing != 0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_WEAPON_MISSING_DELTA);
                }
                if (up.power != 0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_WEAPON_POWER_DELTA);
                }
                if (up.fire_range != 0) {
                    append_delta_clause(v, own, c, has_prior_clause, TEXT_ID_WEAPON_FIRE_RANGE_DELTA);
                }
                finish_upgrade_message(v, own, c);
            }
        }
        return;
    }

    // type == 0 or type > 2 (0x0044066a / 0x0044066f): the original does nothing at all -- see the
    // file banner point 1.
}

} // namespace detail

void handle_upgrade(uint32_t player, int32_t upgrade_id) {
    sim_state st = state();
    detail::apply_upgrade(st.read, st.own, live_upgrade_calls(), player, upgrade_id);
}


} // namespace mh::sim
