#pragma once
#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
namespace mh::mods {
struct Snapshot;
struct TilesetDescriptor {
    std::string   name, label, trees, rules;
    std::wstring  wide_label;
    std::uint8_t  soldier_variant = 0;
    std::uint32_t tlo_payload = 0, rules_payload = 0;
};
// Native stock index 1..8, or zero for an unknown token; ASCII case-insensitive.
std::uint8_t stock_tileset_index(std::string_view token);
bool         tileset_token(std::string_view input, std::string &canonical, std::string &error);
bool         prepare_tilesets(Snapshot &snapshot, std::string &error);
} // namespace mh::mods
