//
// sim_unit_fire_weapon_selftest.cpp -- `simtest` cases for the SIM1E verification-debt pair:
//   llm_strat_unit_fire_weapon               @0x0048bb6c (sim/sim_unit_fire_weapon.h/.cpp)
//   llm_strat_unit_fire_at_target2_if_aimed  @0x004863e6 (sim/sim_unit_fire_at_target2_if_aimed.h/.cpp)
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY -- NOT from the .cpp bodies:
//   tmp/decomp_sim/llm_strat_unit_fire_weapon_0048bb6c.asm (2852 bytes, read address-by-address for
//   this file: the loop-bound/preserved-bug setup at 0x0048bb8d-0x0048bc33, the enabled_2/enabled/
//   matched_count gate at 0x0048bc48-0x0048bc92, the own_class/target_class computation at
//   0x0048bcb4-0x0048bd21, the weapon-target-mask test at 0x0048bd21-0x0048bd57, the range-check gate
//   at 0x0048bd57-0x0048bde2 (incl. the register/arg trace for llm_strat_dist_out_of_range), the switch
//   dispatch at 0x0048be02-0x0048be28, the ammo/pocket/reload bookkeeping at 0x0048c1f4-0x0048c33d /
//   0x0048c53b-0x0048c67d, and the "no eligible weapon" ending at 0x0048c689-0x0048c6a9) and
//   tmp/decomp/llm_strat_unit_fire_at_target2_if_aimed_004863e6.asm (0x120 bytes, read in full).
//
// ---- THE FIRE-MODE SWITCH BODIES (weapon types 1/2/3/4/5/6/8) ARE NOW EXERCISED (2026-08-19 seam) ---
// They used to be un-coverable, which capped this hub at T3. Every non-default switch arm's first action
// is `recompute_mount_or_soldier_pos`, and the arm bodies then call projectile_spawn / fx_anim_spawn /
// weapon_scatter_offset / apply_area_damage. Originally all eight of those were DIRECT `mh::sim::` sibling
// calls to state()-using PUBLIC wrappers that dispatch into their own live `_calls` tables of raw
// `mh::call::` VAs (0x0048c83f / 0x0048ccb4 / 0x0048ce21 for the mount/soldier trio, and the
// projectile/fx/scatter/area wrappers likewise) -- addresses `net_selftest.exe` never maps, so entering
// any arm ACCESS-VIOLATED the whole process. The SIM1E fire_weapon T3->T1 slice routed those eight calls
// through `unit_fire_weapon_calls` (bound to the SAME public wrappers in the live path -- production
// behaviour-identical, the invasion_calls precedent), so this file can now STUB them: section 8 onward
// drives real switch arms with recorder stubs and asserts the exact argument tuples. bldg_get_coords
// stays a direct sibling call (pure, reached only on the building-target range-check path, section 7)
// and is still handled by region-rebasing rather than a stub. See sim_unit_fire_weapon.h's struct
// comment for the seam rationale.
//
// SCOPE:
//   COVERS -- the full weapon-slot ELIGIBILITY spine: the loop-bound formula and the PRESERVED BUG
//   (weapon_slot_select's value affects only the bound, never the starting slot -- 0x0048bb8d-
//   0x0048bc33), the enabled_2/enabled/matched_count gate order (0x0048bc48-0x0048bc92), the
//   target_class conversion from a live-unit target's elevation incl. the "0xa0 bit set but elevation
//   zero" edge (0x0048bce9-0x0048bd21), the weapon-target-mask gate on both GROUND/AIR senses
//   (0x0048bd21-0x0048bd57), the range-check gate's presence/absence by loop_bound and its exact
//   argument order/values on BOTH senses of `dist_out_of_range`'s return (0x0048bd57-0x0048bde2), the
//   range-check-only tile-coordinate resolution on BOTH the building-target and non-building-target
//   arms incl. the PIXEL-space-mask-vs-tile-space-mask distinction and truncate-toward-zero on a
//   negative coordinate (0x0048bbb1-0x0048c22), the switch's DEFAULT arm for type 0/7/9
//   (0x0048be02-0x0048be28, 0x0048c684), and the "no eligible weapon" ending
//   (0x0048c689-0x0048c6a9) incl. that a nonzero matched_count leaves the selection untouched and that
//   neighbouring weapon slots / a different unit's roster record are never written.
//   AND (section 8+): the fire-mode switch arm BODIES -- the mount recompute (calc_mount X/Y/render) and
//   its soldier-count!=0 alternative (rand_below + soldier_screen_pos), projectile_spawn's full 12-arg
//   tuple, the SIM_ACTIVE sound gating, the ammo/pocket/reload bookkeeping on all three ammo==0
//   sub-branches (pocket>0 / pocket->0-disables / pocket==-1-infinite), the case-2/6 double-projectile
//   fallthrough, case 4->3's muzzle/impact fx_anim_spawn arg shapes + own_class-vs-target_class, the
//   scatter forward, the pixel-mask impact math, apply_area_damage's arg tuple incl. the trunc'd
//   ring_count, and -- the M24 discriminator -- that consume_ammo_and_reload writes the FIRED slot's
//   pocket, not slot 0's.
//   fire_at_target2_if_aimed: FULL coverage -- both circular-tolerance booleans on every boundary
//   (JLE/JG/JL senses at 0x00486465/0x00486470/0x00486484 and their check_b mirrors), the documented
//   wraparound example, the exact get_coords/dir_from_to argument forwarding, the tol/facing field
//   reads, and the exact 7-argument forward (incl. the target2_ref/target2_index zero-extension) to
//   fire_weapon on a firing case, plus that NOTHING fires when the aim test fails.
//
#include "sim/sim_unit_fire_at_target.h"
#include "sim/sim_unit_fire_at_target2_if_aimed.h"
#include "sim/sim_unit_fire_weapon.h"

#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across both functions' recorders --------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// =====================================================================================================
// ==== llm_strat_unit_fire_weapon recorders/fixtures =================================================
// =====================================================================================================

struct DistCall {
    int32_t range_min, range_max, x, y, target_tile_x, target_tile_y;
};
std::vector<DistCall> g_dist_calls;
uint32_t              g_dist_ret = 1; // DEFAULT POSTURE: "out of range" -- see the file banner's
                                      // mutation-safety note. Cases that want the "in range" sense
                                      // set this to 0 AND land in the switch's safe default arm.
uint32_t rec_dist_out_of_range(int32_t range_min, int32_t range_max, int32_t x, int32_t y,
                               int32_t target_tile_x, int32_t target_tile_y) {
    tr("dist_out_of_range");
    g_dist_calls.push_back({range_min, range_max, x, y, target_tile_x, target_tile_y});
    return g_dist_ret;
}

// The remaining four callees are never reached by any case in this file (every case either stops
// before the switch or lands in the safe default arm) -- but they still need SAFE bodies, not null
// pointers, so that an unexpected reach (e.g. a mutation that flips the range-check sense while type
// still lands on a non-default arm -- an inherent residual risk this file cannot close, see DECLARED
// NEEDS) fails a NAMED check via the trace rather than calling through a null pointer.
struct DirCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirCall> g_dir_calls;
int32_t              g_dir_from_to_calls = 0;
int32_t              g_dir_from_to_ret   = 1; // KNOB: the "facing" the fx-anim frame math uses.
int32_t              rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    ++g_dir_from_to_calls;
    g_dir_calls.push_back({x1, y1, x2, y2});
    return g_dir_from_to_ret;
}
int32_t g_rand_below_calls = 0;
int32_t rec_rand_below(int32_t) {
    tr("rand_below");
    ++g_rand_below_calls;
    return 0;
}
struct SndAtCall {
    int32_t sound_id, col, row;
};
std::vector<SndAtCall> g_snd_calls;
void                   rec_snd_play_at(int32_t sound_id, int32_t tile_col, int32_t tile_row) {
    tr("snd_play_at");
    g_snd_calls.push_back({sound_id, tile_col, tile_row});
}

// ---- the eight SEAMED SIBLINGS (2026-08-19 fire_weapon T3->T1 slice) ------------------------------
// These are the switch-arm outward calls now routed through unit_fire_weapon_calls (see the file
// banner + sim_unit_fire_weapon.h's struct comment). Stubbing them here is what makes the fire-mode
// switch arms reachable offline. Each stub RECORDS its call and returns a distinct, non-symmetric
// controlled value so a swapped/dropped argument is observable.
struct MountCall {
    uint16_t player;
    int32_t  unit_idx;
    uint32_t mount_idx;
    char     axis_is_x;
};
std::vector<MountCall> g_mount_fine_calls, g_mount_render_calls;
// Distinct per axis so a mount_x/mount_y swap in the recompute is caught; render is its own value.
constexpr uint32_t MOUNT_FINE_X = 700; // returned for axis_is_x != 0
constexpr uint32_t MOUNT_FINE_Y = 800; // returned for axis_is_x == 0
constexpr int32_t  MOUNT_RENDER = 900;
uint32_t           rec_calc_mount_fine_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    tr("calc_mount_fine_pos");
    g_mount_fine_calls.push_back({player, unit_idx, mount_idx, axis_is_x});
    return axis_is_x != 0 ? MOUNT_FINE_X : MOUNT_FINE_Y;
}
int32_t rec_calc_mount_render_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x) {
    tr("calc_mount_render_pos");
    g_mount_render_calls.push_back({player, unit_idx, mount_idx, axis_is_x});
    return MOUNT_RENDER;
}
struct SoldierCall {
    uint16_t player;
    int32_t  unit_idx;
    int32_t  hop;
};
std::vector<SoldierCall> g_soldier_calls;
constexpr uint32_t       SOLDIER_X = 710, SOLDIER_Y = 810; // distinct from the mount values above
void                     rec_soldier_screen_pos(uint16_t player, int32_t unit_idx, int32_t hop, uint32_t *out_x, uint32_t *out_y) {
    tr("soldier_screen_pos");
    g_soldier_calls.push_back({player, unit_idx, hop});
    *out_x = SOLDIER_X;
    *out_y = SOLDIER_Y;
}
struct ProjCall {
    int32_t  src_x, src_y_ground, src_y_vis_offset;
    uint32_t dst_x, dst_y_ground;
    int32_t  dst_y_vis_offset, weapon_id;
    uint8_t  owner_player;
    uint16_t homing_player_and_flags;
    int32_t  homing_target_unit;
    uint32_t shooter_ref;
    int32_t  shooter_unit_index;
};
std::vector<ProjCall> g_proj_calls;
int32_t               rec_projectile_spawn(int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset,
                                           uint32_t dst_x, uint32_t dst_y_ground, int32_t dst_y_vis_offset,
                                           int32_t weapon_id, uint8_t owner_player, uint16_t homing_player_and_flags,
                                           int32_t homing_target_unit, uint32_t shooter_ref, int32_t shooter_unit_index) {
    tr("projectile_spawn");
    g_proj_calls.push_back({src_x, src_y_ground, src_y_vis_offset, dst_x, dst_y_ground, dst_y_vis_offset,
                                          weapon_id, owner_player, homing_player_and_flags, homing_target_unit, shooter_ref,
                                          shooter_unit_index});
    return 0;
}
std::vector<int32_t> g_stride_calls;
int32_t              g_stride_ret = 3; // KNOB: the per-direction anim-frame chain length
int32_t              rec_fx_anim_dir_frame_stride(int32_t start_frame) {
    tr("fx_anim_dir_frame_stride");
    g_stride_calls.push_back(start_frame);
    return g_stride_ret;
}
struct FxSpawnCall {
    uint32_t x, y, anim_frame_id;
    double   elapsed;
    uint32_t layer;
};
std::vector<FxSpawnCall> g_fx_calls;
uint32_t                 rec_fx_anim_spawn(uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed, uint32_t layer) {
    tr("fx_anim_spawn");
    g_fx_calls.push_back({x, y, anim_frame_id, elapsed, layer});
    return 0;
}
struct ScatterCall {
    int32_t  p1, p2;
    uint32_t a2, p4, p5, p6;
};
std::vector<ScatterCall> g_scatter_calls;
constexpr int32_t        SCATTER_DX = 5, SCATTER_DY = -7; // written to the two OUT-pointers
void                     rec_weapon_scatter_offset(int32_t p1, int32_t p2, uint32_t a2, uint32_t p4, uint32_t p5,
                                                   uint32_t p6, int32_t *out_dx, int32_t *out_dy) {
    tr("weapon_scatter_offset");
    g_scatter_calls.push_back({p1, p2, a2, p4, p5, p6});
    *out_dx = SCATTER_DX;
    *out_dy = SCATTER_DY;
}
struct AreaCall {
    int32_t  x, y, target_kind;
    double   damage;
    int32_t  ring_count;
    uint32_t owner_filter_zeroed, killer_info;
    int32_t  killer_unit_index;
};
std::vector<AreaCall> g_area_calls;
void                  rec_apply_area_damage(int32_t x, int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                                            uint32_t owner_filter_zeroed, uint32_t killer_info, int32_t killer_unit_index) {
    tr("apply_area_damage");
    g_area_calls.push_back({x, y, target_kind, damage, ring_count, owner_filter_zeroed, killer_info, killer_unit_index});
}

