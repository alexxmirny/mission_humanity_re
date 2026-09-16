//
// sim/sim_bldg_turret_combat.cpp -- see sim_bldg_turret_combat.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_turret_acquire_target_0047baf9.asm,
// tmp/decomp_sim/llm_strat_turret_fire_0047bf8e.asm), cross-checked field-by-field against the
// header's own FIELDS section and against sim_bldg_state_turret.h's already-committed caller-side
// derivation (which this translation agrees with throughout).
//
#include "sim/sim_bldg_turret_combat.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const turret_acquire_target_calls &live_turret_acquire_target_calls() {
    static const turret_acquire_target_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
    };
    return c;
}

const turret_fire_calls &live_turret_fire_calls() {
    static const turret_fire_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_get_sprite_anchor_coord),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_fx_anim_dir_frame_stride),
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
        MH_LIBMH_BIND(llm_strat_weapon_scatter_offset),
        MH_LIBMH_BIND(llm_strat_projectile_spawn),
        mh::state::evt::snd_play_at,
        MH_LIBMH_BIND(llm_strat_apply_area_damage),
    };
    return c;
}

namespace {

// tile_object::unit / unit::unit_above are both `uint8_t[2]` in the generated header (map::t::
// unit_full_id rendered as a raw byte pair, not a scalar) -- reassembles the little-endian word a
// single `MOVZX reg, word ptr [...]` reads in the original. High nibble = owning player, low 12 bits
// = roster index. Same idiom sim_combat_kill_credit.cpp's own `unit_full_id_word()` documents;
// re-derived locally here per this project's per-TU convention (not a new shared cross-TU helper).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}

// The circular forward-distance over the 24-way heading domain, shared by both candidate arms of
// turret_acquire_target (0x0047bd50-0x0047bd89 / 0x0047bf05-0x0047bf3e, byte-identical loops) --
// factored ONCE within this TU (a local, non-exported function, not a new cross-TU shared helper).
int32_t heading_diff(int32_t raw_dir, int32_t aim_heading) {
    int32_t cur     = raw_dir;
    int32_t counter = 0;
    while (cur != aim_heading) {
        cur = cur % 24 + 1;
        ++counter;
    }
    if (counter > 12) counter = 24 - counter;
    return counter;
}

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only). This function's one call site (0x0047c3c2)
// is an ORDINARY call to it, not compiler-inlined -- reproduced as this TU's own copy of the
// established sim_bldg_find_mothership_position.cpp / sim_bldg_mother_reelect_primary.cpp precedent's
// exact instruction sequence, per the per-TU convention (not a new shared helper). FISTP width
// confirmed 32-bit AT THIS SITE (opcode bytes `db 5d b0` @0x0047c3c7 -> 0xDB ModRM 0x5D, reg field 3,
// i.e. 0xDB /3 == FISTP m32int).
int32_t trunc_to_int32(double val) {
    return ::mh::fp::trunc_i32(val);
}

} // namespace

