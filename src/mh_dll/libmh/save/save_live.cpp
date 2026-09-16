//
// save/save_live.cpp -- see save_live.h. The live bindings, the promoted body, and the in-call A/B.
//
// The body below is a translation of SavePlanetToDisk 0x00447bb3, and the comments are about what the
// disassembly does that a reasonable-looking reimplementation would not. The 42 block writes are NOT
// here: they are table::PLANET, walked by mh::save::save_planet. What is here is everything around
// them, which is where the surprises were.
//
#include "save/save_live.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h" // the rebound pool_reset
#include "addr/mh_export.gen.h"
#include "addr/mh_regions.gen.h" // ST1: every block address is checked against the region registry
#include "save/save_driver.h"
#include "save/save_state.h"      // SB-BIND T5: the live arm's non-block addresses
#include "state/hook_api.h"       // F4D-PRE: the host's hook table -- verify mode's trampoline install
#include "state/region_runtime.h" // ST2: a stock address -> where those bytes are now

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h"
#include "state/rebind_targets.gen.h"

namespace mh::save {
namespace {

void (*g_log)(const char *) = nullptr;

void say(const char *fmt, ...) {
    if (!g_log) return;
    char    line[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(line);
}

bool     g_installed   = false;
int      g_verify_mode = 0;       // 0 = off, 1 = ours vs the original, 2 = the original vs itself (control)
void    *g_tramp       = nullptr; // the original's first 8 bytes + a jmp back; verify modes only
uint32_t g_verify_poke = 0;       // mutation check: flip this byte of the first file before comparing
long     g_calls       = 0;
// The LOAD root's own set. Two trampolines, not one: sites hook the callee ENTRY, and these are two
// different entries with two different bodies.
bool     g_load_installed = false;
void    *g_load_tramp     = nullptr;
long     g_load_calls     = 0;
uint32_t g_load_poke      = 0; // mutation check: corrupt this PLANET-table step's hash between arms
// And the CONTAINER roots'. TWO of them, and each has its own key, trampoline and call counter for
// the same reason the per-planet pair does: they are two entries with two bodies and two oracles,
// independently rollback-able. (`container` was for a while the only one, on the strength of a
// flags-passing tail that turned out to be dead code -- see run_ours_load_container.)
bool     g_container_installed        = false;
void    *g_container_tramp            = nullptr;
long     g_container_calls            = 0;
uint32_t g_container_poke             = 0; // mutation check: flip this byte of the first .sav before comparing
bool     g_container_load_installed   = false;
void    *g_container_load_tramp       = nullptr;
long     g_container_load_calls       = 0;
uint32_t g_container_load_poke        = 0; // mutation: corrupt this CONTAINER-table step's hash
uint32_t g_container_load_member_poke = 0; // mutation: flip byte (n-1) of the first extracted member

// ---------------------------------------------------------------------------------------------
// THE THREE INJECTIONS, RESOLVED.

// SIMABI-VFS: `vfs_write(h, src, n) -> bytes`. The original's fwrite shape -- (data, size, count=1,
// handle) returning the ITEM count, so 1 meant "all n bytes went" -- was the THUNK's, and it now
// lives in mh.dll's binder. What crosses the host boundary is a byte count, the only figure a host
// implementor can produce without being told why the count was 1. `block_io::write` still promises
// 1-on-success, so the conversion happens here, at the one place that ever cared.
int live_write(void *ctx, const void *data, uint32_t size) {
    return mh::host().vfs_write(static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx)), data, size) ==
                   static_cast<int32_t>(size)
               ? 1
               : 0;
}

// read_from_file 0x004cfd58 is fread(dst, elem_size, count, file) in EAX/EDX/EBX/ECX, and its tail
// (0x004cff48 XOR EDX,EDX / MOV EAX,[ESP+8] / DIV ESI) returns bytes_read / elem_size -- the ITEM
// count. Its prototype was committed at SV1-P-LOAD L1; before that the generated wrapper returned
// void and this had to be a refusing stub.
//
// ELEM_SIZE 1, COUNT n -- NOT the original's (n, 1), and the difference is deliberate. Both read the
// same bytes; they differ only in what comes back, and `block_io::read` is specified to return a BYTE
// count. With the original's shape a short read returns 0 and the byte count is unrecoverable. The
// original does not care -- it discards the result (the dead store at 0x00448764) -- but
// `block_workspace::strict_reads`, the opt-in truncation check, needs the real figure. So this is a
// strictly-more-informative call of the same function, not a behaviour change.
//
// SIMABI-VFS: that constant 1 was the last thing this line said about the C runtime, and it is the
// binder's now. `vfs_read` returns bytes by contract -- "a short read must be REPORTED, not padded"
// is the entry's own sentence, and `strict_reads` is what reads it.
int live_read(void *ctx, void *dst, uint32_t size) {
    return mh::host().vfs_read(static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx)), dst, size);
}

// A game address IS a pointer in the game's process. The savetest resolver's sparse slab lookup
// exists only because the test has no such process.
//
// RI-STATE / ST1: the identity is now CHECKED against the state region registry first. Every block
// the table names must fall inside a registered region -- which is a statement about the save format
// and the registry agreeing, and it is the check whose absence let the save driver walk addresses no
// other consumer of those bytes had ever heard of. It cannot fire in this build (the generator's
// lint proves every block is covered before the header is written, so a miss is a BUILD failure);
// the runtime arm exists so that the day a region becomes DLL-owned and is not re-registered, the
// driver refuses rather than serialising whatever is still at the old address. Returning null is
// already a hard failure in the driver, not a skipped block -- see state_io in save_driver.h.
// A game address IS a pointer inside the game's process, so this is the identity -- and it must be
// TOTAL. It briefly was not: ST1 added a `mh::state::covering(addr,size)` guard here, which turned
// the resolver partial and broke the promoted save outright. Found 2026-07-31 by ST4's [save] verify
// A/B, the first one run after ST1 landed: ours wrote 0 bytes and logged
// "IMPOSSIBLE: current_progress_slice failed under the identity resolver".
//
// WHY THE GUARD WAS WRONG rather than merely unlucky. It was meant to catch a BLOCK address no
// region describes -- but every block is already checked AT COMPILE TIME by all_blocks_registered()
// in save_driver.cpp, over both tables. What the runtime guard added was coverage of reads that are
// NOT blocks: the format's FRAMING reads. `PROGRESS_SLICE_WORD` (0x00e16305) is one -- a 2-byte read
// whose value SIZES the progress blocks -- and it is not a block address, so no manifest claims it
// and no region covers it. Same for the member predicate's four reads.
//
// The lesson is the shape, not the address: a check derived from "what the block table names" cannot
// be applied to "everything the driver reads", because the driver also reads the numbers that
// describe the blocks. The compile-time assert is the right home for that rule, and it already is
// its home.
// ST2 CHANGES THE NAME'S MEANING, NOT ITS TOTALITY. It is still the identity for every address in
// this build -- `translate` returns `addr` unchanged unless the covering region has been REBASED,
// which nothing does yet -- but it is now the identity BY DERIVATION rather than by assumption, so
// the day orders (or the AI island) allocates its region somewhere else, the save driver reads the
// bytes it moved to instead of the .bss it left. The one null it can return is unreachable until
// that day and is documented at `translate`; the driver already treats null as a hard failure.
} // namespace

void *identity_resolve(void *, uint32_t addr, uint32_t size) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: THIS RESOLVER TAKES A STOCK ADDRESS, which is the one input a standalone
    // artifact cannot be handed -- there is no image, and mh::state::translate() is not declared
    // there precisely so that asking is a compile error rather than an answer about address zero.
    //
    // It is NOT re-keyed to a rid, because its whole role is the addresses that have no rid: the
    // framing word that SIZES the progress blocks and the member predicate's four reads, none of
    // which any region claims (that is why the ST1 covering() guard here had to be reverted -- see
    // the TOTAL note above). The BLOCK path, which does have a rid for every step, goes through
    // region_resolve() below and works in both configurations.
    //
    // So: refuse. A standalone save is a rid-keyed walk or it is nothing; nullptr is the driver's
    // existing hard-failure value and it already treats it as one.
    (void)addr;
    (void)size;
    return nullptr;
#else
    return mh::state::translate(addr, size);
#endif
}

// SB-HOSTFREE: one RUN of a decomposed block, by region id.
//
// identity_resolve cannot answer this and must not be asked to. It goes through covering(), which
// matches on `reach` and returns the FIRST region in base order containing the window -- and the
// reason a block needs decomposing at all is that an overrunning region's reach swallows its
// neighbours, so a run inside _G_LLM_GAME_SESSION_MODE's 1623-byte window would resolve to
// SESSION_MODE instead of to the region that owns those bytes, which is precisely the bug the
// decomposition exists to fix. The rid comes from the generated table, where the answer is known.
void *region_resolve(void *, uint16_t rid, uint32_t off, uint32_t len) {
    const auto r = static_cast<mh::state::region_id>(rid);
    if (rid >= mh::state::RID_COUNT) return nullptr;
    // The run must fit inside what the host actually bound, or we would hand back a pointer past
    // the end of the arena -- the failure `translate` refuses for whole blocks, applied per run.
    if (static_cast<uint64_t>(off) + len > mh::state::live_size(r)) return nullptr;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(mh::state::live_base(r) + off));
}

namespace {

// THE ENCODER STATE CANNOT BIND TO THE GAME'S GLOBALS. The original keeps chain POINTERS at
// 0x006776b7/0x0067b6b7; ours keeps index+1 so that node 0 is distinguishable from empty
// (mh_lzw.h). Different layouts, so this is ours -- which is safe because compress() clears the
// chain tables on entry, i.e. the state is per-call and nothing carries over between blocks.
lzw::encoder_state g_encoder;

// And the decoder, for the same reason. `live_workspace` used to leave this null with the comment
// "write side only", which was true until the load root was promoted -- at which point read_block
// dereferenced it on the first block and took the process with it. It cost one rig run; the run's
// own log is what localised it (the promoted body logged call #1 and the step counter stopped dead
// on that step). Both codec states are per-call: compress clears the chain tables on entry and
// decompress re-seeds the dictionary from the payload, so nothing carries between blocks.
lzw::decoder_state g_decoder;

block_workspace live_workspace() {
    block_workspace ws{};
    // BOUND TO THE ORIGINAL'S OWN BUFFER, deliberately. save_block.cpp warns that this reintroduces
    // the original's 8-byte overrun for a block at exactly BLOCK_READ_CAP, because G_LZW_TEMP_DATA is
    // exactly that many bytes with the payload at +8. Accepted: ours bounds the encoder at
    // BLOCK_READ_CAP where the original bounds it at NOTHING, so the promoted path is strictly safer
    // than the code it replaces, and the largest block in a real save measures 263574.
    ws.staging             = binds().lzw_staging;
    ws.max_compressed_seen = binds().lzw_max_compressed;
    ws.encoder             = &g_encoder;
    ws.decoder             = &g_decoder;
    // NOT the faithful setting, and off for that reason -- see save_block.h. The original cannot
    // detect a truncated file at all (it discards the read count), so turning this on would change
    // WHEN a load fails, which is a behaviour change and belongs in its own item with its own
    // evidence rather than riding in on a promotion.
    ws.strict_reads = false;
    return ws;
}

// ---------------------------------------------------------------------------------------------
// THE TWO DELEGATED STEPS. save_driver.h's write_delegate comment carries the argument for each.

int delegate_regions(void *ctx) {
    // void __watcall llm_map_save_regions(file_handle) -- no result to report, and the original does
    // not accumulate one either (0x0044804a is followed straight by the next block set-up).
    mh::host().map_save_regions(static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx)));
    return 0;
}

int delegate_player_data(void *ctx) {
    // void __watcall llm_game_save_player_data(save_file). Its return is likewise not accumulated by
    // the original (0x004480cd is followed by a reload of the handle, not an ADD).
    mh::host().save_player_data(ctx);
    return 0;
}

