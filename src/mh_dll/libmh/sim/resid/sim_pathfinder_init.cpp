//
// sim/resid/sim_pathfinder_init.cpp -- see sim_pathfinder_init.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_pathfinder_init_004614f9.asm), the Ghidra .c being a draft.
//
#include "sim/resid/sim_pathfinder_init.h"

#include "addr/mh_calls.gen.h" // typed callables for the frontier originals we still call OUT to
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/host_api.h"    // LIB-CRT: utils_abort is a host callback, not CRT

namespace mh::sim {

const pathfinder_init_calls &live_pathfinder_init_calls() {
    static const pathfinder_init_calls c = {
        MH_CRT(utils_malloc_struct_array),
        MH_CRT(utils_fill_data),
        mh::host().fatal,
    };
    return c;
}

namespace {

constexpr int32_t  JOB_RESULT_SLOT_COUNT  = 100;      // 0x64: loop bound @0x00461518
constexpr uint32_t JOB_STEP_BUF_COUNT     = 3000;     // 0xbb8: array_size @0x0046152a
constexpr uint32_t JOB_STEP_BUF_ELEM_SIZE = 3;        // struct_size @0x00461525 (MOV EDX,0x3)
constexpr uint32_t WORKBUF_BYTES          = 0x82480u; // per-cell pathfinder working state
constexpr uint32_t PARAMS_BLOCK_COUNT     = 1;
constexpr uint32_t PARAMS_BLOCK_SIZE      = 0x20u;

} // namespace

namespace detail {

// ---- llm_strat_pathfinder_init @0x004614f9 ---------------------------------------------------
void pathfinder_init(const sim_view &v, sim_store &own, const pathfinder_init_calls &c) {
    // 0x00461518-0x00461553: 100 job-result slots, each given its own 3000*3-byte packed
    // path-step buffer (utils_malloc_struct_array(3000, 3)). Checked and aborted INDIVIDUALLY --
    // one utils_abort(0) per failed slot inside the loop -- not a single post-loop check, matching
    // the original's re-read-then-test of the just-stored pointer at each iteration.
    for (int32_t i = 0; i < JOB_RESULT_SLOT_COUNT; ++i) {
        void *steps                          = c.malloc_struct_array(JOB_STEP_BUF_COUNT, JOB_STEP_BUF_ELEM_SIZE);
        own.path_job_result_at(i).path_steps = steps;
        if (own.path_job_result_at(i).path_steps == nullptr) {
            c.abort(0);
        }
    }

    // 0x00461555-0x00461585: the shared per-cell pathfinder working buffer -- allocated, stored into
    // map::g::general.pathfinder_workbuf, checked, then zero-filled over its full 0x82480 bytes.
    void *workbuf                = c.malloc_struct_array(WORKBUF_BYTES, 1);
    own.pathfinder_workbuf_mut() = workbuf;
    if (own.pathfinder_workbuf_mut() == nullptr) {
        c.abort(0);
    }
    c.fill_data(own.pathfinder_workbuf_mut(), WORKBUF_BYTES, 0);

    // 0x0046158f-0x004615ae: the packed 32-byte params block -- allocated, stored into
    // map::g::general.pathfinder_params, checked. sim_store::pathfinder_params_mut() hands back the
    // struct's OWN typed pointer rather than `void *&`, so the cast lives here, at the allocation,
    // and not at each of the three field writes below.
    auto *params = static_cast<mh::game::mh_llm_strat_pathfinder_params *>(
        c.malloc_struct_array(PARAMS_BLOCK_COUNT, PARAMS_BLOCK_SIZE));
    own.pathfinder_params_mut() = params;
    if (own.pathfinder_params_mut() == nullptr) {
        c.abort(0);
    }

    // 0x004615ae-0x004615d4: wire the params block's three fields.
    // +0x06 passable: `MOV dword ptr [EAX+0x6],0xb64bb0` -- 0xb64bb0 IS v.passable's address (see the
    // header's 2b address-derivation note); using the manifest-bound view member instead of the
    // literal reproduces the same store.
    params->passable = const_cast<uint8_t *>(v.passable);
    // +0x0e workbuf: `MOV dword ptr [EAX+0xe],EDX`, EDX just re-loaded from general.pathfinder_workbuf
    // -- the same pointer this function stored two steps above.
    params->workbuf = workbuf;
    // +0x0a job_result_table: `MOV dword ptr [EAX+0xa],0xae1958` -- element 0 of the job-result
    // table, reached through the accessor rather than the literal address.
    params->job_result_table = &own.path_job_result_at(0);
}

} // namespace detail

// ---- the public wrapper ------------------------------------------------------------------------

void pathfinder_init() {
    sim_state st = state();
    detail::pathfinder_init(st.read, st.own, live_pathfinder_init_calls());
}

} // namespace mh::sim
