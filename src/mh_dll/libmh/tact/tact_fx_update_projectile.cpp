//
// tact/tact_fx_update_projectile.cpp -- see tact_fx_update_projectile.h. Translated from the
// DISASSEMBLY (tmp/decomp_tact/llm_tact_fx_update_projectile_00431654.asm), not from the Ghidra `.c`
// draft, which silently omits two dead stores (corroborating, not the source of that finding -- see
// the header banner) and reconstructs one comparison operand as a fresh expression instead of the
// local the assembly actually stored it in (value-identical, not a divergence -- see the code
// comment at `candidate_owner`).
//
#include "tact/tact_fx_update_projectile.h"

#include "addr/mh_calls.gen.h" // frontier callees, indirected through the _calls struct below
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_facing_to_delta.h"
#include "tact/tact_unit_enqueue_command.h"
#include "tact/tact_unit_set_anim_state.h"
#include "state/rebind_targets.gen.h"
#include "fp/x87.h"         // CRT-X87: the shared x87 truncation helpers
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const fx_update_projectile_calls &live_fx_update_projectile_calls() {
    static const fx_update_projectile_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_LIBMH_BIND(llm_tact_fx_splash_damage),
        MH_LIBMH_BIND(llm_tact_fx_spawn),
        // KEPT ORIGINAL: was pinned here because the committed shape disagreed with our
        // `int32_t *` out-pointers (sig_<fn> said `void *`, a generator artifact). TACT1-P C6
        // (2026-09-04) fixed the generator so the boundary now carries the committed pointee and
        // the shapes agree -- see libmh_rebind_targets.json. The binding itself is left mh::call::
        // here (a rebind-arming decision, not a type fix).
        // Bound since TACT1-P C6 (2026-09-04): the row was pinned to the original while sig_<fn>
        // spelled its out-params `void *` -- the generator blunting the committed `int *`, not the
        // ABI. See libmh_rebind_targets.json.
        MH_LIBMH_BIND(llm_tact_facing_to_delta),
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state),
        mh::state::evt::inv_tact_unit_slot,
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command),
        MH_CRT(llm_sqrt),
    };
    return c;
}

namespace {

// ---- utils_math_trunc @0x004d0596, reproduced as inline asm (ST0 in/out, x87-only) ---------------
// MH_UNAVAILABLE__parameter_storage_not_marshallable in addr/mh_calls.gen.h -- cannot be called
// through mh::call::, so it is reproduced the same way every other translated TU with this callee
// does (see e.g. sim/sim_projectile_tick.cpp, tact/tact_calc_dir24.cpp): one hand-written __asm block
// per call SHAPE, so the whole FPU chain from the first FLD through the final FISTP stays on the
// 80-bit x87 stack with no round-trip through a 64-bit `double` local, exactly like the assembly.
// Five shapes are needed here (add/sub/mul/div/trunc-only); this function uses no compound
// (mul-then-div or div-then-mul) shape, unlike sim_projectile_tick's.

// trunc(a + b) -- moved_x/moved_y (0x4316df-f0, 0x43170f-20).
int32_t trunc_add(double a, double b) {
    return ::mh::fp::trunc_add(a, b);
}

// trunc(a - b) -- pullback_x/pullback_y (0x431819-32, 0x431835-4e).
int32_t trunc_sub(double a, double b) {
    return ::mh::fp::trunc_sub(a, b);
}

// trunc(a * b) -- the two fine->tile-column probes (0x431772-87, 0x431ba6-cb).
int32_t trunc_mul(double a, double b) {
    return ::mh::fp::trunc_mul(a, b);
}

// trunc(a / b) -- the two fine->tile-row probes (0x43178a-9f, 0x431bd1-f6).
int32_t trunc_div(double a, double b) {
    return ::mh::fp::trunc_div(a, b);
}

// trunc(x) -- the range_max check's sqrt result (0x431c3e-43).
int32_t trunc_only(double x) {
    return ::mh::fp::trunc_i32(x);
}

// ---- the four fine<->tile scale constants (DECLARED NEED -- see header banner item 4) ------------
// Not yet named `mh::addr::` symbols; used here as literals with the Ghidra name/address cited.
// Two logically-identical pairs at DISTINCT addresses (the compiler did not dedupe the two source
// occurrences) -- kept as separate named constants to mirror that distinction, not collapsed to one.
inline constexpr double kFineToTileColScale1   = 0.03125; // _G_LLM_CONST_DBL_1DIV32_500484 @0x00500484
inline constexpr double kFineToTileRowDivisor1 = 24.0;    // _G_LLM_CONST_DBL_24_50048C @0x0050048c
inline constexpr double kFineToTileColScale2   = 0.03125; // _G_LLM_CONST_DBL_1DIV32_500494 @0x00500494
inline constexpr double kFineToTileRowDivisor2 = 24.0;    // _G_LLM_CONST_DBL_24_50049C @0x0050049c

} // namespace

