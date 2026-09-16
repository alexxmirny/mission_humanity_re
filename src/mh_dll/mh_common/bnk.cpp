#include "include/mh_bnk.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <tuple>
#include <unordered_map>

namespace mh::bnk {

namespace {

// V1 RLE opcodes
constexpr uint8_t kEndRow           = 0xFE;
constexpr uint8_t kEndSprite        = 0xFF;
constexpr uint8_t kTypeSkip         = 0x00;
constexpr uint8_t kTypeOpaque       = 0x20;
constexpr uint8_t kTypeBlendPal     = 0x40;
constexpr uint8_t kTypeBlendComplex = 0x60;
constexpr uint8_t kTypeTint         = 0x80;
constexpr uint8_t kTypeDarken       = 0xA0;
constexpr uint8_t kTypeSkipLines    = 0xC0;
constexpr int     kMaxRun           = 31; // 5-bit count

uint8_t EffectPayloadSize(uint8_t type) {
    switch (type) {
        case kTypeBlendPal:
        case kTypeBlendComplex: return 2;
        case kTypeTint: return 1;
        case kTypeDarken: return 0;
        default: return 0xFF; // not an effect type
    }
}

[[noreturn]] void Fail(const std::string &ctx, const std::string &msg) {
    throw BnkError(ctx + ": " + msg);
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw BnkError("cannot open " + p.string());
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    return data;
}

} // namespace

// ---- BANKI.DAT --------------------------------------------------------------

std::vector<BankiEntry> LoadBanki(const std::filesystem::path &datPath) {
    auto data = ReadWholeFile(datPath);
    if (data.size() % sizeof(BankiEntry) != 0)
        throw BnkError(datPath.string() + ": size " + std::to_string(data.size()) +
                       " is not a multiple of 12");
    std::vector<BankiEntry> entries(data.size() / sizeof(BankiEntry));
    std::memcpy(entries.data(), data.data(), data.size());
    return entries;
}

void SaveBanki(const std::filesystem::path &datPath, std::span<const BankiEntry> entries) {
    std::ofstream f(datPath, std::ios::binary);
    if (!f) throw BnkError("cannot write " + datPath.string());
    f.write(reinterpret_cast<const char *>(entries.data()), entries.size() * sizeof(BankiEntry));
    if (!f) throw BnkError("write failed: " + datPath.string());
}

std::string BankFilename(size_t slot) {
    return slot < 100 ? std::format("BANK_{:02}.BNK", slot) : std::format("BANK_{:03}.BNK", slot);
}

// ---- sections ---------------------------------------------------------------

BnkSections SplitBnk(std::span<const uint8_t> file, const BankiEntry &entry, const std::string &ctx) {
    if (IsEmpty(entry)) Fail(ctx, "empty BANKI entry has no BNK file");
    const size_t n        = NumSprites(entry);
    const bool   v2       = IsV2(entry);
    const size_t expected = kFileHeaderSize + entry.spriteDataSize + entry.indicesSize +
                            entry.indicesSize * 6 +
                            (v2 ? 0 : entry.paletteDataSize + entry.indicesSize);
    if (file.size() != expected)
        Fail(ctx, std::format("file size {} != expected {} from BANKI entry", file.size(), expected));

    BnkSections s;
    s.isV2 = v2;
    std::memcpy(s.storedHeader.data(), file.data(), kFileHeaderSize);
    size_t pos  = kFileHeaderSize;
    auto   take = [&](size_t len) {
        std::span<const uint8_t> part = file.subspan(pos, len);
        pos += len;
        return part;
    };
    auto sd = take(entry.spriteDataSize);
    s.spriteData.assign(sd.begin(), sd.end());
    auto i1 = take(entry.indicesSize);
    s.indices1.resize(n);
    std::memcpy(s.indices1.data(), i1.data(), i1.size());
    auto md = take(entry.indicesSize * 6);
    s.metadata.assign(md.begin(), md.end());
    if (!v2) {
        auto pd = take(entry.paletteDataSize);
        s.paletteData.assign(pd.begin(), pd.end());
        auto i2 = take(entry.indicesSize);
        s.indices2.resize(n);
        std::memcpy(s.indices2.data(), i2.data(), i2.size());
    }

    // Validate indices1: first valid offset 0, monotonic over valid entries, in bounds.
    bool     sawValid = false;
    uint32_t prev     = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t off = s.indices1[i];
        if (off == kAbsentSprite) {
            if (!v2) Fail(ctx, std::format("indices1[{}] is the absent-sprite sentinel in a V1 bank", i));
            continue;
        }
        if (off > entry.spriteDataSize)
            Fail(ctx, std::format("indices1[{}] = {} exceeds spriteDataSize {}", i, off, entry.spriteDataSize));
        if (!sawValid && off != 0)
            Fail(ctx, std::format("first valid sprite offset is {} (expected 0)", off));
        if (sawValid && off < prev)
            Fail(ctx, std::format("indices1[{}] = {} is not monotonic (prev {})", i, off, prev));
        prev     = off;
        sawValid = true;
    }
    for (size_t i = 0; i < s.indices2.size(); i++) {
        if (s.indices2[i] > entry.paletteDataSize)
            Fail(ctx, std::format("indices2[{}] = {} exceeds paletteDataSize {}", i, s.indices2[i], entry.paletteDataSize));
    }
    return s;
}