// `at` is the write_at of the run's first step; see save_table.gen.h PLANET.
constexpr step_delegate DELEGATES[] = {
    {0x00000000u, 0, delegate_regions},     // STEP_REGIONS
    {0x004dda81u, 2, delegate_player_data}, // the AI player count + the 1329120-byte block after it
};

// ---------------------------------------------------------------------------------------------
// THE LOAD SIDE'S DELEGATES -- a DIFFERENT set, not the same one read backwards. save_driver.h's
// step_delegate comment carries the per-callee measurement; the short version is that both helper
// pairs split the opposite way from their save-side twins.

int delegate_regions_read(void *ctx) {
    // TWO calls here where the save side has one (0x00448545 / 0x0044854d). The pool reset must come
    // first: llm_map_load_regions mallocs a fresh node list and pushes onto the head of 0x0051de7c,
    // so without the reset it would prepend to the previous planet's graph.
    mh::sim::pool_reset(); // REBOUND 2026-09-02: translated (LT1C), ours binds directly
    mh::host().map_load_regions(static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx)));
    return 0;
}

int delegate_session_init(void *) {
    // EMITS NO BLOCKS -- `covers: 0`. The original runs it at 0x004485cd, after the progress loop and
    // before FUN_004ddaa5's two reads, so it is keyed to the first of those (read_at 0x004ddac0) and
    // that step is then read normally. It takes no arguments and returns nothing.
    MH_LIBMH_BIND(llm_strat_planet_map_session_init)();
    return 0;
}

int delegate_bldg_panel_reset(void *) {
    // LIB-ABI stage E (2026-09-03), the INVERTED split: this step used to delegate the whole
    // llm_ui_bldg_panel_load_state (covers 2) because 0x0041c267 calls llm_ui_bldg_panel_open
    // BEFORE its two LZW block reads. A stream decode cannot take the state-in/visuals-out
    // hoist (running it at the call site AND in the callee would read four blocks where the
    // original reads two and desync every later block), so the split runs the other way: the
    // panel reset stays a host-callback entry, run here to preserve the original's
    // clear-before-decode order, and the TWO block reads (AvailableBuildings + the base-marker
    // coords, save_table rows @0x0041c279/0x0041c28e) are the driver's own from this point --
    // its lzw codec is the one every other block already goes through. No hoisted clears here:
    // both reads cover their regions in full, so the decode overwrites anything the reset (or a
    // no-op host arm's absence of one) left behind.
    mh::state::evt::bldg_panel_open();
    return 0;
}

constexpr step_delegate READ_DELEGATES[] = {
    {0x00000000u, 0, delegate_regions_read},    // STEP_REGIONS -> pool_reset + load_regions
    {0x004ddac0u, 0, delegate_session_init},    // covers 0: run, then read this step normally
    {0x0041c279u, 0, delegate_bldg_panel_reset} // covers 0: panel reset, then BOTH blocks read normally
};

driver_env live_env(int32_t file_handle) {
    driver_env e{};
    // ST4: this is the process's own address space, so a module that has claimed a region may serve
    // it. See driver_env::owners_serve for why savetest must NOT set this.
    e.owners_serve        = true;
    e.io                  = block_io{live_write, live_read, reinterpret_cast<void *>(static_cast<uintptr_t>(file_handle))};
    e.ws                  = live_workspace();
    e.st                  = state_io{identity_resolve, nullptr, region_resolve};
    e.vt                  = version_table{}; // the version gate is the CONTAINER's; a planet file has no header
    e.mem                 = nullptr;         // save_planet allocates nothing
    e.stats               = nullptr;
    e.delegates           = DELEGATES;
    e.delegate_count      = static_cast<int>(sizeof(DELEGATES) / sizeof(DELEGATES[0]));
    e.read_delegates      = READ_DELEGATES;
    e.read_delegate_count = static_cast<int>(sizeof(READ_DELEGATES) / sizeof(READ_DELEGATES[0]));
    e.delegate_ctx        = reinterpret_cast<void *>(static_cast<uintptr_t>(file_handle));
    return e;
}

// ---------------------------------------------------------------------------------------------
// THE PROMOTED BODY.

constexpr uint32_t PATH_BYTES = 256; // [EBP-0x228]..[EBP-0x128], the original's own two buffers

// The three inlined two-bytes-at-a-time copy loops at 0x00447c62 / 0x00447c8f / 0x00447cbd are a
// Watcom strcpy/strcat idiom: each stops at the source NUL and never writes past it. Bounded here
// because ours is a fixed array and the original's is a stack frame -- the original would simply
// smash its own frame, which is not a behaviour worth reproducing.
void append_bounded(char *dst, uint32_t cap, const char *src) {
    uint32_t n = 0;
    while (n < cap && dst[n] != '\0') ++n;
    while (n + 1 < cap && *src != '\0') dst[n++] = *src++;
    if (n < cap) dst[n] = '\0';
}

// Builds the per-planet path exactly as 0x00447c08..0x00447cd5 does. Returns false only for a MODE
// the original leaves the filename buffer UNWRITTEN for -- see below.
bool build_path(char *path, int32_t planet, uint32_t mode) {
    char name[PATH_BYTES] = {};

    // 0x00447c08: param_2 is a MODE, not a flag. 1 and 3 select save%02d.dat, 2 selects
    // qsave%02d.dat, and ANY OTHER VALUE falls through to 0x00447c56 with the 256-byte stack buffer
    // never written -- so the original concatenates uninitialised stack into the path. That is a
    // latent defect, currently unreachable (game::SaveGame passes 3, llm_game_load passes 3), and it
    // is the one place this body deliberately does NOT reproduce the original: uninitialised stack
    // has no reimplementation. We refuse and say so instead.
    if (mode == 1 || mode == 3) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "save%02d.dat", planet);
    } else if (mode == 2) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "qsave%02d.dat", planet);
    } else {
        return false;
    }

    path[0] = '\0';
    append_bounded(path, PATH_BYTES, binds().save_dir);
    append_bounded(path, PATH_BYTES, binds().save_temp_dir);
    append_bounded(path, PATH_BYTES, name);
    return true;
}

// Everything from the entry to the RET, minus the A/B. `out_path` receives the file it wrote (empty
// when it never got that far), so the verify wrapper does not have to rebuild it.
uint32_t run_ours(uint32_t planet_index, uint32_t mode, char *out_path) {
    const int32_t planet = static_cast<int32_t>(planet_index);
    out_path[0]          = '\0';

    // 0x00447bdd: planet_time[planet] = GAME_CLOCK, an FLD/FSTP of the double. THIS IS A STATE
    // MUTATION IN A FUNCTION THAT LOOKS LIKE A WRITER -- it happens before the file is even opened,
    // so it lands even on the paths that return failure.
    binds().planet_time[planet] =
        *binds().game_clock;

    // 0x00447bef: set the status to 1 IF IT IS ZERO. Not an unconditional store -- a planet already
    // at some other status keeps it.
    int32_t *status = binds().planet_status + planet;
    if (*status == 0) *status = 1;

    char       path[PATH_BYTES];
    const bool named = build_path(path, planet, mode);

    // 0x00447cd6: the four dwords are zeroed when the mode is NOT 3 (the JZ skips the stores). Ghidra
    // sees this write as their only reference -- reproduced rather than dropped, because "the ref
    // manager found no reader" is not the same claim as "nothing reads it".
    //
    // THIS RUNS BEFORE THE REFUSAL BELOW, and the ordering is a review finding rather than taste: an
    // unrecognised mode is exactly a mode that is not 3, so the original ZEROES THESE FOUR DWORDS on
    // its way to building a garbage path. An early return on the refusal skipped them, which made the
    // draft's departure bigger than the one it declared -- it would have changed state the original
    // changes, not merely declined to reproduce an uninitialised buffer.
    if (mode != 3) {
        uint32_t *r = binds().save_mode_reset_dwords;
        r[0] = r[1] = r[2] = r[3] = 0;
    }

    if (!named) {
        say("; [save] REFUSED planet=%d mode=%u -- the original leaves its filename buffer "
            "uninitialised for this mode (0x00447c1a); not reproduced\n",
            (int)planet, (unsigned)mode);
        return 0;
    }

    // 0x00447d0f: open_file(path, "wb"). A null handle returns 0 (failure) WITHOUT closing anything.
    const int32_t fh = mh::host().vfs_open(path, MH_VFS_WRITE);
    if (fh == 0) {
        say("; [save] planet=%d mode=%u: open_file('%s') FAILED\n", (int)planet, (unsigned)mode, path);
        return 0;
    }
    std::strcpy(out_path, path);

    driver_env   env   = live_env(fh);
    uint32_t     slice = 0;
    int          rc    = 0;
    planet_image img{};
    // THE ORIGINAL HAS NO CHECK HERE -- 0x00447d1b jumps straight to the first block write -- so this
    // gate is ours, and a review flagged it as a place the draft could produce an empty file where the
    // original would not. It CANNOT fire in the live build: its only failure is `state_io::resolve`
    // returning null, and the live resolver is `identity_resolve`, a total function. Stated so a
    // future resolver that CAN fail does not silently reintroduce the gap.
    // STILL TRUE AFTER ST2, and worth spelling out because the resolver stopped being a one-liner:
    // `translate` has exactly one null path, a request running off the end of a REBASED region, and
    // no region is rebased in any shipped build. The first one that is makes this message reachable
    // -- which is the intent, not a regression: it would be reporting a real out-of-bounds.
    if (!current_progress_slice(env, &slice)) {
        say("; [save] IMPOSSIBLE: current_progress_slice failed under the identity resolver -- the "
            "live state_io is no longer total\n");
        rc = 1;
    } else {
        // save_planet checks the slice it derives against the image's, which is a real check only if
        // the image carries the value the current state implies. There is no loaded image here, so
        // stamp it -- rather than making the field optional and losing the check for every caller.
        img.progress_slice = slice;
        rc                 = save_planet(env, img);
    }

    // 0x004480e0: closed unconditionally, before the result is decided.
    mh::host().vfs_close(fh);

    // 0x004480e5: the accumulator counts FAILED writes, and `JLE` means zero-or-fewer is success --
    // so the return is 1 for success and 0 for failure, the opposite way round from the block layer.
    //
    // A STATED DEPARTURE ON THE FAILURE PATH ONLY: the original attempts EVERY block and reports the
    // total at the end, while save_planet stops at the first failure. On the success path the two are
    // identical; on the failure path both return 0 and both leave an unusable file, ours simply
    // shorter. Reproducing the accumulate-and-continue would mean writing blocks into a stream that
    // is already broken.
    return rc == 0 ? 1u : 0u;
}

// ---------------------------------------------------------------------------------------------
// THE PROMOTED LOAD BODY. LoadPlanetFromDisk 0x00448107 -- the save root's twin, plus the thing that
// makes it bigger: an APPLY PHASE after the file is closed.

