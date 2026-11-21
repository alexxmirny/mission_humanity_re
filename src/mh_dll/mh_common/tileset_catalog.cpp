#include "include/mh_tileset.h"
#include "include/mh_mod_catalog.h"
#include "mh_mapgen/rules.h"
#include "mh_mapgen/mpm.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>

namespace mh::mods {
std::uint8_t stock_tileset_index(std::string_view token) {
    constexpr std::string_view names[] = {"jungle.tlo", "kam256.tlo", "ksie256.tlo", "lod256.tlo",
                                          "metal256.tlo", "ruins256.tlo", "skaly256.tlo", "diuna256.tlo"};
    for (std::uint8_t i = 0; i < 8; ++i) {
        if (token.size() != names[i].size()) continue;
        bool equal = true;
        for (std::size_t j = 0; j < token.size(); ++j) {
            char c = token[j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != names[i][j]) {
                equal = false;
                break;
            }
        }
        if (equal) return static_cast<std::uint8_t>(i + 1);
    }
    return 0;
}

bool tileset_token(std::string_view input, std::string &canonical, std::string &error) {
    canonical.clear();
    error.clear();
    if (input.size() < 5 || input.size() > 12) {
        error = "tileset token length";
        return false;
    }
    const auto stem = input.size() - 4;
    if (stem < 1 || stem > 8) {
        error = "tileset stem length";
        return false;
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (i < stem && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            error = "invalid tileset stem";
            return false;
        }
        canonical += c;
    }
    if (canonical.substr(stem) != ".tlo") {
        canonical.clear();
        error = "expected .tlo";
        return false;
    }
    return true;
}

namespace {
bool descriptor(const Payload &p, TilesetDescriptor &d, std::string &err) {
    std::map<std::string, std::string, std::less<>> fields;
    if (!parse_ini_section(p.bytes, "tileset", fields, err)) return false;
    for (const auto &[key, value] : fields)
        if (key != "format" && key != "name" && key != "label" && key != "trees" && key != "soldiers" && key != "rules") {
            err = "unknown descriptor field: " + key;
            return false;
        }
    auto get = [&](const char *key) -> std::string { auto i = fields.find(key); return i == fields.end() ? std::string() : i->second; };
    if (get("format") != "1") {
        err = "descriptor format must be 1";
        return false;
    }
    if (!tileset_token(get("name"), d.name, err)) return false;
    const std::string expected = "tilesets\\" + d.name.substr(0, d.name.size() - 4) + ".ini";
    if (p.name != expected) {
        err = "descriptor stem does not match name";
        return false;
    }
    d.label = get("label");
    if (!valid_metadata_field(d.label, 64)) {
        err = "invalid descriptor label";
        return false;
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d.label.data(), static_cast<int>(d.label.size()), nullptr, 0);
    if (!n) {
        err = "cannot convert descriptor label";
        return false;
    }
    d.wide_label.resize(n);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d.label.data(), static_cast<int>(d.label.size()), d.wide_label.data(), n) != n) {
        err = "cannot convert descriptor label";
        return false;
    }
    d.trees = get("trees");
    for (char &c : d.trees)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (d.trees != "jungle" && d.trees != "ksie256" && d.trees != "lod256" && d.trees != "metal256" && d.trees != "ruins256" && d.trees != "skaly256" && d.trees != "diuna256" && d.trees != "none") {
        err = "unsupported tree profile";
        return false;
    }
    const auto soldiers = get("soldiers");
    if (soldiers != "0" && soldiers != "1" && soldiers != "2") {
        err = "soldiers must be 0, 1 or 2";
        return false;
    }
    d.soldier_variant = static_cast<std::uint8_t>(soldiers[0] - '0');
    const auto rules  = get("rules");
    if (!canonical_resource_name(rules.c_str(), d.rules, err)) return false;
    return true;
}
bool descriptor_path(std::string_view name) { return name.starts_with("tilesets\\") && name.ends_with(".ini"); }
} // namespace

static bool validate_map(const Snapshot &s, const std::uint8_t *bytes, std::size_t size,
                         bool campaign, const std::array<std::uint16_t, 8> &stock_tiles,
                         bool defer_stock, std::string &error);

