//
// caps/ovf_save.h -- the RAISED-CAPS SAVE (patching:PT-CAP8 S3, slice G; spec tmp/capraise/s23/spec.md section 6).
//
// A save written with the cap raise ON may hold records past the vanilla window (ovf.h). The vanilla save blocks
// only ever carry the window, so the overflow rides in two additive places:
//
//   * a PER-PLANET TRAILER at the very end of `save%02d.dat` / `qsave%02d.dat`:
//         [MHOV blob (ovf.h)] [u32 ovf_len] [u32 'MHOF']          -- the 8-byte footer is the LAST 8 bytes of the file
//     written only while raised() and only when some record is non-zero past the window. The vanilla reader never
//     reads that far, so a trailer is invisible to retail / OFF builds that would never look.
//   * a CONTAINER VERSION STAMP: the 40-byte version string that opens a `.sav` becomes a sixth value no build's table
//     holds when ANY member the container embeds carries a trailer. Retail, v0.2.1 and an OFF v0.2.2 therefore refuse
//     the save at the header -- before any block is read, state untouched. That is the "refuse" (a warn has no safe
//     meaning: with the limits OFF the game's loops stop at the vanilla bound and the loaded overflow would sit
//     unaddressed).
//
// EVERYTHING HERE IS PURE: the file, the member table and the version table are SEAMS, so `net_selftest ovftest`
// drives it over fake files (position restored on every path), a fixture planet table (the stamp predicate) and
// synthetic version tables (the stamp differs from all six slots). caps_install.cpp wires the game side.
//
// OFF IS BYTE-IDENTICAL. save_tail() returns before touching the file unless raised(); stamp_needed() likewise;
// load_tail() and version_check() are read-only unless a footer / the stamp is found, and the file position is
// restored on every path.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "caps/ovf.h"

namespace mh::caps::ovf {

inline constexpr uint32_t FOOTER_MAGIC  = 0x464F484Du; // 'MHOF' little-endian
inline constexpr uint32_t FOOTER_BYTES  = 8;           // u32 ovf_len, u32 magic
inline constexpr uint32_t VERSION_BYTES = 40;          // the container header's version string

// ---- the file seam (production: the game's own fseek / ftell / fread / fwrite on its Watcom FILE) -------------
struct file_io {
    int32_t (*tell)(void *f);                                // -1 = error
    int (*seek)(void *f, int32_t off, int whence);           // whence 0 SET / 1 CUR / 2 END; 0 = ok
    uint32_t (*read)(void *f, void *dst, uint32_t n);        // bytes read
    uint32_t (*write)(void *f, const void *src, uint32_t n); // bytes written
};

void set_save_logger(void (*fn)(const char *));

// The shared encode/decode buffer (max_bytes()), allocated on first use; null when the allocation failed.
uint8_t *scratch();
void     scratch_release(); // tests

// ---- the save tail -------------------------------------------------------------------------------------------
// Append the MHOV blob + footer to `f`, which sits at the end of a freshly written planet file. Returns the bytes
// appended: 0 = nothing (OFF, or nothing past the window: the file stays vanilla), < 0 = failure (logged; the
// file is then a plain vanilla save with a truncated tail the loader treats as "no footer").
int save_tail(const file_io &io, void *f, int planet);

// ---- the load tail -------------------------------------------------------------------------------------------
enum load_tail_result : int {
    LT_NONE        = 0,  // no footer: exactly the vanilla reader (a tell / seek / read / seek only)
    LT_APPLIED     = 1,  // footer found, raised: the overflow records are in the wide arrays
    LT_REFUSED_OFF = -1, // footer found, raise OFF: *fail incremented, refusal flag set
    LT_REFUSED_BAD = -2, // footer found, raised, blob refused (corrupt / other geometry): *fail incremented
};
// `f` is positioned mid-file (after the path blocks: the hook sits behind the free-count block); the position is restored on EVERY path.
int load_tail(const file_io &io, void *f, int32_t *fail);
// The header features (ovf.h FEAT_*) of the trailer the LAST load_tail() applied; 0 when it applied none (no footer, refused, or a v1 trailer).
uint32_t last_load_features();

// ---- the refusal flag (consumed by the load-failure message hook) --------------------------------------------
bool refused_raised();
void set_refused_raised(bool v);
bool take_refused_raised(); // read + clear

// ---- the container stamp -------------------------------------------------------------------------------------
inline constexpr uint32_t STAMP_OFFSET = 29; // bytes 29..32 of the 40-byte slot
// out = slot with bytes 29..32 replaced by "MHC1".
void make_stamp(const uint8_t *slot, uint8_t *out);
bool is_stamp(const uint8_t *hdr, const uint8_t *slot);

// The members a container will embed: the 0x44741a predicate of game_SaveGame (docs/save-format.md
// "embedded-member count"): Planets[i].system_index == CurrentSystem && (G_PLANET_STATUS[i] != 0 || G_PLANET_INDEX == i),
// i = 1..0x1f. `footer_of` says whether member i's file ends in a valid MHOF footer.
inline constexpr int FIRST_MEMBER = 1;
inline constexpr int LAST_MEMBER  = 0x1f;
struct member_probe {
    void *ctx;
    int32_t (*system_of)(void *ctx, int i);
    int32_t (*status_of)(void *ctx, int i);
    bool (*footer_of)(void *ctx, int i);
    int32_t current_system;
    int32_t current_planet;
};
bool member_included(const member_probe &mp, int i);
// raised() AND some included member carries a footer.
bool stamp_needed(const member_probe &mp);

enum version_result : int {
    VC_PASS     = 0, // not the stamp: untouched
    VC_ACCEPTED = 1, // the stamp and raised(): hdr rewritten to the table slot (the vanilla compare then accepts it)
    VC_REFUSED  = 2, // the stamp and OFF: hdr left (the vanilla compare fails -> the load is refused), flag set
};
// `hdr` = the 40 bytes the game just read; `slot` = the table slot of the build's own version.
int version_check(uint8_t *hdr, const uint8_t *slot);

// Does the file end in a valid footer (sane length)? Restores the position. Used by the member probe.
bool file_has_footer(const file_io &io, void *f);

} // namespace mh::caps::ovf