// 0x004481fb: open_file(path, ReadBinary 0x500cda).
uint32_t run_ours_load(uint32_t planet_index, uint32_t mode) {
    const int32_t planet = static_cast<int32_t>(planet_index);

    char path[PATH_BYTES];
    if (!build_path(path, planet, mode)) {
        say("; [save] LOAD REFUSED planet=%d mode=%u -- filename buffer unwritten for this mode\n",
            (int)planet, (unsigned)mode);
        return 0;
    }
    const int32_t fh = mh::host().vfs_open(path, MH_VFS_READ);
    if (fh == 0) {
        say("; [save] LOAD planet=%d mode=%u: open_file('%s') FAILED\n", (int)planet, (unsigned)mode, path);
        return 0;
    }

    driver_env   env   = live_env(fh);
    uint32_t     slice = 0;
    int          rc    = 0;
    planet_image img{};
    if (!current_progress_slice(env, &slice)) {
        rc = 1;
    } else {
        rc = load_planet(env, &img);
    }
    mh::host().vfs_close(fh);
    if (rc != 0) return 0; // 0x004485ed: a nonzero failure accumulator returns 0

    // ---- THE APPLY PHASE (0x004485fc..0x0044864e). None of this is file I/O; it is the loaded
    // state being pushed into the map/render layer, and every call stays a CALL -- they are the
    // closure boundary, not part of the walk.
    //
    // 0x004485fc: llm_planet_tlo_load is SKIPPED for MODE 2 (quickload), and a ZERO return from it is
    // an early `return 0` -- the one place in this body where a callee's result decides the outcome.
    //
    // llm_planet_tlo_load was typed `void` in Ghidra until this item, so the generated wrapper had no
    // result to hand back and this early return could not be expressed at all -- the COMPILER is what
    // said so (C2186: an operand cannot have type 'void'). Committing `int` was the fix. Same class as
    // read_from_file at L1: a missing return type becomes a behaviour gap one level up.
    if (mode != 2 && mh::host().planet_tlo_load(planet_index) == 0) return 0;
    *binds().build_placement_id     = 0;
    *binds().build_preview_suppress = 0;
    MH_LIBMH_BIND(llm_map_setup_dimensions)();
    MH_LIBMH_BIND(llm_map_fog_of_war_recompute)();
    mh::state::evt::cam_set_col(*binds().cam_col);
    mh::state::evt::cam_set_row(*binds().cam_row);
    mh::state::evt::inv_planet_extra_sprite_banks(); // 0x00448649, and the body's last statement
    return 1;
}

// ---------------------------------------------------------------------------------------------
// THE LOAD ORACLE -- and it is NOT the save side's, because a load emits STATE rather than a file.
//
// The same "redirect rather than undo" trick works one level up: LOAD THE SAME FILE TWICE and compare
// the state regions the block table names. Every block reads to a FIXED address, so a second load
// overwrites the first with identical data -- which makes the comparison meaningful without any
// rollback. Run ours, hash; run the original on the same unchanged file, hash; a per-step mismatch
// names the block by the table's own label rather than by an offset.
//
// STEP_REGIONS is excluded from the hash, and deliberately: it is a malloc'd pointer graph, not a
// fixed region, and it is emitted by the ORIGINAL llm_map_load_regions in BOTH arms anyway -- the
// same division of responsibility the save A/B settled.
uint64_t fnv1a(const void *p, uint32_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (uint32_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// Hash every fixed region the PLANET table names. `out` receives one hash per step (0 for the steps
// that have no fixed extent), so a mismatch can be reported by NAME.
void hash_planet_state(uint32_t slice, uint64_t *out) {
    for (int i = 0; i < table::PLANET_STEPS; ++i) {
        const table::step &s = table::PLANET[i];
        if (s.kind == table::STEP_BLOCK) {
            out[i] = fnv1a(reinterpret_cast<const void *>(static_cast<uintptr_t>(s.addr)), s.size);
        } else if (s.kind == table::STEP_PROGRESS) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t k = 0; k < table::PROGRESS_COUNT; ++k)
                h = fnv1a(reinterpret_cast<const void *>(
                              static_cast<uintptr_t>(table::PROGRESS_BASE + k * table::PROGRESS_STRIDE)),
                          slice, h);
            out[i] = h;
        } else {
            out[i] = 0; // STEP_REGIONS -- a pointer graph, see above
        }
    }
}

uint64_t g_hash_a[table::PLANET_STEPS];
uint64_t g_hash_b[table::PLANET_STEPS];

uint32_t call_original_load(uint32_t planet_index, uint32_t mode) {
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    return mh::call::detail::s_u32_EAX_EDX(reinterpret_cast<uintptr_t>(g_load_tramp), planet_index, mode);
#else
    (void)planet_index;
    (void)mode;
    return 0; // unreachable: the trampoline is installed by the hosted detour path only
#endif
}

uint32_t load_verify_round(uint32_t planet_index, uint32_t mode) {
    uint32_t slice = 0;
    {
        // The slice comes from config, not the file, so it is the same for both arms; read it once.
        driver_env probe = live_env(0);
        if (!current_progress_slice(probe, &slice)) slice = 0;
    }

    const uint32_t rc_a = (g_verify_mode == 2) ? call_original_load(planet_index, mode)
                                               : run_ours_load(planet_index, mode);
    hash_planet_state(slice, g_hash_a);
    if (g_load_poke != 0 && g_load_poke < (uint32_t)table::PLANET_STEPS) {
        // MUTATION CHECK: corrupt one hashed step between the arms. Without it a comparator that
        // always agrees looks exactly like one that is right.
        g_hash_a[g_load_poke] ^= 0x5555555555555555ull;
        say("; [save] LOAD POKE step %u ('%s') corrupted -- this run's verdict MUST be DIVERGENT\n",
            (unsigned)g_load_poke, table::PLANET[g_load_poke].name);
    }

    const uint32_t rc_b = call_original_load(planet_index, mode);
    hash_planet_state(slice, g_hash_b);

    int diffs = 0, first = -1;
    for (int i = 0; i < table::PLANET_STEPS; ++i) {
        if (g_hash_a[i] != g_hash_b[i]) {
            ++diffs;
            if (first < 0) first = i;
        }
    }
    if (diffs == 0) {
        say("; [save] LOAD %s planet=%u mode=%u rc(A=%u B=%u): IDENTICAL over %d hashed steps "
            "(slice=%u)\n",
            g_verify_mode == 2 ? "CONTROL orig-vs-orig" : "A/B ours-vs-orig", (unsigned)planet_index,
            (unsigned)mode, (unsigned)rc_a, (unsigned)rc_b, table::PLANET_STEPS, (unsigned)slice);
    } else {
        say("; [save] LOAD %s planet=%u mode=%u rc(A=%u B=%u): DIVERGENT -- %d of %d steps differ, "
            "first '%s' (step %d, %u bytes at %08X)\n",
            g_verify_mode == 2 ? "CONTROL orig-vs-orig" : "A/B ours-vs-orig", (unsigned)planet_index,
            (unsigned)mode, (unsigned)rc_a, (unsigned)rc_b, diffs, table::PLANET_STEPS,
            table::PLANET[first].name, first, (unsigned)table::PLANET[first].size,
            (unsigned)table::PLANET[first].addr);
    }
    // The state left behind is the ORIGINAL's second load, so the session continues exactly as an
    // unpromoted run would whatever our body decided.
    return rc_b;
}

// LIB-REBIND: named from another TU by the generated binder, so this one must have EXTERNAL
// linkage -- it used to sit in this file's anonymous namespace.
} // namespace
uint32_t __cdecl promoted_load_planet(int32_t planet_index, uint32_t mode) {
    const long c = ++g_load_calls;
    if (c == 1 || c == 10 || c == 100) {
        say("; [save] LoadPlanetFromDisk replacement served call #%ld (planet=%u mode=%u, verify=%d)\n",
            c, (unsigned)planet_index, (unsigned)mode, g_verify_mode);
    }
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    if (g_verify_mode != 0 && g_load_tramp != nullptr) return load_verify_round(planet_index, mode);
#endif
    return run_ours_load(planet_index, mode);
}
namespace {

// ---------------------------------------------------------------------------------------------
// THE TWO CONTAINER ROOTS. game::SaveGame 0x0044716e / llm_game_load 0x004475de, both
// `int __watcall f(char *save_name)`. The .sav path is G_SAVE_DIR + save_name + ".sav" -- NOTE no
// `temp\` component; that one belongs to the per-planet files only.

int32_t g_member_fh = 0; // the member file currently open, for member_io's five callbacks

int32_t member_begin_read(void *, int32_t planet) {
    char name[PATH_BYTES], path[PATH_BYTES];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "save%02d.dat", planet);
    path[0] = '\0';
    append_bounded(path, PATH_BYTES, binds().save_dir);
    append_bounded(path, PATH_BYTES, binds().save_temp_dir);
    append_bounded(path, PATH_BYTES, name);
    g_member_fh = mh::host().vfs_open(path, MH_VFS_READ);
    // 0x00447507: the length is SEEK_END + tell + SEEK_SET, not a stat. Reproduced, because the
    // original's own u32 length prefix is whatever tell reported.
    //
    // SIMABI-VFS WEIGHED RETIRING THIS PAIR and refused, on the evidence rather than on taste. The
    // idea was to thread the byte count `map_SavePlanetToDisk` had just written straight into the
    // embed. But the container embeds EVERY VISITED PLANET (game_SaveGame's plate: "appends each
    // visited/current planet's per-planet save file, size-prefixed"; members_write_streaming walks
    // `member_included` over all of them), and save%02d.dat has THREE original writers --
    // game_SaveGame 0x0044719a, SwitchToPlanet 0x0044cece and llm_strat_try_enter_tactical_mission
    // 0x0044d38f -- the last two untranslated and firing at arbitrary earlier instants, in earlier
    // sessions. At most ONE member's length is ever knowable from a writing path; every other one is
    // a file on disk. So the length genuinely comes from the file, seek/tell stay, and they are a
    // real host obligation rather than a leftover of the mechanical table.
    if (mh::host().vfs_seek(g_member_fh, 0, 2 /*from the end*/) != 0) return -1;
    const int32_t len = mh::host().vfs_tell(g_member_fh);
    if (mh::host().vfs_seek(g_member_fh, 0, 0 /*from the start*/) != 0) return -1;
    return len;
}

int32_t member_read(void *, void *dst, uint32_t n) {
    return mh::host().vfs_read(g_member_fh, dst, n);
}

int member_begin_write(void *, int32_t planet) {
    char name[PATH_BYTES], path[PATH_BYTES];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "save%02d.dat", planet);
    path[0] = '\0';
    append_bounded(path, PATH_BYTES, binds().save_dir);
    append_bounded(path, PATH_BYTES, binds().save_temp_dir);
    append_bounded(path, PATH_BYTES, name);
    g_member_fh = mh::host().vfs_open(path, MH_VFS_WRITE);
    return g_member_fh != 0 ? 0 : 1;
}

int member_write(void *, const void *src, uint32_t n) {
    return mh::host().vfs_write(g_member_fh, src, n) == static_cast<int32_t>(n) ? 0 : 1;
}

int member_end(void *) {
    if (g_member_fh != 0) mh::host().vfs_close(g_member_fh);
    g_member_fh = 0;
    return 0;
}

constexpr member_io MEMBER_IO = {member_begin_read, member_read, member_begin_write,
                                 member_write, member_end, nullptr};

// The path of member `planet`, which the read oracle also needs (it archives each extracted file
// aside before the second arm overwrites it). Same three components as member_begin_{read,write}.
void member_path(char *path, int32_t planet) {
    char name[PATH_BYTES];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "save%02d.dat", planet);
    path[0] = '\0';
    append_bounded(path, PATH_BYTES, binds().save_dir);
    append_bounded(path, PATH_BYTES, binds().save_temp_dir);
    append_bounded(path, PATH_BYTES, name);
}

// ---------------------------------------------------------------------------------------------
// THE TWO LEGACY HOOKS, BOUND TO THE GAME (save_driver.h `legacy_hooks` carries the argument).
//
// Both are READ-side, version-gated, and UNREACHABLE from a file this build wrote -- it stamps
// version 5. They exist to read an older *Extermination* save, and they are here because a promotion
// that quietly dropped them would not be a promotion: it would be a loader that accepts an old file
// and then silently fails to apply half of what it read.

// 0x004478b6..0x004478f4. Thirty 0x78-byte ANSI records in the block just read become thirty
// 0xf0-byte wide ones in MESSAGE_QUEUE. The register mapping is the one thing here a plausible
// translation gets backwards: at 0x004478cf..0x004478ef EDX is the SOURCE (IMUL EDX,[i],0x78 + the
// stack buffer) and EAX is the DESTINATION (0x5ce484 + i*0xf0), i.e. llm_str_ansi_to_wide(dst, src)
// -- and the committed storage agrees (dst=EAX:4, src=EDX:4), which is why this reads naturally
// rather than looking reversed.
void legacy_message_queue_ansi_to_wide(void *, const uint8_t *src, uint8_t *dst) {
    for (uint32_t i = 0; i < LEGACY_MESSAGE_RECORDS; ++i)
        mh::host().ansi_to_wide(
            dst + i * LEGACY_MESSAGE_WIDE_STRIDE,
            const_cast<char *>(reinterpret_cast<const char *>(src + i * LEGACY_MESSAGE_ANSI_STRIDE)));
}

