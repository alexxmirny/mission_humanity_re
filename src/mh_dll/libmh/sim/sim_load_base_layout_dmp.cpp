//
// sim/sim_load_base_layout_dmp.cpp -- see sim_load_base_layout_dmp.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_load_base_layout_dmp_004d8839.asm) -- no Ghidra .c draft was
// available for this function.
//
#include "sim/sim_load_base_layout_dmp.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const load_base_layout_dmp_calls &live_load_base_layout_dmp_calls() {
    static const load_base_layout_dmp_calls c = {
        mh::host().asset_read,
        MH_CRT(utils_malloc),
        MH_CRT(utils_free),
        MH_LIBMH_BIND(llm_strat_bldg_queue_construction),
        MH_LIBMH_BIND(llm_strat_toroidal_dist_sq),
        MH_LIBMH_BIND(llm_strat_dmp_enqueue_scripted_order),
    };
    return c;
}

namespace detail {

namespace {

// The on-disk/resource record shape both sections share: {uint16 field0; uint8 field1; uint8 field2},
// packed with NO padding (matches the assembly's byte-cursor walk, +2 then +1 then +1). TU-scoped --
// no Ghidra struct exists for this format (searched mh_structs.gen.h for "dmp"/"base_layout": zero
// hits), and this is resource/file data, not a game global, so there is nothing to declare a need
// against (see the header's uncertainty (2)).
struct dmp_record {
    uint16_t field0;
    uint8_t  field1;
    uint8_t  field2;
};

dmp_record read_dmp_record(const uint8_t *&cursor) {
    dmp_record r;
    std::memcpy(&r.field0, cursor, sizeof(r.field0));
    cursor += sizeof(r.field0);
    std::memcpy(&r.field1, cursor, sizeof(r.field1));
    cursor += sizeof(r.field1);
    std::memcpy(&r.field2, cursor, sizeof(r.field2));
    cursor += sizeof(r.field2);
    return r;
}

int32_t read_i32(const uint8_t *&cursor) {
    int32_t v;
    std::memcpy(&v, cursor, sizeof(v));
    cursor += sizeof(v);
    return v;
}

} // namespace

void load_base_layout_dmp(const sim_view &v, sim_store &own, int32_t player, char *dmp_path,
                          const load_base_layout_dmp_calls &c) {
    (void)v; // every read/write this function does is on player_data[player], reached mutably below --
             // see the header's "STATE" note on why no other sim_view member is needed.

    // 0x004d8851-0x004d885b: the resource lookup. NULL -> nothing to load, nothing to free.
    //
    // SIMABI-VFS: two calls where the original made one, and the extra one is the SIZE QUERY -- the
    // original never needed a length because it walked the host's own buffer, which is exactly the
    // ownership this slice removed. An absent asset answers <= 0 here and lands on the same early
    // return the null pointer used to.
    const int32_t len = c.asset_read(dmp_path, nullptr, 0);
    if (len <= 0) {
        return;
    }
    uint8_t *raw = static_cast<uint8_t *>(c.mem_alloc(static_cast<uint32_t>(len)));
    if (raw == nullptr) {
        return;
    }
    if (c.asset_read(dmp_path, raw, static_cast<uint32_t>(len)) != len) {
        c.mem_free(raw);
        return;
    }
    const uint8_t *cursor = raw;

    player_data &pd = own.player_at(static_cast<uint32_t>(player));

    // ---- SECTION 1 (0x004d8861-0x004d8a37): building placements -----------------------------------
    // 0x004d887f-0x004d8883: CMP/JZ tests EQUALITY TO ZERO, not sign -- a negative count would still
    // loop (decrementing toward 0), so this is `!= 0`, not `> 0`, to stay faithful for out-of-range
    // input even though a well-formed resource never produces one.
    int32_t count1 = read_i32(cursor);
    while (count1 != 0) {
        const dmp_record rec           = read_dmp_record(cursor);
        const uint16_t   building_type = rec.field0;
        const uint8_t    x             = rec.field1;
        const uint8_t    y             = rec.field2;
        // 0x004d88b9: DEC dword ptr [EBP-0x14] -- the ONLY decrement site, reached by every path
        // (including the MOTHER-skip below), so it is done once here rather than duplicated per exit.
        --count1;

        // 0x004d88af-0x004d88b9: the landing sequence already places the race MOTHER building -- skip
        // re-queueing it from the scripted layout.
        if (static_cast<uint32_t>(building_type) == pd.ai_mother_building_type) {
            continue;
        }

        // 0x004d88be-0x004d894e: is building_type already pending in ai_build_plan[0..plan_length)?
        const uint32_t plan_len = pd.ai_build_plan_len_and_flag & 0x7fffffffu;
        int32_t        found    = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(plan_len); ++i) {
            if (pd.ai_build_plan[i] == static_cast<int32_t>(building_type)) {
                found = i;
                break;
            }
        }
        // 0x004d88d0-0x004d88fd: if found, remove it -- shift the tail down by one and store
        // (plan_length - 1) back into the low 31 bits, preserving the top ("ring/spiral scan") bit.
        if (found >= 0) {
            const uint32_t flag_bit       = pd.ai_build_plan_len_and_flag & 0x80000000u;
            const uint32_t new_len        = plan_len - 1;
            pd.ai_build_plan_len_and_flag = flag_bit | new_len;
            for (uint32_t i = static_cast<uint32_t>(found); i < new_len; ++i) {
                pd.ai_build_plan[i] = pd.ai_build_plan[i + 1];
            }
        }

        // 0x004d8954-0x004d895f: queue it for real, immediate construction. Return value discarded --
        // not read again by this function.
        c.bldg_queue_construction(player, static_cast<int32_t>(building_type), static_cast<int16_t>(x),
                                  static_cast<uint16_t>(y));

        // 0x004d897a-0x004d8984: flag the entry llm_strat_bldg_queue_construction just appended --
        // ai_bldg_queue[ai_bldg_queue_count - 1].status |= 0x20 ("affordability check waived"). NOT
        // cap/overflow-guarded here, matching the assembly -- see the header's uncertainty (1).
        {
            const int32_t idx = pd.ai_bldg_queue_count - 1;
            pd.ai_bldg_queue[idx].status |= 0x20u;
        }

        // 0x004d898c-0x004d8a32: mine placements ALSO snap the nearest still-unresolved resource
        // site's cached build tile to this record's (x, y).
        if (static_cast<uint32_t>(building_type) == pd.ai_mine_candidate_tier1 ||
            static_cast<uint32_t>(building_type) == pd.ai_mine_candidate_tier2) {
            int32_t best_index = -1;
            int32_t best_dist  = 0x7fffffff; // 0x004d89aa: MOV ECX,0x7fffffff -- SIGNED comparison below
            for (int32_t i = 0; i < pd.ai_resource_site_count; ++i) {
                const auto &site = pd.ai_resource_sites[i];
                if (site.status != 0) {
                    continue; // 0x004d89c0: JNZ -- only status==0 (unresolved) sites are candidates
                }
                // reimpl-verify (2026-08-22): 0x004d89c2/0x004d89cd are MOVZX word -- grid_x/grid_y are
                // ZERO-extended here, not sign-extended (`static_cast<int32_t>(int16_t)` would
                // sign-extend and diverge for a stored bit pattern with the high bit set).
                const int32_t dist = static_cast<int32_t>(c.toroidal_dist_sq(
                    static_cast<int32_t>(x), static_cast<int32_t>(y),
                    static_cast<int32_t>(static_cast<uint16_t>(site.grid_x)) * 4,
                    static_cast<int32_t>(static_cast<uint16_t>(site.grid_y)) * 4));
                if (dist < best_dist) { // 0x004d89e6-0x004d89e8: CMP/JGE, signed
                    best_dist  = dist;
                    best_index = i;
                }
            }
            if (best_index >= 0) { // 0x004d8a0e-0x004d8a12: CMP [best_index],-1 / JZ skip
                pd.ai_resource_sites[best_index].build_tile_x = static_cast<int16_t>(x);
                pd.ai_resource_sites[best_index].build_tile_y = static_cast<int16_t>(y);
            }
        }
    }

