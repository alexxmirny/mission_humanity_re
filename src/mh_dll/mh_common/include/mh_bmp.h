// Minimal 32-bit BGRA BMP writer/reader for the BNK sprite tooling.
// Writes BITMAPV4HEADER + BI_BITFIELDS with standard BGRA masks (explicit alpha, so
// editors keep it); reads 40/52/56/108/124-byte info headers, BI_RGB or BI_BITFIELDS
// with the standard masks, bottom-up or top-down. API buffers are top-down BGRA.
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace mh::bmp {

struct BmpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Bgra32 {
    int                  width = 0, height = 0;
    std::vector<uint8_t> data; // width*height*4, top-down, B G R A per pixel
};

void WriteBgra32(const std::filesystem::path &path, const Bgra32 &img);
// allowRgb24: also accept 24-bit BMPs, expanded to BGRA with A=255 — for mask files,
// which carry no alpha and are often re-saved as 24-bit by editors. Keep it false for
// images where alpha is meaningful (a 24-bit save would have destroyed it silently).
Bgra32 ReadBgra32(const std::filesystem::path &path, bool allowRgb24 = false);

} // namespace mh::bmp
