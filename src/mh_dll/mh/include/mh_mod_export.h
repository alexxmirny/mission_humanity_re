//
// include/mh_mod_export.h -- mods:DATAEXT1: the loose-file resource overlay.
//
// ---- WHAT THE OVERLAY IS ------------------------------------------------------------------------
//
// `[mods] enabled=<id>[,<id>...]` in mh_net.ini (the launcher's canonical comma list, later id
// wins, next-boot only) mounts
//
//     <exe dir>\mods\<id>\mod.ini                 -- metadata (the catalog's; diagnosed, not served)
//     <exe dir>\mods\<id>\data\<game-relative>    -- resource payloads: actual byte copies
//
// as an immutable in-memory snapshot (mh::mods, mh_common/include/mh_mod_catalog.h) that SHADOWS
// the loaded resource packs. A name the snapshot holds is answered at the two public resource
// entry points themselves:
//
//   0x004cf069  GetResourseFilePtr   -> a fresh utils_malloc COPY of the snapshot bytes
//   0x004cf15e  rsr_GetFileRealSize  -> the same winning payload's byte length
//
// both detoured with the hook toolkit's 8-byte-stolen trampoline (mh::hook::install_trampoline),
// whose thunk runs the displaced Watcom prologue and jumps back to entry+8 -- the original CALL
// still executes in place -- so a miss is byte-for-byte retail. Every retail caller frees the
// returned pointer with utils_free (the game-heap contract seams/hostapi_io_bind.cpp documents),
// so the overlay pays one game-heap copy per load and the immutable snapshot bytes are never
// handed out. Pointer lookup and size lookup resolve the SAME winner, so the game's
// GetResourseFilePtr + rsr_GetFileRealSize idiom stays self-consistent (font end pointers,
// mem_realloc sizes, asset_read).
//
// ---- WHY NOT AN EXTRA PACK (the front-pack candidate, examined and rejected) ---------------------
//
// Registering a synthetic descriptor at the front of G_RSR_FILE_PTRS would serve the names
// natively -- but llm_rsr_close_all_files (0x004cef4a) is the shutdown cleanup, and the loop is
// verified in the disassembly: the descriptor pointer lands in [ebp-0x1c] ONCE, the counter
// decrements, and EVERY iteration closes + frees the SAME descriptor (0x004cef73..97; no advance).
// An extra descriptor is therefore a multiple close/free or a leak depending on where it sits --
// the front-pack lead is real, statically. An injected pack must additionally allocate its entry
// array from the GAME's heap (utils_free runs on it), present a file_handler utils_close_file
// tolerates, and serve loose files through a synthesized on-disk .rsr (pack entries are read by
// file offset from the handler) -- a pack writer, a derived cache with its own lifetime, and the
// game's own pack-array growth to compose with. The overlay touches no pack state at all; stock
// cleanup runs unchanged.
//
// ---- BOOT TIMING --------------------------------------------------------------------------------
//
// ONE 5-byte splice, installed from MH_Seam_Init beside MH_LangPack_Install (DllMain, loader lock
// held, no I/O beyond the ini read lang_pack already makes there) -- but ONLY when `[mods]
// enabled` names something:
//
//   0x004c3dbf  E8 9F F7 FF FF   call rsr_TryReadRsrFile   (llm_boot_progress_draw, "mh_ex")
//
// That call is the FIRST pack registration of the process (G_RSR_FILE_PTRS is still empty). The
// stub it jumps to runs the heavy arm -- the two detour installs (inert until publish), the pure
// catalog prepare, map/save readiness, then a single snapshot publication -- AFTER DllMain has returned and BEFORE any
// pack exists: post-loader-lock, pre-first-pack. With no `[mods]` key the splice is never written
// and the boot is byte-identical to stock.
//
// Log channel: mh_video.log, `; [mods] ` tags (the advisory mh.dll-owned GFX/resource channel --
// NOT mh_net.log, whose arm-window ORDER is gated by tools/check_arm_order.py; a stock run writes
// no new line at all).
//
#pragma once

#include <stdint.h>

// Installed from MH_Seam_Init: read `[mods] enabled` and, when it names ids, write the ONE boot
// splice at 0x004c3dbf (the first-pack call). Returns 1 if the splice is in, 0 for stock
// (unset / refused bytes / not the EN exe). Idempotent. The heavy arm runs later, from the splice.
extern "C" int MH_Mods_Boot_Splice_Install(void);

// 1 while the overlay is mounted AND live (the boot arm finished with publish), 0 for stock. A
// refusal leaves 0 and the boot stock; a pre-publish window (inside the boot stub) also reads 0,
// where the installed detours are already transparent.
extern "C" int MH_Mods_Mounted(void);

// Resolve a game-relative resource name (any case, '/' or '\\' separators) against the mounted
// snapshot. Hit: *out_bytes = the IMMUTABLE snapshot bytes (read-only; never freed by the caller),
// *out_size = the exact byte length -- the SAME value rsr_GetFileRealSize answers -- and 1 is
// returned. Miss (or nothing mounted): 0, outs untouched. THE COVERAGE ORACLE for every "do we
// hold X" decision (map picker, preview, transfer, JOIN admission): rsr_GetFileEntry cannot see
// this overlay, and claimed content hashes must come from MH_Mods_ContentDigest, never from pack
// entries.
extern "C" int MH_Mods_Resource_Find(const char *game_rel_name, const uint8_t **out_bytes,
                                     uint32_t *out_size);

// The MP content identity: copies the MOUNTED snapshot's version-framed SHA-256
// (mh::mods::mounted_snapshot()->digest) into out[32]. Before the first mount -- stock boot, or a
// boot refused before publish -- it copies mh::mods::nomod_digest(): the canonical empty-set
// digest. There is no bypass value and no third answer; a stock run and a refused run hash alike.
extern "C" void MH_Mods_ContentDigest(unsigned char out[32]);