// 0x0044797e. The ver<5 branch does NOT merely skip the two blocks.
void legacy_invasion_alert_reset(void *) { MH_LIBMH_BIND(llm_strat_invasion_alert_reset_all)(); }

constexpr legacy_hooks LEGACY_HOOKS = {legacy_message_queue_ansi_to_wide, legacy_invasion_alert_reset,
                                       nullptr};

// The scratch a version-gated DISCARD block is read into. The original uses two stack buffers inside
// its 0x8824-byte frame (0xffff85dc for the ver<2 0x78b4 block, 0xffff77c8 for the ver<4 0xe10 one);
// ours is one static arena the driver bump-allocates from and hands straight back, so the size only
// has to cover the LARGEST single discard rather than their sum. Derived from the table so that
// adding a legacy block cannot silently overflow it.
constexpr uint32_t max_discard_bytes() {
    uint32_t m = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && table::CONTAINER[i].discard &&
            table::CONTAINER[i].size > m)
            m = table::CONTAINER[i].size;
    return m;
}
uint8_t g_container_scratch[max_discard_bytes()];
static_assert(sizeof(g_container_scratch) >= 0x78b4,
              "the ver<2 discard block no longer fits the container reader's scratch");

const void *live_media_source(void *) {
    // 0x004475b8. EBX (the .sav handle) and EDX (0x400) are already set for the BLOCK writer when
    // this is called, so the report itself takes no arguments and returns its buffer in EAX.
    return mh::host().media_diag_block();
}

// The version string the writer stamps: table[build_index], 0x28 bytes
// (0x0044724e IMUL ESI,[0x005d09e4],0x28 / 0x00447255 MOV EAX,0x5d08f4 / ADD).
const char *current_version_string() {
    const int32_t idx = *binds().version_index;
    return binds().version_strings + idx * VERSION_STRING_LEN;
}

bool build_sav_path(char *path, const char *save_name) {
    path[0] = '\0';
    append_bounded(path, PATH_BYTES, binds().save_dir);
    append_bounded(path, PATH_BYTES, save_name);
    append_bounded(path, PATH_BYTES, ".sav");
    return true;
}

// The arena is the container READER's; save_container never allocates. Kept in the one env builder
// anyway, because two builders differing by two fields is how the reader ends up with the writer's
// member_io and a null hook set.
arena g_container_arena{};

driver_env live_container_env(int32_t file_handle) {
    driver_env e      = live_env(file_handle);
    e.members         = &MEMBER_IO;
    e.media_source    = live_media_source;
    e.legacy          = &LEGACY_HOOKS;
    g_container_arena = arena{g_container_scratch, sizeof(g_container_scratch), 0};
    e.mem             = &g_container_arena;
    e.vt              = version_table{binds().version_strings,
                         binds().version_index};
    return e;
}

uint32_t run_ours_savegame(char *save_name, char *out_path) {
    // 0x0044719a: the CURRENT planet is written out FIRST, and its result is deliberately not
    // accumulated (0x0044719f reloads ESI rather than ADDing). With the save closure key on this is our
    // own body; with it off it is the original. Either way the container then embeds what it wrote.
    MH_LIBMH_BIND(map_SavePlanetToDisk)(*binds().planet_index, 3);

    char path[PATH_BYTES];
    build_sav_path(path, save_name);
    if (out_path != nullptr) std::strcpy(out_path, path);

    const int32_t fh = mh::host().vfs_open(path, MH_VFS_WRITE);
    if (fh == 0) {
        say("; [save] SAVEGAME open_file('%s') FAILED\n", path);
        return 0;
    }
    driver_env      env = live_container_env(fh);
    container_image img{};
    std::memcpy(img.header, current_version_string(), VERSION_STRING_LEN);
    img.version = detect_version(env.vt, img.header);
    // save_container drives the version GATE off img.version, and the writer always stamps the
    // current build's string -- so a legacy step can never be selected here. Asserted by the walk
    // itself: a `discard` step is skipped unconditionally on the write side.
    const int rc = save_container(env, img);
    mh::host().vfs_close(fh);
    return rc == 0 ? 1u : 0u;
}

// ---------------------------------------------------------------------------------------------
// THE CONTAINER READER. llm_game_load 0x004475de -- the .sav's blocks, then its embedded per-planet
// members streamed back OUT to save%02d.dat, then an apply tail bigger than either per-planet root's.
//
// THIS ROOT WAS RECORDED AS UNPROMOTABLE AND THE REASON WAS WRONG; the correction is kept because it
// is worth more than the finding. Its tail LOOKS like it passes an argument in the FLAGS:
//
//   0x00447b4e  FLD   double ptr [_G_LLM_STRAT_GAME_CLOCK]
//   0x00447b54  FCOMP double ptr [0x00500cdd]
//   0x00447b5a  FNSTSW AX
//   0x00447b5c  SAHF
//   0x00447b5d  CALL  llm_strat_time_resync_and_tick        <-- entered with EFLAGS live
//
// -- which a generated wrapper could not reproduce, since the marshalling thunk runs between the
// compare and the callee entry. But READ THE CALLEE: llm_strat_time_resync_and_tick 0x00449e21 is 55
// bytes of straight line (two calls, an FSTP to LAST_GAME_TIME, a third call) with no conditional
// jump, no SETcc, no CMOVcc and no ADC/SBB. It never reads EFLAGS, and its own PUSH EBP / CALL
// assert_stack_capacity would destroy them anyway. Nothing after the call site consumes them either
// (0x00447b62 is a PUSH). The FLD/FCOMP pair is FPU-stack net-zero -- push, then compare-and-pop --
// and FNSTSW/SAHF only clobber AX, which a `void f(void)` does not read. THE SEQUENCE IS DEAD, so
// this body simply does not have it, and that is faithful rather than a shortcut.
//
// The blocker had been recorded from the CALL SITE alone. Reading the other end took one query to
// dissolve it, and it would otherwise have bought a hand-written naked shim nobody needed.
uint32_t run_ours_load_container(char *save_name) {
    char path[PATH_BYTES];
    build_sav_path(path, save_name);
    // 0x00447688: open_file(path, ReadBinary 0x500cda). A missing file is `return 0`, not a refusal
    // with a message -- the menu above decides what to say.
    const int32_t fh = mh::host().vfs_open(path, MH_VFS_READ);
    if (fh == 0) {
        say("; [save] LOADGAME open_file('%s') FAILED\n", path);
        return 0;
    }

    driver_env      env = live_container_env(fh);
    container_image img{};
    // load_container reads the 40-byte header, REFUSES a version the table does not contain, walks
    // table::CONTAINER (including the two version-gated legacy behaviours above), and streams each
    // embedded member back out through member_io. All of that is 0x004476a2..0x00447b1c.
    //
    // ONE DELIBERATE NARROWING, and it is in the safe direction. The original accumulates block
    // failures and tests the total ONCE, at 0x0044798e -- so a corrupt block does not stop it, and it
    // keeps reading the remaining blocks into LIVE STATE before returning 0. Ours stops at the first
    // refusal. Both end in `return 0` having already overwritten some state; ours overwrites strictly
    // less of it. Reproducing "keep parsing a file we have already decided is bad" would be
    // faithfulness for its own sake.
    const int rc = load_container(env, &img);
    mh::host().vfs_close(fh);
    if (rc != 0) return 0;

    // ---- THE APPLY PHASE (0x00447b29..0x00447b9d). No file I/O; the loaded world being pushed back
    // into the building/player/camera/sound layers. Every one stays a CALL -- they are the closure
    // boundary. Six of the nine were already callable; three needed prototypes committed for this
    // item (llm_strat_register_bldg_type_callbacks, llm_map_cam_mark_viewport_dirty, and
    // FUN_004cad05, now llm_menu_build_placement_pending_clear).
    MH_LIBMH_BIND(llm_strat_register_bldg_type_callbacks)();
    // 0x00447b2e. SIMABI-NOTIFY: the record is the INSTANT, no payload -- the host re-derives the
    // eight display colours from the colour indices the container load has just installed. Nothing
    // below writes a colour index (libmh's only writer is llm_strat_player_profile_init, a
    // session-begin path this apply phase does not reach), so a poll host reads the same eight.
    mh::state::evt::inv_player_color_lut();

    // 0x00447b33: the CURRENT planet is re-loaded from the member this call just extracted, in mode 3.
    // With the load seam armed this is our own body; otherwise it is the original. A result other
    // than 1 is the one place a callee's answer decides this one's.
    const uint32_t planet = *binds().planet_index;
    if (MH_LIBMH_BIND(map_LoadPlanetFromDisk)(planet, 3) != 1) return 0;

    MH_LIBMH_BIND(llm_strat_time_resync_and_tick)();
    // 0x00447b62: PUSH [LAST_GAME_TIME+4] / PUSH [LAST_GAME_TIME] -- high half then low half, i.e. the
    // double at LAST_GAME_TIME laid out little-endian in the callee's stack slot. Read as a double and
    // passed as one; the generated wrapper writes the same eight bytes at the same offset.
    // The value it reads is the one llm_strat_time_resync_and_tick has JUST written, so the order of
    // these two calls is load-bearing rather than incidental.
    MH_LIBMH_BIND(llm_snd_ambient_reseed_planet_event_times)(
        static_cast<int32_t>(planet), *binds().last_game_time);
    mh::state::evt::cam_jump_queue_clear();
    mh::state::evt::menu_placement_pending_clear();
    mh::state::evt::inv_viewport();

    // 0x00447b87: `CMP [acc],0 / JLE -> return 1`. The accumulator was zeroed at 0x00447b47 and
    // nothing adds to it afterwards, so this test is DEAD and the answer here is always 1. Reproduced
    // as the constant it is, with the test named rather than written out -- a `if (0 > 0)` would read
    // like a translation error to the next person.
    return 1;
}

// ---------------------------------------------------------------------------------------------
// THE IN-CALL A/B.

// THE COMPARISON IS BLOCK-STRUCTURAL, NOT A BYTE DIFF, and the first A/B run is why.
//
// A raw byte offset says almost nothing about a chained format: one block whose compressed length
// differs by fifteen bytes shifts every byte after it, so "DIFFER at offset 129741" reads like a
// catastrophe and was in fact 187 blocks of the file agreeing about everything that matters. Walking
// the chain instead names the BLOCK INDICES that differ, which is exactly the granularity at which
// this promotion's responsibility is divided: the blocks our walk emits are ours, and the blocks the
// region delegate emits come from the ORIGINAL llm_map_save_regions in BOTH arms.
//
// A block is [u32 compressed_len][u32 uncompressed_size][compressed_len payload bytes], so the chain
// is self-delimiting and needs no table to walk.
struct chain_stats {
    int      blocks_a, blocks_b;
    int      first_diff, last_diff, diff_count; // block indices; first_diff = -1 when none
    int      size_mismatches;                   // blocks whose UNCOMPRESSED size disagreed
    int      record_excused;                    // region records differing ONLY in unwritten slots
    int      real_diff;                         // diff_count minus record_excused -- THE VERDICT
    uint32_t bytes_a, bytes_b;
    bool     walked_a, walked_b; // the chain landed exactly on EOF
};

