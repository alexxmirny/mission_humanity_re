//
// sim_weapon_damage_selftest.cpp -- `simtest` cases for four of the SIM1E "combat damage" batch:
//   llm_strat_weapon_pixel_distance_ratio @0x00448cc3 (sim/sim_weapon_damage_calc.cpp)
//   llm_strat_bldg_kill_credit            @0x0044c67f (sim/sim_combat_kill_credit.cpp)
//   llm_strat_unit_kill_credit            @0x0044cac9 (sim/sim_combat_kill_credit.cpp)
//   llm_strat_apply_area_damage           @0x0044c3fb (sim/sim_combat_kill_credit.cpp)
//
// DELIBERATELY NOT COVERED: llm_strat_unit_estimate_weapon_damage @0x004d32aa, the fifth function of
// the same slice. It carries a DECLARED, UNRESOLVABLE divergence (the armor_prob lookup re-reads an
// UNINITIALISED stack slot on the live `target_ref & 0x0f` path -- the stack-probe imprint,
// tools/data/sim_migration.json's preserve_bug entry, and sim_weapon_damage_calc.h's own corrected
// banner). Giving it a passing offline oracle would launder that divergence into an evidence tier, so
// it gets none here on purpose. Its sibling in the same TU (weapon_pixel_distance_ratio) is clean and
// IS covered.
//
// EVERY EXPECTED VALUE BELOW WAS DERIVED FROM THE RAW DISASSEMBLY in tmp/decomp_sim1e2/
// (llm_strat_apply_area_damage_0044c3fb.asm, llm_strat_bldg_kill_credit_0044c67f.asm,
// llm_strat_unit_kill_credit_0044cac9.asm, llm_strat_weapon_pixel_distance_ratio_00448cc3.asm), NOT
// from the .cpp under test or from its header banner. Instruction addresses are cited inline wherever
// a specific branch/bound/field is what the assertion pins. Two shape facts were re-verified here
// before anything was encoded, because getting either wrong would have baked a WRONG implementation
// into a green test:
//
//   (1) THE RING BOUND IS EXCLUSIVE. 0x0044c428-0x0044c430: `MOV EAX,[ring] / CMP EAX,[ring_count] /
//       JL body / JMP epilogue`. ring == ring_count is never visited. CONFIRMED.
//
//   (2) EACH RING VISITS A FILLED SQUARE, NOT A PERIMETER. This is the one the brief for this file
//       predicted the other way, so it is spelled out. The body is a plain nest with NO
//       edge-skipping test anywhere in the 0x284 bytes:
//         0x0044c43d  col   = width_mask  & (x - ring)          ; once per ring
//         0x0044c44e  xi    = ring*2 + 1
//         0x0044c457  while (xi != 0) {                          ; JNZ into the row set-up
//         0x0044c479    row = height_mask & (y - ring)           ; RE-INIT for every xi
//         0x0044c48a    yi  = ring*2 + 1
//         0x0044c493    while (yi != 0) { <tile body>            ; JNZ into the per-tile body
//         0x0044c49e      row = height_mask & (row+1); --yi }    ; reached from 0x0044c666
//         0x0044c462    col = width_mask  & (col+1);  --xi }     ; reached from 0x0044c66b
//       So ring r covers the whole (2r+1)x(2r+1) block, and a tile at Chebyshev distance d from the
//       centre is hit once per ring r in [d, ring_count), each time for `damage / (r + 1)`
//       (0x0044c4b5-0x0044c4bf: FILD ring / FLD1 / FADDP / FDIVR damage). test_aad_ring_boundary_*
//       below encodes exactly that, and it is what makes the case fail for BOTH of the wrong shapes
//       (a perimeter-only walk under-credits the centre; an inclusive bound credits the ring the
//       original never reaches).
//
// ---- WHY THERE IS A REGION-REDIRECT HELPER IN A `simtest` FILE -----------------------------------
// sim/sim_combat_kill_credit.cpp is the one sim TU so far that calls SIBLING REIMPLEMENTATIONS through
// their PUBLIC wrappers from inside a `detail::` body -- `mh::sim::notify_ui` (twice),
// `mh::sim::bldg_get_coords`, `mh::sim::get_coords`. Every one of those wrappers calls `mh::sim::
// state()`, and `state()` is NOT inert offline: it DEREFERENCES four pointer-valued globals
// (sim_state.cpp lines 72/111/162/168, `*ptr<unit*>(RID_STRAT_CUR_UNIT)` and its three siblings) at
// their stock .bss addresses, which are unmapped in net_selftest.exe. Reaching any of those four call
// sites from an offline case is an ACCESS VIOLATION, not a failed check -- and it would take the whole
// simtest binary with it.
//
// The fix used here is the mechanism the state layer already provides for exactly this: `mh::state::
// rebase` points a region's LIVE base somewhere else, and `unrebase` puts it back (the same facility
// shadow_selftest.cpp's fixture uses). `live_regions_on_fixture` below is an RAII object that redirects
// the fourteen regions those three siblings can touch onto THIS FIXTURE'S OWN BUFFERS, so a sibling
// called through `state()` reads and writes the same state the case set up, and unredirects them in its
// destructor so no later test (statetest asserts no region is rebased) inherits a moved region.
//
// That turns the hazard into an ASSERTION: with RID_GENERAL pointed at `f.geom`, `notify_ui`'s
// local-player arm (0x00488a4b, one dword store setting change_flag=1 AND change_flag2=0) becomes
// observable as `f.geom.change_flag`, which is how the dead-killer case below proves notify_ui ran and
// ran on the KILLER's side rather than the victim's.
//
// IT IS INSTALLED IN EVERY KILL-CREDIT / AREA-DAMAGE CASE, not only the ones that need it, and that is
// the point rather than laziness. Under the CORRECT implementation most cases below never reach a
// sibling call at all (their gates are deliberately shut). But this file exists to be MUTATION-tested:
// a deliberately broken comparison -- an inverted self-kill gate, a flipped cooldown test -- can send
// any of them into notify_ui or get_coords, and without the redirect that is an access violation that
// takes down the whole simtest binary instead of a red line naming the mutation. Installed everywhere,
// a mutation that opens a gate it should not produces a FAILED CHECK.
//
// A note for the conductor, not a request: moving those three sibling calls into `kill_credit_calls`
// (which is what every other sim/ TU does with an outward call, and what makes a body testable at all
// -- see sim_combat_kill_credit.h's own reasoning for indirecting llm_snd_play et al.) would delete the
// need for this helper entirely.
//
#include <array>
#include <vector>

#include "sim/sim_combat_kill_credit.h"
#include "sim/sim_weapon_damage_calc.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the cast of players/indices -----------------------------------------------------------------
// All distinct, none zero, none equal to the fixture's own defaults where that would make a gate pass
// by accident (sim_test_support.h seeds player_side = 7, planet_index = 0).
constexpr uint32_t VICTIM_P     = 3;
constexpr int32_t  VICTIM_I     = 4; // != 0, so the roster slot-0 feedback record is a DIFFERENT one
constexpr uint32_t KILLER_P     = 5; // != VICTIM_P, so the cross-player gate can open
constexpr int32_t  KILLER_I     = 9;
constexpr uint32_t TILE_OWNER   = 2;  // the owner nibble the area-damage tiles carry
constexpr int32_t  PLANET       = 3;  // != the fixture's planet_index default (0)
constexpr uint16_t VICTIM_PROTO = 9;  // cfg_units / cfg_buildings row id
constexpr uint16_t BLDG_ID      = 12; // cfg_buildings row id for the victim building

// cfg_building::type codes read directly off the raw CMP immediates in
// llm_strat_bldg_kill_credit_0044c67f.asm -- not copied from the .cpp's constants.
constexpr uint8_t BLDG_TYPE_H_MAIN_BASE = 0x22; // 0x0044c6bc  CMP byte ptr [...],0x22
constexpr uint8_t BLDG_TYPE_A_MAIN_BASE = 0x0e; // 0x0044c6e5  CMP byte ptr [...],0x0e
constexpr uint8_t BLDG_TYPE_H_MOTHER    = 0x1a; // 0x0044c896  CMP byte ptr [...],0x1a
constexpr uint8_t BLDG_TYPE_PLAIN       = 0x07; // none of the four above

// ---- recorders ------------------------------------------------------------------------------------
// Captureless lambdas convert to plain function pointers, same shape as sim_prod_completion_selftest
// .cpp's g_pc/g_da/g_sa -- one file-scope recorder per `_calls` table.

struct ev_scale {
    double  value;
    int32_t pct;
};
struct ev2 {
    int32_t a, b;
};
struct ev_reg { // llm_strat_ai_bldg_register_visible_building's five arguments, in order
    int32_t  victim_index;
    uint32_t victim_ref;
    uint32_t aggressor_unit_index;
    uint32_t aggressor_ref;
    int32_t  victim_destroyed;
};

struct kc_recorder {                 // bldg_kill_credit / unit_kill_credit / apply_area_damage share ONE calls table
    std::vector<ev_scale> scale_pct; // llm_math_scale_pct (value, pct)
    std::vector<ev2>      snd_play;  // llm_snd_play (sound_id, volume)
    std::vector<int32_t>  text_ids;  // llm_ui_print_queue_text_id (text_id)
    std::vector<ev_reg>   reg;       // llm_strat_ai_bldg_register_visible_building
    void                  reset() { *this = kc_recorder{}; }
};
kc_recorder g_kc;

