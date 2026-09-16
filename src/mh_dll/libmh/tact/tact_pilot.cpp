//
// tact/tact_pilot.cpp -- see tact_pilot.h. Translated from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_pilot.h"


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetPrivateProfileIntA -- the [promote] gate, same as mh::sim's sites

namespace mh::tact {
namespace detail {

int32_t quantize_facing_dir(int32_t notch_span, int32_t facing_dir) {
    // SAR EDX,0x1f / SUB EAX,EDX / SAR EAX,0x1 @0x0042d0f2-0x0042d0f7, then ADD into facing_dir
    // @0x0042d0f9. That three-instruction sequence is the compiler's signed divide-by-two: add the
    // sign bit, then arithmetic-shift. It TRUNCATES TOWARD ZERO, so it is `/ 2` in C and NOT `>> 1`
    // -- a bare shift would floor, giving a different answer for a negative notch_span.
    int32_t centred = facing_dir + notch_span / 2;
    // CMP 0x18 / JLE @0x0042d0fc-0x0042d100: only values strictly ABOVE 24 fold back, and by
    // exactly 24. Nothing wraps a negative, and 24 itself is left alone.
    if (centred > 0x18) {
        centred = centred - 0x18;
    }
    // DEC EDX / MOV EAX,EDX / SAR EDX,0x1f / IDIV [notch_span] @0x0042d109-0x0042d10f. A real IDIV
    // here, unlike the halving above.
    return (centred - 1) / notch_span;
}

int32_t ui_mouse_in_rect(const tact_view &v, int32_t rect_x0, int32_t rect_y0, int32_t rect_x1,
                         int32_t rect_y1) {
    // The four compares in the original's order @0x0043592a-0x00435954, all SIGNED (JL/JGE): it
    // fails on `mx < x0` (JL @0x00435932), `my < y0` (JGE-to-continue @0x0043593c), `mx >= x1`
    // (JL-to-continue @0x00435948) and `my >= y1` (JL-to-continue @0x00435954). So the rectangle is
    // half-open, [x0,x1) x [y0,y1). Written as the original's early-out chain rather than one
    // conjunction so the low/high asymmetry stays visible.
    const int32_t mx = *v.sidebar_mouse_x;
    const int32_t my = *v.sidebar_mouse_y;
    if (mx < rect_x0 || my < rect_y0 || rect_x1 <= mx || rect_y1 <= my) {
        return 0;
    }
    return 1;
}

void units_reset_hp_for_active(tact_store &own) {
    // CMP [i],0x80 / JLE @0x0042ad9e-0x0042ada5 -- the loop runs while i <= 0x80, so slot 0 is
    // skipped and slot 0x80 IS included, over a 129-record array.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        tact_unit &u = own.unit_at(i);
        // CMP byte ptr [EAX + 0x8260d0],0x80 / JNC @0x0042adb8-0x0042adbf. JNC is CF==0, i.e. an
        // UNSIGNED >=, so the skip is "type >= 0x80" and the test here is "not marked despawned"
        // (llm_tact_unit_despawn marks by ADDING 0x80 to the type). Not a signed sign test.
        if (u.type < 0x80u) {
            // MOV word ptr [EAX + 0x82611b],0x0 @0x0042adc8 -- a WORD store at record + 0x4b, which
            // is `hp`. Zeroing it as uint16_t, not as a byte.
            u.hp = 0;
        }
    }
}

void move_path_preview_clear(mh::state::mode_planes &planes) {
    // Nested 0..0x7f loops @0x0042eaa9 (outer, column) and @0x0042eac3 (inner, row). The address is
    // built as SHL EDX,0xb / SHL EAX,0x3 / ADD @0x0042ead9-0x0042eae2 -- col*2048 + row*8 -- and the
    // store is `MOV byte ptr [EAX + 0xd1ec85],0x0` @0x0042eae4, i.e. base + 5. Record index is
    // therefore col*256 + row, which is exactly mode_planes' (x<<8)|y (the OR and the ADD agree
    // because row < 0x80 never touches the column bits), and byte 5 is tile_object::unit[1] -- the
    // overlay half of the two-readings pair. `tile_overlay()` is the only sanctioned way to name it.
    for (int32_t col = 0; col < TACT_MAP_DIM; ++col) {
        for (int32_t row = 0; row < TACT_MAP_DIM; ++row) {
            mh::state::tile_overlay(planes, col, row) = 0;
        }
    }
}

} // namespace detail

int32_t quantize_facing_dir(int32_t notch_span, int32_t facing_dir) {
    return detail::quantize_facing_dir(notch_span, facing_dir);
}

int32_t ui_mouse_in_rect(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1, int32_t rect_y1) {
    tact_state st = state();
    return detail::ui_mouse_in_rect(st.read, rect_x0, rect_y0, rect_x1, rect_y1);
}

void units_reset_hp_for_active() {
    tact_state st = state();
    detail::units_reset_hp_for_active(st.own);
}

void move_path_preview_clear() {
    tact_state st = state();
    detail::move_path_preview_clear(st.own.planes());
}


// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// This TU used to carry counter-wrapped `promoted_arm::` adapters and an MH_EXPORT_REPLACE install
// for each, so our bodies could take the game's entry points. The fork's config selector has two
// hosted answers, `original` and `brokered`, and TACTICAL MODE IS ORIGINAL IN BOTH: the reimplemented
// spine the fork ships is the strategic one. So the install surface has no configuration left to be
// armed in, and an installer nothing can arm is not a dormant feature, it is a claim about what runs
// that is false in every run.
//
// THE BODIES ABOVE ARE UNTOUCHED and stay reachable two ways: the offline oracle (net_selftest
// tacttest) drives them directly, and their rebind rows survive (fork ruling Q2 -- the BIND survives,
// only the per-row runtime gate died), so a standalone host binds them unconditionally. What is gone
// is only the route that overwrote the game's own entry inside a hosted process.

} // namespace mh::tact