const unit_fire_weapon_calls &rec_fw_calls() {
    static const unit_fire_weapon_calls c = {
        &rec_dist_out_of_range,
        &rec_dir_from_to,
        &rec_rand_below,
        &rec_snd_play_at,
        &rec_calc_mount_fine_pos,
        &rec_calc_mount_render_pos,
        &rec_soldier_screen_pos,
        &rec_projectile_spawn,
        &rec_fx_anim_dir_frame_stride,
        &rec_fx_anim_spawn,
        &rec_weapon_scatter_offset,
        &rec_apply_area_damage,
    };
    return c;
}

void reset_fw_observations() {
    g_trace.clear();
    g_dist_calls.clear();
    g_dist_ret          = 1;
    g_dir_from_to_calls = 0;
    g_dir_calls.clear();
    g_rand_below_calls = 0;
    g_snd_calls.clear();
    // The seamed-sibling recorders. Clear OBSERVATIONS here; the knobs (g_dir_from_to_ret,
    // g_stride_ret, MOUNT_*/SOLDIER_*/SCATTER_* constants) are set by the seeder / are compile-time
    // constants and are deliberately NOT reset here -- see reset_f2_observations' own note on why a
    // knob cleared in the reset helper silently defeats every case that set it beforehand.
    g_mount_fine_calls.clear();
    g_mount_render_calls.clear();
    g_soldier_calls.clear();
    g_proj_calls.clear();
    g_stride_calls.clear();
    g_fx_calls.clear();
    g_scatter_calls.clear();
    g_area_calls.clear();
}

// ---- live_regions_on_fixture: the SAME 7-region redirect sim_weapon_damage_selftest.cpp's own
// instance uses for this exact hazard, scoped down (fire_weapon's own bldg_get_coords call touches
// only BUILDINGS/BUILDING/GENERAL, plus the 4 cur_* pointer-globals `state()` ALWAYS dereferences to
// bind cur_unit/cur_building/cur_projectile/cur_fx_anim regardless of what the callee needs -- see that
// file's own banner for the full derivation of why those four are unconditional). Needed ONLY for the
// ONE case below that drives the target_ref&0x40 (building-target) branch of the range-check-only tile
// resolution, which is the ONE place in fire_weapon's eligibility spine that reaches a `state()`-based
// public wrapper (`mh::sim::bldg_get_coords`) -- and unlike the mount/soldier-pos siblings, bldg_get_
// coords is PURE (no outward call of its own, per its header banner), so the region redirect alone
// (no raw-VA hazard) makes it safe.
constexpr mh::state::region_id FW_REDIRECTED[] = {
    mh::state::RID_STRAT_CUR_UNIT,
    mh::state::RID_STRAT_CUR_BUILDING,
    mh::state::RID_STRAT_CUR_PROJECTILE,
    mh::state::RID_STRAT_CUR_FX_ANIM,
    mh::state::RID_GENERAL,
    mh::state::RID_BUILDINGS,
    mh::state::RID_BUILDING,
};
class live_regions_on_fixture {
public:
    explicit live_regions_on_fixture(sim_fixture &f)
        : cur_unit_(f.cur_unit_ptr), cur_building_(f.cur_building_ptr),
          cur_projectile_(f.cur_projectile_ptr), cur_fx_anim_(f.cur_fx_anim_ptr) {
        bind(mh::state::RID_STRAT_CUR_UNIT, &cur_unit_, sizeof(cur_unit_));
        bind(mh::state::RID_STRAT_CUR_BUILDING, &cur_building_, sizeof(cur_building_));
        bind(mh::state::RID_STRAT_CUR_PROJECTILE, &cur_projectile_, sizeof(cur_projectile_));
        bind(mh::state::RID_STRAT_CUR_FX_ANIM, &cur_fx_anim_, sizeof(cur_fx_anim_));
        bind(mh::state::RID_GENERAL, &f.geom, sizeof(map_geom));
        bind(mh::state::RID_BUILDINGS, f.buildings.data(), f.buildings.size() * sizeof(building));
        bind(mh::state::RID_BUILDING, f.cfg_buildings.data(), f.cfg_buildings.size() * sizeof(cfg_building));
    }
    ~live_regions_on_fixture() {
        for (mh::state::region_id r : FW_REDIRECTED) mh::state::unrebase(r);
    }
    live_regions_on_fixture(const live_regions_on_fixture &)            = delete;
    live_regions_on_fixture &operator=(const live_regions_on_fixture &) = delete;

private:
    static void bind(mh::state::region_id r, void *p, size_t n) {
        mh::state::rebase(r, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)),
                          static_cast<uint32_t>(n));
    }
    unit       *cur_unit_;
    building   *cur_building_;
    projectile *cur_projectile_;
    fx_anim    *cur_fx_anim_;
};

// ---- the cast: distinct, non-zero, non-symmetric per sim_test_support.h's own rule --------------
constexpr uint32_t FW_PLAYER      = 2;
constexpr int32_t  FW_UNIT_IDX    = 5;
constexpr uint32_t OTHER_PLAYER   = 6;
constexpr int32_t  OTHER_UNIT_IDX = 11;
constexpr uint8_t  WID            = 5; // weapons[0].weapon_id -> cfg_weapons[WID]

// Seeds the firing unit's slot 0 with a weapon that will pass BOTH the enabled_2/enabled gate and
// (unless the caller changes .target) the eligibility mask, plus the firing unit's own tile position.
// `sim_fixture fx` is constructed ONCE and reused across every case below (no `fx.reset()` between
// them, matching sim_bldg_state_destroyed_selftest.cpp's own per-case fixture but WITHOUT that file's
// full reset -- so this function must UNCONDITIONALLY zero all four weapon slots, not just seed slot 0,
// or a later case would silently inherit an earlier case's slot 1/2/3 state. (Caught while writing test
// 2 below: test 1d leaves weapons[3].enabled_2=1, which would otherwise make test 2's "no eligible
// weapon" case see matched_count=1 instead of 0.)
unit &seed_fw_unit(sim_fixture &f) {
    unit &u = f.u((int32_t)FW_PLAYER, FW_UNIT_IDX);
    for (auto &ws : u.weapons) ws = {};
    u.x                       = 12;
    u.y                       = 34;
    u.elevation               = 0;
    u.selected_weapon         = 77; // sentinel != 100, so a spurious reset is observable
    u.weapons[0].weapon_id    = WID;
    u.weapons[0].enabled_2    = 1;
    u.weapons[0].enabled      = 1;
    u.weapons[0].ammo         = 1234; // sentinels no reachable path writes -- see the "untouched" case
    u.weapons[0].pocket       = 5678;
    u.weapons[0].reload_timer = 9.5;
    return u;
}

