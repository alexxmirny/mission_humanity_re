// mh_tools — CLI for Mission Humanity / Exterminacja modding.
// BNK sprite banks <-> editable BMPs with a byte-identical no-edit round-trip.
// Format spec: /BNK_FORMAT.md (+ implementation errata) and /docs/gfx-sprites.md.

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <process.h>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <CLI11.hpp>

#include "mh_bmp.h"
#include "mh_bnk.h"

namespace fs = std::filesystem;
using namespace mh;

namespace {

// ---------- file helpers ----------

std::vector<uint8_t> ReadFileBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

void WriteFileBytes(const fs::path& p, std::span<const uint8_t> data) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!f) throw std::runtime_error("write failed: " + p.string());
}

// ---------- info.txt ----------

std::map<std::string, std::string> ReadInfo(const fs::path& p) {
    std::ifstream f(p);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

void WriteInfo(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& kv) {
    std::ofstream f(p, std::ios::binary);  // binary: stable \n line endings
    if (!f) throw std::runtime_error("cannot write " + p.string());
    for (const auto& [k, v] : kv) f << k << "=" << v << "\n";
}

std::string InfoGet(const std::map<std::string, std::string>& kv, const std::string& key,
                    const fs::path& src) {
    auto it = kv.find(key);
    if (it == kv.end()) throw std::runtime_error(src.string() + ": missing key '" + key + "'");
    return it->second;
}

// ---------- effect masks (extracted-layout v2) ----------
// V1 background-dependent effect pixels are stored as one mask BMP per RLE effect
// type instead of being blended into sprite.bmp. R channel 0 = no effect at that
// pixel (safe: every shipped payload byte0 is >= 1). Levels are stored x8 for
// editor visibility (exact: levels are 0..31); palette indices are stored raw in R.
struct FxMaskSpec { uint8_t type; const char* file; };
constexpr FxMaskSpec kFxMasks[] = {
    {0x40, "fx_blend.bmp"},   // BLEND_PAL:     R = palette index (1..255), G = level*8
    {0x60, "fx_blend2.bmp"},  // BLEND_COMPLEX: R = payload byte0 (1..255), G = level*8
    {0x80, "fx_tint.bmp"},    // TINT:          R=G=B = level*8 (level 1..31)
    {0xA0, "fx_darken.bmp"},  // DARKEN:        white = shadow pixel (no payload)
};

// Normalized per-pixel effect state: memcmp-comparable for change detection.
struct FxPx { uint8_t present, type, d0, d1; };

std::vector<FxPx> NormalizeEffects(const bnk::DecodedV1& d) {
    std::vector<FxPx> fx(d.pixels.size(), FxPx{0, 0, 0, 0});
    for (const auto& e : d.effects)
        fx[static_cast<size_t>(e.row) * d.width + e.col] = {1, e.type, e.data[0], e.data[1]};
    return fx;
}

std::optional<bmp::Bgra32> MakeFxMask(const bnk::DecodedV1& d, uint8_t type) {
    bmp::Bgra32 img;
    img.width = d.width;
    img.height = d.height;
    img.data.assign(static_cast<size_t>(d.width) * d.height * 4, 0);
    for (size_t i = 3; i < img.data.size(); i += 4) img.data[i] = 255;  // opaque black
    bool any = false;
    for (const auto& e : d.effects) {
        if (e.type != type) continue;
        uint8_t* px = img.data.data() + (static_cast<size_t>(e.row) * d.width + e.col) * 4;
        switch (type) {
            case 0xA0: px[0] = px[1] = px[2] = 255; break;
            case 0x80: px[0] = px[1] = px[2] = static_cast<uint8_t>(e.data[0] * 8); break;
            default:   // 0x40 / 0x60
                px[0] = 0;
                px[1] = static_cast<uint8_t>(std::min(255, e.data[1] * 8));
                px[2] = e.data[0];
        }
        any = true;
    }
    if (!any) return std::nullopt;
    return img;
}

// Reads one mask file (if present) into the normalized per-pixel state.
void ApplyMaskFile(const fs::path& path, uint8_t type, int w, int h,
                   std::vector<FxPx>& fx, const std::string& ctx) {
    if (!fs::exists(path)) return;
    auto img = bmp::ReadBgra32(path, /*allowRgb24=*/true);
    if (img.width != w || img.height != h)
        throw std::runtime_error(std::format(
            "{}: {} is {}x{} but the canvas is {}x{}", ctx,
            path.filename().string(), img.width, img.height, w, h));
    for (size_t p = 0; p < fx.size(); p++) {
        const uint8_t r = img.data[p * 4 + 2], g = img.data[p * 4 + 1];
        if (r == 0) continue;
        if (fx[p].present)
            throw std::runtime_error(std::format(
                "{}: pixel {} is marked in two effect masks (0x{:02X} and 0x{:02X}) — "
                "each pixel may carry at most one effect", ctx, p, fx[p].type, type));
        switch (type) {
            case 0xA0: fx[p] = {1, type, 0, 0}; break;
            case 0x80: fx[p] = {1, type, static_cast<uint8_t>(std::clamp(r / 8, 1, 31)), 0}; break;
            default:   fx[p] = {1, type, r, static_cast<uint8_t>(std::min(g / 8, 31))};
        }
    }
}

// ---------- BGRA generation ----------

std::vector<bnk::Rgb888> Palette888(std::span<const uint8_t> slice565) {
    std::vector<bnk::Rgb888> pal(slice565.size() / 2);
    for (size_t k = 0; k < pal.size(); k++) {
        uint16_t c = static_cast<uint16_t>(slice565[k * 2] | (slice565[k * 2 + 1] << 8));
        pal[k] = bnk::Rgb565To888(c);
    }
    return pal;
}

bmp::Bgra32 MakeV1Bgra(const bnk::DecodedV1& d, const std::vector<bnk::Rgb888>& pal,
                       const std::string& ctx) {
    bmp::Bgra32 img;
    img.width = d.width;
    img.height = d.height;
    img.data.assign(static_cast<size_t>(d.width) * d.height * 4, 0);
    for (size_t i = 0; i < d.pixels.size(); i++) {
        uint8_t* px = img.data.data() + i * 4;
        switch (static_cast<bnk::PxKindV1>(d.pixels[i].kind)) {
            case bnk::PxKindV1::Transparent:
                break;  // 0,0,0,0
            case bnk::PxKindV1::Opaque: {
                uint8_t idx = d.pixels[i].idx;
                if (idx >= pal.size())
                    throw std::runtime_error(std::format(
                        "{}: pixel {} references palette index {} but the palette has {} colors",
                        ctx, i, idx, pal.size()));
                px[0] = pal[idx].b; px[1] = pal[idx].g; px[2] = pal[idx].r; px[3] = 255;
                break;
            }
            case bnk::PxKindV1::Effect:
                break;  // background-dependent: lives in the fx_*.bmp masks, not here
            default:
                throw std::runtime_error(ctx + ": corrupt pixels.bin kind byte");
        }
    }
    return img;
}

bmp::Bgra32 MakePaletteBmp(const std::vector<bnk::Rgb888>& pal) {
    bmp::Bgra32 img;
    img.width = 16;
    img.height = 16;
    img.data.assign(16 * 16 * 4, 0);
    for (size_t k = 0; k < 256; k++) {
        uint8_t* px = img.data.data() + k * 4;
        if (k < pal.size()) { px[0] = pal[k].b; px[1] = pal[k].g; px[2] = pal[k].r; }
        px[3] = 255;
    }
    return img;
}

bmp::Bgra32 MakeV2Bgra(const bnk::DecodedV2& d) {
    bmp::Bgra32 img;
    img.width = d.width;
    img.height = d.height;
    img.data.assign(static_cast<size_t>(d.width) * d.height * 4, 0);
    for (size_t i = 0; i < d.pixels.size(); i++) {
        uint8_t* px = img.data.data() + i * 4;
        const auto& p = d.pixels[i];
        switch (static_cast<bnk::PxKindV2>(p.kind)) {
            case bnk::PxKindV2::Transparent:
                break;
            case bnk::PxKindV2::Opaque: {
                auto c = bnk::Rgb565To888(p.rgb565);
                px[0] = c.b; px[1] = c.g; px[2] = c.r; px[3] = 255;
                break;
            }
            case bnk::PxKindV2::Blend: {
                auto c = bnk::Rgb565To888(p.rgb565);
                // A = 8p+4: bijective (p = A/8), never 0 or 255, so the three pixel
                // classes stay unambiguous in the BMP.
                px[0] = c.b; px[1] = c.g; px[2] = c.r;
                px[3] = static_cast<uint8_t>(8 * p.blend + 4);
                break;
            }
            default:
                throw std::runtime_error("corrupt V2 pixels.bin kind byte");
        }
    }
    return img;
}

std::string SpriteDirName(size_t i) { return std::format("{:04}", i); }

// ---------- unpack ----------

struct UnpackOpts {
    fs::path dat, bnkDir, outDir;
    std::vector<int> banks;
    bool force = false;
    bool quiet = false;
};

int CmdUnpack(const UnpackOpts& o) {
    auto entries = bnk::LoadBanki(o.dat);
    const fs::path bnkDir = o.bnkDir.empty() ? o.dat.parent_path() : o.bnkDir;
    const fs::path root = o.outDir / o.dat.stem();
    if (fs::exists(root)) {
        if (!o.force)
            throw std::runtime_error(root.string() + " already exists (use --force to overwrite)");
        fs::remove_all(root);
    }
    fs::create_directories(root);
    WriteInfo(root / "info.txt", {
        {"datFilename", o.dat.filename().string()},
        {"numBanks", std::to_string(entries.size())},
        {"format", "bmp"},
        {"layout", "2"},  // v2: effect masks (fx_*.bmp), no pixels.bin/effects.bin
    });

    std::set<int> filter(o.banks.begin(), o.banks.end());
    size_t banksDone = 0, spritesDone = 0, absentDone = 0, warns = 0;
    for (size_t slot = 0; slot < entries.size(); slot++) {
        const auto& e = entries[slot];
        if (bnk::IsEmpty(e)) continue;
        if (!filter.empty() && !filter.count(static_cast<int>(slot))) continue;
        const std::string bnkName = bnk::BankFilename(slot);
        const fs::path bnkPath = bnkDir / bnkName;
        if (!fs::exists(bnkPath))
            throw std::runtime_error(std::format(
                "{}: BANKI slot {} is non-empty but the file is missing", bnkPath.string(), slot));
        auto fileBytes = ReadFileBytes(bnkPath);
        auto s = bnk::SplitBnk(fileBytes, e, bnkName);

        bnk::BnkFileHeader regen = bnk::MakeHeader(s);
        if (std::memcmp(&regen, s.storedHeader.data(), bnk::kFileHeaderSize) != 0) {
            std::cerr << "WARNING: " << bnkName
                      << ": stored 0x14 header differs from the section-size formula\n";
            warns++;
        }

        const fs::path bankDir = root / fs::path(bnkName).stem();
        fs::create_directories(bankDir);
        WriteFileBytes(bankDir / "header.bin", s.storedHeader);
        WriteFileBytes(bankDir / "indices1.bin",
                       {reinterpret_cast<const uint8_t*>(s.indices1.data()), s.indices1.size() * 4});
        if (!s.isV2)
            WriteFileBytes(bankDir / "indices2.bin",
                           {reinterpret_cast<const uint8_t*>(s.indices2.data()), s.indices2.size() * 4});
        const size_t n = bnk::NumSprites(e);
        WriteInfo(bankDir / "info.txt", {
            {"bankIndex", std::to_string(slot)},
            {"bnkFilename", bnkName},
            {"version", s.isV2 ? "2" : "1"},
            {"numSprites", std::to_string(n)},
            {"indicesSize", std::to_string(e.indicesSize)},
            {"spriteDataSize", std::to_string(e.spriteDataSize)},
            {"paletteDataSize", std::to_string(e.paletteDataSize)},
        });

        for (size_t i = 0; i < n; i++) {
            const std::string ctx = std::format("{} sprite {:04}", bnkName, i);
            const fs::path dir = bankDir / SpriteDirName(i);
            fs::create_directories(dir);
            WriteFileBytes(dir / "meta.bin", bnk::MetaSlice(s, i));
            auto slice = bnk::SpriteSlice(s, i);
            if (s.isV2 && slice.empty()) { absentDone++; continue; }  // absent sprite
            WriteFileBytes(dir / "rle.bin", slice);
            if (!s.isV2) {
                auto palSlice = bnk::PaletteSlice(s, i);
                WriteFileBytes(dir / "palette.bin", palSlice);
                auto d = bnk::DecodeV1(slice, ctx);
                auto pal = Palette888(palSlice);
                bmp::WriteBgra32(dir / "sprite.bmp", MakeV1Bgra(d, pal, ctx));
                bmp::WriteBgra32(dir / "palette.bmp", MakePaletteBmp(pal));
                for (const auto& spec : kFxMasks)
                    if (auto m = MakeFxMask(d, spec.type))
                        bmp::WriteBgra32(dir / spec.file, *m);
            } else {
                auto d = bnk::DecodeV2(slice, ctx);
                bmp::WriteBgra32(dir / "sprite.bmp", MakeV2Bgra(d));
            }
            spritesDone++;
        }
        banksDone++;
        if (!o.quiet)
            std::cout << std::format("{}: {} sprites (v{})\n", bnkName, n, s.isV2 ? 2 : 1);
    }
    std::cout << std::format("unpacked {} banks, {} sprites ({} absent V2 slots) -> {}\n",
                             banksDone, spritesDone, absentDone, root.string());
    if (warns) std::cout << warns << " header warnings\n";
    return 0;
}

// ---------- pack ----------

struct PackOpts {
    fs::path inRoot, outDir;
    std::vector<int> banks;
    bool reencodeAll = false;
    bool quiet = false;
};

struct PackStats {
    size_t verbatim = 0, reencoded = 0, byteIdentical = 0;
};

bnk::BnkSections PackV1Bank(const fs::path& bankDir, size_t n, bool reencodeAll, PackStats& st) {
    const std::string bankName = bankDir.filename().string();
    auto i2raw = ReadFileBytes(bankDir / "indices2.bin");
    if (i2raw.size() != n * 4)
        throw std::runtime_error(bankDir.string() + ": indices2.bin size mismatch");
    std::vector<uint32_t> origI2(n);
    std::memcpy(origI2.data(), i2raw.data(), i2raw.size());

    struct SpriteOut {
        std::vector<uint8_t> rle;
        std::vector<uint8_t> meta;
        std::vector<uint8_t> origPalBytes;
        std::optional<std::vector<uint8_t>> newPalBytes;  // set when the palette was edited
    };
    std::vector<SpriteOut> sprites(n);

    for (size_t i = 0; i < n; i++) {
        const std::string ctx = std::format("{} sprite {:04}", bankName, i);
        const fs::path dir = bankDir / SpriteDirName(i);
        auto& out = sprites[i];
        out.meta = ReadFileBytes(dir / "meta.bin");
        if (out.meta.size() != bnk::kMetaBytesPerSprite)
            throw std::runtime_error(ctx + ": meta.bin must be 24 bytes");
        auto rleOrig = ReadFileBytes(dir / "rle.bin");
        if (rleOrig.size() < sizeof(bnk::SpriteHdrV1))
            throw std::runtime_error(ctx + ": rle.bin too small for a V1 sprite header");
        out.origPalBytes = ReadFileBytes(dir / "palette.bin");
        const size_t ncolors = out.origPalBytes.size() / 2;
        auto origPal888 = Palette888(out.origPalBytes);

        // The verbatim original stream is the change-detection reference: decoding it
        // reproduces exactly what unpack wrote.
        bnk::DecodedV1 orig = bnk::DecodeV1(rleOrig, ctx + " (rle.bin)");
        int w = orig.width, h = orig.height;
        auto expectedSprite = MakeV1Bgra(orig, origPal888, ctx);
        auto expectedFx = NormalizeEffects(orig);
        auto expectedPal = MakePaletteBmp(origPal888);

        auto spriteBmp = bmp::ReadBgra32(dir / "sprite.bmp");
        auto palBmp = bmp::ReadBgra32(dir / "palette.bmp");
        if (palBmp.width != 16 || palBmp.height != 16)
            throw std::runtime_error(ctx + ": palette.bmp must be 16x16");
        // Resizing: the edited sprite.bmp defines the new canvas. EncodeV1
        // recomputes the sprite header geometry from d.width/d.height below, so
        // we just adopt the new dims (and skip the old-grid optimizations).
        const bool resized = (spriteBmp.width != w || spriteBmp.height != h);
        if (resized) { w = spriteBmp.width; h = spriteBmp.height; }
        std::vector<FxPx> fileFx(static_cast<size_t>(w) * h, FxPx{0, 0, 0, 0});
        for (const auto& spec : kFxMasks)
            ApplyMaskFile(dir / spec.file, spec.type, w, h, fileFx, ctx);

        const bool spriteChanged = resized || spriteBmp.data != expectedSprite.data;
        const bool fxChanged = !resized &&
            std::memcmp(fileFx.data(), expectedFx.data(), fileFx.size() * sizeof(FxPx)) != 0;
        const bool palChanged = palBmp.data != expectedPal.data;

        if (!reencodeAll && !spriteChanged && !fxChanged && !palChanged) {
            out.rle = std::move(rleOrig);
            st.verbatim++;
            continue;
        }

        // Rebuild the palette: per color, reuse the original RGB565 when the swatch
        // still shows its RGB888 conversion (avoids 888->565 round-trip loss).
        std::vector<uint8_t> newPalBytes(out.origPalBytes.size());
        std::vector<bnk::Rgb888> newPal888(ncolors);
        for (size_t k = 0; k < ncolors; k++) {
            const uint8_t* cell = palBmp.data.data() + k * 4;
            bnk::Rgb888 c{cell[2], cell[1], cell[0]};
            uint16_t v565;
            if (c.r == origPal888[k].r && c.g == origPal888[k].g && c.b == origPal888[k].b) {
                v565 = static_cast<uint16_t>(out.origPalBytes[k * 2] |
                                             (out.origPalBytes[k * 2 + 1] << 8));
            } else {
                v565 = bnk::Rgb888To565(c);
            }
            newPalBytes[k * 2] = static_cast<uint8_t>(v565 & 0xFF);
            newPalBytes[k * 2 + 1] = static_cast<uint8_t>(v565 >> 8);
            newPal888[k] = c;
        }
        if (palChanged) out.newPalBytes = newPalBytes;

        // Rebuild the pixel grid from sprite.bmp + the effect masks. Effect commands
        // are synthesized from the masks (row-major), so shadows are freely paintable.
        bnk::DecodedV1 d;
        d.header = orig.header;  // EncodeV1 keeps bytes 0-3, recomputes geometry
        d.width = w;
        d.height = h;
        d.pixels.assign(static_cast<size_t>(w) * h, {0, 0});
        for (size_t p = 0; p < d.pixels.size(); p++) {
            const uint8_t* bgra = spriteBmp.data.data() + p * 4;
            const uint8_t a = bgra[3];
            if (fileFx[p].present) {
                if (a != 0)
                    throw std::runtime_error(std::format(
                        "{}: pixel {} is opaque in sprite.bmp AND marked in an effect mask — "
                        "erase it in one of the two", ctx, p));
                const FxPx& f = fileFx[p];
                bnk::EffectEntry e{};
                e.row = static_cast<uint16_t>(p / w);
                e.col = static_cast<uint16_t>(p % w);
                e.type = f.type;
                e.dataSize = (f.type == 0xA0) ? 0 : (f.type == 0x80) ? 1 : 2;
                e.data[0] = f.d0;
                e.data[1] = f.d1;
                d.effects.push_back(e);
                d.pixels[p] = {static_cast<uint8_t>(bnk::PxKindV1::Effect), 0};
                continue;
            }
            if (a == 0) continue;  // transparent
            if (a != 255)
                throw std::runtime_error(std::format(
                    "{}: pixel {} has alpha {} — V1 sprite.bmp supports only 0 (transparent) "
                    "and 255 (opaque); effects go in the fx_*.bmp masks", ctx, p, a));
            bnk::Rgb888 c{bgra[2], bgra[1], bgra[0]};
            // Prefer the palette index this pixel had originally (duplicate colors are
            // common); otherwise take the first exact match in the edited palette.
            int idx = -1;
            if (!resized && orig.pixels[p].kind == static_cast<uint8_t>(bnk::PxKindV1::Opaque)) {
                uint8_t oi = orig.pixels[p].idx;
                if (oi < ncolors && newPal888[oi].r == c.r && newPal888[oi].g == c.g &&
                    newPal888[oi].b == c.b)
                    idx = oi;
            }
            if (idx < 0)
                for (size_t k = 0; k < ncolors; k++)
                    if (newPal888[k].r == c.r && newPal888[k].g == c.g && newPal888[k].b == c.b) {
                        idx = static_cast<int>(k);
                        break;
                    }
            if (idx < 0)
                throw std::runtime_error(std::format(
                    "{}: pixel {} color #{:02X}{:02X}{:02X} is not in the sprite palette — "
                    "add it to palette.bmp first", ctx, p, c.r, c.g, c.b));
            d.pixels[p] = {static_cast<uint8_t>(bnk::PxKindV1::Opaque), static_cast<uint8_t>(idx)};
        }

        out.rle = bnk::EncodeV1(d, ctx);
        st.reencoded++;
        if (out.rle == rleOrig) st.byteIdentical++;
    }

    // Palette section: groups = sprites sharing an original indices2 offset, laid out
    // in original offset order. An edited palette applies to the whole group.
    std::map<uint32_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < n; i++) groups[origI2[i]].push_back(i);
    bnk::BnkSections s;
    std::map<uint32_t, uint32_t> newOffset;
    for (auto& [origOff, members] : groups) {
        const std::vector<uint8_t>* slice = &sprites[members[0]].origPalBytes;
        const std::vector<uint8_t>* edited = nullptr;
        for (size_t m : members) {
            if (sprites[m].newPalBytes) {
                if (edited && *edited != *sprites[m].newPalBytes)
                    throw std::runtime_error(std::format(
                        "{}: sprites {:04} and {:04} share a palette but were edited to "
                        "different palettes — make their palette.bmp files identical",
                        bankName, members[0], m));
                edited = &*sprites[m].newPalBytes;
            }
        }
        if (edited) slice = edited;
        newOffset[origOff] = static_cast<uint32_t>(s.paletteData.size());
        s.paletteData.insert(s.paletteData.end(), slice->begin(), slice->end());
    }

    for (size_t i = 0; i < n; i++) {
        s.indices1.push_back(static_cast<uint32_t>(s.spriteData.size()));
        s.spriteData.insert(s.spriteData.end(), sprites[i].rle.begin(), sprites[i].rle.end());
        s.metadata.insert(s.metadata.end(), sprites[i].meta.begin(), sprites[i].meta.end());
        s.indices2.push_back(newOffset.at(origI2[i]));
    }
    return s;
}