// Bounds-safe element access. The conductor's mutation pass will deliberately make a body call LESS
// than a case expects; when that happens the size assertion must go red and the field assertions must
// ALSO go red -- not index an empty vector, which is a crash (or an ASan report) instead of a line
// naming the failure. Every out-of-range read returns a sentinel no real argument can equal.
ev_reg reg_at(size_t i) {
    static const ev_reg none = {-1, 0xffffffffu, 0xffffffffu, 0xffffffffu, -1};
    return i < g_kc.reg.size() ? g_kc.reg[i] : none;
}
ev_scale scale_at(size_t i) {
    static const ev_scale none = {-1.0, -1};
    return i < g_kc.scale_pct.size() ? g_kc.scale_pct[i] : none;
}
ev2 snd_at(size_t i) {
    static const ev2 none = {-1, -1};
    return i < g_kc.snd_play.size() ? g_kc.snd_play[i] : none;
}
int32_t text_at(size_t i) { return i < g_kc.text_ids.size() ? g_kc.text_ids[i] : -1; }

const kill_credit_calls &rec_kc_calls() {
    static const kill_credit_calls c = {
        // IDENTITY, deliberately: the real llm_math_scale_pct is value*pct/100, but a stub that
        // returns its input keeps every damage expectation below exact binary arithmetic, and the
        // `pct` argument is still captured so the cases can pin WHICH armor row was indexed.
        [](double value, int32_t pct) -> double {
            g_kc.scale_pct.push_back({value, pct});
            return value;
        },
        [](int32_t sound_id, int32_t volume) { g_kc.snd_play.push_back({sound_id, volume}); },
        [](int32_t text_id) { g_kc.text_ids.push_back(text_id); },
        [](int32_t victim_index, uint32_t victim_ref, uint32_t aggressor_unit_index,
           uint32_t aggressor_ref, int32_t victim_destroyed) {
            g_kc.reg.push_back(
                {victim_index, victim_ref, aggressor_unit_index, aggressor_ref, victim_destroyed});
        },
    };
    return c;
}

struct wp_recorder { // weapon_pixel_distance_ratio's two outward calls
    int32_t n_delta = 0;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0; // what pixel_delta_wrapped was handed
    int32_t dx_out = 0, dy_out = 0;         // what it writes back through the two out-pointers
    int32_t n_sqrt   = 0;
    double  sqrt_arg = 0.0; // what llm_sqrt was handed -- the int32 sum, already widened
    double  sqrt_ret = 0.0; // what it hands back
    void    reset() { *this = wp_recorder{}; }
};
wp_recorder g_wp;

const weapon_pixel_distance_ratio_calls &rec_wp_calls() {
    static const weapon_pixel_distance_ratio_calls c = {
        [](int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx, int32_t *out_dy) {
            g_wp.n_delta++;
            g_wp.x1 = x1;
            g_wp.y1 = y1;
            g_wp.x2 = x2;
            g_wp.y2 = y2;
            *out_dx = g_wp.dx_out;
            *out_dy = g_wp.dy_out;
        },
        [](double x) -> double {
            g_wp.n_sqrt++;
            g_wp.sqrt_arg = x;
            return g_wp.sqrt_ret;
        },
    };
    return c;
}

// ---- live_regions_on_fixture ----------------------------------------------------------------------
// See the file banner for WHY. The list is exactly the regions reachable from the three sibling
// PUBLIC wrappers sim_combat_kill_credit.cpp calls directly:
//   state() itself      -- the four pointer-valued globals it DEREFERENCES to bind cur_*
//   notify_ui           -- PLAYERSIDE, GENERAL (change_flag/change_flag2), CLICK_SELECT_TARGET_{ID,FLAGS}
//   bldg_get_coords     -- BUILDINGS, BUILDING (cfg), GENERAL (bw_mask/bh_mask)
//   get_coords          -- UNITS, UNIT (cfg), GENERAL (big_width/big_height), STRAT_FACING_STEP_SIGN
//                          (facing_step_offset -- its OWN region since the LT1E rebase, no longer the
//                          CURSOR_ANIM_STATES+0x100 alias), STRAT_MOVE_MICROSTEPS
// Every one is pointed at the fixture's OWN buffer, so a sibling reached through state() sees exactly
// the state the case built. FACING_STEP_SIGN owns a zeroed scratch of 256 entries -- generous on
// purpose (the live table is 25), so an out-of-domain facing_target introduced by a mutation lands
// in it rather than in the heap.
constexpr mh::state::region_id REDIRECTED[] = {
    mh::state::RID_STRAT_CUR_UNIT,
    mh::state::RID_STRAT_CUR_BUILDING,
    mh::state::RID_STRAT_CUR_PROJECTILE,
    mh::state::RID_STRAT_CUR_FX_ANIM,
    mh::state::RID_GENERAL,
    mh::state::RID_PLAYERSIDE,
    mh::state::RID_CLICK_SELECT_TARGET_FLAGS,
    mh::state::RID_CLICK_SELECT_TARGET_ID,
    mh::state::RID_UNITS,
    mh::state::RID_BUILDINGS,
    mh::state::RID_UNIT,
    mh::state::RID_BUILDING,
    mh::state::RID_STRAT_MOVE_MICROSTEPS,
    mh::state::RID_STRAT_FACING_STEP_SIGN,
};

class live_regions_on_fixture {
public:
    explicit live_regions_on_fixture(sim_fixture &f)
        : cur_unit_(f.cur_unit_ptr), cur_building_(f.cur_building_ptr),
          cur_projectile_(f.cur_projectile_ptr), cur_fx_anim_(f.cur_fx_anim_ptr),
          // PARENTHESES, NOT BRACES -- uint8_t is a scalar element type, so `{N}` would build a
          // ONE-element vector holding N (sim_test_support.h's own banner, and build_selftest.bat's
          // --asan banner, both document this exact trap).
          facing_step_scratch_(std::vector<uint8_t>((size_t)(256 * sizeof(facing_step_offset_pair)))) {
        bind(mh::state::RID_STRAT_CUR_UNIT, &cur_unit_, sizeof(cur_unit_));
        bind(mh::state::RID_STRAT_CUR_BUILDING, &cur_building_, sizeof(cur_building_));
        bind(mh::state::RID_STRAT_CUR_PROJECTILE, &cur_projectile_, sizeof(cur_projectile_));
        bind(mh::state::RID_STRAT_CUR_FX_ANIM, &cur_fx_anim_, sizeof(cur_fx_anim_));
        bind(mh::state::RID_GENERAL, &f.geom, sizeof(map_geom));
        bind(mh::state::RID_PLAYERSIDE, &f.player_side, sizeof(int16_t));
        bind(mh::state::RID_CLICK_SELECT_TARGET_FLAGS, &f.click_select_target_flags, sizeof(uint16_t));
        bind(mh::state::RID_CLICK_SELECT_TARGET_ID, &f.click_select_target_id, sizeof(uint16_t));
        bind(mh::state::RID_UNITS, f.units.data(), f.units.size() * sizeof(unit));
        bind(mh::state::RID_BUILDINGS, f.buildings.data(), f.buildings.size() * sizeof(building));
        bind(mh::state::RID_UNIT, f.cfg_units.data(), f.cfg_units.size() * sizeof(cfg_unit));
        bind(mh::state::RID_BUILDING, f.cfg_buildings.data(), f.cfg_buildings.size() * sizeof(cfg_building));
        bind(mh::state::RID_STRAT_MOVE_MICROSTEPS, f.move_microsteps.data(),
             f.move_microsteps.size() * sizeof(move_microstep));
        bind(mh::state::RID_STRAT_FACING_STEP_SIGN, facing_step_scratch_.data(),
             facing_step_scratch_.size());
        // get_coords' ground arm ends in `% geom.big_width` / `% geom.big_height`. Zero there is an
        // integer division by zero -- a CRASH rather than a red line -- and the fixture's geom starts
        // zeroed. Since this object exists precisely so a mutation cannot turn a failure into a crash,
        // it gives the map a real size (256x64 tiles at 32px) when the case has not set one. Nothing in
        // this closure reads either field except that helper, and no case below asserts on them.
        if (f.geom.big_width == 0) f.geom.big_width = 8192;
        if (f.geom.big_height == 0) f.geom.big_height = 2048;
    }
    ~live_regions_on_fixture() {
        for (mh::state::region_id r : REDIRECTED) mh::state::unrebase(r);
    }
    live_regions_on_fixture(const live_regions_on_fixture &)            = delete;
    live_regions_on_fixture &operator=(const live_regions_on_fixture &) = delete;

private:
    static void bind(mh::state::region_id r, void *p, size_t n) {
        mh::state::rebase(r, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)),
                          static_cast<uint32_t>(n));
    }

    unit                *cur_unit_;
    building            *cur_building_;
    projectile          *cur_projectile_;
    fx_anim             *cur_fx_anim_;
    std::vector<uint8_t> facing_step_scratch_;
};

// ---- small seeding helpers ------------------------------------------------------------------------

// The packed tile/unit-stack word the original addresses with one `MOVZX reg, word ptr` -- high
// nibble = owning player, low 12 bits = roster index (0x0044c5f4-0x0044c5ff computes the owner as
// `(w & 0xf000) >> 12`, 0x0044c623-0x0044c629 the index as `w & 0x0fff`).
void put_unit_word(uint8_t (&packed)[2], uint32_t owner, uint32_t index) {
    const uint16_t w = static_cast<uint16_t>((owner << 12) | (index & 0x0fffu));
    packed[0]        = static_cast<uint8_t>(w & 0xffu);
    packed[1]        = static_cast<uint8_t>(w >> 8);
}

