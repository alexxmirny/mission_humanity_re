//
// sim_load_base_layout_dmp_selftest.cpp -- offline `simtest` oracle for
// llm_strat_load_base_layout_dmp @0x004d8839 (sim/sim_load_base_layout_dmp.h/.cpp, RI-SIM /
// SIM1-G4, arm_ready:false -- see the header's DO-NOT-ARM section: the region closure reaches two
// opaque callees whose full write set is undeclared, so a live shadow-arm comparison cannot
// evidence this site). This offline oracle is its evidence instead.
//
// SPEC used, address-cited throughout: tmp/decomp_sim/llm_strat_load_base_layout_dmp_004d8839.asm
// (no Ghidra .c draft exists for this function) cross-read against the header banner in
// sim/sim_load_base_layout_dmp.h, which already carries the per-branch address citations this file
// echoes in its check messages.
//
#include "sim/sim_load_base_layout_dmp.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- byte-cursor buffer builder -------------------------------------------------------------------
// Mirrors the .cpp's own read_i32/read_dmp_record cursor walk (int32 count; count x {u16;u8;u8}),
// built by hand, byte by byte, little-endian -- NOT via a struct overlay, since the on-disk record
// has no natural alignment guarantee and this is the same posture the .cpp's own comment (searched
// mh_structs.gen.h for "dmp"/"base_layout": zero hits) takes for the format itself.
void push_i32(std::vector<uint8_t> &buf, int32_t v) {
    buf.push_back((uint8_t)(v & 0xff));
    buf.push_back((uint8_t)((v >> 8) & 0xff));
    buf.push_back((uint8_t)((v >> 16) & 0xff));
    buf.push_back((uint8_t)((v >> 24) & 0xff));
}
void push_record(std::vector<uint8_t> &buf, uint16_t field0, uint8_t field1, uint8_t field2) {
    buf.push_back((uint8_t)(field0 & 0xff));
    buf.push_back((uint8_t)((field0 >> 8) & 0xff));
    buf.push_back(field1);
    buf.push_back(field2);
}

struct rec1 { // SECTION 1 record shape: {building_type, x, y}
    uint16_t building_type;
    uint8_t  x;
    uint8_t  y;
};
struct rec2 { // SECTION 2 record shape: {order_code, x, y}
    uint16_t order_code;
    uint8_t  x;
    uint8_t  y;
};

std::vector<uint8_t> build_buffer(const std::vector<rec1> &r1, const std::vector<rec2> &r2) {
    std::vector<uint8_t> buf;
    push_i32(buf, (int32_t)r1.size());
    for (const auto &r : r1) push_record(buf, r.building_type, r.x, r.y);
    push_i32(buf, (int32_t)r2.size());
    for (const auto &r : r2) push_record(buf, r.order_code, r.x, r.y);
    return buf;
}

// ---- recorder / stub mocks for load_base_layout_dmp_calls ------------------------------------------

std::vector<uint8_t> g_resource_buffer; // the ASSET's bytes; the callee now copies out of it
bool                 g_resource_null      = false;
int32_t              g_get_resource_calls = 0;
const char          *g_last_dmp_path      = nullptr;

// SIMABI-VFS: the mock is `asset_read`, which answers a SIZE QUERY (dst_cap 0) and a COPY. An
// absent asset answers -1 to both, which is what used to be a null pointer. Every call is counted,
// so the counts below say how many times the callee went to the host -- one for a missing asset,
// two for a present one.
int32_t rec_asset_read(const char *name, void *dst, uint32_t dst_cap) {
    ++g_get_resource_calls;
    g_last_dmp_path = name;
    if (g_resource_null) return -1;
    const uint32_t len = (uint32_t)g_resource_buffer.size();
    if (dst_cap == 0) return (int32_t)len;
    const uint32_t n = len < dst_cap ? len : dst_cap;
    if (n != 0) std::memcpy(dst, g_resource_buffer.data(), n);
    return (int32_t)len;
}