BnkFileHeader MakeHeader(const BnkSections &s) {
    BnkFileHeader h{};
    h.spriteDataSize  = static_cast<uint32_t>(s.spriteData.size());
    h.indices1Size    = static_cast<uint32_t>(s.indices1.size() * 4);
    h.metadataSize    = static_cast<uint32_t>(s.metadata.size());
    h.paletteDataSize = static_cast<uint32_t>(s.paletteData.size());
    h.indices2Size    = static_cast<uint32_t>(s.indices2.size() * 4);
    return h;
}

std::vector<uint8_t> AssembleBnk(const BnkSections &s) {
    BnkFileHeader        h = MakeHeader(s);
    std::vector<uint8_t> out;
    out.reserve(kFileHeaderSize + s.spriteData.size() + s.indices1.size() * 4 +
                s.metadata.size() + s.paletteData.size() + s.indices2.size() * 4);
    auto append = [&](const void *p, size_t len) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        out.insert(out.end(), b, b + len);
    };
    append(&h, sizeof(h));
    append(s.spriteData.data(), s.spriteData.size());
    append(s.indices1.data(), s.indices1.size() * 4);
    append(s.metadata.data(), s.metadata.size());
    append(s.paletteData.data(), s.paletteData.size());
    append(s.indices2.data(), s.indices2.size() * 4);
    return out;
}

std::span<const uint8_t> SpriteSlice(const BnkSections &s, size_t i) {
    uint32_t off = s.indices1.at(i);
    if (off == kAbsentSprite) return {};
    uint32_t end = static_cast<uint32_t>(s.spriteData.size());
    for (size_t j = i + 1; j < s.indices1.size(); j++) {
        if (s.indices1[j] != kAbsentSprite) {
            end = s.indices1[j];
            break;
        }
    }
    return std::span<const uint8_t>(s.spriteData).subspan(off, end - off);
}

std::span<const uint8_t> PaletteSlice(const BnkSections &s, size_t i) {
    uint32_t off = s.indices2.at(i);
    // Shared palettes: end = the next STRICTLY greater offset anywhere in indices2.
    uint32_t end = static_cast<uint32_t>(s.paletteData.size());
    for (uint32_t o : s.indices2)
        if (o > off && o < end) end = o;
    return std::span<const uint8_t>(s.paletteData).subspan(off, end - off);
}

std::span<const uint8_t> MetaSlice(const BnkSections &s, size_t i) {
    return std::span<const uint8_t>(s.metadata).subspan(i * kMetaBytesPerSprite, kMetaBytesPerSprite);
}

// ---- V1 decode --------------------------------------------------------------

