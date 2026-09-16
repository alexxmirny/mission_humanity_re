//
// save/save_driver.h -- the save file's DRIVERS, reimplemented as walks over the extracted block
// table (RI-SAVE / SV1-DRIVERS).
//
//   game::SaveGame        0x0044716e  -> save_container
//   llm_game_load         0x004475de  -> load_container
//   SavePlanetToDisk      0x00447bb3  -> save_planet
//   LoadPlanetFromDisk    0x00448107  -> load_planet
//   llm_map_save_regions  0x00424773  -> the STEP_REGIONS arm, plus marshal_region_records
//   llm_map_load_regions  0x00424a68  -> the same arm, plus unmarshal_region_records
//   llm_game_save_player_data / FUN_004ddaa5, FUN_0041c1e6 / llm_ui_bldg_panel_load_state,
//   FUN_0041c152 / llm_game_load_available_projects  -> ordinary runs inside the two tables
//
// The order, the addresses and the sizes are NOT in this file. They are in save_table.gen.h, derived
// from the disassembly by the save-block table extractor and shaped by tools/gen_save_table_header.py,
// which also checks the writer's sequence against the reader's. This file is only the walk.
//
// NOTHING IS BOUND TO THE GAME. Alongside the injected `block_io` the block layer already takes, the
// driver takes a `state_io`: a resolver from a game ADDRESS to a pointer. In the live build that is
// the identity function; in `net_selftest.exe savetest` it is a sparse address space over heap slabs,
// which is what lets a real save file round-trip with no game running and no file open. Binding
// either one for real is SV1-P.
//
// RETURN VALUES ARE THE LAYER BELOW'S: 0 = success, non-zero = failure.
//
// THREE THINGS IN THIS FORMAT ARE NOT FIXED-SIZE, and each is called out where it is handled:
//   1. the region graph -- N records, N read from the file's own fourth header block;
//   2. the progress[] slice -- 3 x a word in .data that IS NOT SAVED, i.e. it comes from config and
//      a save is only readable by a build whose config agrees;
//   3. the embedded-member count -- implicit, recomputed by a predicate over state read earlier in
//      the same file.
//
#pragma once
#include <cstdint>

#include "save_block.h"
#include "save_format.h"
#include "save_table.gen.h"

