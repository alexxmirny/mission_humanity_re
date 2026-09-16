//
// state/nav_trailer.h -- the world blob's CARRIED NAV DECOMPOSITION (tracker LIB-REF).
//
// ---- WHY THIS EXISTS: a rebuild is not the recording's partition ---------------------------------
//
// The map-region ("nav") decomposition is a graph of heap-allocated `llm_map_region` nodes reached
// through four fixed-VA carriers -- MAP_REGION_GRID (256*256 cells), MAP_REGION_BY_INDEX (4096
// pointers), MAP_REGION_LIST_HEAD and MAP_REGION_POOL_FREE_HEAD. A bound-region dump carries those
// four verbatim, which means it carries POINTERS INTO THE RECORDING PROCESS'S HEAP and none of the
// nodes they point at. world_snapshot_dispositions.json's answer was that the importing host must
// REBUILD the decomposition from the carried `passable` plane, with llm_map_build_regions -- the
// same body map_ReadMap calls.
//
// THAT IS MEASURED INSUFFICIENT (2026-09-11). The recording's step-0 decomposition is not
// `map_ReadMap`'s output: it is that output THEN MUTATED by everything the session did before step 0
// -- landing, spawn_ai_base, and every building placement through bldg_footprint_set_passable /
// map_region_split / merge_small_regions. A fresh build_regions over the step-0 `passable` plane
// reproduces "what the builder makes of this plane", never "what incremental maintenance made of its
// history". Measured against the committed fixture the two agree on the region COUNT (180 each) and
// disagree on 2181 of 65536 tiles (3.328%), with 10 more tiles left unclaimed by the rebuild. The
// disposition file predicted this would fail loudly ("a wild read, not a drift"); it DRIFTS -- the
// rebuild is a structurally valid graph, so nothing faults, and the replay diverged 290 steps later
// with an AI unit hover-engaging where the recording deploys to a building.
//
// So the decomposition is CARRIED, in a portable index form, and rebuilt from the facts rather than
// re-derived from an input that no longer describes it. This is the same "carry the facts, re-derive
// the addresses" pattern the whole blob already uses for bases; here the facts are slot ids.
//
// ---- THE PRECEDENT: the game already serializes this graph ---------------------------------------
//
// `llm_map_save_regions` @0x00424773 / `llm_map_load_regions` @0x00424a68 are the game's own
// index-based serialization of exactly this structure, over its LZW block codec. We do NOT reuse the
// bytes (that would need a translated reader and drags LZW framing into a blob that is already
// compressed whole) -- we use the LOADER AS THE FIDELITY CHECKLIST. What it bothers to serialize is
// what this format must carry. Read off 0x00424a68 directly:
//
//   * FOUR header scalars: `counter` (RID_COUNTER, the pool's allocation counter),
//     MAP_REGION_MERGE_THRESHOLD, MAP_REGION_COORD_WRAP_MASK, G_LAST_MAP_INDEX. **This trailer does
//     not carry any of them** -- all four are already bound regions the blob carries verbatim, and
//     duplicating a carried fact is a second source that can disagree with the first.
//   * A 0x40c-byte per-region record: index(u16) x(u8) y(u8) cell_count(u32) neighbor_count(u32),
//     then neighbors[128] AS INDICES and neighbor_data[128]. Our node record is that, plus the two
//     list links (see below), with neighbours as SLOTs rather than indices.
//   * Zeroed by the loader, therefore scratch and not carried here: `route_bfs_dist` (+0x410),
//     `route_mark` (+0x418) and `prev` (+0x41c). It leaves `route_parent` (+0x414) as raw malloc
//     residue, which is the same statement more bluntly: none of the four is load-bearing across a
//     load. We zero all four, and the claim was re-checked against the readers rather than inherited
//     from the loader: `llm_map_region_find_route` @0x00423d86 resets `route_bfs_dist` over the whole
//     active list at the top of every call before any read; `route_parent` is written at
//     BFS-discovery time and only ever followed for nodes discovered in the same call; `route_mark`
//     is the interesting one -- it is NOT find_route-private (llm_map_region_flood_reachable
//     @0x00424ee0 reads it as a cross-call "is this region reachable" gate) and find_route only
//     resets it on its SUCCESS branch, but every static path into a reader was checked and each is
//     preceded in the same call by a producer that re-establishes it from scratch
//     (llm_strat_group_plan_formation_positions @0x0041dbfe does its own full-list reset).
//   * The grid, as one dword per cell holding a region INDEX with 0 meaning "no region", and the
//     terrain_flags plane SEPARATELY as one byte per cell. That second block is why the live cell is
//     8 bytes and not 4 -- see the CELL STRIDE trap below.
//   * `llm_map_init_region_route_step_deltas` @0x00423299, called by the loader at its top. Under
//     schema 2 `build_regions` no longer runs at import and it was the only caller, so the import
//     path must call it exactly as the loader does.
//
// WHERE WE DELIBERATELY CARRY MORE THAN THE GAME DOES. The loader discards the FREE LIST entirely
// (`POOL_FREE_HEAD = 0`, never repopulated). That is sound for the game, which only ever loads at a
// session boundary. It is not sound here: our replay resumes mid-session and then runs
// region_split / merge_small_regions as buildings are placed, `get_next_block` POPS the free head,
// and a different pop order hands a new region a different slot. So the free list and its ORDER are
// carried. The game's loader is a floor on fidelity, not a ceiling.
//
// ---- WHERE IT LIVES: a TRAILER, not a block ------------------------------------------------------
//
// NOT in WORLD_SNAPSHOT_BLOCKS. That table is one block per registry region and `worldtest`'s arms B
// and C walk it per-region; a synthetic entry would make both arms ambiguous about what they are
// measuring. The trailer sits AFTER the last block payload -- past `sizeof(blob_header) +
// payload_len`, so the shared engine's `off != payload_len` completeness check is untouched -- and
// `blob_header::nav_offset` / `nav_len` describe it. It carries its own magic, version and checksum
// because it is outside the engine's `content_hash` by construction.
//
// `world::FORMAT` goes 1 -> 2, which moves the schema fingerprint, which makes every schema-1 blob
// refuse with ERR_SCHEMA (-3) instead of importing without a decomposition. That refusal is the
// point: a v1 fixture under a v2 build is not a fixture with a missing extra, it is a fixture whose
// nav graph would be silently wrong.
//
#pragma once
#include <cstddef>
#include <cstdint>