// ---- the region-record equivalence test ---------------------------------------------------------
//
// THE ORIGINAL IS NOT BYTE-REPRODUCIBLE, and it is not a guess: `[save] verify=2` ran the
// ORIGINAL TWICE from the same state and got 180 of 239 blocks differing, indices 42..221, with ZERO
// uncompressed-size mismatches and file sizes ~1 KB apart. That span is exactly the 1036-byte region
// records, and `llm_map_save_regions` refills ONE reused 0x40c stack buffer field by field
// (save_driver.cpp marshal_region_records), so every slot past a node's neighbour count still holds
// the previous node's value -- or, for the first node, whatever was on the stack. Two calls from two
// different stack contexts therefore emit different bytes for the same graph.
//
// So a byte comparison of these blocks cannot be satisfied BY ANY IMPLEMENTATION, the original
// included. The answer is NOT to excuse the span -- that would leave 75% of the blocks unverified.
// It is to compare what the format actually carries: decode both records and compare the header plus
// the neighbour slots the count says are IN USE. Everything past the count is, by the original's own
// construction, undefined.
constexpr uint32_t REC_CNT_OFF   = 0x008;
constexpr uint32_t REC_PTRS_OFF  = 0x00c;
constexpr uint32_t REC_DATA_OFF  = 0x20c;
constexpr uint32_t REC_END       = table::REGION_RECORD_BYTES; // 0x40c
constexpr uint32_t REC_SLOTS     = (REC_DATA_OFF - REC_PTRS_OFF) / 4;
constexpr uint32_t REC_DEC_SLACK = 8; // mh_lzss::decompress_block can overrun dst by up to 8

lzw::decoder_state g_dec;
uint8_t            g_rec_a[REC_END + REC_DEC_SLACK];
uint8_t            g_rec_b[REC_END + REC_DEC_SLACK];

bool decode_record(const uint8_t *payload, uint32_t clen, uint8_t *dst) {
    uint32_t io = clen;
    std::memset(dst, 0, REC_END + REC_DEC_SLACK);
    lzw::decompress(g_dec, payload, dst, &io, REC_END);
    return io == REC_END;
}

// True when two region records describe the SAME GRAPH NODE, ignoring the slots the original never
// wrote. False also when either side fails to decode -- an undecodable block is not an excused one.
bool records_equivalent(const uint8_t *pa, uint32_t ca, const uint8_t *pb, uint32_t cb) {
    if (!decode_record(pa, ca, g_rec_a) || !decode_record(pb, cb, g_rec_b)) return false;
    if (std::memcmp(g_rec_a, g_rec_b, REC_PTRS_OFF) != 0) return false; // id, the two bytes, d4, count
    uint32_t n = 0;
    std::memcpy(&n, g_rec_a + REC_CNT_OFF, 4);
    if (n > REC_SLOTS) return false; // a count past the array is a real defect, not stack noise
    if (std::memcmp(g_rec_a + REC_PTRS_OFF, g_rec_b + REC_PTRS_OFF, n * 4) != 0) return false;
    return std::memcmp(g_rec_a + REC_DATA_OFF, g_rec_b + REC_DATA_OFF, n * 4) == 0;
}

uint8_t *slurp(const char *path, uint32_t *len_out) {
    *len_out = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    const uint32_t n = GetFileSize(h, nullptr);
    uint8_t       *p = static_cast<uint8_t *>(VirtualAlloc(nullptr, n ? n : 1, MEM_COMMIT, PAGE_READWRITE));
    if (p == nullptr) {
        CloseHandle(h);
        return nullptr;
    }
    DWORD got = 0;
    if (!ReadFile(h, p, n, &got, nullptr) || got != n) {
        VirtualFree(p, 0, MEM_RELEASE);
        CloseHandle(h);
        return nullptr;
    }
    CloseHandle(h);
    *len_out = n;
    return p;
}

// Advance one block. Returns false when the cursor is not on a well-formed block.
bool next_block(const uint8_t *p, uint32_t len, uint32_t off, uint32_t *clen, uint32_t *ulen) {
    if (off + 8 > len) return false;
    std::memcpy(clen, p + off, 4);
    std::memcpy(ulen, p + off + 4, 4);
    return *clen != 0 && off + 8 + *clen <= len;
}

// Returns false only if a file could not be read at all.
bool compare_chains(const char *a, const char *b, chain_stats *st) {
    std::memset(st, 0, sizeof *st);
    st->first_diff = st->last_diff = -1;
    uint8_t *pa                    = slurp(a, &st->bytes_a);
    if (pa == nullptr) return false;
    uint8_t *pb = slurp(b, &st->bytes_b);
    if (pb == nullptr) {
        VirtualFree(pa, 0, MEM_RELEASE);
        return false;
    }
    uint32_t oa = 0, ob = 0;
    for (;;) {
        uint32_t   ca = 0, ua = 0, cb = 0, ub = 0;
        const bool ga = next_block(pa, st->bytes_a, oa, &ca, &ua);
        const bool gb = next_block(pb, st->bytes_b, ob, &cb, &ub);
        if (!ga || !gb) {
            st->walked_a = !ga && oa == st->bytes_a;
            st->walked_b = !gb && ob == st->bytes_b;
            if (ga != gb) { // one chain still has blocks -- count the rest as differing
                ++st->diff_count;
                if (st->first_diff < 0) st->first_diff = ga ? st->blocks_a : st->blocks_b;
                st->last_diff = ga ? st->blocks_a : st->blocks_b;
            }
            break;
        }
        const bool same = ca == cb && ua == ub && std::memcmp(pa + oa + 8, pb + ob + 8, ca) == 0;
        if (!same) {
            ++st->diff_count;
            if (st->first_diff < 0) st->first_diff = st->blocks_a;
            st->last_diff = st->blocks_a;
            if (ua != ub) ++st->size_mismatches;
            // A region record is excused ONLY when it decodes to the same node -- see above. Anything
            // else, including a block that merely happens to be 1036 bytes, counts as a real diff.
            if (ua == ub && ua == REC_END && records_equivalent(pa + oa + 8, ca, pb + ob + 8, cb))
                ++st->record_excused;
        }
        oa += 8 + ca;
        ob += 8 + cb;
        ++st->blocks_a;
        ++st->blocks_b;
    }
    VirtualFree(pa, 0, MEM_RELEASE);
    VirtualFree(pb, 0, MEM_RELEASE);
    st->real_diff = st->diff_count - st->record_excused;
    return true;
}

void poke_byte(const char *path, uint32_t off) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        say("; [save] POKE FAILED to open '%s' (err %lu) -- the mutation check did NOT run\n", path,
            (unsigned long)GetLastError());
        return;
    }
    uint8_t b  = 0;
    DWORD   n  = 0;
    bool    ok = SetFilePointer(h, static_cast<LONG>(off), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
    ok         = ok && ReadFile(h, &b, 1, &n, nullptr) && n == 1;
    b          = static_cast<uint8_t>(b ^ 0xFF);
    ok         = ok && SetFilePointer(h, static_cast<LONG>(off), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
    ok         = ok && WriteFile(h, &b, 1, &n, nullptr) && n == 1;
    CloseHandle(h);
    say("; [save] POKE byte %u of '%s' flipped: %s -- this run's verdict MUST be DIVERGENT\n",
        (unsigned)off, path, ok ? "ok" : "FAILED");
}

// ---------------------------------------------------------------------------------------------
// THE CONTAINER A/B. Same shape as the per-planet one -- ours writes, is renamed aside, the original
// writes its own from the same state, compare -- but the COMPARISON differs, because a .sav is not a
// flat block chain: it is a 40-byte version string, then blocks, then length-prefixed MEMBERS (raw
// bytes, not blocks), then the media block. So the chain walk is started past the header and is
// expected to stop at the member boundary; what it covers is the container's own block section, and
// the members are compared as (count, lengths) plus a whole-file size check.
//
// The members' CONTENTS are deliberately not byte-compared: they are per-planet files, and SV1-P
// measured that those carry the original's region-record stack noise, so a byte comparison there is
// unsatisfiable by any implementation. Their equality was established at SV1-P by the per-planet A/B,
// which is the right place for it.
struct sav_stats {
    int      blocks_compared, blocks_expected, block_diffs, first_block_diff;
    uint32_t header_bytes_differ;
    uint32_t bytes_a, bytes_b;
    uint32_t block_section_a, block_section_b;
};

bool compare_sav(const char *a, const char *b, sav_stats *st) {
    std::memset(st, 0, sizeof *st);
    st->first_block_diff = -1;
    uint8_t *pa          = slurp(a, &st->bytes_a);
    if (pa == nullptr) return false;
    uint8_t *pb = slurp(b, &st->bytes_b);
    if (pb == nullptr) {
        VirtualFree(pa, 0, MEM_RELEASE);
        return false;
    }
    if (st->bytes_a >= VERSION_STRING_LEN && st->bytes_b >= VERSION_STRING_LEN)
        for (uint32_t i = 0; i < VERSION_STRING_LEN; ++i)
            if (pa[i] != pb[i]) ++st->header_bytes_differ;

    // HOW MANY BLOCKS TO COMPARE IS KNOWN, NOT GUESSED. The first attempt stopped when the two
    // uncompressed sizes disagreed, on the theory that the member section would not parse as a block
    // -- and it walked ONE STEP PAST the boundary, because a member's `u32 length` followed by that
    // member's own first block header happens to read as a plausible block whose "uncompressed size"
    // matches in both arms. It then reported that step as a differing block, which is true and
    // meaningless. `table::CONTAINER` says exactly how many blocks the writer emits: every STEP_BLOCK
    // that is not `discard` (a version-gated legacy read the writer never produces).
    int want = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && !table::CONTAINER[i].discard) ++want;

    uint32_t oa = VERSION_STRING_LEN, ob = VERSION_STRING_LEN;
    while (st->blocks_compared < want) {
        uint32_t ca = 0, ua = 0, cb = 0, ub = 0;
        if (!next_block(pa, st->bytes_a, oa, &ca, &ua) || !next_block(pb, st->bytes_b, ob, &cb, &ub)) break;
        if (ca != cb || ua != ub || std::memcmp(pa + oa + 8, pb + ob + 8, ca) != 0) {
            ++st->block_diffs;
            if (st->first_block_diff < 0) st->first_block_diff = st->blocks_compared;
        }
        ++st->blocks_compared;
        oa += 8 + ca;
        ob += 8 + cb;
    }
    st->blocks_expected = want;
    st->block_section_a = oa;
    st->block_section_b = ob;
    VirtualFree(pa, 0, MEM_RELEASE);
    VirtualFree(pb, 0, MEM_RELEASE);
    return true;
}

uint32_t call_original_savegame(char *save_name) {
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    return mh::call::detail::s_u32_EAX(reinterpret_cast<uintptr_t>(g_container_tramp),
                                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(save_name)));
#else
    (void)save_name;
    return 0; // unreachable: the trampoline is installed by the hosted detour path only
#endif
}

int32_t container_verify_round(char *save_name) {
    char ours_path[PATH_BYTES];
    // verify=2 is the CONTROL: the ORIGINAL written twice from the same state, so a difference in the
    // container blocks can be attributed. The per-planet control found the original disagreeing with
    // ITSELF over its region records, which is precisely why the members are out of scope here -- the
    // container blocks come from fixed addresses and should have no such freedom, and this is what
    // establishes that rather than assuming it.
    const uint32_t rc_a = (g_verify_mode == 2) ? call_original_savegame(save_name)
                                               : run_ours_savegame(save_name, ours_path);
    if (g_verify_mode == 2) build_sav_path(ours_path, save_name);
    char kept[PATH_BYTES + 8];
    _snprintf_s(kept, sizeof(kept), _TRUNCATE, "%s.%s", ours_path, g_verify_mode == 2 ? "ctl" : "mine");
    if (!MoveFileExA(ours_path, kept, MOVEFILE_REPLACE_EXISTING)) {
        say("; [save] CONTAINER A/B ABANDONED -- could not rename '%s' aside (err %lu)\n", ours_path,
            (unsigned long)GetLastError());
        return static_cast<int32_t>(rc_a);
    }
    // MUTATION ARM. Offset 100 is inside container block 0's payload (the 40-byte version string, then
    // block 0's 8-byte header), so the comparator has no excuse available and must go red.
    if (g_container_poke != 0) poke_byte(kept, g_container_poke);

    const uint32_t rc_b = call_original_savegame(save_name);

    sav_stats st{};
    if (!compare_sav(kept, ours_path, &st)) {
        say("; [save] CONTAINER A/B: COULD NOT COMPARE -- a failed comparison is NOT a pass\n");
        return static_cast<int32_t>(rc_b);
    }
    // A short walk is a FAILURE, not a pass: it means the container did not even emit the blocks the
    // table says it must, and comparing zero blocks would otherwise read as agreement.
    const bool ok = st.block_diffs == 0 && st.header_bytes_differ == 0 &&
                    st.blocks_compared == st.blocks_expected;
    say("; [save] CONTAINER %s '%s' rc(A=%u B=%u): %s -- version header %s, %d of %d container blocks "
        "compared, %d differ (first %d); member section starts at %u vs %u; files %u vs %u bytes; "
        "A kept as '%s'\n",
        g_verify_mode == 2 ? "CONTROL orig-vs-orig" : "A/B ours-vs-orig",
        save_name ? save_name : "(null)", (unsigned)rc_a, (unsigned)rc_b,
        ok ? "EQUIVALENT (every container block is byte-identical)" : "DIVERGENT",
        st.header_bytes_differ == 0 ? "IDENTICAL" : "DIFFERS", st.blocks_compared, st.blocks_expected,
        st.block_diffs,
        st.first_block_diff, (unsigned)st.block_section_a, (unsigned)st.block_section_b,
        (unsigned)st.bytes_a, (unsigned)st.bytes_b, kept);
    if (ok) DeleteFileA(kept);
    return static_cast<int32_t>(rc_b);
}