bnk::BnkSections PackV2Bank(const fs::path& bankDir, size_t n, bool reencodeAll, PackStats& st) {
    const std::string bankName = bankDir.filename().string();
    bnk::BnkSections s;
    s.isV2 = true;
    for (size_t i = 0; i < n; i++) {
        const std::string ctx = std::format("{} sprite {:04}", bankName, i);
        const fs::path dir = bankDir / SpriteDirName(i);
        auto meta = ReadFileBytes(dir / "meta.bin");
        if (meta.size() != bnk::kMetaBytesPerSprite)
            throw std::runtime_error(ctx + ": meta.bin must be 24 bytes");
        s.metadata.insert(s.metadata.end(), meta.begin(), meta.end());
        if (!fs::exists(dir / "rle.bin")) {  // absent sprite
            s.indices1.push_back(bnk::kAbsentSprite);
            continue;
        }
        auto rleOrig = ReadFileBytes(dir / "rle.bin");
        if (rleOrig.size() < sizeof(bnk::SpriteHdrV2))
            throw std::runtime_error(ctx + ": rle.bin too small for a V2 sprite header");
        bnk::DecodedV2 orig = bnk::DecodeV2(rleOrig, ctx + " (rle.bin)");
        int w = orig.width, h = orig.height;
        auto expected = MakeV2Bgra(orig);
        auto spriteBmp = bmp::ReadBgra32(dir / "sprite.bmp");
        // Resizing: adopt the edited sprite.bmp dims (EncodeV2 recomputes the
        // header geometry); the unchanged-pixel shortcut below only applies when
        // the grid still lines up with the original.
        const bool resized = (spriteBmp.width != w || spriteBmp.height != h);
        if (resized) { w = spriteBmp.width; h = spriteBmp.height; }

        std::vector<uint8_t> rle;
        if (!reencodeAll && !resized && spriteBmp.data == expected.data) {
            rle = std::move(rleOrig);
            st.verbatim++;
        } else {
            bnk::DecodedV2 d;
            d.header = orig.header;
            d.width = w;
            d.height = h;
            d.pixels.assign(static_cast<size_t>(w) * h,
                            {0, 0, static_cast<uint8_t>(bnk::PxKindV2::Transparent)});
            for (size_t p = 0; p < d.pixels.size(); p++) {
                const uint8_t* bgra = spriteBmp.data.data() + p * 4;
                const uint8_t* exp = expected.data.data() + p * 4;
                if (!resized && std::memcmp(bgra, exp, 4) == 0) {
                    d.pixels[p] = orig.pixels[p];  // unchanged: keep the original RGB565/param
                    continue;
                }
                const uint8_t a = bgra[3];
                bnk::Rgb888 c{bgra[2], bgra[1], bgra[0]};
                if (a == 0) continue;
                if (a == 255)
                    d.pixels[p] = {bnk::Rgb888To565(c), 0,
                                   static_cast<uint8_t>(bnk::PxKindV2::Opaque)};
                else
                    d.pixels[p] = {bnk::Rgb888To565(c), static_cast<uint8_t>(std::min(a / 8, 31)),
                                   static_cast<uint8_t>(bnk::PxKindV2::Blend)};
            }
            rle = bnk::EncodeV2(d, ctx);
            st.reencoded++;
            if (rle == rleOrig) st.byteIdentical++;
        }
        s.indices1.push_back(static_cast<uint32_t>(s.spriteData.size()));
        s.spriteData.insert(s.spriteData.end(), rle.begin(), rle.end());
    }
    return s;
}