DecodedV1 DecodeV1(std::span<const uint8_t> sprite, const std::string &ctx) {
    if (sprite.size() < sizeof(SpriteHdrV1) + 1)
        Fail(ctx, std::format("V1 sprite slice too small ({} bytes)", sprite.size()));
    DecodedV1 d;
    std::memcpy(&d.header, sprite.data(), sizeof(SpriteHdrV1));
    const auto &h = d.header;
    if (h.xLeft < 0 || h.bboxW1 < 0 || h.bboxH1 < 0 || h.yStart < 0)
        Fail(ctx, "negative V1 header geometry");
    d.width  = h.xLeft + h.bboxW1 + 1;
    d.height = h.yStart + h.bboxH1 + 1;
    if (d.width > 4096 || d.height > 4096)
        Fail(ctx, std::format("implausible canvas {}x{}", d.width, d.height));
    d.pixels.assign(static_cast<size_t>(d.width) * d.height, {0, 0});

    int    row = h.yStart, col = 0;
    size_t pos  = sizeof(SpriteHdrV1);
    auto   need = [&](size_t bytes) {
        if (pos + bytes > sprite.size()) Fail(ctx, "V1 RLE stream overrun");
    };
    auto checkDraw = [&](int count) {
        if (row >= d.height || col + count > d.width)
            Fail(ctx, std::format("V1 draw of {} px at row {} col {} exceeds canvas {}x{}",
                                  count, row, col, d.width, d.height));
    };
    for (;;) {
        need(1);
        uint8_t b = sprite[pos++];
        if (b == kEndSprite) break;
        if (b == kEndRow) {
            row++;
            col = 0;
            continue;
        }
        const uint8_t type  = b & 0xE0;
        const int     count = b & 0x1F;
        switch (type) {
            case kTypeSkip:
                col += count;
                if (col > d.width) Fail(ctx, "V1 SKIP past row end");
                break;
            case kTypeOpaque: {
                need(count);
                checkDraw(count);
                for (int k = 0; k < count; k++)
                    d.pixels[static_cast<size_t>(row) * d.width + col++] = {
                        static_cast<uint8_t>(PxKindV1::Opaque), sprite[pos++]};
                break;
            }
            case kTypeBlendPal:
            case kTypeBlendComplex:
            case kTypeTint:
            case kTypeDarken: {
                const uint8_t per = EffectPayloadSize(type);
                need(static_cast<size_t>(count) * per);
                checkDraw(count);
                for (int k = 0; k < count; k++) {
                    EffectEntry e{};
                    e.row      = static_cast<uint16_t>(row);
                    e.col      = static_cast<uint16_t>(col);
                    e.type     = type;
                    e.dataSize = per;
                    for (int m = 0; m < per; m++) e.data[m] = sprite[pos++];
                    d.effects.push_back(e);
                    d.pixels[static_cast<size_t>(row) * d.width + col++] = {
                        static_cast<uint8_t>(PxKindV1::Effect), 0};
                }
                break;
            }
            case kTypeSkipLines:
                row += count;
                col = 0;
                break;
            default:
                Fail(ctx, std::format("unknown V1 RLE opcode 0x{:02X}", b));
        }
        if (row > d.height) Fail(ctx, "V1 rows past canvas height");
    }
    return d;
}

// ---- V1 encode --------------------------------------------------------------

namespace {

void EmitChunked(std::vector<uint8_t> &out, uint8_t type, int total,
                 const std::function<void(int chunkStart, int chunkLen)> &payload) {
    int done = 0;
    while (done < total) {
        int chunk = std::min(kMaxRun, total - done);
        out.push_back(type | static_cast<uint8_t>(chunk));
        if (payload) payload(done, chunk);
        done += chunk;
    }
}

} // namespace

