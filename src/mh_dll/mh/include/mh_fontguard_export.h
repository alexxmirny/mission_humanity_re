//
// include/mh_fontguard_export.h -- mp:F2: the glyph-lookup guard in front of the text layout pass,
// and the font probe that makes a non-ASCII string renderable by a UI-suite scenario.
//
// ---- WHAT THE ORIGINAL DOES, AND WHERE IT BREAKS -----------------------------------------------
//
// `llm_gfx_font_layout_text` (EN 0x004a2400) turns a UTF-16 string into the glyph-pointer list the
// blitters consume, through TWO indirections and no bounds check on either:
//
//     glyph = CUR_FONT_GLYPH_PTR_TABLE[ charmap[code_unit] ]      charmap = *(int**)0x005d81f4
//
// `charmap` is filled by `llm_ui_main_menu_screen_load` straight out of `fnt\FontLay.txt`
// (`charmap[cp] = ordinal`, counting from 1), and `llm_gfx_font_load` builds the pointer table with
// one entry per `.FNT` record at index 1..N -- then parks a zero-filled scratch buffer at index 0,
// whose first byte is the UI-scaled line height.
//
// ---- ORDINAL 0 IS THE SPACE. READ THIS BEFORE "FIXING" ANYTHING HERE -------------------------
//
// **U+0020 is not in any shipped FONTLAY** -- not EN base, not EN override, not RU override. The
// game draws a space by falling through the charmap's zero onto `glyph_ptr[0]`, which is that
// blank scratch buffer, i.e. a blank cell one line-height wide. So ordinal 0 is a FALLBACK THE
// ORIGINAL DEPENDS ON, not the garbage the decompile makes it look like.
//
// The first version of this guard read it the other way round and substituted the box for every
// ordinal 0. The rendered frame said so immediately -- the probe came out as
// "F2?GUARD?[??]?ascii?intact", '?' where every space belonged. Hence the rule the code actually
// uses: the substitution is keyed on the CODE UNIT, not the ordinal. At or below U+0020 (space and
// the C0 controls) ordinal 0 is left exactly as retail leaves it; above it, a character the charmap
// does not map is a genuinely missing glyph and gets the box.
//
// ---- SO THERE IS ONE DEFECT, NOT TWO, AND IT IS NOT IN THE FONT THE PLAN NAMED ----------------
//
//   * SLOTS 1-6 (PFMENU0-4, every menu/lobby/dialog string). `charmap` is `DAT_0060434c`, and it is
//     EXACTLY 0x40000 bytes -- 65536 u32 entries, one per u16 code unit, ending at 0x0064434c where
//     the next initialised variable begins. An unmapped code unit here is NOT an out-of-bounds read
//     and never was: it reads a legitimate zero and draws the blank cell above. Harmless, and the
//     retail RU build relies on the array being this big (its FontLay lists U+2026).
//   * SLOT 0 (FONTY08, the HUD numeric font). `charmap` is `DAT_005d81f8`, and that one is 257
//     entries (0x404 bytes, ending at `DAT_005d85fc`, the font-name buffer). Code unit >= 257 reads
//     PAST it -- over the font-name buffer and whatever follows -- and uses the garbage it finds as
//     a glyph-table index. THAT is the wild pointer, and it is one string away from a crash.
//
// The guard replaces the whole body (`install_jmp`, so the original never runs) with the same loop
// plus two decisions: the charmap index is clamped to the active array's real extent (the crash
// fix, unconditional), and a code unit above U+0020 that the charmap does not map is substituted --
// U+FFFD if this font carries it (the merged fonts from `src/formats/fnt.py merge` do), else U+00A4,
// else '?', else retail's blank cell. Everything else is byte-for-byte what the original computed,
// including the running width in `_G_LLM_GFX_TEXT_LAYOUT_WIDTH`.
//
// ---- THE PROBE ---------------------------------------------------------------------------------
//
// `[fonts] probe_text=<utf8>` draws one string per present through the game's own
// `llm_gfx_draw_text_blend_clipped`, i.e. through the real layout + blit path this file guards.
// It exists because nothing else in the harness can put an arbitrary UTF-16 code point on a frame:
// the typed path folds through `ToAscii`/CP_ACP (that is mp:F3, which DEPENDS on this item), and
// `setup.dat` is byte-transparent, so both can only deliver 8-bit values. The probe is the only
// way F2's own acceptance -- "these glyphs render" -- can be a picture rather than an assertion.
//
// OFF unless the ini says otherwise. No `[fonts]` section at all = probe off, guard on.
//
#pragma once

// Arm the guard (and read the [fonts] ini block). Returns 1 if the guard took the entry.
extern "C" int MH_FontGuard_Install(void);

// Per-present: draw `[fonts] probe_text`, if one is configured. Cheap (one compare) when idle.
extern "C" void MH_FontGuard_OnPresent(void);
