//
// sim_cfg_apply_project_resources_selftest.cpp -- `simtest` offline oracle for
// llm_cfg_apply_project_resources (sim/sim_cfg_apply_project_resources.{h,cpp}, RI-SIM / SIM1F,
// batch F).
//
// The whole function is one unbounded resource walk behind a one-member `_calls` struct
// (resource_add), fully offline-coverable via the recording stub below -- no live-image call, no
// float, no roster write, no sim_store access at all (pure read + calls).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_cfg_apply_project_resources_00492e35.asm) per the header's own derivation, not the
// .cpp.
//
#include "sim/sim_cfg_apply_project_resources.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct ResourceAddCall {
    int32_t player;
    int32_t resource_id;
    int32_t amount;
};
std::vector<ResourceAddCall> g_calls_log;

void stub_resource_add(int32_t player, int32_t resource_id, int32_t amount) {
    g_calls_log.push_back({player, resource_id, amount});
}

const cfg_apply_project_resources_calls g_calls = {stub_resource_add};

void clear_calls() { g_calls_log.clear(); }

} // namespace

void run_apply_project_resources_tests() {
    sim_fixture fx;

    // ---- C1: three real (id,val) entries then a sentinel -> exactly 3 calls, IN ORDER, with id/val
    // kept paired per slot (not swapped or reordered), and player masked to its low 16 bits.
    // Mutation note: swapping `id`/`val` in the call, reading the wrong slot's val, or dropping the
    // player mask would flip one of these checks red.
    fx.reset();
    clear_calls();
    fx.cfg_projects[4].resource[0].id  = 11;
    fx.cfg_projects[4].resource[0].val = 100;
    fx.cfg_projects[4].resource[1].id  = 22;
    fx.cfg_projects[4].resource[1].val = 200;
    fx.cfg_projects[4].resource[2].id  = 33;
    fx.cfg_projects[4].resource[2].val = 300;
    fx.cfg_projects[4].resource[3].id  = 0; // terminator (already 0 post-reset(); set explicitly)
    detail::cfg_apply_project_resources(fx.view(), g_calls, /*player_id*/ 0x00070006u, /*project_index*/ 4u);
    ck_eq((uint32_t)g_calls_log.size(), 3u, "C1: exactly 3 calls (stops at the id==0 sentinel)");
    ck_eq((uint32_t)g_calls_log[0].player, 6u, "C1[0]: player masked to low16 (0x00070006 -> 6)");
    ck_eq((uint32_t)g_calls_log[0].resource_id, 11u, "C1[0]: id=11");
    ck_eq((uint32_t)g_calls_log[0].amount, 100u, "C1[0]: val=100, paired with id=11");
    ck_eq((uint32_t)g_calls_log[1].resource_id, 22u, "C1[1]: id=22");
    ck_eq((uint32_t)g_calls_log[1].amount, 200u, "C1[1]: val=200, paired with id=22");
    ck_eq((uint32_t)g_calls_log[2].resource_id, 33u, "C1[2]: id=33");
    ck_eq((uint32_t)g_calls_log[2].amount, 300u, "C1[2]: val=300, paired with id=33");

    // ---- C2: an empty resource list (first slot already the sentinel) -> zero calls -----------------
    fx.reset();
    clear_calls();
    // fx.cfg_projects[4].resource[0].id is 0 by default after reset() -- an empty list.
    detail::cfg_apply_project_resources(fx.view(), g_calls, /*player_id*/ 1u, /*project_index*/ 4u);
    ck_eq((uint32_t)g_calls_log.size(), 0u, "C2: empty resource list -> zero calls");

    // ---- C3: a sentinel MID-array ends the walk early; an entry placed AFTER it must never be seen --
    // resource[2] holds a large, easily-distinguished (id,val) pair; if the walk incorrectly continued
    // past the terminator at resource[1], a 3rd call would appear with that id.
    fx.reset();
    clear_calls();
    fx.cfg_projects[5].resource[0].id  = 7;
    fx.cfg_projects[5].resource[0].val = 70;
    fx.cfg_projects[5].resource[1].id  = 0;  // terminator
    fx.cfg_projects[5].resource[2].id  = 99; // must never be read -- lies past the terminator
    fx.cfg_projects[5].resource[2].val = 9999;
    detail::cfg_apply_project_resources(fx.view(), g_calls, /*player_id*/ 2u, /*project_index*/ 5u);
    ck_eq((uint32_t)g_calls_log.size(), 1u, "C3: sentinel at slot 1 stops the walk (1 call, not 2)");
    ck_eq((uint32_t)g_calls_log[0].resource_id, 7u, "C3: the one call is the pre-sentinel entry (id=7)");

    // ---- C4: all 7 declared resource slots populated, none zero -> exactly 7 calls, no more --------
    // (boundary coverage for CFG_RESOURCE_SLOTS==7; unlike game_TryStartProject's identical-shape
    // walk, resource_add here is a STUBBED call with no live-image side effect, so this test is safe
    // to run to completion regardless of what the walk does with the array.)
    fx.reset();
    clear_calls();
    for (int i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        fx.cfg_projects[6].resource[i].id  = (uint32_t)(i + 1); // ids 1..7, all nonzero
        fx.cfg_projects[6].resource[i].val = (i + 1) * 10;      // 10..70, distinct per slot
    }
    detail::cfg_apply_project_resources(fx.view(), g_calls, /*player_id*/ 3u, /*project_index*/ 6u);
    ck_eq((uint32_t)g_calls_log.size(), 7u, "C4: exactly 7 calls for a fully-populated resource array");
    ck_eq((uint32_t)g_calls_log[0].resource_id, 1u, "C4[0]: id=1 (first slot)");
    ck_eq((uint32_t)g_calls_log[6].resource_id, 7u, "C4[6]: id=7 (last of the 7 declared slots)");
    ck_eq((uint32_t)g_calls_log[6].amount, 70u, "C4[6]: val=70, paired with the last id");

    // ---- C5: player masking on a single-entry walk, distinct player/id/val from every case above ---
    fx.reset();
    clear_calls();
    fx.cfg_projects[7].resource[0].id  = 5;
    fx.cfg_projects[7].resource[0].val = 55;
    fx.cfg_projects[7].resource[1].id  = 0;
    detail::cfg_apply_project_resources(fx.view(), g_calls, /*player_id*/ 0x00abcd04u, /*project_index*/ 7u);
    ck_eq((uint32_t)g_calls_log.size(), 1u, "C5: one call");
    ck_eq((uint32_t)g_calls_log[0].player, 0xcd04u, "C5: player masked to low16 (0x00abcd04 -> 0xcd04)");
}

} // namespace mh::sim::test