// ==== 1. LOOP BOUND + THE PRESERVED BUG (0x0048bb8d-0x0048bc33) =====================================
//
// weapon_slot_select==4 -> loop_bound=4 (0x0048bb93-0x0048bba1). weapon_slot_select!=4 -> loop_bound=1
// (0x0048bba3-0x0048bbae) -- BUT the starting slot is UNCONDITIONALLY reset to 0 at 0x0048bc2c, AFTER
// weapon_slot_select was copied into the same stack slot, so a specific-slot request always inspects
// slot 0, never the requested slot (see sim_unit_fire_weapon.h's "PRESERVED BUG" banner). Every slot
// used here is `enabled_2=1, enabled=0` -- passes the FIRST gate (so `matched_count` proves the slot was
// really visited) and fails the SECOND (so it is always a safe continue, independent of loop_bound: if a
// mutation somehow made loop_bound wrong, extra slots still only ever "continue").
void run_fw_tests() {
    sim_fixture fx;

    // ---- 1a: weapon_slot_select==4 visits ALL FOUR slots (loop_bound==4) ----------------------------
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 0; i < 4; ++i) {
            u.weapons[i].enabled_2 = 1;
            u.weapons[i].enabled   = 0; // matched, but never fires -- safe
        }
        u.weapons[0].enabled_2 = 0; // slot 0 itself is the ONE excluded slot -- see below
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0, 0, 0, 0);
        // matched_count counts slots 1,2,3 (enabled_2=1) but NOT slot 0 (enabled_2=0 here) -> nonzero
        // -> selected_weapon is left alone. If loop_bound were wrongly < 4, slot 3 would never be
        // reached but slots 1/2 alone still leave matched_count nonzero -- see case 1c below, which
        // isolates slot 3 specifically to pin the bound is truly 4, not merely >1.
        ck_eq((uint32_t)u.selected_weapon, 77u,
              "fw 1a (0x0048bb93 loop_bound=4): matched_count>0 from slots 1-3 -> selected_weapon untouched");
    }

    // ---- 1b: weapon_slot_select!=4 (e.g. 2) visits ONLY slot 0, never the requested slot -----------
    {
        unit &u                = seed_fw_unit(fx);
        u.weapons[0].enabled_2 = 0; // slot 0 (the ACTUALLY visited slot per the preserved bug): no match
        u.weapons[2].enabled_2 = 1; // the REQUESTED slot: WOULD match if the bug were absent
        u.weapons[2].enabled   = 0; // (still a safe continue either way)
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 2, 0, 0, 0, 0);
        // Bug present (as coded): only slot 0 is visited, enabled_2==0 there -> matched_count==0 ->
        // selected_weapon RESET to 100. A translation that seeded the loop's start index from
        // weapon_slot_select (bug "fixed") would instead visit slot 2 (enabled_2==1) -> matched_count==1
        // -> selected_weapon left at 77 -- this is the discriminating observable.
        ck_eq((uint32_t)u.selected_weapon, 100u,
              "fw 1b (0x0048bc2c PRESERVED BUG): weapon_slot_select=2 still inspects slot 0 (whose "
              "enabled_2=0), never slot 2 (whose enabled_2=1) -- selected_weapon reset to 100 proves it");
    }

    // ---- 1c: weapon_slot_select!=4 visits EXACTLY ONE slot (loop_bound==1, not >1) -----------------
    {
        unit &u                = seed_fw_unit(fx);
        u.weapons[0].enabled_2 = 0; // slot 0 (visited): no match
        u.weapons[1].enabled_2 = 1; // if loop_bound were wrongly >1, slot 1 would ALSO be inspected
        u.weapons[1].enabled   = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 3, 0, 0, 0, 0);
        ck_eq((uint32_t)u.selected_weapon, 100u,
              "fw 1c (0x0048bb93/0x0048bc36 loop_bound=1): weapon_slot_select=3 inspects ONLY slot 0 -- "
              "slot 1's enabled_2=1 is never reached, so matched_count stays 0");
    }

    // ---- 1d: weapon_slot_select==4 really reaches slot index 3, not just >1 ------------------------
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 0; i < 3; ++i) u.weapons[i].enabled_2 = 0; // slots 0,1,2: no match
        u.weapons[3].enabled_2 = 1;                             // ONLY the last slot matches
        u.weapons[3].enabled   = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0, 0, 0, 0);
        ck_eq((uint32_t)u.selected_weapon, 77u,
              "fw 1d (0x0048bc36 loop bound truly reaches slot 3): only slot 3 has enabled_2=1 -- "
              "selected_weapon untouched proves the loop got there");
    }

    // ==== 2. THE "NO ELIGIBLE WEAPON" ENDING (0x0048c689-0x0048c6a9) ================================
    //
    // matched_count==0 (no slot ever had enabled_2!=0) -> selected_weapon forced to 100 (0x0048c6a2, the
    // literal 0x64 immediate). Also checks neighbouring weapon-slot fields and a DIFFERENT unit's own
    // roster record are untouched -- nothing on any safe-continue path writes state.
    {
        unit &u                = seed_fw_unit(fx);
        u.weapons[0].enabled_2 = 0; // the only configured slot now fails immediately -- no match at all
        unit &other            = fx.u((int32_t)OTHER_PLAYER, OTHER_UNIT_IDX);
        other.selected_weapon  = 42;
        other.weapons[0].ammo  = 999;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0, 0, 0, 0);
        ck_eq((uint32_t)u.selected_weapon, 100u,
              "fw 2a (0x0048c6a2 MOV byte,0x64): matched_count==0 -> selected_weapon = 100 (0x64)");
        ck_eq((uint32_t)u.weapons[0].ammo, 1234u, "fw 2a: weapons[0].ammo untouched on the no-fire path");
        ck_eq((uint32_t)u.weapons[0].pocket, 5678u, "fw 2a: weapons[0].pocket untouched on the no-fire path");
        ck_eq_d(u.weapons[0].reload_timer, 9.5, "fw 2a: weapons[0].reload_timer untouched on the no-fire path");
        ck(g_trace.empty(), "fw 2a: no `_calls` member fires at all when the very first gate fails");
        ck_eq((uint32_t)other.selected_weapon, 42u,
              "fw 2a: a DIFFERENT unit's selected_weapon is never touched (wrong-unit-index guard)");
        ck_eq((uint32_t)other.weapons[0].ammo, 999u, "fw 2a: a different unit's weapon slot is never touched");
    }

    // ==== 3. TARGET_CLASS CONVERSION + THE WEAPON-TARGET-MASK GATE (0x0048bce9-0x0048bd57) ==========
    //
    // Default posture for this whole section: weapon_slot_select=4 (loop_bound=4, so the range check
    // runs) and g_dist_ret=1 ("out of range") -- so if the mask gate is ever mutated to wrongly pass,
    // the range check is the independent second net that still stops it before the switch (see the file
    // banner). target_ref never carries bit 0x40, so no bldg_get_coords/region-redirect is needed here.
    {
        // 3a: no live-unit-target bits (target_ref&0xa0==0) -> target_class stays 1 (GROUND), regardless
        // of the target's own elevation. weapon.target=GROUND-only -> mask PASSES -> range check runs.
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target               = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].range_min[FW_PLAYER] = 100;
        fx.cfg_weapons[WID].range_max[FW_PLAYER] = 300;
        unit &live_target                        = fx.u(1, 7); // target_ref's owner nibble = 1, index 7
        live_target.elevation                    = 999;        // would flip target_class to AIR IF 0xa0 were set
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        // target_ref=0x01 (owner nibble 1, no 0x40/0xa0 bits), target_index=7, fine (640,960) -> tile (20,30).
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x01, 7, 640, 960);
        ck_eq((uint32_t)g_dist_calls.size(), 1u,
              "fw 3a (0x0048bce9 TEST word,0xa0 -> JZ, target_class stays 1): GROUND weapon vs GROUND "
              "target_class -> mask passes -> dist_out_of_range IS reached");
        if (!g_dist_calls.empty()) {
            const auto &d = g_dist_calls[0];
            ck(d.range_min == 99 && d.range_max == 301 && d.x == 12 && d.y == 34,
               "fw 3a (0x0048bdb0/0x0048bdca args): dist_out_of_range(range_min-1=99, range_max+1=301, "
               "u.x=12, u.y=34, ...)");
            ck(d.target_tile_x == 20 && d.target_tile_y == 30,
               "fw 3a (0x0048bbfd-0x0048bc22, not-a-building arm): target_tile = fine_to_tile(640,960) = "
               "(20,30), raw target_fine_x/y used directly (no bldg_get_coords call for target_ref&0x40==0)");
        }

        // 3b: same target_class=GROUND, but weapon.target=AIR-only -> mask FAILS -> no dist call at all.
        u.weapons[0].enabled_2     = 1; // re-seed (seed_fw_unit already set it, kept explicit for clarity)
        fx.cfg_weapons[WID].target = WEAPON_TARGET_AIR;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x01, 7, 640, 960);
        ck(g_dist_calls.empty(),
           "fw 3b (0x0048bd21 CMP AL,1 / JNZ -> TEST byte,0x2 / JZ continue): AIR-only weapon vs "
           "GROUND target_class -> mask fails -> dist_out_of_range NOT reached");

        // 3c: target_ref carries 0xa0 AND the referenced unit's elevation!=0 -> target_class=2 (AIR).
        // weapon.target=AIR-only now PASSES.
        fx.cfg_weapons[WID].target = WEAPON_TARGET_AIR;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        // target_ref=0x81 (owner nibble 1, bit 0x80 set -> &0xa0 != 0), same live_target (elevation=999).
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x81, 7, 640, 960);
        ck_eq((uint32_t)g_dist_calls.size(), 1u,
              "fw 3c (0x0048bcf1-0x0048bd21): target_ref&0xa0!=0 AND unit_of(target).elevation!=0 -> "
              "target_class=2 (AIR) -> AIR-only weapon mask now passes");

        // 3d: SAME target_ref (0x81, 0xa0 bit set) but the referenced unit's elevation IS zero ->
        // target_class stays 1 (GROUND) despite the live-unit-target bit being set. AIR-only weapon
        // must now FAIL the mask (proving elevation, not merely "is a unit target", decides target_class).
        live_target.elevation = 0;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x81, 7, 640, 960);
        ck(g_dist_calls.empty(),
           "fw 3d (0x0048bd17 CMP dword,0x0 / JZ 0x0048bd21): target_ref&0xa0!=0 but the referenced "
           "unit's elevation==0 -> target_class stays 1 (GROUND) -> AIR-only weapon mask fails");
        // And the GROUND sense of the same fixture passes, confirming 3d really landed on class=1:
        fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x81, 7, 640, 960);
        ck_eq((uint32_t)g_dist_calls.size(), 1u,
              "fw 3d-ground: the same fixture with a GROUND-only weapon passes -- confirms target_class "
              "really is 1 here, not merely 'mask happened to fail'");
    }

    // ==== 4. THE RANGE-CHECK GATE ITSELF: presence, both senses, and its skip in non-auto mode =======
    {
        // 4a: loop_bound==1 (specific slot) -> the range check is SKIPPED ENTIRELY (0x0048bd57 JLE),
        // proceeding straight from the mask pass into the switch -- landed here on the SAFE default arm
        // (weapon.type=7) so this is observable without crashing.
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type   = 7; // dedicated no-op arm (0x0048c684, caseD_7)
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck(g_dist_calls.empty(),
           "fw 4a (0x0048bd57 CMP loop_bound,1 / JLE 0x0048be02): a specific-slot request "
           "(weapon_slot_select!=4) skips llm_strat_dist_out_of_range ENTIRELY, not just its result");
        ck(trace_eq({}), "fw 4a: no `_calls` member fires -- type=7's own body is a bare JMP, no callee");
        ck_eq((uint32_t)u.selected_weapon, 77u, "fw 4a: matched_count=1 (slot 0) -> selection untouched");

        // 4b: loop_bound==4, dist_out_of_range returns NONZERO (out of range) -> continue, switch never
        // reached (weapon.type left at a WOULD-crash value on purpose, to prove the continue is real:
        // if this gate wrongly passed, type=1 would recompute mount pos and crash).
        unit &u2 = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u2.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type   = 1;
        g_dist_ret                 = 1; // out of range (default posture, restated explicitly here)
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x01, 7, 640, 960);
        ck_eq((uint32_t)g_dist_calls.size(), 1u, "fw 4b: dist_out_of_range reached once (auto mode)");
        ck_eq((uint32_t)u2.selected_weapon, 77u,
              "fw 4b (0x0048bdd6 TEST EAX,EAX / 0x0048bdd8 JNZ continue): a NONZERO return continues "
              "past the slot WITHOUT reaching the switch, even though weapon.type=1 would otherwise fire");

        // 4c: same fixture, dist_out_of_range returns ZERO (in range) -> proceeds into the switch -- but
        // weapon.type is set to 9 (out-of-[1,8] default) so the landing is safe.
        unit &u3 = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u3.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type   = 9;
        g_dist_ret                 = 0; // in range
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x01, 7, 640, 960);
        ck_eq((uint32_t)g_dist_calls.size(), 1u, "fw 4c: dist_out_of_range reached once");
        ck_eq((uint32_t)u3.selected_weapon, 77u,
              "fw 4c (0x0048bdd6/0x0048bdd8, a ZERO return does NOT jump to continue): the slot proceeds "
              "into the switch; type=9 (>8) lands safely in the default no-op arm");
        g_dist_ret = 1; // restore the default posture for later cases
    }

    // ==== 5. THE SWITCH'S DEFAULT ARM: type 0, 7, and 9 all land on the SAME bare no-op ================
    {
        for (uint8_t type_val : {(uint8_t)0, (uint8_t)7, (uint8_t)9}) {
            unit &u = seed_fw_unit(fx);
            for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
            fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;
            fx.cfg_weapons[WID].type   = type_val;
            reset_fw_observations();
            sim_view  v   = fx.view();
            sim_store own = fx.store();
            // weapon_slot_select=1 (loop_bound=1) skips the range check entirely -- the cleanest, most
            // mutation-robust way to reach the switch (see 4a): no second callee to accidentally invoke.
            detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 0, 0);
            char what[128];
            std::snprintf(what, sizeof(what),
                          "fw 5 (0x0048be17 CMP AL,7 / JA caseD_7, or the type==7 dedicated JMP): "
                          "weapon.type=%u lands on the safe no-op default arm (selected_weapon untouched, "
                          "no callee fires)",
                          (unsigned)type_val);
            ck_eq((uint32_t)u.selected_weapon, 77u, what);
            ck(g_trace.empty(), "fw 5: the default arm's body is a bare JMP -- no `_calls` member fires");
        }
    }

    // ==== 6. TILE-CONVERSION TRUNCATION SENSE (toward zero, not floor) ON A NEGATIVE COORDINATE ======
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target               = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].range_min[FW_PLAYER] = 0;
        fx.cfg_weapons[WID].range_max[FW_PLAYER] = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        // target_fine_x=-65: truncate-toward-zero(-65/32) = -2 (C's `/`); FLOOR would give -3. target_ref
        // has no 0x40 bit, so this is the raw-fine (not building-derived) arm.
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x00, 0, -65, -96);
        ck_eq((uint32_t)g_dist_calls.size(), 1u, "fw 6: reached the range check (auto mode, mask passes)");
        if (!g_dist_calls.empty()) {
            ck_eq((uint32_t)g_dist_calls[0].target_tile_x, (uint32_t)(-2),
                  "fw 6 (0x0048bbfd-0x0048bc0b SAR/SHL/SBB/SAR idiom): fine_to_tile(-65) = -2, "
                  "TRUNCATE TOWARD ZERO -- a floor()-style translation would produce -3");
            ck_eq((uint32_t)g_dist_calls[0].target_tile_y, (uint32_t)(-3),
                  "fw 6: fine_to_tile(-96) = -3 exactly (no rounding ambiguity: -96 is a multiple of 32), "
                  "a control confirming -2 above is really the -65 case and not a fixed sentinel");
        }
    }

    // ==== 7. BUILDING-TARGET TILE RESOLUTION USES THE PIXEL-SPACE MASKS (bw_mask/bh_mask), NOT THE
    // TILE-SPACE ONES (width_mask/height_mask) -- the ONE case in this file needing bldg_get_coords ===
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        fx.cfg_weapons[WID].target = WEAPON_TARGET_GROUND;

        constexpr uint32_t BLDG_PLAYER    = 3;
        constexpr int32_t  BLDG_IDX       = 2;
        constexpr uint16_t BLDG_ROW       = 9;
        building          &b              = fx.b((int32_t)BLDG_PLAYER, BLDG_IDX);
        b.building_id                     = BLDG_ROW;
        b.x                               = 250;
        b.y                               = 60;
        fx.cfg_buildings[BLDG_ROW].width  = 4;
        fx.cfg_buildings[BLDG_ROW].height = 2;
        // Distinct from geom.width_mask/height_mask (fixture reset(): 0xff / 0x3f) AND large enough not
        // to truncate the real sums below -- so a translation that reached for the wrong mask pair
        // (width_mask/height_mask instead of bw_mask/bh_mask) produces a VISIBLY different, truncated
        // result rather than merely being unreachable-to-detect.
        fx.geom.bw_mask = 0xffffu;
        fx.geom.bh_mask = 0x7fffu;
        // *out_x = (250*32 + 4*16) & 0xffff = 8064 & 0xffff = 8064 -> tile 8064/32 = 252.
        // *out_y = (60*32  + 2*16) & 0x7fff = 1952 & 0x7fff = 1952 -> tile 1952/32 = 61.
        // (had it used width_mask=0xff/height_mask=0x3f instead: 8064&0xff=128 -> tile 4;
        //  1952&0x3f=32 -> tile 1 -- both clearly different from 252/61.)

        reset_fw_observations();
        live_regions_on_fixture redirect(fx); // bldg_get_coords is state()-based -- see the file banner
        sim_view                v   = fx.view();
        sim_store               own = fx.store();
        // target_ref = 0x40 | BLDG_PLAYER (a building target), target_index = BLDG_IDX.
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4,
                                 0x40u | BLDG_PLAYER, (uint16_t)BLDG_IDX, /*unused for the tile calc*/ 0, 0);
        ck_eq((uint32_t)g_dist_calls.size(), 1u, "fw 7: reached the range check");
        if (!g_dist_calls.empty()) {
            ck_eq((uint32_t)g_dist_calls[0].target_tile_x, 252u,
                  "fw 7 (0x0048bbce llm_strat_bldg_get_coords, then 0x0048bbd6-0x0048bbe4 /32): building "
                  "target -> tile_x = ((250*32+4*16) & geom.bw_mask=0xffff) / 32 = 252, NOT the "
                  "width_mask=0xff result (4)");
            ck_eq((uint32_t)g_dist_calls[0].target_tile_y, 61u,
                  "fw 7: tile_y = ((60*32+2*16) & geom.bh_mask=0x7fff) / 32 = 61, NOT the "
                  "height_mask=0x3f result (1)");
        }
        ck_eq((uint32_t)u.selected_weapon, 77u, "fw 7: matched_count=1, out-of-range default -> untouched");
    }

    // ==== 8. THE FIRE-MODE SWITCH ARMS -- NOW REACHABLE (2026-08-19 seam, see the file banner) ========
    //
    // case 1/5/8 (0x0048c419-0x0048c67d): the full shot. recompute mount/soldier pos, SIM_ACTIVE-gated
    // sound, projectile_spawn, ammo/pocket/reload bookkeeping. Driven with weapon_slot_select=1 (a
    // specific slot: loop_bound=1, range check skipped, slot 0 fired per the preserved bug) so the arm
    // is reached with no second callee to marshal. u.unit_proto_id=0 -> cfg_units[0].soldier_count=0 ->
    // the calc_mount branch of recompute (the soldier branch is section 9).
    {
        // ---- 8a: type 1, sim_active=0 (no sound). Pins the mount recompute + projectile_spawn arg
        // shape + the ammo!=0 bookkeeping branch. ------------------------------------------------------
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        u.elevation                    = 3; // own_elevation -> projectile src_y_vis_offset (own_class unused in case 1)
        u.weapons[0].ammo              = 5;
        u.weapons[0].pocket            = 4;
        fx.cfg_weapons[WID].target     = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type       = 1;
        fx.cfg_weapons[WID].short_time = 2.5;
        fx.cfg_weapons[WID].long_time  = 9.0;
        fx.cfg_weapons[WID].sound_fire = 55;
        fx.sim_active                  = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);

        ck_eq((uint32_t)g_mount_fine_calls.size(), 2u,
              "fw 8a (0x0048bfae-0x0048c00a recompute, soldier_count==0): calc_mount_fine_pos called "
              "twice (X then Y axis)");
        ck_eq((uint32_t)g_mount_render_calls.size(), 1u, "fw 8a: calc_mount_render_pos called once");
        if (g_mount_fine_calls.size() == 2) {
            ck(g_mount_fine_calls[0].player == 2 && g_mount_fine_calls[0].unit_idx == 5 &&
                   g_mount_fine_calls[0].mount_idx == 1 && g_mount_fine_calls[0].axis_is_x != 0,
               "fw 8a: calc_mount_fine_pos(player=2, unit_idx=5, mount_idx=1, axis_is_x=1) first (the X axis)");
            ck(g_mount_fine_calls[1].axis_is_x == 0,
               "fw 8a: calc_mount_fine_pos's SECOND call is axis_is_x=0 (the Y axis) -- not two X reads");
        }
        ck(g_snd_calls.empty(),
           "fw 8a (0x0048c419 TEST SIM_ACTIVE / JZ): sim_active==0 -> NO sound (snd_play_at skipped)");
        ck_eq((uint32_t)g_proj_calls.size(), 1u, "fw 8a (0x0048c536 fire_projectile): projectile_spawn once");
        if (!g_proj_calls.empty()) {
            const auto &p = g_proj_calls[0];
            ck(p.src_x == (int32_t)MOUNT_FINE_X && p.src_y_ground == (int32_t)MOUNT_FINE_Y,
               "fw 8a: projectile_spawn src_x/src_y_ground = the recompute's mount_x/mount_y (700/800)");
            ck_eq((uint32_t)p.src_y_vis_offset, 3u, "fw 8a: src_y_vis_offset = own_elevation (u.elevation=3)");
            ck(p.dst_x == 640u && p.dst_y_ground == 960u,
               "fw 8a: dst_x/dst_y_ground = the RAW incoming target_fine_x/y (640/960), not building-derived");
            ck_eq((uint32_t)p.dst_y_vis_offset, 0u,
                  "fw 8a: dst_y_vis_offset = target_elevation = 0 (target_ref 0x01 has no 0xa0 bit)");
            ck_eq((uint32_t)p.weapon_id, (uint32_t)WID, "fw 8a: weapon_id = the firing slot's weapon_id (WID)");
            ck_eq((uint32_t)p.owner_player, 1u, "fw 8a: owner_player = target_class = 1 (GROUND)");
            ck_eq((uint32_t)p.homing_player_and_flags, 0x0001u,
                  "fw 8a (0x0048c4dc MOVZX word): homing_player_and_flags = (uint16)target_ref = 0x0001");
            ck_eq((uint32_t)p.homing_target_unit, 7u, "fw 8a: homing_target_unit = target_index = 7");
            ck_eq(p.shooter_ref, 0x82u,
                  "fw 8a (0x0048c500 OR 0x80): shooter_ref = (player | 0x80) = 0x82");
            ck_eq((uint32_t)p.shooter_unit_index, (uint32_t)FW_UNIT_IDX, "fw 8a: shooter_unit_index = unit_index = 5");
        }
        ck_eq((uint32_t)u.weapons[0].ammo, 4u, "fw 8a (0x0048c53b DEC ammo): ammo 5 -> 4");
        ck_eq_d(u.weapons[0].reload_timer, 2.5,
                "fw 8a (0x0048c66e, ammo!=0 branch): reload_timer = w.short_time = 2.5");
        ck_eq((uint32_t)u.weapons[0].enabled, 0u, "fw 8a (0x0048c67d): enabled cleared after firing");
        ck_eq((uint32_t)u.weapons[0].pocket, 4u, "fw 8a: pocket untouched (ammo did not reach 0)");
        ck_eq((uint32_t)u.weapons[0].enabled_2, 1u, "fw 8a: enabled_2 untouched (pocket did not reach 0)");
        ck(trace_eq({"calc_mount_fine_pos", "calc_mount_fine_pos", "calc_mount_render_pos", "projectile_spawn"}),
           "fw 8a: call order recompute(x,y,render) -> projectile_spawn (no sound)");

        // ---- 8b: same, sim_active=1 -> the sound pair fires with the mount/render tile args. ---------
        u.weapons[0].enabled   = 1; // re-arm (8a cleared it)
        u.weapons[0].enabled_2 = 1;
        fx.sim_active          = 1;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck_eq((uint32_t)g_snd_calls.size(), 1u, "fw 8b (0x0048c419 sim_active!=0): snd_play_at fires once");
        if (!g_snd_calls.empty())
            ck(g_snd_calls[0].sound_id == 55 && g_snd_calls[0].col == 21 && g_snd_calls[0].row == 28,
               "fw 8b (0x0048c451-0x0048c46a): snd_play_at(w.sound_fire=55, "
               "fine_to_tile(mount_x)=21, fine_to_tile(render_y)=28)");
        ck(trace_eq({"calc_mount_fine_pos", "calc_mount_fine_pos", "calc_mount_render_pos",
                     "snd_play_at", "projectile_spawn"}),
           "fw 8b: sound is BETWEEN the recompute and projectile_spawn");

        // ---- 8c/8d/8e: the three ammo==0 sub-branches of consume_ammo_and_reload. --------------------
        // 8c: pocket>0 -> decrement, reload = long_time.
        u.weapons[0].enabled = 1;
        u.weapons[0].ammo    = 1;
        u.weapons[0].pocket  = 4;
        fx.sim_active        = 0;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck_eq((uint32_t)u.weapons[0].ammo, 0u, "fw 8c: ammo 1 -> 0");
        ck_eq((uint32_t)u.weapons[0].pocket, 3u, "fw 8c (0x0048c556, pocket!=-1): pocket 4 -> 3");
        ck_eq_d(u.weapons[0].reload_timer, 9.0, "fw 8c (0x0048c58e, pocket!=0): reload_timer = w.long_time = 9.0");
        ck_eq((uint32_t)u.weapons[0].enabled_2, 1u, "fw 8c: enabled_2 stays set (pocket did not reach 0)");

        // 8d: pocket reaches 0 -> the slot is disabled (enabled_2=0), reload_timer NOT set.
        u.weapons[0].enabled      = 1;
        u.weapons[0].enabled_2    = 1;
        u.weapons[0].ammo         = 1;
        u.weapons[0].pocket       = 1;
        u.weapons[0].reload_timer = 123.0; // sentinel: the pocket==0 branch must NOT overwrite it
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck_eq((uint32_t)u.weapons[0].pocket, 0u, "fw 8d: pocket 1 -> 0");
        ck_eq((uint32_t)u.weapons[0].enabled_2, 0u,
              "fw 8d (0x0048c565, pocket==0): the slot is DISABLED (enabled_2 cleared)");
        ck_eq_d(u.weapons[0].reload_timer, 123.0, "fw 8d: reload_timer NOT written on the pocket==0 branch");

        // 8e: pocket == -1 (infinite magazine) -> stays -1, reload = long_time.
        u.weapons[0].enabled   = 1;
        u.weapons[0].enabled_2 = 1;
        u.weapons[0].ammo      = 1;
        u.weapons[0].pocket    = -1;
        reset_fw_observations();
        v   = fx.view();
        own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck_eq((uint32_t)u.weapons[0].pocket, 0xffffffffu,
              "fw 8e (0x0048c550, pocket==-1): the `pocket!=-1` guard skips the decrement -- pocket stays -1");
        ck_eq_d(u.weapons[0].reload_timer, 9.0, "fw 8e: pocket!=0 (it is -1) -> reload_timer = w.long_time");
        ck_eq((uint32_t)u.weapons[0].enabled_2, 1u, "fw 8e: enabled_2 stays set (pocket is -1, not 0)");
    }

    // ==== 9. THE SOLDIER-POSITION RECOMPUTE BRANCH (soldier_count != 0) ================================
    // 0x0048bfae-0x0048c00a: when cfg_units[proto].soldier_count!=0, recompute draws rand_below(count)
    // and reads the position from unit_soldier_get_sprite_screen_pos instead of calc_mount_*.
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        u.elevation                    = 0;
        u.weapons[0].ammo              = 5;
        fx.cfg_units[0].soldier_count  = 3; // proto 0 -> the soldier branch (rand_below(3))
        fx.cfg_weapons[WID].target     = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type       = 1;
        fx.cfg_weapons[WID].short_time = 2.5;
        fx.sim_active                  = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck(g_mount_fine_calls.empty() && g_mount_render_calls.empty(),
           "fw 9: soldier_count!=0 -> the calc_mount_* branch is NOT taken");
        ck_eq((uint32_t)g_rand_below_calls, 1u, "fw 9 (0x0048bfc5): rand_below(soldier_count=3) drawn once");
        ck_eq((uint32_t)g_soldier_calls.size(), 1u, "fw 9: unit_soldier_get_sprite_screen_pos called once");
        if (!g_soldier_calls.empty())
            ck(g_soldier_calls[0].player == 2 && g_soldier_calls[0].unit_idx == 5 && g_soldier_calls[0].hop == 0,
               "fw 9: soldier_screen_pos(player=2, unit_idx=5, hop=rand_below result=0)");
        ck_eq((uint32_t)g_proj_calls.size(), 1u, "fw 9: projectile_spawn once");
        if (!g_proj_calls.empty())
            ck(g_proj_calls[0].src_x == (int32_t)SOLDIER_X && g_proj_calls[0].src_y_ground == (int32_t)SOLDIER_Y,
               "fw 9: projectile src_x/src_y_ground = the soldier screen pos (710/810), NOT the mount pos");
        ck(trace_eq({"rand_below", "soldier_screen_pos", "projectile_spawn"}),
           "fw 9: call order rand_below -> soldier_screen_pos -> projectile_spawn");
        fx.cfg_units[0].soldier_count = 0; // restore: later cases assume the calc_mount branch
    }

    // ==== 10. case 2/6 SPAWNS TWO PROJECTILES (the unsounded case-2 body FALLS THROUGH into case 1) ====
    // 0x0048c342-0x0048c414 then fallthrough 0x0048c419-... : two independent recomputes, two
    // projectile_spawns, but only ONE consume_ammo_and_reload (the case-1 tail).
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        u.elevation                    = 0;
        u.weapons[0].ammo              = 5;
        fx.cfg_weapons[WID].target     = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type       = 2;
        fx.cfg_weapons[WID].short_time = 2.5;
        fx.sim_active                  = 0;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x01, 7, 640, 960);
        ck_eq((uint32_t)g_proj_calls.size(), 2u,
              "fw 10 (case 2 body + case 1 fallthrough): TWO projectile_spawns per shot");
        ck_eq((uint32_t)g_mount_fine_calls.size(), 4u, "fw 10: TWO recomputes -> 4 calc_mount_fine_pos calls");
        ck_eq((uint32_t)g_mount_render_calls.size(), 2u, "fw 10: TWO recomputes -> 2 calc_mount_render_pos calls");
        if (g_proj_calls.size() == 2)
            ck(g_proj_calls[0].src_x == g_proj_calls[1].src_x && g_proj_calls[0].dst_x == g_proj_calls[1].dst_x &&
                   g_proj_calls[0].weapon_id == g_proj_calls[1].weapon_id,
               "fw 10: both projectiles carry the same shot parameters (recomputed fresh, identical stub)");
        ck_eq((uint32_t)u.weapons[0].ammo, 4u,
              "fw 10: ammo decremented ONCE (5->4) -- the case-2 body uses no ammo, only the case-1 tail does");
    }

    // ==== 11. case 4 -> case 3 FALLTHROUGH: muzzle flash, scatter, impact fx, dual sound, area damage ==
    // The densest arm. own_class from the FIRING unit's elevation (2 here); target_class from the TARGET
    // (2 here). Pins the fx_anim_spawn arg shapes, the scatter forward, the pixel-mask impact math, and
    // apply_area_damage's arg tuple incl. the trunc'd ring_count.
    {
        unit &u = seed_fw_unit(fx);
        for (int i = 1; i < 4; ++i) u.weapons[i].enabled_2 = 0;
        u.elevation                                  = 5;  // own_class = 2
        u.experience                                 = 17; // scatter arg p1
        u.weapons[0].ammo                            = 5;
        unit &live_target                            = fx.u(1, 7);        // target_ref 0x81 -> owner nibble 1, index 7
        live_target.elevation                        = 4;                 // target_class = 2, target_elevation = 4
        fx.cfg_weapons[WID].target                   = WEAPON_TARGET_AIR; // target_class 2 -> AIR mask
        fx.cfg_weapons[WID].type                     = 4;
        fx.cfg_weapons[WID].short_time               = 2.5;
        fx.cfg_weapons[WID].fite_explo               = 100;
        fx.cfg_weapons[WID].target_explo             = 200;
        fx.cfg_weapons[WID].sound_fire               = 55;
        fx.cfg_weapons[WID].sound_target             = 66;
        fx.cfg_weapons[WID].missing[FW_PLAYER]       = 6; // scatter arg p2
        fx.cfg_weapons[WID].power[FW_PLAYER]         = 45.0;
        fx.cfg_weapons[WID].fire_range[FW_PLAYER]    = 2.9; // ring_count = trunc(2.9) = 2
        fx.cfg_weapons[WID].area_damage_owner_filter = 3;
        fx.geom.bw_mask                              = 0xffffu;
        fx.geom.bh_mask                              = 0x7fffu;
        fx.game_clock                                = 12.5;
        fx.sim_active                                = 1;
        g_dir_from_to_ret                            = 4; // facing
        g_stride_ret                                 = 3;
        reset_fw_observations();
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        // target_ref 0x81, target_index 7, fine (640,960).
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 1, 0x81, 7, 640, 960);

        ck_eq((uint32_t)g_mount_fine_calls.size(), 4u, "fw 11: two recomputes (case4 mount_idx=2, case3 mount_idx=1)");
        if (g_mount_render_calls.size() == 2)
            ck(g_mount_render_calls[0].mount_idx == 2 && g_mount_render_calls[1].mount_idx == 1,
               "fw 11: case4 recomputes mount_idx=2, case3 recomputes mount_idx=1");
        ck_eq((uint32_t)g_dir_calls.size(), 2u, "fw 11: dir_from_to called once per case (case4 + case3)");
        if (!g_dir_calls.empty())
            ck(g_dir_calls[0].x1 == (int32_t)MOUNT_FINE_X && g_dir_calls[0].y1 == (int32_t)MOUNT_FINE_Y &&
                   g_dir_calls[0].x2 == 640 && g_dir_calls[0].y2 == 960,
               "fw 11: dir_from_to(mount_x=700, mount_y=800, target_fine_x=640, target_fine_y=960)");
        ck_eq((uint32_t)g_stride_calls.size(), 2u, "fw 11: fx_anim_dir_frame_stride(w.fite_explo) once per case");
        ck_eq((uint32_t)g_fx_calls.size(), 3u,
              "fw 11: THREE fx_anim_spawn (case4 muzzle, case3 muzzle, case3 impact)");
        if (g_fx_calls.size() == 3) {
            // muzzle frame id = fite_explo + stride*(facing-1) = 100 + 3*(4-1) = 109; layer = own_class = 2.
            ck(g_fx_calls[0].x == MOUNT_FINE_X && g_fx_calls[0].y == (uint32_t)MOUNT_RENDER &&
                   g_fx_calls[0].anim_frame_id == 109u && g_fx_calls[0].elapsed == 12.5 && g_fx_calls[0].layer == 2u,
               "fw 11 (case4 muzzle 0x0048bee0): fx_anim_spawn(mount_x=700, render_y=900, "
               "fite_explo+stride*(facing-1)=109, game_clock=12.5, own_class=2)");
            ck(g_fx_calls[1].anim_frame_id == 109u && g_fx_calls[1].layer == 2u,
               "fw 11 (case3 muzzle 0x0048c028): a SECOND own_class muzzle flash, same frame/layer");
            // impact: imp_x = bw_mask & (target_fine_x + scatter_dx) = 0xffff & (640+5) = 645;
            //         imp_y_vis = bh_mask & (target_fine_y + scatter_dy - target_elevation) = 0x7fff & (960-7-4) = 949.
            ck(g_fx_calls[2].x == 645u && g_fx_calls[2].y == 949u && g_fx_calls[2].anim_frame_id == 200u &&
                   g_fx_calls[2].layer == 2u,
               "fw 11 (case3 impact 0x0048c0e2): fx_anim_spawn(imp_x=645, imp_y_vis=949, target_explo=200, "
               "game_clock=12.5, target_class=2)");
        }
        ck_eq((uint32_t)g_scatter_calls.size(), 1u, "fw 11: weapon_scatter_offset once");
        if (!g_scatter_calls.empty()) {
            const auto &s = g_scatter_calls[0];
            ck(s.p1 == 17 && s.p2 == 6,
               "fw 11 (0x0048bf5a): weapon_scatter_offset(u.experience=17, w.missing[player]=6, ...)");
            ck(s.a2 == MOUNT_FINE_X && s.p4 == MOUNT_FINE_Y && s.p5 == 640u && s.p6 == 960u,
               "fw 11: scatter src=mount(700,800), dst=target_fine(640,960)");
        }
        ck(g_snd_calls.size() == 2,
           "fw 11 (0x0048c1b8-0x0048c1ec): dual SIM_ACTIVE-gated sound -- fire at the mount, target at the impact");
        if (g_snd_calls.size() == 2)
            ck(g_snd_calls[0].sound_id == 55 && g_snd_calls[0].col == 21 && g_snd_calls[0].row == 28 &&
                   g_snd_calls[1].sound_id == 66 && g_snd_calls[1].col == 20 && g_snd_calls[1].row == 29,
               "fw 11: snd_play_at(w.sound_fire=55, mount tile 21,28) then snd_play_at("
               "w.sound_target=66, impact-vis tile fine_to_tile(645)=20, fine_to_tile(949)=29)");
        ck_eq((uint32_t)g_area_calls.size(), 1u, "fw 11: apply_area_damage once");
        if (!g_area_calls.empty()) {
            const auto &a = g_area_calls[0];
            // imp_y_ground = bh_mask & (960-7) = 953; area x = fine_to_tile(645)=20, y = fine_to_tile(953)=29.
            ck(a.x == 20 && a.y == 29,
               "fw 11 (0x0048c1f4): apply_area_damage tile = (fine_to_tile(imp_x=645)=20, "
               "fine_to_tile(imp_y_ground=953)=29) -- the GROUND impact y, not the vis one");
            ck_eq((uint32_t)a.target_kind, 2u, "fw 11: target_kind = target_class = 2");
            ck_eq_d(a.damage, 45.0, "fw 11: damage = w.power[player] = 45.0");
            ck_eq((uint32_t)a.ring_count, 2u, "fw 11: ring_count = trunc(w.fire_range[player]=2.9) = 2");
            ck_eq((uint32_t)a.owner_filter_zeroed, 3u, "fw 11: owner_filter = w.area_damage_owner_filter = 3");
            ck_eq(a.killer_info, 0x82u, "fw 11: killer_info = (player | 0x80) = 0x82");
            ck_eq((uint32_t)a.killer_unit_index, (uint32_t)FW_UNIT_IDX, "fw 11: killer_unit_index = unit_index = 5");
        }
        ck_eq((uint32_t)u.weapons[0].ammo, 4u, "fw 11: ammo decremented once (case 3's own consume tail)");
        g_dir_from_to_ret = 1; // restore knobs for any later case
        g_stride_ret      = 3;
    }

    // ==== 12. THE M24 DISCRIMINATOR: consume_ammo_and_reload writes the FIRED slot, not slot 0 =========
    //
    // Auto mode (weapon_slot_select=4, loop_bound=4) with slot 0 disabled and slot 1 the firing weapon,
    // so consume_ammo_and_reload runs on `ws = u.weapons[1]`. This is the ONLY configuration that
    // distinguishes `ws.pocket -= 1` (correct) from `u.weapons[0].pocket -= 1` (mutation M24 in
    // tools/oneoff/2026-08-16-mutate-sim1e-debt.py) -- the non-auto cases above always fire slot 0, where
    // the two are identical. M24 was "expect MISSED / scope measurement" while the arms were unreachable;
    // this case is why it is now a real CAUGHT check.
    {
        unit &u                                  = seed_fw_unit(fx);
        u.weapons[0].enabled_2                   = 0;    // slot 0: skipped (never fires; its pocket stays the seed sentinel)
        u.weapons[0].pocket                      = 5678; // the sentinel M24 would wrongly decrement
        u.weapons[1]                             = {};
        u.weapons[1].weapon_id                   = WID;
        u.weapons[1].enabled_2                   = 1;
        u.weapons[1].enabled                     = 1;
        u.weapons[1].ammo                        = 1;
        u.weapons[1].pocket                      = 4;
        u.weapons[2].enabled_2                   = 0;
        u.weapons[3].enabled_2                   = 0;
        fx.cfg_weapons[WID].target               = WEAPON_TARGET_GROUND;
        fx.cfg_weapons[WID].type                 = 1;
        fx.cfg_weapons[WID].long_time            = 9.0;
        fx.cfg_weapons[WID].range_min[FW_PLAYER] = 0;
        fx.cfg_weapons[WID].range_max[FW_PLAYER] = 1000;
        fx.sim_active                            = 0;
        reset_fw_observations();
        g_dist_ret    = 0; // in range (auto mode runs the range check; must pass to reach the switch)
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_fw_calls(), FW_PLAYER, (uint32_t)FW_UNIT_IDX, 4, 0x01, 7, 640, 960);
        ck_eq((uint32_t)u.weapons[1].ammo, 0u, "fw 12: the FIRED slot (1) is the one whose ammo decremented");
        ck_eq((uint32_t)u.weapons[1].pocket, 3u,
              "fw 12 (M24 discriminator): consume_ammo_and_reload decremented slot 1's OWN pocket (4->3), "
              "not slot 0's -- M24 (hardcoding weapons[0]) leaves this at 4");
        ck_eq((uint32_t)u.weapons[0].pocket, 5678u,
              "fw 12 (M24 discriminator): the disabled slot 0's pocket is UNTOUCHED (5678) -- M24 would "
              "decrement it to 5677");
        ck_eq_d(u.weapons[1].reload_timer, 9.0, "fw 12: slot 1's reload_timer = long_time (pocket 3, !=0)");
        ck_eq((uint32_t)u.selected_weapon, 77u, "fw 12: matched_count>0 (slot 1) -> selected_weapon untouched");
    }
}

} // namespace

