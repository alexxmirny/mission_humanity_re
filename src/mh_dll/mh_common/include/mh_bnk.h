// BNK sprite-bank format (BANKI.DAT + BANK_NN.BNK) — parsing, RLE codecs, palette math.
// Format reference: /BNK_FORMAT.md + the "Implementation errata" appendix (this tool's
// corpus measurements), /docs/gfx-sprites.md.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mh::bnk {

struct BnkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#pragma pack(push, 1)
// One BANKI.DAT record. numBanks = fileSize / 12; all-zero record = empty slot.
struct BankiEntry {
    uint32_t indicesSize;     // bytes of each index array; sprite count = indicesSize / 4
    uint32_t spriteDataSize;  // bytes of the RLE sprite-data section
    uint32_t paletteDataSize; // bytes of the palette section; 0 => V2 bank
};
static_assert(sizeof(BankiEntry) == 12);

// The 0x14-byte BNK file header: five LE section sizes (verified on all 64 shipped banks).
// Regenerated from section sizes on repack.
struct BnkFileHeader {
    uint32_t spriteDataSize;
    uint32_t indices1Size;
    uint32_t metadataSize;    // numSprites * 24
    uint32_t paletteDataSize; // 0 for V2
    uint32_t indices2Size;    // == indices1Size for V1, 0 for V2
};
static_assert(sizeof(BnkFileHeader) == 20);

// V1 sprite header (14 bytes, RLE stream at +0x0E). Canvas rule verified on all
// 6841 shipped V1 sprites: W = xLeft + bboxW1 + 1, H = yStart + bboxH1 + 1;
// RLE row 0 lands on canvas row yStart, each row starts at canvas col 0.
struct SpriteHdrV1 {
    uint8_t unk0, version, unk2, unk3; // preserved verbatim (unk0/unk2 look like dims but lie)
    int16_t xLeft;                     // first drawn column
    int16_t yTop;                      // == yStart in all shipped data
    int16_t bboxW1;                    // canvasW - 1 - xLeft
    int16_t bboxH1;                    // canvasH - 1 - yStart
    int16_t yStart;                    // canvas row of the first encoded RLE row
};
static_assert(sizeof(SpriteHdrV1) == 14);

// V2 sprite header (7 bytes, RLE stream at +7).
struct SpriteHdrV2 {
    uint8_t width;       // 32 in all shipped data
    uint8_t heightTiles; // renderer bottom-anchors: dst_y -= heightTiles * 24
    uint8_t unk2, unk3, unk4, unk5;
    uint8_t yStart; // blank canvas rows above the first encoded row
};
static_assert(sizeof(SpriteHdrV2) == 7);

// One record per effect pixel (V1 background-dependent commands), stream order.
struct EffectEntry {
    uint16_t row, col; // canvas coordinates
    uint8_t  type;     // 0x40 BLEND_PAL / 0x60 BLEND_COMPLEX / 0x80 TINT / 0xA0 DARKEN
    uint8_t  dataSize; // payload bytes per pixel: 2 / 2 / 1 / 0
    uint8_t  data[2];  // zero-padded
};
static_assert(sizeof(EffectEntry) == 8);
#pragma pack(pop)

constexpr uint32_t kAbsentSprite       = 0xFFFFFFFFu; // V2 indices1 sentinel for absent sprites
constexpr size_t   kFileHeaderSize     = 0x14;
constexpr size_t   kMetaBytesPerSprite = 24;

inline bool IsEmpty(const BankiEntry &e) {
    return e.indicesSize == 0 && e.spriteDataSize == 0 && e.paletteDataSize == 0;
}
inline bool   IsV2(const BankiEntry &e) { return !IsEmpty(e) && e.paletteDataSize == 0; }
inline size_t NumSprites(const BankiEntry &e) { return e.indicesSize / 4; }

std::vector<BankiEntry> LoadBanki(const std::filesystem::path &datPath);
void                    SaveBanki(const std::filesystem::path &datPath, std::span<const BankiEntry> entries);
std::string             BankFilename(size_t slot); // BANK_%02u.BNK below 100, BANK_%03u.BNK from 100

