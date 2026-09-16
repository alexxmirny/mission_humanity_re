//
// lib_trans_selftest.cpp -- `net_selftest.exe libtranstest`: the lib_trans domain's offline oracle
// (LT0, 2026-09-02). See tools/data/migration/lib_trans.json `oracle` and the libtrans closure.
//
// WHAT THIS FILE IS AT LT0 (before any batch has translated a body): the EXPECTATION side of the
// oracle, non-vacuous by construction. Three layers:
//
//   1. A REFERENCE MODEL of the deterministic RNG family's arithmetic, written from the
//      DISASSEMBLY constants (state' = ROR16(state + 0x9248, 3); draw = lo + state'*(hi-lo)/0xffff;
//      rand_state_advance's normalized return = (double)(int)state' / 65535.0 -- the value of
//      _G_LLM_STRAT_RNG_NORM_DIVISOR @0x00603f0c, read out of the image).
//   2. GOLDEN VECTORS as literals -- computed once from the recurrence and EMBEDDED, so a mutation
//      of the model's constants (or, once LT1A lands, a wrong translation) disagrees with stored
//      numbers, not with itself. The mutation test that armed this file: 0x9248 -> 0x9249 in
//      lcg_step() fails the vector cases BY NAME; reverted.
//   3. The verified body we ALREADY own: mh::sim::detail::rng_next (SIM1F, disasm-verified,
//      shadow-armed) is driven over the fixture store and compared against the SAME vectors --
//      proving the vectors describe the binary's arithmetic, not merely this file's model. That is
//      what makes layer 2 non-circular.
//
// BATCH-A CONTRACT CASES: the six batch-A rows are thin wrappers (per their exported decompiles):
//   llm_rand_below(n)        == rng_next(0, 0, n)          (ch0, the gameplay stream)
//   llm_rand_below_fx(n)     == rng_next(1, 0, n)          (ch1, the cosmetic stream)
//   llm_strat_rng_seed_ch0(v) == rng_seed_channel(0, v)    (v & 0xffff -- the preserve-bug)
//   llm_strat_rng_seed_ch1(v) == rng_seed_channel(1, v)
//   llm_rand_below_ai(n)      -- ch2 via llm_rand_prng_tick_slot (settle the tick-slot shape at LT1A)
//   llm_rand_state_advance(i) -- same recurrence over _G_LLM_STRAT_RNG_STATE[i], double return
// The contract cases below exercise those equalities through the reference model + our
// rng_next/rng_seed_channel bodies. When LT1A lands its translations, each wrapper's own case is
// one line: call the mh::sim body and compare against the same expectation function. Do NOT edit
// the vectors to make a translation pass -- the vectors are the spec (the orders_issue golden rule).
//
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sim/sim_rng_next.h"
#include "sim/resid/sim_rng_seed_channel.h"
#include "sim/libtrans/sim_lt_available_projects_clear.h"
#include "sim/libtrans/sim_lt_map_region_pool.h"
#include "sim/libtrans/sim_lt_placement_offsets.h"
#include "sim/libtrans/sim_lt_rng_draws.h"
#include "sim/libtrans/sim_lt_rng_raw_step.h"
#include "sim/libtrans/sim_lt_rng_seed.h"
#include "lockstep/lt_chat_ally_mask.h"
#include "lockstep/lt_player_by_side_id.h"
#include "lockstep/turn_engine.h"
#include "sim/libtrans/sim_lt_diplomacy.h"
#include "sim/libtrans/sim_lt_invasion_alert.h"
#include "sim/libtrans/sim_lt_math.h"
#include "sim/libtrans/sim_lt_progress_finalize.h"
#include "sim/libtrans/sim_lt_scan_masked_table.h"
#include "sim/libtrans/sim_lt_str.h"
#include "sim/libtrans/sim_lt_available_remove.h"
#include "sim/libtrans/sim_lt_bldg_cell_grid.h"
#include "sim/libtrans/sim_lt_frame.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

int lt_checks = 0;
int lt_fails  = 0;

void check(bool ok, const char *name) {
    ++lt_checks;
    if (!ok) {
        ++lt_fails;
        printf("FAIL: %s\n", name);
    }
}

// ---- layer 1: the reference model (disasm constants -- see the banner) --------------------------
uint16_t lcg_step(uint16_t s) {
    const uint16_t t = static_cast<uint16_t>(s + 0x9248);
    return static_cast<uint16_t>((t >> 3) | (t << 13));
}
int32_t expected_draw(uint16_t s_new, int32_t lo, int32_t hi) {
    return lo + static_cast<int32_t>(s_new) * (hi - lo) / 0xffff;
}
double expected_advance_return(uint16_t s_new) { return static_cast<double>(s_new) / 65535.0; }

// ---- layer 2: the golden vectors (literals; regenerate ONLY from the disasm, never from a body) --
struct vec {
    uint16_t seed;
    uint16_t states[6];
};
const vec VECTORS[] = {
    {0x0000, {0x1249, 0x3492, 0x58db, 0x7d64, 0x81f5, 0xa287}},
    {0x1234, {0x948f, 0xe4da, 0x4ee4, 0x9c25, 0xa5cd, 0xa702}},
    {0xffff, {0xf248, 0x1092, 0x545b, 0x7cd4, 0x81e3, 0x6285}},
};
struct draw_vec {
    int32_t  lo, hi, ret;
    uint16_t state;
};
const draw_vec DRAWS_FROM_SEED0[] = {
    // consecutive draws on one channel, seed 0
    {0, 100, 7, 0x1249},
    {0, 0x10000, 13458, 0x3492},
    {5, 10, 6, 0x58db},
    {0, 1, 0, 0x7d64},
};

void t1_model_matches_vectors() {
    for (const vec &v : VECTORS) {
        uint16_t s = v.seed;
        for (int i = 0; i < 6; ++i) {
            s = lcg_step(s);
            char name[96];
            snprintf(name, sizeof name, "t1 lcg vector seed=0x%04x step=%d (model vs literal)",
                     v.seed, i);
            check(s == v.states[i], name);
        }
    }
}

void t2_our_rng_next_matches_vectors(sim_fixture &fx) {
    // layer 3: the already-verified body over the fixture store agrees with the literals.
    for (const vec &v : VECTORS) {
        fx.reset();
        sim_store s = fx.store();
        detail::rng_seed_channel(s, 0, v.seed);
        for (int i = 0; i < 6; ++i) {
            (void)detail::rng_next(s, 0, 0, 0xffff);
            char name[96];
            snprintf(name, sizeof name, "t2 rng_next state trajectory seed=0x%04x step=%d", v.seed, i);
            check(fx.rng_state[0] == v.states[i], name);
        }
    }
    fx.reset();
    sim_store s = fx.store();
    detail::rng_seed_channel(s, 0, 0);
    for (const draw_vec &dv : DRAWS_FROM_SEED0) {
        const int32_t got = detail::rng_next(s, 0, dv.lo, dv.hi);
        char          name[96];
        snprintf(name, sizeof name, "t2 rng_next draw lo=%d hi=%d (ret + state)", dv.lo, dv.hi);
        check(got == dv.ret && fx.rng_state[0] == dv.state, name);
        check(got == expected_draw(dv.state, dv.lo, dv.hi), "t2 expected_draw agrees with body");
    }
}