std::vector<uint8_t> EncodeV1(const DecodedV1 &d, const std::string &ctx) {
    const int W = d.width, H = d.height;
    if (static_cast<size_t>(W) * H != d.pixels.size())
        Fail(ctx, "pixel buffer size mismatch");
    auto at = [&](int r, int c) -> const DecodedV1::Px & {
        return d.pixels[static_cast<size_t>(r) * W + c];
    };

    // Effect lookup by (row,col); every entry must be consumed exactly once.
    std::unordered_map<uint32_t, const EffectEntry *> fx;
    for (const auto &e : d.effects) {
        uint32_t key = (static_cast<uint32_t>(e.row) << 16) | e.col;
        if (!fx.emplace(key, &e).second)
            Fail(ctx, std::format("duplicate effect entry at row {} col {}", e.row, e.col));
    }
    size_t fxUsed = 0;
    auto   fxAt   = [&](int r, int c) -> const EffectEntry     *{
        auto it = fx.find((static_cast<uint32_t>(r) << 16) | static_cast<uint32_t>(c));
        return it == fx.end() ? nullptr : it->second;
    };

    // Recompute header geometry within the fixed canvas.
    int minCol = W, firstRow = H;
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++)
            if (at(r, c).kind != static_cast<uint8_t>(PxKindV1::Transparent)) {
                if (c < minCol) minCol = c;
                if (r < firstRow) { firstRow = r; }
            }

    SpriteHdrV1 h = d.header; // bytes 0-3 verbatim
    if (firstRow < H) {       // has content
        h.xLeft  = static_cast<int16_t>(std::min<int>(d.header.xLeft, minCol));
        h.bboxW1 = static_cast<int16_t>(W - 1 - h.xLeft);
        h.yStart = h.yTop = static_cast<int16_t>(std::min<int>(d.header.yStart, firstRow));
        h.bboxH1          = static_cast<int16_t>(H - 1 - h.yStart);
    }

    std::vector<uint8_t> out(sizeof(SpriteHdrV1));
    std::memcpy(out.data(), &h, sizeof(h));

    if (firstRow == H) { // fully empty grid — matches the 8 shipped empties
        out.push_back(kEndSprite);
    } else {
        auto rowBlank = [&](int r) {
            for (int c = 0; c < W; c++)
                if (at(r, c).kind != static_cast<uint8_t>(PxKindV1::Transparent)) return false;
            return true;
        };
        int r = h.yStart;
        while (r < H) {
            if (rowBlank(r)) {
                int n = 0;
                while (r + n < H && rowBlank(r + n)) n++;
                EmitChunked(out, kTypeSkipLines, n, nullptr);
                r += n;
                continue;
            }
            // Content row: trailing transparent pixels are not emitted.
            int lastContent = W - 1;
            while (at(r, lastContent).kind == static_cast<uint8_t>(PxKindV1::Transparent)) lastContent--;
            int c = 0;
            while (c <= lastContent) {
                uint8_t kind = at(r, c).kind;
                if (kind == static_cast<uint8_t>(PxKindV1::Transparent)) {
                    int n = 0;
                    while (c + n <= lastContent &&
                           at(r, c + n).kind == static_cast<uint8_t>(PxKindV1::Transparent))
                        n++;
                    EmitChunked(out, kTypeSkip, n, nullptr);
                    c += n;
                } else if (kind == static_cast<uint8_t>(PxKindV1::Opaque)) {
                    int n = 0;
                    while (c + n <= lastContent &&
                           at(r, c + n).kind == static_cast<uint8_t>(PxKindV1::Opaque))
                        n++;
                    EmitChunked(out, kTypeOpaque, n, [&](int start, int len) {
                        for (int k = 0; k < len; k++) out.push_back(at(r, c + start + k).idx);
                    });
                    c += n;
                } else { // Effect: group consecutive markers sharing the entry type
                    const EffectEntry *first = fxAt(r, c);
                    if (!first)
                        Fail(ctx, std::format(
                                      "effect pixel at row {} col {} has no effects.bin entry — effect "
                                      "pixels can only be moved together with matching effects.bin edits",
                                      r, c));
                    int                              n = 0;
                    std::vector<const EffectEntry *> run;
                    while (c + n <= lastContent &&
                           at(r, c + n).kind == static_cast<uint8_t>(PxKindV1::Effect)) {
                        const EffectEntry *e = fxAt(r, c + n);
                        if (!e)
                            Fail(ctx, std::format("effect pixel at row {} col {} has no effects.bin entry", r, c + n));
                        if (e->type != first->type) break;
                        run.push_back(e);
                        n++;
                    }
                    EmitChunked(out, first->type, n, [&](int start, int len) {
                        for (int k = 0; k < len; k++) {
                            const EffectEntry *e = run[start + k];
                            for (int m = 0; m < e->dataSize; m++) out.push_back(e->data[m]);
                        }
                    });
                    fxUsed += n;
                    c += n;
                }
            }
            out.push_back(kEndRow);
            r++;
        }
        out.push_back(kEndSprite);
    }

    if (fxUsed != fx.size())
        Fail(ctx, std::format(
                      "{} effects.bin entries were not consumed (their pixels are no longer effect-marked) "
                      "— effect pixels can only be moved together with matching effects.bin edits",
                      fx.size() - fxUsed));

    // Self-check: the encoded stream must decode back to exactly this sprite.
    DecodedV1 back = DecodeV1(out, ctx + " (self-check)");
    bool      ok   = back.width == W && back.height == H &&
              std::memcmp(back.pixels.data(), d.pixels.data(), d.pixels.size() * sizeof(DecodedV1::Px)) == 0 &&
              back.effects.size() == d.effects.size();
    if (ok) {
        auto key = [](const EffectEntry &e) {
            return std::tuple(e.row, e.col, e.type, e.dataSize, e.data[0], e.data[1]);
        };
        auto a = back.effects, b = d.effects;
        std::sort(a.begin(), a.end(), [&](auto &x, auto &y) { return key(x) < key(y); });
        std::sort(b.begin(), b.end(), [&](auto &x, auto &y) { return key(x) < key(y); });
        for (size_t i = 0; i < a.size() && ok; i++)
            ok = key(a[i]) == key(b[i]);
    }
    if (!ok) Fail(ctx, "V1 encoder self-check failed (Decode(Encode(x)) != x)");
    return out;
}

