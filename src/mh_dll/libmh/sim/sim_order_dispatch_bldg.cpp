#include "sim/sim_order_dispatch.h"

// NO direct mh::call:: in this file. w_sprintf's measured `vss` shape used to be called directly
// here on the argument that a recording stub cannot stand in for a shape thunk; it can, and the
// direct call made the six arms that print a message unreachable by `simtest` (the thunk jumps to a
// game image address, unmapped in net_selftest.exe). It is a `dispatch_calls` member now -- see the
// note there. Its output goes to own.text_scratch(), which reaches neither the determinism hash nor
// a save.

namespace mh::sim {
namespace {

// ---- x87 comparison idioms, spelled out because C's operators are not all faithful to them ------
//
// FCOM/FCOMP set C3/C2/C0 as {equal: 100, less: 001, greater: 000, UNORDERED: 111}, and SAHF moves
// C0->CF, C2->PF, C3->ZF. So every branch below has a defined behaviour on NaN that the "obvious"
// C expression does not necessarily share. These three cover every FP branch in the range.

// `FLDZ; FCOMP energy; FNSTSW AX; SAHF; JNC <skip>` -- 28 sites. CF is set when 0 < energy AND when
// the compare is unordered, so the body runs on a NaN energy. `!(energy <= 0.0)` has exactly that
// truth table; `0.0 < energy` differs on precisely the NaN case and is what the decompile shows.
inline bool energy_gate(double energy) { return !(energy <= 0.0); }

// `FLD a; FCOMP b; SAHF; JNZ <skip>` -- index 12's "is this building at full charge" test. ZF is C3,
// which is set for EQUAL *and* for UNORDERED, so the arm proceeds on NaN. `a == b` would drop that
// case; `!(a != b)` would invert it. Two strict comparisons are what actually reproduce ZF.
inline bool fcom_eq_or_unordered(double a, double b) { return !(a < b) && !(a > b); }

// `FLD a; FCOMP b; SAHF; JC <then>` (equivalently `JNC <else>`) -- indices 15 and 16/17's "below its
// cfg energy" test. CF is C0, set for a < b AND for unordered.
inline bool fcom_below_or_unordered(double a, double b) { return !(a >= b); }

// ---- cfg_enum_E_BUILDING members this table compares Building[].type against --------------------
// The generated header renders an enum field as its underlying scalar (see sim_state.h's note on
// the same limitation for cfg_unit::weapons), so the literals are named here. Values read off the
// CMPs in this range; the A_/H_ pairing is the +0x14 race offset the strategic-sim notes records, and
// A_MOTHER/H_MOTHER agree with ai/ai_state.h's BLDG_TYPE_A_MOTHER/_H_MOTHER (6 / 26).
inline constexpr uint8_t BLDG_TYPE_A_MOTHER   = 0x06;
inline constexpr uint8_t BLDG_TYPE_A_BARRACKS = 0x07;
inline constexpr uint8_t BLDG_TYPE_A_GARAGE   = 0x08;
inline constexpr uint8_t BLDG_TYPE_A_AIRFIELD = 0x09;
inline constexpr uint8_t BLDG_TYPE_A_HELIPAD  = 0x0a;
inline constexpr uint8_t BLDG_TYPE_A_PORT     = 0x0c;
inline constexpr uint8_t BLDG_TYPE_A_SHUTTLE  = 0x0d;
inline constexpr uint8_t BLDG_TYPE_H_MOTHER   = 0x1a;
inline constexpr uint8_t BLDG_TYPE_H_BARRACKS = 0x1b;
inline constexpr uint8_t BLDG_TYPE_H_GARAGE   = 0x1c;
inline constexpr uint8_t BLDG_TYPE_H_AIRFIELD = 0x1d;
inline constexpr uint8_t BLDG_TYPE_H_HELIPAD  = 0x1e;
inline constexpr uint8_t BLDG_TYPE_H_PORT     = 0x20;
inline constexpr uint8_t BLDG_TYPE_H_SHUTTLE  = 0x21;

// The four-way "is this a transfer terminal" test the shuttle/cargo arms (16/17, 18, 19, 20, 21, 22,
// 23) each spell out inline, always these four types in this order. Transcribed once because it is
// literally the same test seven times, not because the arms were consolidated -- each arm still
// carries its own gates.
inline bool is_port_or_mother(uint8_t t) {
    return t == BLDG_TYPE_A_PORT || t == BLDG_TYPE_H_PORT || t == BLDG_TYPE_A_MOTHER ||
           t == BLDG_TYPE_H_MOTHER;
}

// ---- llm_strat_bldg_state members this table reads or writes ------------------------------------
// Same reason as the building types: the field is a bare uint16_t in the generated header. Names are
// Ghidra's own enum members as they appear in the decompile.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION        = 0x64;
inline constexpr uint16_t BLDG_STATE_CHARGE_GATE         = 0x69;
inline constexpr uint16_t BLDG_STATE_CHARGE_STEP         = 0x6a;
inline constexpr uint16_t BLDG_STATE_DISMANTLING         = 0x6b;
inline constexpr uint16_t BLDG_STATE_PROD_PICK_NEXT      = 0x6c;
inline constexpr uint16_t BLDG_STATE_PROD_WORKING        = 0x6d;
inline constexpr uint16_t BLDG_STATE_PROD_BLOCKED_NOTIFY = 0x6e;
inline constexpr uint16_t BLDG_STATE_MINE_SCAN_DEPOSITS  = 0x72;
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP           = 0x77;
inline constexpr uint16_t BLDG_STATE_HANGAR_RECHARGE_CHK = 0x78;
inline constexpr uint16_t BLDG_STATE_TURRET_ATTACK       = 0x7b;
inline constexpr uint16_t BLDG_STATE_UPGRADING           = 0x82;
inline constexpr uint16_t BLDG_STATE_RESEARCHING         = 0x89;

// _G_LLM_GAME_SESSION_MODE == 1 is single-player; 3 is MP lockstep (sim_state.h). Every transfer arm
// gates on == 1 exactly, never on "!= 3".
inline constexpr int32_t SESSION_SP = 1;

// The production queue's per-building cap, and its slot-0 running total. queued_count[0] is the SUM
// row (every add/remove touches both it and the per-unit-type slot), which is why the cap is tested
// against slot 0 and not against the slot being added to.
inline constexpr int32_t PRODUCTION_QUEUE_CAP = 0x32;

// G_TEXT_PTRS index 136 -- the "not enough energy / not ready" reason string the two mother/shuttle
// arms print instead of a callee's own status code. 0x0058462c = G_TEXT_PTRS + 136*4.
inline constexpr int32_t TEXT_ID_INSUFFICIENT_ENERGY = 136;

// llm_snd_play(0xb3, 100) -- the construction-refused blip, index 25's only sound.
inline constexpr int32_t SND_BUILD_REFUSED   = 0xb3;
inline constexpr int32_t SND_BUILD_REFUSED_V = 100;

// The format at 0x005011be. A wide "%s: %s"; supplied as our own literal rather than read out of the
// image, which is equivalent for a format string and keeps a literal VA out of this TU.
constexpr const wchar_t *TEXT_FMT_NAME_REASON = L"%s: %s";

// PlayerSide is a 16-bit field the original MOVZX-widens before comparing it against the order's
// low-nibble owner. Named here so the widening appears once instead of at six sites.
inline bool is_local_player(const sim_view &v, uint32_t player) {
    return (uint32_t)(uint16_t)*v.player_side == player;
}

// The six "<building name>: <reason>" sites (0x0046814f, 0x00468431, 0x004684b7, 0x00468ad4,
// 0x00468d5c, 0x00468fb2). Byte-identical apart from which text id supplies the reason: three pass a
// callee's nonzero status code, two pass TEXT_ID_INSUFFICIENT_ENERGY. The PlayerSide guard is NOT
// folded in here -- it sits at different places in the different arms' branch trees.
inline void print_building_message(const sim_view &v, sim_store &own, const dispatch_calls &c,
                                   uint16_t building_id, int32_t reason_text_id) {
    c.w_sprintf__vss(own.text_scratch(), TEXT_FMT_NAME_REASON,
                     v.text_ptrs[v.cfg_buildings[building_id].id], v.text_ptrs[reason_text_id]);
    c.game_ui_PrintTextMessage(own.text_scratch());
}

// `current_workers` is a uint16_t compared SIGNED against a 32-bit builder_count (MOVZX then
// CMP/JLE), and the surplus handed to unassign is the same widened difference.
inline int32_t workers_of(const building &b) { return (int32_t)(uint32_t)b.current_workers; }

// `EAX = human; EDX = human >> 31; EAX -= EDX; EAX >>= 1` -- signed divide by two, truncating toward
// zero. Appears at all three re-staffing sites (0x00468eb6, 0x00468c82, 0x00469431).
inline int32_t half_idle_population(int32_t human) { return human / 2; }

} // namespace

namespace detail {

void dispatch_building_order(const sim_view &v, sim_store &own, const dispatch_calls &c,
                             const dispatch_ctx &x) {
    // The record and the building are both re-read from their arrays at every access in the
    // original; a reference to ONE record is what sim_store's accessor hands out, so holding it is
    // the same thing. No FIELD is cached -- callees below write buildings[] and the order queue.
    order    &o = own.order_at(x.slot);
    building &b = own.building_at(x.player, (int32_t)x.object_index);

    // Watcom's sparse switch: `SUB EAX,0x6a / CMP word,0x80 / JA case_0 / REPNE SCASB / JMP tbl`.
    // decode_building_order reproduces the 16-bit guard, the 8-bit scan and the fact that a
    // no-match and a match on the LAST table entry both leave ECX == 0 -- see sim_order_dispatch.h.
    switch (decode_building_order((uint16_t)o.param0)) {

        // ---- index 1 (param0 0x6a) @0x00468d78: pay the cycle's inputs and enter the charge cycle ----
        case bldg_arm::PAY_CYCLE_UPKEEP: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            const int32_t rc = c.llm_strat_bldg_pay_cycle_inputs(x.player, (uint32_t)x.object_index);
            if (rc != 0) {
                // Paying failed; rc is the reason's text id.
                if (is_local_player(v, x.player)) print_building_message(v, own, c, b.building_id, rc);
                return;
            }
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            b.state          = BLDG_STATE_CHARGE_STEP;
            b.cycle_progress = 0.0; // two zero dwords in the original; 0.0 is bit-identical

            if (workers_of(b) > v.cfg_buildings[b.building_id].builder_count) {
                // PRESERVED DEFECT (0x00468e68, registered as `preserve_bug` in
                // tools/data/migration/sim.json). The GUARD three instructions earlier indexes
                // Building[] by the building's own building_id -- correctly -- but the AMOUNT is
                // `IMUL EAX,[EBP-0x38],0x842`, and [EBP-0x38] is the order's KIND NIBBLE, not a
                // building id. On this path the kind is always 0x40 (the router reaches here only for
                // kind == 0x40 exactly), so the surplus is always computed against Building[64]'s
                // builder_count. Reproduced verbatim; do not "fix" it -- the sibling sites at
                // 0x00468c4a (index 12) and 0x004693b4 (index 2) show what the correct index looks
                // like, and silently correcting this one desyncs against the original.
                c.llm_strat_bldg_unassign_workers(
                    (uint16_t)x.player, (uint32_t)x.object_index,
                    (uint32_t)(workers_of(b) - v.cfg_buildings[x.kind].builder_count));
            }
            if (v.population[x.player].human != 0 &&
                v.cfg_buildings[b.building_id].builder_count != 0) {
                c.llm_strat_bldg_assign_workers(x.player, (uint32_t)x.object_index,
                                                half_idle_population(v.population[x.player].human));
            }
            if (v.cfg_buildings[b.building_id].builder_count == 0 || b.current_workers != 0) {
                c.llm_strat_bldg_set_staffed_flag((uint16_t)x.player, x.object_index);
            } else {
                c.llm_strat_bldg_clear_staffed_flag((uint16_t)x.player, (uint32_t)x.object_index);
            }
            c.llm_strat_refresh_building((uint16_t)x.player, x.object_index);
            b.state = BLDG_STATE_CHARGE_GATE;
            return;
        }

        // ---- index 2 (param0 0x6b) @0x0046906a: start dismantling ------------------------------------
        case bldg_arm::RESTART_CONSTRUCT: {
            if (!energy_gate(b.energy) || b.state == BLDG_STATE_DISMANTLING) return;
            // The original materialises this as an explicit 0/1 in [EBP-0x64] and then tests it, which
            // is the decompile's `bVar3`. A mother is never dismantled.
            {
                const uint8_t t = v.cfg_buildings[b.building_id].type;
                if (t == BLDG_TYPE_H_MOTHER || t == BLDG_TYPE_A_MOTHER) return;
            }

            if (b.state != BLDG_STATE_CONSTRUCTION) {
                // Twelve types in this exact order (0x0046913a-0x00469304). Every one of them holds
                // units, so its garrison is scrapped before the building comes down. No call happens
                // between the twelve comparisons, so the single read of building_id is exact.
                const uint8_t t = v.cfg_buildings[b.building_id].type;
                if (t == BLDG_TYPE_H_GARAGE || t == BLDG_TYPE_H_AIRFIELD || t == BLDG_TYPE_H_HELIPAD ||
                    t == BLDG_TYPE_H_BARRACKS || t == BLDG_TYPE_H_PORT || t == BLDG_TYPE_H_SHUTTLE ||
                    t == BLDG_TYPE_A_GARAGE || t == BLDG_TYPE_A_AIRFIELD || t == BLDG_TYPE_A_HELIPAD ||
                    t == BLDG_TYPE_A_BARRACKS || t == BLDG_TYPE_A_SHUTTLE || t == BLDG_TYPE_A_PORT) {
                    c.llm_bldg_scrap_stored_units(x.player, x.object_index);
                }
                c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
                c.llm_bldg_reset_construction_anim(x.player, x.object_index);
                // Dismantling runs the construction timer backwards from full, hence build_time_2.
                b.cycle_progress = v.cfg_buildings[b.building_id].build_time_2;
            }

            if (workers_of(b) > v.cfg_buildings[b.building_id].builder_count) {
                c.llm_strat_bldg_unassign_workers(
                    (uint16_t)x.player, (uint32_t)x.object_index,
                    (uint32_t)(workers_of(b) - v.cfg_buildings[b.building_id].builder_count));
            }
            // NOTE the ordering: the state flips AFTER the unassign and BEFORE the re-staffing, which
            // is not the order index 1 uses.
            b.state = BLDG_STATE_DISMANTLING;
            if (v.population[x.player].human != 0 &&
                v.cfg_buildings[b.building_id].builder_count != 0) {
                c.llm_strat_bldg_assign_workers(x.player, (uint32_t)x.object_index,
                                                half_idle_population(v.population[x.player].human));
            }
            if (v.cfg_buildings[b.building_id].builder_count == 0 || b.current_workers != 0) {
                c.llm_strat_bldg_set_staffed_flag((uint16_t)x.player, x.object_index);
            } else {
                c.llm_strat_bldg_clear_staffed_flag((uint16_t)x.player, (uint32_t)x.object_index);
            }
            c.llm_strat_refresh_building((uint16_t)x.player, x.object_index);
            c.llm_bldg_set_connected_flag((uint16_t)x.player, x.object_index);
            return;
        }

        // ---- index 3 (param0 0x6d) @0x0046857c: add to the production queue --------------------------
        case bldg_arm::PROD_QUEUE_ACCUM: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;

            // queued_count[0] is the running TOTAL row; the cap is tested against it, and the request in
            // args[1] is CLAMPED IN THE ORDER RECORD (a write back into the queue, 0x00468628) rather
            // than into a local.
            if (o.args[1] + own.production_at(x.player, b.sub_id).queued_count[0] >
                PRODUCTION_QUEUE_CAP) {
                o.args[1] =
                    PRODUCTION_QUEUE_CAP - own.production_at(x.player, b.sub_id).queued_count[0];
            }
            if (o.args[1] <= 0) return;

            own.production_at(x.player, b.sub_id).queued_count[0] += o.args[1];
            own.production_at(x.player, b.sub_id).queued_count[o.args[0]] += o.args[1];

            if (b.state != BLDG_STATE_PROD_PICK_NEXT && b.state != BLDG_STATE_PROD_WORKING &&
                b.state != BLDG_STATE_PROD_BLOCKED_NOTIFY) {
                c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
                b.state = BLDG_STATE_PROD_PICK_NEXT;
                c.llm_strat_refresh_building((uint16_t)x.player, x.object_index);
            }

            // 0x0046873c-0x00468761 is DEAD and omitted: guarded by `PlayerSide != player &&
            // G_PLANET_INDEX > 4`, its entire body is `IMUL EAX,args[0],0x23f / CMP [Unit[].ai_unit],4`
            // -- a comparison whose flags the immediately following unconditional JMP discards. It
            // reads no state we do not already read and writes none, so omitting it cannot diverge.
            // Ghidra dropped it silently; it is named here so a reader of the .asm does not think it
            // was missed.
            return;
        }