void t3_seed_mask_and_channel_isolation(sim_fixture &fx) {
    fx.reset();
    sim_store s = fx.store();
    // the preserve-bug: high 16 bits of the seed are discarded unconditionally.
    detail::rng_seed_channel(s, 0, 0xABCD1234u);
    check(fx.rng_state[0] == 0x1234, "t3 seed masks to 16 bits (preserve-bug)");
    // channel isolation: advancing ch0 leaves ch1's state alone (the two-streams design the
    // rand_below/rand_below_fx split exists for).
    detail::rng_seed_channel(s, 1, 0xffffu);
    (void)detail::rng_next(s, 0, 0, 100);
    check(fx.rng_state[1] == 0xffff, "t3 ch0 draw does not touch ch1");
    (void)detail::rng_next(s, 1, 0, 100);
    check(fx.rng_state[1] == 0xf248, "t3 ch1 draw follows the same recurrence (vector seed=0xffff)");
}

void t4_batch_a_wrapper_contracts(sim_fixture &fx) {
    // The batch-A wrapper CONTRACTS, expressed executably before the bodies exist: rand_below is
    // rng_next(0,0,n), rand_below_fx is rng_next(1,0,n), seed_ch0/1 are rng_seed_channel(0/1, v).
    // Each is exercised here through the verified rng_next/rng_seed_channel bodies; LT1A's
    // translations must match these same calls (its cases replace the right-hand sides with the
    // mh::sim wrapper bodies and keep the expectations).
    fx.reset();
    sim_store s = fx.store();
    detail::rng_seed_channel(s, 0, 0);                    // == the seed_ch0(0) contract
    const int32_t below = detail::rng_next(s, 0, 0, 100); // == the rand_below(100) contract
    check(below == 7, "t4 rand_below(100) contract from seed 0 (vector)");
    detail::rng_seed_channel(s, 1, 0);                       // == the seed_ch1(0) contract
    const int32_t below_fx = detail::rng_next(s, 1, 0, 100); // == rand_below_fx(100)
    check(below_fx == 7, "t4 rand_below_fx(100) contract from seed 0 (same recurrence, own channel)");
    check(fx.rng_state[0] == 0x1249 && fx.rng_state[1] == 0x1249,
          "t4 both channels advanced exactly once");
}

void t5_rand_state_advance_reference() {
    // llm_rand_state_advance's return normalization: (double)(int)state' / 65535.0
    // (_G_LLM_STRAT_RNG_NORM_DIVISOR @0x00603f0c = 65535.0, read from the image). Same recurrence.
    uint16_t     s          = 0;
    const double expected[] = {0.07142748149843595, 0.20535591668574044, 0.3470969710841535};
    for (int i = 0; i < 3; ++i) {
        s = lcg_step(s);
        char name[96];
        snprintf(name, sizeof name, "t5 rand_state_advance normalized return, step %d", i);
        check(std::fabs(expected_advance_return(s) - expected[i]) < 1e-15, name);
    }
}

// ---- t6: the LT1A translations against the SAME expectations (added when the batch landed) ------
// Per the LT0 banner: each wrapper's own case is one line -- call the mh::sim detail body over the
// fixture store and compare against the expectation layer above. The vectors are NOT re-derived.
void t6_lt1a_translations(sim_fixture &fx) {
    fx.reset();
    sim_store s = fx.store();
    // a2: the seed wrappers, incl. the callee-owned mask (t3's expectation).
    detail::rng_seed_ch0(s, 0xABCD0000u);
    check(fx.rng_state[0] == 0x0000, "t6 rng_seed_ch0 passes 32 bits through; callee masks");
    detail::rng_seed_ch0(s, 0);
    detail::rng_seed_ch1(s, 0);
    // a1: the draw wrappers reproduce t4's contracts from seed 0.
    check(detail::rand_below(s, 100) == 7, "t6 rand_below(100) == 7 (t4's vector)");
    check(detail::rand_below_fx(s, 100) == 7u, "t6 rand_below_fx(100) == 7 (own channel)");
    check(fx.rng_state[0] == 0x1249 && fx.rng_state[1] == 0x1249,
          "t6 each draw advanced its own channel exactly once");
    // a3: the adopted tick step IS lcg_step over the slot.
    fx.rng_state[2] = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t got = detail::rng_tick_slot(s, 2);
        check(got == VECTORS[0].states[i] && fx.rng_state[2] == got,
              "t6 rng_tick_slot follows the seed-0 vector and stores what it returns");
    }
    // a3: rand_below_ai scales >>16, not /0xffff -- from a fresh ch2 seed, first draw:
    // state' = 0x1249 (4681); (4681*100)>>16 = 7, but (4681*0x10000)>>16 = 4681 shows the shift.
    fx.rng_state[2] = 0;
    check(detail::rand_below_ai(s, 100) == 7, "t6 rand_below_ai(100) from seed 0");
    fx.rng_state[2] = 0;
    check(detail::rand_below_ai(s, 0x10000) == 0x1249,
          "t6 rand_below_ai(0x10000) returns the raw state' -- the >>16 scaling, not /0xffff");
    // a3: rand_state_advance reproduces t5's normalized returns on an arbitrary slot (3).
    fx.rng_state[3]         = 0;
    const double expected[] = {0.07142748149843595, 0.20535591668574044, 0.3470969710841535};
    for (int i = 0; i < 3; ++i) {
        const double got = detail::rand_state_advance(fx.view(), s, 3);
        char         name[96];
        snprintf(name, sizeof name, "t6 rand_state_advance step %d matches the t5 vector", i);
        check(std::fabs(got - expected[i]) < 1e-15, name);
    }
}

// ---- t7: the map-region pool trio (LT1C c1 -- UNARMABLE, so this IS its runnable evidence) ------
// Contract read off context_C.md section 1's instruction-cited table (each clause carries the asm
// address there): LIFO free list sharing `next` with the active list; counter starts at 1 and index
// 0 is never allocated; a recycled node keeps its stale route/neighbor fields AND its index;
// region_free's tail runs UNCONDITIONALLY (a node never found on the active list still gets
// BY_INDEX[index]=0, the free-list push, and the count decrement); G_LAST_MAP_INDEX is a live-node
// COUNT. The allocator is substituted (the _calls seam exists precisely so this suite can run).
int lt_alloc_count = 0, lt_free_count = 0, lt_arena_next = 0;
alignas(8) uint8_t lt_arena[4][0x420];
void *lt_test_malloc(uint32_t size) {
    check(size == 0x420, "t7 allocator asked for exactly one node (0x420)");
    ++lt_alloc_count;
    return lt_arena[lt_arena_next++];
}
void lt_test_free(void *) { ++lt_free_count; }

