// seams/host_event_sink.cpp -- mh.dll's SINK for the event-channel surface (LIFT-EVQ;
// docs/libmh-abi.md R7). HARNESS-side on purpose: this TU is the hosted config's routing of
// each designed record back onto the original fine thunk, invoked SYNCHRONOUSLY at emit by
// libmh's emitter -- the original draw/sound happens at the original instant, which is what
// keeps the R5 bit-identity oracle exact through every conversion. A fork host never links
// this file; it binds its own sink or polls.
//
// An UNKNOWN (channel, kind) is a contract breach between libmh and this routing table -- the
// version handshake exists to prevent it, so a hit is counted and reported at arm-report time
// rather than silently ignored (the same visible-red posture as the [hostapi] unbound walk).
#include <cstring>

#include "hook/host_event_sink.h"

#include "state/region_runtime.h" // mh::state::ptr<> -- the palette cells are RELOCATABLE regions
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/host_events.h"
#include "tact/tact_state.h" // the two composite draw scopes read the surface/icon state host-side

namespace {

uint32_t g_unknown = 0;

// ---- FX_DEBRIS_BURST's intensity chain, host-side (the LIFT-NOTIFY offscreen conversion) --------------------------
// Verbatim relocation of sim_bldg_state_destroyed.cpp's debris_intensity() + DEBRIS_INTENSITY_SCALE
// (the 0x004734d3-0x004734e7 x87 chain + the inlined utils_math_trunc @0x004d0596; DAT_00501312
// read via ReVA read-memory 2026-08-13 = IEEE754 double 2000.0 exactly). It lives HERE now because
// the record carries (tile_col, tile_row, energy_max) and the whole camera-dependent derivation --
// offscreen_fx_scale -> x87 intensity -> spawn_debris_burst -- is the hosted sink's business; the
// x87 form is kept (not a plain double expression) so the intermediate stays at 80-bit precision
// exactly as the original computes it.
constexpr double DEBRIS_INTENSITY_SCALE = 2000.0; // DAT_00501312

int32_t debris_intensity(int32_t fx_scale, double energy_max, double divisor) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fild    dword ptr [fx_scale]    ; ST(0) = (double)fx_scale                  (0x004734d3)
        fmul    qword ptr [energy_max]  ; ST(0) *= cfg_buildings[bid].energy        (0x004734d6)
        fdiv    qword ptr [divisor]     ; ST(0) /= DAT_00501312                     (0x004734dc)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                          (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended       (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                          (0x004d05a4)
        frndint                     ;                                          (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                 (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]       ;                                          (0x004734e7)
    }
    // clang-format on
    return r;
}

// ---- the two composite draw scopes (LIFT-TACT L4b) -----------------------------------------------
//
// Kinds 11 and 12 replace a whole body's draw sequence rather than one fine entry, so the sequence
// -- and every pixel literal in it -- lives HERE now. Both are verbatim relocations of the draw
// tails that were in tact_active_unit_count_hud_draw.cpp and tact_selection_panel_refresh.cpp, in
// the same order, with the same immediates read off the same .asm. Nothing is "cleaned up": the
// duplicate pack_rgb16 call (the original packs the same constant colour twice, once per line) and
// the x==clip_x / y==clip_y literal pairing are preserved because the original does them.
//
// THE SURFACE AND ICON POINTERS ARE READ HOST-SIDE, which is R4 working as intended: they used to
// cross the boundary as arguments (the icon one being a RAW POINTER out of the registry), and now
// the sink reads the state itself. That is one crossing of `sel_panel_icon_gfx_at` retired -- see
// docs/libmh-abi.md section 4, where the rest of that array's entanglement is still open.
constexpr const char *HUD_COUNT_FMT = ": %2d"; // s_:_%2d_005004e6

void draw_active_count_hud(int32_t active, int32_t cached) {
    mh::tact::tact_state st      = mh::tact::state();
    void *const          surface = const_cast<uint8_t *>(*st.read.gfx_draw_surface);
    const int32_t        pitch   = *st.read.gfx_panel_row_skip;

    // @0x00434e5d-0x00434e86: the icon blit. x/y and clip_x/clip_y are the same literal pair.
    mh::call::llm_ui_set_draw_surface(surface, pitch, 0x8a, 0x90, 0x8a, 0x90, 0x16, 0x18,
                                      st.own.sel_panel_icon_gfx_at(0));

    // @0x00434e8b-0x00434f19: the two formatted lines, 10 px apart. Each re-formats into the same
    // buffer and re-packs the same colour before drawing -- that per-line ordering is the thing the
    // retired offline test used to pin, so it is spelled out rather than hoisted.
    char buf[32];
    mh::call::utils_sprintf__vi(buf, HUD_COUNT_FMT, active);
    uint16_t color = mh::call::llm_gfx_pack_rgb16(0xb4, 0, 0xfa);
    mh::call::llm_ui_text_draw_rgb16(0x8a, 0x90,
                                     (char *)mh::call::llm_str_ansi_to_wide_scratch(buf), color);
    mh::call::utils_sprintf__vi(buf, HUD_COUNT_FMT, cached);
    color = mh::call::llm_gfx_pack_rgb16(0xb4, 0, 0xfa);
    mh::call::llm_ui_text_draw_rgb16(0x8a, 0x9a,
                                     (char *)mh::call::llm_str_ansi_to_wide_scratch(buf), color);
}

// ---- LIFT-TABLE S3: the per-planet GRAPHICS tail of cfg_final_planet_Construct -----------------
//
// Verbatim relocation of 0x0045b8fe..0x0045ba1c, the contiguous graphics tail that used to sit at
// the end of sim/libtrans/sim_lt_cfg_planet.cpp. libmh keeps the sim half of that body and resolves
// tlo_index (cfg_GetTloIndex reads a stack local of ITS frame, so the index crosses and the pointer
// does not); everything below -- the 8-way switch, the `SHL AL,6`, the two Planets[] byte stores
// and the eight FillBankData calls -- is the host's, and after LIFT-TABLE S2 none of those bytes is
// in the determinism hash.
//
// The switch mapping is the RAW TABLE DECODE recorded in the original translation: the 8 dwords at
// switchdataD_0045b6d5 are 0x45b94b/0x45b9be/0x45b97e/0x45b9ae/0x45b99e/0x45b98e/0x45b96e/0x45b95e
// for slots 0..7 (= tlo 1..8), and tlo_index==2 shares its body with the out-of-range default.
void planet_gfx_setup(int32_t planet_index, int32_t tlo_index) {
    // Through the region runtime like the palette below. RID_PLANETS is not declared relocatable
    // today, so a constant address would pass check_movable_addresses -- which is exactly why it is
    // not spelled that way: the declaration is the thing that could change.
    auto *const planets = mh::state::ptr<mh::game::mh_cfg_final_struct_Planet>(
        mh::state::RID_PLANETS);
    // A slot outside cfg::final::data::Planets[32] is a contract breach, not something to store
    // through -- the same posture draw_order_button takes for an out-of-table button id.
    if (planet_index < 0 || planet_index >= 32) {
        ++g_unknown;
        return;
    }
    mh::game::mh_cfg_final_struct_Planet &planet = planets[planet_index];

    planet.tlo_index = static_cast<uint8_t>(tlo_index); // 0x0045b912

    int32_t bank_count, bank_id; // [EBP-0xc] / [EBP-0x10]
    switch (tlo_index) {
        case 1:
            bank_count = 2;
            bank_id    = 0x33;
            break; // caseD_1 @0x0045b94b
        case 3:
            bank_count = 1;
            bank_id    = 0x34;
            break; // caseD_3 @0x0045b97e
        case 4:
            bank_count = 2;
            bank_id    = 0x35;
            break; // caseD_4 @0x0045b9ae
        case 5:
            bank_count = 2;
            bank_id    = 0x37;
            break; // caseD_5 @0x0045b99e
        case 6:
            bank_count = 1;
            bank_id    = 0x36;
            break; // caseD_6 @0x0045b98e
        case 7:
            bank_count = 1;
            bank_id    = 0x38;
            break; // caseD_7 @0x0045b96e
        case 8:
            bank_count = 3;
            bank_id    = 0x32;
            break; // caseD_8 @0x0045b95e
        case 2:
        default:
            bank_count = 1;
            bank_id    = 0x00;
            break; // caseD_2 @0x0045b9be, shared with JA (>7)
    }

    // 0x0045b9cc-0x0045b9dd: BYTE-WIDTH `DEC AL / SHL AL,6`.
    planet.soldier_sprite_bank_offset =
        static_cast<uint8_t>((static_cast<uint8_t>(bank_count) - 1) << 6); // 0x0045b9d1

    // 0x0045b9e3-0x0045ba1c: planet 0x1f only (the injected/tutorial map slot) -- clear soldier
    // sprite banks 0x32..0x38 (used=0), then load the switch-selected bank_id (used=1).
    if (planet_index == 0x1f) {
        for (uint32_t bank = 0x32; bank < 0x39; ++bank) {
            mh::call::cfg_final_planet_FillBankData(0, bank,
                                                    static_cast<uint32_t>(planet_index)); // x7
        }
        mh::call::cfg_final_planet_FillBankData(1, static_cast<uint32_t>(bank_id),
                                                static_cast<uint32_t>(planet_index));
    }
}

// ---- LIFT-TABLE S4: the planet-map palette block (llm_strat_planet_map_session_init's tail) -----
//
// Verbatim relocation of 0x0045dc69c..0x004dc744 -- nine llm_gfx_pack_rgb16 calls and their nine
// stores, in the original's order, with the original's literal triples. It reads no state and takes
// no payload.
//
// THE ARGUMENT ORDER IS (red, blue, green), NOT (r, g, b), and it is spelled out here because this
// is now the only place the names matter. llm_gfx_pack_rgb16 @0x0043c56b: param_1 & 0xf8 << 8 ->
// bits 15-11 (red), param_2 & 0xf8 >> 3 -> bits 4-0 (BLUE), param_3 & 0xfc << 3 -> bits 10-5
// (GREEN). Its plate says the same. Two of the game's own globals inherited the mistake and were
// renamed with this change (EN v398): 0x00fe5b64 is fed pack_rgb16(0,0,0xff) = pure GREEN and had
// been named _..._BLUE; 0x00fe5b68 is fed pack_rgb16(0,0xff,0) = pure BLUE and had been named
// _..._GREEN. The VALUES below are unchanged -- every call is positional -- so this is a naming fix,
// not a behaviour change, and it is the fix that keeps the next reader from swapping two channels.
void planet_map_palette() {
    auto pack = [](uint32_t red, uint32_t blue, uint32_t green) {
        return mh::call::llm_gfx_pack_rgb16(red, blue, green);
    };
    // THROUGH THE REGION RUNTIME, not mh::addr::. All nine cells are RELOCATABLE regions: under
    // [harness] relocate_state=1 they move and the stock range is filled with 0xCD, so a constant
    // address here would write poison. check_movable_addresses enforces it, and it caught this.
    auto at = [](mh::state::region_id r) { return mh::state::ptr<uint16_t>(r); };

    *at(mh::state::RID_STRAT_PLANET_MAP_PAL5_RED)     = pack(0xff, 0x00, 0x00); // @0x004dc6a5/aa
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL5_BLUE)    = pack(0x00, 0xff, 0x00); // @0x004dc6b9/be
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL5_MAGENTA) = pack(0xff, 0x00, 0xff); // @0x004dc6cd/d2
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL5_GREEN)   = pack(0x00, 0x00, 0xff); // @0x004dc6e1/e6
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL5_BLACK)   = pack(0x00, 0x00, 0x00); // @0x004dc6f2/f7
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL4_WHITE)   = pack(0xff, 0xff, 0xff); // @0x004dc706/0b
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL4_BLACK)   = pack(0x00, 0x00, 0x00); // @0x004dc717/1c
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL4_MAGENTA) = pack(0xff, 0x00, 0xff); // @0x004dc72b/30
    *at(mh::state::RID_STRAT_PLANET_MAP_PAL4_YELLOW)  = pack(0xff, 0xff, 0x00); // @0x004dc73f/44
}

