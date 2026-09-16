#include "include/mh_bmp.h"

#include <cstring>
#include <format>
#include <fstream>

namespace mh::bmp {

namespace {

#pragma pack(push, 1)
struct FileHeader {
    uint16_t type; // 'BM'
    uint32_t fileSize;
    uint16_t res1, res2;
    uint32_t offBits;
};
static_assert(sizeof(FileHeader) == 14);

struct V4Header {
    uint32_t size; // 108
    int32_t  width, height;
    uint16_t planes, bpp;
    uint32_t compression; // BI_BITFIELDS
    uint32_t sizeImage;
    int32_t  xppm, yppm;
    uint32_t clrUsed, clrImportant;
    uint32_t maskR, maskG, maskB, maskA;
    uint32_t csType; // LCS_sRGB
    int32_t  endpoints[9];
    uint32_t gammaR, gammaG, gammaB;
};
static_assert(sizeof(V4Header) == 108);
#pragma pack(pop)

constexpr uint32_t kBiRgb       = 0;
constexpr uint32_t kBiBitfields = 3;
constexpr uint32_t kMaskR = 0x00FF0000, kMaskG = 0x0000FF00, kMaskB = 0x000000FF, kMaskA = 0xFF000000;

[[noreturn]] void Fail(const std::filesystem::path &p, const std::string &msg) {
    throw BmpError(p.string() + ": " + msg);
}

} // namespace

void WriteBgra32(const std::filesystem::path &path, const Bgra32 &img) {
    if (img.width <= 0 || img.height <= 0 ||
        img.data.size() != static_cast<size_t>(img.width) * img.height * 4)
        Fail(path, "invalid image buffer");

    const uint32_t rowBytes = static_cast<uint32_t>(img.width) * 4; // 32bpp rows need no padding
    V4Header       ih{};
    ih.size        = sizeof(V4Header);
    ih.width       = img.width;
    ih.height      = img.height; // positive: bottom-up
    ih.planes      = 1;
    ih.bpp         = 32;
    ih.compression = kBiBitfields;
    ih.sizeImage   = rowBytes * img.height;
    ih.xppm = ih.yppm = 2835; // 72 DPI
    ih.maskR          = kMaskR;
    ih.maskG          = kMaskG;
    ih.maskB          = kMaskB;
    ih.maskA          = kMaskA;
    ih.csType         = 0x73524742; // 'sRGB'

    FileHeader fh{};
    fh.type     = 0x4D42; // 'BM'
    fh.offBits  = sizeof(FileHeader) + sizeof(V4Header);
    fh.fileSize = fh.offBits + ih.sizeImage;

    std::ofstream f(path, std::ios::binary);
    if (!f) Fail(path, "cannot open for writing");
    f.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
    for (int r = img.height - 1; r >= 0; r--) // bottom-up
        f.write(reinterpret_cast<const char *>(img.data.data() + static_cast<size_t>(r) * rowBytes),
                rowBytes);
    if (!f) Fail(path, "write failed");
}

Bgra32 ReadBgra32(const std::filesystem::path &path, bool allowRgb24) {
    std::ifstream f(path, std::ios::binary);
    if (!f) Fail(path, "cannot open");
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
    if (raw.size() < sizeof(FileHeader) + 40) Fail(path, "file too small for a BMP");

    FileHeader fh;
    std::memcpy(&fh, raw.data(), sizeof(fh));
    if (fh.type != 0x4D42) Fail(path, "not a BMP file");

    uint32_t ihSize;
    std::memcpy(&ihSize, raw.data() + sizeof(FileHeader), 4);
    if (ihSize != 40 && ihSize != 52 && ihSize != 56 && ihSize != 108 && ihSize != 124)
        Fail(path, std::format("unsupported BMP info-header size {}", ihSize));

    // The first 40 bytes of every supported header match BITMAPINFOHEADER.
    struct {
        int32_t  width, height;
        uint16_t planes, bpp;
        uint32_t compression;
    } core;
    std::memcpy(&core.width, raw.data() + sizeof(FileHeader) + 4, 8);
    std::memcpy(&core.planes, raw.data() + sizeof(FileHeader) + 12, 4);
    std::memcpy(&core.compression, raw.data() + sizeof(FileHeader) + 16, 4);

    if (core.bpp != 32 && !(core.bpp == 24 && allowRgb24))
        Fail(path, std::format(
                       "expected a 32-bit BMP, got {}-bit — re-save the image as 32-bit BGRA", core.bpp));
    if (core.compression != kBiRgb && core.compression != kBiBitfields)
        Fail(path, std::format("unsupported BMP compression {} (need uncompressed)", core.compression));
    if (core.compression == kBiBitfields) {
        // Masks follow the info header for size 40; are embedded from size 52 up.
        uint32_t masks[4]  = {0, 0, 0, 0};
        size_t   maskOff   = sizeof(FileHeader) + (ihSize == 40 ? 40 : 40);
        size_t   available = (ihSize == 40) ? 12 : (ihSize >= 56 ? 16 : 12);
        if (raw.size() < sizeof(FileHeader) + ihSize + (ihSize == 40 ? available : 0))
            Fail(path, "truncated BMP header");
        std::memcpy(masks, raw.data() + maskOff, available);
        if (masks[0] != kMaskR || masks[1] != kMaskG || masks[2] != kMaskB ||
            (available == 16 && masks[3] != 0 && masks[3] != kMaskA))
            Fail(path, "unsupported BMP channel masks (need standard BGRA)");
    }

    const bool topDown = core.height < 0;
    const int  w = core.width, h = topDown ? -core.height : core.height;
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) Fail(path, "implausible BMP dimensions");
    const size_t bytesPerPx = core.bpp / 8;
    const size_t srcRow     = (static_cast<size_t>(w) * bytesPerPx + 3) & ~size_t(3);
    const size_t dstRow     = static_cast<size_t>(w) * 4;
    if (raw.size() < fh.offBits + srcRow * h) Fail(path, "truncated BMP pixel data");

    Bgra32 img;
    img.width  = w;
    img.height = h;
    img.data.resize(dstRow * h);
    for (int r = 0; r < h; r++) {
        const uint8_t *src = raw.data() + fh.offBits + srcRow * (topDown ? r : h - 1 - r);
        uint8_t       *dst = img.data.data() + static_cast<size_t>(r) * dstRow;
        if (bytesPerPx == 4) {
            std::memcpy(dst, src, dstRow);
        } else {
            for (int x = 0; x < w; x++) { // BGR -> BGRA, A=255
                dst[x * 4]     = src[x * 3];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    return img;
}

} // namespace mh::bmp