// =====================================================================================================
// ==== llm_strat_unit_fire_at_target2_if_aimed @0x004863e6 ============================================
// =====================================================================================================
namespace {
using namespace mh::sim; // re-stated defensively: this is a separately-reopened anonymous-namespace
                         // block from the fire_weapon one above, which already has this at file scope.

struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_gc_calls;
int32_t                    g_gc_out_x = 0, g_gc_out_y = 0;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    tr("get_coords");
    g_gc_calls.push_back({player, unit_index});
    *out_x = g_gc_out_x;
    *out_y = g_gc_out_y;
}

struct DirCall2 {
    int32_t x1, y1, x2, y2;
};
std::vector<DirCall2> g_dir2_calls;
int32_t               g_dir2_ret = 0;
int32_t               rec_dir_from_to2(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir2_calls.push_back({x1, y1, x2, y2});
    return g_dir2_ret;
}

struct FireCall {
    uint32_t player, unit_index;
    uint8_t  weapon_slot;
    uint32_t target_ref;
    uint16_t target_index;
    int32_t  target_fine_x, target_fine_y;
};
std::vector<FireCall> g_fire_calls;
void                  rec_unit_fire_weapon2(uint32_t player, uint32_t unit_index, uint8_t weapon_slot,
                                            uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                                            int32_t target_fine_y) {
    tr("fire_weapon");
    g_fire_calls.push_back({player, unit_index, weapon_slot, target_ref, target_index, target_fine_x, target_fine_y});
}