void draw_sel_panel_bg() {
    // @0x00434b0f-0x00434b35: the panel's background plate, icon slot 1.
    mh::tact::tact_state st = mh::tact::state();
    mh::call::llm_ui_set_draw_surface(const_cast<uint8_t *>(*st.read.gfx_draw_surface),
                                      *st.read.gfx_panel_row_skip, /*x=*/0, /*y=*/0xf0,
                                      /*clip_x=*/0, /*clip_y=*/0x48, /*clip_w=*/0xa0,
                                      /*clip_h=*/0xf0, st.own.sel_panel_icon_gfx_at(1));
}

// The four order buttons' icon blits (LIFT-TACT slice A). ONE primitive repeated four times in
// llm_tact_ui_order_buttons_minimap_tick @0x00435cf1 with a different icon slot and origin; the
// clip is always the icon's own header dims at (0,0), which is why only slot/x/y needed a table.
// The comments are the four set_draw_surface call sites in the original.
void draw_order_button(int32_t button) {
    struct button_geom {
        int32_t slot, x, y;
    };
    static constexpr button_geom BUTTONS[4] = {
        {0x23, 0x01, 0x8f}, // @0x00435d5b  CLEAR
        {0x24, 0x14, 0x8f}, // @0x00435e26  CLEAR + HOLD
        {0x28, 0x25, 0x8f}, // @0x00435ef1  STOP
        {0x25, 0x36, 0x91}, // @0x00435fc6  MOVE + STOP
    };
    // An id outside the table is a contract breach between libmh and this routing, not something
    // to blit at a guessed address -- the unknown counter downstream is the right place for it.
    if (button < 0 || button > 3) {
        ++g_unknown;
        return;
    }

    const button_geom   &g    = BUTTONS[button];
    mh::tact::tact_state st   = mh::tact::state();
    void                *icon = st.own.sel_panel_icon_gfx_at(g.slot);
    const uint16_t      *hdr  = static_cast<const uint16_t *>(icon);
    mh::call::llm_ui_set_draw_surface(const_cast<uint8_t *>(*st.read.gfx_draw_surface),
                                      *st.read.gfx_panel_row_skip, g.x, g.y, /*clip_x=*/0,
                                      /*clip_y=*/0, hdr[0], hdr[1], icon);
}

