//
// sim/libtrans/sim_lt_map_region_pool.cpp -- see sim_lt_map_region_pool.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/map_block_8_GetNextBlock_004222cf.asm,
// tmp/decomp_lib_trans/llm_map_region_free_0042239a.asm,
// tmp/decomp_lib_trans/llm_map_region_pool_reset_004234b8.asm), not from the Ghidra `.c` drafts.
//
#include "sim/libtrans/sim_lt_map_region_pool.h"

#include "addr/mh_calls.gen.h" // MH_CRT(utils_malloc) / utils_free -- bound live in live_lt_map_region_pool_calls()
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const lt_map_region_pool_calls &live_lt_map_region_pool_calls() {
    static const lt_map_region_pool_calls c = {
        MH_CRT(utils_malloc),
        MH_CRT(utils_free),
    };
    return c;
}

namespace detail {

// ---- map_block_8_GetNextBlock @0x004222cf ------------------------------------------------------
llm_map_region *get_next_block(sim_store &own, const lt_map_region_pool_calls &c) {
    static_assert(sizeof(llm_map_region) == 0x420,
                  "mh_llm_map_region size drift vs 0x004222f0's MOV EAX,0x420 literal");

    llm_map_region *node;

    if (own.region_pool_free_head() == nullptr) {
        // 0x004222f0-0x004222fa: malloc a fresh node.
        node = static_cast<llm_map_region *>(c.malloc_(static_cast<uint32_t>(sizeof(llm_map_region))));

        // 0x004222fd-0x0042230c: index = counter, then counter += 1. A 16-bit store of the 32-bit
        // counter value (`MOV word ptr [EAX],DX`) -- node->index is u16, so the narrowing is exact.
        int32_t &counter = own.region_alloc_counter();
        node->index      = static_cast<uint16_t>(counter);
        counter += 1;
    } else {
        // 0x00422311-0x00422324: pop the free list. `index` is NOT touched here -- a recycled node
        // keeps its original id.
        node                        = own.region_pool_free_head();
        own.region_pool_free_head() = node->next;
    }

    // 0x00422329-0x00422335: register in BY_INDEX[index] on BOTH paths (`index << 2` == the
    // pointer-sized stride into the 4096-slot table).
    own.region_by_index()[node->index] = node;

    // 0x0042233e/0x00422348/0x00422352/0x00422359/0x00422360: reset ONLY these five fields on BOTH
    // paths. neighbors[]/neighbor_data[]/route_bfs_dist/route_parent/route_mark are left stale on a
    // recycled node -- the original's contract (llm_map_region_recompute_adjacency rebuilds the
    // neighbour arrays), not an omission. No memset (translator-brief rule 11).
    node->cell_count     = 0;       // 0x0042233e
    node->neighbor_count = 0;       // 0x00422348
    node->x              = 0;       // 0x00422352
    node->y              = 0;       // 0x00422359
    node->prev           = nullptr; // 0x00422360

    // 0x0042236a-0x0042237c: push onto the active list (next = old head; head = node).
    node->next                 = own.region_list_head_mut();
    own.region_list_head_mut() = node;

    // 0x00422381: live-node count increment. G_LAST_MAP_INDEX is a COUNT despite its name, not an
    // index and not a high-water mark.
    own.last_map_index() += 1;

    return node;
}

// ---- llm_map_region_free @0x0042239a -----------------------------------------------------------
void region_free(sim_store &own, llm_map_region *node) {
    // 0x004223b5: prev := nullptr; 0x004223bc-0x004223c1: cur := LIST_HEAD.
    llm_map_region *prev = nullptr;
    llm_map_region *cur  = own.region_list_head_mut();

    // 0x004223c4-0x00422414: walk the active list for `node`. The moment `cur` runs out
    // (0x004223ca), control falls straight to the UNCONDITIONAL tail below (LAB_00422416) -- the
    // exact same target the "found and unlinked" arm reaches via 0x0042240c. No guard distinguishes
    // the two paths.
    while (cur != nullptr) {
        if (cur == node) {
            if (prev == nullptr) {
                // 0x004223fc-0x00422407: head case. Re-reads LIST_HEAD from memory rather than
                // from `cur` -- equivalent here (nothing mutates LIST_HEAD earlier in this walk),
                // reproduced literally.
                own.region_list_head_mut() = own.region_list_head_mut()->next;
            } else {
                // 0x004223e8-0x004223f4: mid-list splice.
                prev->next = cur->next;
            }
            break;
        }
        // 0x0042240e-0x00422414 / 0x004223cc-0x004223d8: advance.
        prev = cur;
        cur  = cur->next;
    }

    // LAB_00422416 (0x00422416-0x00422440): UNCONDITIONAL. Reached whether `node` was found and
    // unlinked above OR never found on the active list at all (the 0x004223ca fallthrough) -- no
    // guard, no early return. A node that was never on the active list still gets its BY_INDEX slot
    // cleared, still gets pushed onto the free list, and the live-node count still decrements.
    own.region_by_index()[node->index] = nullptr;                     // 0x00422419-0x0042241f
    node->next                         = own.region_pool_free_head(); // 0x00422429/0x00422432
    own.region_pool_free_head()        = node;                        // 0x0042243b
    own.last_map_index() -= 1;                                        // 0x00422440
}

// ---- llm_map_region_pool_reset @0x004234b8 -----------------------------------------------------
void pool_reset(sim_store &own, const lt_map_region_pool_calls &c) {
    // LAB_004234d0-0x004234e3: drain the whole active list through region_free. The original's own
    // CALL at 0x004234de targets llm_map_region_free directly -- here, the in-TU sibling
    // detail::region_free, a plain call per translator-brief rule 3c (both live in this TU; no
    // `_calls` member needed since region_free itself makes no outward call).
    while (own.region_list_head_mut() != nullptr) {
        region_free(own, own.region_list_head_mut());
    }

    // LAB_004234e5-0x0042350e: utils_free every node still on the (now-swollen) free list. `next`
    // is saved BEFORE the free, matching the original's own read-next-then-free-then-advance order.
    while (own.region_pool_free_head() != nullptr) {
        llm_map_region *node = own.region_pool_free_head();
        llm_map_region *next = node->next;  // 0x004234f3/0x004234f9
        c.free_(node);                      // 0x00423501: utils_free(FREE_HEAD)
        own.region_pool_free_head() = next; // 0x00423509
    }

    own.last_map_index()       = 0; // 0x00423510
    own.region_alloc_counter() = 1; // 0x0042351a: NOT 0 -- index 0 is never allocated
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

llm_map_region *get_next_block() {
    sim_state st = state();
    return detail::get_next_block(st.own, live_lt_map_region_pool_calls());
}

void region_free(uint32_t region_ptr) {
    // Ghidra gap G1: the committed export takes `uint32_t`, but the parameter is unambiguously a
    // node pointer (dereferenced at 0x00422419/0x00422432 in the original). Cast at the boundary,
    // matching sim_map_region_split.h's region_pick_smaller precedent.
    sim_state st = state();
    detail::region_free(st.own, reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(region_ptr)));
}

void pool_reset() {
    sim_state st = state();
    detail::pool_reset(st.own, live_lt_map_region_pool_calls());
}

} // namespace mh::sim