    // ---- the section 1/2 transition (0x004d8a37-0x004d8a4d) ---------------------------------------
    pd.ai_build_plan_cursor = 0;
    pd.ai_build_plan_len_and_flag |= 0x80000000u;
    int32_t count2 = read_i32(cursor);

    // ---- SECTION 2 (0x004d8a50-0x004d8a8e): scripted orders ---------------------------------------
    // 0x004d8a50-0x004d8a54: same `!= 0`-not-`> 0` posture as section 1's loop test above.
    while (count2 != 0) {
        const dmp_record rec        = read_dmp_record(cursor);
        const uint16_t   order_code = rec.field0;
        const uint8_t    x          = rec.field1;
        const uint8_t    y          = rec.field2;

        // 0x004d8a7a-0x004d8a86: param_1=x, param_2=y, param_3=order_code, param_4=player -- register
        // load order confirmed immediately before the CALL. Return value discarded.
        c.dmp_enqueue_scripted_order(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                     static_cast<uint32_t>(order_code), static_cast<uint16_t>(player));
        --count2; // 0x004d8a8b: DEC dword ptr [EBP-0x14]
    }

    // 0x004d8a90-0x004d8a93: free the ORIGINAL pointer (not the walked cursor). Since SIMABI-VFS
    // that pointer is OUR buffer rather than the host's, so this is a same-heap free -- the shape of
    // the mistake it would be (freeing the walked cursor) is unchanged, which is why T11 stays.
    c.mem_free(raw);
}

} // namespace detail

// ---- the public wrapper ----------------------------------------------------------------------------

void load_base_layout_dmp(int32_t player, char *dmp_path) {
    sim_state st = state();
    detail::load_base_layout_dmp(st.read, st.own, player, dmp_path, live_load_base_layout_dmp_calls());
}


} // namespace mh::sim
