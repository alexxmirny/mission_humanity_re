/*
 * libmh.h -- the C export surface of libmh (LIB0's enumeration; the endgame plan).
 *
 * STATUS: DECLARED, NOT YET IMPLEMENTED. This header is the enumerated contract the standalone
 * builds toward -- each entry names the tracker item that lands its implementation, and nothing
 * here is exported by the current libmh.lib yet (zero extern-C symbols existed in the modules when
 * LIB0 opened, deliberately: the hosted configuration reaches the modules through mh_export.gen.h
 * entry thunks, not through this surface). Treat additions/renames here as ABI design decisions:
 * record them, don't drift them.
 *
 * SHAPE RATIONALE (D-E3): the surface is a small C89-compatible facade over the C++ modules --
 * everything crossing it is POD, buffers, or opaque handles, so a Godot GDExtension, the LIB-REF
 * headless host, and mh.exe's thin shim can all bind it without C++ ABI coupling.
 */

#ifndef LIBMH_H
#define LIBMH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versioning ------------------------------------------------------------------------- */

/* Bumped on any breaking change to this header. The implementation returns the value it was
 * compiled with; a host refuses a mismatch. */
#define LIBMH_ABI_VERSION 1u
uint32_t libmh_abi_version(void);

/* ---- the state ABI (SB-BIND) ------------------------------------------------------------ */

/* D4: the HOST tells libmh where the state is. One entry per top-level registry region; inside
 * mh.exe the host binds the stock .bss addresses (nothing moves), a standalone host binds memory
 * it allocated. `count` forwards the runtime capacity so compile-time caps stop being load-bearing
 * (the 500-buildings composition defect). The region ids and the authoritative table layout are
 * generated from tools/data/state_regions.json -- SB-BIND owns finalizing this struct. */
typedef struct libmh_region_bind {
    uint32_t region_id; /* generated id, addr/mh_regions.gen.h */
    void    *base;      /* where this region's bytes live in the host */
    uint32_t size;      /* bytes -- the region's REACH, not what its symbol measures (see below) */
    uint32_t count;     /* element count where the region is an array; 0 = not an array */
} libmh_region_bind;

/* How many regions the table has. A host sizes its buffer with this; it is RID_COUNT. */
size_t libmh_region_count(void); /* SB-BIND */

/* The stock, in-binary answer: base = the .bss address the binary itself uses, size = the region's
 * REACH (the furthest byte any manifest touches, which for 11 regions exceeds what the symbol
 * measures -- a save block overrunning its symbol's tail is resolved against reach, so a host that
 * allocated only `size` bytes would be handed a null on load). `count` is 0 throughout: the registry
 * carries no element stride, so the stock table declares no capacity and the compile-time caps stand.
 *
 * Writes min(cap, libmh_region_count()) entries and returns the FULL count, so passing cap 0 is the
 * legal way to ask how big a buffer to allocate. This is what mh.exe's host binds -- unchanged, it
 * makes libmh_bind_regions a no-op, which is the property the hosted arm rests on.
 *
 * IN THE STANDALONE LIBRARY THIS RETURNS 0 AND WRITES NOTHING, and that is part of the contract
 * rather than a failure (LIB-REF-SPLIT). The "stock, in-binary answer" is the one answer a build
 * with no binary cannot give: nothing is mapped at those addresses and the stock base column is not
 * carried at all. Returning the table with zeroed bases would look like an answer and be 845
 * pointers to address 0, so the refusal is explicit.
 *
 * It composes with the cap-0 idiom rather than colliding with it: a host asking "how big a buffer?"
 * gets 0 and learns there is no default table from the same call it would have used to size one.
 * 0 cannot be read as "no regions" -- libmh_region_count() is unchanged and still reports the full
 * count. A standalone host must answer for every region itself via libmh_bind_regions, and
 * libmh_bound_count() stays below libmh_region_count() until it has. */
size_t libmh_default_binds(libmh_region_bind *out, size_t cap); /* SB-BIND */

/* Bind the host's answer for `n` regions. Returns the number bound (>= 0), or:
 *   -1  `binds` is null and n != 0
 *   -2  an entry names a region_id >= libmh_region_count()
 *   -3  an entry has a null base with a non-zero size
 * On any error NOTHING is bound -- the table is validated in full before the first write, so a
 * rejected call cannot leave the registry half-answered. */