const unit_fire_at_target2_if_aimed_calls &rec_f2_calls() {
    static const unit_fire_at_target2_if_aimed_calls c = {
        &rec_unit_get_coords,
        &rec_dir_from_to2,
        &rec_unit_fire_weapon2,
    };
    return c;
}

void reset_f2_observations() {
    g_trace.clear();
    g_gc_calls.clear();
    g_dir2_calls.clear();
    g_fire_calls.clear();
    // NOTE g_dir2_ret is deliberately NOT reset here. It is a per-case KNOB, not an observation, and
    // every caller sets it BEFORE calling this function -- so clearing it here silently zeroed
    // target_dir for every aim case. The whole aim test then ran at target_dir==0 regardless of what
    // the case asked for, which made four cases pass for the wrong reason and failed the rest.
    // Caught on this file's first real run, 2026-08-16. Reset knobs in the SEEDER, observations here.
}

constexpr uint16_t F2_PROTO = 3;

// Seeds cur_unit (== units[0][0], the fixture's default cur_unit_ptr target) and cfg_units[F2_PROTO].
// tol=5 is the shared tolerance for every boundary case below (0..23-domain, matching the header
// banner's own derivation).
unit &seed_f2(sim_fixture &f, int32_t tol) {
    unit &u                                       = f.u(0, 0);
    u.unit_proto_id                               = F2_PROTO;
    u.facing_current                              = 0; // overwritten per case
    u.target2_ref                                 = 0;
    u.target2_index                               = 0;
    u.target2_fine_x                              = 4001; // distinct sentinel, forwarded verbatim on fire
    u.target2_fine_y                              = 4002;
    f.cfg_units[F2_PROTO].weapon_facing_tolerance = (uint32_t)tol;
    f.view_cur_player                             = 8;
    f.view_cur_index                              = 13;
    g_gc_out_x                                    = 501; // cur_x/cur_y -- arbitrary but distinct, only
    g_gc_out_y                                    = 502; // their FORWARDING to dir_from_to is asserted
    return u;
}