// One icon blit at a literal origin, at clip (0,0) and clipped to the icon's OWN HEADER DIMS --
// the shape every draw in the panel/sidebar cluster reduces to once its geometry stays host-side.
//
// THE CLIP ORIGIN IS NOT A PARAMETER, and that is the fix rather than a tidy-up. It used to take
// clip_x/clip_y, and exactly one caller passed a non-zero pair: draw_group_panel, whose original
// clips a 0x46 x 0x17 SUB-RECT. Reading the origin from the caller while still taking the SIZE
// from the icon header asks for a 160x168 read starting 142 rows in -- 1.9 KB past the end of a
// 0xD204-byte sprite, which faults whenever the next page happens to be unmapped. That is what
// made --tact-equiv give both verdicts on one unchanged commit. Every
// original in this cluster that clips at (0,0) also takes the size from the header, and the one
// that clips elsewhere also carries its own size literals -- so the two always travel together
// and a helper that separates them is the trap. It cannot be passed one now.
void blit_panel_icon(int32_t slot, int32_t x, int32_t y) {
    mh::tact::tact_state st   = mh::tact::state();
    void                *icon = st.own.sel_panel_icon_gfx_at(slot);
    const uint16_t      *hdr  = static_cast<const uint16_t *>(icon);
    mh::call::llm_ui_set_draw_surface(const_cast<uint8_t *>(*st.read.gfx_draw_surface),
                                      *st.read.gfx_panel_row_skip, x, y, /*clip_x=*/0,
                                      /*clip_y=*/0, hdr[0], hdr[1], icon);
}

// llm_tact_sidebar_dispatch @0x00435a47. The one draw in the cluster whose clip is NOT (0,0) AND
// whose size is NOT the icon's header: the original passes the sub-rect (2, 0x8e) 0x46 x 0x17 out
// of icon slot 0 -- the same background plate draw_sel_panel_bg and the count HUD both blit
// pieces of. All six numbers are the original's literals, which is why this is spelled out here
// rather than routed through the helper above.
void draw_group_panel() {
    mh::tact::tact_state st = mh::tact::state();
    mh::call::llm_ui_set_draw_surface(const_cast<uint8_t *>(*st.read.gfx_draw_surface),
                                      *st.read.gfx_panel_row_skip, /*x=*/2, /*y=*/0x8e,
                                      /*clip_x=*/2, /*clip_y=*/0x8e, /*clip_w=*/0x46,
                                      /*clip_h=*/0x17, st.own.sel_panel_icon_gfx_at(0));
}

