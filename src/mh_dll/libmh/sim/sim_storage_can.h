#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PORT/_A_SHUTTLE/_H_SHUTTLE, BUILT_FLAGS_OPERATIONAL
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_GROUND

namespace mh::sim {

// ---- can_exit's one outward call, indirected for testability (see the header banner on why -- a
// direct mh::call:: inside detail:: reaches into the live game image, making the body untestable by
// net_selftest.exe simtest). Signature copied verbatim from addr/mh_calls.gen.h.
struct storage_can_exit_calls {
    uint32_t (*storage_exit_tile_is_clear)(uint16_t player, int32_t storage_slot); // @0x00489cff
    void (*storage_resolve_exit_blockage)(uint32_t player, int32_t storage_slot);  // @0x00489b40
};

const storage_can_exit_calls &live_storage_can_exit_calls();

// ---- can_enter's outward calls. ----
struct storage_can_enter_calls {
    // w_sprintf's measured `vss` shape (dst, format, a0, a1) -- see addr/mh_calls.gen.h's
    // w_sprintf__vss. Its output goes to own.text_scratch(), which reaches neither the determinism
    // hash nor a save (same posture sim_order_dispatch_bldg.cpp's print_building_message documents).
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0,
                              const wchar_t *a1);                                  // @0x004d0320
    uint32_t (*game_ui_PrintTextMessage)(void *text);                              // @0x00496508
    int32_t (*shuttle_slot_bind_default)(uint32_t player, int32_t building_index); // @0x0048dea7
};

const storage_can_enter_calls &live_storage_can_enter_calls();

namespace detail {

// llm_strat_storage_can_exit @0x0048a548. See the header banner above for the full derivation.
int32_t storage_can_exit(const sim_view &v, const storage_can_exit_calls &c, int32_t player,
                         uint32_t unit_index, int32_t storage_slot);

// llm_strat_storage_can_enter @0x0048a808. See the header banner above for the full derivation.
int32_t storage_can_enter(const sim_view &v, sim_store &own, const storage_can_enter_calls &c,
                          uint16_t player, uint32_t unit_index, uint32_t storage_slot);

// llm_strat_storage_can_land @0x0048ae90. See the header banner above -- unit_index is accepted but
// genuinely unread (confirmed against the whole disassembly), kept for prototype fidelity.
int32_t storage_can_land(const sim_view &v, int32_t player, uint32_t unit_index, int32_t storage_slot);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the committed prototypes
// (sig_llm_strat_storage_can_{exit,enter,land} in addr/mh_export.gen.h / addr/mh_calls.gen.h) exactly.

int32_t storage_can_exit(int32_t player, uint32_t unit_index, int32_t storage_slot);
int32_t storage_can_enter(uint16_t player, uint32_t unit_index, uint32_t storage_slot);
int32_t storage_can_land(int32_t player, uint32_t unit_index, int32_t storage_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
