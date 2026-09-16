//
// sim_pathfinder_init_selftest.cpp -- `simtest` offline oracle for llm_strat_pathfinder_init
// @0x004614f9 (sim/resid/sim_pathfinder_init.h/.cpp, RI-SIM sim_resid batch, SIM-RESID-IF third close).
//
// NO SHADOW SITE -- session-entry-only allocator (see the header banner's "NO SHADOW SITE" note).
// This file is the ONLY verification (proof:OFFLINE).
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_pathfinder_init_004614f9.asm) -- the .asm is the spec, NEVER the .c
// beside it (the .c has silently lied elsewhere in this project):
//   Loop i=0..99 (`CMP [ebp-0x18],0x64` @0x00461518, JL taken): utils_malloc_struct_array(3000, 3)
//     @0x0046152a/0x0046152f, stored into _G_LLM_STRAT_PATH_JOB_RESULT_TABLE[i].path_steps
//     @0x0046153c, RE-READ from that same slot and checked @0x00461542/0x00461548, utils_abort(0)
//     INSIDE the loop @0x0046154e if null -- the loop then falls through (JMP @0x00461553) to
//     increment/continue REGARDLESS of the abort call. This is checked and aborted INDIVIDUALLY per
//     iteration, not once after the loop -- a "single check after the loop" regression is exactly what
//     this oracle's T2 case below is for.
//   Workbuf: utils_malloc_struct_array(0x82480, 1) @0x0046155a/0x0046155f, stored into
//     map::g::general.pathfinder_workbuf @0x00461564, checked @0x00461569/0x0046156f, utils_abort(0)
//     @0x00461574 if null, THEN utils_fill_data(workbuf, 0x82480, 0) @0x00461579-0x00461585
//     UNCONDITIONALLY -- there is no branch around the fill, so it runs (and receives whatever pointer
//     got stored, even null) either way.
//   Params: utils_malloc_struct_array(1, 0x20) @0x0046158a/0x00461594, stored into
//     map::g::general.pathfinder_params @0x00461599, checked @0x0046159e/0x004615a5, utils_abort(0)
//     @0x004615a9 if null, THEN three unconditional field stores through that SAME pointer (again no
//     branch around them): +0x6 passable=0xb64bb0 @0x004615b3 (== map::g::passable's base, matching
//     sim_view::passable per the header's 2b derivation note), +0xe workbuf=<the just-stored workbuf
//     pointer> @0x004615c5, +0xa job_result_table=0xae1958 @0x004615cd (== &path_job_result[0]).
//
// A NOTE ON THE PARAMS-NULL CASE (T4 below), since it cannot be tested the same way as T2/T3: unlike
// the job-slot and workbuf failures, the params fall-through DEREFERENCES the (possibly-null) params
// pointer directly (`params->passable = ...`) rather than handing it to an outward call. In the real
// binary this is dead code -- utils_abort never returns (it compiles to `_exit`; see
// the "witness the doors" finding) -- so that dereference never
// actually executes on the null path in production. But an offline mock whose abort() is a plain
// recorder (needed so T2 can observe "the loop continued") would SEGFAULT this test binary if reused
// for T4. So T4 alone uses a SEPARATE abort callback (g_calls_throwing_abort) that records THEN throws
// a sentinel, modelling utils_abort's real non-return so the test unwinds instead of dereferencing
// null. Landing in the catch block IS the proof abort fired -- if the .cpp's null check were ever
// skipped, the mock would never be called, nothing would throw, and the case would instead crash on
// the dereference (a strictly stronger failure signal than a normal check failing).
//
#include "sim/resid/sim_pathfinder_init.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void *FP(uintptr_t v) { return reinterpret_cast<void *>(v); }

struct malloc_call {
    uint32_t count;
    uint32_t size;
};
struct fill_call {
    void    *ptr;
    uint32_t size;
    uint8_t  value;
};

std::vector<malloc_call> g_malloc_calls;
std::vector<fill_call>   g_fill_calls;
std::vector<int32_t>     g_abort_calls;

// Scripted per-call return values for malloc_struct_array, consumed IN CALL ORDER. A case that wants
// one particular allocation to fail overwrites that one slot with nullptr before calling.
std::vector<void *> g_malloc_responses;
size_t              g_malloc_response_idx = 0;

// THE PARAMS BLOCK IS A REAL OBJECT; the other 101 responses are fake pointers, and the asymmetry is
// forced by what the translation DOES with each. The 100 job-slot pointers and the workbuf pointer are
// only ever STORED (into path_job_result_at(i).path_steps and general.pathfinder_workbuf), so a fake
// value is both safe and better -- an identifiable constant rather than a real address that might
// coincidentally match something. The params pointer is DEREFERENCED three lines later
// (`params->passable = ...`, 0x004615ae-0x004615d4), so a fake one is a wild write. The first draft of
// this oracle used FP(0x40000000) here and ASan named it on run #1: "access-violation on unknown
// address 0x40000006" -- the +0x6 `passable` store -- at sim_pathfinder_init.cpp:71.
alignas(8) mh::game::mh_llm_strat_pathfinder_params g_params_block;