// A unit that will absorb damage without dying: energy far above anything a case adds.
unit &seed_live_unit(sim_fixture &f, uint32_t player, int32_t index, uint16_t proto) {
    unit &u          = f.u((int32_t)player, index);
    u.unit_proto_id  = proto;
    u.energy         = 1000000000.0;
    u.pending_damage = 0.0;
    return u;
}

} // namespace

// ==== (1) llm_strat_weapon_pixel_distance_ratio @0x00448cc3 =========================================
//
// The whole body, from the listing:
//   0x00448ce4-0x00448cf8  pixel_delta_wrapped(EAX=x1, EDX=y1, EBX=x2, ECX=y2, &dx, &dy)
//                          -- &dx is pushed SECOND (it lands at [EBP-0x10], the slot the two IMULs at
//                             0x00448d04/0x00448d07 read), &dy FIRST ([EBP-0xc], read at 0x00448d0b).
//   0x00448d04-0x00448d14  dist_sq = dx*dx + dy*dy, 32-bit IMUL/IMUL/ADD into a DWORD stack slot
//   0x00448d17-0x00448d20  FILD dword ptr [dist_sq] -> llm_sqrt          (widened only HERE)
//   0x00448d25             FMUL   Weapon[weapon_id].speed  (double, weapon_id*0x16c + 0xc3a642)
//   0x00448d32-0x00448d38  FILD   Weapon[weapon_id].length (int32,  weapon_id*0x16c + 0xc3a666), FDIVP
namespace {

void test_wp_argument_passthrough_and_ratio() {
    sim_fixture f;
    g_wp.reset();
    // Three adjacent weapon rows with distinct values, so an off-by-one row index cannot pass.
    f.cfg_weapons[4].speed  = 100.0;
    f.cfg_weapons[4].length = 7;
    f.cfg_weapons[5].speed  = 3.0;
    f.cfg_weapons[5].length = 4;
    f.cfg_weapons[6].speed  = 0.5;
    f.cfg_weapons[6].length = 8;
    g_wp.dx_out             = 3;
    g_wp.dy_out             = 4;
    g_wp.sqrt_ret           = 5.0;

    sim_view     v = f.view();
    const double r = detail::weapon_pixel_distance_ratio(v, 5, 11, 22, 33, 44, rec_wp_calls());

    ck_eq((uint32_t)g_wp.n_delta, 1u, "wp: pixel_delta_wrapped called exactly once");
    ck(g_wp.x1 == 11 && g_wp.y1 == 22 && g_wp.x2 == 33 && g_wp.y2 == 44,
       "wp: pixel_delta_wrapped(x1=11, y1=22, x2=33, y2=44) -- EAX/EDX/EBX/ECX order, unpermuted");
    ck_eq((uint32_t)g_wp.n_sqrt, 1u, "wp: llm_sqrt called exactly once");
    ck_eq_d(g_wp.sqrt_arg, 25.0, "wp: llm_sqrt receives dx*dx + dy*dy = 3*3 + 4*4 = 25");
    ck_eq_d(r, 3.75, "wp: (sqrt(25)=5.0 * Weapon[5].speed 3.0) / Weapon[5].length 4 = 3.75");
}

void test_wp_indexes_the_named_weapon_row() {
    sim_fixture f;
    g_wp.reset();
    f.cfg_weapons[5].speed  = 3.0;
    f.cfg_weapons[5].length = 4;
    f.cfg_weapons[6].speed  = 0.5;
    f.cfg_weapons[6].length = 8;
    g_wp.dx_out             = 3;
    g_wp.dy_out             = 4;
    g_wp.sqrt_ret           = 5.0;

    sim_view     v = f.view();
    const double r = detail::weapon_pixel_distance_ratio(v, 6, 0, 0, 0, 0, rec_wp_calls());

    ck_eq_d(r, 0.3125,
            "wp: weapon_id 6 -> (5.0 * 0.5) / 8 = 0.3125, NOT weapon 5's 3.75 -- the 0x16c-stride row "
            "index is the argument, not the argument off by one");
}

void test_wp_dist_sq_is_a_32_bit_wrapping_sum() {
    sim_fixture f;
    g_wp.reset();
    f.cfg_weapons[5].speed  = 3.0;
    f.cfg_weapons[5].length = 4;
    // 65536*65536 == 2^32, which is exactly 0 in the DWORD stack slot the original stores into at
    // 0x00448d14 before FILD widens it at 0x00448d17. A translation that promoted to double FIRST
    // would hand llm_sqrt 4294967296.0 instead.
    g_wp.dx_out   = 65536;
    g_wp.dy_out   = 0;
    g_wp.sqrt_ret = 123.0;

    sim_view     v = f.view();
    const double r = detail::weapon_pixel_distance_ratio(v, 5, 0, 0, 0, 0, rec_wp_calls());

    ck_eq_d(g_wp.sqrt_arg, 0.0,
            "wp: dx=65536 -> dx*dx wraps to 0 in the int32 slot (0x00448d04-0x00448d14), so llm_sqrt "
            "sees 0.0, not 4294967296.0");
    ck_eq_d(r, 123.0 * 3.0 / 4.0, "wp: the wrapped-to-zero sum still flows through speed/length");
}

void test_wp_negative_deltas_square_positive() {
    sim_fixture f;
    g_wp.reset();
    f.cfg_weapons[5].speed  = 0.25;
    f.cfg_weapons[5].length = 2;
    g_wp.dx_out             = -6;
    g_wp.dy_out             = 8;
    g_wp.sqrt_ret           = 10.0;

    sim_view     v = f.view();
    const double r = detail::weapon_pixel_distance_ratio(v, 5, 0, 0, 0, 0, rec_wp_calls());

    ck_eq_d(g_wp.sqrt_arg, 100.0, "wp: (-6)*(-6) + 8*8 = 100 -- signed IMUL, so a negative dx squares positive");
    ck_eq_d(r, 1.25, "wp: (10.0 * 0.25) / 2 = 1.25");
}

void test_wp_length_is_a_signed_divisor() {
    sim_fixture f;
    g_wp.reset();
    f.cfg_weapons[5].speed = 3.0;
    // FILD dword ptr (0x00448d32) is a SIGNED integer load, so a negative length divides negative.
    // An unsigned read of the same bytes would give 4294967292 and a ratio of ~3.5e-9.
    f.cfg_weapons[5].length = -4;
    g_wp.dx_out             = 3;
    g_wp.dy_out             = 4;
    g_wp.sqrt_ret           = 5.0;

    sim_view     v = f.view();
    const double r = detail::weapon_pixel_distance_ratio(v, 5, 0, 0, 0, 0, rec_wp_calls());

    ck_eq_d(r, -3.75, "wp: Weapon.length is FILD'd as SIGNED int32 -- length -4 gives -3.75");
}

// ==== (2) llm_strat_unit_kill_credit @0x0044cac9 ====================================================
//
// The four-part shape, read off the listing:
//   0x0044caea-0x0044cb23  scaled = llm_math_scale_pct(damage, Unit[victim.proto].armor_prob[VICTIM
//                          player]) -- 0x0044cb06 recomputes the row from [EBP-0x14], the VICTIM's own
//                          player id, NOT the killer's.
//   0x0044cb26-0x0044cbe2  pre = pending; e = energy; pending += scaled;
//                          just_died = (pre < e) && (e <= pending)   (two FCOMP/SAHF pairs, 0x0044cb52
//                          with JNC and 0x0044cbc1 with JC -- a TRANSITION, not a state)
//   0x0044cbf3-0x0044cc8b  on just_died: if ((killer_info & 0x80) && (killer_info & 0xf) != victim)
//                            killer.experience += Unit[victim.proto].kill_score; notify_ui(...)
//                          then ALWAYS profile[killer & 0xf].units_killed_total[planet] += 1
//                          (0x0044cc74: killer*0x740 + planet*4 + 0xcff674)
//   0x0044cc8b-0x0044cda7  if (victim != killer && Unit[proto].soldier_count < 1) the local-player
//                          feedback window, then units[victim][0].activity_clock = game_clock
//   0x0044cda7-0x0044ce03  now_dead = (energy <= pending), re-derived FRESH; then
//                          ai_bldg_register_visible_building(victim_index, victim|0x80,
//                          killer_unit_index, killer_info & 0xffff, now_dead)

// Seeds a victim unit whose cfg row has DISTINCT armor_prob entries, so indexing the row by the wrong
// player is visible. armor_prob[i] = 10 + 7*i: [2]=24, [3]=31, [5]=45.
void seed_uk_cfg(sim_fixture &f) {
    for (int32_t i = 0; i < 9; ++i) f.cfg_units[VICTIM_PROTO].armor_prob[i] = 10 + 7 * i;
    f.cfg_units[VICTIM_PROTO].soldier_count = 0;
    f.cfg_units[VICTIM_PROTO].kill_score    = 37; // the per-kill experience award
    f.cfg_units[VICTIM_PROTO].type          = 0;  // < UNIT_TYPE_A_HELI (0x0f): get_coords' wrap arm
    f.planet_index                          = PLANET;
}

void test_uk_scales_damage_by_the_victims_own_armor_row() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    unit &u  = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.energy = 1000.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    // killer nibble == VICTIM_P: the victim!=killer feedback block stays shut, so this case observes
    // ONLY the scale/accumulate/register spine.
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 8.0, 0x0003u, KILLER_I);

    ck_eq((uint32_t)g_kc.scale_pct.size(), 1u, "uk: llm_math_scale_pct called exactly once");
    ck_eq_d(scale_at(0).value, 8.0, "uk: scale_pct receives the RAW damage");
    ck_eq((uint32_t)scale_at(0).pct, 31u,
          "uk: scale_pct's pct is armor_prob[VICTIM player 3] = 31, not the killer's row (armor_prob[5] "
          "= 45) -- 0x0044cb06 re-reads [EBP-0x14], the victim's own id");
    ck_eq_d(u.pending_damage, 8.0, "uk: the scaled damage lands in the victim's pending_damage");
    ck_eq_d(u.energy, 1000.0, "uk: energy itself is NOT decremented here (that is apply_damage's job)");
    ck_eq((uint32_t)g_kc.reg.size(), 1u, "uk: register_visible_building fires exactly once, always");
    ck_eq((uint32_t)reg_at(0).victim_index, (uint32_t)VICTIM_I, "uk: register victim_index = victim unit index");
    ck_eq(reg_at(0).victim_ref, VICTIM_P | 0x80u, "uk: register victim_ref = victim_player | 0x80 (0x0044cdfb, OR AL,0x80)");
    ck_eq(reg_at(0).aggressor_unit_index, (uint32_t)KILLER_I, "uk: register aggressor_unit_index = killer_unit_index");
    ck_eq(reg_at(0).aggressor_ref, 0x0003u, "uk: register aggressor_ref = killer_info & 0xffff");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 0u, "uk: energy(1000) > pending(8) -> victim_destroyed = 0");
}