namespace detail {

// ---- llm_strat_turret_acquire_target @0x0047baf9 ---------------------------------------------------

uint32_t turret_acquire_target(const sim_view &v, sim_store &own, const turret_acquire_target_calls &c,
                               uint32_t player, int32_t building_index, uint32_t *out_1,
                               uint32_t *out_2) {
    const uint16_t player_u16 = static_cast<uint16_t>(player);

    // 0x0047bb1a-0x0047bb27: the turret's OWN building fine coords -- reused unchanged as the "self"
    // point for every heading-diff computation below.
    int32_t self_fine_x = 0;
    int32_t self_fine_y = 0;
    c.bldg_get_coords(player_u16, building_index, &self_fine_x, &self_fine_y);

    // 0x0047bb2c-0x0047bb51: truncating signed divide-by-32 (fine pixel -> tile), the SAME idiom
    // every sibling in this migration set uses for this exact conversion.
    const int32_t base_col = self_fine_x / 32;
    const int32_t base_row = self_fine_y / 32;

    // 0x0047bb54: sentinel meaning "no candidate found yet" -- the scan's max possible heading_diff
    // is 12 (24-way domain), so 30 can never be reached by a real candidate.
    constexpr int32_t NO_CANDIDATE = 0x1e;
    int32_t           best_diff    = NO_CANDIDATE;

    // 0x0047bb5b-0x0047bb8a: turrets[player][buildings[player][building_index].sub_id]'s own
    // aim_heading/attack_range -- read-only (v.turrets, the SAME row-major indexing
    // sim_store::turret_at() writes it with). CORRECTED 2026-08-22 (reimpl-verify): the original
    // truncates `player` to its low 16 bits (MOVZX word) at every use site in this function with no
    // exception, including this one -- use `player_u16`, not the raw 32-bit parameter.
    const uint8_t sub_id = building_of(v, player_u16, building_index).sub_id;
    const turret &self_turret =
        v.turrets[player_u16 * static_cast<uint32_t>(v.caps.turrets) + sub_id];
    const int32_t aim_heading = self_turret.aim_heading;
    const int32_t range       = self_turret.attack_range;

    // Shared best-diff update -- a local lambda (not a cross-TU helper) used by both candidate arms.
    auto consider_candidate = [&](int32_t target_fine_x, int32_t target_fine_y, uint32_t ref,
                                  uint32_t index) {
        const int32_t raw_dir = c.dir_from_to(self_fine_x, self_fine_y, target_fine_x, target_fine_y);
        const int32_t diff    = heading_diff(raw_dir, aim_heading);
        if (diff < best_diff) {
            best_diff = diff;
            *out_1    = ref;
            *out_2    = index;
        }
    };

    // 0x0047bbc8-0x0047bf6e: the diamond sweep. Both loop tops re-test `best_diff == 0` (an EXACT
    // heading match short-circuits the whole remaining scan) in addition to the range bound --
    // reproduced as the for-loops' own continuation conditions since the original re-tests
    // identically at both loop tops on every pass.
    for (int32_t dx = -range; dx <= range && best_diff != 0; ++dx) {
        for (int32_t dy = -range; dy <= range && best_diff != 0; ++dy) {
            const uint32_t     tile_x = static_cast<uint32_t>(base_col + dx) & map_width_mask(v);
            const uint32_t     tile_y = static_cast<uint32_t>(base_row + dy) & map_height_mask(v);
            const tile_object &t =
                tile_at(v, static_cast<int32_t>(tile_x), static_cast<int32_t>(tile_y));

            // 0x0047bc3d-0x0047bc71 (building arm) / 0x0047bde6-0x0047be55 (unit arm): the manhattan-
            // distance check the original re-derives independently in each arm (including one
            // PROVABLY-always-true redundant re-test in the unit arm at 0x0047be53, `TEST EAX,EAX;
            // JGE` -- tautological since EAX is a sum of two non-negative absolute values). Computed
            // ONCE here: pure arithmetic, no outward call, so folding it changes nothing observable
            // (unlike turret_fire's genuinely-duplicated get_sprite_anchor_coord calls below, which
            // stay unfolded because those ARE real outward calls).
            const int32_t manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

            // ---- the BUILDING candidate (0x0047bc33-0x0047bdb0) --------------------------------
            if (t.building != 0) {
                if (manhattan > range) continue;                                    // 0x0047bc74: out of range, skip the WHOLE cell
                const uint32_t owner = static_cast<uint32_t>(t.class_owner) & 0xfu; // 0x0047bc9e
                // 0x0047bcaa-0x0047bcc0: not-enemy (or own building) -- skip the whole cell. `2` is
                // an established magic-literal relation code, not a named enum -- same posture
                // sim_diplomacy_set_relation.cpp's own identical `relation == 2` literal carries.
                // CORRECTED 2026-08-22 (reimpl-verify): player_u16, matching the original's
                // unconditional 16-bit truncation at 0x0047bcaa/0x0047bcb3.
                if (!(player_u16 != owner && own.player_relation_at(player_u16, owner) == 2)) continue;
                const uint8_t class_owner_full = t.class_owner; // 0x0047bce9, re-read the full byte
                if ((class_owner_full & 0x10u) != 0) continue;  // 0x0047bcfa: decoration class, skip

                int32_t target_fine_x = 0, target_fine_y = 0;
                if ((class_owner_full & 0x80u) == 0) {
                    // 0x0047bd1d: bit 0x80 clear -> the tile's `.building` slot is a BUILDING index.
                    c.bldg_get_coords(static_cast<uint16_t>(owner), t.building, &target_fine_x,
                                      &target_fine_y);
                } else {
                    // 0x0047bd09: bit 0x80 set -> the SAME `.building` slot is read as a UNIT index
                    // (the raw asm reuses EDX=[EBP-0x18], the exact slot the building id was loaded
                    // into, on this branch).
                    c.unit_get_coords(static_cast<uint16_t>(owner), t.building, &target_fine_x,
                                      &target_fine_y);
                }
                // 0x0047bda0/0x0047bdae: *out_1 = class_owner_full | owner -- a NO-OP OR (`owner` is
                // already class_owner_full's low nibble), kept literal rather than simplified away,
                // per the project's established "don't collapse a provably-equivalent original
                // expression" convention (e.g. sim_combat_kill_credit.cpp's JC/JBE/JZ cascade note).
                consider_candidate(target_fine_x, target_fine_y,
                                   static_cast<uint32_t>(class_owner_full) | owner, t.building);
                // FALLS THROUGH (no `continue`) into the unit check below for the SAME tile -- no JMP
                // exists between 0x0047bdae and 0x0047bdb0 in the raw disassembly (verified, not a
                // decompiler artifact). A tile can contribute BOTH a building and a unit candidate.
            }

            // ---- the UNIT candidate / chain walk (0x0047bdb0-0x0047bf64) ------------------------
            // Reached when `.building == 0` OR the building arm above fell all the way through
            // (every early `continue` above skips this section for the current cell entirely).
            uint16_t chain = unit_full_id_word(t.unit);    // 0x0047bdd2
            if (chain == 0 || manhattan > range) continue; // 0x0047bde0 / 0x0047be1d

            uint32_t chain_owner = (static_cast<uint32_t>(chain) >> 12) & 0xfu; // 0x0047be5f-0x0047be67
            bool     found_enemy = false;
            for (;;) {
                // CORRECTED 2026-08-22 (reimpl-verify): player_u16, matching the original's
                // unconditional 16-bit truncation at 0x0047be6e/0x0047be77.
                if (player_u16 != chain_owner && own.player_relation_at(player_u16, chain_owner) == 2) {
                    found_enemy = true; // 0x0047be7d-0x0047be84
                    break;
                }
                // 0x0047be86-0x0047bebb: advance to the next unit in the stack (`.unit_above`).
                const uint32_t idx = static_cast<uint32_t>(chain) & 0xfffu;
                chain              = unit_full_id_word(unit_of(v, chain_owner, static_cast<int32_t>(idx)).unit_above);
                if (chain == 0) break; // chain exhausted -- no enemy anywhere in the stack
                chain_owner = (static_cast<uint32_t>(chain) >> 12) & 0xfu;
            }
            if (!found_enemy) {
                // 0x0047bec1: `JZ 0x0047bf69` -> 0x0047bbdb -- ABANDONS THE REST OF THIS `dy` ROW
                // (advances the OUTER `dx` loop directly), not merely this cell. Verified
                // byte-for-byte from the raw disassembly's own label targets, not inferred -- see the
                // header banner's flagged uncertainty; almost certainly an original quirk rather than
                // a translation choice, worth extra scrutiny.
                break;
            }

            // 0x0047beca: candidate resolved -- chain now holds the enemy unit's own packed full-id.
            const uint32_t candidate_index = static_cast<uint32_t>(chain) & 0xfffu;
            int32_t        target_fine_x = 0, target_fine_y = 0;
            c.unit_get_coords(static_cast<uint16_t>(chain_owner), static_cast<int32_t>(candidate_index),
                              &target_fine_x, &target_fine_y); // 0x0047bed2-0x0047bedf
            consider_candidate(target_fine_x, target_fine_y, chain_owner | 0x80u, candidate_index);
        }
    }

    // 0x0047bf6e-0x0047bf84: `return best_diff != 0x1e;` -- *out_1/*out_2 are left UNTOUCHED when
    // this returns 0 (no candidate ever beat the sentinel).
    return best_diff != NO_CANDIDATE ? 1u : 0u;
}

// ---- llm_strat_turret_fire @0x0047bf8e --------------------------------------------------------------

void turret_fire(const sim_view &v, sim_store &own, const turret_fire_calls &c, uint32_t player,
                 uint32_t bldg_index, int32_t target_fine_x, int32_t target_fine_y,
                 int32_t target_elevation, uint32_t /*last_tick_time_lo*/,
                 uint32_t /*last_tick_time_hi*/, uint8_t fire_kind) {
    const uint16_t player_u16 = static_cast<uint16_t>(player);
    const int32_t  bldg_idx   = static_cast<int32_t>(bldg_index);

    // 0x0047bfb6-0x0047bfd0: buildings[player][bldg_index].sub_id -- read-only.
    const uint8_t     sub_id = building_of(v, player, bldg_idx).sub_id;
    turret           &t      = own.turret_at(player, sub_id);
    const cfg_weapon &w      = v.cfg_weapons[t.weapon_id];

    // 0x0047bfed-0x0047c017: the target-bit gate. fire_kind (1 or 2, per
    // sim_bldg_state_turret.h's already-committed caller-side derivation) reuses sim_state.h's
    // existing WEAPON_TARGET_GROUND/WEAPON_TARGET_AIR constants (rule 17a) rather than the literal
    // 1/2. A pure no-op (immediate return) if the required bit is clear.
    const uint8_t required_bit = (fire_kind == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;
    if ((w.target & required_bit) == 0) return;

    // 0x0047c01d-0x0047c0f8: ammo/resupply/reload-timer bookkeeping, unconditional once the gate
    // above passes.
    t.ammo -= 1;
    if (t.ammo != 0) {
        t.reload_timer = w.short_time; // 0x0047c063-0x0047c069
    } else {
        if (t.resupply_cycles_remaining != -1) t.resupply_cycles_remaining -= 1; // 0x0047c084-0x0047c09d
        if (t.resupply_cycles_remaining == 0) {
            t.resupply_available = 0; // 0x0047c0e1-0x0047c0f1
        } else {
            t.reload_timer = w.long_time; // 0x0047c0d3-0x0047c0d9
        }
    }
    // 0x0047c11a-0x0047c12a: unconditional once the two branches above converge.
    t.reload_ready_flag = 0;

    // 0x0047c131-0x0047c154: switch(Weapon[weapon_id].type), values 1..8 (0 or >8 falls to the no-op
    // default). The case-VALUE -> physical-target mapping below was NOT read directly from the raw
    // 32-byte jump table (no tool available to dump it from this exported .asm) -- it was
    // cross-derived from TWO independent pieces of evidence that agree: the Ghidra .c draft's own
    // `switch` case labels, and the raw disassembly's PHYSICAL fallthrough adjacency (no JMP between
    // caseD_4's last instruction and caseD_3's first at 0x0047c1e6; none between caseD_2's last and
    // caseD_1's first at 0x0047c4d8). Flagged in uncertainties as an indirect derivation.
    switch (w.type) {
        case 2:
        case 6: {
            // 0x0047c41d-0x0047c4d3: ONE projectile at anchor-slot 2. CORRECTED 2026-08-22: the two
            // calls are NOT redundant -- llm_strat_bldg_get_sprite_anchor_coord has a 4th param (CL,
            // axis) that was missing from the committed Ghidra prototype until fixed it
            // (see the session's ghidra_findings.json entry). The asm at
            // 0x0047c4c7 passes axis=0 then axis=1 (Y then X, per the callee's own plate); the
            // downstream projectile_spawn(a2_2, a2_1, ...) call already reads them in (X, Y) order,
            // so only the axis literal needed fixing here.
            const uint32_t a2_1 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 2, 0);
            const uint32_t a2_2 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 2, 1);
            c.projectile_spawn(static_cast<int32_t>(a2_2), static_cast<int32_t>(a2_1), 0,
                               static_cast<uint32_t>(target_fine_x),
                               static_cast<uint32_t>(target_fine_y), target_elevation, t.weapon_id,
                               fire_kind, t.counter_ref, t.counter_target_slot,
                               static_cast<uint32_t>(player) | 0x40u, bldg_idx);
            [[fallthrough]];
        }
        case 1:
        case 5:
        case 8: {
            // 0x0047c4d8-0x0047c58e: a SECOND, separate projectile at anchor-slot 1 -- this arm
            // always runs, whether entered directly (type 1/5/8) or via the {2,6} fallthrough above.
            // CORRECTED 2026-08-22: axis=0 then axis=1 (Y then X) at 0x0047c582, per the fixed
            // get_sprite_anchor_coord prototype -- see the case-{2,6} block's comment above.
            const uint32_t a1_1 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 0);
            const uint32_t a1_2 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 1);
            c.projectile_spawn(static_cast<int32_t>(a1_2), static_cast<int32_t>(a1_1), 0,
                               static_cast<uint32_t>(target_fine_x),
                               static_cast<uint32_t>(target_fine_y), target_elevation, t.weapon_id,
                               fire_kind, t.counter_ref, t.counter_target_slot,
                               static_cast<uint32_t>(player) | 0x40u, bldg_idx);
            // 0x0047c593-0x0047c601: SIM_ACTIVE-gated fire sound, TWO MORE (redundant) get_sprite_
            // anchor_coord calls feeding ONE offscreen_snd_volume call -- kept unfolded (real calls).
            if (*v.sim_active != 0) {
                // CORRECTED 2026-08-22: axis=0 then axis=1 (Y then X) at 0x0047c5aa/0x0047c5d1.
                const uint32_t sa1 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 0);
                const uint32_t sa2 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 1);
                // The original offscreen_snd_volume(0x0047c5ec)+snd_play pair, fused into ONE
                // position-carrying record (the LIFT-NOTIFY offscreen conversion): the hosted sink re-runs that
                // exact pair synchronously at emit.
                c.snd_play_at(w.sound_fire, static_cast<int32_t>(sa2) / 32,
                              static_cast<int32_t>(sa1) / 32);
            }
            break;
        }
        case 4: {
            // 0x0047c15b-0x0047c1e1: ONE fx-anim at anchor-slot 2. CORRECTED 2026-08-22: axis=1 then
            // axis=0 (X then Y) at 0x0047c16c/0x0047c182 -- see the case-{2,6} block's comment above.
            const uint32_t a2_1   = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 2, 1);
            const uint32_t a2_2   = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 2, 0);
            const int32_t  dir    = c.dir_from_to(static_cast<int32_t>(a2_1), static_cast<int32_t>(a2_2),
                                                  target_fine_x, target_fine_y);
            const int32_t  stride = c.fx_anim_dir_frame_stride(w.fite_explo);
            c.fx_anim_spawn(a2_1, a2_2, static_cast<uint32_t>(w.fite_explo + (dir - 1) * stride),
                            *v.game_clock, 1u);
            [[fallthrough]];
        }
        case 3: {
            // 0x0047c1e6-0x0047c418: scatter offset + a SECOND fx-anim + area damage, at anchor-slot
            // 1 -- this arm always runs, whether entered directly (type 3) or via the {4} fallthrough.
            // CORRECTED 2026-08-22: axis=1 then axis=0 (X then Y) at 0x0047c1f7/0x0047c20d.
            const uint32_t a1_1 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 1);
            const uint32_t a1_2 = c.bldg_get_sprite_anchor_coord(player, bldg_idx, 1, 0);
            const int32_t  dir  = c.dir_from_to(static_cast<int32_t>(a1_1), static_cast<int32_t>(a1_2),
                                                target_fine_x, target_fine_y);

            int32_t scatter_dx = 0, scatter_dy = 0;
            c.weapon_scatter_offset(0, w.missing[player], a1_1, a1_2,
                                    static_cast<uint32_t>(target_fine_x),
                                    static_cast<uint32_t>(target_fine_y), &scatter_dx, &scatter_dy);

            const int32_t stride = c.fx_anim_dir_frame_stride(w.fite_explo);
            c.fx_anim_spawn(a1_1, a1_2, static_cast<uint32_t>(w.fite_explo + stride * (dir - 1)),
                            *v.game_clock, 1u);

            // 0x0047c2a6-0x0047c2d9: pixel-space wrap via general.bw_mask/bh_mask -- NOT the
            // tile-space width_mask/height_mask pair map_width_mask()/map_height_mask() wrap (see the
            // header banner). impact_y_ground additionally subtracts target_elevation BEFORE masking.
            const uint32_t impact_x = v.geom->bw_mask & static_cast<uint32_t>(target_fine_x + scatter_dx);
            const uint32_t impact_y_full =
                v.geom->bh_mask & static_cast<uint32_t>(target_fine_y + scatter_dy);
            const uint32_t impact_y_ground = v.geom->bh_mask &
                                             static_cast<uint32_t>(target_fine_y + scatter_dy - target_elevation);

            c.fx_anim_spawn(impact_x, impact_y_ground, static_cast<uint32_t>(w.target_explo),
                            *v.game_clock, fire_kind);

            // 0x0047c305-0x0047c38b: SIM_ACTIVE-gated fire+target sounds. Each of the original's
            // TWO offscreen_snd_volume+snd_play pairs (own volume call apiece, identical pan from
            // (impact_x, impact_y_ground)) is ONE position-carrying record now (LIFT-NOTIFY slice
            // 3): the hosted sink re-runs each exact pair synchronously at emit -- still two
            // volume calls at the binary level, in the original order.
            if (*v.sim_active != 0) {
                c.snd_play_at(w.sound_fire, static_cast<int32_t>(impact_x) / 32,
                              static_cast<int32_t>(impact_y_ground) / 32);
                c.snd_play_at(w.sound_target, static_cast<int32_t>(impact_x) / 32,
                              static_cast<int32_t>(impact_y_ground) / 32);
            }

            // 0x0047c3c2-0x0047c413: apply_area_damage at (impact_x, impact_y_FULL) -- NOT
            // impact_y_ground; ring_count = TRUNC(fire_range[player]) via trunc_to_int32() (see the
            // header banner's FP note).
            c.apply_area_damage(static_cast<int32_t>(impact_x) / 32,
                                static_cast<int32_t>(impact_y_full) / 32, fire_kind, w.power[player],
                                trunc_to_int32(w.fire_range[player]), w.area_damage_owner_filter,
                                static_cast<uint32_t>(player) | 0x40u, bldg_idx);
            break;
        }
        default:
            break;
    }
}

} // namespace detail

// ---- the public wrappers ----------------------------------------------------------------------

uint32_t turret_acquire_target(uint32_t player, int32_t building_index, uint32_t *out_1, uint32_t *out_2) {
    sim_state st = state();
    return detail::turret_acquire_target(st.read, st.own, live_turret_acquire_target_calls(), player,
                                         building_index, out_1, out_2);
}

void turret_fire(uint32_t player, uint32_t bldg_index, int32_t param_3, int32_t param_4,
                 int32_t param_5, uint32_t param_6, uint32_t param_7, uint8_t param_8) {
    sim_state st = state();
    detail::turret_fire(st.read, st.own, live_turret_fire_calls(), player, bldg_index, param_3,
                        param_4, param_5, param_6, param_7, param_8);
}


} // namespace mh::sim