// The buffer the callee owns now. Recorded rather than merely malloc'd, because the assertion T11
// exists for -- freed with the buffer's own pointer, not the walked cursor -- is unchanged in shape
// and only changes in whose pointer it is.
std::vector<void *> g_alloc_calls;
std::vector<void *> g_free_calls;
void               *rec_mem_alloc(uint32_t size) {
    void *p = std::malloc(size != 0 ? size : 1);
    g_alloc_calls.push_back(p);
    return p;
}
void rec_mem_free(void *ptr) {
    g_free_calls.push_back(ptr);
    std::free(ptr);
}

struct queue_call {
    int32_t  player;
    int32_t  building_type;
    int16_t  x;
    uint16_t y;
};
std::vector<queue_call> g_queue_calls;
int32_t                 g_queue_return = 0; // arbitrary, must be discarded by the callee -- T-return
int32_t                 rec_bldg_queue_construction(int32_t player, int32_t building_type, int16_t x, uint16_t y) {
    g_queue_calls.push_back({player, building_type, x, y});
    return g_queue_return;
}

struct dist_call {
    int32_t x1, y1, x2, y2;
};
std::vector<dist_call> g_dist_calls;
// Popped in CALL ORDER (i.e. scan order over ai_resource_sites) -- lets a case predict exactly which
// site "wins" the min-search without needing a closed-form distance function.
std::vector<uint32_t> g_dist_results;
uint32_t              rec_toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const size_t idx = g_dist_calls.size();
    g_dist_calls.push_back({x1, y1, x2, y2});
    return idx < g_dist_results.size() ? g_dist_results[idx] : 0u;
}

struct order_call {
    uint32_t x, y, order_code;
    uint16_t player;
};
std::vector<order_call> g_order_calls;
uint32_t                g_order_return = 0; // arbitrary, must be discarded -- T-return
uint32_t                rec_dmp_enqueue_scripted_order(uint32_t p1, uint32_t p2, uint32_t p3, uint16_t p4) {
    g_order_calls.push_back({p1, p2, p3, p4});
    return g_order_return;
}

const load_base_layout_dmp_calls g_calls = {
    &rec_asset_read,
    &rec_mem_alloc,
    &rec_mem_free,
    &rec_bldg_queue_construction,
    &rec_toroidal_dist_sq,
    &rec_dmp_enqueue_scripted_order,
};

constexpr int32_t PLAYER = 3;

void reset_recorders() {
    g_resource_buffer.clear();
    g_resource_null      = false;
    g_get_resource_calls = 0;
    g_last_dmp_path      = nullptr;
    g_alloc_calls.clear();
    g_free_calls.clear();
    g_queue_calls.clear();
    g_queue_return = 0;
    g_dist_calls.clear();
    g_dist_results.clear();
    g_order_calls.clear();
    g_order_return = 0;
}

// Neutral defaults so a case that isn't specifically exercising the MOTHER-skip / build-plan /
// mine-candidate mechanisms doesn't accidentally trip one. 0xffffffff for ai_mother_building_type
// matches the field's own documented "none" sentinel; -1 for the mine-candidate tiers likewise.
// ai_bldg_queue_count defaults to 1 (not 0) so any case that queues a building indexes
// ai_bldg_queue[0] rather than the underflowed ai_bldg_queue[-1] the header's uncertainty (1) flags
// as unguarded in the original -- deliberately not exercised here since nothing pins its behaviour.
player_data &seed_defaults(sim_fixture &fx, int32_t player) {
    player_data &pd               = fx.players[(size_t)player];
    pd.ai_mother_building_type    = 0xffffffffu;
    pd.ai_build_plan_len_and_flag = 0;
    pd.ai_mine_candidate_tier1    = -1;
    pd.ai_mine_candidate_tier2    = -1;
    pd.ai_resource_site_count     = 0;
    pd.ai_bldg_queue_count        = 1;
    return pd;
}

} // namespace