namespace mh::save {

// ---------------------------------------------------------------------------------------------
// The address space, injected.
//
// `resolve` maps a game address to a writable pointer covering [addr, addr+size). Returning null
// means "not mapped", which the driver treats as a hard failure rather than skipping the block --
// a skipped block would desynchronise every block after it.
struct state_io {
    void *(*resolve)(void *ctx, uint32_t addr, uint32_t size);
    void *ctx;
    // SB-HOSTFREE: resolve one RUN of a decomposed block, by REGION rather than by address.
    //
    // `resolve` cannot answer this. It goes through covering(), which matches on `reach` and
    // returns the FIRST region in base order that contains the window -- and the whole reason a
    // block needs decomposing is that an overrunning region's reach covers its neighbours, so a run
    // inside _G_LLM_GAME_SESSION_MODE's 1623-byte window resolves to SESSION_MODE rather than to the
    // region that owns those bytes. Asking by rid removes the ambiguity at the point where the
    // answer is actually known: the generated decomposition.
    //
    // NULL IS LEGAL and means "fall back to resolve(stock_addr, len)", which is what an offline
    // fixture wants -- savetest maps stock addresses into a sparse slab and has no registry at all.
    // Appended at the END: both call sites use aggregate initialisation, and SB-BIND T2 is the
    // session that learned what inserting a member mid-aggregate costs.
    void *(*resolve_region)(void *ctx, uint16_t rid, uint32_t off, uint32_t len) = nullptr;
};

// A bump allocator for the variable-length parts. The caller owns the memory; the driver never calls
// malloc, so it can run inside the game's address space later without an allocator of its own.
struct arena {
    uint8_t *base;
    uint32_t size;
    uint32_t used;
};
void *arena_alloc(arena &a, uint32_t bytes);

// ---------------------------------------------------------------------------------------------
// What a load captures that is NOT state: the bytes the driver must be able to re-emit verbatim.

// One embedded per-planet member: a plain uint32 length, then that planet file's bytes.
struct member {
    int32_t  planet; // i, the save%02d.dat index the predicate selected
    uint32_t length;
    uint8_t *bytes;
};

// The region graph, kept in FILE order rather than in list order. See marshal_region_records for why
// that distinction is load-bearing.
struct region_image {
    uint32_t nodes;    // N == the value of the fourth header block (G_LAST_MAP_INDEX)
    uint8_t *records;  // nodes * table::REGION_RECORD_BYTES
    uint8_t *grid_ids; // table::REGION_GRID_IDS_BYTES
    uint8_t *terrain;  // table::REGION_TERRAIN_BYTES
};

struct container_image {
    char    header[VERSION_STRING_LEN]; // the 40-byte version string, verbatim
    int32_t version;                    // detect_version's answer for it
    member  members[table::MEMBER_LIMIT];
    int32_t member_count;
    // The trailing media block, captured whole (header included) because nothing reads it and its
    // contents are a live CD/TOC query the driver cannot reproduce. Re-emitted byte for byte.
    uint8_t *tail;
    uint32_t tail_len;
};

struct planet_image {
    region_image regions;
    uint32_t     progress_slice; // 3 * (uint16)[PROGRESS_SLICE_WORD], as it was at load time
};

// Observables for the assertions the acceptance test names. Every field is something a wrong table,
// a wrong predicate or a wrong derived size would change.
struct driver_stats {
    uint32_t blocks_read;
    uint32_t blocks_written;
    uint32_t blocks_discarded;      // legacy version-gated reads consumed and thrown away
    uint32_t legacy_steps_selected; // legacy steps the version gate SELECTED, read or not
    uint32_t region_nodes;
    uint32_t progress_slice;
    int32_t  members;
    uint32_t member_bytes;
    uint32_t tail_bytes;
    int32_t  version;
};

// ---------------------------------------------------------------------------------------------
// WHAT THE LIVE SAVE PATH DELEGATES BACK TO THE ORIGINAL, and why it is not "everything we could".
//
// SavePlanetToDisk makes three outward calls. Two of them are OUTSIDE the closure and stay calls
// even when the root is ours; the third is inside it and is inlined. The distinction is not
// convenience -- each one is a measured property of the callee:
//
//   llm_map_save_regions 0x00424773      OUTSIDE. It serialises a LINKED LIST of malloc'd nodes
//                                        hanging off 0x0051de7c, not an array at a fixed address.
//                                        `regions_write` emits that from a `region_image` a load
//                                        produced; the live game has no image, it has the list.
//                                        Walking a live pointer graph would make the promoted
//                                        body's bytes depend on the heap -- the one thing a
//                                        byte-identity claim cannot tolerate.
//   llm_game_save_player_data 0x004dda66 OUTSIDE, and this one CORRECTS THE LEDGER: it was recorded
//                                        as "prologue + its two block writes + epilogue", which the
//                                        disassembly refutes -- 0x004dda97 also calls FUN_004ee8df
//                                        with s_Saving_file_00506db0, a status/UI side effect that
//                                        inlining the two blocks would silently drop.
//   FUN_0041c1e6 0x0041c1e6              INSIDE. Prologue, two block writes, its own failure
//                                        accumulator, epilogue -- nothing else, and SavePlanetToDisk
//                                        is its ONLY caller. Its two steps are walked like any other.
//
// THE LOAD SIDE SPLITS THE OPPOSITE WAY, and assuming symmetry would drop a side effect exactly as
// it nearly did above. Measured 2026-07-30 from LoadPlanetFromDisk 0x00448107:
//
//   FUN_004ddaa5 0x004ddaa5            PURE -- prologue + its two ReadCompressedFromFile calls and
//     (read twin of save_player_data)  nothing else. Its two steps are WALKED. Its save-side twin is
//                                      the impure one.
//   llm_ui_bldg_panel_load_state       IMPURE -- 0x0041c267 calls llm_ui_bldg_panel_open BEFORE its
//     0x0041c245 (read twin of         two reads. Was an outward covers-2 delegate until LIB-ABI
//     FUN_0041c1e6)                    stage E (2026-09-03) INVERTED it: the panel reset is a
//                                      covers-0 host callback and the two blocks are the driver's
//                                      own reads (see delegate_bldg_panel_reset in save_live.cpp).
//                                      Its save-side twin (FUN_0041c1e6) is the pure one.
//   STEP_REGIONS                       TWO calls here, not one: llm_map_region_pool_reset 0x004234b8
//                                      then llm_map_load_regions 0x00424a68. The latter MALLOCS the
//                                      node list, which is why it stays outward regardless.
//   llm_strat_planet_map_session_init  NO save-side twin at all, and it emits NO BLOCKS -- it simply
//     0x004dc65a                       runs between the progress loop and the next block read
//                                      (0x004485cd). That is what `covers == 0` is for.
//
// A delegate covers a RUN of consecutive table steps, because one original call emits several
// blocks. `at` matches the run's first step's `write_at`/`read_at`; 0 means the STEP_REGIONS step,
// which has no call address of its own (the extractor has no single one for a data-dependent block
// count).
struct step_delegate {
    uint32_t at;          // step::write_at (save) / read_at (load) of the first covered step, or 0 = REGIONS
    int      covers;      // consecutive STEP_BLOCK steps this one call emits. ZERO means the call emits
                          // NONE -- run it, then process the step normally. Ignored for STEP_REGIONS.
    int (*fn)(void *ctx); // returns the block layer's 0 = ok / non-zero = failed
};

// ---------------------------------------------------------------------------------------------
// THE CONTAINER'S MEMBERS ARE STREAMED THROUGH A SECOND FILE, and that is the whole reason
// SV1-P-CONTAINER is separate work from the per-planet roots.
//
// `container_image::members` holds each member's bytes in the arena, which is what lets `savetest`
// round-trip a real .sav with no game. The GAME does not do that: it opens save%02d.dat and shuttles
// the bytes through G_LZW_TEMP_DATA in <=MEMBER_CHUNK pieces, never holding a member whole.
//   game::SaveGame  0x004474ed open("rb") / seek END / tell -> length / seek 0, then the u32 length
//                   into the .sav and the chunk loop 0x00447581 read -> 0x00447596 write.
//   llm_game_load   0x00447a95 open("wb"), the u32 length out of the .sav at 0x00447abf, then
//                   0x00447af2 read -> 0x00447b07 write.
//
// Injected rather than branched on, so the arena path stays EXACTLY as savetest exercises it: null
// here is the old behaviour, byte for byte.
struct member_io {
    // WRITE side. Open member `planet` for reading; return its byte length, or < 0 on failure.
    int32_t (*begin_read)(void *ctx, int32_t planet);
    int32_t (*read)(void *ctx, void *dst, uint32_t n);
    // READ side. Open member `planet` for writing.
    int (*begin_write)(void *ctx, int32_t planet);
    int (*write)(void *ctx, const void *src, uint32_t n);
    // Close whichever is open. Called once per member, in both directions.
    int (*end)(void *ctx);
    void *ctx;
};

// ---------------------------------------------------------------------------------------------
// THE TWO LEGACY BEHAVIOURS THE BLOCK TABLE CANNOT ENCODE, BECAUSE THEY ARE NOT BLOCKS.
//
// The table is extracted from the disassembly as an ordered list of (address, size) block calls, so
// it captures the three version gates exactly -- which block is read, which is skipped, which is
// consumed and dropped. What it CANNOT capture is the work llm_game_load does around those gates
// that emits no block at all, and both instances are on the READ side only:
//
//   ver < 4  0x004478b6..0x004478f4  the 0xe10 block the table marks `discard` is not discarded at
//            all on this path -- it is a 30-record ANSI message queue, and the original walks it
//            converting each 0x78-byte record into a 0xf0-byte wide one in MESSAGE_QUEUE through
//            llm_str_ansi_to_wide. Treating it as a plain discard would leave MESSAGE_QUEUE holding
// whatever an earlier pass left there, silently, on exactly the files that need it.
//   ver < 5  0x0044797e             when the two ver>=5 steps (INVASION_ALERT_TIME, ADVISOR_NEXT_
//            TIME) are SKIPPED, the original also CALLS llm_strat_invasion_alert_reset_all. A skip
//            AND a reset -- so a reader that only skips leaves stale alert state from before the
//            load.
//
// Neither is reachable from a save THIS build wrote (it stamps version 5); they exist to read an
// older *Extermination* file. They are hooks rather than inline code because the driver must stay
// bindable to `net_selftest savetest`, which has no game to call into -- a null hook set is the
// round-trip behaviour, unchanged. WHICH STEP FIRES WHICH HOOK is decided by constexpr predicates
// over the generated table (save_driver.cpp), with static_asserts on the match count, so a table
// reshape breaks the build instead of silently disarming a compat path.
// `src` is the 0xe10 bytes just read (called BEFORE the scratch goes back to the arena) and `dst` is
// MESSAGE_QUEUE, already RESOLVED through state_io -- the driver hands both over so that neither the
// destination address nor the layout leaves the table, and the hook is left with exactly the part the
// table cannot express: the call into llm_str_ansi_to_wide.
struct legacy_hooks {
    void (*message_queue_ansi_to_wide)(void *ctx, const uint8_t *src, uint8_t *dst);
    void (*invasion_alert_reset)(void *ctx);
    void *ctx;
};

// The ver<4 conversion loop's own three constants, read off 0x004478b6..0x004478f4. They are NOT in
// save_table.gen.h and cannot be: the generator parses block CALLS, and this loop makes none. The
// driver static_asserts them against the two MESSAGE_QUEUE steps' sizes, which the generator DOES
// extract, so the pair still has to agree.
inline constexpr uint32_t LEGACY_MESSAGE_RECORDS     = 0x1e; // CMP [i],0x1e @ 0x004478b6
inline constexpr uint32_t LEGACY_MESSAGE_ANSI_STRIDE = 0x78; // IMUL EDX,[i],0x78 @ 0x004478cf
inline constexpr uint32_t LEGACY_MESSAGE_WIDE_STRIDE = 0xf0; // IMUL EBX,[i],0xf0 @ 0x004478de

struct driver_env {
    block_io        io;
    block_workspace ws;
    state_io        st;
    version_table   vt;
    arena          *mem;
    driver_stats   *stats; // may be null