void t7_map_region_pool(sim_fixture &fx) {
    fx.reset();
    lt_alloc_count = lt_free_count = lt_arena_next = 0;
    sim_store                      s               = fx.store();
    const lt_map_region_pool_calls c{&lt_test_malloc, &lt_test_free};

    // reset over an empty pool: counter := 1 (index 0 never allocated), count := 0, no frees.
    detail::pool_reset(s, c);
    check(fx.region_alloc_counter == 1 && fx.last_map_index == 0 && lt_free_count == 0,
          "t7 pool_reset(empty): counter=1, count=0, nothing freed");

    // malloc path: index from counter++, five fields zeroed, pushed onto the active head, indexed.
    llm_map_region *n = detail::get_next_block(s, c);
    check(lt_alloc_count == 1 && n != nullptr, "t7 first block came from the allocator");
    check(n->index == 1 && fx.region_alloc_counter == 2, "t7 index=counter++ (so index 1 first)");
    check(fx.region_list_head == n && fx.region_by_index[1] == n && fx.last_map_index == 1,
          "t7 alloc pushes the active head, indexes BY_INDEX, ++count");
    check(n->x == 0 && n->y == 0 && n->cell_count == 0 && n->neighbor_count == 0 && n->prev == nullptr,
          "t7 exactly the five documented fields are cleared");

    llm_map_region *m = detail::get_next_block(s, c);
    check(m->index == 2 && m->next == n && fx.region_list_head == m && fx.last_map_index == 2,
          "t7 second block links ahead of the first");

    // free the head: unlink, BY_INDEX slot zeroed, LIFO push onto the free list, --count.
    detail::region_free(s, m);
    check(fx.region_list_head == n && fx.region_by_index[2] == nullptr &&
              fx.region_pool_free_head == m && fx.last_map_index == 1,
          "t7 region_free(head): unlinked, de-indexed, on the free list, --count");

    // recycle: pop from the free list -- NO malloc, index UNTOUCHED, stale fields KEPT (adding a
    // clear is a divergence -- recompute_adjacency owns rebuilding them), five fields re-zeroed.
    m->route_mark     = 0x77;
    llm_map_region *p = detail::get_next_block(s, c);
    check(p == m && lt_alloc_count == 2, "t7 recycle pops the free list, no new allocation");
    check(p->index == 2 && p->route_mark == 0x77 && p->cell_count == 0,
          "t7 recycled node keeps its index and stale route state; zeroed fields re-zeroed");
    check(fx.region_by_index[2] == p && fx.last_map_index == 2, "t7 recycle re-indexes and ++count");

    // the UNCONDITIONAL tail: a node that is NOT on the active list still gets the full teardown.
    llm_map_region detached{};
    detached.index          = 3;
    fx.region_by_index[3]   = &detached;
    const int32_t count_was = fx.last_map_index;
    detail::region_free(s, &detached);
    check(fx.region_by_index[3] == nullptr && fx.region_pool_free_head == &detached &&
              fx.last_map_index == count_was - 1,
          "t7 region_free(not-on-list): BY_INDEX zeroed, free-pushed, --count anyway (no guard)");

    // full reset: drain the active list through region_free, then free EVERY free-list node.
    lt_free_count = 0;
    detail::pool_reset(s, c);
    check(fx.region_list_head == nullptr && fx.region_pool_free_head == nullptr,
          "t7 pool_reset drains both lists");
    check(lt_free_count == 3, "t7 pool_reset frees every node that ended on the free list (p, n, detached)");
    check(fx.region_alloc_counter == 1 && fx.last_map_index == 0, "t7 pool_reset: counter=1, count=0");
}

// ---- t8: game_ClearAvailableProjects (LT1B b4 -- site vacuous by region flags, offline-only) ----
uint32_t lt_fill_calls = 0, lt_fill_size = 0;
uint8_t  lt_fill_value = 0xee;
void    *lt_test_fill(void *ptr, uint32_t size, uint8_t v) {
    ++lt_fill_calls;
    lt_fill_size  = size;
    lt_fill_value = v;
    memset(ptr, v, size);
    return ptr;
}

void t8_available_projects_clear(sim_fixture &fx) {
    fx.reset();
    sim_store s                      = fx.store();
    s.available_projects_base()[0]   = 0x7f7f7f7f;
    s.available_projects_base()[799] = 0x7f7f7f7f;
    lt_fill_calls                    = 0;
    const available_projects_clear_calls c{&lt_test_fill};
    detail::available_projects_clear(s, c);
    check(lt_fill_calls == 1 && lt_fill_size == 0xc80 && lt_fill_value == 0,
          "t8 one whole-region fill of 0xc80 bytes with 0 (0x0041c046's EBX/EDX)");
    check(s.available_projects_base()[0] == 0 && s.available_projects_base()[799] == 0,
          "t8 both ends of the 3200-byte region cleared");
}

// ---- t9: the placement pair (LT1E e2 -- writes only caller memory, site-less by construction) ---
void t9_placement_offsets(sim_fixture &fx) {
    fx.reset();
    // squad_placement_offset_lookup: cell = table[slot*0x30 + soldier_count*8], out_a=+0, out_b=+4.
    fx.squad_placement_offset_table[2 * 0x30 + 3 * 8 + 0] = 0xAB;
    fx.squad_placement_offset_table[2 * 0x30 + 3 * 8 + 4] = 0xCD;
    char a = 0, b = 0;
    detail::squad_placement_offset_lookup(fx.view(), 3, 2, &a, &b);
    check(static_cast<uint8_t>(a) == 0xAB && static_cast<uint8_t>(b) == 0xCD,
          "t9 lookup reads bytes +0/+4 of cell [slot*0x30 + count*8]");
    // facing_step_apply: dx==-1 -> -0x20, dx==+1 -> +0x20, byte-width (wraps mod 256).
    fx.facing_step_offset[3] = {-1, 1};
    char x = 0x40, y = 0x40;
    detail::facing_step_apply(fx.view(), &x, &y, 3);
    check(static_cast<uint8_t>(x) == 0x20 && static_cast<uint8_t>(y) == 0x60,
          "t9 facing 3 {-1,+1}: x -= 0x20, y += 0x20");
    x = 0x10;
    detail::facing_step_apply(fx.view(), &x, &y, 3);
    check(static_cast<uint8_t>(x) == 0xF0, "t9 the store is byte-width -- 0x10 - 0x20 wraps to 0xF0");
}