int libmh_bind_regions(const libmh_region_bind *binds, size_t n); /* SB-BIND */

/* How many regions have been answered for. The hosted arm asserts this reaches
 * libmh_region_count() at init; a standalone host uses it the same way. */
int libmh_bound_count(void); /* SB-BIND */

/* ---- host callbacks (LIB-ABI) ----------------------------------------------------------- */

/* The residual outward calls become TWO versioned tables the host fills at init
 * (LIB-IFACE-SPLIT, 2026-09-10): the SIM table (libmh_host_api -- the follow-on abstraction
 * target) and the TACT table (libmh_tact_host_api -- frozen with its mode). Membership is
 * GENERATED from the adjudication ledger (tools/data/libmh_call_ledger.json) plus the
 * accessor-site scan, and an entry both sides call appears in BOTH tables -- duplication is
 * the design, so nothing is assigned exclusively and a host may bind a shared entry
 * differently per table. mh.dll implements both thinly over the mh::call:: thunks, the
 * headless host no-ops the notify class. The real structs + version constants + entry
 * metadata live in the GENERATED libmh_host_api.gen.h / libmh_tact_host_api.gen.h
 * (tools/gen_libmh_hostapi.py); this header keeps only the forward declarations so
 * including libmh.h never drags the tables in. */
typedef struct libmh_host_api      libmh_host_api;
typedef struct libmh_tact_host_api libmh_tact_host_api;
/* 0 ok; -1 version mismatch; -2 null table. A failure keeps the previously bound table.
 * One handshake per table (LIBMH_HOST_API_VERSION / LIBMH_TACT_HOST_API_VERSION). */
int libmh_set_host_api(const libmh_host_api *api, uint32_t api_version);           /* LIB-ABI */
int libmh_set_tact_host_api(const libmh_tact_host_api *api, uint32_t api_version); /* LIB-ABI */
/* Startup diagnostic: calls on_unbound(name) per NULL entry (may be null), returns the NULL
 * count, or -1 if no table is bound. The arm-time check requires BOTH to report zero. */
int libmh_host_api_unbound(void (*on_unbound)(const char *name));      /* LIB-ABI */
int libmh_tact_host_api_unbound(void (*on_unbound)(const char *name)); /* LIB-ABI */

/* The sim table's `vfs_open` mode (SIMABI-VFS). The io group used to hand the host a C-runtime
 * mode STRING because the original thunk took one; across every libmh site that string was only
 * ever "rb" or "wb", so the entry takes this enum instead and mh.dll's binder synthesises the
 * spelling. Two values, and there is no third: a host implements exactly these. */
#define MH_VFS_READ  0 /* open an existing file for reading; 0 handle if it is not there  */
#define MH_VFS_WRITE 1 /* create/truncate for writing                                     */

/* ---- the floating-point contract (CRT-X87-CPP) ------------------------------------------ */

/* LIBMH GUARANTEES x87 PRECISION CONTROL = 53 BITS, and it INSTALLS it rather than inheriting it.
 *
 * This is an ABI guarantee, not an implementation detail, because libmh's own arithmetic depends on
 * it. Eleven of the hoisted x87 helpers are C++ rather than assembly, and three of those are WRONG
 * at PC=64: `trunc_mul` differs from the original on 1743 of 4254 swept inputs there (the k * (1/k)
 * family lands exactly halfway below 1.0, so a 53-bit product rounds up and truncates to 1 while a
 * 64-bit one stays below and truncates to 0). At PC=53 all eleven are bit-identical to the assembly
 * they replaced -- `net_selftest fptest` re-derives that every run, at BOTH settings, so the
 * dependency is measured continuously rather than recorded once.
 *
 * WHY INSTALL RATHER THAN INHERIT. Inside mh.exe the word is already 53 on every measured path --
 * with the determinism harness's pin on, with it explicitly off, and on both peers across 8000
 * steps. A full disassembly of mh.exe says why nothing disturbs it: the image contains no
 * instruction that installs a control word at all (every FLDCW takes a stack slot -- save then
 * restore -- `_control87` has zero callers, and Watcom's `__init_80x87` is dead code). So the 53 is
 * supplied by the HOST TOOLCHAIN, not by the game -- which is precisely why a standalone host on
 * another toolchain or OS cannot be assumed to supply it.
 *
 * Idempotent; safe to call more than once. Sets ONLY the precision field -- rounding mode and the
 * exception masks stay as the host left them, since libmh guarantees a precision, not a whole FP
 * environment. Returns 1 on success, 0 if the control word could not be set.
 *
 * A host need not call this: the standalone build installs the guarantee from `libmh_set_host_api`,
 * the first entry any host binds through. The explicit entry exists for a host that wants the
 * guarantee in force before that, and so the obligation is visible in the contract rather than
 * buried in an initialiser. Inside mh.exe the install is deliberately NOT performed -- the process
 * already satisfies the guarantee, and the hosted arm must not change behaviour. */