    // The steps the LIVE build must not walk itself -- see step_delegate. Null/0 = walk everything,
    // which is `net_selftest savetest`. The two directions have DIFFERENT sets (that comment again:
    // each helper pair splits the opposite way), so they are two fields rather than one.
    // ST4: may the driver ask a MODULE for a claimed region's bytes instead of reading the address?
    //
    // ONLY IN THE LIVE PROCESS, and this is not a limitation to fix -- it is what `state_io` means.
    // A module's state binding (mh::orders::state()) resolves through the region registry to the
    // game's real .bss; it knows nothing about an injected address space. `net_selftest savetest`
    // runs these same walks over a SPARSE ADDRESS SPACE OF HEAP SLABS with no game present, which is
    // exactly what lets a real .sav round-trip offline -- and in that world an owner would read the
    // wrong memory. So owners serve when the address space is the process's own, and savetest keeps
    // exercising the block path, which is the half it exists to test (the FORMAT). The delegation's
    // own proof is the live save_verify A/B plus its mutation test, not this one.
    //
    // Found the hard way: routing unconditionally made savetest's planet member go *** MISMATCH.
    bool owners_serve = false;

    const step_delegate *delegates; // save_planet
    int                  delegate_count;
    const step_delegate *read_delegates; // load_planet
    int                  read_delegate_count;
    void                *delegate_ctx; // shared: both sides pass the file handle