void reset_recorders() {
    // The params block is a file-static, so a later case would otherwise inherit the previous case's
    // three field values and a DROPPED store could pass by leftover.
    g_params_block = mh::game::mh_llm_strat_pathfinder_params{};
    g_malloc_calls.clear();
    g_fill_calls.clear();
    g_abort_calls.clear();
    g_malloc_responses.clear();
    g_malloc_response_idx = 0;
}

void *rec_malloc_struct_array(uint32_t count, uint32_t size) {
    g_malloc_calls.push_back({count, size});
    void *result = (g_malloc_response_idx < g_malloc_responses.size())
                       ? g_malloc_responses[g_malloc_response_idx]
                       : nullptr;
    ++g_malloc_response_idx;
    return result;
}

void *rec_fill_data(void *ptr, uint32_t size, uint8_t value) {
    g_fill_calls.push_back({ptr, size, value});
    return ptr;
}

void rec_abort(int32_t status) { g_abort_calls.push_back(status); }

// Only for T4 (the params-null case) -- see the file banner above. Throws immediately after
// recording so the caller unwinds instead of dereferencing the null params pointer the .cpp writes
// through next.
struct pathfinder_abort_signal {};
void rec_abort_throwing(int32_t status) {
    g_abort_calls.push_back(status);
    throw pathfinder_abort_signal{};
}

const pathfinder_init_calls g_calls = {
    &rec_malloc_struct_array,
    &rec_fill_data,
    &rec_abort,
};

const pathfinder_init_calls g_calls_throwing_abort = {
    &rec_malloc_struct_array,
    &rec_fill_data,
    &rec_abort_throwing,
};

// The 102-entry happy-path response script: 100 DISTINCT job-slot pointers (0x20000000 + i*0x100),
// then the workbuf pointer (0x30000000), then &g_params_block -- three separate ranges so a
// slot/workbuf/params pointer landing in the wrong place is an identifiable value, not a coincidental
// match.
void seed_all_success_responses() {
    for (int i = 0; i < 100; ++i) g_malloc_responses.push_back(FP(0x20000000u + (uintptr_t)i * 0x100u));
    g_malloc_responses.push_back(FP(0x30000000u)); // workbuf, index 100
    g_malloc_responses.push_back(&g_params_block); // params, index 101 -- REAL, it gets written through
}

} // namespace