void test_uk_just_died_transition_credits_the_planet_total() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    unit &u                                     = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.energy                                    = 10.0;
    u.pending_damage                            = 0.0;
    f.game_clock                                = 10.0;
    f.u((int32_t)KILLER_P, KILLER_I).experience = 111;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    // killer_info 0x0005: a DIFFERENT player, but WITHOUT bit 0x80 -- the experience/notify arm stays
    // shut while the planet-total arm (which is not gated on 0x80 at all) still runs.
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0005u, KILLER_I);

    ck_eq_d(u.pending_damage, 10.0, "uk(just_died): pending 0 -> 10, exactly energy");
    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[PLANET], 1u,
          "uk(just_died): profile[killer 5].units_killed_total[planet 3] += 1");
    ck_eq((uint32_t)f.profiles[VICTIM_P].units_killed_total[PLANET], 0u,
          "uk(just_died): the VICTIM's own profile is not credited");
    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[0], 0u,
          "uk(just_died): the credit is indexed by planet_index (3), not a hardcoded 0");
    ck_eq((uint32_t)f.profiles[KILLER_P].buildings_killed_total[PLANET], 0u,
          "uk(just_died): the UNIT counter is bumped, not the building one (0xcff674, not 0xcff6f4)");
    ck_eq((uint32_t)f.u((int32_t)KILLER_P, KILLER_I).experience, 111u,
          "uk(just_died, killer_info without bit 0x80): no experience award (0x0044cbf3 TEST word,0x80)");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 1u, "uk(just_died): energy(10) <= pending(10) -> victim_destroyed = 1");
    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 10.0,
            "uk: victim!=killer and soldier_count<1 -> units[victim][0].activity_clock = game_clock, "
            "outside the notify window (0x0044cd91-0x0044cda1)");
}

void test_uk_already_dead_before_this_damage_is_not_a_transition() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    unit &u = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    // pre_pending(10) is NOT < energy(10): the first of the two comparisons already fails, so this is
    // a corpse taking another hit, not a kill.
    u.energy         = 10.0;
    u.pending_damage = 10.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq_d(u.pending_damage, 15.0, "uk(already dead): the damage still accumulates");
    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[PLANET], 0u,
          "uk(already dead): NO planet-total credit -- just_died is the pre<energy<=post TRANSITION, "
          "not the standing energy<=pending state");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 1u,
          "uk(already dead): victim_destroyed is nonetheless 1 -- it is the FRESH energy<=pending read "
          "at 0x0044cdcd, a different question from just_died");
}

void test_uk_survivor_is_neither_a_kill_nor_destroyed() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    unit &u          = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.energy         = 100.0;
    u.pending_damage = 0.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0085u, KILLER_I);

    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[PLANET], 0u, "uk(survives): no planet-total credit");
    ck_eq((uint32_t)f.u((int32_t)KILLER_P, KILLER_I).experience, 0u, "uk(survives): no experience award");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 0u, "uk(survives): victim_destroyed = 0");
}

// THE MANDATORY CASE (SIM1E's own acceptance clause). The original has NO liveness check on the
// killer: 0x0044cc10-0x0044cc50 computes the killer's roster address straight from (killer_info & 0xf,
// killer_unit_index) and does `ADD dword ptr [.. + experience], EDX` with nothing in between, then
// 0x0044cc64 calls notify_ui on the same pair. So a killer whose own record has already been torn
// down still takes the credit, and that ordering is reachable in the real game's teardown.
void test_uk_dead_killer_still_receives_the_credit() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    live_regions_on_fixture redirect(f); // notify_ui is reached on this path -- see the file banner

    unit &u          = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.energy         = 10.0;
    u.pending_damage = 0.0;

    // The killer, as the teardown leaves it: zero energy, damage far past it, proto id cleared.
    unit &killer          = f.u((int32_t)KILLER_P, KILLER_I);
    killer.unit_proto_id  = 0;
    killer.energy         = 0.0;
    killer.pending_damage = 500.0;
    killer.experience     = 111;

    // player_side == the KILLER's id, so notify_ui's local-player arm fires and stamps change_flag --
    // that write is the only offline evidence that notify_ui ran, and on WHICH side.
    f.player_side       = (int16_t)KILLER_P;
    f.geom.change_flag  = 0;
    f.geom.change_flag2 = 0x55;
    f.game_clock        = 10.0; // 0 + unit_lost_feedback_cooldown(60) < 10 is FALSE -> no camera pan

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0085u, KILLER_I);

    ck_eq((uint32_t)killer.experience, 148u,
          "uk(dead killer): experience 111 + Unit[VICTIM's proto].kill_score(37) = 148 -- credited "
          "to a killer whose record is already dead, exactly as the original does (no liveness guard)");
    ck_eq_d(killer.energy, 0.0, "uk(dead killer): the killer's energy is not touched by the credit");
    ck_eq_d(killer.pending_damage, 500.0, "uk(dead killer): the killer's pending_damage is not touched either");
    ck_eq((uint32_t)f.geom.change_flag, 1u,
          "uk(dead killer): notify_ui ran and took its local-player arm -> general.change_flag = 1, "
          "which also proves it was called with the KILLER's side (5), not the victim's (3)");
    ck_eq((uint32_t)(uint16_t)f.geom.change_flag2, 0u,
          "uk(dead killer): the same single dword store zeroes change_flag2 (seeded 0x55)");
    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[PLANET], 1u,
          "uk(dead killer): the planet total is credited to the dead killer too");
    ck_eq((uint32_t)g_kc.reg.size(), 1u, "uk(dead killer): one register_visible_building call");
    ck(reg_at(0).victim_index == VICTIM_I && reg_at(0).victim_ref == (VICTIM_P | 0x80u) &&
           reg_at(0).aggressor_unit_index == (uint32_t)KILLER_I &&
           reg_at(0).aggressor_ref == 0x0085u && reg_at(0).victim_destroyed == 1,
       "uk(dead killer): register(victim 4, 0x83, killer_unit 9, killer_info 0x85, destroyed 1)");
    ck_eq((uint32_t)g_kc.snd_play.size(), 0u, "uk(dead killer): cooldown not elapsed -> no sound");
    ck_eq((uint32_t)g_kc.text_ids.size(), 0u, "uk(dead killer): cooldown not elapsed -> no text notify");
    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 10.0,
            "uk(dead killer): units[victim][0].activity_clock is stamped even though the window was shut");
}

void test_uk_self_kill_gate_blocks_experience_but_not_the_planet_total() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    live_regions_on_fixture redirect(f); // so a broken gate that DOES call notify_ui fails, not crashes

    unit &u                                     = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.energy                                    = 10.0;
    u.pending_damage                            = 0.0;
    f.u((int32_t)VICTIM_P, KILLER_I).experience = 111;
    f.player_side                               = (int16_t)VICTIM_P; // notify_ui would stamp change_flag
    f.geom.change_flag                          = 0;
    f.game_clock                                = 10.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    // killer_info 0x0083: bit 0x80 SET, but the low nibble is the victim's own player id.
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0083u, KILLER_I);

    ck_eq((uint32_t)f.u((int32_t)VICTIM_P, KILLER_I).experience, 111u,
          "uk(self-kill): (killer_info & 0xf) == victim_player -> no experience award (0x0044cc0a CMP/JNZ)");
    ck_eq((uint32_t)f.geom.change_flag, 0u,
          "uk(self-kill): notify_ui was NOT called -- change_flag would be 1 if the gate had let it through");
    ck_eq((uint32_t)f.profiles[VICTIM_P].units_killed_total[PLANET], 1u,
          "uk(self-kill): the planet total IS still credited -- 0x0044cc69 is outside the cross-player gate");
    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 0.0,
            "uk(self-kill): victim == killer -> the whole feedback block is skipped, so activity_clock "
            "is NOT stamped (0x0044cc9c JZ straight past it)");
}

void test_uk_soldier_bearing_victim_skips_the_feedback_block() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_cfg(f);
    f.cfg_units[VICTIM_PROTO].soldier_count = 2; // > 0 -> 0x0044ccc9's JG skips the whole block
    seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    f.game_clock = 100.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 0.0,
            "uk(soldier_count 2): the feedback block is gated on soldier_count < 1, so activity_clock "
            "is not stamped at all");
    ck_eq((uint32_t)g_kc.snd_play.size(), 0u, "uk(soldier_count 2): no sound");
    ck_eq((uint32_t)g_kc.text_ids.size(), 0u, "uk(soldier_count 2): no text notify");
}