// The mode tab, from either tick -- same origin, different icon. @0x00436a3c / @0x00436217.
void draw_sel_panel_mode_tab(int32_t mode) {
    if (mode != 0 && mode != 1) {
        ++g_unknown;
        return;
    }
    blit_panel_icon(/*slot=*/mode == 0 ? 3 : 4, /*x=*/0, /*y=*/0xa8);
}

// A per-row toggle in the multi-select panel. The row -> y arithmetic is the original's, kept
// here with it: @0x00436396 (defense) and @0x00436420 (gun).
void draw_sel_panel_row_toggle(int32_t which, int32_t row) {
    if (which == 0) {
        blit_panel_icon(/*slot=*/0x26, /*x=*/0x84, /*y=*/row * 0x30 + 0xf7);
    } else if (which == 1) {
        blit_panel_icon(/*slot=*/0x27, /*x=*/0x44, /*y=*/row * 0x30 + 0x101);
    } else {
        ++g_unknown;
    }
}

// llm_tact_ui_sel_panel_init's HEAD, host-side (LIFT-TACT slice A). A verbatim relocation of
// @0x00433eac-0x00434029 -- the icon bank's load + 565->555 conversion, the three fixed background
// panels, both font selections and both labels -- in the original's order, with the original's own
// immediates. What did NOT come with it is the tail: `ui_sel_panel_multi_mode = 0` and the
// refresh/redraw/clear trio stay in libmh, because libmh reads that latch every tick and the trio
// re-enters libmh's own converted bodies.
//
// THE ICON ARRAY IS WRITTEN HERE NOW, which is the point rather than a side effect: the deferral in
// docs/libmh-abi.md section 4 asked whether the pointer array stays libmh-owned, and the answer is
// no -- every consumer only ever blitted the pointer or read its (w,h) header as the clip for that
// same blit, so with the draws host-side no translated body touches the array at either end.
// Slot 2 is deliberately not drawn: the .asm's base+offset arithmetic (0x00558f04 / 0x00558f08 /
// 0x00558f10) skips it, and skipping it is preserved rather than normalised.
constexpr const char *SEL_PANEL_ICON_PATH_FMT    = "panelb\\%s.gfx"; // s_panelb\%s.gfx_005004b4
constexpr int32_t     SEL_PANEL_ICON_NAME_STRIDE = 0xa;
constexpr int32_t     SEL_PANEL_TEXT_ID_LABEL_1  = 580;
constexpr int32_t     SEL_PANEL_TEXT_ID_LABEL_2  = 581;

void blit_icon_own_header(void *surface, int32_t pitch, int32_t x, int32_t y, void *icon) {
    const uint16_t *hdr = static_cast<const uint16_t *>(icon);
    mh::call::llm_ui_set_draw_surface(surface, pitch, x, y, /*clip_x=*/0, /*clip_y=*/0, hdr[0],
                                      hdr[1], icon);
}

void draw_sel_panel_init() {
    mh::tact::tact_state st      = mh::tact::state();
    void *const          surface = const_cast<uint8_t *>(*st.read.gfx_draw_surface);
    const int32_t        pitch   = *st.read.gfx_panel_row_skip;

    // @0x00433eac-0x00433f0d: format each slot's sprite path, resolve it, store the pointer, then
    // convert it in place. The icon-name record's base IS the name string (offset 0).
    for (int32_t i = 0; i < *st.read.sel_panel_icon_count; ++i) {
        char        icon_path[0x20];
        const char *icon_name = reinterpret_cast<const char *>(
            st.read.sel_panel_icon_names + i * SEL_PANEL_ICON_NAME_STRIDE);
        mh::call::utils_sprintf__vs(icon_path, SEL_PANEL_ICON_PATH_FMT, icon_name);
        uint8_t *sprite                 = mh::call::GetResourseFilePtr(icon_path);
        st.own.sel_panel_icon_gfx_at(i) = sprite;
        mh::call::llm_gfx_convert_pixels_565_to_555(reinterpret_cast<int16_t *>(sprite));
    }

    // @0x00433f0f-0x00433fa8: the three fixed background panels, icon slots 0, 1 and 3.
    blit_icon_own_header(surface, pitch, 0, 0, st.own.sel_panel_icon_gfx_at(0));
    blit_icon_own_header(surface, pitch, 0, 0xa8, st.own.sel_panel_icon_gfx_at(1));
    blit_icon_own_header(surface, pitch, 0, 0xa8, st.own.sel_panel_icon_gfx_at(3));

    // @0x00433fa8-0x00433fb9: font 0 THEN font 1 -- both calls happen, in this order, even though
    // only font 1 is live for the two draws below.
    mh::call::llm_gfx_font_select(0);
    mh::call::llm_gfx_font_select(1);

    // @0x00433fb9-0x00434029: two fixed labels, each re-packing the same colour before drawing.
    {
        wchar_t label_buf[0x20];
        mh::call::w_sprintf__v(label_buf, st.read.text_ptrs[SEL_PANEL_TEXT_ID_LABEL_1]);
        uint16_t color = mh::call::llm_gfx_pack_rgb16(0xc8, 0, 0xc8);
        mh::call::llm_ui_text_draw_rgb16(0x52, 0x90, reinterpret_cast<char *>(label_buf), color);
    }
    {
        wchar_t label_buf[0x20];
        mh::call::w_sprintf__v(label_buf, st.read.text_ptrs[SEL_PANEL_TEXT_ID_LABEL_2]);
        uint16_t color = mh::call::llm_gfx_pack_rgb16(0xc8, 0, 0xc8);
        mh::call::llm_ui_text_draw_rgb16(0x52, 0x9a, reinterpret_cast<char *>(label_buf), color);
    }
}