// Runs one aim-test case and returns whether fire_weapon was invoked.
bool run_f2_case(sim_fixture &f, int32_t cur_facing, int32_t target_dir) {
    unit &u          = f.u(0, 0);
    u.facing_current = (uint8_t)cur_facing;
    g_dir2_ret       = target_dir;
    reset_f2_observations();
    sim_view v = f.view();
    detail::unit_fire_at_target2_if_aimed(v, rec_f2_calls());
    return !g_fire_calls.empty();
}

void run_f2_tests() {
    sim_fixture fx;

    // ==== A. get_coords / dir_from_to argument forwarding, and tol/facing field reads (always run,
    // regardless of the aim outcome -- 0x004863fe-0x0048645c is an unconditional prefix) ==============
    {
        seed_f2(fx, 5);
        unit &u          = fx.u(0, 0);
        u.facing_current = 10;
        // target_dir must be within tol of cur_facing for this block to reach fire_weapon and pin the
        // FULL three-call order. 13 is 3 away from 10, inside tol=5, so check_a passes trivially
        // (10<=13) and check_b passes via e==3<=tol. NOT 20: the two checks are MIRRORS, so at most
        // one of them is ever trivially true -- with target_dir=20, check_b's own arms are
        // (20<=10)=false, (e=10<=5)=false, (24-5=19<=10)=false, i.e. NO FIRE. "cur_facing<=target_dir
        // so both pass trivially" is the reasoning trap this comment exists to block.
        g_dir2_ret = 13;
        reset_f2_observations();
        sim_view v = fx.view();
        detail::unit_fire_at_target2_if_aimed(v, rec_f2_calls());

        ck_eq((uint32_t)g_gc_calls.size(), 1u, "f2 A (0x00486412): unit_get_coords called exactly once");
        ck(g_gc_calls[0].player == 8 && g_gc_calls[0].unit_index == 13,
           "f2 A: unit_get_coords(*cur_player=8, *cur_index=13)");
        ck_eq((uint32_t)g_dir2_calls.size(), 1u, "f2 A (0x00486433): dir_from_to called exactly once");
        ck(g_dir2_calls[0].x1 == 501 && g_dir2_calls[0].y1 == 502,
           "f2 A (0x0048642d/0x00486430): dir_from_to's x1/y1 = cur_x/cur_y as WRITTEN by get_coords' "
           "own out-params (501,502), not some other pair -- pins the EBX=out_x/[EBP-0x20], "
           "ECX=out_y/[EBP-0x1c] binding the .cpp banner documents");
        ck(g_dir2_calls[0].x2 == 4001 && g_dir2_calls[0].y2 == 4002,
           "f2 A (0x0048641c/0x00486427): dir_from_to's x2/y2 = cur_unit->target2_fine_x/_fine_y (4001,4002)");
        ck(trace_eq({"get_coords", "dir_from_to", "fire_weapon"}),
           "f2 A: call order get_coords -> dir_from_to -> fire_weapon (cur_facing=10, target_dir=13: "
           "check_a trivial via 10<=13, check_b via e=3<=tol=5 -> fires)");
    }

    // ==== B. check_a's own boundary: cur_facing>target_dir, d=cur_facing-target_dir vs tol ==========
    {
        seed_f2(fx, 5); // tol=5
        // d==tol (5) exactly -> PASSES (0x0048646d CMP / 0x00486470 JG: d>tol is the FAIL branch, so
        // d==tol takes the NOT-taken side of JG, i.e. passes via the <= sense).
        ck(run_f2_case(fx, 15, 10), "f2 B1 (0x00486470 JG, d==tol boundary): d=15-10=5==tol -> check_a "
                                    "passes (d<=tol), check_b trivial (target_dir<=cur_facing) -> fires");
        // d==tol+1 (6), and d < 24-tol (19) -> the "wraparound" fallback also fails -> NO FIRE.
        ck(!run_f2_case(fx, 16, 10), "f2 B2 (0x00486484 JL, d=6<24-tol=19): d>tol and d<24-tol -> check_a "
                                     "FAILS -> no fire (the true 'far apart, no wraparound help' case)");
        // d==24-tol-1 (18): still fails (one below the wraparound boundary).
        ck(!run_f2_case(fx, 20, 2), "f2 B3: d=18, one less than 24-tol=19 -> check_a still fails");
        // d==24-tol (19) exactly: PASSES via the wraparound OR-clause -- pins the >= sense, not >.
        ck(run_f2_case(fx, 21, 2), "f2 B4 (0x00486482 CMP / 0x00486484 JL, d==24-tol boundary): d=19==24-"
                                   "tol -> check_a passes via the wraparound sense (d>=24-tol, not d>24-tol)");
    }

    // ==== C. check_b's own boundary (mirrors B with target_dir>cur_facing, e=target_dir-cur_facing) ===
    {
        seed_f2(fx, 5);
        // check_a passes TRIVIALLY here (cur_facing<=target_dir always true when target_dir>cur_facing),
        // so these cases isolate check_b as the ONLY thing that can block the fire.
        // e==tol+1 (6), e<24-tol(19) -> check_b FAILS despite check_a's trivial pass.
        ck(!run_f2_case(fx, 2, 8), "f2 C1 (0x0048649b/0x004864ab): check_a trivially passes (2<=8) but "
                                   "check_b fails (e=8-2=6>tol=5 and e=6<24-tol=19) -> no fire overall");
        // e==tol (5) exactly -> check_b passes via e<=tol.
        ck(run_f2_case(fx, 3, 8), "f2 C2 (0x00486497 JG, e==tol boundary): e=8-3=5==tol -> check_b passes");
        // e==24-tol-1 (18): fails.
        ck(!run_f2_case(fx, 2, 20), "f2 C3: e=18, one less than 24-tol=19 -> check_b still fails");
        // e==24-tol (19) exactly: passes via the wraparound OR-clause.
        ck(run_f2_case(fx, 2, 21), "f2 C4 (0x004864a9 CMP / 0x004864ab JL, e==24-tol boundary): e=19==24-"
                                   "tol -> check_b passes via the wraparound sense");
    }

    // ==== D. the header banner's own documented wraparound example (raw diff 22, circular distance 2) =
    {
        seed_f2(fx, 3); // tol=3
        ck(run_f2_case(fx, 1, 23), "f2 D (documented example): cur_facing=1, target_dir=23 -- raw e=22 "
                                   "wraps to a circular distance of 2 (<=tol=3) via e>=24-tol=21 -> fires, "
                                   "matching sim_unit_fire_at_target2_if_aimed.h's own hand-traced note");
    }

    // ==== E. when the aim test fails, get_coords/dir_from_to STILL ran (unconditional prefix) but
    // fire_weapon did NOT ====================================================================
    {
        seed_f2(fx, 5);
        ck(!run_f2_case(fx, 16, 10), "f2 E: the B2 no-fire fixture again");
        ck(g_fire_calls.empty(), "f2 E: fire_weapon's recorder was never invoked");
        ck(trace_eq({"get_coords", "dir_from_to"}),
           "f2 E: the trace stops after dir_from_to -- no third call at all when the aim test fails");
    }

    // ==== F. the exact 7-argument forward to fire_weapon, incl. target2_ref/target2_index ZERO-
    // extension (0x004864b1-0x004864f7) ==================================================================
    {
        seed_f2(fx, 5);
        unit &u            = fx.u(0, 0);
        u.selected_weapon  = 44;
        u.target2_ref      = (int16_t)-1; // 0xffff as a raw word -- must ZERO-extend, not sign-extend
        u.target2_index    = (int16_t)-2; // 0xfffe as a raw word -- same
        u.target2_fine_x   = -777;
        u.target2_fine_y   = 888;
        fx.view_cur_player = 8;
        fx.view_cur_index  = 13;
        ck(run_f2_case(fx, 10, 10), "f2 F: cur_facing==target_dir -> both checks pass trivially -> fires");
        ck_eq((uint32_t)g_fire_calls.size(), 1u, "f2 F: fire_weapon called exactly once");
        if (!g_fire_calls.empty()) {
            const auto &c = g_fire_calls[0];
            ck(c.player == 8 && c.unit_index == 13,
               "f2 F (0x004864ef/0x004864f0): fire_weapon(player=*cur_player=8, unit_index=*cur_index=13)");
            ck_eq((uint32_t)c.weapon_slot, 44u, "f2 F (0x004864e5): weapon_slot = cur_unit->selected_weapon");
            ck_eq(c.target_ref, 0xffffu,
                  "f2 F (0x004864d9 MOVZX ECX, word): target_ref = (uint16_t)target2_ref ZERO-extended -- "
                  "0xffff, NOT 0xffffffff (a sign-extending translation would produce the latter)");
            ck_eq((uint32_t)c.target_index, 0xfffeu,
                  "f2 F (0x004864cc MOVZX EAX, word): target_index = (uint16_t)target2_index = 0xfffe, "
                  "zero-extended the same way");
            ck_eq((uint32_t)c.target_fine_x, (uint32_t)(-777),
                  "f2 F (0x004864c1): target_fine_x forwarded RAW (signed, not masked) = cur_unit->"
                  "target2_fine_x");
            ck_eq((uint32_t)c.target_fine_y, (uint32_t)888,
                  "f2 F (0x004864b6): target_fine_y forwarded RAW = cur_unit->target2_fine_y");
        }
    }
}

} // namespace