// The local-player feedback window. get_coords is reached here (a sibling PUBLIC wrapper), so the
// region redirect is mandatory -- see the file banner.
//
// get_coords' own arithmetic, for a proto with type < UNIT_TYPE_A_HELI (0x0f):
//   remaining = 0x1f - move_microstep;  dx = dy = 0 (the redirected FACING_STEP_SIGN scratch is zero)
//   out_x = (big_width  + dx*remaining + u.x*32 + 16) % big_width
//   out_y = (big_height + dy*remaining + u.y*32 + 16) % big_height
// then the caller applies fine_to_tile (the SAR/SHL/SBB/SAR idiom at 0x0044cd51-0x0044cd82 = /32) in
// place to both.
void seed_uk_feedback(sim_fixture &f) {
    seed_uk_cfg(f);
    unit &u          = seed_live_unit(f, VICTIM_P, VICTIM_I, VICTIM_PROTO);
    u.x              = 10;
    u.y              = 6;
    u.move_microstep = 0;
    u.facing_target  = 0;
    // The roster slot-0 record deliberately sits somewhere ELSE, so a translation that panned to
    // units[victim][0] (the cooldown record) instead of the victim would produce col 30, not 10.
    unit &slot0          = f.u((int32_t)VICTIM_P, 0);
    slot0.x              = 30;
    slot0.y              = 12;
    slot0.activity_clock = 0.0;
    f.geom.big_width     = 8192; // 256 tiles * 32
    f.geom.big_height    = 2048; // 64 tiles * 32
    f.player_side        = (int16_t)VICTIM_P;
    f.game_clock         = 100.0; // 0 + unit_lost_feedback_cooldown(60.0) < 100 -> the window is OPEN
}

void test_uk_local_player_feedback_pans_sounds_and_notifies() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_feedback(f);
    live_regions_on_fixture redirect(f);
    f.sim_active  = 1;
    f.player_race = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck(g_kc.snd_play.size() == 1 && snd_at(0).a == 2 && snd_at(0).b == 100,
       "uk(local feedback, race != 2): llm_snd_play(0 + 2, 100) -- 0x0044cd2d ADD EAX,0x2 / EDX = 0x64");
    ck(g_kc.text_ids.size() == 1 && text_at(0) == 0x73,
       "uk(local feedback): llm_ui_print_queue_text_id(0x73) -- 0x0044cd87 MOV EAX,0x73");
    ck_eq((uint32_t)f.cam_pan_target_col, 10u,
          "uk(local feedback): CAM_PAN_TARGET_COL = fine_to_tile((8192 + 10*32 + 16) % 8192 = 336) = 10 "
          "-- the VICTIM's tile, not slot 0's 30");
    ck_eq((uint32_t)f.cam_pan_target_row, 6u,
          "uk(local feedback): CAM_PAN_TARGET_ROW = fine_to_tile((2048 + 6*32 + 16) % 2048 = 208) = 6");
    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 100.0,
            "uk(local feedback): units[victim][0].activity_clock refreshed to game_clock");
}

void test_uk_race2_shifts_the_sound_id() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_feedback(f);
    live_regions_on_fixture redirect(f);
    f.sim_active  = 1;
    f.player_race = 2; // 0x0044cd14 CMP PLAYER_RACE,0x2 -> base 0x12 instead of 0

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck(g_kc.snd_play.size() == 1 && snd_at(0).a == 0x14,
       "uk(local feedback, race == 2): llm_snd_play(0x12 + 2 = 0x14, 100)");
}

void test_uk_sim_inactive_skips_only_the_sound() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_feedback(f);
    live_regions_on_fixture redirect(f);
    f.sim_active = 0; // 0x0044cd04 CMP SIM_ACTIVE,0 / JZ -- a REAL gate on this arm

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)g_kc.snd_play.size(), 0u, "uk(sim inactive): no sound -- this arm's sim_active gate is live");
    ck(g_kc.text_ids.size() == 1 && text_at(0) == 0x73,
       "uk(sim inactive): the pan + text notify still happen (only the sound is gated)");
    ck_eq((uint32_t)f.cam_pan_target_col, 10u, "uk(sim inactive): the camera still pans");
}

void test_uk_non_local_player_gets_no_feedback_but_still_stamps_the_clock() {
    sim_fixture f;
    g_kc.reset();
    seed_uk_feedback(f);
    live_regions_on_fixture redirect(f);
    f.sim_active  = 1;
    f.player_side = 7; // the victim (3) is NOT the local side

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::unit_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)g_kc.snd_play.size(), 0u, "uk(not the local side): no sound");
    ck_eq((uint32_t)g_kc.text_ids.size(), 0u, "uk(not the local side): no text notify");
    ck_eq((uint32_t)f.cam_pan_target_col, 0u, "uk(not the local side): no camera pan");
    ck_eq_d(f.u((int32_t)VICTIM_P, 0).activity_clock, 100.0,
            "uk(not the local side): activity_clock is stamped ANYWAY -- 0x0044cd91 sits outside the "
            "PlayerSide branch");
}

// ==== (3) llm_strat_bldg_kill_credit @0x0044c67f ====================================================
//
// Same four-part shape as the unit twin, with three real differences read off the listing:
//   0x0044c6bc/0x0044c6e5  a MAIN_BASE victim (cfg type 0x22 or 0x0e) multiplies `damage` by the
//                          0x00500f0c boot constant BEFORE anything else. There is no armor_prob /
//                          llm_math_scale_pct step at all on the building side.
//   0x0044c896/0x0044c8bf  the feedback block SPLITS on MOTHER (0x1a / 0x06) vs everything else:
//                            MOTHER     -> gate on buildings[p][0].cycle_progress + K_mother(0x500f14),
//                                          snd_play gated on SIM_ACTIVE, text id 2, cycle_progress
//                                          stamped INSIDE the window, last_tick_time stamped ALWAYS
//                            non-MOTHER -> gate on buildings[p][0].last_tick_time + K_other(0x500f1c),
//                                          NO snd_play at all (0x0044c9de's CMP SIM_ACTIVE,0 has its
//                                          flags clobbered by the very next CMP before any Jcc reads
//                                          them), text id 3, last_tick_time stamped
//   0x0044cab1             the register call's victim_ref is victim_player | 0x40, not | 0x80

void seed_bk(sim_fixture &f, uint8_t bldg_type) {
    f.planet_index                      = PLANET;
    f.cfg_buildings[BLDG_ID].type       = bldg_type;
    f.cfg_buildings[BLDG_ID].kill_score = 41; // the per-kill experience award
    building &b                         = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.building_id                       = BLDG_ID;
    b.energy                            = 1000000000.0;
    b.pending_damage                    = 0.0;
}

void test_bk_main_base_type_scales_the_damage() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_H_MAIN_BASE);
    // The fixture's boot value is 0.0001, which is not exactly representable; 0.5 keeps the expectation
    // exact and is still distinct from every OTHER double in sim_view's boot-constant run (20.0, 60.0,
    // 60.0), so a translation that reached for the wrong one of the four disagrees here.
    f.bldg_main_base_damage_mult = 0.5;
    building &b                  = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy                     = 1000.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 8.0, 0x0003u, KILLER_I);

    ck_eq_d(b.pending_damage, 4.0, "bk(H_MAIN_BASE 0x22): damage 8.0 * 0.5 = 4.0 (0x0044c6f1 FMUL)");
    ck_eq((uint32_t)g_kc.scale_pct.size(), 0u,
          "bk: the building side never calls llm_math_scale_pct -- there is no armor_prob step");
}

void test_bk_alien_main_base_type_also_scales() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_A_MAIN_BASE);
    f.bldg_main_base_damage_mult = 0.5;
    building &b                  = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy                     = 1000.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 8.0, 0x0003u, KILLER_I);

    ck_eq_d(b.pending_damage, 4.0, "bk(A_MAIN_BASE 0x0e): the second CMP arm scales identically");
}

void test_bk_other_type_is_not_scaled() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_PLAIN);
    f.bldg_main_base_damage_mult = 0.5;
    building &b                  = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy                     = 1000.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 8.0, 0x0003u, KILLER_I);

    ck_eq_d(b.pending_damage, 8.0, "bk(type 0x07): NOT a main base -> the damage passes through unscaled");
}

void test_bk_just_died_transition_credits_the_building_total() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_PLAIN);
    building &b      = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy         = 10.0;
    b.pending_damage = 0.0;
    f.game_clock     = 10.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)f.profiles[KILLER_P].buildings_killed_total[PLANET], 1u,
          "bk(just_died): profile[killer].buildings_killed_total[planet] += 1 (0x0044c859, base 0xcff6f4)");
    ck_eq((uint32_t)f.profiles[KILLER_P].units_killed_total[PLANET], 0u,
          "bk(just_died): the BUILDING counter is bumped, not the unit one");
    ck_eq((uint32_t)g_kc.reg.size(), 1u, "bk: register_visible_building fires exactly once, always");
    ck_eq(reg_at(0).victim_ref, VICTIM_P | 0x40u,
          "bk: register victim_ref = victim_player | 0x40 (0x0044cab1 OR AL,0x40), the building tag");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 1u, "bk(just_died): victim_destroyed = 1");
}