uint32_t lt_rm_bldg_calls = 0;
uint32_t lt_rm_proj_calls = 0, lt_rm_proj_player = 99, lt_rm_proj_index = 99;
void     lt_stub_rm_bldg(uint32_t, int32_t) { ++lt_rm_bldg_calls; }
void     lt_stub_rm_proj(uint32_t player, uint32_t idx) {
    ++lt_rm_proj_calls;
    lt_rm_proj_player = player;
    lt_rm_proj_index  = idx;
}

void t10_progress_finalize_acquire(sim_fixture &fx) {
    fx.reset();
    lt_rm_bldg_calls = lt_rm_proj_calls = 0;
    sim_store s                         = fx.store();
    fx.player_side                      = 1; // player 2 != side 1, so the SetEvent gate never fires and the default
                                             // c_set_event's mh::call pointers are never invoked offline.
    const lt_progress_finalize_calls c{&lt_stub_rm_bldg, &lt_stub_rm_proj};
    // The SYSTEM arm's recursion, depth > 1 ASSERTED: invention 10 (type SYSTEM, index 7) owns
    // planets 1 and 3 (system_index 7 -- the loop SEEDS AT PLANET 1, slot 0 is never a candidate);
    // planet 1's invention 20 is a PROJECT (index 77 -> the removal stub), planet 3's invention 21
    // is an UPGRADE (the no-op arm -- still gets the two record writes).
    fx.cfg_inventions[10].type        = 5;
    fx.cfg_inventions[10].index       = 7;
    fx.cfg_planets[1].system_index    = 7;
    fx.cfg_planets[1].invention_index = 20;
    fx.cfg_planets[3].system_index    = 7;
    fx.cfg_planets[3].invention_index = 21;
    fx.cfg_inventions[20].type        = 3;
    fx.cfg_inventions[20].index       = 77;
    fx.cfg_inventions[21].type        = 6;
    detail::progress_finalize_acquire(fx.view(), s, c, 2, 10);
    check(s.progress_at(2, 10).f3 && !s.progress_at(2, 10).available,
          "t10 outer record: f3 set, available cleared (the two pre-switch writes)");
    check(s.progress_at(2, 20).f3 && s.progress_at(2, 21).f3,
          "t10 recursion reached BOTH owned planets' inventions (depth > 1, asserted not assumed)");
    check(lt_rm_proj_calls == 1 && lt_rm_proj_player == 2 && lt_rm_proj_index == 77,
          "t10 the recursed PROJECT arm fired the removal stub with (player, Progress.index)");
    check(lt_rm_bldg_calls == 0, "t10 no building removal anywhere on this input");
    // The negative arm: an unknown/no-case type gets the two writes and NOTHING else.
    fx.reset();
    fx.player_side             = 1;
    fx.cfg_inventions[30].type = 6;
    lt_rm_bldg_calls = lt_rm_proj_calls = 0;
    detail::progress_finalize_acquire(fx.view(), s, c, 2, 30);
    check(s.progress_at(2, 30).f3 && !s.progress_at(2, 30).available && lt_rm_proj_calls == 0 &&
              lt_rm_bldg_calls == 0,
          "t10 negative arm: unknown type = the two writes only, no calls");
}

// ---- t11: scan_masked_table_for_empty_cell (LT1C c3 -- the placement-fit matrix) ----------------
void t11_scan_masked_table(sim_fixture &fx) {
    fx.reset();
    // The grid is indexed by the PACKED cursor (x<<8)|y AND'ed with the wrap mask -- a 256-stride
    // virtual plane, so a 4x4 torus needs bytes up to index 0x303.
    static uint8_t grid[0x400];
    uint8_t        fp[4];
    memset(fp, 0xff, sizeof fp);
    uint32_t wrap_mask = 0xAAAA0000u; // bits 16-31 must SURVIVE (the preserve discipline)
    memset(grid, 1, sizeof grid);
    int32_t r = detail::scan_masked_table_for_empty_cell(&wrap_mask, grid, 4, 4, fp, 2, 2, 0, 0);
    check(r == 1, "t11 all-passable 2x2 footprint at (0,0) FITS (returns 1 -- the inverted name)");
    check(wrap_mask == 0xAAAA0303u,
          "t11 wrap mask: low byte height-1, HIGH byte width-1, bits 16-31 preserved");
    grid[0] = 6;
    check(detail::scan_masked_table_for_empty_cell(&wrap_mask, grid, 4, 4, fp, 2, 2, 0, 0) == 0,
          "t11 REFUSAL: a tested cell reading 6 does not fit (returns 0)");
    grid[0] = 0;
    check(detail::scan_masked_table_for_empty_cell(&wrap_mask, grid, 4, 4, fp, 2, 2, 0, 0) == 0,
          "t11 REFUSAL: a tested cell reading 0 does not fit either");
}

// ---- t12: player_by_side_id (LT1D d8 -- crafted-message cases: hit, miss, duplicate) ------------
void t12_player_by_side_id() {
    static mh::lockstep::player_profile profs[8]{};
    mh::lockstep::engine_state          st{};
    st.players       = profs;
    profs[2].side_id = 7;
    profs[3].side_id = 42;
    profs[5].side_id = 7; // duplicate of slot 2 -- the FIRST hit must win
    check(mh::lockstep::detail::player_by_side_id(st, 42) == 3, "t12 hit: side 42 -> slot 3");
    check(mh::lockstep::detail::player_by_side_id(st, 999) == -1, "t12 miss: unknown side -> -1");
    check(mh::lockstep::detail::player_by_side_id(st, 7) == 2,
          "t12 duplicate sides: the LOWEST slot wins (scan order)");
}

// ---- t13: the diplomacy inits (LT1D d2 -- the WHOLE relation table asserted) --------------------
wchar_t lt_wide_buf[64];
void   *lt_stub_a2w(char *) { return lt_wide_buf; }
int32_t lt_stub_visis(void *, const wchar_t *, int32_t, const wchar_t *, int32_t, const wchar_t *) {
    return 0;
}
int32_t lt_stub_vs(void *, const wchar_t *, const wchar_t *) { return 0; }
int32_t lt_swap_calls = 0;
int32_t lt_stub_swap(int32_t, int32_t, int32_t) {
    ++lt_swap_calls;
    return 0;
}
int     lt_rebuild_calls = 0;
void    lt_stub_rebuild() { ++lt_rebuild_calls; }
int     lt_chat_calls = 0;
uint8_t lt_chat_ids[8];
void    lt_stub_chat_add(uint8_t id) { lt_chat_ids[lt_chat_calls++] = id; }