int32_t __cdecl promoted_savegame(char *save_name) {
    const long c = ++g_container_calls;
    if (c == 1 || c == 10) {
        say("; [save] game::SaveGame replacement served call #%ld (name='%s', verify=%d)\n", c,
            save_name ? save_name : "(null)", g_verify_mode);
    }
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    if (g_verify_mode != 0 && g_container_tramp != nullptr) return container_verify_round(save_name);
#endif
    return static_cast<int32_t>(run_ours_savegame(save_name, nullptr));
}

// ---------------------------------------------------------------------------------------------
// THE CONTAINER READ ORACLE, and it has TWO halves because a container load produces two things.
//
// (1) STATE. Same trick as SV1-P-LOAD's: every block reads to a FIXED address, so loading the same
//     file twice overwrites the first result with the second. Run ours, hash every region
//     table::CONTAINER names; run the original on the same unchanged file, hash again.
//
// (2) THE EXTRACTED MEMBERS, which is the half the WRITER's evidence does not cover and the reason
//     this oracle is not just (1). The reader streams each embedded member back out to its own
//     save%02d.dat, and those files ARE byte-comparable -- unlike the writer's members, which are
//     out of scope there because they carry the original's region-record stack noise. Nothing is
//     marshalled on this path: it is a verbatim copy of bytes already in the .sav, so "byte for
//     byte" is satisfiable and is exactly the right bar. Ours must be ARCHIVED between the arms,
//     because the second arm rewrites the same paths.
//
// ONE SPAN IS EXCUSED, DERIVED FROM THE DISASSEMBLY RATHER THAN FROM A FAILING RUN. The apply tail
// calls llm_strat_time_resync_and_tick, which FSTPs a fresh GetCurrentTime() into LAST_GAME_TIME and
// then runs llm_strat_time_tick over the neighbouring clock doubles -- and 0x00e587b1..0x00e587e1
// lies INSIDE the _G_LLM_GAME_SESSION_MODE block (0x00e58344 + 0x657). Two arms read the wall clock
// at two different instants, so those six doubles cannot agree in ANY arm, the CONTROL included. The
// excusal is narrow, named, and its completeness is what the CONTROL arm is for: if anything else in
// that block moves, CONTROL goes red and says which step.
struct excused_span {
    uint32_t    addr;
    uint32_t    size;
    const char *why;
};
// SB-BIND T5: the window's base comes from the REGISTRY, not from mh::addr::. Both spellings are
// constexpr and both name 0x00e587b1 today, so this is not a behaviour change -- it is the same
// discipline as every other address in this module, and it means the excusal and the block table
// cannot come to disagree about where CURRENT_GAME_TIME is. The addresses here stay STOCK on
// purpose: hash_excusing() compares them against the stock addresses the block table carries, and
// the translation happens once, at the read.
constexpr excused_span EXCUSED[] = {
    {mh::state::REGIONS[mh::state::RID_CURRENT_GAME_TIME].base, 0x30,
     "the six contiguous clock doubles (CURRENT/LAST/TOTAL_GAME_TIME, SIM_STEP_INTERVAL, "
     "GAME_TIME_DELTA, game_speed) -- wall-clock derived, re-stamped by the apply tail"},
};
constexpr int N_EXCUSED = static_cast<int>(sizeof EXCUSED / sizeof *EXCUSED);
// EVERYTHING FROM HERE TO hash_container_state IS THE VERIFY-MODE ORACLE, and verify mode compares
// our save against the ORIGINAL's -- so it is hosted by definition, not merely by convenience
// (LIB-VA0's standing rule in this file; the standalone stub for install_with_trampoline above is
// the same decision one layer up). It is guarded as a BLOCK because all of it is one mechanism: the
// EXCUSED window is expressed as stock addresses, the assert compares two stock bases, and the
// walker translates stock addresses chunk by chunk. None of those has a standalone meaning.
#ifndef MH_LIBMH_BUILD
static_assert(mh::state::REGIONS[mh::state::RID_CURRENT_GAME_TIME].base + 0x30 >
                  mh::state::REGIONS[mh::state::RID_LAST_GAME_TIME].base,
              "the excusal window no longer covers LAST_GAME_TIME, the value the tail actually writes");

// How many hash chunks translate() refused (SB-BIND T5). Nonzero means a rebased region runs past
// what the host bound, so part of the comparison read NOTHING -- which is indistinguishable from
// agreement to a comparator that only counts mismatches, and is therefore folded into the verdict.
int g_hash_untranslatable = 0;

// Hash [addr, addr+size) skipping any overlap with an excused span, so the excusal is a HOLE in one
// step's hash rather than a whole step waved through -- the rest of SESSION_MODE stays compared.
uint64_t hash_excusing(uint32_t addr, uint32_t size) {
    uint64_t       h   = 0xcbf29ce484222325ull;
    uint32_t       cur = addr;
    const uint32_t end = addr + size;
    while (cur < end) {
        uint32_t stop = end; // next boundary: the start of an excused span, or its end
        uint32_t skip = 0;
        for (int i = 0; i < N_EXCUSED; ++i) {
            const uint32_t es = EXCUSED[i].addr, ee = EXCUSED[i].addr + EXCUSED[i].size;
            if (cur >= es && cur < ee) { // inside one: skip to its end
                skip = (ee < end ? ee : end) - cur;
                stop = cur;
                break;
            }
            if (es > cur && es < stop) stop = es; // before one: hash up to it
        }
        if (skip != 0) {
            cur += skip;
            continue;
        }
        // SB-BIND T5: read where the bytes ARE, not where the block table says they were. On an
        // unrebased build translate() is the identity, so this is byte-for-byte the previous
        // behaviour; under a relocated bind it is the difference between hashing the live region
        // and hashing the abandoned copy -- and the abandoned copy would agree with itself in BOTH
        // arms, which is the vacuous-green shape this comparator exists to refuse.
        const void *p = mh::state::translate(cur, stop - cur);
        if (p == nullptr) {
            ++g_hash_untranslatable;
            cur = stop;
            continue;
        }
        h   = fnv1a(p, stop - cur, h);
        cur = stop;
    }
    return h;
}

uint64_t g_chash_a[table::CONTAINER_STEPS];
uint64_t g_chash_b[table::CONTAINER_STEPS];

void hash_container_state(uint64_t *out) {
    for (int i = 0; i < table::CONTAINER_STEPS; ++i) {
        const table::step &s = table::CONTAINER[i];
        // A `discard` step has no address: it is read into scratch and dropped (or, on the ver<4 path,
        // converted into the MESSAGE_QUEUE step, which IS hashed). Nothing to compare either way.
        out[i] = (s.kind == table::STEP_BLOCK && !s.discard) ? hash_excusing(s.addr, s.size) : 0;
    }
}
#endif // !MH_LIBMH_BUILD -- the verify-mode oracle block

// How many CONTAINER steps carry a comparable region. Printed with the verdict so that a hash sweep
// which silently covered nothing cannot read as agreement.
constexpr int count_hashed_steps() {
    int n = 0;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i)
        if (table::CONTAINER[i].kind == table::STEP_BLOCK && !table::CONTAINER[i].discard) ++n;
    return n;
}

uint32_t call_original_loadgame(char *save_name) {
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    return mh::call::detail::s_u32_EAX(reinterpret_cast<uintptr_t>(g_container_load_tramp),
                                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(save_name)));
#else
    (void)save_name;
    return 0; // unreachable: the trampoline is installed by the hosted detour path only
#endif
}

// Archive every member the CURRENT state selects, and return how many were archived. Called between
// the arms; `expected` is the same predicate's count, so a member that failed to extract shows up as
// archived < expected instead of quietly shrinking the comparison.
int archive_members(char paths[table::MEMBER_LIMIT][PATH_BYTES], int32_t planets[table::MEMBER_LIMIT],
                    int *expected) {
    // resolve_region supplied for the same reason as the driver's env: consistency, so nobody
    // later has to work out why one state_io in this file answers by region and one does not.
    const state_io st{identity_resolve, nullptr, region_resolve};
    int            n = 0;
    *expected        = 0;
    for (int32_t i = table::MEMBER_FIRST; i < table::MEMBER_LIMIT; ++i) {
        if (!member_included(st, i)) continue;
        ++*expected;
        char src[PATH_BYTES];
        member_path(src, i);
        char dst[PATH_BYTES];
        _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s.mine", src);
        if (!CopyFileA(src, dst, FALSE)) continue;
        std::strcpy(paths[n], dst);
        planets[n] = i;
        ++n;
    }
    return n;
}

bool files_identical(const char *a, const char *b, uint32_t *len_a, uint32_t *len_b) {
    uint8_t *pa = slurp(a, len_a);
    if (pa == nullptr) return false;
    uint8_t *pb = slurp(b, len_b);
    if (pb == nullptr) {
        VirtualFree(pa, 0, MEM_RELEASE);
        return false;
    }
    const bool same = *len_a == *len_b && std::memcmp(pa, pb, *len_a) == 0;
    VirtualFree(pa, 0, MEM_RELEASE);
    VirtualFree(pb, 0, MEM_RELEASE);
    return same;
}