int libmh_install_fp_precision(void); /* CRT-X87-CPP */

/* ---- boot: producing a starting world (LIB-BOOT / LIB-WORLD) ---------------------------- */

/* Import the post-cfg prototype snapshot (LIB-BOOT) or a full step-0 world blob (LIB-WORLD --
 * prototype tables + map planes + rosters, captured alongside an order+clock recording). The blob
 * schema is derived from the region registry; cfg parsing and map-file loading stay excluded
 * pre-fork (the endgame plan D-E5). */
int libmh_import_snapshot(const void *blob, size_t n); /* LIB-BOOT */
int libmh_import_world(const void *blob, size_t n);    /* LIB-WORLD */

/* ---- session parameters the host supplies once (LIFT-TABLE S5) --------------------------- */

/* The wall-clock seconds the strategic RNG seeds from at planet-session begin.
 *
 * This is an INIT PARAMETER rather than a host callback, and the difference is the point. The
 * original reads it itself -- llm_strat_rng_seed_wallclock_seconds @0x00499dc8 is time() +
 * _localtime(), returning tm_sec -- twice, both from llm_strat_planet_session_begin, seeding RNG
 * channels 0 and 1. Leaving that as a pulled host service means every peer in a multiplayer session
 * seeds from ITS OWN clock, which is a divergence hazard the ABI can simply not have: a host that
 * gives both peers the same number cannot desync here. So the value is pushed once and libmh reads
 * its own slot.
 *
 * TWO CONSEQUENCES, both deliberate and both declared. (1) The value is sampled when the host
 * chooses, not at session begin, so a long-running host seeds from an older second than the
 * original would have. (2) The original's two draws are two SEPARATE reads of tm_sec; one slot
 * makes them one value. They agree except across a second boundary, and agreeing is the property
 * the multiplayer fix wants.
 *
 * Defaults to 0 until the host sets it. mh.dll pushes GetLocalTime().wSecond at bind time -- NOT
 * the original function, which is time() + _localtime() and runs in mh.exe's Watcom CRT: the bind
 * happens in DllMain, before that CRT is initialised, and calling it there killed every boot in the
 * UI suite at once. The harness `pin_strat_seed` value is pushed directly beside its own arm
 * instead, which is a better place for it than a trampoline over the original entry. */
void libmh_set_session_seed(int32_t wallclock_seconds); /* LIFT-TABLE */

/* ---- the deterministic spine (LIB-REF drives these) ------------------------------------- */

/* Feed one wire-format order record into the pending set (the ST5 codec is the one shape the wire
 * and the save already share -- the replay file stores exactly these bytes). */
int libmh_submit_order(const void *record, size_t n); /* LIB-REF */

/* Advance the strategic sim one step (the llm_strat_sim_step closure, ours since SIM1). */
void libmh_sim_step(void); /* LIB-REF */

/* The lockstep state hash over the bound regions -- the same derivation as the in-binary
 * determinism harness, which is what makes the standalone replay comparable to a rig run. */
uint64_t libmh_state_hash(void); /* LIB-REF */

/* ---- save/load (RI-SAVE, already ours in-binary) ---------------------------------------- */

/* Byte-compatible with the original save format -- the frozen side's permanent guarantee
 * (LIB-FORK). Buffer-based here; file IO belongs to the host. */
int libmh_save_planet(void *buf, size_t cap, size_t *out_len); /* LIB-REF */
int libmh_load_planet(const void *buf, size_t n);              /* LIB-REF */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBMH_H */
