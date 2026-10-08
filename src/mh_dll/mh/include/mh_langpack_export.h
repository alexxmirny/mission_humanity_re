//
// include/mh_langpack_export.h -- mods:LANG1 / LANG2: language packs on the EN exe.
//
// ---- WHAT A LANGUAGE PACK IS -----------------------------------------------------------------
//
// Every localized byte of the game lives in ONE resource pack, `mh_ex.rsr`/`mh_ex.nam` (the
// initlang TEXT table, the fonts, the menu art, the voice samples, the info texts). `mh.rsr` is
// byte-identical across the EN and RU builds, and so is the data layout of the two exes. So "play
// the EN exe in Russian" is "open a different mh_ex", and retail already does exactly that when a
// player copies the RU `mh_ex` over the EN one (user, 2026-09-29).
//
// `[lang] pack=<id>` in mh_net.ini makes that a configuration instead of a file swap:
//
//     <exe dir>\lang\<id>\mh_ex.rsr + mh_ex.nam     -- the pack (required, both, as a pair)
//     <exe dir>\lang\<id>\Msgs.dat                  -- the title / CD-prompt strings (optional)
//
// `pack=` unset, empty or `en` is the stock install, byte for byte: nothing is patched. A pack
// whose folder or pack pair is missing is logged and the stock `mh_ex` loads instead.
//
// ---- THE SEAM (least invasive available) -------------------------------------------------------
//
// Two 5-byte `mov eax, imm32` operands, each guarded by its exact original bytes
// (mh::hook::patch_bytes_guarded), repointed at a DLL-owned path string:
//
//   0x004c3dba  B8 E2 31 50 00   mov eax, "mh_ex"     (llm_boot_progress_draw -> rsr_TryReadRsrFile)
//   0x004cb6bc  B8 CF 34 50 00   mov eax, "Msgs.dat"  (ReadMsgsDat -> utils_open_file)
//
// Both instructions execute AFTER DllMain (the first in the boot-progress path, the second at the
// top of WinMain), so writing them from the arm is in time. rsr_TryReadRsrFile formats
// "%s.nam"/"%s.rsr" into a 128-byte stack buffer, so the path handed over is RELATIVE
// (`lang\<id>\mh_ex`) whenever the working directory is the exe's -- the same assumption the
// stock bare name `mh_ex` makes -- and absolute only when that fits the buffer. mh.rsr is never
// touched; everything the pack does not carry still comes from it.
//
// Log channel: mh_video.log (the advisory mh.dll-owned GFX/resource channel -- NOT mh_net.log,
// whose arm-window ORDER is gated by tools/check_arm_order.py; a stock run writes no new line).
//
#pragma once

// Read `[lang] pack=` and, when a pack is named and present, repoint mh_ex + Msgs.dat at it.
// Returns 1 if a pack was armed, 0 for stock (unset/en/missing/refused). Idempotent.
extern "C" int MH_LangPack_Install(void);

// The armed pack id ("ru"), or "" when the stock mh_ex is in use.
extern "C" const char *MH_LangPack_Id(void);

// RL17: the 8-bit codepage the armed pack declares in `lang\<id>\pack.ini` (`[pack] codepage=1251`),
// or 0 when no pack is armed / the file or key is absent. ui_chat_input.cpp uses it as the input
// codepage when `[input] codepage` is not pinned in mh_net.ini (an explicit setting always wins).
extern "C" unsigned MH_LangPack_Codepage(void);

// mods:LANG2. Does the mh_ex that WILL be loaded (the pack's, or the stock one) carry a real
// 7-button main-menu background? 1 = yes (MENUBCK2 differs from MENUBCK1), 0 = no (MENUBCK2 is
// a byte copy of the 6-button MENUBCK1, as in the retail RU pack, or is absent), -1 = could not
// tell (pack unreadable). Read from the .nam/.rsr on disk once and cached; costs two ~250 KB reads
// only when the entries' sizes match.
extern "C" int MH_LangPack_MenuHas7ButtonArt(void);
