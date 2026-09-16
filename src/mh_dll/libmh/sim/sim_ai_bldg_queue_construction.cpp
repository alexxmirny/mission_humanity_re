//
// sim/sim_ai_bldg_queue_construction.cpp -- see sim_ai_bldg_queue_construction.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_bldg_queue_construction_004e2550.asm,
// tmp/decomp/llm_strat_bldg_queue_construction_thunk_004e4e36.asm), not from the Ghidra `.c`
// drafts.
//
#include "sim/sim_ai_bldg_queue_construction.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_bldg_queue_construction -- bound live in live_bldg_queue_construction_thunk_calls()
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_queue_construction_thunk_calls &live_bldg_queue_construction_thunk_calls() {
    static const bldg_queue_construction_thunk_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_queue_construction),
    };
    return gc;
}

namespace detail {

int32_t bldg_queue_construction(const sim_view &v, sim_store &own, int32_t player,
                                int32_t building_type, int16_t x, uint16_t y) {
    player_data &pd = own.player_at(static_cast<uint32_t>(player));

    // 0x004e257a-0x004e2588: `(uint)ai_bldg_queue_count < 0x40` -- JC (unsigned below) falls into
    // the append path; the not-below case rejects (return 1) without touching the queue.
    if (static_cast<uint32_t>(pd.ai_bldg_queue_count) >= 0x40) {
        return 1;
    }

    // The count is re-read from memory before every field write in the original (see header
    // banner); a single local `slot` is behaviourally identical since nothing in this body writes
    // the count before its own final increment below.
    const int32_t slot = pd.ai_bldg_queue_count;
    auto         &e    = pd.ai_bldg_queue[slot];

    e.status          = 1;                                   // 0x004e2594: CONSTRUCTION kind
    e.tick_or_unit_id = static_cast<uint8_t>(building_type); // 0x004e25a6
    e.build_tile_x    = x;                                   // 0x004e25b4
    // 0x004e25c3: resource_reserved[0] REUSED as the cached build tile Y (see the struct's own
    // field comment) -- not a cost slot here.
    e.resource_reserved[0] = static_cast<int16_t>(y);
    e.building_index       = -1; // 0x004e25d2: unresolved

    // 0x004e25dd-0x004e260c: reset resource_reserved[1..4] to 0 before accrual. Slot [0] (the
    // reused build-tile-Y just written above) is deliberately left untouched by this loop.
    for (int32_t i = 1; i <= 4; ++i) e.resource_reserved[i] = 0;

    // 0x004e2612-0x004e2665: walk up to CFG_RESOURCE_SLOTS(7) entries of
    // cfg_buildings[building_type].resource[], stopping at the first UNDEFINED (id==0) id,
    // accruing each cost's low 16 bits into resource_reserved[id].
    const cfg_building &b = v.cfg_buildings[building_type];
    for (int32_t i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        const uint32_t id = b.resource[i].id;
        if (id == 0) break; // UNDEFINED sentinel

        // See the header banner's "THE UNBOUNDED STORE INDEX" section -- the original store here
        // is unbounded (`ADD word[entry+id*2+4],DX` with no check on `id`), the same class of bug
        // ai/ai_queue_enqueue.cpp's cost_walk() documents for the sibling train/repair/upgrade
        // enqueue functions on this SAME struct field. Reproducing it literally would be an
        // out-of-bounds write (UB in C++), so an id outside [1, BLDG_QUEUE_RESOURCE_RESERVED_COUNT)
        // is skipped rather than corrupting a neighbouring field. Unreachable with shipped cfg
        // data (every Building.resource[] id is 1..4).
        if (id >= static_cast<uint32_t>(BLDG_QUEUE_RESOURCE_RESERVED_COUNT)) continue;

        // 0x004e2652/0x004e2659: `val`'s LOW 16 BITS only (MOV DX, word ...), added as a 16-bit
        // ADD -- i.e. int16_t-width accrual, matching the destination field's own width.
        e.resource_reserved[id] = static_cast<int16_t>(
            e.resource_reserved[id] + static_cast<int16_t>(static_cast<uint16_t>(b.resource[i].val)));
    }

    pd.ai_bldg_queue_count = slot + 1; // 0x004e267d
    return 0;                          // 0x004e267b: EDX zeroed on the append path
}

void bldg_queue_construction_thunk(const bldg_queue_construction_thunk_calls &c, int32_t player,
                                   int32_t building_type, int16_t x, uint16_t y) {
    // 0x004e4e36-0x004e4e3d: `MOV EAX,ESI ; CALL llm_strat_bldg_queue_construction ; JMP
    // 0x004e792e` -- a pure tail-call forwarder. The callee's return value is discarded, matching
    // the thunk's own committed `void` prototype.
    c.queue_construction(player, building_type, x, y);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t bldg_queue_construction(int32_t player, int32_t building_type, int16_t x, uint16_t y) {
    sim_state st = state();
    return detail::bldg_queue_construction(st.read, st.own, player, building_type, x, y);
}

void bldg_queue_construction_thunk(int32_t player, int32_t building_type, int16_t x, uint16_t y) {
    detail::bldg_queue_construction_thunk(live_bldg_queue_construction_thunk_calls(), player,
                                          building_type, x, y);
}


} // namespace mh::sim