void run_pathfinder_init_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the whole happy path: exact allocation count/order/sizes (100 job slots then workbuf then
    // params), every job slot's pointer lands in its OWN table entry (never aliased/swapped), the
    // workbuf is zero-filled through the STORED pointer over the full 0x82480 bytes, the params
    // block's three fields are wired from three DIFFERENT sources with DISTINCT pointer values so a
    // swap among them fails, and abort is never called.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_all_success_responses();

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::pathfinder_init(v, own, g_calls);

        ck_eq((uint32_t)g_malloc_calls.size(), 102u,
              "T1: utils_malloc_struct_array called exactly 102 times (100 job slots + workbuf + "
              "params), loop bound `CMP ...,0x64` @0x00461518");

        if (g_malloc_calls.size() >= 1) {
            ck_eq(g_malloc_calls[0].count, 3000u, "T1: alloc[0] (job slot 0, FIRST) count=3000, 0x0046152a");
            ck_eq(g_malloc_calls[0].size, 3u, "T1: alloc[0] (job slot 0, FIRST) size=3, 0x00461525");
        }
        if (g_malloc_calls.size() >= 100) {
            ck_eq(g_malloc_calls[99].count, 3000u, "T1: alloc[99] (job slot 99, LAST) count=3000, 0x0046152a");
            ck_eq(g_malloc_calls[99].size, 3u, "T1: alloc[99] (job slot 99, LAST) size=3, 0x00461525");
        }
        if (g_malloc_calls.size() >= 101) {
            ck_eq(g_malloc_calls[100].count, 0x82480u, "T1: alloc[100] (workbuf) count=0x82480, 0x0046155a");
            ck_eq(g_malloc_calls[100].size, 1u, "T1: alloc[100] (workbuf) size=1, 0x00461555");
        }
        if (g_malloc_calls.size() >= 102) {
            ck_eq(g_malloc_calls[101].count, 1u, "T1: alloc[101] (params) count=1, 0x0046158f");
            ck_eq(g_malloc_calls[101].size, 0x20u, "T1: alloc[101] (params) size=0x20, 0x0046158a");
        }

        for (int i = 0; i < 100; ++i) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "T1: path_job_result[%d].path_steps == alloc[%d]'s own distinct return, "
                          "store @0x0046153c",
                          i, i);
            ck(own.path_job_result_at(i).path_steps == FP(0x20000000u + (uintptr_t)i * 0x100u), msg);
        }

        ck(own.pathfinder_workbuf_mut() == FP(0x30000000u),
           "T1: general.pathfinder_workbuf == alloc[100]'s return, store @0x00461564");
        ck_eq((uint32_t)g_fill_calls.size(), 1u, "T1: utils_fill_data called exactly once, 0x00461585");
        if (!g_fill_calls.empty()) {
            ck(g_fill_calls[0].ptr == FP(0x30000000u),
               "T1: fill_data's ptr arg is the STORED workbuf pointer, 0x00461580 (MOV "
               "EAX,[general.pathfinder_workbuf]) -> CALL @0x00461585");
            ck_eq(g_fill_calls[0].size, 0x82480u,
                  "T1: fill_data's size arg is the full 0x82480 bytes, 0x0046157e/0x00461579");
            ck_eq((uint32_t)g_fill_calls[0].value, 0u, "T1: fill_data's fill byte is 0, 0x0046157e (XOR EDX,EDX)");
        }

        ck(own.pathfinder_params_mut() == &g_params_block,
           "T1: general.pathfinder_params == alloc[101]'s return, store @0x00461599");
        if (own.pathfinder_params_mut() != nullptr) {
            ck(static_cast<const void *>(own.pathfinder_params_mut()->passable) ==
                   static_cast<const void *>(v.passable),
               "T1: params+0x6 passable == v.passable (map::g::passable base, literal 0xb64bb0 in the "
               "asm), store @0x004615b3");
            ck(own.pathfinder_params_mut()->workbuf == FP(0x30000000u),
               "T1: params+0xe workbuf == the STORED workbuf pointer (not the params pointer or v.passable), "
               "store @0x004615c5");
            ck(own.pathfinder_params_mut()->job_result_table == &own.path_job_result_at(0),
               "T1: params+0xa job_result_table == &path_job_result[0] (table base, literal 0xae1958 "
               "in the asm), store @0x004615cd");
        }

        ck_eq((uint32_t)g_abort_calls.size(), 0u,
              "T1: utils_abort never called on the all-success path, 0x0046154e/0x00461574/0x004615a9");
    }

    // =================================================================================================
    // T2 -- ONE job slot's allocation fails (index 42: neither first nor last): abort fires exactly
    // once, for THAT slot only, and the loop demonstrably continues -- every other slot still gets its
    // own correct pointer, and the function still runs the workbuf/params steps afterward. This is the
    // check a "single check after the loop instead of one per iteration" regression fails.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_all_success_responses();
        g_malloc_responses[42] = nullptr; // job slot 42's allocation fails

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::pathfinder_init(v, own, g_calls);

        ck_eq((uint32_t)g_malloc_calls.size(), 102u,
              "T2: all 102 allocations still attempted despite slot 42's failure -- the loop did not "
              "stop, 0x00461518-0x00461553");
        ck(own.path_job_result_at(42).path_steps == nullptr,
           "T2: path_job_result[42].path_steps is null (the failed alloc's return, stored as-is), "
           "store @0x0046153c");
        for (int i = 0; i < 100; ++i) {
            if (i == 42) continue;
            char msg[140];
            std::snprintf(msg, sizeof(msg),
                          "T2: path_job_result[%d].path_steps unaffected by slot 42's failure -- own "
                          "distinct pointer, 0x0046153c",
                          i);
            ck(own.path_job_result_at(i).path_steps == FP(0x20000000u + (uintptr_t)i * 0x100u), msg);
        }
        ck_eq((uint32_t)g_abort_calls.size(), 1u,
              "T2: utils_abort called EXACTLY ONCE -- for slot 42 alone, not once per remaining "
              "iteration and not a single post-loop check, 0x0046154e");
        if (!g_abort_calls.empty())
            ck_eq((uint32_t)g_abort_calls[0], 0u, "T2: abort's status arg is 0, 0x0046154c (XOR EAX,EAX)");

        ck(own.pathfinder_workbuf_mut() == FP(0x30000000u),
           "T2: workbuf allocation still runs and stores normally after slot 42's abort, 0x00461564");
        ck(own.pathfinder_params_mut() == &g_params_block,
           "T2: params allocation still runs and stores normally after slot 42's abort, 0x00461599");
    }

    // =================================================================================================
    // T3 -- the workbuf allocation itself fails: abort fires once, and the fill_data call right after
    // is NOT skipped -- there is no branch around it (0x00461569-0x00461585 falls straight through
    // into LAB_00461579) -- so fill_data runs with whatever (null) pointer got stored. Safe to execute
    // here because fill_data is an OUTWARD call, not a local dereference (contrast T4 below).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_all_success_responses();
        g_malloc_responses[100] = nullptr; // workbuf allocation fails

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::pathfinder_init(v, own, g_calls);

        ck_eq((uint32_t)g_malloc_calls.size(), 102u,
              "T3: params allocation still attempted after the workbuf failure, 0x0046158f");
        ck(own.pathfinder_workbuf_mut() == nullptr,
           "T3: general.pathfinder_workbuf stored as null (the failed alloc's return), 0x00461564");
        ck_eq((uint32_t)g_abort_calls.size(), 1u,
              "T3: utils_abort called exactly once, for the workbuf failure, 0x00461574");
        if (!g_abort_calls.empty())
            ck_eq((uint32_t)g_abort_calls[0], 0u, "T3: abort's status arg is 0, 0x00461572 (XOR EAX,EAX)");

        ck_eq((uint32_t)g_fill_calls.size(), 1u,
              "T3: utils_fill_data is STILL called once even though workbuf is null -- no branch "
              "guards it, 0x00461579-0x00461585");
        if (!g_fill_calls.empty()) {
            ck(g_fill_calls[0].ptr == nullptr,
               "T3: fill_data's ptr arg is the null workbuf pointer, passed through as-is, 0x00461580");
            ck_eq(g_fill_calls[0].size, 0x82480u, "T3: fill_data's size arg is still 0x82480, 0x0046157e");
            ck_eq((uint32_t)g_fill_calls[0].value, 0u, "T3: fill_data's fill byte is still 0, 0x0046157e");
        }

        ck(own.pathfinder_params_mut() == &g_params_block,
           "T3: params allocation still runs normally after the workbuf failure, 0x00461599");
        if (own.pathfinder_params_mut() != nullptr) {
            ck(own.pathfinder_params_mut()->workbuf == nullptr,
               "T3: params+0xe workbuf field is null too (propagated from the failed workbuf alloc, "
               "not silently defaulted), store @0x004615c5");
            ck(static_cast<const void *>(own.pathfinder_params_mut()->passable) ==
                   static_cast<const void *>(v.passable),
               "T3: params+0x6 passable is unaffected by the workbuf failure, store @0x004615b3");
        }
    }

    // =================================================================================================
    // T4 -- the params allocation itself fails. See the file banner: this path DEREFERENCES the null
    // params pointer directly if execution continues, so this case alone uses
    // g_calls_throwing_abort -- reaching the catch block is the proof abort fired.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_all_success_responses();
        g_malloc_responses[101] = nullptr; // params allocation fails

        sim_view  v      = fx.view();
        sim_store own    = fx.store();
        bool      caught = false;
        try {
            detail::pathfinder_init(v, own, g_calls_throwing_abort);
        } catch (const pathfinder_abort_signal &) {
            caught = true;
        }
        ck(caught,
           "T4: utils_abort fired when the params allocation returned null (the mock's abort throws to "
           "unwind before the null-pointer field writes at 0x004615ae-0x004615d4 that follow it), "
           "0x004615a9");
        ck_eq((uint32_t)g_abort_calls.size(), 1u, "T4: utils_abort called exactly once, 0x004615a9");
        if (!g_abort_calls.empty())
            ck_eq((uint32_t)g_abort_calls[0], 0u, "T4: abort's status arg is 0, 0x004615a7 (XOR EAX,EAX)");

        ck_eq((uint32_t)g_malloc_calls.size(), 102u,
              "T4: all 102 allocations still attempted -- the params failure is the LAST one, "
              "0x0046158f");
        ck(own.pathfinder_params_mut() == nullptr,
           "T4: general.pathfinder_params stored as null before the check/abort, 0x00461599");

        ck(own.pathfinder_workbuf_mut() == FP(0x30000000u),
           "T4: the workbuf step still completed normally before the params failure, 0x00461564");
        ck_eq((uint32_t)g_fill_calls.size(), 1u,
              "T4: fill_data still ran once for the workbuf, before the params failure, 0x00461585");
        for (int i = 0; i < 100; ++i) {
            char msg[140];
            std::snprintf(msg, sizeof(msg),
                          "T4: path_job_result[%d].path_steps unaffected, completed before the params "
                          "step, 0x0046153c",
                          i);
            ck(own.path_job_result_at(i).path_steps == FP(0x20000000u + (uintptr_t)i * 0x100u), msg);
        }
    }
}

} // namespace mh::sim::test