// LIB-VA0/LIB-REF-SPLIT: the CONSUMER of the verify-mode oracle guarded above, and it has to carry
// the same guard or it references symbols that block no longer defines. Verify mode runs OUR load and
// the ORIGINAL's over one file and compares; there is no original standalone, so the whole round is
// hosted. (The compiler found this rather than a reader: guarding the producers first left these
// three call sites naming g_chash_a/hash_container_state/g_hash_untranslatable with nothing behind
// them, which is the honest way to discover where a mechanism actually ends.)
#ifndef MH_LIBMH_BUILD
int32_t container_load_verify_round(char *save_name) {
    const uint32_t rc_a = (g_verify_mode == 2) ? call_original_loadgame(save_name)
                                               : run_ours_load_container(save_name);
    hash_container_state(g_chash_a);

    // The member set is enumerated from the state arm A just loaded, which is the same state arm B
    // will load -- so both arms select the same planets and the archive is a like-for-like snapshot.
    static char    paths[table::MEMBER_LIMIT][PATH_BYTES];
    static int32_t planets[table::MEMBER_LIMIT];
    int            expected = 0;
    const int      archived = archive_members(paths, planets, &expected);

    // MUTATION ARM 1: corrupt one hashed step between the arms.
    if (g_container_load_poke != 0 && g_container_load_poke < (uint32_t)table::CONTAINER_STEPS) {
        g_chash_a[g_container_load_poke] ^= 0x5555555555555555ull;
        say("; [save] LOADGAME POKE step %u ('%s') corrupted -- this run's verdict MUST be DIVERGENT\n",
            (unsigned)g_container_load_poke, table::CONTAINER[g_container_load_poke].name);
    }
    // MUTATION ARM 2: flip a byte of the FIRST archived member. Separate from arm 1 because the two
    // halves of this oracle can fail independently, and a mutation that only exercises one of them
    // leaves the other's comparator unwatched.
    if (g_container_load_member_poke != 0 && archived > 0)
        poke_byte(paths[0], g_container_load_member_poke - 1);

    const uint32_t rc_b = call_original_loadgame(save_name);
    hash_container_state(g_chash_b);

    int diffs = 0, first = -1;
    for (int i = 0; i < table::CONTAINER_STEPS; ++i) {
        if (g_chash_a[i] != g_chash_b[i]) {
            ++diffs;
            if (first < 0) first = i;
        }
    }

    int mem_same = 0, mem_read_fail = 0, mem_first_bad = -1;
    for (int i = 0; i < archived; ++i) {
        char now[PATH_BYTES];
        member_path(now, planets[i]);
        uint32_t la = 0, lb = 0;
        if (files_identical(paths[i], now, &la, &lb)) {
            ++mem_same;
        } else {
            if (la == 0 || lb == 0) ++mem_read_fail;
            if (mem_first_bad < 0) mem_first_bad = planets[i];
        }
        DeleteFileA(paths[i]);
    }

    // EVERY CLAUSE HERE EXISTS BECAUSE ITS ABSENCE WOULD LET A VACUOUS RUN READ GREEN: zero hashed
    // steps, zero members extracted, or a member that failed to archive all look exactly like
    // agreement to a comparator that only counts mismatches.
    const bool ok = diffs == 0 && archived == expected && expected > 0 && mem_same == archived &&
                    mem_read_fail == 0 && g_hash_untranslatable == 0;
    say("; [save] LOADGAME %s '%s' rc(A=%u B=%u): %s -- %d of %d hashed steps differ%s%s%s; members %d "
        "of %d extracted, %d byte-identical%s\n",
        g_verify_mode == 2 ? "CONTROL orig-vs-orig" : "A/B ours-vs-orig",
        save_name ? save_name : "(null)", (unsigned)rc_a, (unsigned)rc_b,
        ok ? "IDENTICAL" : "DIVERGENT", diffs, count_hashed_steps(),
        first >= 0 ? ", first '" : "", first >= 0 ? table::CONTAINER[first].name : "",
        first >= 0 ? "'" : "", archived, expected, mem_same,
        mem_first_bad >= 0 ? " (first differing member is not identical)" : "");
    if (g_hash_untranslatable != 0)
        say("; [save] LOADGAME %d hash chunk(s) UNTRANSLATABLE -- a rebased region runs past what "
            "the host bound, so those bytes were compared by neither arm\n",
            g_hash_untranslatable);
    if (first >= 0)
        say("; [save] LOADGAME first differing step %d '%s' -- %u bytes at %08X\n", first,
            table::CONTAINER[first].name, (unsigned)table::CONTAINER[first].size,
            (unsigned)table::CONTAINER[first].addr);
    // The state and the member files left behind are the ORIGINAL's second load, so the session
    // continues exactly as an unpromoted run would whatever our body decided.
    return static_cast<int32_t>(rc_b);
}
#endif // !MH_LIBMH_BUILD

int32_t __cdecl promoted_load_container(char *save_name) {
    const long c = ++g_container_load_calls;
    if (c == 1 || c == 10) {
        say("; [save] llm_game_load replacement served call #%ld (name='%s', verify=%d)\n", c,
            save_name ? save_name : "(null)", g_verify_mode);
    }
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    if (g_verify_mode != 0 && g_container_load_tramp != nullptr)
        return container_load_verify_round(save_name);
#endif
    return static_cast<int32_t>(run_ours_load_container(save_name));
}

uint32_t call_original(uint32_t planet_index, uint32_t mode) {
    // __mhfastocall: planet in EAX, mode in EDX, result in EAX. g_tramp holds the stolen prologue and
    // jumps back to entry+8, so this is the ORIGINAL body even though its entry now holds our JMP.
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    return mh::call::detail::s_u32_EAX_EDX(reinterpret_cast<uintptr_t>(g_tramp), planet_index, mode);
#else
    (void)planet_index;
    (void)mode;
    return 0; // unreachable: the trampoline is installed by the hosted detour path only
#endif
}

void report(const char *what, uint32_t planet_index, uint32_t mode, uint32_t rc_a, uint32_t rc_b,
            const chain_stats &st, const char *kept) {
    if (st.diff_count == 0) {
        say("; [save] %s planet=%u mode=%u rc(A=%u B=%u) %d blocks / %u bytes: IDENTICAL\n", what,
            (unsigned)planet_index, (unsigned)mode, (unsigned)rc_a, (unsigned)rc_b, st.blocks_a,
            (unsigned)st.bytes_a);
        return;
    }
    // The verdict is `real_diff`, not `diff_count`. An excused block is one whose region record
    // DECODES to the same node -- the original's own reused stack buffer is what makes the raw bytes
    // differ, and the control run proves it does so against ITSELF.
    say("; [save] %s planet=%u mode=%u rc(A=%u B=%u): %s -- %d of %d blocks differ byte-wise (indices "
        "%d..%d, %d with a different UNCOMPRESSED size), %d of those are region records EQUIVALENT "
        "after decode, %d REAL; %u vs %u bytes; chain-to-EOF A=%d B=%d; A kept as '%s'\n",
        what, (unsigned)planet_index, (unsigned)mode, (unsigned)rc_a, (unsigned)rc_b,
        st.real_diff == 0 ? "EQUIVALENT (every difference is stack noise the original also emits)"
                          : "DIVERGENT",
        st.diff_count, st.blocks_a, st.first_diff, st.last_diff, st.size_mismatches, st.record_excused,
        st.real_diff, (unsigned)st.bytes_a, (unsigned)st.bytes_b, (int)st.walked_a, (int)st.walked_b, kept);
}

// verify=1: OURS vs the ORIGINAL. verify=2: the ORIGINAL vs ITSELF -- the CONTROL, which is not
// optional. The first A/B came back DIFFER, and every differing byte turned out to be in region
// records the ORIGINAL emits in both arms; without a control, "ours differs from the original" and
// "the original differs from itself" produce the identical log line.
uint32_t verify_round(uint32_t planet_index, uint32_t mode) {
    char     first_path[PATH_BYTES];
    uint32_t rc_a = 0;
    if (g_verify_mode == 2) {
        rc_a = call_original(planet_index, mode);
        // The control has to learn the path the same way the A/B does, and it must not rebuild it by
        // a second route -- a control that computes its own answer for anything is not a control.
        if (!build_path(first_path, static_cast<int32_t>(planet_index), mode)) first_path[0] = '\0';
    } else {
        rc_a = run_ours(planet_index, mode, first_path);
    }
    if (first_path[0] == '\0') {
        say("; [save] A/B SKIPPED planet=%u mode=%u -- no file was written (rc=%u, mode=%d)\n",
            (unsigned)planet_index, (unsigned)mode, (unsigned)rc_a, g_verify_mode);
        return rc_a;
    }

    // Move the first file aside so the second writer produces the same name from the same state.
    // RENAMING rather than pointing the second writer elsewhere is what keeps the two comparable: the
    // path is built from globals, so redirecting would mean mutating them.
    char kept[PATH_BYTES + 8];
    _snprintf_s(kept, sizeof(kept), _TRUNCATE, "%s.%s", first_path, g_verify_mode == 2 ? "ctl" : "mine");
    if (!MoveFileExA(first_path, kept, MOVEFILE_REPLACE_EXISTING)) {
        say("; [save] A/B ABANDONED planet=%u -- could not rename '%s' aside (err %lu)\n",
            (unsigned)planet_index, first_path, (unsigned long)GetLastError());
        return rc_a;
    }

    // MUTATION CHECK. `[save] verify_poke=<offset>` flips one byte of the FIRST file before
    // the comparison. The whole point of the decode-and-compare-used-slots rule above is to forgive a
    // class of difference, and a forgiving comparator that forgives everything is worth less than no
    // comparator at all -- so there has to be a configuration in which this one goes red. Pick an
    // offset inside an ORDINARY block (100 lands in block 0's payload, uncompressed size 20400) and
    // the run must report DIVERGENT with real >= 1.
    if (g_verify_poke != 0) poke_byte(kept, g_verify_poke);

    const uint32_t rc_b = call_original(planet_index, mode);

    chain_stats st{};
    if (!compare_chains(kept, first_path, &st)) {
        say("; [save] A/B planet=%u mode=%u: COULD NOT COMPARE (err %lu) -- a failed comparison is "
            "NOT a pass\n",
            (unsigned)planet_index, (unsigned)mode, (unsigned long)GetLastError());
        return rc_b;
    }
    report(g_verify_mode == 2 ? "CONTROL orig-vs-orig" : "A/B ours-vs-orig", planet_index, mode, rc_a, rc_b,
           st, kept);
    if (st.real_diff == 0) DeleteFileA(kept); // keep the evidence only when it REALLY disagreed

    // The file left on disk is the SECOND writer's, which is the original in both modes -- so the
    // session sees exactly what an unpromoted run would, whatever our body decided.
    return rc_b;
}

// LIB-REBIND: named from another TU by the generated binder, so this one must have EXTERNAL
// linkage -- it used to sit in this file's anonymous namespace.
} // namespace
uint32_t __cdecl promoted_save_planet(uint32_t planet_index, uint32_t mode) {
    // PROVE THE BODY RAN. A promotion that silently did not install passes every other check in the
    // acceptance test, and an A/B that never fires logs exactly like one that always agreed.
    const long c = ++g_calls;
    if (c == 1 || c == 10 || c == 100 || c == 1000) {
        say("; [save] SavePlanetToDisk replacement served call #%ld (planet=%u mode=%u, verify=%d)\n", c,
            (unsigned)planet_index, (unsigned)mode, g_verify_mode);
    }
#ifndef MH_LIBMH_BUILD // LIB-VA0: verify mode is a HOSTED oracle -- see the note at g_tramp
    if (g_verify_mode != 0 && g_tramp != nullptr) return verify_round(planet_index, mode);
#endif
    char path[PATH_BYTES];
    return run_ours(planet_index, mode, path);
}
namespace {

} // namespace
} // namespace mh::save

// Defines mh_export_thunk_map_SavePlanetToDisk (naked, generated) + mh_export_install_map_SavePlanetToDisk().
// The binding is type-checked against mh::exp::sig_map_SavePlanetToDisk, so a wrong parameter list would
// not compile. At global scope, matching libmh/orders/order_queue.cpp.
MH_EXPORT_REPLACE(map_SavePlanetToDisk, mh::save::promoted_save_planet)
MH_EXPORT_REPLACE(map_LoadPlanetFromDisk, mh::save::promoted_load_planet)
// Both container roots. llm_game_load's "flags-passing tail" was a misreading of its call site; the
// callee never reads EFLAGS, so it binds through the ordinary generated entry thunk like the rest.
MH_EXPORT_REPLACE(game_SaveGame, mh::save::promoted_savegame)
MH_EXPORT_REPLACE(llm_game_load, mh::save::promoted_load_container)