void t13_diplomacy_inits(sim_fixture &fx) {
    const diplomacy_set_relation_calls c_sr{&lt_stub_a2w, &lt_stub_visis, &lt_stub_vs, &lt_stub_swap,
                                            &lt_stub_rebuild};
    const lt_diplomacy_calls           c_d{&lt_stub_chat_add};
    // MULTIPLAYER: all 8 slots enabled; 0/1 human (bit2), 2/3 AI (bit3), 4..7 neither.
    fx.reset();
    lt_chat_calls = lt_rebuild_calls = 0;
    sim_store s                      = fx.store();
    for (int i = 0; i < 8; ++i) fx.profiles[i].status_flags = 0x01;
    fx.profiles[0].status_flags |= 0x04;
    fx.profiles[1].status_flags |= 0x04;
    fx.profiles[2].status_flags |= 0x08;
    fx.profiles[3].status_flags |= 0x08;
    detail::diplomacy_init_multiplayer(fx.view(), s, c_d, c_sr);
    check(lt_chat_calls == 2 && lt_chat_ids[0] == 0 && lt_chat_ids[1] == 1,
          "t13 mp: exactly the enabled HUMAN slots became chat targets");
    check(lt_rebuild_calls == 64, "t13 mp: all 64 ordered pairs went through set_relation");
    bool table_ok = true;
    for (uint32_t i = 0; i < 8 && table_ok; ++i)
        for (uint32_t j = 0; j < 8; ++j) {
            const bool both_ai = i != j && (fx.profiles[i].status_flags & 8) != 0 &&
                                 (fx.profiles[j].status_flags & 8) != 0;
            const uint8_t expected = (i == j || both_ai) ? 1 : 2;
            if (s.player_relation_at(i, j) != expected) {
                table_ok = false;
                break;
            }
        }
    check(table_ok, "t13 mp: the WHOLE 8x8 relation table matches the literal ally rule");
    // SKIRMISH: PlayerSide 1 hostile to everyone; everyone else mutually allied.
    fx.reset();
    s = fx.store();
    for (int i = 0; i < 8; ++i) fx.profiles[i].status_flags = 0x01;
    fx.player_side = 1;
    detail::diplomacy_init_skirmish(fx.view(), s, c_sr);
    table_ok = true;
    for (uint32_t i = 0; i < 8 && table_ok; ++i)
        for (uint32_t j = 0; j < 8; ++j) {
            const uint8_t expected = (i == j) ? 1 : (i != 1 && j != 1) ? 1
                                                                       : 2;
            if (s.player_relation_at(i, j) != expected) {
                table_ok = false;
                break;
            }
        }
    check(table_ok, "t13 skirmish: the WHOLE table -- PlayerSide hostile, others mutually allied");
}

// ---- t15: the LT1B adopted Remove twins (site-less like t8; the Insert seam stubbed) ------------
int32_t lt_ins_calls = 0, lt_ins_size = 0, lt_ins_item = 0, lt_ins_new = 0;
void   *lt_ins_arr = nullptr;
void    lt_stub_insert(int32_t *arr, int32_t size, uint32_t, int32_t item, int32_t new_item) {
    ++lt_ins_calls;
    lt_ins_arr  = arr;
    lt_ins_size = size;
    lt_ins_item = item;
    lt_ins_new  = new_item;
}

void t15_available_remove(sim_fixture &fx) {
    fx.reset();
    sim_store                       s = fx.store();
    const lt_available_remove_calls c{&lt_stub_insert};
    lt_ins_calls = 0;
    detail::remove_from_available_buildings(s, c, 3, 17);
    check(lt_ins_calls == 1 && lt_ins_arr == s.available_buildings_row(3) && lt_ins_size == 0x32 &&
              lt_ins_item == 17 && lt_ins_new == 0,
          "t15 buildings: Insert(row[player], 50, ITEM=b_i, NEW=0) -- the Add mirror");
    fx.cfg_projects[9].type = 1;
    detail::remove_from_available_projects(fx.view(), s, c, 2, 9);
    check(lt_ins_calls == 2 && lt_ins_arr == s.available_projects_bucket(2, 1) && lt_ins_item == 9 &&
              lt_ins_new == 0,
          "t15 projects: the bucket comes from Projects[p_i].type, ITEM=p_i, NEW=0");
}

// ---- t14: the E leaves -- boundaries, the truncating no-op arm, the alert re-arm ----------------
void t14_e_leaves(sim_fixture &fx) {
    fx.reset();
    // math pair (llm_math_manhattan_dist has no state parameter at all).
    check(detail::manhattan_dist(3, 4, 13, -2) == 16, "t14 manhattan(3,4 -> 13,-2) = 10 + 6");
    check(detail::manhattan_dist(0, 0, -5, 5) == 10, "t14 manhattan negative-delta arm (CMP/JGE/NEG)");
    check(detail::scale_pct(fx.view(), 200.0, 50) == 100.0, "t14 scale_pct(200, 50%) = 100");
    // char_subst's three arms, incl. the truncate-to-empty no-op arm.
    char buf[16];
    strcpy(buf, "abcab");
    detail::str_char_subst(buf, 1, 'a', 'b');
    check(strcmp(buf, "c") == 0, "t14 STRIP keeps only chars differing from BOTH c1 and c2");
    strcpy(buf, "abc");
    detail::str_char_subst(buf, 2, 'b', 'x');
    check(strcmp(buf, "axc") == 0, "t14 REPLACE swaps c1 -> c2, everything else kept");
    strcpy(buf, "abc");
    detail::str_char_subst(buf, 0, 'a', 'b');
    check(buf[0] == 0, "t14 mode 0 is the TRUNCATE-TO-EMPTY no-op arm (original behaviour)");
    // invasion alert: fire + re-arm (interval 25.0 seeded with the real image value).
    sim_store s = fx.store();
    for (int i = 0; i < 32; ++i) detail::invasion_alert_clear(s, i); // all -1.0 = disarmed
    fx.invasion_alert_time[5] = 100.0;
    check(detail::invasion_alert_poll(fx.view(), s, 126.0) == 5,
          "t14 poll fires the one armed slot whose interval elapsed");
    check(fx.invasion_alert_time[5] == 126.0, "t14 the fired slot RE-ARMS to now");
    check(detail::invasion_alert_poll(fx.view(), s, 126.5) == -1,
          "t14 immediately after the re-arm nothing fires (interval not elapsed)");
}