namespace detail {

void fx_update_projectile(const tact_view &tv, tact_store &own, int32_t fx_index,
                          const fx_update_projectile_calls &c) {
    fx_entry &fx = own.fx_at(fx_index); // _G_LLM_TACT_FX_POOL[fx_index] (DECLARED NEED, see header)

    // @0x431666-7d: cache the slot's fx-type id ONCE for the whole call -- never re-read from
    // fx.fx_type below, even on the path that writes fx.fx_type = 0 without returning (see the
    // pure-animation branch's frame-overflow kill further down).
    const int32_t fx_type_id = fx.fx_type;
    if (fx_type_id == 0) return; // @0x431681

    for (;;) {
        // @0x431687-a9: catch-up gate. `next_move_clock` is read via a SINGLE time_get_current_time()
        // call per iteration and reused for both the comparison and the store below -- the original
        // recomputes the identical sum a second time with no intervening call (0x4316af-c7); folding
        // it is behaviorally identical, not a collapse of the substep loop itself.
        const fx_type &kind            = tv.fx_type_table[fx_type_id];
        const double   next_move_clock = fx.move_clock + kind.speed;
        if (c.time_get_current_time() <= next_move_clock) return;
        fx.move_clock = next_move_clock; // @0x4316af-c7

        if (fx_type_id < 0x20) {
            // ==== moving projectile substep (@0x4316d7-0x431ce9) ====
            const int32_t moved_x = trunc_add(fx.pos_x, fx.vel_x); // @0x4316df-f0
            // @0x4316f3-04: SAR/SHL/SBB/SAR-by-5 idiom, verified == C's truncating `/32` (see header
            // banner) -- NOT floor division.
            const int32_t candidate_x = moved_x / 32;
            const int32_t moved_y     = trunc_add(fx.pos_y, fx.vel_y); // @0x43170f-20
            const int32_t candidate_y = moved_y / 24;                  // @0x431723-33, native IDIV

            // @0x431736-6d: out of bounds -- kill outright, no splash/spawn.
            if (candidate_x >= *tv.grid_width || candidate_y >= *tv.grid_height || candidate_x < 0 ||
                candidate_y < 0) {
                fx.fx_type = 0;           // @0x43175c-60
                own.fx_live_count() -= 1; // @0x431767 (DECLARED NEED, see header)
                return;                   // @0x43176d
            }

            // @0x431772-9f: a SECOND, independent fine->tile probe pair -- NOT bounds-checked against
            // candidate_x/candidate_y and not required to equal them (vel_x/vel_y-sized drift between
            // "position after this step" and "position now" is possible and preserved, not clamped).
            const int32_t flag_probe_x = trunc_mul(fx.pos_x, kFineToTileColScale1);   // @0x431772-87
            const int32_t flag_probe_y = trunc_div(fx.pos_y, kFineToTileRowDivisor1); // @0x43178a-9f
            // @0x4317a2-b9: `fx.altitude / 24` computed here (native IDIV, same shape as candidate_y
            // above) but DEAD -- see header banner. Not reproduced.

            // @0x4317ca-e8: either half-tile probe carries the 0x20 bit in flags[1].
            const bool terrain_flag_hit =
                (tile_at(tv, candidate_x, flag_probe_y).flags[1] & 0x20) != 0 ||
                (tile_at(tv, flag_probe_x, candidate_y).flags[1] & 0x20) != 0;

            if (terrain_flag_hit && passable_at(tv, candidate_x, candidate_y) == 0 &&
                tile_at(tv, candidate_x, candidate_y).building == 0) {
                // @0x431819-4: terrain impact. Pull back to the PRE-step position (pos - vel, a fresh
                // trunc pair -- distinct from moved_x/moved_y above, which this branch does NOT
                // overwrite), kill the slot, optional splash damage, spawn the terrain-collision fx,
                // return.
                const int32_t pullback_x      = trunc_sub(fx.pos_x, fx.vel_x); // @0x431819-32
                const int32_t pullback_y      = trunc_sub(fx.pos_y, fx.vel_y); // @0x431835-4e
                const int32_t pullback_tile_x = pullback_x / 32;               // @0x431851-62
                const int32_t pullback_tile_y = pullback_y / 24;               // @0x431865-75

                fx.fx_type = 0;           // @0x43187c
                own.fx_live_count() -= 1; // @0x431883

                if (kind.range_kill != 0) {
                    // @0x431889-b1
                    c.fx_splash_damage(pullback_tile_x, pullback_tile_y, kind.range_kill, kind.power);
                }
                // @0x4318b6-e6: x/y AND x2/y2 both = the pullback position (duplicated, matching the
                // asm's register+stack argument layout: EBX/ECX carry them once, the stack pushes
                // carry them again).
                c.fx_spawn(kind.colision1, fx.owner, pullback_x, pullback_y, pullback_x, pullback_y,
                           fx.altitude);
                return; // @0x4318eb
            }

            // ==== building/unit collision check at the candidate tile (@0x4318f0-0x431b39) ====
            const uint16_t building_id = tile_at(tv, candidate_x, candidate_y).building; // @0x4318f0-fe
            // @0x431901-12: the original stores this owner byte to its own stack slot and compares
            // against IT at 0x431983 -- the Ghidra `.c` draft instead re-derives
            // `UNITS[tile_objects[...].building].owner` as a fresh expression at the comparison site.
            // Both read the identical value (no write to tile_objects/UNITS happens in between), so
            // this is a rendering difference, not a behavioral one; using the one clean local matches
            // the assembly's actual data flow.
            const uint8_t candidate_owner = unit_of(tv, building_id).owner;
            // @0x431915-1c: a dead CMP in the original (flags set, never consumed by any branch) --
            // no observable effect, not reproduced.
            // @0x43191d-35: `building_id` re-read here in the original (fresh IMUL+MOVZX of the
            // identical `.building` field, no intervening write) -- one read serves both uses.

            if (building_id != 0 &&
                (fx.altitude < 0x1e ||
                 (unit_of(tv, building_id).anim_state != 2 &&
                  unit_of(tv, building_id).anim_state != 3)) &&
                fx.owner != candidate_owner && unit_of(tv, building_id).anim_state != 0x1f) {
                // @0x43199d-ab: `UNITS[building_id].type` read into a local here in the original, but
                // DEAD -- see header banner. Not reproduced.

                // @0x4319ae-de: x/y AND x2/y2 both = moved_x/moved_y (the post-step position from the
                // top of this substep -- this branch never overwrote that stack slot, unlike the
                // terrain-hit branch above).
                c.fx_spawn(kind.colision2, fx.owner, moved_x, moved_y, moved_x, moved_y, fx.altitude);
                fx.fx_type = 0;           // @0x4319e7
                own.fx_live_count() -= 1; // @0x4319ee

                if (building_id != 0) { // @0x4319f4-f8: redundant (already proven nonzero above), preserved literally
                    tact_unit &bld = own.unit_at(building_id);
                    if (bld.hp <= kind.power) {
                        // @0x431a1f-b5: the hit kills the building.
                        if (bld.anim_state < 2 && bld.progress != 0) {
                            // @0x431a41-7b: knock a passability opening in the building's facing
                            // direction. ORIGINAL callee, not the sibling translation
                            // mh::tact::facing_to_delta (bit-independence).
                            int32_t dx = 0, dy = 0;
                            c.facing_to_delta(bld.facing_dir, &dx, &dy);
                            own.planes().passable_at(candidate_x + dx, candidate_y + dy) =
                                mh::state::PASSABLE_DEFAULT; // @0x431a6b
                            own.planes().passable_at(candidate_x, candidate_y) =
                                mh::state::PASSABLE_BLOCKED; // @0x431a7b
                        }
                        bld.hp       = 0;                         // @0x431a89
                        bld.progress = 0;                         // @0x431a99
                        c.unit_set_anim_state(building_id, 0x1f); // @0x431aa0-ad
                        c.unit_refresh_ui_slot(building_id);      // @0x431aad-b0
                        return;                                   // @0x431ab5
                    }
                    // @0x431aba-d4: damage only, no kill.
                    bld.hp = static_cast<uint16_t>(bld.hp - kind.power);
                    c.unit_refresh_ui_slot(building_id); // @0x431ad4-d7
                    if (bld.def_stat == 0) return;       // @0x431adc-ea

                    // @0x431af0-b34: the counter-turn-order dir-wrap (dir24 range is 1..24 -- see
                    // mh_llm_tact_fx.dir24's field comment).
                    int32_t wrapped_dir = static_cast<int32_t>(fx.dir24) - 0xb;      // @0x431af0-fe
                    if (wrapped_dir < 1) wrapped_dir += 0x18;                        // @0x431b01-07
                    if (bld.facing_dir == wrapped_dir) return;                       // @0x431b0b-1c
                    c.unit_enqueue_command(building_id, 6, 1, wrapped_dir, 0, 0, 0); // @0x431b1e-34
                }
                return; // @0x431b39
            }

            // ==== normal move + frame update (@0x431b3e-0x431ce9) -- reached whenever no unit-hit
            // was processed above (no building at the candidate tile, or the hit was disqualified) ====
            fx.pos_x += fx.vel_x;     // @0x431b3e-52
            fx.pos_y += fx.vel_y;     // @0x431b58-6c
            fx.travel_dx += fx.vel_x; // @0x431b72-86
            fx.travel_dy += fx.vel_y; // @0x431b8c-a0

            fx.tile_col = static_cast<uint8_t>(trunc_mul(fx.pos_x, kFineToTileColScale2));   // @0x431ba6-cb
            fx.tile_row = static_cast<uint8_t>(trunc_div(fx.pos_y, kFineToTileRowDivisor2)); // @0x431bd1-f6

            if (kind.range_max > 0) {
                // @0x431c09-53: kill (fx_type = 0 ONLY -- no live-count decrement, an asymmetry
                // against every other kill site in this function, preserved literally -- see header
                // banner) once accumulated travel distance reaches range_max.
                const double  dist_sq = fx.travel_dx * fx.travel_dx + fx.travel_dy * fx.travel_dy;
                const double  dist    = c.sqrt_fn(dist_sq);
                const int32_t range   = trunc_only(dist);
                if (kind.range_max <= range) {
                    fx.fx_type = 0; // @0x431c59
                    return;         // @0x431c60
                }
            }

            fx.frame_counter += 1; // @0x431c65-69
            if (static_cast<uint16_t>(fx.frame_counter) >= static_cast<uint16_t>(kind.frames)) {
                fx.frame_counter = 0; // @0x431c88-8c
            }
            if (kind.direct == 1) {
                // @0x431ca2-cd
                fx.sprite_frame =
                    static_cast<uint16_t>((fx.dir24 - 1) * kind.frames + fx.frame_counter);
            } else {
                fx.sprite_frame = static_cast<uint16_t>(fx.frame_counter); // @0x431cd3-e2
            }
        } else {
            // ==== pure animation effect substep (@0x431cf3-0x431d7e) ====
            fx.frame_counter += 1; // @0x431cf3-f7
            if (static_cast<uint16_t>(fx.frame_counter) < static_cast<uint16_t>(kind.frames)) {
                if (kind.direct == 1) {
                    // @0x431d36-5f: NOTE dir24 ALONE, NOT dir24-1 -- asymmetric vs. the moving-fx
                    // branch above (0x431ca2-cd), preserved literally.
                    fx.sprite_frame = static_cast<uint16_t>(kind.frames * fx.dir24 + fx.frame_counter);
                } else {
                    fx.sprite_frame = static_cast<uint16_t>(fx.frame_counter); // @0x431d68-77
                }
            } else {
                // @0x431d16-21: kill the slot but DO NOT RETURN -- the substep loop continues, still
                // driven by the fx_type_id cached before the loop (fx.fx_type is never re-read).
                // Matches `JMP 0x431d7e` (loop back), NOT `JMP 0x431d83` (function return) -- see
                // header banner.
                fx.fx_type = 0;           // @0x431d1a
                own.fx_live_count() -= 1; // @0x431d21
            }
        }
        // @0x431d7e: loop back to the top regardless of which branch ran.
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void fx_update_projectile(int32_t fx_index) {
    tact_state st = state();
    detail::fx_update_projectile(st.read, st.own, fx_index, live_fx_update_projectile_calls());
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
