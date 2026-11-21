// mh_mapgen -- the strategic planet-map model (.MP / .mpm) and its byte-exact codec.
//
// Both shipped containers share one body; .mpm adds a tail after the landing spots:
//   0x00  "ver02"
//   0x05  u32 width          0x09 u32 height          0x0d  tileset name, NUL padded to 0x2c
//   0x2d  u32 2*w*h          0x31 u32 w*h             0x35  u32 f1 (f1 record count, always 0)
//   0x39  u32 f2 (trees)     0x3d u32 (w/4)*(h/4)     0x41  i32 f3 (landing spots)
//   0x61  tile plane  w*h*4  (u16 flags, u16 building), COLUMN-major: index = col*h + row
//         passability w*h*1  (1 passable / 0 blocked)
//         f1 records  f1*28  (dead, carried opaque)
//         half-res plane (w/2)*(h/2)*4 (carried opaque)
//         trees       f2*28  {u32 slot, i32 kind, px_x, px_y, next, x(col), y(row)}
//         ore plane   (w/4)*(h/4) blocks * 8 u16, block index = bc*(w/4) + br
//         landings    f3*12  {i32 x(col), y(row), status}
//         .mpm tail   u32 id (CRC-32 (IEEE/zlib) of every preceding byte, stored not recomputed on read) + description bytes (CRLF lines)
// The model carries every byte, so write(read(f)) == f. Pure: bytes in, bytes out, no file or OS API.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mh_mapgen {

constexpr std::size_t   MPM_HEADER_LEN = 0x61;
constexpr std::uint16_t TILE_ID_MASK   = 0x1FFF; // flags & mask = tile index in the tileset
constexpr std::uint16_t TILE_FLAG_BASE = 0x8000; // the only per-tile flag bit seen on disk
constexpr std::size_t   TREE_BYTES     = 28;
constexpr std::size_t   LANDING_BYTES  = 12;
constexpr std::size_t   ORE_WORDS      = 8; // u16 channels per 4x4 ore block (0-3 used)
constexpr std::size_t   NAME_OFF       = 0x0d;
constexpr std::size_t   NAME_LEN       = 0x20; // 0x0d .. 0x2c

struct MpmTree {
    std::uint32_t slot = 0;
    std::int32_t  kind = 0, px_x = 0, px_y = 0, next = 0, x = 0, y = 0;
};

struct MpmLanding {
    std::int32_t x = 0, y = 0, status = 0;
};

struct MpmModel {
    std::array<std::uint8_t, MPM_HEADER_LEN> header_raw{}; // carried verbatim; fields below override on write
    std::uint32_t                            width = 0, height = 0;
    std::vector<std::uint16_t>               flags, building; // w*h each, column-major
    std::vector<std::uint8_t>                passable;        // w*h
    std::vector<std::uint8_t>                f1_raw;          // f1*28
    std::vector<std::uint8_t>                halfres_raw;     // (w/2)*(h/2)*4
    std::vector<MpmTree>                     trees;
    std::vector<std::uint16_t>               ore; // (w/4)*(h/4)*8
    std::vector<MpmLanding>                  landings;
    bool                                     has_tail = false; // .mpm only
    std::uint32_t                            tail_id  = 0;
    std::vector<std::uint8_t>                tail_desc; // description bytes after the id

    std::size_t   cell(std::uint32_t col, std::uint32_t row) const { return std::size_t(col) * height + row; }
    std::uint16_t tile_at(std::uint32_t col, std::uint32_t row) const { return flags[cell(col, row)] & TILE_ID_MASK; }
    std::string   tileset() const; // header name up to the first NUL, exact spelling (e.g. "diuna256.tlo")
};

// Expected plane sizes for a w x h map (w, h multiples of 4).
std::size_t mpm_ore_words(std::uint32_t w, std::uint32_t h);
std::size_t mpm_halfres_bytes(std::uint32_t w, std::uint32_t h);

// Validate the same framing as read_mpm without allocating decoded planes.
bool validate_mpm(const std::uint8_t *data, std::size_t size, std::string &err);

// Parse a whole file image. On failure returns false and fills err (size/magic/consumption checks).
bool read_mpm(const std::uint8_t *data, std::size_t size, MpmModel &out, std::string &err);

// Structural validation (plane sizes agree with width/height, record sizes, dimensions multiples of 4).
bool mpm_validate(const MpmModel &m, std::string &err);

// Everything up to (not including) the .mpm tail. Empty on an invalid model.
std::vector<std::uint8_t> write_mpm_body(const MpmModel &m);

// Body, then (id, description) when has_tail. Empty on an invalid model.
std::vector<std::uint8_t> write_mpm(const MpmModel &m);

// CRC-32, IEEE 802.3 / zlib (reflected, poly 0xEDB88320, init and xorout 0xFFFFFFFF).
std::uint32_t crc32_ieee(const std::uint8_t *data, std::size_t size);

// The .mpm id of a model: CRC-32 of write_mpm_body(m).
std::uint32_t mpm_compute_id(const MpmModel &m);

// Make m an .mpm: set the description and recompute the id from the current body.
void mpm_set_tail(MpmModel &m, const std::string &description);

// Fill header_raw and sized zero planes for a fresh w x h map of `tileset` (design §1). Does not touch
// trees/landings/ore contents beyond sizing them; header counts are rewritten on every write.
void mpm_init(MpmModel &m, const std::string &tileset, std::uint32_t w, std::uint32_t h);

} // namespace mh_mapgen