// ---- t16: the frame pair (LT1F) -- call ORDER, both mode gates, and the asymmetry ---------------
//
// Recorder mocks over lt_frame_calls; plain function pointers, so the channel is file-static. The
// pump mock can FLIP the mode byte (through g_t16_store_mode), which is the arm the frame/redraw
// asymmetry hangs on: only `frame` re-checks GAME_MODE==2 after the pump (0x0043ed2e); redraw
// ticks regardless (0x0043edc2 falls straight through).
int      g_t16_seq[10];
int      g_t16_n         = 0;
uint8_t *g_t16_mode      = nullptr; // the fixture's game_mode byte
uint8_t  g_t16_pump_sets = 0xff;    // 0xff = pump leaves the mode alone

void t16_rec(int id) {
    if (g_t16_n < 10) g_t16_seq[g_t16_n] = id;
    ++g_t16_n;
}
void t16_input() { t16_rec(1); }
void t16_pump() {
    t16_rec(2);
    if (g_t16_pump_sets != 0xff) *g_t16_mode = g_t16_pump_sets;
}
void    t16_pace_tt() { t16_rec(3); }
int32_t t16_tt() {
    t16_rec(4);
    return 77;
} // result unread, as the original
void t16_harness_st() { t16_rec(5); }
void t16_st() { t16_rec(6); }
void t16_present() { t16_rec(7); }
void t16_view() { t16_rec(8); }

bool t16_seq_is(const int *want, int n) {
    if (g_t16_n != n) return false;
    for (int i = 0; i < n; ++i)
        if (g_t16_seq[i] != want[i]) return false;
    return true;
}

void t16_frame_pair(sim_fixture &fx) {
    fx.reset();
    g_t16_mode = &fx.game_mode;
    const lt_frame_calls c{t16_input, t16_pump, t16_pace_tt, t16_tt,
                           t16_harness_st, t16_st, t16_present, t16_view};
    sim_store            own = fx.store();

    // (a) mode 6 (tactical): frame still runs input_update FIRST (0x0043ed12 precedes the gate),
    // then bails; redraw runs NOTHING.
    fx.game_mode    = 6;
    fx.session_mode = 0;
    g_t16_pump_sets = 0xff;
    g_t16_n         = 0;
    detail::frame(fx.view(), own, c);
    {
        const int w[] = {1};
        check(t16_seq_is(w, 1), "t16a frame mode=6: input_update only (0x0043ed17)");
    }
    g_t16_n = 0;
    detail::frame_redraw_behind_dialog(fx.view(), own, c);
    check(g_t16_n == 0, "t16a redraw mode=6: nothing at all (0x0043edb0)");

    // (b) session 3, pump leaves mode 2: the FULL chain, in the exact order -- input, pump,
    // C4 chain hook BEFORE our time_tick, C6 chain hook BEFORE our sim_tick, render_present.
    fx.game_mode    = 2;
    fx.session_mode = 3;
    g_t16_pump_sets = 0xff;
    g_t16_n         = 0;
    detail::frame(fx.view(), own, c);
    {
        const int w[] = {1, 2, 3, 4, 5, 6, 7};
        check(t16_seq_is(w, 7), "t16b frame session=3 mode=2: input,pump,pace,tt,harness,st,present IN ORDER");
    }

    // (c) THE ASYMMETRY: pump flips mode 2 -> 5. frame STOPS after the recheck (0x0043ed2e);
    // redraw ticks anyway (no recheck exists on its path).
    fx.game_mode    = 2;
    fx.session_mode = 3;
    g_t16_pump_sets = 5;
    g_t16_n         = 0;
    detail::frame(fx.view(), own, c);
    {
        const int w[] = {1, 2};
        check(t16_seq_is(w, 2), "t16c frame: pump flipped mode -> ticks SKIPPED (0x0043ed2e)");
    }
    fx.game_mode = 2;
    g_t16_n      = 0;
    detail::frame_redraw_behind_dialog(fx.view(), own, c);
    {
        const int w[] = {2, 3, 4, 5, 6, 8};
        check(t16_seq_is(w, 6), "t16c redraw: pump flipped mode and it ticks ANYWAY (no 0x0043ed2e recheck) + render_view");
    }

    // (d) session != 3, mode neither 6 nor 2: no pump, and the ticks run anyway -- the mode==2
    // check exists ONLY inside the session==3 arm (0x0043ed27 jumps straight to 0x0043ed37).
    fx.game_mode    = 4;
    fx.session_mode = 1;
    g_t16_pump_sets = 0xff;
    g_t16_n         = 0;
    detail::frame(fx.view(), own, c);
    {
        const int w[] = {1, 3, 4, 5, 6, 7};
        check(t16_seq_is(w, 6), "t16d frame session!=3 mode=4: NO pump, ticks anyway (mode-2 check is session-arm-only)");
    }
}