// ---- V2 decode --------------------------------------------------------------

DecodedV2 DecodeV2(std::span<const uint8_t> sprite, const std::string &ctx) {
    if (sprite.size() < sizeof(SpriteHdrV2) + 1)
        Fail(ctx, std::format("V2 sprite slice too small ({} bytes)", sprite.size()));
    DecodedV2 d;
    std::memcpy(&d.header, sprite.data(), sizeof(SpriteHdrV2));

    // Parse into per-row runs first: width may need to be measured (header width is 32
    // for all shipped sprites but one degenerate).
    using Px = DecodedV2::Px;
    std::vector<std::vector<Px>> rows;
    std::vector<Px>              cur;
    size_t                       pos  = sizeof(SpriteHdrV2);
    auto                         need = [&](size_t bytes) {
        if (pos + bytes > sprite.size()) Fail(ctx, "V2 RLE stream overrun");
    };
    for (;;) {
        need(1);
        uint8_t b = sprite[pos];
        if (b == kEndSprite) {
            pos++;
            break;
        }
        if (b == kEndRow) {
            pos++;
            rows.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        need(2);
        uint8_t skip = sprite[pos];
        uint8_t cmd  = sprite[pos + 1];
        pos += 2;
        for (int k = 0; k < skip; k++) cur.push_back({0, 0, static_cast<uint8_t>(PxKindV2::Transparent)});
        if (cmd < 0x80) {
            need(static_cast<size_t>(cmd) * 2);
            for (int k = 0; k < cmd; k++) {
                uint16_t c = static_cast<uint16_t>(sprite[pos] | (sprite[pos + 1] << 8));
                pos += 2;
                cur.push_back({c, 0, static_cast<uint8_t>(PxKindV2::Opaque)});
            }
        } else {
            const int count = cmd & 0x7F;
            need(static_cast<size_t>(count) * 3);
            for (int k = 0; k < count; k++) {
                // Triple layout measured on all shipped blend pixels: [RGB565 LE][param].
                uint16_t c     = static_cast<uint16_t>(sprite[pos] | (sprite[pos + 1] << 8));
                uint8_t  param = sprite[pos + 2];
                pos += 3;
                if (param > 31)
                    Fail(ctx, std::format("V2 blend param {} out of range 0-31", param));
                cur.push_back({c, param, static_cast<uint8_t>(PxKindV2::Blend)});
            }
        }
    }
    if (!cur.empty()) rows.push_back(std::move(cur)); // stream ended without a final 0xFE

    size_t maxExtent = 0;
    for (const auto &r : rows) maxExtent = std::max(maxExtent, r.size());
    d.width = std::max<int>(d.header.width, static_cast<int>(maxExtent));
    if (d.width == 0) d.width = 1;
    d.height = d.header.yStart + static_cast<int>(rows.size());
    if (d.height == 0) d.height = 1;
    if (d.width > 4096 || d.height > 4096)
        Fail(ctx, std::format("implausible V2 canvas {}x{}", d.width, d.height));

    d.pixels.assign(static_cast<size_t>(d.width) * d.height,
                    {0, 0, static_cast<uint8_t>(PxKindV2::Transparent)});
    for (size_t r = 0; r < rows.size(); r++)
        std::copy(rows[r].begin(), rows[r].end(),
                  d.pixels.begin() + (d.header.yStart + r) * static_cast<size_t>(d.width));
    return d;
}

// ---- V2 encode --------------------------------------------------------------

std::vector<uint8_t> EncodeV2(const DecodedV2 &d, const std::string &ctx) {
    const int W = d.width, H = d.height;
    if (static_cast<size_t>(W) * H != d.pixels.size())
        Fail(ctx, "pixel buffer size mismatch");
    auto at = [&](int r, int c) -> const DecodedV2::Px & {
        return d.pixels[static_cast<size_t>(r) * W + c];
    };
    auto transparent = [&](int r, int c) {
        return at(r, c).kind == static_cast<uint8_t>(PxKindV2::Transparent);
    };
    // Rows above yStart are implicit; painting there is unsupported (header is verbatim).
    for (int r = 0; r < std::min<int>(d.header.yStart, H); r++)
        for (int c = 0; c < W; c++)
            if (!transparent(r, c))
                Fail(ctx, std::format(
                              "pixel painted at row {} above the header yStart {} — V2 header geometry "
                              "is fixed; erase the pixel or edit the raw header",
                              r, d.header.yStart));

    std::vector<uint8_t> out(sizeof(SpriteHdrV2));
    std::memcpy(out.data(), &d.header, sizeof(SpriteHdrV2));

    for (int r = d.header.yStart; r < H; r++) {
        int lastContent = W - 1;
        while (lastContent >= 0 && transparent(r, lastContent)) lastContent--;
        int c = 0;
        while (c <= lastContent) {
            int skip = 0;
            while (transparent(r, c + skip)) skip++;
            c += skip;
            // Draw run of consecutive same-kind pixels.
            uint8_t kind = at(r, c).kind;
            int     n    = 0;
            while (c + n <= lastContent && at(r, c + n).kind == kind) n++;
            int emitted = 0;
            while (emitted < n) {
                int chunk = std::min(127, n - emitted);
                while (skip >= kEndRow) { // keep the skip byte clear of 0xFE/0xFF
                    out.push_back(0xFD);
                    out.push_back(0x00);
                    skip -= 0xFD;
                }
                out.push_back(static_cast<uint8_t>(skip));
                skip = 0;
                if (kind == static_cast<uint8_t>(PxKindV2::Opaque)) {
                    out.push_back(static_cast<uint8_t>(chunk));
                    for (int k = 0; k < chunk; k++) {
                        uint16_t v = at(r, c + emitted + k).rgb565;
                        out.push_back(static_cast<uint8_t>(v & 0xFF));
                        out.push_back(static_cast<uint8_t>(v >> 8));
                    }
                } else {
                    out.push_back(static_cast<uint8_t>(0x80 | chunk));
                    for (int k = 0; k < chunk; k++) {
                        const auto &p = at(r, c + emitted + k);
                        out.push_back(static_cast<uint8_t>(p.rgb565 & 0xFF));
                        out.push_back(static_cast<uint8_t>(p.rgb565 >> 8));
                        out.push_back(p.blend);
                    }
                }
                emitted += chunk;
            }
            c += n;
        }
        out.push_back(kEndRow);
    }
    out.push_back(kEndSprite);

    // Self-check.
    DecodedV2 back = DecodeV2(out, ctx + " (self-check)");
    bool      ok   = back.width == W && back.height == H &&
              std::memcmp(back.pixels.data(), d.pixels.data(),
                          d.pixels.size() * sizeof(DecodedV2::Px)) == 0;
    if (!ok) Fail(ctx, "V2 encoder self-check failed (Decode(Encode(x)) != x)");
    return out;
}

// ---- color ------------------------------------------------------------------

Rgb888 Rgb565To888(uint16_t c) {
    const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    return {static_cast<uint8_t>((r5 * 255 + 15) / 31),
            static_cast<uint8_t>((g6 * 255 + 31) / 63),
            static_cast<uint8_t>((b5 * 255 + 15) / 31)};
}

uint16_t Rgb888To565(Rgb888 c) {
    const int r5 = (c.r * 31 + 127) / 255, g6 = (c.g * 63 + 127) / 255, b5 = (c.b * 31 + 127) / 255;
    return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

} // namespace mh::bnk