int CmdPack(const PackOpts& o) {
    auto global = ReadInfo(o.inRoot / "info.txt");
    const size_t numBanks = std::stoul(InfoGet(global, "numBanks", o.inRoot / "info.txt"));
    const std::string datName = InfoGet(global, "datFilename", o.inRoot / "info.txt");
    auto layoutIt = global.find("layout");
    if (layoutIt == global.end() || layoutIt->second != "2")
        throw std::runtime_error(
            (o.inRoot / "info.txt").string() +
            ": tree uses an older extracted layout (magenta markers + effects.bin) — "
            "re-run 'mh_tools unpack' with this tool version");
    fs::create_directories(o.outDir);

    std::set<int> filter(o.banks.begin(), o.banks.end());
    std::vector<bnk::BankiEntry> entries(numBanks, bnk::BankiEntry{0, 0, 0});
    PackStats st;
    size_t banksDone = 0;
    for (size_t slot = 0; slot < numBanks; slot++) {
        const std::string bnkName = bnk::BankFilename(slot);
        const fs::path bankDir = o.inRoot / fs::path(bnkName).stem();
        if (!fs::exists(bankDir)) continue;
        auto info = ReadInfo(bankDir / "info.txt");
        const size_t n = std::stoul(InfoGet(info, "numSprites", bankDir / "info.txt"));
        const int version = std::stoi(InfoGet(info, "version", bankDir / "info.txt"));
        bnk::BnkSections s = (version == 2) ? PackV2Bank(bankDir, n, o.reencodeAll, st)
                                            : PackV1Bank(bankDir, n, o.reencodeAll, st);
        entries[slot] = {static_cast<uint32_t>(n * 4), static_cast<uint32_t>(s.spriteData.size()),
                         static_cast<uint32_t>(s.paletteData.size())};
        if (filter.empty() || filter.count(static_cast<int>(slot))) {
            auto bytes = bnk::AssembleBnk(s);
            WriteFileBytes(o.outDir / bnkName, bytes);
            banksDone++;
            if (!o.quiet) std::cout << std::format("{}: {} sprites written\n", bnkName, n);
        }
    }
    bnk::SaveBanki(o.outDir / datName, entries);
    std::cout << std::format(
        "packed {} banks -> {} ({} sprites verbatim, {} re-encoded, {} of those byte-identical)\n",
        banksDone, o.outDir.string(), st.verbatim, st.reencoded, st.byteIdentical);
    return 0;
}