// ---- t17: bldg_recompute_cell_grid (LT1C c4) -- whole-grid post-state ---------------------------
void t17_bldg_cell_grid(sim_fixture &fx) {
    fx.reset();
    sim_store own = fx.store();
    auto      g   = [&](int b, int row, int col) -> uint8_t        &{
        return fx.bldg_cell_grid[(size_t)(b * 64 + row * 8 + col)];
    };

    // Building types: [1] plain (5) with a single footprint cell; [2] wall-type 0x1c with the same
    // single cell; [3] type 7 (SE-quadrant stamp), empty area. total=3 -- INCLUSIVE bound (JBE).
    fx.cfg_building_sec.total      = 3;
    fx.cfg_buildings[1].type       = 5;
    fx.cfg_buildings[1].area[0][0] = 1;
    fx.cfg_buildings[2].type       = 0x1c;
    fx.cfg_buildings[2].area[0][0] = 1;
    fx.cfg_buildings[3].type       = 7;
    // Untouched sentinels: slot 0 (b starts at 1) and slot 4 (past the inclusive bound).
    g(0, 0, 0)                     = 0xAA;
    fx.bldg_cell_grid_row_shift[0] = 0x55;
    g(4, 0, 0)                     = 0xBB;

    detail::bldg_recompute_cell_grid(fx.view(), own);

    // [1] plain: stamp at (0+1, 0+1) = (1,1); after halo+normalize the 3x3 block around it is all
    // 1 and EVERYTHING else in grid[1] is 0. Assert the whole 64 bytes.
    check(fx.bldg_cell_grid_row_shift[1] == 0, "t17 plain type: row_shift 0 (0x004dc2a5)");
    {
        bool ok = true;
        for (int r = 0; r < 8; ++r)
            for (int col = 0; col < 8; ++col) {
                const uint8_t want = (r >= 0 && r <= 2 && col >= 0 && col <= 2) ? 1 : 0;
                if (g(1, r, col) != want) ok = false;
            }
        check(ok, "t17 plain type WHOLE grid: stamp(1,1) + 8-neighbour halo, normalized to 1, rest 0");
    }

    // [2] wall type 0x1c: row_shift -1 moves the stamp DOWN -- (0+1-(-1), 1) = (2,1) -- pinning
    // the SIGN of the shift (0x004dc2e7 SUB). Phase 7 also fills the NE quadrant rows 0..3 x
    // cols 4..7. Assert the shift cell, its halo, the quadrant, and that (1,1) is halo-only.
    check(fx.bldg_cell_grid_row_shift[2] == -1, "t17 type 0x1c: row_shift = -1 (0x004dc2be)");
    check(g(2, 2, 1) == 1 && g(2, 1, 1) == 1 && g(2, 3, 2) == 1,
          "t17 type 0x1c: footprint stamped at (2,1), not (1,1) -- the shift SUBTRACTS");
    {
        bool quad = true;
        for (int r = 0; r < 4; ++r)
            for (int col = 4; col < 8; ++col)
                if (g(2, r, col) != 1) quad = false;
        check(quad, "t17 type 0x1c: NE quadrant rows 0..3 x cols 4..7 stamped (0x004dc4c7)");
    }

    // [3] type 7, empty footprint: SE quadrant stamped -- and NO halo around it, which pins the
    // PHASE ORDER: the halo pass (0x004dc310) runs BEFORE the type-quadrant stamps (0x004dc444+),
    // so a quadrant with no phase-3 footprint gets no ring. Everything outside the quadrant is 0.
    {
        bool quad = true, clean = true;
        for (int r = 0; r < 8; ++r)
            for (int col = 0; col < 8; ++col) {
                const uint8_t want = (r >= 4 && col >= 4) ? 1 : 0;
                if (g(3, r, col) != want) { (r >= 4 && col >= 4) ? quad = false : clean = false; }
            }
        check(quad, "t17 type 7: SE quadrant rows 4..7 x cols 4..7 stamped (0x004dc45c)");
        check(clean, "t17 type 7: NO halo -- the halo pass precedes the quadrant stamp (phase order "
                     "0x004dc310 < 0x004dc444), and no flat-probe row bleed");
    }

    // Normalize left no transient 2s anywhere in the written range.
    {
        bool no2 = true;
        for (size_t i = 64; i < 4 * 64; ++i)
            if (fx.bldg_cell_grid[i] == 2) no2 = false;
        check(no2, "t17 normalize: no halo 2s survive (0x004dc501)");
    }

    // Untouched sentinels: slot 0 (loop starts at 1) and slot 4 (bound is INCLUSIVE total=3, so
    // grid[3] was written and grid[4] was not).
    check(g(0, 0, 0) == 0xAA && fx.bldg_cell_grid_row_shift[0] == 0x55,
          "t17 slot 0 untouched (b starts at 1, 0x004dc26e)");
    check(g(4, 0, 0) == 0xBB, "t17 slot 4 untouched (JBE inclusive bound wrote 1..3, 0x004dc53a)");
}

// ---- t18/t19: the two internalized chat-target entries (SIMABI-CHAT) ---------------------------
//
// llm_ui_chat_recalc_target_mode @0x0049d631 and llm_ui_chat_target_add @0x0049d5c1 left the sim host
// table when libmh took their bodies (libmh/lockstep/lt_chat_ally_mask.cpp). Neither is shadow-armed, and
// neither needs to be: both are PURE over `chat_target_state`, so the cheap parameterised-state test
// the reimpl-loop prefers to a T3 covers every branch off the game entirely.
//
// The DUAL-WRITER story is what makes exactness load-bearing rather than merely desirable: the
// originals stay live for untranslated UI (llm_ui_chat_target_remove, llm_ui_diplomacy_apply_and_resume)
// and write the SAME two region-registered cells, so a divergence would surface intermittently,
// depending on which writer ran last. Every case below therefore asserts the cell the function is not
// supposed to touch as well as the one it is.
//
// The profile array is file-scope, not a local: 8 x 0x740 is ~15 KB of stack per call.
mh::game::mh_llm_strat_player_profile lt_chat_profiles[8];

struct lt_chat_fixture {
    uint8_t  mode = 0xEE; // a sentinel no branch can produce (recalc writes only 0/2/3)
    uint8_t  mask = 0;
    uint16_t side = 0;

    mh::lockstep::chat_target_state st() {
        return mh::lockstep::chat_target_state{&mode, &mask, lt_chat_profiles, &side};
    }
    // ALIVE|HUMAN = the two bits recalc gates on (TEST ...,0x2 / TEST ...,0x4 at 0x0049d686/0x0049d696)
    void reset(uint32_t flags_for_all = 0) {
        for (int i = 0; i < 8; ++i) lt_chat_profiles[i].status_flags = flags_for_all;
        mode = 0xEE;
        mask = 0;
        side = 0;
    }
};

constexpr uint32_t LT_ALIVE_HUMAN = 0x02u | 0x04u;