// The two bodies LIFT-TACT slice A moved host-side WHOLE. There is no geometry table here because
// there is no geometry to hold: the originals ARE the draw, and the sink's whole job is to call
// them with the roster identity the record carried.
//
// `highlight_flag` is the literal 1 both original call sites pass (llm_tact_ui_sel_panel_single_
// mode_tick @0x00436e69 and @0x00436f05). It is passed here rather than crossed because it says
// nothing about what happened -- the row lit up; that IS the highlight.
void draw_sidebar_row_hover(int32_t side, int32_t unit_id, int32_t row) {
    if (side == 0) {
        mh::call::llm_tact_ui_sidebar_row_draw_left(unit_id, row, /*highlight_flag=*/1);
    } else if (side == 1) {
        mh::call::llm_tact_ui_sidebar_row_draw_right(unit_id, row, /*highlight_flag=*/1);
    } else {
        ++g_unknown;
    }
}

// llm_tact_frame's mine-blast exit wipe, @0x004333c1-0x004333e6 (LIFT-TACT slice B). The loop
// bound is the original's own compile-time 0x10 and the three calls are in the original's order --
// the whole thing is host-side because the frame loop is, not because sixteen records would have
// been wrong. Collapsing it is also what absorbs llm_tact_scroll_fade_step: this loop was its only
// libmh site, so the fine entry leaves with the scope rather than needing one of its own.
void run_blast_transition() {
    for (int32_t i = 0; i < 0x10; ++i) {
        mh::call::llm_tact_scroll_fade_step();
        mh::call::llm_tact_blink_overlay_clear();
        mh::call::llm_tact_frame_cursor_and_reset();
    }
}

void (*g_logger)(const char *) = nullptr;
bool g_seen[4][64]; // [channel][kind] first-dispatch latch; larger values just skip the line

void log_first_dispatch(const libmh_event *e) {
    if (g_logger == nullptr || e->channel >= 4 || e->kind >= 64 || g_seen[e->channel][e->kind])
        return;
    g_seen[e->channel][e->kind] = true;
    char m[96];
    // wsprintfA-free: hook/ must not pull user32; small manual format.
    int n = 0;
    for (const char *s = "; [hostevt] first dispatch: channel "; *s; ++s) m[n++] = *s;
    m[n++] = static_cast<char>('0' + e->channel);
    for (const char *s = ", kind "; *s; ++s) m[n++] = *s;
    if (e->kind >= 10) m[n++] = static_cast<char>('0' + e->kind / 10);
    m[n++] = static_cast<char>('0' + e->kind % 10);
    m[n++] = '\n';
    m[n]   = '\0';
    g_logger(m);
}

// The TEXT surface's routing: the sink receives the emitter's own composed buffer synchronously at
// emit and hands it to the original print thunk -- the same pointer the site used to pass itself,
// so the hosted config stays bit-identical. Kind 0 reuses the record latch's channel-0 row (unused
// by any record channel) for the first-dispatch census.
void route_text(uint16_t kind, const uint16_t *text) {
    if (g_logger != nullptr && kind < 64 && !g_seen[0][kind]) {
        g_seen[0][kind] = true;
        char m[64];
        int  n = 0;
        for (const char *s = "; [hostevt] first text dispatch: kind "; *s; ++s) m[n++] = *s;
        m[n++] = static_cast<char>('0' + kind % 10);
        m[n++] = '\n';
        m[n]   = '\0';
        g_logger(m);
    }
    void *t = const_cast<uint16_t *>(text);
    switch (kind) {
        case LIBMH_EVTK_TEXT_MAIN: mh::call::game_ui_PrintTextMessage(t); return;
        case LIBMH_EVTK_TEXT_FLOAT_RED: mh::call::llm_ui_print_floating_msg_red(t); return;
        case LIBMH_EVTK_TEXT_FLOAT_CYAN: mh::call::llm_ui_print_floating_msg_cyan(t); return;
        case LIBMH_EVTK_TEXT_TACT_EXIT_CONFIRM:
            // The prompt's position and colour are the original's literals at
            // llm_tact_frame @0x004343xx; only the composed string crossed.
            mh::call::llm_gfx_draw_text_rgb(0x96, 0xf0, t, 0xff, 0xff, 0xff);
            return;
        default: break;
    }
    ++g_unknown;
}

