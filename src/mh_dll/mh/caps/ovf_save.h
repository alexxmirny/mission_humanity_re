//
// caps/ovf_save.h -- sparse overflow tail IO. The composed policy carrier owns complete saves.
//
// Native blocks carry the vanilla window; nonzero overflow records append one MHOV blob and
// an MHOF footer at planet EOF. This tail writer does not write when caps are OFF.
// The COMPLETE save still carries required policy state, including default-off policy.
// Required-support headers, exact member requirements and admission belong exclusively to
// mh_common/include/mh_storage_policy_save.h, not a parallel caps-only version authority.
// File callbacks use the game's opaque Watcom FILE; load_tail restores its position.
//
#pragma once
#include <cstddef>
#include <cstdint>

#include "caps/ovf.h"

namespace mh::caps::ovf {

inline constexpr uint32_t FOOTER_MAGIC = 0x464F484Du; // 'MHOF' little-endian
inline constexpr uint32_t FOOTER_BYTES = 8;           // u32 ovf_len, u32 magic

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
// appended: 0 = no overflow (OFF or nothing past the window), < 0 = failure.
// The composed writer propagates a short/failed tail write as a failed save.
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


} // namespace mh::caps::ovf