void t18_chat_recalc_target_mode() {
    lt_chat_fixture fx;

    // [1] NO ELIGIBLE SLOT AT ALL -> 3. The mask is irrelevant: a slot that fails the gate is skipped
    // entirely (0x0049d68d/0x0049d69d -> 0x0049d6ad), so it neither counts nor clears all_selected.
    fx.reset(0);
    fx.mask = 0xFF;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 3, "t18 no eligible slot: mode 3 even with every mask bit set");
    check(fx.mask == 0xFF, "t18 recalc NEVER writes the mask");

    // [2] THE EMPTY-SELECTION CASE: eligible targets exist, none selected -> 3 (any_selected false at
    // 0x0049d719). This is the branch the whole 8-slot loop exists to distinguish from [1].
    fx.reset(0);
    for (int i = 1; i < 4; ++i) lt_chat_profiles[i].status_flags = LT_ALIVE_HUMAN;
    fx.mask = 0;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 3, "t18 empty selection: 3 eligible targets, no mask bit -> mode 3");

    // [3] ALL SELECTED, MORE THAN ONE -> 0 (0x0049d72d).
    fx.reset(0);
    for (int i = 1; i < 4; ++i) lt_chat_profiles[i].status_flags = LT_ALIVE_HUMAN;
    fx.mask = 0x0E; // bits 1,2,3
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 0, "t18 all three eligible selected -> mode 0");

    // [4] THE `selected > 1` BOUNDARY (`CMP [EBP-0x24],1 / JG` at 0x0049d725): exactly ONE eligible
    // target, and it IS selected, so all_selected still holds -- yet the mode is PARTIAL, not ALL.
    // A reimplementation that reads the branch as "everything selected -> 0" passes [3] and fails here.
    fx.reset(0);
    lt_chat_profiles[1].status_flags = LT_ALIVE_HUMAN;
    fx.mask                          = 0x02;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 2, "t18 exactly one eligible target, selected: mode 2, NOT 0 (selected > 1)");

    // [5] PARTIAL: 2 of 3 selected -> 2 (0x0049d736).
    fx.reset(0);
    for (int i = 1; i < 4; ++i) lt_chat_profiles[i].status_flags = LT_ALIVE_HUMAN;
    fx.mask = 0x06; // bits 1,2 -- slot 3 eligible and unselected
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 2, "t18 two of three selected -> mode 2");

    // [6] SELF IS SKIPPED (0x0049d6a1-0x0049d6ab), and PlayerSide is read as a 16-bit UNSIGNED value.
    // Same eligible set and same mask, twice, differing only in whose slot is self.
    fx.reset(0);
    for (int i = 0; i < 3; ++i) lt_chat_profiles[i].status_flags = LT_ALIVE_HUMAN;
    fx.side = 0;
    fx.mask = 0x07; // bits 0,1,2
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 0, "t18 self skipped: slots 1,2 selected -> mode 0 (self's own bit ignored)");
    fx.mode = 0xEE;
    fx.mask = 0x01; // ONLY self's bit
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 3, "t18 self skipped: only self's bit set -> mode 3, not 0");

    // [7] THE GATE IS BOTH BITS. ALIVE alone and HUMAN alone are each skipped, so a mask bit on such a
    // slot is inert -- it neither counts nor clears all_selected.
    fx.reset(0);
    lt_chat_profiles[1].status_flags = 0x02; // ALIVE, not HUMAN
    lt_chat_profiles[2].status_flags = 0x04; // HUMAN, not ALIVE
    lt_chat_profiles[3].status_flags = LT_ALIVE_HUMAN;
    fx.mask                          = 0x08; // only the genuinely eligible slot 3
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 2, "t18 half-flagged slots skipped: one real target selected -> mode 2");
    fx.mode = 0xEE;
    fx.mask = 0x06; // bits on the two half-flagged slots ONLY
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 3, "t18 half-flagged slots skipped: their mask bits select nothing -> mode 3");

    // [8] ALL EIGHT SLOTS, none of them self: the loop bound is `CMP [i],0x8 / JL` -- slot 7 counts.
    fx.reset(LT_ALIVE_HUMAN);
    fx.side = 8;    // out of range on purpose: no slot is self
    fx.mask = 0x7F; // bits 0..6 -- slot 7 eligible and UNSELECTED
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 2, "t18 slot 7 is inside the loop: leaving its bit clear gives 2, not 0");
    fx.mode = 0xEE;
    fx.mask = 0xFF;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == 0, "t18 all eight slots selected -> mode 0");

    // [9] IDEMPOTENT -- the property the dual-writer story rests on.
    fx.mode = 0xEE;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    const uint8_t once = fx.mode;
    mh::lockstep::detail::chat_recalc_target_mode(fx.st());
    check(fx.mode == once && fx.mask == 0xFF, "t18 recalc is idempotent over both cells");
}

void t19_chat_target_add() {
    lt_chat_fixture fx;

    // [1] THE PLAIN SET, and the recalc tail (0x0049d5e9) really runs: mode moves off the sentinel.
    fx.reset(0);
    for (int i = 1; i < 4; ++i) lt_chat_profiles[i].status_flags = LT_ALIVE_HUMAN;
    mh::lockstep::detail::chat_target_add(fx.st(), 1);
    check(fx.mask == 0x02, "t19 add(1) on an empty mask -> bit 1 only");
    check(fx.mode == 2, "t19 add recalcs the mode in the same call (1 of 3 selected -> 2)");

    // [2] THE ALREADY-SET CASE: OR is idempotent, and so is the recalc behind it.
    mh::lockstep::detail::chat_target_add(fx.st(), 1);
    check(fx.mask == 0x02, "t19 add(1) twice: the mask is unchanged (OR, not toggle/count)");
    check(fx.mode == 2, "t19 add(1) twice: the mode is unchanged");

    // [3] OTHER BITS SURVIVE -- it is an OR of one bit, never an assignment.
    fx.mask = 0x05; // bits 0,2
    mh::lockstep::detail::chat_target_add(fx.st(), 1);
    check(fx.mask == 0x07, "t19 add(1) preserves bits 0 and 2");

    // [4] EVERY SLOT, one at a time, ends with the full byte.
    fx.reset(LT_ALIVE_HUMAN);
    fx.side = 8;
    for (uint8_t i = 0; i < 8; ++i) mh::lockstep::detail::chat_target_add(fx.st(), i);
    check(fx.mask == 0xFF, "t19 adding 0..7 fills the byte");
    check(fx.mode == 0, "t19 after filling the byte the mode is 0 (all eight selected)");

    // [5] THE 8-BIT SHIFT WIDTH (`MOV AL,1 / SHL AL,CL` at 0x0049d5df). A player_id whose low five
    // bits are >= 8 shifts the bit clean out of AL, so the OR is a no-op -- what the ORIGINAL does,
    // not a guess about what it should do. Callers only ever pass 0..7; this pins the width so a
    // 32-bit `1u << id` (which would leave the mask alone here too, but a `1u << (id & 7)` would NOT)
    // cannot creep in unnoticed.
    fx.reset(LT_ALIVE_HUMAN);
    fx.side = 8;
    fx.mask = 0x0F;
    mh::lockstep::detail::chat_target_add(fx.st(), 8);
    check(fx.mask == 0x0F, "t19 add(8): the 8-bit shift drops the bit, mask unchanged");
    mh::lockstep::detail::chat_target_add(fx.st(), 9);
    check(fx.mask == 0x0F, "t19 add(9): same -- NOT a wrap to bit 1");
}

int run_all() {
    printf("=== libtranstest (lib_trans: the domain oracle -- LT0 expectation layer + LT1A) ===\n");
    {
        sim_fixture fx;
        t1_model_matches_vectors();
        t2_our_rng_next_matches_vectors(fx);
        t3_seed_mask_and_channel_isolation(fx);
        t4_batch_a_wrapper_contracts(fx);
        t5_rand_state_advance_reference();
        t6_lt1a_translations(fx);
        t7_map_region_pool(fx);
        t8_available_projects_clear(fx);
        t9_placement_offsets(fx);
        t10_progress_finalize_acquire(fx);
        t11_scan_masked_table(fx);
        t12_player_by_side_id();
        t13_diplomacy_inits(fx);
        t14_e_leaves(fx);
        t15_available_remove(fx);
        t16_frame_pair(fx);
        t17_bldg_cell_grid(fx);
        t18_chat_recalc_target_mode();
        t19_chat_target_add();
    }
    printf("=== libtranstest: %d check(s), %d failure(s) ===\n", lt_checks, lt_fails);
    return lt_fails == 0 ? 0 : 1;
}

} // namespace
} // namespace mh::sim::test

int run_libtranstest() { return mh::sim::test::run_all(); }
