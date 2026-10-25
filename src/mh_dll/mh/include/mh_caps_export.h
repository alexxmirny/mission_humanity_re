#pragma once
//
// patching:PT-CAP8 (v0.2.2 cap raise) -- the boot arm of the relocated roster arrays.
//
// MH_Caps_Install applies the two cap manifests (src/patcher/cap_v022_{reloc,limits}_EN.mh.patch.json,
// compiled into mh/patch/manifest_cap_v022_*.gen.h) to the loaded image, fills the mh::caps geometry
// table from the placement, and installs the two canonical-mirror hooks (gather at the planet-save
// entry, scatter after the planet-load array blocks). The ARRAYS are relocated at every boot;
// `[caps] raise` (a PT-SET1 host-synced row, applied through seams/gameplay_hooks.cpp) only flips the
// 51 limit sites at a quiescent point. Spec: docs/inmem-patching.md "The v0.2.2 cap layers".
//
//   [caps] relocate=0|1      dev kill-switch (default 1): 0 = stock geometry, raise refused
//   [caps] mirror_check=0|1  dev (default 0): report a writer that touched a stock address
//   [caps] raise=0|1         host-synced limit layer (default 0)
//   [patch] dump=<path>      with `[patch] inmem` unset: after the gameplay settings apply, write the
//                            parity dump of the reloc manifest to <path> and the limit manifest to
//                            <path>.limits (tools/check_inmem_patch_parity.py); dump_exit=1 then exits
//
// Returns MH_CAPS_* bits. 0 = not armed (not the EN build / refused); the log line says which.
#ifdef __cplusplus
extern "C" {
#endif

#define MH_CAPS_RELOCATED       1  // the reloc manifest was applied and the geometry table points at the wide arrays
#define MH_CAPS_LIMITS          2  // the limit layer is staged (set_limits can flip it)
#define MH_CAPS_UNITS_RELOCATED 4  // PT-CAP9-U1: the units + soldiers manifest was applied (`.mhcapu`)
#define MH_CAPS_UNITS_LIMITS    8  // ... and its limit layer is staged (set_limits flips it with the v0.2.2 one)
#define MH_CAPS_AI              16 // PT-CAP11: the AI scratch layer is staged (it flips with the limit layer; `[caps] ai_scratch=0` skips it)
#define MH_CAPS_PATH            32 // PT-CAP9-U4: the path-slot pool was relocated into `.mhcapp` (it comes with the units manifest)
#define MH_CAPS_ORDERS          64 // PT-CAP9-U5: the order layer (queue / pending / staging arrays + quota) is staged (`[caps] orders_layer=0` skips it)
int MH_Caps_Install(const char *ini_path);

// Written right AFTER the gameplay settings apply, so a `raise=1` lane dumps the ON image.
void MH_Caps_DumpIfAsked(const char *ini_path);

// PT-CAP9-U8b: after the session-close revert, with `[caps] post_match_dump=1`: the toggle layers' live sites -> <MH_LOG_ROOT>\caps_post_match_<pid>.<layer>.
void MH_Caps_DumpPostMatch(void);

// ---- host rows for mh_harness.dll (dev instrument; the DLL is separate, so it cannot link mh::caps) ----
//
// The stock .bss extents are the canonical MIRROR of the wide arrays (D1). The harness hashes the mirror
// (hash_base) and its fixtures READ it; so it must refresh it before the hash (MH_Caps_CanonGather,
// first statement of on_sim_step) and re-publish it after any import that wrote it
// (MH_Caps_CanonScatter). A fixture that WRITES a roster record must write the LIVE record instead --
// MH_Caps_Rec returns &array[player][idx] in the wide array (or the stock one when not relocated). PT-CAP9-U7d: arr = MH_CAPS_IDX_PATH_SLOT (16) returns the path
// pool's slot `idx` (0..500) of `player` -- 600 waypoint bytes, null unless the pool is relocated (the order arrays are reached through MH_Caps_OrderGeom).
#define MH_CAPS_IDX_PATH_SLOT 16u
// `arr` is the MH_CAPS_IDX_* value of caps/caps.h.
void  MH_Caps_CanonGather(void);
void  MH_Caps_CanonScatter(void);
void *MH_Caps_Rec(unsigned arr, unsigned player, unsigned idx);
// PT-CAP9-U5: the order arrays the GAME is using right now. which: 0 queue, 1 pending, 2 staging. Returns the record capacity (the stock
// bound while the order layer is OFF, the raised one once it is ON; 0 for a bad `which`) and stores the address of record [0] in *base.
// The count variables never move (queue 0x5d0194, staging 0x5d01a4, pending 0x5d01a8).
unsigned MH_Caps_OrderGeom(unsigned which, unsigned *base);

// PT-CAP8 S2: the overflow extension (records past the vanilla window; mh/caps/ovf.h) as the harness sees it.
// MH_Caps_OvfSums: the slice sums (21 since PT-CAP9-U7d: 11 roster + 10 path / order-queue; pass room for 32; the parameter keeps its old name so the harness contract is unchanged) recomputed FROM SCRATCH over the live wide arrays (not the per-step tracker's -- an
// independent witness); zeros when the arrays are not relocated. MH_Caps_OvfDigest: FNV-1a-64 over the same bytes (0 when not
// relocated) -- the save round-trip and the poke fixture compare it before/after.
void               MH_Caps_OvfSums(unsigned long long *out16);
unsigned long long MH_Caps_OvfDigest(void);

// PT-CAP9-U4: the path-slot pool as the harness sees it (the pool is relocated; the stock extents are only the slots-0..99 mirror).
// MH_Caps_PathCensus: units of `player` holding a path, the highest slot id held, busy flags, the game's free-slot counter.
// MH_Caps_PathLimit: usable slots now (100 / 500). MH_Caps_PathCheckNow: run the pool invariants once -> violations found (they are logged).
void     MH_Caps_PathCensus(unsigned player, unsigned *holding, unsigned *max_id, unsigned *flags_set, int *free_count);
unsigned MH_Caps_PathLimit(void);
unsigned MH_Caps_PathCheckNow(void);
unsigned MH_Caps_PathViolations(void);
// PT-CAP9-U7b: four FNV-1a-64 digests of the live path pool for the save round-trip rows: out[0] the stock window (slots 0..99 of every player: waypoints +
// busy flag), out[1] the slots past the window (100..500), out[2] the whole pool (501 slots), out[3] the full overflow digest (every id included).
void MH_Caps_PathDigests(unsigned long long *out4);

#ifdef __cplusplus
}
#endif