void run_load_base_layout_dmp_tests() {
    sim_fixture fx;
    char        dmp_path[] = "layout.dmp";

    // =================================================================================================
    // T1 -- ABSENT resource: the size query answers -1 -> immediate return. Nothing is allocated
    // and nothing is freed, no section-1/2 processing at all. 0x004d8851-0x004d885b.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_defaults(fx, PLAYER);
        g_resource_null = true;

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_get_resource_calls, 1u,
              "T1: an absent asset costs ONE host call -- the size query, never a load, 0x004d8851");
        ck(g_last_dmp_path == dmp_path, "T1: dmp_path forwarded to asset_read, 0x004d884f-0x004d8851");
        ck_eq((uint32_t)g_alloc_calls.size(), 0u, "T1: absent asset -- nothing allocated");
        ck_eq((uint32_t)g_free_calls.size(), 0u, "T1: absent asset -- nothing freed, 0x004d885b JZ");
        ck_eq((uint32_t)g_queue_calls.size(), 0u, "T1: absent asset -- no section-1 processing at all");
        ck_eq((uint32_t)g_order_calls.size(), 0u, "T1: absent asset -- no section-2 processing at all");
    }

    // =================================================================================================
    // T2 -- Section 1 MOTHER-skip (0x004d88af-0x004d88b9): a record whose building_type ==
    // ai_mother_building_type is skipped (bldg_queue_construction NOT called for it) but the loop
    // still ADVANCES to the next record (0x004d88b9 DEC / 0x004d88bc JMP back) -- proven here by a
    // second, non-mother record that IS processed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd            = seed_defaults(fx, PLAYER);
        pd.ai_mother_building_type = 55;
        g_resource_buffer          = build_buffer({{55, 1, 2}, {99, 7, 8}}, {});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_queue_calls.size(), 1u,
              "T2: MOTHER record skipped, only the non-mother record queues, 0x004d88af-0x004d88b9");
        ck_eq((uint32_t)g_queue_calls[0].building_type, 99u,
              "T2: the queued record is the SECOND (non-mother) one -- proves the loop advanced past the skip");
        ck(g_alloc_calls.size() == 1 && g_free_calls.size() == 1 && g_free_calls[0] == g_alloc_calls[0],
           "T2: the asset buffer is freed once, with the ORIGINAL pointer (not the walked cursor), "
           "0x004d8a90-0x004d8a93");
        ck_eq((uint32_t)g_get_resource_calls, 2u, "T2: a present asset costs two host calls -- query, then copy");
    }

    // =================================================================================================
    // T3 -- Section 1 build-plan removal, PRESENT case (0x004d88c2-0x004d894e / 0x004d88d0-0x004d88fd):
    // building_type found in ai_build_plan[i] -> tail shifts down by one, plan_length -= 1, and the
    // TOP BIT of ai_build_plan_len_and_flag is PRESERVED across the update (seeded set here).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd     = seed_defaults(fx, PLAYER);
        pd.ai_build_plan[0] = 10;
        pd.ai_build_plan[1] = 20;
        pd.ai_build_plan[2] = 30;
        // top bit SET beforehand -- 0x004d88d6: AND EDX,0x80000000 pulls it from the CURRENT value,
        // so this must be non-zero going in for the preservation to be observable.
        pd.ai_build_plan_len_and_flag = 0x80000000u | 3u;

        g_resource_buffer = build_buffer({{20, 5, 6}}, {});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq(pd.ai_build_plan_len_and_flag & 0x7fffffffu, 2u,
              "T3: plan_length decremented 3 -> 2 on a found removal, 0x004d88df-0x004d88e2");
        ck((pd.ai_build_plan_len_and_flag & 0x80000000u) != 0,
           "T3: top ('ring/spiral') bit PRESERVED across the update, 0x004d88d0-0x004d88d6");
        ck_eq((uint32_t)pd.ai_build_plan[0], 10u, "T3: plan[0] untouched (before the removed index)");
        ck_eq((uint32_t)pd.ai_build_plan[1], 30u, "T3: plan[2] shifted down into plan[1], 0x004d88ea-0x004d88fd");
        ck(g_queue_calls.size() == 1 && g_queue_calls[0].building_type == 20 && g_queue_calls[0].x == 5 &&
               g_queue_calls[0].y == 6,
           "T3: bldg_queue_construction(player, building_type, x, y) staged args, 0x004d8954-0x004d895f");
    }

    // =================================================================================================
    // T4 -- Section 1 build-plan removal, NOT-PRESENT case: building_type absent from
    // ai_build_plan[0..plan_length) -> plan array AND plan_length left byte-identical (the removal
    // block 0x004d88d0-0x004d88fd is only reached via the JNZ-not-taken path at 0x004d88ce).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd               = seed_defaults(fx, PLAYER);
        pd.ai_build_plan[0]           = 10;
        pd.ai_build_plan[1]           = 20;
        pd.ai_build_plan[2]           = 30;
        pd.ai_build_plan_len_and_flag = 3; // top bit clear here -- this case doesn't need it set

        g_resource_buffer = build_buffer({{999, 1, 1}}, {}); // 999 not in {10,20,30}

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq(pd.ai_build_plan_len_and_flag & 0x7fffffffu, 3u,
              "T4: not-present -- plan_length unchanged, 0x004d88ce JNZ falls through the whole removal block");
        ck_eq((uint32_t)pd.ai_build_plan[0], 10u, "T4: plan[0] unchanged");
        ck_eq((uint32_t)pd.ai_build_plan[1], 20u, "T4: plan[1] unchanged");
        ck_eq((uint32_t)pd.ai_build_plan[2], 30u, "T4: plan[2] unchanged");
    }

    // =================================================================================================
    // T5 -- Section 1 queue call + flag (0x004d8954-0x004d8984): bldg_queue_construction called with
    // the exact staged (player, building_type, x, y), then ai_bldg_queue[ai_bldg_queue_count-1].status
    // |= 0x20 -- ORing into a status byte that already has OTHER bits set (not overwriting them).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd            = seed_defaults(fx, PLAYER);
        pd.ai_bldg_queue_count     = 5;
        pd.ai_bldg_queue[4].status = 0xC5u; // 0x80|0x40|0x05 -- distinct OTHER bits pre-set

        g_resource_buffer = build_buffer({{42, 12, 34}}, {});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck(g_queue_calls.size() == 1 && g_queue_calls[0].player == PLAYER &&
               g_queue_calls[0].building_type == 42 && g_queue_calls[0].x == 12 &&
               (uint32_t)g_queue_calls[0].y == 34u,
           "T5: bldg_queue_construction(player, building_type, x, y) exact staged args, 0x004d895d-0x004d895f");
        ck_eq((uint32_t)pd.ai_bldg_queue[4].status, 0xE5u,
              "T5: status |= 0x20 ORed in, OTHER bits (0xC5) preserved -> 0xE5, 0x004d897a-0x004d8984");
    }

    // =================================================================================================
    // T6 -- Section 1 mine-candidate site-snap (0x004d898c-0x004d8a32), tier1 match: scans
    // ai_resource_sites[0..count), SKIPS status != 0 entries (zero toroidal_dist_sq calls for them),
    // picks the MINIMUM toroidal_dist_sq among the remaining, writes build_tile_x/y on ONLY that site.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd            = seed_defaults(fx, PLAYER);
        pd.ai_mine_candidate_tier1 = 77;
        pd.ai_mine_candidate_tier2 = -999; // distinct, never matches
        pd.ai_resource_site_count  = 3;
        pd.ai_resource_sites[0]    = {1, 2, 0, -1, -1}; // status 0 -- candidate
        pd.ai_resource_sites[1]    = {3, 4, 0, -1, -1}; // status 0 -- candidate, WINS (dist 50 < 100)
        pd.ai_resource_sites[2]    = {5, 6, 5, -1, -1}; // status != 0 -- SKIPPED, no dist call at all

        g_resource_buffer = build_buffer({{77, 9, 11}}, {});
        g_dist_results    = {100u, 50u}; // call order = site scan order 0, 1 (site 2 never called)

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_dist_calls.size(), 2u,
              "T6: exactly 2 dist calls -- site[2] (status!=0) skipped entirely, 0x004d89b8-0x004d89c0 JNZ");
        ck(g_dist_calls[0].x1 == 9 && g_dist_calls[0].y1 == 11 && g_dist_calls[0].x2 == 1 * 4 &&
               g_dist_calls[0].y2 == 2 * 4,
           "T6: toroidal_dist_sq(x, y, site.grid_x*4, site.grid_y*4) for site[0], 0x004d89c2-0x004d89de");
        ck(g_dist_calls[1].x1 == 9 && g_dist_calls[1].y1 == 11 && g_dist_calls[1].x2 == 3 * 4 &&
               g_dist_calls[1].y2 == 4 * 4,
           "T6: same call shape for site[1]");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[1].build_tile_x, 9u,
              "T6: build_tile_x written on the WINNING (min-dist) site[1] only, 0x004d8a1e-0x004d8a2b");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[1].build_tile_y, 11u, "T6: build_tile_y likewise");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[0].build_tile_x, 0xffffu,
              "T6: the LOSING site[0] left untouched (still the -1 sentinel seeded)");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[2].build_tile_x, 0xffffu,
              "T6: the SKIPPED site[2] left untouched");
    }

    // =================================================================================================
    // T7 -- mine-candidate site-snap: building_type is NEITHER tier1 NOR tier2 -> the whole scan is
    // skipped, ZERO toroidal_dist_sq calls (0x004d898c-0x004d899d falls through to the loop-back
    // JNZ, never reaching 0x004d89a3's scan setup).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd            = seed_defaults(fx, PLAYER);
        pd.ai_mine_candidate_tier1 = 77;
        pd.ai_mine_candidate_tier2 = 88;
        pd.ai_resource_site_count  = 2;
        pd.ai_resource_sites[0]    = {1, 2, 0, -1, -1};
        pd.ai_resource_sites[1]    = {3, 4, 0, -1, -1};

        g_resource_buffer = build_buffer({{200, 9, 11}}, {}); // 200 != 77, != 88

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_dist_calls.size(), 0u,
              "T7: building_type matches neither mine-candidate tier -- no site-snap scan at all, 0x004d898c-0x004d899d");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[0].build_tile_x, 0xffffu, "T7: sites left untouched");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[1].build_tile_x, 0xffffu, "T7: sites left untouched");
    }

    // =================================================================================================
    // T8 -- mine-candidate site-snap: every candidate site has status != 0 -> best_index stays -1, no
    // site written at all (0x004d8a0e-0x004d8a12: CMP [best_index],-1 / JZ skip).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd            = seed_defaults(fx, PLAYER);
        pd.ai_mine_candidate_tier1 = 77;
        pd.ai_mine_candidate_tier2 = -999;
        pd.ai_resource_site_count  = 2;
        pd.ai_resource_sites[0]    = {1, 2, 1, -1, -1}; // status != 0
        pd.ai_resource_sites[1]    = {3, 4, 7, -1, -1}; // status != 0

        g_resource_buffer = build_buffer({{77, 9, 11}}, {});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_dist_calls.size(), 0u, "T8: every site status!=0 -- no dist calls, all skipped");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[0].build_tile_x, 0xffffu,
              "T8: best_index stays -1 -- site[0] left untouched, 0x004d8a0e-0x004d8a12");
        ck_eq((uint32_t)(uint16_t)pd.ai_resource_sites[1].build_tile_x, 0xffffu,
              "T8: site[1] left untouched too");
    }

    // =================================================================================================
    // T9 -- the section 1/2 transition (0x004d8a37-0x004d8a4d): once count1 is exhausted (here: 0
    // records, so the transition runs immediately), ai_build_plan_cursor is set to 0 and the TOP BIT
    // of ai_build_plan_len_and_flag is SET even though it started CLEAR. Also folds in the count2==0
    // case: zero dmp_enqueue_scripted_order calls.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd               = seed_defaults(fx, PLAYER);
        pd.ai_build_plan_cursor       = 7; // nonzero beforehand
        pd.ai_build_plan_len_and_flag = 5; // top bit CLEAR beforehand, low31 = 5

        g_resource_buffer = build_buffer({}, {}); // count1 = 0, count2 = 0

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)pd.ai_build_plan_cursor, 0u, "T9: ai_build_plan_cursor set to 0, 0x004d8a37");
        ck((pd.ai_build_plan_len_and_flag & 0x80000000u) != 0,
           "T9: top bit SET on the transition even though it started clear, 0x004d8a41 OR 0x80");
        ck_eq(pd.ai_build_plan_len_and_flag & 0x7fffffffu, 5u, "T9: low 31 bits (length) untouched by the OR");
        ck_eq((uint32_t)g_order_calls.size(), 0u, "T9: count2 == 0 -- dmp_enqueue_scripted_order never called");
    }

    // =================================================================================================
    // T10 -- Section 2 (0x004d8a50-0x004d8a8e): each record calls dmp_enqueue_scripted_order(x, y,
    // order_code, (uint16_t)player) with the EXACT argument mapping, and multiple records are
    // processed IN ORDER -- distinct x/y/order_code per record catches a mixed-up mapping.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_defaults(fx, PLAYER);

        g_resource_buffer = build_buffer({}, {{0x1111, 11, 22}, {0x2222, 33, 44}});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_order_calls.size(), 2u, "T10: count2 == 2 -- exactly 2 calls, in order");
        ck(g_order_calls[0].x == 11 && g_order_calls[0].y == 22 && g_order_calls[0].order_code == 0x1111 &&
               g_order_calls[0].player == (uint16_t)PLAYER,
           "T10: record 0 -- dmp_enqueue_scripted_order(x=11, y=22, order_code=0x1111, player), "
           "param mapping confirmed from the pre-CALL register loads, 0x004d8a7a-0x004d8a86");
        ck(g_order_calls[1].x == 33 && g_order_calls[1].y == 44 && g_order_calls[1].order_code == 0x2222 &&
               g_order_calls[1].player == (uint16_t)PLAYER,
           "T10: record 1 processed AFTER record 0, same mapping -- proves iteration order");
    }

    // =================================================================================================
    // T11 -- the asset buffer is freed EXACTLY once, with the pointer the allocation returned
    // (0x004d8a90-0x004d8a93), even after the cursor has walked all the way through both sections.
    // SIMABI-VFS moved whose buffer that is -- the host's, once; ours, now -- and the mistake the
    // test guards against (freeing the WALKED CURSOR) is exactly as available as it ever was.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_defaults(fx, PLAYER);

        g_resource_buffer = build_buffer({{1, 1, 1}}, {{2, 2, 2}});

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_alloc_calls.size(), 1u, "T11: the asset buffer is allocated exactly once");
        ck_eq((uint32_t)g_free_calls.size(), 1u, "T11: it is freed exactly once");
        ck(g_free_calls[0] == g_alloc_calls[0],
           "T11: freed with the ORIGINAL buffer pointer, NOT the walked cursor, 0x004d8a90-0x004d8a93");
    }

    // =================================================================================================
    // T12 -- return-value discarding: bldg_queue_construction and dmp_enqueue_scripted_order return
    // arbitrary (nonzero/negative) values and the function must not react to them -- both sections
    // keep processing every record regardless (no early exit branch on either return value).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        player_data &pd        = seed_defaults(fx, PLAYER);
        pd.ai_bldg_queue_count = 3;
        g_queue_return         = -1;          // arbitrary negative
        g_order_return         = 0xdeadbeefu; // arbitrary nonzero

        g_resource_buffer =
            build_buffer({{5, 1, 1}, {6, 2, 2}}, {{7, 3, 3}, {8, 4, 4}}); // 2 + 2 records

        sim_store own = fx.store();
        detail::load_base_layout_dmp(fx.view(), own, PLAYER, dmp_path, g_calls);

        ck_eq((uint32_t)g_queue_calls.size(), 2u,
              "T12: both section-1 records processed despite a discarded negative return value");
        ck_eq((uint32_t)g_order_calls.size(), 2u,
              "T12: both section-2 records processed despite a discarded nonzero return value");
    }
}

} // namespace mh::sim::test