// ---------- verify ----------

bool CompareFiles(const fs::path& a, const fs::path& b, std::vector<std::string>& report) {
    if (!fs::exists(b)) {
        report.push_back(std::format("MISSING    {}", b.string()));
        return false;
    }
    const bool same = ReadFileBytes(a) == ReadFileBytes(b);
    report.push_back(std::format("{}  {}", same ? "identical" : "DIFFERENT!", a.filename().string()));
    return same;
}

struct VerifyOpts {
    fs::path dat, bnkDir, work;
    bool keep = false;
    bool semantic = false;
};

int CmdVerify(const VerifyOpts& o) {
    const fs::path bnkDir = o.bnkDir.empty() ? o.dat.parent_path() : o.bnkDir;
    fs::path work = o.work;
    if (work.empty())
        work = fs::temp_directory_path() / std::format("mh_tools_verify_{}", _getpid());
    fs::create_directories(work);

    auto entries = bnk::LoadBanki(o.dat);

    std::cout << "verify: unpacking...\n";
    CmdUnpack({o.dat, bnkDir, work / "unpacked", {}, /*force=*/true, /*quiet=*/true});
    const fs::path root = work / "unpacked" / o.dat.stem();
    std::cout << "verify: repacking...\n";
    CmdPack({root, work / "repacked", {}, /*reencodeAll=*/false, /*quiet=*/true});

    std::vector<std::string> report;
    bool allOk = CompareFiles(o.dat, work / "repacked" / o.dat.filename(), report);
    for (size_t slot = 0; slot < entries.size(); slot++) {
        if (bnk::IsEmpty(entries[slot])) continue;
        const std::string name = bnk::BankFilename(slot);
        allOk &= CompareFiles(bnkDir / name, work / "repacked" / name, report);
    }
    for (const auto& line : report) std::cout << "  " << line << "\n";
    std::cout << std::format("byte round-trip: {}\n", allOk ? "PASS" : "FAIL");

    bool semanticOk = true;
    if (o.semantic) {
        std::cout << "verify --semantic: repacking with --reencode-all...\n";
        CmdPack({root, work / "repacked2", {}, /*reencodeAll=*/true, /*quiet=*/true});
        std::cout << "verify --semantic: unpacking the re-encoded set...\n";
        CmdUnpack({work / "repacked2" / o.dat.filename(), work / "repacked2", work / "unpacked2",
                   {}, /*force=*/true, /*quiet=*/true});
        const fs::path root2 = work / "unpacked2" / o.dat.stem();
        size_t checked = 0, mismatches = 0;
        for (size_t slot = 0; slot < entries.size(); slot++) {
            if (bnk::IsEmpty(entries[slot])) continue;
            const fs::path a = root / fs::path(bnk::BankFilename(slot)).stem();
            const fs::path b = root2 / fs::path(bnk::BankFilename(slot)).stem();
            for (const auto& sd : fs::directory_iterator(a)) {
                if (!sd.is_directory()) continue;
                const fs::path bd = b / sd.path().filename();
                for (const char* f : {"sprite.bmp", "palette.bmp", "fx_blend.bmp",
                                      "fx_blend2.bmp", "fx_tint.bmp", "fx_darken.bmp"}) {
                    const bool inA = fs::exists(sd.path() / f), inB = fs::exists(bd / f);
                    if (!inA && !inB) continue;
                    checked++;
                    if (inA != inB || ReadFileBytes(sd.path() / f) != ReadFileBytes(bd / f)) {
                        mismatches++;
                        std::cout << std::format("  SEMANTIC MISMATCH: {}\\{}\n",
                                                 sd.path().string(), f);
                    }
                }
            }
        }
        semanticOk = mismatches == 0;
        std::cout << std::format("semantic round-trip: {} ({} sidecars compared, {} mismatches)\n",
                                 semanticOk ? "PASS" : "FAIL", checked, mismatches);
    }

    if (!o.keep) {
        std::error_code ec;
        fs::remove_all(work, ec);
    } else {
        std::cout << "work dir kept: " << work.string() << "\n";
    }
    return (allOk && semanticOk) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"mh_tools — Mission Humanity / Exterminacja modding tool"};
    argv = app.ensure_utf8(argv);
    app.require_subcommand(1);

    UnpackOpts uo;
    auto* unpack = app.add_subcommand("unpack", "Unpack BANKI.DAT + BANK_*.BNK to editable BMPs");
    unpack->add_option("-d,--dat", uo.dat, "Path to BANKI.DAT")->required()->check(CLI::ExistingFile);
    unpack->add_option("-b,--bnk-dir", uo.bnkDir, "Directory with BANK_*.BNK (default: the DAT's directory)");
    unpack->add_option("-o,--out", uo.outDir, "Output directory")->required();
    unpack->add_option("--bank", uo.banks, "Only these bank slots (repeatable)");
    unpack->add_flag("--force", uo.force, "Overwrite an existing output tree");

    PackOpts po;
    auto* pack = app.add_subcommand("pack", "Repack an unpacked tree into BANKI.DAT + BANK_*.BNK");
    pack->add_option("-i,--in", po.inRoot, "Unpacked root (the folder containing info.txt)")
        ->required()->check(CLI::ExistingDirectory);
    pack->add_option("-o,--out", po.outDir, "Output directory")->required();
    pack->add_option("--bank", po.banks, "Only write these bank slots (BANKI.DAT is always complete)");
    pack->add_flag("--reencode-all", po.reencodeAll, "Force the canonical encoder for every sprite");

    VerifyOpts vo;
    auto* verify = app.add_subcommand("verify", "Round-trip all banks and byte-compare the result");
    verify->add_option("-d,--dat", vo.dat, "Path to BANKI.DAT")->required()->check(CLI::ExistingFile);
    verify->add_option("-b,--bnk-dir", vo.bnkDir, "Directory with BANK_*.BNK (default: the DAT's directory)");
    verify->add_option("--work", vo.work, "Work directory (default: %TEMP%\\mh_tools_verify_<pid>)");
    verify->add_flag("--keep", vo.keep, "Keep the work directory");
    verify->add_flag("--semantic", vo.semantic, "Also validate the canonical encoders end-to-end");

    CLI11_PARSE(app, argc, argv);
    try {
        if (*unpack) return CmdUnpack(uo);
        if (*pack) return CmdPack(po);
        if (*verify) return CmdVerify(vo);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
