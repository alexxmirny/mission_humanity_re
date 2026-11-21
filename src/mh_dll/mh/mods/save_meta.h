//
// mods/save_meta.h -- DATAEXT1: bounded mod-origin metadata in the OUTER .sav tail.
// The binary layout and load/provenance rules are documented in docs/save-format.md.
//
// The outer container game_SaveGame writes is
//     40-byte version string, 19 blocks, length-prefixed embedded planet files, one trailing
//     0x400 media block (write-only, no reader ever consumes it -- docs/save-format.md).
// This module appends, AFTER that media block and only when the game state has a mod origin worth
// naming:
//         [carrier blob] [u32 blob_len] [u32 'MHM1']        -- the footer is the LAST 8 bytes
// the exact idiom the caps overflow trailer uses on the per-planet files ([MHOV][len]['MHOF'],
// caps/ovf_save.h). A vanilla loader stops after the planet loop and never reads the media block,
// so appended bytes are invisible to it; libmh's round-trip driver re-emits the captured tail
// verbatim (an extension after the media block already survives one byte-identically).
//
// THE CARRIER (version 1, little-endian, every count bounded; a reader validates BEFORE it reads):
//     u8  version   = 1
//     u8  flags     = 0x01 exactly (bit0: the set is known; every other bit is malformed -- no
//                      silent forward compatibility: a variant gets version 2)
//     u16 count     (0..MAX_MODS), entries in enabled order (later wins), exactly consuming the blob
//     u8  digest[32]  the mounted snapshot's content digest (mh::mods::mounted_snapshot()->digest)
//     per entry: u8 id_len (1..32), u8 ver_len (0..64), id bytes, version bytes
// ID grammar: exactly the catalog's, ASCII [a-z0-9_-]; embedded NUL / other bytes / duplicates are
// malformed. VERSION: opaque UTF-8, strlen == ver_len, no C0/C1 control or DEL. The bounds match
// the catalog's frozen limits (id 1..32, version 0..64 bytes) and are COORDINATED: the catalog
// rejects an active set larger than MAX_MODS at mount time, so a valid mounted set is always
// representable and a save can never hit the bound. Anything that does not parse exactly is
// MALFORMED: treated as absent (one log line), and it can never weaken a structural gate -- the
// caps stamp lives in the 40-byte header and the caps trailers inside the planet members, sharing
// no bytes with this footer.
//
// PROVENANCE (the explicit contract; no silent relabeling):
//     BOOT     the state was derived from the current boot (fresh game / session rebuild). A save
//              records the MOUNTED set when it is non-empty, nothing when it is empty -- an
//              ordinary no-mod save stays byte-vanilla.
//     LOADED   the state came from a save whose carrier parsed. A save records THAT set (ids,
//              versions, digest as read), even when the mounted set differs or is empty: loading
//              saveA under boot B and re-saving must not relabel the state as B.
//     UNKNOWN  the state came from a save WITHOUT a carrier (a legacy or previously-uncertain
//              save). A save appends NOTHING: absence is the on-disk marker for unknown, so the
//              uncertainty survives every resave -- the next load under an active set warns again
//              instead of silently adopting the mounted set. A confirmed full new-game config restore returns the state to BOOT, so the next New
//              Game records the mounted set again.
//
// WARNING (one localized deferred notice per successful load, never modal, never inside the load):
// the header-read probe compares the carrier against the MOUNTED set and queues the verdict; the
// load-succeeded branch prints it once after the blocks are in (so the game's own message queue is
// already restored and our entry is not overwritten by the file). A failure load discards the
// stage. The probe validates the footer bounds before reading anything into the scratch buffer,
// restores the FILE position on every path, preserves the read result/registers, and never touches
// caps state or failure counters.
//
// EVERYTHING HERE IS PURE over the file_io and active_set_source seams (production wires the former
// to the game's CRT calls and the latter to mh::mods::mounted_snapshot(); `net_selftest
// modsavetest` drives fixtures). caps_install owns the outer carrier sites; G17 and the replay
// boundary owner compose new-game and continuation provenance; the mods boot must mount nothing unless hooks_ready() -- a half-mounted feature could
// neither label its saves nor clear provenance at New Game, so a required-hook failure is the
// whole-set stock fallback.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include "mh_mod_catalog.h"

