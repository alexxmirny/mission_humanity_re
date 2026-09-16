//
// sim/sim_game_add_project_to_available.cpp -- see sim_game_add_project_to_available.h. Translated
// from the DISASSEMBLY (tmp/decomp/game_AddProjectToAvailable_0041422e.asm,
// tmp/decomp/game_AddProjectToAvailableWithCheck_00440174.asm), which the Ghidra .c drafts agree with
// exactly.
//
#include "sim/sim_game_add_project_to_available.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const add_project_to_available_calls &live_add_project_to_available_calls() {
    static const add_project_to_available_calls c = {
        MH_LIBMH_BIND(game_InsertItemInPlayerArray),
    };
    return c;
}

namespace detail {

void add_project_to_available(const sim_view &v, sim_store &own,
                              const add_project_to_available_calls &c, uint32_t player, uint32_t p_i) {
    // 0x0041422e-0x0041426c: bucket = &AvailableProjects[player][Projects[p_i].type].
    // `Projects[p_i].type` read through the already-bound sim_view::cfg_projects rather than the raw
    // [p_i*0xd0 + 0xbe1c6c] offset the assembly computes -- same field (cfg_final_struct_Project::type
    // @+0xc); see the header's DECLARED NEED 1 for the bucket-pointer accessor this line depends on.
    const int32_t type   = v.cfg_projects[p_i].type;
    int32_t      *bucket = own.available_projects_bucket(player, type); // DECLARED NEED, see header

    // 0x0041426f-0x00414280: game_InsertItemInPlayerArray(bucket, 50, player, 0, p_i). Register order
    // (EAX=arr, EDX=size, EBX=player, ECX=item, stack=new_item) matches the committed
    // game_InsertItemInPlayerArray(void*, int32_t, uint32_t, int32_t, int32_t) signature exactly.
    c.insert_item_in_player_array(bucket, AVAILABLE_PROJECTS_SLOTS_PER_BUCKET, player, ITEM_UNUSED,
                                  static_cast<int32_t>(p_i));
}

void add_project_to_available_with_check(const sim_view &v, sim_store &own,
                                         const add_project_to_available_calls &c, uint16_t player,
                                         uint32_t p_i) {
    // 0x00440174-0x00440198: MOVZX EAX, word ptr[player_stack_slot] -- the entire body is this one
    // forwarding call, no gate of any kind despite the function's name.
    add_project_to_available(v, own, c, static_cast<uint32_t>(player), p_i);
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

void add_project_to_available(uint32_t player, uint32_t p_i) {
    sim_state st = state();
    detail::add_project_to_available(st.read, st.own, live_add_project_to_available_calls(), player,
                                     p_i);
}

void add_project_to_available_with_check(uint16_t player, uint32_t p_i) {
    sim_state st = state();
    detail::add_project_to_available_with_check(st.read, st.own, live_add_project_to_available_calls(),
                                                player, p_i);
}


} // namespace mh::sim