// A BNK file split into its sections (sizes from the BANKI entry).
struct BnkSections {
    std::vector<uint8_t>                 spriteData;
    std::vector<uint32_t>                indices1;       // offsets into spriteData; may hold kAbsentSprite (V2)
    std::vector<uint8_t>                 metadata;       // numSprites * 24
    std::vector<uint8_t>                 paletteData;    // empty for V2
    std::vector<uint32_t>                indices2;       // offsets into paletteData; empty for V2
    std::array<uint8_t, kFileHeaderSize> storedHeader{}; // as found in the file (cross-check)
    bool                                 isV2 = false;
};

// Validates total size, index monotonicity (V2 sentinels skipped), first offset == 0.
// ctx prefixes error messages (e.g. "BANK_10.BNK").
BnkSections          SplitBnk(std::span<const uint8_t> file, const BankiEntry &entry, const std::string &ctx);
BnkFileHeader        MakeHeader(const BnkSections &s);  // from section sizes
std::vector<uint8_t> AssembleBnk(const BnkSections &s); // regenerated header + sections

std::span<const uint8_t> SpriteSlice(const BnkSections &s, size_t i);  // empty if absent (V2)
std::span<const uint8_t> PaletteSlice(const BnkSections &s, size_t i); // V1; shared palettes: end = next strictly greater offset
std::span<const uint8_t> MetaSlice(const BnkSections &s, size_t i);    // 24 bytes

// ---- decoded sprites -------------------------------------------------------

// V1 pixel classes. Effect pixels are background blends; their command bytes live in
// DecodedV1::effects (idx is 0 for them).
enum class PxKindV1 : uint8_t { Transparent = 0,
                                Opaque      = 1,
                                Effect      = 2 };
enum class PxKindV2 : uint8_t { Transparent = 0,
                                Opaque      = 1,
                                Blend       = 2 };

struct DecodedV1 {
    SpriteHdrV1 header{};
    int         width = 0, height = 0;
#pragma pack(push, 1)
    struct Px {
        uint8_t kind;
        uint8_t idx;
    }; // kind = PxKindV1; idx = palette index when Opaque
#pragma pack(pop)
    std::vector<Px>          pixels;  // width*height, top-down row-major
    std::vector<EffectEntry> effects; // stream order (row-major)
};

struct DecodedV2 {
    SpriteHdrV2 header{};
    int         width = 0, height = 0;
#pragma pack(push, 1)
    struct Px {
        uint16_t rgb565;
        uint8_t  blend;
        uint8_t  kind;
    }; // kind = PxKindV2; blend = raw 0..31 when Blend
#pragma pack(pop)
    std::vector<Px> pixels; // width*height, top-down row-major
};

// Decoders hard-error (BnkError, message prefixed with ctx) on stream overrun, canvas
// violations, or unknown opcodes — the tripwire for spec drift on unseen corpora.
DecodedV1 DecodeV1(std::span<const uint8_t> sprite, const std::string &ctx);
DecodedV2 DecodeV2(std::span<const uint8_t> sprite, const std::string &ctx);

// Canonical encoders (used only for edited sprites; unchanged sprites repack verbatim).
// EncodeV1: header bytes 0-3 verbatim, geometry recomputed within the fixed canvas;
// effect pixels re-inserted from d.effects by exact (row,col) — orphan marker/entry is a
// hard error. Both encoders self-check Decode(Encode(d)) == d and throw on mismatch.
std::vector<uint8_t> EncodeV1(const DecodedV1 &d, const std::string &ctx);
std::vector<uint8_t> EncodeV2(const DecodedV2 &d, const std::string &ctx);

// ---- color -----------------------------------------------------------------

struct Rgb888 {
    uint8_t r, g, b;
};
Rgb888   Rgb565To888(uint16_t c);
uint16_t Rgb888To565(Rgb888 c);

} // namespace mh::bnk