bool prepare_tilesets(Snapshot &s, std::string &error) {
    std::vector<TilesetDescriptor> result;
    for (std::size_t i = 0; i < s.payloads.size(); ++i) {
        const Payload &p = s.payloads[i];
        if (!descriptor_path(p.name)) continue;
        TilesetDescriptor d;
        if (!descriptor(p, d, error)) {
            error = p.name + ": " + error;
            return false;
        }
        const auto winner = s.winners.find(p.name);
        if (winner == s.winners.end() || winner->second != i) continue;
        const auto t = s.winners.find(d.name), r = s.winners.find(d.rules);
        if (t == s.winners.end() || r == s.winners.end() || !s.payload(t->second) || !s.payload(r->second)) {
            error = p.name + ": missing TLO or rules";
            return false;
        }
        d.tlo_payload     = t->second;
        d.rules_payload   = r->second;
        const auto &bytes = s.payloads[d.tlo_payload].bytes;
        if (bytes.size() <= 512 || (bytes.size() - 512) % 1024 || (bytes.size() - 512) / 1024 > 8192) {
            error = p.name + ": invalid strategic TLO";
            return false;
        }
        const std::size_t count = (bytes.size() - 512) / 1024;
        const auto       &rb    = s.payloads[d.rules_payload].bytes;
        mh_mapgen::Rules  rules;
        if (!mh_mapgen::rules_from_text(std::string_view(reinterpret_cast<const char *>(rb.data()), rb.size()), rules, error) || !mh_mapgen::validate_rules(rules, error)) {
            error = p.name + ": " + error;
            return false;
        }
        std::string rule_name;
        if (!tileset_token(rules.tileset, rule_name, error) || rule_name != d.name || rules.tree_profile != d.trees) {
            error = p.name + ": rules token or tree profile mismatch";
            return false;
        }
        if (rules.tiles.size() > 1024) {
            error = p.name + ": too many active generator tiles";
            return false;
        }
        for (const auto &tile : rules.tiles)
            if (tile.id >= count || tile.id >= 4096) {
                error = p.name + ": rules tile outside safe TLO domain";
                return false;
            }
        result.push_back(std::move(d));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    s.tilesets = std::move(result);
    for (const auto &[name, index] : s.winners) {
        const Payload *source = s.payload(index);
        if (!source) {
            error = "map winner index out of range";
            s.tilesets.clear();
            return false;
        }
        const Payload &p        = *source;
        const bool     campaign = name.ends_with(".mp");
        if (!campaign && !p.name.ends_with(".mpm")) continue;
        if (!validate_map(s, p.bytes.data(), p.bytes.size(), campaign, {}, true, error)) {
            error = p.name + ": " + error;
            s.tilesets.clear();
            return false;
        }
    }
    return true;
}
const TilesetDescriptor *Snapshot::tileset(std::string_view name) const {
    auto compare = [](std::string_view canonical, std::string_view input) {
        const std::size_t n = std::min(canonical.size(), input.size());
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(input[i]);
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            const unsigned char a = static_cast<unsigned char>(canonical[i]);
            if (a != c) return a < c ? -1 : 1;
        }
        return canonical.size() < input.size() ? -1 : canonical.size() > input.size() ? 1
                                                                                      : 0;
    };
    const auto i = std::lower_bound(tilesets.begin(), tilesets.end(), name,
                                    [&](const auto &a, std::string_view b) { return compare(a.name, b) < 0; });
    return i != tilesets.end() && compare(i->name, name) == 0 ? &*i : nullptr;
}

// Validate a full map image against the immutable descriptor set. For a stock token whose
// TLO is not overridden, stock_tiles must be the actual count read by the pack adapter.
static bool validate_map(const Snapshot &s, const std::uint8_t *bytes, std::size_t size,
                         bool campaign, const std::array<std::uint16_t, 8> &stock_tiles,
                         bool defer_stock, std::string &error) {
    if (!bytes) {
        error = "null map bytes";
        return false;
    }
    if (campaign) {
        if (!validate_campaign_map(bytes, size, "map", error)) return false;
    } else {
        if (!mh_mapgen::validate_mpm(bytes, size, error)) return false;
    }
    constexpr std::size_t off  = mh_mapgen::NAME_OFF;
    const auto           *name = reinterpret_cast<const char *>(bytes + off);
    const void           *end  = std::memchr(name, 0, mh_mapgen::NAME_LEN);
    if (!end) {
        error = "map tileset name is not terminated";
        return false;
    }
    std::string token;
    if (!tileset_token(std::string_view(name, static_cast<const char *>(end) - name), token, error)) return false;
    const auto  stock = stock_tileset_index(token);
    const auto *d     = s.tileset(token);
    if (!stock && !d) {
        error = "unknown map tileset: " + token;
        return false;
    }
    const Payload *tlo   = d ? s.payload(d->tlo_payload) : s.winner(token);
    std::size_t    count = stock ? stock_tiles[stock - 1] : 0;
    if (tlo) {
        if (tlo->bytes.size() <= 512 || (tlo->bytes.size() - 512) % 1024 || (tlo->bytes.size() - 512) / 1024 > 8192) {
            error = "invalid strategic TLO";
            return false;
        }
        count = (tlo->bytes.size() - 512) / 1024;
    }
    if ((!count && !defer_stock) || count > 8192) {
        error = "actual stock TLO tile count required";
        return false;
    }
    auto                u32   = [&](std::size_t p) { return std::uint32_t(bytes[p]) | (std::uint32_t(bytes[p + 1]) << 8) | (std::uint32_t(bytes[p + 2]) << 16) | (std::uint32_t(bytes[p + 3]) << 24); };
    const std::uint64_t cells = std::uint64_t(u32(5)) * u32(9);
    if (cells > (size - mh_mapgen::MPM_HEADER_LEN) / 4) {
        error = "truncated map tile plane";
        return false;
    }
    for (std::size_t i = 0; i < cells; ++i) {
        const std::size_t at = mh_mapgen::MPM_HEADER_LEN + i * 4;
        const unsigned    id = (unsigned(bytes[at]) | (unsigned(bytes[at + 1]) << 8)) & mh_mapgen::TILE_ID_MASK;
        if ((count && id >= count) || id >= 4096) {
            error = "map tile outside safe TLO domain";
            return false;
        }
    }
    return true;
}
bool validate_tileset_map(const Snapshot &s, const std::uint8_t *bytes, std::size_t size,
                          bool campaign, const std::array<std::uint16_t, 8> &stock_tiles, std::string &error) {
    return validate_map(s, bytes, size, campaign, stock_tiles, false, error);
}
} // namespace mh::mods