namespace mh::save {
namespace {

// Verify mode's install: a TRAMPOLINE, not a plain entry JMP. install_export's install_jmp leaves the
// original unreachable, and the whole oracle is that it stays reachable. The guards it would have
// applied are reproduced here rather than skipped -- an owned entry and a byte mismatch need
// different words because they need different actions (hook/export.cpp).
//
// ONE BODY IN BOTH BUILDS since F4D-PRE. There used to be a second, `#ifdef MH_LIBMH_BUILD` copy
// that refused with "verify mode is UNAVAILABLE in the libmh build (no injection harness)" -- it
// existed only so the TU could compile without hook/ headers. The primitives now arrive through
// mh::hosthook, which answers "unowned"/false when nothing is bound, so the guard is gone and the
// unavailable case is stated FIRST and by name below. A standalone build takes that branch and
// still says why; it no longer says it from a different function.
bool install_with_trampoline(const char *what, uintptr_t target, uint64_t expect, void *thunk,
                             void **tramp_out) {
    if (!mh::hosthook::bound()) {
        say("; [promote] %s: verify mode is UNAVAILABLE -- no host hook table is bound (no "
            "injection harness), so there is nothing to install a trampoline through\n",
            what);
        return false;
    }
    if (const char *owner = mh::hosthook::entry_owner_of(target)) {
        say("; [promote] %s: REFUSED -- entry %08X is OWNED by %s; promote by rebinding that "
            "detour, not by a second entry patch\n",
            what, (unsigned)target, owner);
        return false;
    }
    uint64_t seen = 0;
    std::memcpy(&seen, reinterpret_cast<const void *>(target), sizeof(seen));
    if (seen != expect) {
        say("; [promote] %s: REFUSED -- entry bytes %016llX != expected %016llX (wrong build, or "
            "already hooked)\n",
            what, (unsigned long long)seen, (unsigned long long)expect);
        return false;
    }
    // entry_claim::rebind (C9): a promoter, like hook/export.cpp's install_jmp -- this is the code
    // that MAKES a body promoted, so the promotion interlock must not stand in front of it.
    // expect_prologue 0 (U30): the exact eight entry bytes were compared against `expect` just
    // above -- the generated constant, stronger than the generic prologue -- and plenty of promotable
    // leaf targets have no Watcom frame at all (hook/export.h says so). `what` names the promoter.
    if (!mh::hosthook::install_trampoline(target, thunk, tramp_out, 8, LIBMH_ENTRY_CLAIM_REBIND,
                                          what, 0)) {
        say("; [promote] %s: REFUSED -- install_trampoline failed\n", what);
        return false;
    }
    // THE BODY IS NOT DEAD HERE, and this line used to say it was. install_export's non-verify path
    // calls note_promoted because its install_jmp leaves the original UNREACHABLE; a TRAMPOLINE is
    // installed for the exact opposite reason -- call_original_* jumps back to `target+8` on every
    // A/B round, so [target+8, end) is LIVE CODE for the whole run. `note_promoted` is the tree's
    // one way of saying "this body is dead", and X-TOMB (a28e2cb7, 2026-09-01) then trap-fills the
    // dead remainder of every noted body with INT3 and TerminateProcess'es on a hit. So from that
    // commit the FIRST verify round walked straight into its own tombstone:
    //
    //   ; [save] SavePlanetToDisk replacement served call #1 (planet=3 mode=3, verify=1)
    //   ; [tombstone] HIT -- map_SavePlanetToDisk @00447BBB ENTERED (armed as promoted)
    //
    // -- 00447BBB being 00447BB3+8, the trampoline's own jump-back address. Same hit at 0044810F for
    // the load direction. The oracle was dead for six weeks and the symptom the rig reported was a
    // bare `PROCESS-GONE at 233 steps`: a promotion that keeps a TRAMPOLINE tells the tree its body is dead.
    //
    // NOTHING replaces the call. The claim was false, not merely inconvenient: the C1 interlock would
    // have refused a registered byte fix inside a body that still executes in arm B, which is the
    // wrong answer there too. What IS true -- that we hold the entry -- is not recorded either, and
    // deliberately: note_entry_owner feeds SIM1-P clause 6's yield derivation, and turning four rows'
    // libmh bindings off is a behaviour change this fix has no evidence for. No other detour in the
    // tree targets these four entries, and install_export's per-function byte guard still refuses a
    // second install on them.
    say("; [promote] %s: verify mode keeps the ORIGINAL body REACHABLE through the trampoline, so it "
        "is NOT noted as promoted and NOT tombstoned -- entry %08X\n",
        what, (unsigned)target);
    return true;
}

} // namespace

void set_logger(void (*fn)(const char *)) { g_log = fn; }

bool promotion_active() { return g_installed; }
bool verify_active() { return g_installed && g_verify_mode != 0; }

int install_promotion(const char *ini_path, int default_on) {
    if (default_on == 0) return 0;
    g_verify_mode = GetPrivateProfileIntA("save", "verify", 0, ini_path);
    g_verify_poke = (uint32_t)GetPrivateProfileIntA("save", "verify_poke", 0, ini_path);

#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the verify arm is a HOSTED oracle (see install_with_trampoline's standalone
    // stub above) and it is the only thing here that names an original entry ADDRESS and an entry
    // THUNK -- both of which are absent standalone, the thunk as a symbol and the address as four
    // literal VAs this arm would otherwise leave in the object. Not `install_with_trampoline(...)`
    // with null arguments: that would still pass the addr_/entry_ constants as immediates.
    const bool ok = mh_export_install_map_SavePlanetToDisk();
#else
    const bool ok = g_verify_mode != 0
                        ? install_with_trampoline("save", mh::exp::addr_map_SavePlanetToDisk,
                                                  mh::exp::entry_map_SavePlanetToDisk,
                                                  reinterpret_cast<void *>(mh_export_thunk_map_SavePlanetToDisk),
                                                  &g_tramp)
                        : mh_export_install_map_SavePlanetToDisk();
#endif
    if (!ok) {
        say("; [promote] save: SavePlanetToDisk is NOT promoted -- treat this run as unpromoted\n");
        return 0;
    }
    g_installed = true;
    g_calls     = 0;
    say("; [promote] save: SavePlanetToDisk is LIVE (mh::save::save_planet over the real file API), "
        "verify=%d\n",
        g_verify_mode);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// THE LOAD SEAM. SEPARATELY ARMED, not a tail on `save`, because it is a separate
// entry with separate evidence -- and because the two must be independently rollback-able while only
// one of them has been through a gate.
bool load_promotion_active() { return g_load_installed; }

int install_load_promotion(const char *ini_path, int default_on) {
    if (default_on == 0) return 0;
    // Deliberately the SAME verify mode as the save side: a run is either verifying or it is not, and
    // two independent verify levels would produce configurations nobody has a name for.
    if (g_verify_mode == 0) g_verify_mode = GetPrivateProfileIntA("save", "verify", 0, ini_path);
    g_load_poke = (uint32_t)GetPrivateProfileIntA("save", "load_verify_poke", 0, ini_path);

#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the verify arm is a HOSTED oracle (see install_with_trampoline's standalone
    // stub above) and it is the only thing here that names an original entry ADDRESS and an entry
    // THUNK -- both of which are absent standalone, the thunk as a symbol and the address as four
    // literal VAs this arm would otherwise leave in the object. Not `install_with_trampoline(...)`
    // with null arguments: that would still pass the addr_/entry_ constants as immediates.
    const bool ok = mh_export_install_map_LoadPlanetFromDisk();
#else
    const bool ok = g_verify_mode != 0
                        ? install_with_trampoline("load", mh::exp::addr_map_LoadPlanetFromDisk,
                                                  mh::exp::entry_map_LoadPlanetFromDisk,
                                                  reinterpret_cast<void *>(mh_export_thunk_map_LoadPlanetFromDisk),
                                                  &g_load_tramp)
                        : mh_export_install_map_LoadPlanetFromDisk();
#endif
    if (!ok) {
        say("; [promote] load: LoadPlanetFromDisk is NOT promoted -- treat this run as unpromoted\n");
        return 0;
    }
    g_load_installed = true;
    g_load_calls     = 0;
    say("; [promote] load: LoadPlanetFromDisk is LIVE (mh::save::load_planet over the real file API), "
        "verify=%d\n",
        g_verify_mode);
    return 1;
}

const char *last_save_path() {
    // Rebuilt rather than remembered: the promoted body is not the only writer (an unpromoted arm's
    // save goes through the original entirely), and a cached value would be stale exactly in the
    // configuration the archive is meant to compare against.
    static char   path[PATH_BYTES];
    const int32_t planet = static_cast<int32_t>(*binds().planet_index);
    if (!build_path(path, planet, 3)) path[0] = '\0';
    return path;
}

bool container_promotion_active() { return g_container_installed; }

int install_container_promotion(const char *ini_path, int default_on) {
    if (default_on == 0) return 0;
    if (g_verify_mode == 0) g_verify_mode = GetPrivateProfileIntA("save", "verify", 0, ini_path);
    g_container_poke = (uint32_t)GetPrivateProfileIntA("save", "container_verify_poke", 0, ini_path);

#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the verify arm is a HOSTED oracle (see install_with_trampoline's standalone
    // stub above) and it is the only thing here that names an original entry ADDRESS and an entry
    // THUNK -- both of which are absent standalone, the thunk as a symbol and the address as four
    // literal VAs this arm would otherwise leave in the object. Not `install_with_trampoline(...)`
    // with null arguments: that would still pass the addr_/entry_ constants as immediates.
    const bool ok = mh_export_install_game_SaveGame();
#else
    const bool ok = g_verify_mode != 0
                        ? install_with_trampoline("container", mh::exp::addr_game_SaveGame,
                                                  mh::exp::entry_game_SaveGame,
                                                  reinterpret_cast<void *>(mh_export_thunk_game_SaveGame),
                                                  &g_container_tramp)
                        : mh_export_install_game_SaveGame();
#endif
    if (!ok) {
        say("; [promote] container: game::SaveGame is NOT promoted -- treat this run as unpromoted\n");
        return 0;
    }
    g_container_installed = true;
    g_container_calls     = 0;
    say("; [promote] container: game::SaveGame is LIVE (mh::save::save_container over the real file "
        "API, members STREAMED), verify=%d\n",
        g_verify_mode);
    return 1;
}

// The container READER, separately armable. Separate from `container` on
// purpose: the write and read directions of this root have separate evidence, and the one that has
// not yet been through a gate must be rollback-able without taking the other with it.
bool container_load_promotion_active() { return g_container_load_installed; }

int install_container_load_promotion(const char *ini_path, int default_on) {
    if (default_on == 0) return 0;
    if (g_verify_mode == 0) g_verify_mode = GetPrivateProfileIntA("save", "verify", 0, ini_path);
    g_container_load_poke =
        (uint32_t)GetPrivateProfileIntA("save", "container_load_verify_poke", 0, ini_path);
    g_container_load_member_poke =
        (uint32_t)GetPrivateProfileIntA("save", "container_load_member_poke", 0, ini_path);

#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the verify arm is a HOSTED oracle (see install_with_trampoline's standalone
    // stub above) and it is the only thing here that names an original entry ADDRESS and an entry
    // THUNK -- both of which are absent standalone, the thunk as a symbol and the address as four
    // literal VAs this arm would otherwise leave in the object. Not `install_with_trampoline(...)`
    // with null arguments: that would still pass the addr_/entry_ constants as immediates.
    const bool ok = mh_export_install_llm_game_load();
#else
    const bool ok = g_verify_mode != 0
                        ? install_with_trampoline("container_load", mh::exp::addr_llm_game_load,
                                                  mh::exp::entry_llm_game_load,
                                                  reinterpret_cast<void *>(mh_export_thunk_llm_game_load),
                                                  &g_container_load_tramp)
                        : mh_export_install_llm_game_load();
#endif
    if (!ok) {
        say("; [promote] container_load: llm_game_load is NOT promoted -- treat this run as "
            "unpromoted\n");
        return 0;
    }
    g_container_load_installed = true;
    g_container_load_calls     = 0;
    say("; [promote] container_load: llm_game_load is LIVE (mh::save::load_container over the real "
        "file API, members STREAMED OUT, legacy ver<4/ver<5 paths bound), verify=%d\n",
        g_verify_mode);
    return 1;
}

// loadgame_now / savegame_now / save_planet_now MOVED to seams/save_triggers.cpp on
// 2026-09-09 (LIFT-TABLE S6.1). They are A/B triggers, not save logic: one-line forwards
// through the game's ENTRY so a single trigger exercises the original in an unpromoted run
// and ours in a promoted one. This file compiles into libmh.vcxproj, so hosting them here put
// three raw VA calls into the standalone build for harness work that build never runs.

} // namespace mh::save
