//
// ai/ai_mine_yield.cpp -- see ai_mine_yield.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_calc_mine_yield_estimate_004e30e9.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h and addr/mh_addrs.gen.h:
//   0x00825084 = map_width                    0x00825064 = map_height
//   0x006616bc = _G_LLM_STRAT_AI_MINE_YIELD_KERNEL   (+0 dx, +4 dy, +8 weight; 0x10 stride)
//   0x00c28530 = map::resources                (row stride 1024 = 64 cells of 0x10; X is the ROW)
//   0xd9ec80   = cfg Building base, so [..+0xd9f21d] = Building + 0x59d = extract_id[]
//                                        and [..+0xd9f22d] = Building + 0x5ad = extract_val[]
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_mine_yield.h"

#include <bit>

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

int32_t x87_scale_and_trunc(uint32_t extract_val, const double *weight) {
    return ::mh::fp::x87_scale_and_trunc(extract_val, weight);
}

mine_yield_report calc_mine_yield_estimate(const ai_view &v, int32_t building_id, uint32_t tile_x,
                                           uint32_t tile_y, int32_t *out_yield,
                                           mine_quality *out_quality) {
    mine_yield_report rep{};

    // SIGNED /4 (SAR 0x1f / SHL 2 / SBB / SAR 2 @0x004e3107-0x004e312f), then used as an UNSIGNED
    // divisor below. Zero for a map under 4 tiles wide -- the original faults there too.
    const uint32_t coarse_w = (uint32_t)(*v.map_width / 4);
    const uint32_t coarse_h = (uint32_t)(*v.map_height / 4);
    const uint32_t cx       = tile_x >> 2; // SHR, UNSIGNED (0x004e3131)
    const uint32_t cy       = tile_y >> 2; //                (0x004e3136)

    // The four stack arrays. The original leaves slot [0] and slots [5..7] as whatever the frame
    // held and never reads them; zero-initialising here is not observable (they are locals, not
    // compared state) and avoids reading indeterminate values.
    int32_t present[MINE_EXTRACT_SLOTS] = {};
    int32_t extract[MINE_EXTRACT_SLOTS] = {};
    float   best[MINE_EXTRACT_SLOTS]    = {};

    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) { // 0x004e3142
        present[r] = 0;
        extract[r] = 0;
    }
    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) { // 0x004e3162
        out_yield[r] = 0;                                             // NOT out_yield[0] -- see the header
        best[r]      = 0.0f;
    }

    // ---- which resources can this building TYPE extract? 0x004e317d-0x004e31ae ----
    // A TERMINATED LIST: the walk breaks on the first zero id, it does not skip it.
    const cfg_building &bt = v.cfg_buildings[building_id];
    for (int32_t k = 0; k < 4; ++k) {
        const int32_t id = bt.extract_id[k]; // 0x004e318a
        if (id == 0) break;                  // JZ 0x004e31b0
        ++rep.extract_slots;
        if ((uint32_t)id >= (uint32_t)MINE_EXTRACT_SLOTS) {
            // The original writes past its own eight-dword locals here. Not expressible; counted.
            ++rep.bad_extract_id;
            continue;
        }
        present[id] = 1;                 // 0x004e3199
        extract[id] = bt.extract_val[k]; // 0x004e31a6
    }

    // ---- the kernel scan, 0x004e31ba-0x004e3267 ----
    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
        if (present[r] == 0) continue; // 0x004e31ba

        for (int32_t k = 0; k < MINE_KERNEL_CELLS; ++k) { // CMP ECX,0x19 / JC @0x004e325a
            const mine_kernel_cell &cell = v.mine_yield_kernel[k];
            // add-then-UNSIGNED-DIV remainder: the +extent is what keeps a negative offset in range.
            const uint32_t wx = ((uint32_t)((int32_t)cx + cell.dx) + coarse_w) % coarse_w; // 0x004e31d0
            const uint32_t wy = ((uint32_t)((int32_t)cy + cell.dy) + coarse_h) % coarse_h; // 0x004e31e6

            // (wx << 10) + (wy << 4) + base, i.e. resources[wx][wy] -- X selects the ROW.
            const map_resources &c = v.resources[wx * (uint32_t)COARSE_PLANE_SIDE + wy];
            if (c.value[r] == 0) continue; // CMP word ptr [..],0 / JZ @0x004e3202

            out_yield[r] = x87_scale_and_trunc((uint32_t)extract[r], &cell.weight); // 0x004e323b
            ++rep.resources_hit;

            // The dead running-maximum (header note 3). `!(best >= w)` and not `best < w`, so that an
            // unordered compare stores -- which is what JNC on C0 does (0x004e3242-0x004e3253).
            if (!((double)best[r] >= cell.weight)) best[r] = (float)cell.weight;
            break; // both arms fall to the OUTER increment at 0x004e3263
        }
    }

    // ---- the total and the ring histogram, 0x004e326d-0x004e32e4 ----
    out_quality->near_count = 0; // 0x004e3274
    out_quality->mid_count  = 0; // 0x004e327a
    out_quality->far_count  = 0; // 0x004e3281

    for (int32_t r = RESOURCE_ID_FIRST; r <= RESOURCE_ID_LAST; ++r) {
        out_yield[0] += out_yield[r]; // ADD, never a store -- see the header (0x004e329f)

        // The float compared AS A SIGNED INT, exactly as the original does it.
        const int32_t bits = std::bit_cast<int32_t>(best[r]);
        if (bits >= MINE_RING0_BITS) {
            ++out_quality->near_count; // 0x004e32b2
        } else if (bits >= MINE_RING1_BITS) {
            ++out_quality->mid_count; // 0x004e32c7
        } else if (bits >= MINE_RING2_BITS) {
            ++out_quality->far_count; // 0x004e32dd
        }
        // A resource with no deposit anywhere in the kernel keeps best == 0.0f and is counted in none
        // of the three (the last JL @0x004e32d4 falls straight to the increment).
    }
    return rep;
}

} // namespace detail

void calc_mine_yield_estimate(int32_t building_id, uint32_t tile_x, uint32_t tile_y, int32_t *out_yield,
                              int32_t *out_quality) {
    const ai_state st = state();
    // committed llm_strat_ai_calc_mine_yield_estimate's 5th param is int32_t *; the real payload is
    // mh::ai::mine_quality (near/mid/far ring counts).
    (void)detail::calc_mine_yield_estimate(st.read, building_id, tile_x, tile_y,
                                           out_yield, reinterpret_cast<mine_quality *>(out_quality));
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// NOTHING IS STUBBED because nothing is called: apart from the inert Watcom stack probe the body's
// only CALL is utils_math_trunc @0x004d0596, which touches no memory at all (it saves and restores
// the x87 control word on its own stack). Every write goes through the two out-pointers, which the
// only caller in the image aims at rows of _G_LLM_STRAT_AI_MINE_YIELD_ESTIMATE and
// _G_LLM_STRAT_AI_MINE_QUALITY_HIST -- the two regions the site declares by hand, because a pointer
// written through a register is invisible to the state matrix (the register-write blind spot).
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. `hit == 0` on every call means no resource was ever found in
// any of the 25 cells, so the x87 product, the store and all three ring buckets never ran and the
// site proved only that an empty neighbourhood writes zeros. `slots == 0` means the building type
// extracts nothing at all, which makes even the outer loop vacuous. Read both before the divergence
// count, and prefer a save with a developed base -- an early skirmish start has one mine type and a
// thin plane.

} // namespace mh::ai