void test_bk_already_dead_before_this_damage_is_not_a_transition() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_PLAIN);
    building &b      = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy         = 10.0;
    b.pending_damage = 10.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)f.profiles[KILLER_P].buildings_killed_total[PLANET], 0u,
          "bk(already dead): pre_pending is not < energy -> no kill credit");
    ck_eq((uint32_t)reg_at(0).victim_destroyed, 1u,
          "bk(already dead): victim_destroyed is still 1 -- the fresh 0x0044ca83 re-read");
}

// The building-side twin of the mandatory dead-killer case.
void test_bk_dead_killer_still_receives_the_credit() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_PLAIN);
    live_regions_on_fixture redirect(f);

    building &b      = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy         = 10.0;
    b.pending_damage = 0.0;

    unit &killer          = f.u((int32_t)KILLER_P, KILLER_I);
    killer.unit_proto_id  = 0;
    killer.energy         = 0.0;
    killer.pending_damage = 500.0;
    killer.experience     = 111;

    f.player_side       = (int16_t)KILLER_P;
    f.geom.change_flag  = 0;
    f.geom.change_flag2 = 0x55;
    f.game_clock        = 10.0; // 0 + bldg_lost_feedback_cooldown_other(60.0) < 10 is FALSE

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0085u, KILLER_I);

    ck_eq((uint32_t)killer.experience, 152u,
          "bk(dead killer): experience 111 + Building[victim's id].kill_score(41) = 152, credited to "
          "an already-dead killer record");
    ck_eq_d(killer.energy, 0.0, "bk(dead killer): the killer's own record is otherwise untouched");
    ck_eq_d(killer.pending_damage, 500.0, "bk(dead killer): ... including its pending_damage");
    ck_eq((uint32_t)f.geom.change_flag, 1u,
          "bk(dead killer): notify_ui ran on the KILLER's side (change_flag = 1)");
    ck_eq((uint32_t)(uint16_t)f.geom.change_flag2, 0u, "bk(dead killer): change_flag2 zeroed by the same store");
    ck_eq((uint32_t)f.profiles[KILLER_P].buildings_killed_total[PLANET], 1u,
          "bk(dead killer): the planet total is credited too");
    ck(g_kc.reg.size() == 1 && reg_at(0).victim_index == VICTIM_I &&
           reg_at(0).victim_ref == (VICTIM_P | 0x40u) &&
           reg_at(0).aggressor_unit_index == (uint32_t)KILLER_I &&
           reg_at(0).aggressor_ref == 0x0085u && reg_at(0).victim_destroyed == 1,
       "bk(dead killer): register(victim 4, 0x43, killer_unit 9, killer_info 0x85, destroyed 1)");
}

void test_bk_self_kill_gate_blocks_experience_but_not_the_planet_total() {
    sim_fixture f;
    g_kc.reset();
    seed_bk(f, BLDG_TYPE_PLAIN);
    live_regions_on_fixture redirect(f);

    building &b                                 = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.energy                                    = 10.0;
    b.pending_damage                            = 0.0;
    f.u((int32_t)VICTIM_P, KILLER_I).experience = 111;
    f.player_side                               = (int16_t)VICTIM_P;
    f.geom.change_flag                          = 0;
    f.game_clock                                = 10.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 10.0, 0x0083u, KILLER_I);

    ck_eq((uint32_t)f.u((int32_t)VICTIM_P, KILLER_I).experience, 111u,
          "bk(self-kill): no experience award (0x0044c7de CMP/JNZ)");
    ck_eq((uint32_t)f.geom.change_flag, 0u, "bk(self-kill): notify_ui was NOT called");
    ck_eq((uint32_t)f.profiles[VICTIM_P].buildings_killed_total[PLANET], 1u,
          "bk(self-kill): the planet total is still credited");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).last_tick_time, 0.0,
            "bk(self-kill): victim == killer -> the whole feedback block is skipped (0x0044c870 JZ)");
}

// The MOTHER feedback arm. bldg_get_coords is reached (a sibling PUBLIC wrapper), so the region
// redirect is mandatory. Its arithmetic:
//   out_x = ((b.x << 5) + (cfg.width  << 4)) & geom.bw_mask
//   out_y = ((b.y << 5) + (cfg.height << 4)) & geom.bh_mask
// then fine_to_tile (/32) in place.
void seed_bk_feedback(sim_fixture &f, uint8_t bldg_type) {
    seed_bk(f, bldg_type);
    f.cfg_buildings[BLDG_ID].width  = 2;
    f.cfg_buildings[BLDG_ID].height = 4;
    building &b                     = f.b((int32_t)VICTIM_P, VICTIM_I);
    b.x                             = 10;
    b.y                             = 6;
    // Slot 0 (the cooldown record) is a DIFFERENT building at a different place, with cfg row 0 whose
    // width/height are zero -- so a translation that panned to slot 0 would report col 30, not 11.
    building &slot0      = f.b((int32_t)VICTIM_P, 0);
    slot0.building_id    = 0;
    slot0.x              = 30;
    slot0.y              = 12;
    slot0.cycle_progress = 0.0;
    slot0.last_tick_time = 0.0;
    f.geom.bw_mask       = 0xffff;
    f.geom.bh_mask       = 0xffff;
    f.player_side        = (int16_t)VICTIM_P;
    f.game_clock         = 100.0;
}

void test_bk_mother_type_local_feedback() {
    sim_fixture f;
    g_kc.reset();
    seed_bk_feedback(f, BLDG_TYPE_H_MOTHER);
    live_regions_on_fixture redirect(f);
    f.sim_active  = 1;
    f.player_race = 2; // 0x0044c911 -> sound base 0x12

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck(g_kc.snd_play.size() == 1 && snd_at(0).a == 0x13 && snd_at(0).b == 100,
       "bk(MOTHER, local, race 2): llm_snd_play(0x12 + 1 = 0x13, 100) -- 0x0044c92d INC EAX, note the "
       "+1 here versus the unit side's +2");
    ck(g_kc.text_ids.size() == 1 && text_at(0) == 2,
       "bk(MOTHER, local): llm_ui_print_queue_text_id(2) -- 0x0044c982 MOV EAX,0x2");
    ck_eq((uint32_t)f.cam_pan_target_col, 11u,
          "bk(MOTHER, local): col = fine_to_tile(((10<<5) + (2<<4)) & 0xffff = 352) = 11 -- the VICTIM "
          "building, not slot 0's 30");
    ck_eq((uint32_t)f.cam_pan_target_row, 8u,
          "bk(MOTHER, local): row = fine_to_tile(((6<<5) + (4<<4)) & 0xffff = 256) = 8");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).cycle_progress, 100.0,
            "bk(MOTHER): cycle_progress stamped INSIDE the notify window (0x0044c99c)");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).last_tick_time, 100.0,
            "bk(MOTHER): last_tick_time stamped ALWAYS (0x0044c9ac, outside the window)");
}

void test_bk_mother_cooldown_not_elapsed_still_stamps_last_tick_time() {
    sim_fixture f;
    g_kc.reset();
    seed_bk_feedback(f, BLDG_TYPE_H_MOTHER);
    live_regions_on_fixture redirect(f);
    f.sim_active = 1;
    // 90 + bldg_lost_feedback_cooldown_mother(20.0) = 110, which is NOT < game_clock 100.
    f.b((int32_t)VICTIM_P, 0).cycle_progress = 90.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)g_kc.snd_play.size(), 0u, "bk(MOTHER, cooldown open): no sound");
    ck_eq((uint32_t)g_kc.text_ids.size(), 0u, "bk(MOTHER, cooldown open): no text notify");
    ck_eq((uint32_t)f.cam_pan_target_col, 0u, "bk(MOTHER, cooldown open): no camera pan");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).cycle_progress, 90.0,
            "bk(MOTHER, cooldown open): cycle_progress NOT stamped -- it lives inside the window");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).last_tick_time, 100.0,
            "bk(MOTHER, cooldown open): last_tick_time IS stamped -- it lives outside it. This "
            "asymmetry is the whole point of the MOTHER arm's two-timestamp shape");
}

void test_bk_non_mother_uses_the_other_cooldown_and_plays_no_sound() {
    sim_fixture f;
    g_kc.reset();
    seed_bk_feedback(f, BLDG_TYPE_PLAIN);
    live_regions_on_fixture redirect(f);
    // sim_active is DELIBERATELY 1: the original's own CMP SIM_ACTIVE,0 in this arm (0x0044c9de) is
    // dead (its flags are clobbered before any Jcc), and this arm calls no llm_snd_play at all. With
    // sim_active = 1, "no sound" can only be produced by the correct shape.
    f.sim_active  = 1;
    f.player_race = 2;
    // 0 + bldg_lost_feedback_cooldown_other(60.0) < game_clock 100 -> the window is open.
    f.b((int32_t)VICTIM_P, 0).last_tick_time = 0.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)g_kc.snd_play.size(), 0u,
          "bk(non-MOTHER): NO llm_snd_play exists in this arm, even with sim_active = 1");
    ck(g_kc.text_ids.size() == 1 && text_at(0) == 3,
       "bk(non-MOTHER): llm_ui_print_queue_text_id(3) -- 0x0044ca3d MOV EAX,0x3, a different id from "
       "the MOTHER arm's 2");
    ck_eq((uint32_t)f.cam_pan_target_col, 11u, "bk(non-MOTHER): the camera pans the same way");
    ck_eq((uint32_t)f.cam_pan_target_row, 8u, "bk(non-MOTHER): ... on both axes");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).last_tick_time, 100.0, "bk(non-MOTHER): last_tick_time stamped");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).cycle_progress, 0.0,
            "bk(non-MOTHER): cycle_progress is NEVER touched in this arm");
}

