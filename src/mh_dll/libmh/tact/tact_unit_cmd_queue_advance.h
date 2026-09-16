//
// tact/tact_unit_cmd_queue_advance.h -- TACT1B: drain the command queue from a given
// head index, following the head as it moves.
//
//   llm_tact_unit_cmd_queue_advance @0x004313cf (0xdb)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_cmd_queue_advance @0x004313cf.
//
// 1. @0x004313ec-0x0043140d: proceed only when status bit 0x2 is set AND progress (+0x44) == 0;
//    otherwise return immediately.
// 2. @0x00431414-0x0043142b: clear status bit 0x2 (`status &= 0xfd`).
// 3. @0x0043143e-0x0043149f: drain the queue starting at `cmd_index`, dispatching
//    llm_tact_unit_cmd_advance(unit_idx, cmd_index) (frontier) once per live entry. THE LOOP
//    FOLLOWS THE UNIT'S OWN cmd_index FIELD (+0x53), NOT the local parameter: after each dispatch
//    (except the op==0x7f arm, which stops immediately after dispatching), `cmd_index` is
//    RE-READ from the unit's own cmd_index field (0x0043148e-0x0043149c) -- the frontier callee is
//    expected to have advanced it. Stops when the entry at the current cmd_index has op == 0.
void unit_cmd_queue_advance(tact_store &own, int32_t unit_idx, uint32_t cmd_index);

} // namespace detail

void unit_cmd_queue_advance(int32_t unit_idx, uint32_t cmd_index);


} // namespace mh::tact