namespace mh::mods::save {

// The same shape as mh::caps::ovf::file_io. Re-declared on purpose: the mod layer stays
// independent of the caps headers in every configuration (relocate on/off).
struct file_io {
    int32_t (*tell)(void *f);                                // -1 = error
    int (*seek)(void *f, int32_t off, int whence);           // whence 0 SET / 1 CUR / 2 END; 0 = ok
    uint32_t (*read)(void *f, void *dst, uint32_t n);        // bytes read
    uint32_t (*write)(void *f, const void *src, uint32_t n); // bytes written
};

// The mounted-set reader (the catalog's immutable boot snapshot). A null snapshot (an ordinary
// no-mod boot before/without any mount) means count 0 and the stock identity -- the same values a
// failure boot publishes; no fake snapshot object is required. Production:
// install_active_set_source().
struct active_set_source {
    void *ctx;
    unsigned (*count)(void *ctx);
    bool (*id_of)(void *ctx, unsigned i, char *out, unsigned cap);      // NUL-terminated; false = bad index/cap
    bool (*version_of)(void *ctx, unsigned i, char *out, unsigned cap); // NUL-terminated; false = bad index/cap
    void (*digest_of)(void *ctx, unsigned char out[32]);
};

void set_logger(void (*fn)(const char *));
void set_active_set_source(const active_set_source *s); // null = stock (count 0)
void install_active_set_source();                       // production: wired ONCE by the resource boot arm, after mount_snapshot

// Hook readiness for the mods boot: the boot must consult this BEFORE publishing any snapshot and
// mount nothing (whole-stock fallback, its own readable reason) unless it is true. Stored by
// caps_install plus the G17 and continuation owners report their actual installation results.
void set_hooks_ready(bool armed);
void set_new_game_ready(bool armed);
void set_continue_ready(bool armed);
bool hooks_ready();

// ---- carrier bounds (COORDINATED with the catalog's mount-time validation) ------------------------------------
inline constexpr uint32_t FOOTER_MAGIC    = 0x314D484Du; // 'MHM1' little-endian
inline constexpr uint32_t FOOTER_BYTES    = 8;           // u32 blob_len, u32 magic
inline constexpr uint32_t CARRIER_VERSION = 1;
inline constexpr uint32_t CARRIER_FLAGS   = 0x01;                                                           // bit0 = known; the ONLY accepted value
inline constexpr unsigned MAX_MODS        = mh::mods::MODS_MAX;                                             // the catalog must reject larger sets AT MOUNT
inline constexpr unsigned MAX_ID_BYTES    = 32;                                                             // the manifest contract's id limit
inline constexpr unsigned MAX_VER_BYTES   = 64;                                                             // the catalog's frozen version limit (bytes)
inline constexpr uint32_t MIN_BLOB_BYTES  = 36;                                                             // version + flags + count + digest
inline constexpr uint32_t MAX_BLOB_BYTES  = MIN_BLOB_BYTES + MAX_MODS * (2 + MAX_ID_BYTES + MAX_VER_BYTES); // exact maximum encoded size

// ---- write side ----------------------------------------------------------------------------------------------
// Append the origin carrier at the current (EOF) position of a freshly written container.
// Returns the bytes appended; 0 = nothing to append (BOOT with an empty mounted set, or UNKNOWN --
// the file stays byte-vanilla); < 0 = failure (logged; never fails the save itself).
int append_origin(const file_io &io, void *f);

// ---- read side -----------------------------------------------------------------------------------------------
enum class verdict { MATCH,
                     MISMATCH,
                     LEGACY_UNCERTAIN,
                     NONE };
// MATCH            the carrier equals the mounted set: silent.
// MISMATCH         a known carrier differs from the mounted set (ids, versions, order, digest, or
//                  the save has mods and none is mounted): warn, continue.
// LEGACY_UNCERTAIN no (usable) carrier while mods are mounted: warn, continue.
// NONE             nothing to say (no carrier, no mounted mods).

// Bounded EOF probe of the container, BEFORE the blocks load. Footer bounds are validated (wide
// arithmetic, against MIN/MAX and the footer's own position) BEFORE anything is read into the
// scratch buffer. Restores the file position on EVERY path; never touches caps state or failure
// counters; stages the provenance outcome (commit_staged applies it when the load succeeds).
verdict probe_container_io(const file_io &io, void *f);

// The load succeeded (llm_menu_loadgame_confirm's success branch, before the handover to the game):
// apply the staged provenance and return the verdict the caller should announce. A second probe
// overwrites the stage; a session reset clears it.
verdict commit_staged();

// G17 calls this only after capture: a complete restore establishes BOOT; an incomplete or
// disabled restore that leaves different tables establishes UNKNOWN. A soft campaign continue
// preserves an established process origin and otherwise becomes UNKNOWN.
void on_new_game(bool boot_config_confirmed);
void on_campaign_continue();

// tests: back to the fresh-boot state, no logger, no source, hooks not ready.
void reset_for_test();

} // namespace mh::mods::save

// Naked-stub entry points (caps_install.cpp's inline asm calls these; C linkage so the asm can
// name them) and the FILE-capture slot both header-read stubs store into before the read.
extern "C" {
extern uintptr_t g_hdr_read_file;                      // the container FILE* at the 40-byte header read (0 = none)
void             mh_mods_save_on_header_probe(void);   // llm_game_load's header read (post-read)
void             mh_mods_save_commit_and_notify(void); // the load-succeeded branch: commit + one notice
}