        // ---- index 4 (param0 0x6f) @0x0046876d: remove from the production queue ---------------------
        case bldg_arm::PROD_ITEM_COMPLETE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;

            if (own.production_at(x.player, b.sub_id).queued_count[o.args[0]] != 0) {
                // Still queued: drop one from the type's row and one from the total row.
                own.production_at(x.player, b.sub_id).queued_count[o.args[0]] -= 1;
                own.production_at(x.player, b.sub_id).queued_count[0] -= 1;
                return;
            }
            // Nothing queued for that type -- if it is the one being BUILT right now, cancel the build.
            if (b.state != BLDG_STATE_PROD_WORKING ||
                (int32_t)(uint32_t)own.production_at(x.player, b.sub_id).active_unit_type !=
                    o.args[0]) {
                return;
            }
            b.state = BLDG_STATE_PROD_PICK_NEXT;
            c.llm_unit_apply_production_completion(
                x.player, (int32_t)(uint32_t)own.production_at(x.player, b.sub_id).active_unit_type);
            c.llm_strat_ai_notify_unit_lifecycle(
                (uint16_t)x.player,
                (uint16_t)own.production_at(x.player, b.sub_id).active_unit_type, 0u, 2u);
            return;
        }

        // ---- index 5 (param0 0x74) @0x004694d3: rescan the mine's deposits ---------------------------
        case bldg_arm::MINE_RESCAN: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            b.state = BLDG_STATE_MINE_SCAN_DEPOSITS;
            return;
        }

        // ---- index 6 (param0 0x79) @0x00468fc9: hangar recharge, only out of the idle state ----------
        case bldg_arm::HANGAR_RECHARGE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (b.state != BLDG_STATE_IDLE_NOOP) return;
            b.state          = BLDG_STATE_HANGAR_RECHARGE_CHK;
            b.cycle_progress = 0.0;
            c.llm_strat_refresh_building((uint16_t)x.player, x.object_index);
            return;
        }

        // ---- index 7 (param0 0x7b) @0x004676a9: point a turret at a counter-target -------------------
        case bldg_arm::TURRET_ACQUIRE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;

            int32_t bldg_fine_x = 0, bldg_fine_y = 0;
            c.llm_strat_bldg_get_coords((uint16_t)x.player, x.object_index, &bldg_fine_x, &bldg_fine_y);

            // args[5] is a packed owner|kind ref: low nibble = owning player, bit 0x80 = the target is a
            // UNIT (else a building). args[6] is its roster slot. The original TESTs the LOW BYTE.
            const uint32_t target_player = (uint32_t)(o.args[5] & 0xf);
            const int32_t  target_index  = o.args[6];

            int32_t target_fine_x = 0, target_fine_y = 0;
            if ((o.args[5] & 0x80) != 0) {
                const unit &u = unit_of(v, target_player, target_index);
                if (u.unit_proto_id == 0 || !energy_gate(u.energy)) return;
                c.llm_strat_unit_get_coords((uint16_t)target_player, target_index, &target_fine_x,
                                            &target_fine_y);
            } else {
                const building &tb = building_of(v, target_player, target_index);
                if (tb.building_id == 0 || !energy_gate(tb.energy)) return;
                c.llm_strat_bldg_get_coords((uint16_t)target_player, target_index, &target_fine_x,
                                            &target_fine_y);
            }

            // SIX arguments, four of them independent truncating /32 conversions (the SAR/SHL/SBB/SAR
            // idiom, value-for-value C's `/ 32` on a signed int -- sim_order_enqueue.cpp's fine_to_tile
            // note). The decompile's CONCAT22 pairing of two of them is a rendering artifact: the
            // assembly pushes plain dwords at 0x004677d0 and 0x004677e2 and passes the other two in
            // EBX/ECX.
            //
            // DECLARED NEED: turret::attack_range is the dword at map_object_turret + 0x20
            // (`MOV EDX,[EAX + 0xcc1000]` @0x0046782c). Ghidra's map_object_turret has no component
            // there, so addr/mh_structs.gen.h collapses it into `_pad_0x1c[27]` -- reaching through the
            // padding is exactly what this project forbids, so the field is named here and declared for
            // the conductor to add. It is the range llm_strat_dist_out_of_range compares the
            // building->target tile distance against.
            const uint32_t out_of_range = c.llm_strat_dist_out_of_range(
                0, own.turret_at(x.player, b.sub_id).attack_range, bldg_fine_x / 32, bldg_fine_y / 32,
                target_fine_x / 32, target_fine_y / 32);
            if (out_of_range != 0) return;

            own.turret_at(x.player, b.sub_id).counter_ref         = (uint16_t)o.args[5];
            own.turret_at(x.player, b.sub_id).counter_target_slot = o.args[6];
            b.state                                               = BLDG_STATE_TURRET_ATTACK;
            c.llm_strat_bldg_notify_ui((uint16_t)x.player, (uint32_t)x.object_index);
            return;
        }

        // ---- index 8 (param0 0x7e) @0x00467394: assign workers ---------------------------------------
        case bldg_arm::ASSIGN_WORKERS: {
            if (!energy_gate(b.energy)) return; // no online_state gate on this arm
            c.llm_strat_bldg_assign_workers(x.player, (uint32_t)x.object_index, o.args[1]);
            return;
        }

        // ---- index 9 (param0 0x7f) @0x004673cc: unassign workers -------------------------------------
        case bldg_arm::UNASSIGN_WORKERS: {
            if (!energy_gate(b.energy)) return;
            c.llm_strat_bldg_unassign_workers((uint16_t)x.player, (uint32_t)x.object_index,
                                              (uint32_t)o.args[1]);
            return;
        }

        // ---- index 10 (param0 0x80) @0x00467560: raise the staffed flag ------------------------------
        case bldg_arm::SET_STAFFED: {
            if (!energy_gate(b.energy)) return;
            c.llm_strat_bldg_set_staffed_flag((uint16_t)x.player, x.object_index);
            return;
        }

        // ---- index 11 (param0 0x81) @0x0046758e: drop the staffed flag -------------------------------
        case bldg_arm::CLEAR_STAFFED: {
            if (!energy_gate(b.energy)) return;
            // Six exempt types, in the original's order: a port, a shuttle pad or a mother is never
            // de-staffed this way.
            const uint8_t t = v.cfg_buildings[b.building_id].type;
            if (t == BLDG_TYPE_H_PORT || t == BLDG_TYPE_A_PORT) return;
            if (t == BLDG_TYPE_H_SHUTTLE || t == BLDG_TYPE_A_SHUTTLE) return;
            if (t == BLDG_TYPE_H_MOTHER || t == BLDG_TYPE_A_MOTHER) return;
            c.llm_strat_bldg_clear_staffed_flag((uint16_t)x.player, (uint32_t)x.object_index);
            return;
        }

        // ---- index 12 (param0 0x82) @0x00468aeb: start an upgrade ------------------------------------
        case bldg_arm::START_UPGRADE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (b.state == BLDG_STATE_UPGRADING) return;

            // The TARGET building type; every builder_count below is the UPGRADE's, not the current
            // building's -- which is what makes index 1's use of the kind nibble visibly a bug.
            const int32_t upgrade_id = v.cfg_buildings[b.building_id].upgrade_index;

            // Only a building at exactly full charge may upgrade (equal, or unordered -- see
            // fcom_eq_or_unordered). ENERGY here is the HP/charge stat, not the POWER resource.
            if (!fcom_eq_or_unordered(b.energy, v.cfg_buildings[b.building_id].energy)) return;

            const int32_t rc = c.llm_bldg_pay_build_cost(x.player, upgrade_id);
            if (rc != 0) {
                if (is_local_player(v, x.player)) print_building_message(v, own, c, b.building_id, rc);
                return;
            }
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            b.state          = BLDG_STATE_UPGRADING;
            b.cycle_progress = 0.0;

            if (workers_of(b) > v.cfg_buildings[upgrade_id].builder_count) {
                c.llm_strat_bldg_unassign_workers(
                    (uint16_t)x.player, (uint32_t)x.object_index,
                    (uint32_t)(workers_of(b) - v.cfg_buildings[upgrade_id].builder_count));
            }
            if (v.population[x.player].human != 0 && v.cfg_buildings[upgrade_id].builder_count != 0) {
                c.llm_strat_bldg_assign_workers(x.player, (uint32_t)x.object_index,
                                                half_idle_population(v.population[x.player].human));
            }
            if (v.cfg_buildings[upgrade_id].builder_count == 0 || b.current_workers != 0) {
                c.llm_strat_bldg_set_staffed_flag((uint16_t)x.player, x.object_index);
            } else {
                c.llm_strat_bldg_clear_staffed_flag((uint16_t)x.player, (uint32_t)x.object_index);
            }
            c.llm_strat_refresh_building((uint16_t)x.player, x.object_index);
            c.llm_strat_ai_queue_release_order((int32_t)x.player, x.object_index, 0);
            return;
        }

        // ---- index 13 (param0 0x83) @0x00469535: abandon whatever the building is doing --------------
        case bldg_arm::CANCEL_RESET: {
            // No gate at all -- not even the energy one.
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            return;
        }

        // ---- index 14 (param0 0x89) @0x0046893b: start a research project ----------------------------
        case bldg_arm::START_RESEARCH: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            // Reject a duplicate request: same project AND already researching.
            if (own.lab_at(x.player, b.sub_id).active_project_id == o.args[7] &&
                b.state == BLDG_STATE_RESEARCHING) {
                return;
            }
            const int32_t rc = c.game_TryStartProject((uint16_t)x.player, x.object_index,
                                                      (uint32_t)o.args[7]);
            if (rc != 0) {
                if (is_local_player(v, x.player)) print_building_message(v, own, c, b.building_id, rc);
                return;
            }
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            own.lab_at(x.player, b.sub_id).active_project_id = o.args[7];
            b.state                                          = BLDG_STATE_RESEARCHING;
            b.online_state                                   = 2;
            b.cycle_progress                                 = 0.0;
            return;
        }

        // ---- index 15 (param0 0xd2) @0x00467fd9: launch a SHUTTLE ------------------------------------
        case bldg_arm::SHUTTLE_DEPART: {
            // 0x00467fd9 is a DEAD `CMP _G_LLM_STRAT_OUTER_PLANET_LANDED_FLAG,0` -- the very next
            // instruction is `IMUL EDX,[EBP-0x34],0x6aa4`, which overwrites every flag it set, and no
            // branch reads them. Omitted; the global is deliberately not added to sim_view for it.
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            // A shuttle whose cfg configures NO fuel type (fuel[0].id == 0) may only "fly" to the
            // planet it is already on; a configured fuel type is what lets it leave at all. (This
            // comment said the opposite until 2026-08-08 -- the CODE was always right and matches
            // 0x0046800d; a test written from the prose rather than the guard failed, which is how
            // it was found.)
            if (v.cfg_buildings[b.building_id].fuel[0].id == 0 && o.args[6] != *v.planet_index) return;
            // ...and off-world traffic exists in single player only.
            if (*v.session_mode != SESSION_SP && o.args[6] != *v.planet_index) return;
            {
                const uint8_t t = v.cfg_buildings[b.building_id].type;
                if (t != BLDG_TYPE_A_SHUTTLE && t != BLDG_TYPE_H_SHUTTLE) return;
            }

            if (fcom_below_or_unordered(b.energy, v.cfg_buildings[b.building_id].energy)) {
                // Damaged: refuse with the fixed "not ready" reason rather than a callee status.
                if (is_local_player(v, x.player)) {
                    print_building_message(v, own, c, b.building_id, TEXT_ID_INSUFFICIENT_ENERGY);
                }
                return;
            }
            // NOTE: unlike index 16/17, a failed fuel check here prints NOTHING.
            if (c.llm_prod_shuttle_fuel_check((uint16_t)x.player, x.object_index, o.args[6]) != 0) {
                return;
            }
            c.llm_prod_shuttle_fuel_apply((uint16_t)x.player, x.object_index, o.args[6]);
            if (b.shuttle_slot == 0) {
                c.llm_prod_shuttle_slot_bind_default(x.player, x.object_index);
            }
            if (b.shuttle_slot == 0) return;
            if (o.args[6] != *v.planet_index &&
                c.llm_strat_prod_bind_planet((int32_t)x.player, *v.planet_index,
                                             (int32_t)(uint32_t)b.shuttle_slot) == 0) {
                return;
            }
            c.llm_prod_shuttle_depart((uint16_t)x.player, x.object_index, o.args[6]);
            c.llm_bldg_transfer_notify_noop();
            return;
        }

        // ---- indices 17 and 16 (param0 0xd4 and 0xd3) @0x00468256 / 0x0046825d ------------------------
        // Index 17's whole body is a DEAD `CMP _G_LLM_STRAT_OUTER_PLANET_LANDED_FLAG,0` at 0x00468256
        // whose flags the first instruction of index 16 (`IMUL EDX,...`) clobbers, and then it FALLS
        // THROUGH into index 16. So the two arms are one body, which is what the decompile's shared
        // `case 0x11: case 0x10:` says. The fallthrough is kept; the dead read is omitted.
        case bldg_arm::MOTHER_DEPART:
        case bldg_arm::PORT_DEPART: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (*v.session_mode != SESSION_SP && o.args[6] != *v.planet_index) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;

            {
                const uint8_t t = v.cfg_buildings[b.building_id].type;
                if ((t == BLDG_TYPE_A_MOTHER || t == BLDG_TYPE_H_MOTHER) &&
                    fcom_below_or_unordered(b.energy, v.cfg_buildings[b.building_id].energy)) {
                    // A damaged mothership cannot launch; a damaged PORT can.
                    if (is_local_player(v, x.player)) {
                        print_building_message(v, own, c, b.building_id, TEXT_ID_INSUFFICIENT_ENERGY);
                    }
                    return;
                }
            }

            const int32_t rc =
                c.llm_prod_shuttle_fuel_check((uint16_t)x.player, x.object_index, o.args[6]);
            if (rc != 0) {
                // ...and here, unlike index 15, the failure IS reported.
                if (is_local_player(v, x.player)) print_building_message(v, own, c, b.building_id, rc);
                return;
            }
            c.llm_prod_shuttle_fuel_apply((uint16_t)x.player, x.object_index, o.args[6]);
            if (b.shuttle_slot == 0) {
                c.llm_prod_shuttle_slot_bind_default(x.player, x.object_index);
            }
            if (b.shuttle_slot == 0) return;
            if (o.args[6] != *v.planet_index &&
                c.llm_strat_prod_bind_planet((int32_t)x.player, *v.planet_index,
                                             (int32_t)(uint32_t)b.shuttle_slot) == 0) {
                return;
            }
            // NOTE: no llm_bldg_transfer_notify_noop() on this arm, unlike index 15.
            c.llm_prod_bldg_depart_finalize((uint16_t)x.player, x.object_index, o.args[6]);
            return;
        }

        // ---- index 18 (param0 0xd6) @0x004678d3: claim a cargo slot ----------------------------------
        case bldg_arm::CARGO_BIND_SLOT: {
            // Note the INVERTED slot test: this arm only runs while the building has NO slot bound.
            if (!energy_gate(b.energy) || b.shuttle_slot != 0) return;
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            c.llm_prod_shuttle_slot_bind_default(x.player, x.object_index);
            c.llm_bldg_transfer_notify_noop();
            return;
        }

        // ---- index 19 (param0 0xdb) @0x00467aff: load passengers -------------------------------------
        case bldg_arm::LOAD_PASSENGERS: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            if (b.shuttle_slot == 0) {
                c.llm_prod_shuttle_slot_bind_default(x.player, x.object_index);
            }
            if (b.shuttle_slot == 0) return;
            // MOVZX: only the low 16 bits of args[1] reach the callee.
            c.llm_prod_shuttle_load_passengers((uint16_t)x.player, x.object_index,
                                               (uint32_t)(uint16_t)o.args[1]);
            return;
        }

        // ---- index 20 (param0 0xdc) @0x00467c40: unload passengers -----------------------------------
        case bldg_arm::UNLOAD_PASSENGERS: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (b.shuttle_slot == 0) return; // pre-gate here, unlike index 19's bind-on-demand
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            // Full dword, not the MOVZX index 19 uses.
            c.llm_prod_shuttle_unload_passengers((uint16_t)x.player, x.object_index, o.args[1]);
            return;
        }

        // ---- index 21 (param0 0xdd) @0x00467d5d: load cargo ------------------------------------------
        case bldg_arm::LOAD_RESOURCE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            if (b.shuttle_slot == 0) {
                c.llm_prod_shuttle_slot_bind_default(x.player, x.object_index);
            }
            if (b.shuttle_slot != 0) {
                c.llm_prod_shuttle_load_resource(x.player, x.object_index, (uint32_t)(uint16_t)o.args[3],
                                                 (uint32_t)(uint16_t)o.args[2]);
            }
            // OUTSIDE the slot test: the tail noop runs whether or not anything was loaded.
            c.llm_bldg_load_resource_tail_noop();
            return;
        }

        // ---- index 22 (param0 0xde) @0x00467eb1: unload cargo ----------------------------------------
        case bldg_arm::UNLOAD_RESOURCE: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            if (b.shuttle_slot == 0) return;
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            // args[3] narrowed to 16 bits, args[2] full width -- asymmetric, and it is the original's.
            c.llm_prod_shuttle_unload_resource((uint16_t)x.player, x.object_index,
                                               (uint16_t)o.args[3], o.args[2]);
            return;
        }

        // ---- index 23 (param0 0xdf) @0x004679e9: empty the cargo hold ---------------------------------
        case bldg_arm::FLUSH_CARGO_HOLD: {
            // No online_state gate on this arm.
            if (!energy_gate(b.energy) || b.shuttle_slot == 0) return;
            if (*v.session_mode != SESSION_SP) return;
            if (!is_port_or_mother(v.cfg_buildings[b.building_id].type)) return;
            c.llm_strat_bldg_flush_cargo_hold(x.player, x.object_index);
            c.llm_bldg_transfer_notify_noop();
            return;
        }

        // ---- index 24 (param0 0xe6) @0x00467364: purge the dock's dead/pending entries -----------------
        case bldg_arm::PURGE_DEAD_DOCKED: {
            // No gate at all. The sub-roster is reached through the BUILDING's sub_id, as always.
            c.llm_strat_storage_purge_dead_docked((int32_t)x.player, (int32_t)(uint32_t)b.sub_id);
            c.llm_storage_cancel_pending_docked(x.player, x.object_index);
            return;
        }

        // ---- index 25 (param0 0xea) @0x00467404: place a building ------------------------------------
        case bldg_arm::INSTANT_CONSTRUCT: {
            // args[4]/args[5] are the site's tile x/y, args[0] the building type, args[1] an opaque
            // payload construct_finalize takes as its first (ECX) argument.
            const int32_t clear = c.llm_bldg_footprint_is_clear(o.args[4], o.args[5], o.args[0], 8u);

            const bool may_build =
                clear != 0 && v.profiles[x.player].primary_mother_bldg[*v.planet_index] != 0 &&
                (*v.tutorial_step == 0 || is_local_player(v, x.player));

            if (may_build && c.llm_bldg_pay_build_cost(x.player, o.args[0]) == 0) {
                // Paid: commit the building. Six arguments in the committed
                // ECX/EDX/stack/stack/EAX/EBX order -- see addr/mh_calls.gen.h; the register order and
                // the parameter order are NOT the same here.
                c.llm_bldg_construct_finalize((uint32_t)o.args[1], o.args[5], (uint16_t)x.player,
                                              (char)1, (uint32_t)o.args[4], (uint32_t)o.args[0]);
                return;
            }

            // Refused, or unaffordable: the SAME two statements the original emits twice, once per
            // branch (0x004674b9 and 0x0046750b are instruction-for-instruction identical).
            if (*v.sim_active != 0 && is_local_player(v, x.player)) {
                c.llm_snd_play(SND_BUILD_REFUSED, SND_BUILD_REFUSED_V);
            }
            c.llm_strat_ai_notify_bldg_constructed(x.player, (uint32_t)o.args[4], 0u,
                                                   (uint32_t)o.args[0], (uint32_t)o.args[5], 2u);
            return;
        }

        // ---- index 0: EVERY param0 the table does not name @0x00469545 -------------------------------
        // Not an empty default. `REPNE SCASB` leaves ECX == 0 when the scan finds nothing, and this
        // body runs -- storing param0 STRAIGHT INTO the building's state machine index. It is also
        // where the scan's one-byte over-read past the table lands, which is harmless precisely
        // because it lands HERE (sim_order_dispatch.h's note on the tables; the value that over-read
        // byte would decode to is not an opcode and appears in no enum). Switching on the decoded
        // index rather than on the opcode is what makes all of that free.
        default: {
            if (!energy_gate(b.energy) || b.online_state == 0) return;
            c.llm_bldg_finish_current_order(x.player, (uint32_t)x.object_index);
            b.state          = (uint16_t)o.param0;
            b.cycle_progress = 0.0;
            return;
        }
    }
}

} // namespace detail
} // namespace mh::sim
