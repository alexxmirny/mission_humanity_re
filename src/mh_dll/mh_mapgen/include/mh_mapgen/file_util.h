// mh_mapgen -- whole-file helpers for the CLI and tests. The library proper never touches files.
#pragma once
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace mh_mapgen {

inline bool read_file(const std::string &path, std::vector<std::uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

inline bool write_file(const std::string &path, const void *data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char *>(data), std::streamsize(size));
    return bool(f);
}

} // namespace mh_mapgen