void route(const libmh_event *e) {
    log_first_dispatch(e);
    switch (e->channel) {
        case LIBMH_EVC_INVALIDATE:
            switch (e->kind) {
                case LIBMH_EVK_INV_TACT_VIEW_TILES:
                    mh::call::llm_tact_mark_view_tiles_dirty();
                    return;
                case LIBMH_EVK_INV_VIEWPORT: mh::call::llm_map_cam_mark_viewport_dirty(); return;
                case LIBMH_EVK_INV_TACT_VIS_MAP: mh::call::llm_tact_vis_map_fill_default(); return;
                case LIBMH_EVK_INV_TACT_VIS_MARGIN:
                    mh::call::llm_tact_vis_map_clear_right_margin();
                    return;
                case LIBMH_EVK_INV_TACT_UNIT_SLOT:
                    mh::call::llm_tact_unit_refresh_ui_slot(e->a);
                    return;
                case LIBMH_EVK_INV_TACT_SIDEBAR_ROSTER:
                    mh::call::llm_tact_ui_sidebar_roster_refresh();
                    return;
                case LIBMH_EVK_INV_TACT_SIDEBAR_ROWS:
                    mh::call::llm_tact_ui_sidebar_draw_rows();
                    return;
                case LIBMH_EVK_INV_TACT_SIDEBAR_SLOT:
                    mh::call::llm_tact_ui_sidebar_redraw_unit_slot(e->a);
                    return;
                case LIBMH_EVK_INV_TACT_PLAYER_ROWS:
                    mh::call::llm_tact_ui_draw_player_row_list(e->a);
                    return;
                // ---- whole-body draw scopes (LIFT-TACT L4b) ---------------------------------
                case LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW:
                    mh::call::llm_tact_ui_char_panel_row_refresh(e->a);
                    return;
                case LIBMH_EVK_INV_TACT_ACTIVE_COUNT_HUD: draw_active_count_hud(e->a, e->b); return;
                case LIBMH_EVK_INV_TACT_SEL_PANEL_BG: draw_sel_panel_bg(); return;
                case LIBMH_EVK_INV_TACT_ORDER_BUTTON:
                    draw_order_button(e->a);
                    return;
                case LIBMH_EVK_INV_TACT_GROUP_PANEL: draw_group_panel(); return;
                case LIBMH_EVK_INV_TACT_SEL_PANEL_MODE_TAB:
                    draw_sel_panel_mode_tab(e->a);
                    return;
                case LIBMH_EVK_INV_TACT_SEL_PANEL_ROW_TOGGLE:
                    draw_sel_panel_row_toggle(e->a, e->b);
                    return;
                case LIBMH_EVK_INV_TACT_SEL_PANEL: mh::call::llm_tact_ui_sel_panel_draw(); return;
                case LIBMH_EVK_INV_TACT_CHAR_PANEL_ROW_DRAW:
                    mh::call::llm_tact_ui_char_panel_row_draw(e->a, e->b);
                    return;
                case LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER:
                    draw_sidebar_row_hover(e->a, e->b, e->c);
                    return;
                case LIBMH_EVK_INV_TACT_SEL_PANEL_INIT: draw_sel_panel_init(); return;
                case LIBMH_EVK_INV_TACT_CURSOR_SPRITE: mh::call::gfx_LoadSprite(e->a); return;
                case LIBMH_EVK_INV_TACT_DRAWN_MAP_CLEAR:
                    mh::call::llm_tact_blink_overlay_clear();
                    return;
                case LIBMH_EVK_INV_TACT_DRAG_BOX:
                    mh::call::llm_tact_drag_box_clamp(e->a, e->b, e->c, e->d);
                    return;
                case LIBMH_EVK_INV_TACT_MINIMAP_OVERLAY:
                    mh::call::llm_tact_tile_overlay_refresh();
                    return;
                // ---- the mission start/end one-shots (LIFT-TACT slice B2) --------------------
                case LIBMH_EVK_INV_GFX_VIEW_METRICS: mh::call::llm_gfx_view_metrics_init(); return;
                case LIBMH_EVK_INV_TACT_VIEW_METRICS:
                    mh::call::llm_tact_view_metrics_init();
                    return;
                case LIBMH_EVK_INV_TACT_VIEW_TILE_ROWS:
                    mh::call::llm_tact_gfx_view_tile_rows_init();
                    return;
                case LIBMH_EVK_INV_TACT_SPRITE_BANKS:
                    mh::call::llm_tact_gfx_load_banks_alt();
                    return;
                case LIBMH_EVK_INV_ALL_SPRITE_BANKS:
                    mh::call::llm_gfx_load_all_sprite_banks();
                    return;
                case LIBMH_EVK_INV_SPRITE_PIX_OFFSETS:
                    mh::call::llm_gfx_sprite_pix_offsets_init();
                    return;
                case LIBMH_EVK_INV_PLANET_GFX_SETUP: planet_gfx_setup(e->a, e->b); return;
                case LIBMH_EVK_INV_PLANET_MAP_PALETTE: planet_map_palette(); return;
                // ---- SIMABI-NOTIFY: the sim table's two no-payload invalidates ---------------
                case LIBMH_EVK_INV_PLAYER_COLOR_LUT:
                    mh::call::llm_strat_player_apply_all_colors();
                    return;
                case LIBMH_EVK_INV_PLANET_EXTRA_SPRITE_BANKS:
                    mh::call::llm_gfx_load_planet_extra_sprite_banks();
                    return;
                default: break;
            }
            break;
        case LIBMH_EVC_EVENT:
            switch (e->kind) {
                case LIBMH_EVK_SND_PLAY: mh::call::llm_snd_play(e->a, e->b); return;
                case LIBMH_EVK_SND_AMBIENT_TICK: mh::call::llm_snd_ambient_tick(); return;
                case LIBMH_EVK_SND_AMBIENT_CLONE:
                    mh::call::llm_snd_ambient_planet_clone(static_cast<uint32_t>(e->a));
                    return;
                case LIBMH_EVK_SND_STOP_ALL: mh::call::llm_snd_stop_all_channels(); return;
                case LIBMH_EVK_SND_RACE_ALERT: mh::call::llm_strat_race_alert_sound_emit(); return;
                case LIBMH_EVK_SND_ZONE_PLAY: mh::call::llm_tact_zone_sound_play(e->a); return;
                case LIBMH_EVK_SND_FX_PLAY:
                    mh::call::llm_tact_fx_play_sound(e->a, e->b, e->c);
                    return;
                case LIBMH_EVK_TEXT_QUEUE_ID: mh::call::llm_ui_print_queue_text_id(e->a); return;
                case LIBMH_EVK_TEXT_GAME_SPEED: mh::call::llm_ui_print_game_speed(); return;
                case LIBMH_EVK_TEXT_RACE_ALERT: mh::call::llm_strat_race_alert_text_emit(); return;
                case LIBMH_EVK_PROGRESS_UNIT_AVAILABLE:
                    mh::call::llm_progress_notify_unit_available(static_cast<uint16_t>(e->a),
                                                                 static_cast<uint16_t>(e->b));
                    return;
                case LIBMH_EVK_PLAYER_SET_COLOR:
                    mh::call::llm_strat_player_set_color(e->a, static_cast<uint32_t>(e->b));
                    return;
                case LIBMH_EVK_MSG_QUEUE_CLEAR: mh::call::llm_ui_message_queue_clear_all(); return;
                case LIBMH_EVK_CAM_JUMP_QUEUE_CLEAR: mh::call::llm_cam_jump_queue_clear(); return;
                case LIBMH_EVK_MENU_PLACEMENT_CLEAR:
                    mh::call::llm_menu_build_placement_pending_clear();
                    return;
                case LIBMH_EVK_CAM_SET_COL: mh::call::llm_map_cam_set_col(e->a); return;
                case LIBMH_EVK_CAM_SET_ROW: mh::call::llm_map_cam_set_row(e->a); return;
                case LIBMH_EVK_VIEW_ZOOM_SCALE: {
                    double  zx, zy;
                    int32_t x[2] = {e->a, e->b}, y[2] = {e->c, e->d};
                    std::memcpy(&zx, x, sizeof(zx));
                    std::memcpy(&zy, y, sizeof(zy));
                    mh::call::llm_map_set_zoom_scale(zx, zy);
                    return;
                }
                case LIBMH_EVK_VIEW_MINIMAP_ZOOM:
                    mh::call::llm_map_set_minimap_zoom_for_size();
                    return;
                case LIBMH_EVK_SND_PLAY_AT: {
                    // The original pair, in the original order, at the original (emit) instant.
                    const int32_t vol = mh::call::llm_strat_offscreen_snd_volume(e->b, e->c);
                    mh::call::llm_snd_play(e->a, vol);
                    return;
                }
                case LIBMH_EVK_FX_DEBRIS_BURST: {
                    const int32_t scale = mh::call::llm_strat_offscreen_fx_scale(e->a, e->b);
                    const int32_t intensity =
                        debris_intensity(scale, static_cast<double>(e->c), DEBRIS_INTENSITY_SCALE);
                    mh::call::llm_strat_spawn_debris_burst(intensity);
                    return;
                }
                case LIBMH_EVK_SCREENSHOT_SAVE: mh::call::llm_tact_save_screenshot(); return;
                default: break;
            }
            break;
        case LIBMH_EVC_SCREEN:
            switch (e->kind) {
                case LIBMH_EVK_SCR_OUTCOME_DIALOG:
                    mh::call::llm_ui_outcome_dialog(static_cast<uint8_t>(e->a));
                    return;
                case LIBMH_EVK_SCR_OVERLAY_DISMISS:
                    mh::call::llm_net_lockstep_overlay_dismiss();
                    return;
                case LIBMH_EVK_SCR_WAIT_PLAYER_SHOW:
                    mh::call::llm_net_lockstep_wait_player_overlay_show(e->a);
                    return;
                case LIBMH_EVK_SCR_MP_LEAVE_RESET:
                    mh::call::llm_net_mp_leave_reset_game_mode();
                    return;
                case LIBMH_EVK_SCR_BLDG_PANEL_OPEN: mh::call::llm_ui_bldg_panel_open(); return;
                case LIBMH_EVK_SCR_SYNC_OVERLAY_SHOW:
                    // The original fuses the arm with a poll-and-clear of the answer global and
                    // returns it. libmh reads that answer itself now, BEFORE emitting, and only
                    // emits on the frames where it read none -- so the poll half runs here over
                    // a -1 and does nothing, and the arm half is what the record buys.
                    (void)mh::call::llm_net_lockstep_sync_overlay_show();
                    return;
                case LIBMH_EVK_SCR_PLANET_SELECT_OPEN:
                    mh::call::llm_ui_planet_select_screen_open(e->a);
                    return;
                case LIBMH_EVK_SCR_DLG_FROM_TABLE: {
                    // The id -> table-address map is the HOST's, which is the point of carrying an
                    // id (R4). One entry today; an unknown id falls through to the unknown counter
                    // rather than dereferencing something it guessed.
                    if (e->a == static_cast<int32_t>(LIBMH_SCR_DLGT_MAIN_MENU_QUIT)) {
                        mh::call::llm_ui_dlg_build_from_table(
                            reinterpret_cast<void *>(0x00656d6au)); // llm_ui_dlg_table_00656d6a
                        return;
                    }
                    break;
                }
                // ---- the tutorial's choreography ------------------------------------------
                case LIBMH_EVK_SCR_TUT_UISTATE_RESTORE:
                    mh::call::llm_menu_tutorial_uistate_restore();
                    return;
                case LIBMH_EVK_SCR_MENU_BG_REDRAW:
                    // int return discarded -- libmh's adapter already answered 0 to its caller.
                    (void)mh::call::llm_ui_menu_bg_redraw_cb();
                    return;
                case LIBMH_EVK_SCR_CURSOR_MENU_DRAW: mh::call::llm_gfx_draw_cursor_menu(); return;
                case LIBMH_EVK_SCR_PRESENT_FLIP: mh::call::llm_gfx_present_flip(); return;
                case LIBMH_EVK_SCR_FONT_DESC_FOR_FLAGS:
                    // Descriptor pointer discarded -- it is the HOST's address and stays here (R4).
                    (void)mh::call::llm_gfx_font_desc_for_flags(static_cast<uint32_t>(e->a));
                    return;
                case LIBMH_EVK_SCR_FADE_TRANSITION_RUN:
                    // The spin the original body performed, performed HERE. mh.dll owns the frame
                    // loop, so blocking until the armed transition reports done is its business;
                    // libmh asked once and is not waiting on us.
                    while (mh::call::llm_ui_screen_fade_transition_tick() == 0) {
                    }
                    return;
                case LIBMH_EVK_SCR_TUT_HINT_LAYOUT: {
                    // The four assignments the original derives from the sprite's metrics, with the
                    // metric queries staying host-side. Widget geometry is int32 in
                    // _G_LLM_UI_TUTORIAL_HINT_WIDGET; libmh writes only .label and reads .flags.
                    const uint32_t sprite = static_cast<uint32_t>(e->a);
                    const int32_t  w      = static_cast<int32_t>(mh::call::llm_gfx_sprite_width(sprite));
                    const int32_t  h =
                        static_cast<int32_t>(mh::call::llm_gfx_ui_sprite_get_header_field2(sprite));
                    mh::game::mh_llm_ui_widget *hint = reinterpret_cast<mh::game::mh_llm_ui_widget *>(
                        0x00650f63u); // _G_LLM_UI_TUTORIAL_HINT_WIDGET
                    hint->x      = -0xa4 - w;
                    hint->y      = -0x24 - h;
                    hint->width  = w - 0x10;
                    hint->height = h - 8;
                    return;
                }
                case LIBMH_EVK_SCR_WGTL_CENTER:
                case LIBMH_EVK_SCR_WGTL_DRAW: {
                    // The id -> widget-list-address map is the HOST's, same as the dialog tables
                    // above. An unknown id falls through to the unknown counter rather than
                    // dereferencing an address it guessed.
                    void *list = nullptr;
                    switch (static_cast<uint32_t>(e->a)) {
                        case LIBMH_SCR_WGTL_TUTORIAL_STEP:
                            list = reinterpret_cast<void *>(0x00653b17u);
                            break;
                        case LIBMH_SCR_WGTL_TUTORIAL_DONE:
                            list = reinterpret_cast<void *>(0x00653bafu);
                            break;
                        case LIBMH_SCR_WGTL_TUTORIAL_INTRO:
                            list = reinterpret_cast<void *>(0x00653acbu);
                            break;
                        case LIBMH_SCR_WGTL_TUTORIAL_WELCOME:
                            list = reinterpret_cast<void *>(0x00653b63u);
                            break;
                        default: break;
                    }
                    if (list != nullptr) {
                        if (e->kind == LIBMH_EVK_SCR_WGTL_CENTER)
                            mh::call::llm_ui_widget_list_center(list);
                        else
                            mh::call::llm_ui_widget_list_draw(
                                static_cast<mh::game::mh_llm_ui_widget_list *>(list));
                        return;
                    }
                    break;
                }
                case LIBMH_EVK_SCR_TACT_BLAST_TRANSITION: run_blast_transition(); return;
                case LIBMH_EVK_SCR_TACT_FRAME_PRESENT:
                    mh::call::llm_tact_frame_cursor_and_reset();
                    return;
                case LIBMH_EVK_SCR_TACT_MISSION_MEDIA:
                    mh::call::llm_ui_info_media_draw_p1();
                    return;
                // ---- SIMABI-NOTIFY: the strategic frame's two ends ---------------------------
                // Dispatched synchronously at emit, so the original present happens at the
                // original instant -- the frame body's last statement, exactly where it was.
                case LIBMH_EVK_SCR_STRAT_FRAME_PRESENT:
                    mh::call::llm_strat_render_present();
                    return;
                case LIBMH_EVK_SCR_STRAT_FRAME_REDRAW:
                    mh::call::llm_strat_render_view();
                    return;
                // ---- SIMABI-DISPLAY: the strategic view's display size -----------------------
                // THE STORE IS PART OF THE ROUTING, not an extra. The original caller
                // (llm_game_start_tutorial @0x004bafdc) assigned the entry's return -- the
                // APPLIED mode, read back off WindowWidth -- into _G_LLM_VIEW_SIZE_MODE, and
                // libmh has stopped doing that because it cannot compute the value. The cell has
                // untranslated original readers (llm_ui_outcome_dlg_open, llm_ui_menu_close_to_hud,
                // llm_strat_input_update) and a translated tact one, so in the HOSTED config it
                // must keep being written at this instant: R5's bit-identity oracle is the point.
                // A headless host with no window binds a sink that does nothing here.
                case LIBMH_EVK_SCR_SET_DISPLAY_MODE:
                    *mh::state::ptr<int32_t>(mh::state::RID_VIEW_SIZE_MODE) =
                        mh::call::llm_view_set_size_mode(e->a);
                    return;
                default: break;
            }
            break;
        default: break;
    }
    ++g_unknown;
}

} // namespace

namespace mh::hook {

int bind_host_event_sink() {
    const int rc_records = libmh_set_event_sink(route, LIBMH_HOST_EVENTS_VERSION);
    const int rc_texts   = libmh_set_text_sink(route_text, LIBMH_HOST_EVENTS_VERSION);
    return rc_records != 0 ? rc_records : rc_texts; // one rc: both surfaces bind or the seam is red
}

uint32_t host_event_sink_unknown_count() { return g_unknown; }

void set_host_event_sink_logger(void (*logger)(const char *line)) { g_logger = logger; }

} // namespace mh::hook