    // Null = the arena model (savetest). Non-null = the game's streaming model, and it ALSO marks
    // "this is the live container path", which is what makes load_container skip STEP_TAIL: the
    // original never reads the trailer, so there is nothing live to capture and no arena to put it in.
    const member_io *members;
    // STEP_MEDIA, write side. The trailer's contents are a live CD/TOC query, so the live build takes
    // them from llm_build_media_diag_report rather than from a captured image. Null = re-emit
    // `container_image::tail` verbatim, which is what a round-trip needs.
    const void *(*media_source)(void *ctx);

    // load_container's two version-gated non-block behaviours -- see legacy_hooks. Null = neither
    // fires, which is `savetest`'s round-trip: it reads modern files, so neither gate is selected
    // there anyway, and a null hook on a gate that DID select is a hard failure rather than a silent
    // skip (a reader that quietly drops a compat path is the exact bug this struct exists to prevent).
    const legacy_hooks *legacy;
};

// ---------------------------------------------------------------------------------------------
// The four drivers.

// Reads the 40-byte version string, REFUSES a header the table does not contain, then walks
// table::CONTAINER. Version-gated legacy blocks are consumed into scratch and dropped, exactly as the
// original does.
int load_container(const driver_env &env, container_image *img);

// Walks the same table in the other direction. The version string, the members and the media trailer
// come from `img`; every block's contents come from state.
int save_container(const driver_env &env, const container_image &img);

int load_planet(const driver_env &env, planet_image *img);
int save_planet(const driver_env &env, const planet_image &img);

// The embedded-member predicate, `Planets[i].system_index == CurrentSystem && (PlanetStatus[i] != 0
// || PlanetIndex == i)` -- byte for byte the same test at 0x0044741a (save) and 0x004479c2 (load).
// Exposed because the container's READ oracle has to enumerate the member set for itself: it compares
// the per-planet files a load EXTRACTED, and asking the driver's own predicate is the only way to
// know how many there should be, which is what keeps "0 of 0 members identical" from reading green.
bool member_included(const state_io &st, int32_t i);

// The progress[] slice size derived from the CURRENT state, i.e. what save_planet is about to frame
// its eight progress blocks at. Exposed for the live save path, which has no loaded image to take it
// from and must stamp its own `planet_image` with the value save_planet will then check against --
// keeping that check a real one rather than making the field optional. Returns false if the config
// word is not mapped.
bool current_progress_slice(const driver_env &env, uint32_t *out);

// ---------------------------------------------------------------------------------------------
// The region graph's node <-> record marshalling, i.e. the part of llm_map_{save,load}_regions that is
// not framing. Exposed because it carries a finding worth asserting rather than burying:
//
// THE ORIGINAL'S LOAD REVERSES THE LIST. llm_map_load_regions pushes each record's node onto the head
// of the list at 0x0051de7c (`node->next = head; head = node`), and llm_map_save_regions walks from
// that head -- so a save/load/save cycle emits the region records in the OPPOSITE order. It is
// semantically harmless (each record carries its own id and the neighbour arrays are rebuilt through
// REGION_BY_INDEX) but it means the original is not byte-stable across a cycle, which is why the
// round-trip oracle keeps `region_image::records` in FILE order and tests the marshalling separately.
//
// `nodes` is an array of `count` node images of table::REGION_NODE_BYTES each, and `by_index` is a
// table::REGION_BY_INDEX-style array of pointers indexed by region id.

// records -> nodes, in the original's order: list order becomes the REVERSE of file order.
// `list_head_out` receives the index of the resulting head node, or -1 for an empty list.
int unmarshal_region_records(const uint8_t *records, uint32_t count, uint8_t *nodes,
                             uint8_t **by_index, uint32_t by_index_len, int32_t *list_head_out);

// nodes -> records, walking the list from `list_head`. Writes `count` records and returns the number
// of nodes actually walked, which is the quantity the original never checks against N.
int marshal_region_records(const uint8_t *nodes, int32_t list_head, uint8_t *records, uint32_t count,
                           uint32_t *walked_out);

} // namespace mh::save