void test_bk_non_mother_cooldown_not_elapsed_skips_the_notify() {
    sim_fixture f;
    g_kc.reset();
    seed_bk_feedback(f, BLDG_TYPE_PLAIN);
    live_regions_on_fixture redirect(f);
    // 50 + 60.0 = 110, NOT < 100.
    f.b((int32_t)VICTIM_P, 0).last_tick_time = 50.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::bldg_kill_credit(v, own, rec_kc_calls(), VICTIM_P, VICTIM_I, 5.0, 0x0005u, KILLER_I);

    ck_eq((uint32_t)g_kc.text_ids.size(), 0u, "bk(non-MOTHER, cooldown open): no text notify");
    ck_eq((uint32_t)f.cam_pan_target_col, 0u, "bk(non-MOTHER, cooldown open): no camera pan");
    ck_eq_d(f.b((int32_t)VICTIM_P, 0).last_tick_time, 100.0,
            "bk(non-MOTHER, cooldown open): last_tick_time is stamped regardless");
}

// ==== (4) llm_strat_apply_area_damage @0x0044c3fb ===================================================

// Places `index`'s unit-stack head on tile (tx,ty) and gives the unit a body that survives any damage
// the cases below apply. Returns the unit so the caller can assert on its pending_damage.
unit &place_unit_on_tile(sim_fixture &f, int32_t tx, int32_t ty, int32_t index) {
    unit &u = seed_live_unit(f, TILE_OWNER, index, VICTIM_PROTO);
    put_unit_word(f.t(tx, ty).unit, TILE_OWNER, (uint32_t)index);
    return u;
}

void seed_aad_cfg(sim_fixture &f) {
    for (int32_t i = 0; i < 9; ++i) f.cfg_units[VICTIM_PROTO].armor_prob[i] = 10 + 7 * i;
    f.cfg_units[VICTIM_PROTO].soldier_count = 0;
    f.planet_index                          = PLANET;
}

// THE MANDATORY CASE (SIM1E's own acceptance clause: "area damage over a hand-built target arrangement
// including the boundary radius"). Centre (20,20), ring_count 3, damage 12.0, over the fixture's masks
// (width 0xff, height 0x3f) so no wrap is involved and the geometry is the only variable.
//
// Rings 0,1,2 run; ring 3 does NOT (0x0044c42e's JL). Ring r covers the full (2r+1)^2 block, so a tile
// at Chebyshev distance d takes one hit per ring in [d,3), each of `12.0 / (r+1)`:
//   d = 0  ->  12/1 + 12/2 + 12/3 = 12 + 6 + 4 = 22.0   (all three exactly representable)
//   d = 1  ->         12/2 + 12/3 =      6 + 4 = 10.0
//   d = 2  ->                12/3 =          4 =  4.0   <- the LAST ring actually visited
//   d = 3  ->                                     0.0   <- the first ring NOT visited
// A perimeter-only walk would give the centre 12.0 and the d=1 tile 6.0; an inclusive ring bound would
// give the d=3 tiles 3.0. Either mutation turns this case red.
void test_aad_ring_boundary_over_a_hand_built_arrangement() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);

    unit &at_centre     = place_unit_on_tile(f, 20, 20, 11); // Chebyshev 0
    unit &at_ring1      = place_unit_on_tile(f, 21, 20, 12); // Chebyshev 1
    unit &at_ring2_edge = place_unit_on_tile(f, 22, 20, 13); // Chebyshev 2 -- last visited
    unit &at_ring3      = place_unit_on_tile(f, 23, 20, 14); // Chebyshev 3 -- NEVER visited
    unit &at_ring2_diag = place_unit_on_tile(f, 18, 22, 15); // Chebyshev 2, a corner of ring 2's block
    unit &at_ring3_diag = place_unit_on_tile(f, 17, 17, 16); // Chebyshev 3 diagonal -- NEVER visited

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    // target_kind 2 (the unit-stack arm); killer nibble == TILE_OWNER so every kill_credit's own
    // feedback block stays shut and this case observes geometry alone.
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 2, 12.0, 3, 0u, 0x0002u, KILLER_I);

    ck_eq_d(at_centre.pending_damage, 22.0,
            "aad(d=0): the centre is inside EVERY ring's block -> 12/1 + 12/2 + 12/3 = 22.0. A "
            "perimeter-only walk would leave 12.0 here");
    ck_eq_d(at_ring1.pending_damage, 10.0, "aad(d=1): rings 1 and 2 -> 12/2 + 12/3 = 10.0");
    ck_eq_d(at_ring2_edge.pending_damage, 4.0,
            "aad(d=2): ring 2 only -> 12/3 = 4.0 -- the LAST ring the exclusive bound reaches");
    ck_eq_d(at_ring2_diag.pending_damage, 4.0,
            "aad(d=2, diagonal corner): the ring is a FILLED block, so its corner is hit too");
    ck_eq_d(at_ring3.pending_damage, 0.0,
            "aad(d=3): ring_count is EXCLUSIVE (0x0044c42e JL) -- ring 3 never runs, so this tile takes "
            "NOTHING. An inclusive bound would put 3.0 here");
    ck_eq_d(at_ring3_diag.pending_damage, 0.0, "aad(d=3, diagonal): likewise untouched");
    ck_eq((uint32_t)g_kc.reg.size(), 7u,
          "aad: exactly 7 kill_credit calls -- 3 (centre) + 2 (d=1) + 1 + 1 (the two d=2 tiles) and "
          "none for d=3; a walk that visited more or fewer tiles disagrees");
    ck_eq((uint32_t)scale_at(0).pct, 24u,
          "aad: each kill_credit still scales by armor_prob[TILE_OWNER 2] = 24");
}

void test_aad_ring_count_zero_visits_nothing() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    unit &u = place_unit_on_tile(f, 20, 20, 11);

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 2, 12.0, 0, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 0u, "aad(ring_count 0): the loop body never runs -- not even the centre");
    ck_eq_d(u.pending_damage, 0.0, "aad(ring_count 0): the centre tile takes no damage");
}

void test_aad_masks_wrap_the_block_at_the_map_edge() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    // Centre (0,0). Ring 1's block starts at col = width_mask & (0-1) = 0xff = 255 and row =
    // height_mask & (0-1) = 0x3f = 63, so the tile diagonally "before" the origin is a real member.
    unit &at_origin = place_unit_on_tile(f, 0, 0, 11);
    unit &wrapped   = place_unit_on_tile(f, 255, 63, 12);

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 0, 0, 2, 8.0, 2, 0u, 0x0002u, KILLER_I);

    ck_eq_d(at_origin.pending_damage, 12.0, "aad(wrap): the origin is in rings 0 and 1 -> 8/1 + 8/2 = 12.0");
    ck_eq_d(wrapped.pending_damage, 4.0,
            "aad(wrap): (255,63) is ring 1's top-left corner via width_mask 0xff / height_mask 0x3f "
            "(0x0044c443 / 0x0044c47f) -> 8/2 = 4.0. Swapping the two masks moves this tile elsewhere");
}

void test_aad_building_class_dispatches_to_bldg_kill_credit() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    tile_object &t                = f.t(20, 20);
    t.building                    = 6;
    t.class_owner                 = (uint8_t)(0x40u | TILE_OWNER); // class nibble 0x40 -> the building arm
    building &b                   = f.b((int32_t)TILE_OWNER, 6);
    b.building_id                 = BLDG_ID;
    b.energy                      = 1000000000.0;
    f.cfg_buildings[BLDG_ID].type = BLDG_TYPE_PLAIN;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 1, 9.0, 1, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 1u, "aad(kind 1, class 0x40): exactly one kill_credit call");
    ck_eq((uint32_t)reg_at(0).victim_index, 6u, "aad(kind 1, class 0x40): victim index = tile.building");
    ck_eq(reg_at(0).victim_ref, TILE_OWNER | 0x40u,
          "aad(kind 1, class 0x40): routed to bldg_kill_credit (victim_ref carries the |0x40 tag)");
    ck_eq_d(b.pending_damage, 9.0, "aad(kind 1, class 0x40): ring 0 -> 9/1 lands on the building");
}

void test_aad_unit_class_reads_the_building_field_not_the_unit_field() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    tile_object &t = f.t(20, 20);
    t.building     = 6;
    t.class_owner  = (uint8_t)(0x80u | TILE_OWNER); // class nibble 0x80 -> the unit arm...
    t.unit[0]      = 0;                             // ...but the INDEX still comes from .building
    t.unit[1]      = 0;
    unit &u        = seed_live_unit(f, TILE_OWNER, 6, VICTIM_PROTO);

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 1, 9.0, 1, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 1u, "aad(kind 1, class 0x80): exactly one kill_credit call");
    ck_eq((uint32_t)reg_at(0).victim_index, 6u,
          "aad(kind 1, class 0x80): the victim index is tile.BUILDING (0x0044c5a1 MOVZX from +0x02), "
          "NOT tile.unit -- the .unit field here is zero, so reading it would give index 0");
    ck_eq(reg_at(0).victim_ref, TILE_OWNER | 0x80u,
          "aad(kind 1, class 0x80): routed to unit_kill_credit (victim_ref carries the |0x80 tag)");
    ck_eq_d(u.pending_damage, 9.0, "aad(kind 1, class 0x80): the damage lands on units[owner][6]");
}

