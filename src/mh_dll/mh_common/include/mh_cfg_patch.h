#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mh::mods::cfg {
struct Input {
    std::string_view              source;
    std::string_view              path;
    std::span<const std::uint8_t> bytes;
    std::uint32_t                 mod_index;
};
struct Result {
    bool                      ok          = false;
    bool                      passthrough = false;
    std::string               error;
    std::vector<std::uint8_t> init;
    std::vector<std::uint8_t> lang;
    std::vector<std::string>  changed_planet_maps;
};
Result prepare(std::span<const std::uint8_t> base_init,
               std::span<const std::uint8_t> base_lang,
               std::span<const Input>        inputs,
               std::string_view              locale);
} // namespace mh::mods::cfg