namespace mh::sim {
// Forward-declared rather than included: this header is the FORMAT, and pulling the whole sim state
// in for one two-pointer struct would make every consumer of the format depend on it.
struct lt_map_region_pool_calls;
} // namespace mh::sim

namespace mh::state::nav {

// 'MHNV' little-endian. Distinct from the blob's own MHWRLD so a truncated blob cannot be read as a
// trailer and vice versa.
inline constexpr uint32_t TRAILER_MAGIC   = 0x564e484du;
inline constexpr uint32_t TRAILER_VERSION = 1u;

// Slot sentinels. Slots are dense 0..node_count-1 and node_count is capped well below these, so a
// sentinel can never collide with a real slot.
inline constexpr uint16_t NO_SLOT      = 0xffffu; // a null pointer (no region / end of list)
inline constexpr uint16_t GRID_PENDING = 0xfffeu; // the grid's 0xFFFFFFFF flood-fill-pending sentinel

// The hard caps. MAX_NODES is deliberately the same 4096 as BY_INDEX rather than a round number: a
// pool holding more live nodes than the index table can address is a state we have never observed
// and would not know how to serialize correctly, so it REFUSES and says so instead of truncating.
inline constexpr uint32_t MAX_NODES  = 4096u;
inline constexpr uint32_t BY_INDEX_N = 4096u;  // MAP_REGION_BY_INDEX is 16384 B of pointers
inline constexpr uint32_t GRID_CELLS = 65536u; // 256 * 256

// ---- the wire format ----------------------------------------------------------------------------

struct trailer_header {
    uint32_t magic;          // TRAILER_MAGIC
    uint32_t version;        // TRAILER_VERSION
    uint32_t node_count;     // active_count + free_count, and the length of nodes[]
    uint32_t by_index_count; // BY_INDEX_N
    uint32_t grid_cells;     // GRID_CELLS
    uint32_t active_count;
    uint32_t free_count;
    uint16_t active_head; // slot of MAP_REGION_LIST_HEAD, or NO_SLOT
    uint16_t free_head;   // slot of MAP_REGION_POOL_FREE_HEAD, or NO_SLOT
    uint32_t checksum;    // FNV-1a32 over every byte after this header
    uint32_t pad_;        // a file format does not end in whatever the compiler left
};
static_assert(sizeof(trailer_header) == 40, "the nav trailer header is a file format");

// Mirrors mh_llm_map_region with every pointer replaced by a slot. 784 bytes; the neighbour arrays
// dominate and are left fixed-stride exactly as the game's own 0x40c record is -- a variable-length
// record would save ~1% of a blob that is zlib'd whole.
struct trailer_node {
    uint16_t index; // the node's own region id (its BY_INDEX position)
    uint8_t  x, y;  // flood-fill seed tile
    uint32_t cell_count;
    uint32_t neighbor_count;
    uint16_t next_slot; // the intrusive list link, carried rather than derived -- see the .cpp
    // WHERE `prev` (+0x41c) WOULD GO, AND WHY IT IS NOT CARRIED. The live record has a `prev`, but
    // the list is SINGLY linked and `prev` is dead: map_block_8_GetNextBlock @0x004222cf slams it to
    // 0 without fixing the old head's, llm_map_region_free @0x0042239a unlinks by walking `next` with
    // a LOCAL predecessor variable and never touches it, llm_map_save_regions @0x00424773 repurposes
    // it as a seen-flag and leaves 1 in every region, and llm_map_load_regions zeroes it. Three
    // writers, three mutually inconsistent values, and no reader anywhere on the region-list paths.
    // Carrying it would also have made capture REFUSE after any save, because `1` is not a pool
    // pointer -- so it is zeroed at import exactly as the game's own loader zeroes it.
    uint16_t pad_;
    uint16_t neighbors[128];     // SLOTs; NO_SLOT at and past neighbor_count
    uint32_t neighbor_data[128]; // plain payload, carried whole (no translation, so no refusal risk)
};
static_assert(sizeof(trailer_node) == 784, "the nav trailer node is a file format");

// nodes[node_count], then by_index[BY_INDEX_N] (u16 slots), then grid[GRID_CELLS] (u16 slots).
inline constexpr size_t MAX_TRAILER_BYTES = sizeof(trailer_header) + MAX_NODES * sizeof(trailer_node) +
                                            BY_INDEX_N * 2u + GRID_CELLS * 2u;

// ---- refusal codes ------------------------------------------------------------------------------
//
// Every one of these is a REFUSAL, never a fallback. A capture that cannot map a pointer must not
// emit NO_SLOT for it: that would silently shrink the graph and the replay would diverge hundreds of
// steps later with nothing pointing back here.
enum nav_err : int {
    NAV_OK               = 0,
    NAV_ERR_ARG          = -1, // null buffer, or capacity below what the pool needs
    NAV_ERR_MAGIC        = -2, // not a nav trailer, or a version this build does not know
    NAV_ERR_TRUNCATED    = -3, // the declared counts do not fit in the bytes provided
    NAV_ERR_CHECKSUM     = -4,
    NAV_ERR_OVERFLOW     = -5, // more pool nodes than MAX_NODES
    NAV_ERR_CYCLE        = -6, // a list walk exceeded the pool size -- a cycle or a corrupt chain
    NAV_ERR_UNMAPPED     = -7, // a live pointer that is on neither list: the pool walk is incomplete
    NAV_ERR_BAD_SLOT     = -8, // a carried slot is out of range
    NAV_ERR_INCONSISTENT = -9, // the relinked lists do not reproduce the carried counts
    NAV_ERR_ALLOC        = -10,
};

// ---- capture (hosted) ---------------------------------------------------------------------------
//
// Reads the LIVE pool through sim_store and writes a trailer into `buf`. `*out_len` receives the
// bytes written. Runs in the recording process, at the same instant as the block capture.
int capture(void *buf, size_t cap, size_t *out_len);

// ---- import (standalone or hosted) --------------------------------------------------------------
//
// Rebuilds the pool from `buf` through the GAME'S OWN ALLOCATOR, relinks both lists, refills
// BY_INDEX, and patches the grid's pointer dword in place leaving terrain_flags exactly as the
// carried block wrote it. Replaces build_regions under schema 2 -- see state/spine.cpp.
//
// THE ALLOCATOR IS A PARAMETER on the second form, and that is not test scaffolding for its own
// sake. `live_lt_map_region_pool_calls()` resolves through MH_CRT: the vendored CRT under
// MH_LIBMH_BUILD, and `mh::call::utils_malloc` -- a FIXED VA inside the loaded game image --
// otherwise. net_selftest.exe is neither: it compiles the module sources without MH_LIBMH_BUILD and
// has no game image mapped, so the live form would call 0x004d0155 in a process where that address
// is not mapped. `navtest` therefore injects its own, which is also what makes the pool this suite
// builds legal to free.
int import(const void *buf, size_t n);
int import(const void *buf, size_t n, const mh::sim::lt_map_region_pool_calls &c);

// Whether a NAV_OK import has happened in this process, for the host's own reporting.
struct import_stats {
    uint32_t nodes    = 0;
    uint32_t active   = 0;
    uint32_t free     = 0;
    uint32_t grid_set = 0; // cells given a non-null region
};
const import_stats &last_import();

} // namespace mh::state::nav