void test_aad_class_nibbles_outside_0x40_and_0x80_are_ignored() {
    const uint8_t ignored[] = {0x10, 0x20, 0x30, 0x50, 0x60, 0x70, 0x90, 0xf0};
    for (uint8_t hi : ignored) {
        sim_fixture f;
        g_kc.reset();
        seed_aad_cfg(f);
        tile_object &t                     = f.t(20, 20);
        t.building                         = 6;
        t.class_owner                      = (uint8_t)(hi | TILE_OWNER);
        f.b((int32_t)TILE_OWNER, 6).energy = 1000000000.0;
        seed_live_unit(f, TILE_OWNER, 6, VICTIM_PROTO);

        live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
        sim_view                v   = f.view();
        sim_store               own = f.store();
        detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 1, 9.0, 1, 0u, 0x0002u, KILLER_I);

        ck_eq((uint32_t)g_kc.reg.size(), 0u,
              "aad(kind 1): a class nibble that is neither 0x40 nor 0x80 dispatches nowhere -- the "
              "JC(<0x40)/JBE(<=0x40)/JZ(==0x80) cascade at 0x0044c539-0x0044c549 falls through");
    }
}

void test_aad_zero_building_field_skips_the_building_arm() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    tile_object &t = f.t(20, 20);
    t.building     = 0; // 0x0044c4d6 CMP word ptr [...],0x0 / JNZ -- zero means "no occupant"
    t.class_owner  = (uint8_t)(0x40u | TILE_OWNER);

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 1, 9.0, 1, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 0u,
          "aad(kind 1): tile.building == 0 short-circuits the whole building arm before the class test");
}

void test_aad_unit_stack_chain_walks_every_link() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    // The tile names unit 5; each unit's OWN unit_above names the next (0x0044c635-0x0044c664 recomputes
    // the address from the CURRENT link's owner/index every step), terminating at a zero word.
    unit &u5 = place_unit_on_tile(f, 20, 20, 5);
    unit &u6 = seed_live_unit(f, TILE_OWNER, 6, VICTIM_PROTO);
    unit &u7 = seed_live_unit(f, TILE_OWNER, 7, VICTIM_PROTO);
    put_unit_word(u5.unit_above, TILE_OWNER, 6);
    put_unit_word(u6.unit_above, TILE_OWNER, 7);
    u7.unit_above[0] = 0;
    u7.unit_above[1] = 0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 2, 6.0, 1, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 3u, "aad(kind 2): the whole stack is credited, one call per link");
    ck((uint32_t)reg_at(0).victim_index == 5u && (uint32_t)reg_at(1).victim_index == 6u &&
           (uint32_t)reg_at(2).victim_index == 7u,
       "aad(kind 2): the chain is walked head-first, 5 -> 6 -> 7, then the zero word ends it");
    ck(reg_at(0).victim_ref == (TILE_OWNER | 0x80u) && reg_at(2).victim_ref == (TILE_OWNER | 0x80u),
       "aad(kind 2): the owner comes from each link word's HIGH nibble");
    ck_eq_d(u5.pending_damage, 6.0, "aad(kind 2): head of the stack damaged");
    ck_eq_d(u6.pending_damage, 6.0, "aad(kind 2): middle of the stack damaged");
    ck_eq_d(u7.pending_damage, 6.0, "aad(kind 2): tail of the stack damaged");
}

void test_aad_target_kind_other_touches_nothing() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    tile_object &t = f.t(20, 20);
    t.building     = 6;
    t.class_owner  = (uint8_t)(0x40u | TILE_OWNER);
    place_unit_on_tile(f, 20, 20, 11);
    f.b((int32_t)TILE_OWNER, 6).energy = 1000000000.0;

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    // target_kind 3: neither the ==1 test at 0x0044c4c2 nor the ==2 test at 0x0044c5b1 matches.
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 3, 9.0, 2, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 0u,
          "aad(kind 3): both arms are exact-value tests -- a tile carrying BOTH a building and a unit "
          "stack is still untouched");
}

void test_aad_target_kind_1_does_not_run_the_unit_stack_arm() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    unit &stacked = place_unit_on_tile(f, 20, 20, 11); // only the .unit field is set; .building stays 0

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 1, 9.0, 1, 0u, 0x0002u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 0u, "aad(kind 1): the unit-stack arm belongs to kind 2 only");
    ck_eq_d(stacked.pending_damage, 0.0, "aad(kind 1): the stacked unit is untouched");
}

void test_aad_killer_info_is_masked_to_16_bits() {
    sim_fixture f;
    g_kc.reset();
    seed_aad_cfg(f);
    place_unit_on_tile(f, 20, 20, 11);

    live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
    sim_view                v   = f.view();
    sim_store               own = f.store();
    // 0x0044c619 / 0x0044c621 push `MOVZX EAX, word ptr [killer_info]` -- only the low 16 bits reach
    // the kill-credit callee.
    detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 2, 9.0, 1, 0u, 0x12340082u, KILLER_I);

    ck_eq((uint32_t)g_kc.reg.size(), 1u, "aad(dirty killer_info): one call");
    ck_eq(reg_at(0).aggressor_ref, 0x0082u,
          "aad(dirty killer_info): the upper 16 bits are masked off before the callee sees it");
}

void test_aad_owner_filter_argument_is_dead() {
    // 0x0044c41a `MOV dword ptr [EBP+0x14],0x0` clobbers the owner_filter argument with zero before
    // any read, so the two `CMP owner_filter,0 / JZ` tests at 0x0044c50d and 0x0044c602 always take
    // the skip arm and the parameter's value cannot matter. Run the SAME scenario twice with opposite
    // filter values and require identical results.
    double         damage_seen[2] = {0.0, 0.0};
    uint32_t       calls_seen[2]  = {0u, 0u};
    const uint32_t filters[2]     = {0u, 0xffffffffu};

    for (int i = 0; i < 2; ++i) {
        sim_fixture f;
        g_kc.reset();
        seed_aad_cfg(f);
        // The tile's owner nibble matches the killer's, which is precisely the pair the dead filter
        // WOULD have suppressed had it been live.
        unit &u = place_unit_on_tile(f, 20, 20, 11);

        live_regions_on_fixture redirect(f); // crash-proofing, not a need of this case -- see the file banner
        sim_view                v   = f.view();
        sim_store               own = f.store();
        detail::apply_area_damage(v, own, rec_kc_calls(), 20, 20, 2, 12.0, 2, filters[i], 0x0002u,
                                  KILLER_I);

        damage_seen[i] = u.pending_damage;
        calls_seen[i]  = (uint32_t)g_kc.reg.size();
    }

    ck_eq_d(damage_seen[0], 18.0, "aad(filter 0): centre takes 12/1 + 12/2 = 18.0");
    ck_eq_d(damage_seen[1], damage_seen[0],
            "aad: owner_filter_zeroed is DEAD (clobbered to 0 at 0x0044c41a) -- 0xffffffff produces "
            "the identical result, including for a same-owner target");
    ck_eq(calls_seen[1], calls_seen[0], "aad: ... and the identical number of kill-credit calls");
}

} // namespace

void run_weapon_damage_tests() {
    test_wp_argument_passthrough_and_ratio();
    test_wp_indexes_the_named_weapon_row();
    test_wp_dist_sq_is_a_32_bit_wrapping_sum();
    test_wp_negative_deltas_square_positive();
    test_wp_length_is_a_signed_divisor();

    test_uk_scales_damage_by_the_victims_own_armor_row();
    test_uk_just_died_transition_credits_the_planet_total();
    test_uk_already_dead_before_this_damage_is_not_a_transition();
    test_uk_survivor_is_neither_a_kill_nor_destroyed();
    test_uk_dead_killer_still_receives_the_credit();
    test_uk_self_kill_gate_blocks_experience_but_not_the_planet_total();
    test_uk_soldier_bearing_victim_skips_the_feedback_block();
    test_uk_local_player_feedback_pans_sounds_and_notifies();
    test_uk_race2_shifts_the_sound_id();
    test_uk_sim_inactive_skips_only_the_sound();
    test_uk_non_local_player_gets_no_feedback_but_still_stamps_the_clock();

    test_bk_main_base_type_scales_the_damage();
    test_bk_alien_main_base_type_also_scales();
    test_bk_other_type_is_not_scaled();
    test_bk_just_died_transition_credits_the_building_total();
    test_bk_already_dead_before_this_damage_is_not_a_transition();
    test_bk_dead_killer_still_receives_the_credit();
    test_bk_self_kill_gate_blocks_experience_but_not_the_planet_total();
    test_bk_mother_type_local_feedback();
    test_bk_mother_cooldown_not_elapsed_still_stamps_last_tick_time();
    test_bk_non_mother_uses_the_other_cooldown_and_plays_no_sound();
    test_bk_non_mother_cooldown_not_elapsed_skips_the_notify();

    test_aad_ring_boundary_over_a_hand_built_arrangement();
    test_aad_ring_count_zero_visits_nothing();
    test_aad_masks_wrap_the_block_at_the_map_edge();
    test_aad_building_class_dispatches_to_bldg_kill_credit();
    test_aad_unit_class_reads_the_building_field_not_the_unit_field();
    test_aad_class_nibbles_outside_0x40_and_0x80_are_ignored();
    test_aad_zero_building_field_skips_the_building_arm();
    test_aad_unit_stack_chain_walks_every_link();
    test_aad_target_kind_other_touches_nothing();
    test_aad_target_kind_1_does_not_run_the_unit_stack_arm();
    test_aad_killer_info_is_masked_to_16_bits();
    test_aad_owner_filter_argument_is_dead();
}

} // namespace mh::sim::test
