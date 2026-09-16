//
// sim/sim_unit_order_disembark_soldiers.h -- llm_unit_order_disembark_soldiers @0x0046e76c
// (RI-SIM, X-TL-DRAIN step 4).
//
// `void __watcall llm_unit_order_disembark_soldiers(uint player, int unit_idx)` -- player in EAX,
// unit_idx in EDX, matching the committed prototype (sig_llm_unit_order_disembark_soldiers).
// Translated from the DISASSEMBLY (tmp/decomp/llm_unit_order_disembark_soldiers_0046e76c.asm).
//
// ---- WHAT IT IS ---------------------------------------------------------------------------------
//
// The order-issue wrapper for "this transport/squad unit lets its soldiers out": if the unit's TYPE
// carries soldiers at all, issue order 0x32 against it; otherwise do nothing at all. Six
// instructions of guard and one dispatch.
//
// It is being translated because it is one of the two remaining original callers of
// `llm_strat_order_dispatch`, a body libmh owns -- the edge that kept that function in the inbound
// surface's X-TL exclusion class (see sim_bldg_init_all.h's banner for the same argument).
//
// ---- THE BODY, INSTRUCTION BY INSTRUCTION ------------------------------------------------------
//
//   0x0046e789  MOVZX EAX,word ptr [EBP-0x14]          player, truncated to 16 bits
//   0x0046e78d  IMUL EDX,EAX,0x5b04                    ... * the units row stride
//   0x0046e793  IMUL EAX,[EBP-0x18],0xe9               unit_idx * sizeof(map_object_unit)
//   0x0046e79a  ADD EAX,EDX
//   0x0046e79c  MOVZX EAX,word ptr [EAX + 0xdd8c4a]    units[player][unit_idx].unit_proto_id
//                                                      (`units` base 0x00dd8c48, +2 == unit_proto_id)
//   0x0046e7a3  IMUL EAX,EAX,0x23f                     ... * sizeof(cfg Unit)
//   0x0046e7a9  CMP dword ptr [EAX + 0xe4a2bf],0x0     Unit[proto].soldier_count
//                                                      (`Unit` base 0x00e4a098, +0x227 == +551)
//   0x0046e7b0  JLE <return>                           <= 0: NOTHING happens -- SIGNED compare
//   0x0046e7b2  MOV ECX,0x32   /  0x0046e7b7 MOV EBX,0x32
//   0x0046e7bc  MOV EAX,[EBP-0x14] / OR AL,0x80 / MOVZX EDX,AX
//   0x0046e7c4  MOVZX EAX,word ptr [EBP-0x18]
//   0x0046e7c8  CALL 0x00465fdf                        llm_strat_order_dispatch
//
// Both field offsets were resolved against the generated struct layout as well as the disp32, per
// the measured offsets: 0xdd8c4a - 0xdd8c48 = 2 == map_object_unit::unit_proto_id, and
// 0xe4a2bf - 0xe4a098 = 0x227 == cfg_final_struct_Unit::soldier_count. Neither index was taken from
// a Ghidra field NAME.
//
// THE `| 0x80` IS APPLIED TO AL AND THE RESULT READ AS AX, so the value dispatch receives is
// `(uint16_t)(player | 0x80)`. The Ghidra draft renders it `player & 0xffff | 0x80`, which is the
// same number for every input (the OR only touches bit 7, below the truncation), but the ORDER below
// follows the instructions.
//
// 0x32 GOES INTO BOTH ECX AND EBX. The committed prototype's `op_code` lands in EBX and `arg` in ECX
// (mh_calls.gen.h routes them through s_u32_EAX_EDX_EBX_ECX), so the two are indistinguishable here --
// there is no argument-order hazard at this call site, and none is invented.
//
// ---- THE SHADOW ARM IS DELIBERATELY VACUOUS -----------------------------------------------------
//
// This body writes nothing itself and returns void; its whole observable effect is the dispatch call.
// That call CANNOT run for real in a shadow arm: llm_strat_order_dispatch routes into
// llm_strat_order_stage_scheduled, which runs llm_strat_order_integrity_check -- and in SESSION_MODE 3
// a failed check tears the session down and forces a return to the main menu (the closure walk reaches
// llm_menu_force_return_to_main and llm_ui_dlg_build_from_table at depth 4/5). That is the effect
// libmh/orders/order_queue.cpp's own dispatch notes call "strictly worse than useless", and the
// reasoning transfers unchanged to a CALLER of dispatch. So the arm binds an INERT dispatch, which
// leaves nothing to compare: void return, zero declared regions. gen_shadow_ini.py classifies the site
// VACUOUS from the manifest and holds it out of every arm set automatically -- recorded here so the
// classification reads as a derivation and not an oversight.
//
// ITS ORACLE IS THEREFORE OFFLINE: mh_nettest/sim_unit_order_disembark_soldiers_selftest.cpp drives
// detail::unit_order_disembark_soldiers against a sim_fixture with a recording dispatch stub, which
// can observe exactly what a shadow cannot -- that the gate fires on the sign, and that the four
// arguments carry the values the register setup above computes.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

struct unit_order_disembark_soldiers_calls {
    // llm_strat_order_dispatch @0x00465fdf. Return is DISCARDED at this call site (the original never
    // reads EAX back); kept in the slot type because the committed prototype returns int32_t and a
    // calls-struct member must match it exactly.
    int32_t (*order_dispatch)(uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg);
};

const unit_order_disembark_soldiers_calls &live_unit_order_disembark_soldiers_calls();
// The shadow arm's table: the same struct with `order_dispatch` replaced by a no-op. See the banner.
const unit_order_disembark_soldiers_calls &inert_unit_order_disembark_soldiers_calls();

namespace detail {

// llm_unit_order_disembark_soldiers @0x0046e76c.
void unit_order_disembark_soldiers(const sim_view &v, const unit_order_disembark_soldiers_calls &c,
                                   uint32_t player, int32_t unit_idx);


} // namespace detail

// Live wrapper: matches sig_llm_unit_order_disembark_soldiers exactly.
void unit_order_disembark_soldiers(uint32_t player, int32_t unit_idx);

} // namespace mh::sim