// =====================================================================================================
// ==== llm_strat_unit_fire_at_target_if_aimed @0x004862c6 / llm_strat_unit_fire_at_target @0x00486506 ==
// =====================================================================================================
// Both operate on mh_map_object_unit's PRIMARY target quartet (target_ref@0x8c, target_index@0x8a,
// target_fine_x@0x8e, target_fine_y@0x92) -- NOT the target2_* quartet the sibling above reads --
// and share the exact circular_aim_ok() shape f2 already established (re-verified against THIS pair's
// own bytes, per the .h banner). fire_at_target additionally has TWO GUARDS in front that _if_aimed
// does not have: an unconditional move_op_code==0x12 -> unit_set_state(0x2e) prefix, and a
// selected_weapon==UNIT_SELECT_WEAPON_NOT_FOUND early-out gating the whole aim-check+fire tail.
namespace {
using namespace mh::sim; // re-stated defensively, same reason as the f2 block above.

struct FatGetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<FatGetCoordsCall> g_fat_gc_calls;
int32_t                       g_fat_gc_out_x = 0, g_fat_gc_out_y = 0;
void                          rec_fat_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    tr("get_coords");
    g_fat_gc_calls.push_back({player, unit_index});
    *out_x = g_fat_gc_out_x;
    *out_y = g_fat_gc_out_y;
}

struct FatDirCall {
    int32_t x1, y1, x2, y2;
};
std::vector<FatDirCall> g_fat_dir_calls;
int32_t                 g_fat_dir_ret = 0;
int32_t                 rec_fat_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_fat_dir_calls.push_back({x1, y1, x2, y2});
    return g_fat_dir_ret;
}

// fire_at_target's extra guard-(a) callee -- _if_aimed never calls this (it has no move_op_code guard).
std::vector<uint16_t> g_fat_set_state_calls;
void                  rec_fat_set_state(uint16_t new_state) {
    tr("set_state");
    g_fat_set_state_calls.push_back(new_state);
}

struct FatFireCall {
    uint32_t player, unit_index;
    uint8_t  weapon_slot;
    uint32_t target_ref;
    uint16_t target_index;
    int32_t  target_fine_x, target_fine_y;
};
std::vector<FatFireCall> g_fat_fire_calls;
void                     rec_fat_fire_weapon(uint32_t player, uint32_t unit_index, uint8_t weapon_slot,
                                             uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                                             int32_t target_fine_y) {
    tr("fire_weapon");
    g_fat_fire_calls.push_back(
        {player, unit_index, weapon_slot, target_ref, target_index, target_fine_x, target_fine_y});
}

const unit_fire_at_target_calls &rec_fat_calls() {
    static const unit_fire_at_target_calls c = {
        &rec_fat_get_coords,
        &rec_fat_dir_from_to,
        &rec_fat_set_state,
        &rec_fat_fire_weapon,
    };
    return c;
}

void reset_fat_observations() {
    g_trace.clear();
    g_fat_gc_calls.clear();
    g_fat_dir_calls.clear();
    g_fat_set_state_calls.clear();
    g_fat_fire_calls.clear();
    // g_fat_dir_ret is a per-case KNOB, deliberately NOT reset here -- same trap reset_f2_observations'
    // own comment documents: every caller sets it BEFORE calling this, so clearing it here would
    // silently zero target_dir for every case that follows.
}

constexpr uint16_t FAT_PROTO = 9; // distinct slot from f2's F2_PROTO(3) -- no aliasing risk, but kept
                                  // distinct for clarity when reading a case in isolation.

// Seeds cur_unit (== units[0][0]) and cfg_units[FAT_PROTO] for the PRIMARY-target pair. tol is the
// shared circular tolerance. move_op_code defaults to 0 (does NOT trigger fire_at_target's guard (a),
// which fires only on ==0x12) and selected_weapon defaults to a real slot (0, not
// UNIT_SELECT_WEAPON_NOT_FOUND) so fire_at_target's guard (b) does not gate by default -- both are
// overridden per-case wherever the guard itself is under test.
unit &seed_fat(sim_fixture &f, int32_t tol) {
    unit &u                                        = f.u(0, 0);
    u.unit_proto_id                                = FAT_PROTO;
    u.facing_current                               = 0; // overwritten per case
    u.selected_weapon                              = 0; // overwritten where the guard/weapon_slot matters
    u.target_ref                                   = 0;
    u.target_index                                 = 0;
    u.target_fine_x                                = 5001; // distinct sentinel, forwarded verbatim on fire
    u.target_fine_y                                = 5002;
    f.cfg_units[FAT_PROTO].weapon_facing_tolerance = (uint32_t)tol;
    f.cfg_units[FAT_PROTO].move_op_code            = 0; // overwritten where guard (a) is under test
    f.view_cur_player                              = 21;
    f.view_cur_index                               = 34;
    g_fat_gc_out_x                                 = 701; // own_x/own_y -- only their FORWARDING is
    g_fat_gc_out_y                                 = 702; // asserted, values themselves are arbitrary
    return u;
}

// Runs one _if_aimed aim-test case and returns whether fire_weapon was invoked.
bool run_fat_if_case(sim_fixture &f, int32_t cur_facing, int32_t target_dir) {
    unit &u          = f.u(0, 0);
    u.facing_current = (uint8_t)cur_facing;
    g_fat_dir_ret    = target_dir;
    reset_fat_observations();
    sim_view v = f.view();
    detail::unit_fire_at_target_if_aimed(v, rec_fat_calls());
    return !g_fat_fire_calls.empty();
}

// Runs one fire_at_target case (guard fixture state already applied by the caller) and returns whether
// fire_weapon was invoked.
bool run_fat_case(sim_fixture &f, int32_t cur_facing, int32_t target_dir) {
    unit &u          = f.u(0, 0);
    u.facing_current = (uint8_t)cur_facing;
    g_fat_dir_ret    = target_dir;
    reset_fat_observations();
    sim_view v = f.view();
    detail::unit_fire_at_target(v, rec_fat_calls());
    return !g_fat_fire_calls.empty();
}

void run_fat_if_aimed_tests() {
    sim_fixture fx;

    // ==== A. get_coords / dir_from_to argument forwarding on the PRIMARY-target fields, and tol/facing
    // field reads (always run, unconditional prefix at 0x004862de-0x0048633c) =========================
    {
        seed_fat(fx, 5);
        unit &u          = fx.u(0, 0);
        u.facing_current = 10;
        g_fat_dir_ret    = 13; // 3 away from 10, inside tol=5 -- same reasoning as f2's own A case: this
                               // isolates the shape rather than passing both checks trivially (20 would)
        reset_fat_observations();
        sim_view v = fx.view();
        detail::unit_fire_at_target_if_aimed(v, rec_fat_calls());

        ck_eq((uint32_t)g_fat_gc_calls.size(), 1u, "fat_if A (0x004862f2): unit_get_coords called exactly once");
        ck(g_fat_gc_calls[0].player == 21 && g_fat_gc_calls[0].unit_index == 34,
           "fat_if A: unit_get_coords(*cur_player=21, *cur_index=34)");
        ck_eq((uint32_t)g_fat_dir_calls.size(), 1u, "fat_if A (0x0048631f): dir_from_to called exactly once");
        ck(g_fat_dir_calls[0].x1 == 701 && g_fat_dir_calls[0].y1 == 702,
           "fat_if A (0x0048631c/0x00486319): dir_from_to's x1/y1 = own_x/own_y as WRITTEN by get_coords' "
           "own out-params (701,702) -- pins the EBX=out_x/[EBP-0x20], ECX=out_y/[EBP-0x1c] binding this "
           "function's own frame (different offsets from fire_at_target's own copy, per the .h banner)");
        ck(g_fat_dir_calls[0].x2 == 5001 && g_fat_dir_calls[0].y2 == 5002,
           "fat_if A (0x00486313/0x00486308): dir_from_to's x2/y2 = cur_unit->target_fine_x/_fine_y "
           "(5001,5002) -- the PRIMARY quartet, not target2_*");
        ck(trace_eq({"get_coords", "dir_from_to", "fire_weapon"}),
           "fat_if A: call order get_coords -> dir_from_to -> fire_weapon (cur_facing=10, target_dir=13: "
           "check_a trivial via 10<=13, check_b via e=3<=tol=5 -> fires)");
    }

    // ==== B. check_a's own boundary: cur_facing>target_dir, d=cur_facing-target_dir vs tol ==========
    {
        seed_fat(fx, 5);
        ck(run_fat_if_case(fx, 15, 10), "fat_if B1 (0x00486350 JG, d==tol boundary): d=15-10=5==tol -> "
                                        "check_a passes (d<=tol), check_b trivial -> fires");
        ck(!run_fat_if_case(fx, 16, 10), "fat_if B2 (0x00486364 JL, d=6<24-tol=19): d>tol and d<24-tol -> "
                                         "check_a FAILS -> no fire");
        ck(!run_fat_if_case(fx, 20, 2), "fat_if B3: d=18, one less than 24-tol=19 -> check_a still fails");
        ck(run_fat_if_case(fx, 21, 2), "fat_if B4 (0x00486364 JL, d==24-tol boundary): d=19==24-tol -> "
                                       "check_a passes via the wraparound sense (d>=24-tol, not d>24-tol)");
    }

    // ==== C. check_b's own boundary (mirrors B with target_dir>cur_facing, e=target_dir-cur_facing) ===
    {
        seed_fat(fx, 5);
        ck(!run_fat_if_case(fx, 2, 8), "fat_if C1 (0x00486377/0x0048638b): check_a trivially passes (2<=8) "
                                       "but check_b fails (e=6>tol=5 and e=6<24-tol=19) -> no fire overall");
        ck(run_fat_if_case(fx, 3, 8), "fat_if C2 (0x00486377 JG, e==tol boundary): e=8-3=5==tol -> check_b "
                                      "passes");
        ck(!run_fat_if_case(fx, 2, 20), "fat_if C3: e=18, one less than 24-tol=19 -> check_b still fails");
        ck(run_fat_if_case(fx, 2, 21), "fat_if C4 (0x0048638b JL, e==24-tol boundary): e=19==24-tol -> "
                                       "check_b passes via the wraparound sense");
    }

    // ==== D. the header banner's own documented wraparound example, re-verified against THIS function's
    // own bytes (raw diff 22, circular distance 2) ======================================================
    {
        seed_fat(fx, 3); // tol=3
        ck(run_fat_if_case(fx, 1, 23), "fat_if D: cur_facing=1, target_dir=23 -- raw e=22 wraps to a "
                                       "circular distance of 2 (<=tol=3) via e>=24-tol=21 -> fires");
    }

    // ==== E. when the aim test fails, get_coords/dir_from_to STILL ran (unconditional prefix) but
    // fire_weapon did NOT ================================================================================
    {
        seed_fat(fx, 5);
        ck(!run_fat_if_case(fx, 16, 10), "fat_if E: the B2 no-fire fixture again");
        ck(g_fat_fire_calls.empty(), "fat_if E: fire_weapon's recorder was never invoked");
        ck(trace_eq({"get_coords", "dir_from_to"}),
           "fat_if E: the trace stops after dir_from_to -- no fire_weapon call when the aim test fails");
    }

    // ==== F. the exact 7-argument forward to fire_weapon on the PRIMARY quartet, incl. target_ref/
    // target_index ZERO-extension (0x00486391-0x004863d7) ================================================
    {
        seed_fat(fx, 5);
        unit &u            = fx.u(0, 0);
        u.selected_weapon  = 44;
        u.target_ref       = (int16_t)-1; // 0xffff as a raw word -- must ZERO-extend, not sign-extend
        u.target_index     = (int16_t)-2; // 0xfffe as a raw word -- same
        u.target_fine_x    = -777;
        u.target_fine_y    = 888;
        fx.view_cur_player = 21;
        fx.view_cur_index  = 34;
        ck(run_fat_if_case(fx, 10, 10), "fat_if F: cur_facing==target_dir -> both checks pass trivially "
                                        "-> fires");
        ck_eq((uint32_t)g_fat_fire_calls.size(), 1u, "fat_if F: fire_weapon called exactly once");
        if (!g_fat_fire_calls.empty()) {
            const auto &c = g_fat_fire_calls[0];
            ck(c.player == 21 && c.unit_index == 34,
               "fat_if F (0x004863d0/0x004863c9): fire_weapon(player=*cur_player=21, unit_index=*cur_index=34)");
            ck_eq((uint32_t)c.weapon_slot, 44u, "fat_if F (0x004863c5): weapon_slot = cur_unit->selected_weapon");
            ck_eq(c.target_ref, 0xffffu,
                  "fat_if F (0x004863b9 MOVZX ECX, word): target_ref = (uint16_t)target_ref ZERO-extended "
                  "-- 0xffff, NOT 0xffffffff (a sign-extending translation would produce the latter)");
            ck_eq((uint32_t)c.target_index, 0xfffeu,
                  "fat_if F (0x004863ac MOVZX EAX, word): target_index = (uint16_t)target_index = 0xfffe, "
                  "zero-extended the same way");
            ck_eq((uint32_t)c.target_fine_x, (uint32_t)(-777),
                  "fat_if F (0x004863a1): target_fine_x forwarded RAW (signed, not masked) = cur_unit->"
                  "target_fine_x");
            ck_eq((uint32_t)c.target_fine_y, (uint32_t)888,
                  "fat_if F (0x00486396): target_fine_y forwarded RAW = cur_unit->target_fine_y");
        }
    }
}

void run_fat_tests() {
    sim_fixture fx;

    // ==== G. guard (a) FIRES: move_op_code==0x12 -> unit_set_state(0x2e) unconditionally, BEFORE
    // anything else -- gated behind guard (b) here (selected_weapon==NOT_FOUND) so ONLY the guard-(a)
    // call is observed, proving it does not depend on the rest of the function reaching anything
    // (0x0048651e-0x0048653f, then the 0x00486545/0x00486549 early-out gates everything downstream) =====
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = UNIT_SELECT_WEAPON_NOT_FOUND;
        fx.cfg_units[FAT_PROTO].move_op_code = 0x12;
        ck(!run_fat_case(fx, 0, 0), "fat G (0x00486549 JZ): gated by selected_weapon==100 -> no fire "
                                    "regardless of the (irrelevant) aim inputs");
        ck_eq((uint32_t)g_fat_set_state_calls.size(), 1u,
              "fat G (0x0048653b): unit_set_state called exactly once when move_op_code==0x12");
        if (!g_fat_set_state_calls.empty())
            ck_eq((uint32_t)g_fat_set_state_calls[0], 0x2eu,
                  "fat G (0x00486536 MOV EAX,0x2e): unit_set_state's argument is the literal 0x2e");
        ck(g_fat_gc_calls.empty(), "fat G: selected_weapon==100 gates get_coords -- never reached even "
                                   "though guard (a) fired");
        ck(g_fat_fire_calls.empty(), "fat G: fire_weapon never reached either");
        ck(trace_eq({"set_state"}),
           "fat G: the ENTIRE trace is just set_state -- guard (a)'s call is the only thing that happens");
    }

    // ==== H. guard (a) does NOT fire when move_op_code != 0x12 (0x0048652d CMP / 0x00486534 JNZ), and
    // with guard (b) also gating (selected_weapon==100) NOTHING AT ALL happens =========================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = UNIT_SELECT_WEAPON_NOT_FOUND;
        fx.cfg_units[FAT_PROTO].move_op_code = 0; // NOT 0x12
        ck(!run_fat_case(fx, 0, 0), "fat H: gated by selected_weapon==100 -> no fire");
        ck(g_fat_set_state_calls.empty(),
           "fat H (0x00486534 JNZ taken): move_op_code=0 != 0x12 -> unit_set_state never called");
        ck(trace_eq({}), "fat H: NOTHING at all happens -- guard (a) false and guard (b) both gate the "
                         "whole body");
    }

    // ==== I. normal path: selected_weapon != NOT_FOUND proceeds past guard (b) into the aim check
    // (move_op_code left non-triggering so guard (a) stays silent) ======================================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 2;
        fx.cfg_units[FAT_PROTO].move_op_code = 0;
        ck(run_fat_case(fx, 10, 13), "fat I: cur_facing=10, target_dir=13 (same shape as fat_if A) -> "
                                     "fires via the normal (non-guarded) path");
        ck(trace_eq({"get_coords", "dir_from_to", "fire_weapon"}),
           "fat I: call order get_coords -> dir_from_to -> fire_weapon, no set_state -- guard (a) stayed "
           "silent (move_op_code=0)");
    }

    // ==== J. both guards combined: move_op_code==0x12 (guard (a) fires) AND selected_weapon valid
    // (guard (b) does not gate) AND the aim test PASSES -- proves guard (a)'s call precedes the aim-test
    // trio unconditionally, as a PREFIX, not an alternative to it =========================================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 2;
        fx.cfg_units[FAT_PROTO].move_op_code = 0x12;
        ck(run_fat_case(fx, 10, 13), "fat J: aim passes with both guards live -> fires");
        ck_eq((uint32_t)g_fat_set_state_calls.size(), 1u, "fat J (0x0048653b): unit_set_state still called once");
        ck(trace_eq({"set_state", "get_coords", "dir_from_to", "fire_weapon"}),
           "fat J: FULL order set_state -> get_coords -> dir_from_to -> fire_weapon -- guard (a) is an "
           "unconditional PREFIX, not a substitute for the aim-check-and-fire tail");
    }

    // ==== K. both guards combined but the aim test FAILS -- set_state still ran (unconditional),
    // get_coords/dir_from_to still ran (unconditional prefix through the aim check), fire_weapon did not
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 2;
        fx.cfg_units[FAT_PROTO].move_op_code = 0x12;
        ck(!run_fat_case(fx, 16, 10), "fat K: the fat_if B2 no-fire fixture again, both guards live");
        ck_eq((uint32_t)g_fat_set_state_calls.size(), 1u, "fat K: unit_set_state still called once");
        ck(trace_eq({"set_state", "get_coords", "dir_from_to"}),
           "fat K: trace stops after dir_from_to -- fire_weapon not reached, set_state unaffected");
    }

    // ==== L. check_a's own boundary on fire_at_target's OWN bytes (0x004865b6 JLE trivial / 0x004865c1
    // JG tol boundary / 0x004865d5 JL wraparound boundary) -- re-verified independently against THIS
    // function's own bytes, not assumed identical to fat_if's addresses =================================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 2;
        fx.cfg_units[FAT_PROTO].move_op_code = 0;
        ck(run_fat_case(fx, 15, 10), "fat L1 (0x004865c1 JG, d==tol boundary): d=5==tol -> check_a passes");
        ck(!run_fat_case(fx, 16, 10), "fat L2 (0x004865d5 JL, d=6<24-tol=19): check_a FAILS -> no fire");
        ck(!run_fat_case(fx, 20, 2), "fat L3: d=18, one less than 24-tol=19 -> check_a still fails");
        ck(run_fat_case(fx, 21, 2), "fat L4 (0x004865d5 JL, d==24-tol boundary): check_a passes via the "
                                    "wraparound sense");
    }

    // ==== M. check_b's own boundary on fire_at_target's OWN bytes (0x004865dd JGE trivial / 0x004865e8
    // JG tol boundary / 0x004865fc JL wraparound boundary) ==============================================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 2;
        fx.cfg_units[FAT_PROTO].move_op_code = 0;
        ck(!run_fat_case(fx, 2, 8), "fat M1 (0x004865e8/0x004865fc): check_a trivially passes but check_b "
                                    "fails (e=6>tol=5 and e=6<24-tol=19) -> no fire overall");
        ck(run_fat_case(fx, 3, 8), "fat M2 (0x004865e8 JG, e==tol boundary): e=5==tol -> check_b passes");
        ck(!run_fat_case(fx, 2, 20), "fat M3: e=18, one less than 24-tol=19 -> check_b still fails");
        ck(run_fat_case(fx, 2, 21), "fat M4 (0x004865fc JL, e==24-tol boundary): check_b passes via the "
                                    "wraparound sense");
    }

    // ==== N. the exact 7-argument forward to fire_weapon on fire_at_target's OWN call site
    // (0x00486602-0x00486648), incl. target_ref/target_index ZERO-extension =============================
    {
        seed_fat(fx, 5);
        unit &u                              = fx.u(0, 0);
        u.selected_weapon                    = 77;
        u.target_ref                         = (int16_t)-1;
        u.target_index                       = (int16_t)-2;
        u.target_fine_x                      = -321;
        u.target_fine_y                      = 654;
        fx.cfg_units[FAT_PROTO].move_op_code = 0; // silent -- isolates the fire_weapon-forward pin
        fx.view_cur_player                   = 21;
        fx.view_cur_index                    = 34;
        ck(run_fat_case(fx, 10, 10), "fat N: cur_facing==target_dir -> both checks pass trivially -> fires");
        ck_eq((uint32_t)g_fat_fire_calls.size(), 1u, "fat N: fire_weapon called exactly once");
        ck(g_fat_set_state_calls.empty(), "fat N: unit_set_state not called (move_op_code silent)");
        if (!g_fat_fire_calls.empty()) {
            const auto &c = g_fat_fire_calls[0];
            ck(c.player == 21 && c.unit_index == 34,
               "fat N (0x00486641/0x0048663a): fire_weapon(player=*cur_player=21, unit_index=*cur_index=34)");
            ck_eq((uint32_t)c.weapon_slot, 77u, "fat N (0x00486636): weapon_slot = cur_unit->selected_weapon");
            ck_eq(c.target_ref, 0xffffu,
                  "fat N (0x0048662a MOVZX ECX, word): target_ref = (uint16_t)target_ref ZERO-extended");
            ck_eq((uint32_t)c.target_index, 0xfffeu,
                  "fat N (0x0048661d MOVZX EAX, word): target_index = (uint16_t)target_index ZERO-extended");
            ck_eq((uint32_t)c.target_fine_x, (uint32_t)(-321),
                  "fat N (0x00486612): target_fine_x forwarded RAW = cur_unit->target_fine_x");
            ck_eq((uint32_t)c.target_fine_y, (uint32_t)654,
                  "fat N (0x00486607): target_fine_y forwarded RAW = cur_unit->target_fine_y");
        }
    }
}

} // namespace

// ---- the one entry point (required exact signature) --------------------------------------------
void run_unit_fire_weapon_tests() {
    printf("-- llm_strat_unit_fire_weapon --\n");
    run_fw_tests();
    printf("-- llm_strat_unit_fire_at_target2_if_aimed --\n");
    run_f2_tests();
    printf("-- llm_strat_unit_fire_at_target_if_aimed / llm_strat_unit_fire_at_target --\n");
    run_fat_if_aimed_tests();
    run_fat_tests();
}

} // namespace mh::sim::test
