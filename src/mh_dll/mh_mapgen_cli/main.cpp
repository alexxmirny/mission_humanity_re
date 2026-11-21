// mapgen.exe -- procedural strategic-map tooling.
//   mapgen learn --in <dir>... --out <rulesdir> [--stock <list>] [--allow-nonstock]
//   mapgen check <map> --rules <rulesdir> [--wrap]
//   mapgen preview <map.mpm> <tileset.tlo> <out.png|out.bmp> [--size 128]   (the in-game map preview, for eye checks)
//   mapgen gen --rules <rulesdir> --tileset <name> --size 64|128|256 --players N --seed S --out <file.mpm>
//              [--no-wrap] [--ensure-paths | --no-paths] [--name <title>] [--desc <text>]
// Exit codes: 0 ok, 1 illegal pair(s) found, 2 usage/IO error, 3 generation failed (reason on stderr).
#include "CLI11.hpp"
#include "mh_mapgen/check.h"
#include "mh_mapgen/calibrate.h"
#include "mh_mapgen/file_util.h"
#include "mh_mapgen/gen.h"
#include "mh_mapgen/infer.h"
#include "mh_mapgen/mpm.h"
#include "mh_mapgen/preview.h"
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;
using namespace mh_mapgen;

namespace {

constexpr int EXIT_ILLEGAL = 1;
constexpr int EXIT_ERROR   = 2;
constexpr int EXIT_GENFAIL = 3;

// Peak working set in MB (0 where unknown): lets the sweep print the per-map memory next to the timing.
std::size_t peak_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return std::size_t(pmc.PeakWorkingSetSize >> 20);
#endif
    return 0;
}

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The stock list ships next to the library sources; find it from the exe location or the repo root.
std::string find_stock_list(const char *argv0) {
    std::vector<fs::path> cand;
    std::error_code       ec;
    fs::path              exe = fs::absolute(argv0, ec).parent_path();
    cand.push_back(exe / ".." / ".." / "mh_mapgen" / "data" / "stock_maps.sha256");
    cand.push_back(exe / ".." / ".." / ".." / "mh_mapgen" / "data" / "stock_maps.sha256");
    cand.push_back(fs::path("src") / "mh_dll" / "mh_mapgen" / "data" / "stock_maps.sha256");
    for (const fs::path &p : cand)
        if (fs::exists(p, ec)) return p.string();
    return {};
}

struct LoadedMap {
    std::string path, sha256;
    MpmModel    model;
};

// ---- learn -------------------------------------------------------------------------------------------

int run_learn(const char *argv0, const std::vector<std::string> &dirs, const std::string &outdir, std::string stock_path, bool allow_nonstock) {
    std::vector<fs::path> files;
    for (const std::string &d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) {
            std::fprintf(stderr, "learn: not a directory: %s\n", d.c_str());
            return EXIT_ERROR;
        }
        for (const auto &e : fs::directory_iterator(d)) {
            std::string ext = lower(e.path().extension().string());
            if (e.is_regular_file() && (ext == ".mp" || ext == ".mpm")) files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<LoadedMap> maps(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::vector<std::uint8_t> bytes;
        std::string               err;
        if (!read_file(files[i].string(), bytes) || !read_mpm(bytes.data(), bytes.size(), maps[i].model, err)) {
            std::fprintf(stderr, "learn: cannot load %s: %s\n", files[i].string().c_str(), err.empty() ? "read failed" : err.c_str());
            return EXIT_ERROR;
        }
        maps[i].path   = files[i].string();
        maps[i].sha256 = sha256_hex(bytes.data(), bytes.size());
    }

    // Provenance gate: the rules are only meaningful for the shipped maps they were designed against.
    if (stock_path.empty()) stock_path = find_stock_list(argv0);
    std::vector<StockEntry> stock;
    bool                    have_stock = false;
    if (!stock_path.empty()) {
        std::vector<std::uint8_t> bytes;
        std::string               err;
        if (!read_file(stock_path, bytes)) {
            std::fprintf(stderr, "learn: cannot read stock list %s\n", stock_path.c_str());
            return EXIT_ERROR;
        }
        if (!stock_list_parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), stock, err)) {
            std::fprintf(stderr, "learn: %s: %s\n", stock_path.c_str(), err.c_str());
            return EXIT_ERROR;
        }
        have_stock = true;
    }
    std::size_t nonstock = 0;
    if (have_stock) {
        for (const LoadedMap &m : maps)
            if (!stock_list_contains(stock, m.sha256)) {
                std::fprintf(stderr, "learn: NOT A STOCK MAP: %s (%s)\n", m.path.c_str(), m.sha256.c_str());
                ++nonstock;
            }
    } else std::fprintf(stderr, "learn: no stock list found (pass --stock <file>)\n");
    if ((nonstock || !have_stock) && !allow_nonstock) {
        std::fprintf(stderr, "learn: refusing to learn from inputs that do not match the stock list; --allow-nonstock overrides\n");
        return EXIT_ERROR;
    }
    if (nonstock || !have_stock) std::fprintf(stderr, "learn: WARNING: continuing with unverified inputs (--allow-nonstock); rules are NOT the reference rules\n");

    std::vector<LearnInput> in;
    for (const LoadedMap &m : maps) in.push_back({&m.model, m.sha256});
    std::vector<Rules> rules = learn_all(in);

    std::error_code ec;
    fs::create_directories(outdir, ec);
    std::uint32_t unique = 0;
    for (const Rules &r : rules) unique += r.maps;
    std::printf("learn: %zu files, %u unique maps, %zu tilesets\n", maps.size(), unique, rules.size());
    for (const Rules &r : rules) {
        std::string text = rules_to_text(r);
        fs::path    out  = fs::path(outdir) / rules_file_name(r.tileset);
        if (!write_file(out.string(), text.data(), text.size())) {
            std::fprintf(stderr, "learn: cannot write %s\n", out.string().c_str());
            return EXIT_ERROR;
        }
        std::printf("  %-14s maps %2u  tiles %4zu  h-pairs %6zu  v-pairs %6zu  kinds %2zu  -> %s (%zu bytes)\n", r.tileset.c_str(), r.maps, r.tiles.size(), r.h.size(), r.v.size(), r.kinds.size(), out.string().c_str(), text.size());
    }
    return 0;
}

// ---- check -------------------------------------------------------------------------------------------

int run_check(const std::string &map_path, const std::string &rules_dir, bool wrap) {
    std::vector<std::uint8_t> bytes;
    MpmModel                  m;
    std::string               err;
    if (!read_file(map_path, bytes) || !read_mpm(bytes.data(), bytes.size(), m, err)) {
        std::fprintf(stderr, "check: cannot load %s: %s\n", map_path.c_str(), err.empty() ? "read failed" : err.c_str());
        return EXIT_ERROR;
    }
    fs::path                  rpath = fs::path(rules_dir) / rules_file_name(m.tileset());
    std::vector<std::uint8_t> rb;
    Rules                     rules;
    if (!read_file(rpath.string(), rb)) {
        std::fprintf(stderr, "check: no rules for tileset %s (%s)\n", m.tileset().c_str(), rpath.string().c_str());
        return EXIT_ERROR;
    }
    if (!rules_from_text(std::string_view(reinterpret_cast<const char *>(rb.data()), rb.size()), rules, err)) {
        std::fprintf(stderr, "check: %s: %s\n", rpath.string().c_str(), err.c_str());
        return EXIT_ERROR;
    }
    Report rep = check_map(m, rules, wrap);
    for (const IllegalPair &p : rep.illegal_pairs) std::printf("ILLEGAL %s\n", format_illegal(p).c_str());
    std::printf("%s: %llu pairs checked, %zu illegal, %u unknown tile cells%s\n", map_path.c_str(), static_cast<unsigned long long>(rep.pairs_checked), rep.illegal_pairs.size(), rep.unknown_tiles, wrap ? " (wrap)" : "");
    return rep.ok() ? 0 : EXIT_ILLEGAL;
}

// A threshold is only an exploratory score cutoff, not a learned inference policy.
int run_calibrate(const std::string &tlo_path, const std::string &rules_path) {
    std::vector<std::uint8_t> art, rb;
    std::string               err;
    Rules                     rules;
    if (!read_file(tlo_path, art) || !read_file(rules_path, rb)) {
        std::fprintf(stderr, "calibrate: cannot read inputs\n");
        return EXIT_ERROR;
    }
    if (!rules_from_text(std::string_view(reinterpret_cast<const char *>(rb.data()), rb.size()), rules, err)) {
        std::fprintf(stderr, "calibrate: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    PixelCalibration report;
    if (!calibrate_pixels(art.data(), art.size(), rules, report, err)) {
        std::fprintf(stderr, "calibrate: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    std::printf("pixel-calibration 1\ntileset %s\nart_sha256 %s\nrules_serialized_sha256 %s\nactive %zu\n", rules.tileset.c_str(), report.art_sha256.c_str(), report.rules_sha256.c_str(), rules.tiles.size());
    const auto print_dir = [](const char *dir, const SeamCalibration &s) {
        std::vector<std::uint32_t> pos;
        for (const auto &p : s.pairs)
            if (p.weight) pos.push_back(p.score);
        std::printf("direction %s candidates %zu observed %zu observed_weight %llu\n", dir, s.pairs.size(), pos.size(), static_cast<unsigned long long>(s.positive_weight));
        if (pos.empty()) return;
        const int pct[] = {50, 75, 90, 95, 99, 100};
        for (int p : pct) {
            const std::size_t   idx       = (pos.size() * std::size_t(p) + 99) / 100 - 1;
            const std::uint32_t threshold = pos[idx];
            std::size_t         proposed = 0, hits = 0, tied = 0;
            std::uint64_t       weighted = 0;
            for (const auto &x : s.pairs) {
                if (x.score == threshold) ++tied;
                if (x.score <= threshold) {
                    ++proposed;
                    if (x.weight) {
                        ++hits;
                        weighted += x.weight;
                    }
                }
            }
            std::printf("cut percentile=%d score=%u proposed=%zu observed=%zu/%zu weighted=%llu/%llu unobserved=%zu tied=%zu\n", p, threshold, proposed, hits, pos.size(), static_cast<unsigned long long>(weighted), static_cast<unsigned long long>(s.positive_weight), proposed - hits, tied);
        }
        int shown = 0;
        for (auto it = s.pairs.rbegin(); it != s.pairs.rend() && shown < 10; ++it)
            if (it->weight) {
                std::printf("high_observed a=%u b=%u score=%u weight=%u\n", unsigned(it->a), unsigned(it->b), it->score, it->weight);
                ++shown;
            }
        std::printf("unobserved pairs are unlabeled, not negatives\n");
    };
    print_dir("horizontal", report.horizontal);
    print_dir("vertical", report.vertical);
    return 0;
}
// The author template supplies every nonvisual property, and contains no h/v pairs.
int run_infer(const std::string &tlo_path, const std::string &semantic_path, const std::string &out_path,
              std::uint32_t h_limit, std::uint32_t v_limit, const std::string &ref_path) {
    std::vector<std::uint8_t> art, bytes;
    if (!read_file(tlo_path, art) || !read_file(semantic_path, bytes)) {
        std::fprintf(stderr, "infer: cannot read input\n");
        return EXIT_ERROR;
    }
    Rules       semantic, result;
    std::string err;
    if (!rules_from_text(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), semantic, err)) {
        std::fprintf(stderr, "infer: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    InferenceOptions options{h_limit, v_limit, {}};
    if (!ref_path.empty()) {
        std::vector<std::uint8_t> ref;
        if (!read_file(ref_path, ref) || ref.size() > 16384) {
            std::fprintf(stderr, "infer: cannot read bounded refinements\n");
            return EXIT_ERROR;
        }
        std::istringstream lines(std::string(ref.begin(), ref.end()));
        std::string        line;
        unsigned           lineno = 0;
        while (std::getline(lines, line)) {
            ++lineno;
            std::istringstream fields(line);
            char               action = 0, dir = 0;
            unsigned           a = 0, b = 0;
            std::string        extra;
            if (!(fields >> action)) continue;
            if (action == '#') continue;
            if ((action != '+' && action != '-') || !(fields >> dir >> a >> b) || (dir != 'h' && dir != 'v') || a >= 4096 || b >= 4096 || (fields >> extra) || options.refinements.size() >= 256) {
                std::fprintf(stderr, "infer: invalid refinement at line %u\n", lineno);
                return EXIT_ERROR;
            }
            options.refinements.push_back({dir, std::uint16_t(a), std::uint16_t(b), action == '+'});
        }
    }
    InferenceReport report;
    if (!infer_rules(art.data(), art.size(), semantic, options, result, report, err)) {
        std::fprintf(stderr, "infer: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    const std::string text = rules_to_text(result);
    if (!write_file(out_path, text.data(), text.size())) {
        std::fprintf(stderr, "infer: cannot write output\n");
        return EXIT_ERROR;
    }
    std::printf("infer: tileset %s horizontal %zu vertical %zu refinements +%zu -%zu -> %s\n", result.tileset.c_str(), report.horizontal, report.vertical, report.added, report.removed, out_path.c_str());
    return 0;
}

// ---- gen ---------------------------------------------------------------------------------------------

// "Skaly256", "skaly256.tlo", "SKALY256.TLO" -> "skaly256.tlo"
std::string tileset_name(std::string s) {
    s = lower(s);
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".tlo") != 0) s += ".tlo";
    return s;
}

int run_gen(const std::string &rules_dir, const std::string &tileset_arg, std::uint32_t size, std::uint32_t players, std::uint64_t seed, const std::string &out_path, bool wrap, bool ensure_paths, const std::string &name, const std::string &desc, const std::string &tune, bool verbose) {
    const std::string         tileset = tileset_name(tileset_arg);
    const fs::path            rpath   = fs::path(rules_dir) / rules_file_name(tileset);
    std::vector<std::uint8_t> rb;
    if (!read_file(rpath.string(), rb)) {
        std::fprintf(stderr, "gen: no rules for tileset %s (%s); supply authored/inferred or learned rules\n", tileset.c_str(), rpath.string().c_str());
        return EXIT_ERROR;
    }
    Rules       rules;
    std::string err;
    if (!rules_from_text(std::string_view(reinterpret_cast<const char *>(rb.data()), rb.size()), rules, err)) {
        std::fprintf(stderr, "gen: %s: %s\n", rpath.string().c_str(), err.c_str());
        return EXIT_ERROR;
    }
    if (lower(rules.tileset) != tileset) {
        std::fprintf(stderr, "gen: requested tileset does not match rules\n");
        return EXIT_ERROR;
    }
    if (tileset == "kam256.tlo" && (rules.gameplay_profile.empty() || rules.tree_profile.empty() || !validate_rules(rules, err))) {
        std::fprintf(stderr, "gen: kam256 requires valid explicit-profile rules: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    GenParams gp;
    gp.tileset      = rules.tileset;
    gp.size         = size;
    gp.players      = players;
    gp.seed         = seed;
    gp.wrap         = wrap;
    gp.ensure_paths = ensure_paths;
    gp.name         = name;
    gp.desc         = desc;
    gp.tune         = tune;
    err             = gen_validate(rules, gp);
    if (!err.empty()) {
        std::fprintf(stderr, "gen: %s\n", err.c_str());
        return EXIT_ERROR;
    }
    const auto t0  = std::chrono::steady_clock::now();
    GenResult  res = generate(rules, gp);
    const long ms  = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    if (verbose)
        for (const std::string &l : res.log) std::fprintf(stderr, "gen: retry: %s\n", l.c_str());
    if (!res.ok) {
        std::fprintf(stderr, "gen: FAILED: %s\n", res.reason.c_str());
        return EXIT_GENFAIL;
    }
    const std::vector<std::uint8_t> bytes = write_mpm(res.map);
    if (bytes.empty() || !write_file(out_path, bytes.data(), bytes.size())) {
        std::fprintf(stderr, "gen: cannot write %s\n", out_path.c_str());
        return EXIT_ERROR;
    }
    std::printf("gen: %s %ux%u %u players seed %llu wrap %s paths %s -> %s (%zu bytes, sha256 %s)\n", rules.tileset.c_str(), size, size, players, static_cast<unsigned long long>(seed), wrap ? "on" : "off", ensure_paths ? "on" : "off", out_path.c_str(), bytes.size(), sha256_hex(bytes.data(), bytes.size()).c_str());
    std::printf("gen: attempts %u (plan failed %u, wfc failed %u, gates failed %u)  repairs %u  path edits %u  ramps %u  wfc resets %u  decals %u (dropped %u)  trees %u  ore blocks %u  %ld ms (plan %u, wfc %u, place %u)  peak %zu MB\n", res.stats.attempts, res.stats.plan_failures, res.stats.wfc_failures, res.stats.gate_failures, res.stats.repair_passes, res.stats.path_edits, res.stats.ramps, res.stats.resets, res.stats.decals, res.stats.decal_drops, res.stats.trees, res.stats.ore_blocks, ms, res.stats.ms_plan, res.stats.ms_wfc, res.stats.ms_place, peak_mb());
    return 0;
}


// ---- preview -----------------------------------------------------------------------------------------
// A PNG of uncompressed (stored) deflate blocks: no zlib needed, big but exact. Rows are RGB, 8 bits.
void put32be(std::vector<std::uint8_t> &v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(std::uint8_t(x >> s));
}

void png_chunk(std::vector<std::uint8_t> &png, const char *type, const std::vector<std::uint8_t> &data) {
    put32be(png, std::uint32_t(data.size()));
    std::vector<std::uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    put32be(png, crc32_ieee(td.data(), td.size()));
}

std::vector<std::uint8_t> make_png(const std::vector<std::uint8_t> &rgb, int w, int h) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgb.begin() + std::size_t(y) * w * 3, rgb.begin() + std::size_t(y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t             a = 1, b = 0;
    for (std::uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n    = std::min<std::size_t>(65535, raw.size() - off);
        const bool        last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(std::uint8_t(n & 255));
        z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n & 255));
        z.push_back(std::uint8_t((~n >> 8) & 255));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    put32be(z, (b << 16) | a);
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, std::uint32_t(w));
    put32be(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    return png;
}

std::vector<std::uint8_t> make_bmp(const std::vector<std::uint8_t> &rgb, int w, int h) {
    const std::size_t         stride = (std::size_t(w) * 3 + 3) & ~std::size_t(3);
    std::vector<std::uint8_t> bmp(54 + stride * h, 0);
    auto                      le32 = [&](std::size_t o, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bmp[o + i] = std::uint8_t(v >> (8 * i));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    le32(2, std::uint32_t(bmp.size()));
    le32(10, 54);
    le32(14, 40);
    le32(18, std::uint32_t(w));
    le32(22, std::uint32_t(h));
    bmp[26] = 1;
    bmp[28] = 24;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) bmp[54 + std::size_t(h - 1 - y) * stride + std::size_t(x) * 3 + (2 - c)] = rgb[(std::size_t(y) * w + x) * 3 + c];
    return bmp;
}

int run_preview(const std::string &map_path, const std::string &tlo_path, const std::string &out_path, int size) {
    std::vector<std::uint8_t> mb, tb;
    MpmModel                  m;
    std::string               err;
    if (!read_file(map_path, mb) || !read_mpm(mb.data(), mb.size(), m, err)) {
        std::fprintf(stderr, "preview: cannot load %s: %s\n", map_path.c_str(), err.empty() ? "read failed" : err.c_str());
        return EXIT_ERROR;
    }
    std::vector<std::uint16_t> tab;
    if (!read_file(tlo_path, tb) || !tlo_tile_means(tb.data(), tb.size(), tab)) {
        std::fprintf(stderr, "preview: cannot load a tileset from %s\n", tlo_path.c_str());
        return EXIT_ERROR;
    }
    std::vector<std::uint16_t> px(std::size_t(size) * size);
    PreviewStats               st;
    const auto                 t0 = std::chrono::steady_clock::now();
    if (!render_preview(m, tab.data(), tab.size(), size, px.data(), &st)) {
        std::fprintf(stderr, "preview: the map cannot be rendered at %d px\n", size);
        return EXIT_ERROR;
    }
    const long                us = long(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    std::vector<std::uint8_t> rgb;
    rgb.reserve(px.size() * 3);
    for (std::uint16_t v : px) { // expand the 565 channels the way a display would
        const unsigned r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
        rgb.push_back(std::uint8_t((r * 255 + 15) / 31));
        rgb.push_back(std::uint8_t((g * 255 + 31) / 63));
        rgb.push_back(std::uint8_t((b * 255 + 15) / 31));
    }
    const bool                      bmp = lower(fs::path(out_path).extension().string()) == ".bmp";
    const std::vector<std::uint8_t> img = bmp ? make_bmp(rgb, size, size) : make_png(rgb, size, size);
    if (!write_file(out_path, img.data(), img.size())) {
        std::fprintf(stderr, "preview: cannot write %s\n", out_path.c_str());
        return EXIT_ERROR;
    }
    std::printf("preview: %s %ux%u tileset %s (%zu tiles) spots %d ore blocks %d trees %d -> %s %dx%d (%ld us)\n", map_path.c_str(), st.w, st.h, m.tileset().c_str(), tab.size(), st.spots, st.ore_blocks, st.trees, out_path.c_str(), size, size, us);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    CLI::App app{"mapgen -- procedural strategic-map tooling", "mapgen"};
    app.require_subcommand(1);

    std::vector<std::string> learn_in;
    std::string              learn_out, learn_stock;
    bool                     learn_allow = false;
    CLI::App                *learn       = app.add_subcommand("learn", "learn per-tileset adjacency rules from shipped maps");
    learn->add_option("--in", learn_in, "directory of .MP/.mpm maps (repeatable)")->required();
    learn->add_option("--out", learn_out, "rules output directory (<TILESET>.mgr)")->required();
    learn->add_option("--stock", learn_stock, "stock_maps.sha256 (default: located next to the sources)");
    learn->add_flag("--allow-nonstock", learn_allow, "learn even if inputs differ from the stock list (rules are then not the reference rules)");

    std::string check_map_path, check_rules;
    bool        check_wrap = false;
    CLI::App   *check      = app.add_subcommand("check", "report every adjacent tile pair absent from the learned rules (exit 1 if any)");
    check->add_option("map", check_map_path, ".MP or .mpm file")->required();
    check->add_option("--rules", check_rules, "rules directory from `learn`")->required();
    check->add_flag("--wrap", check_wrap, "also check the wrap seam (planet wraps)");

    std::string prev_map, prev_tlo, prev_out;
    int         prev_size = PREVIEW_SIZE;
    CLI::App   *prev      = app.add_subcommand("preview", "render the in-game map preview (RGB565 thumbnail) of a map as a PNG/BMP");
    prev->add_option("map", prev_map, ".MP or .mpm file")->required();
    prev->add_option("tlo", prev_tlo, "the map's tileset, an extracted .tlo (decompressed from mh.rsr)")->required();
    prev->add_option("out", prev_out, "output image (.png, or .bmp)")->required();
    prev->add_option("--size", prev_size, "square size in pixels (default 128, the in-game size)");

    std::string cal_tlo, cal_rules;
    CLI::App   *cal = app.add_subcommand("calibrate", "report pixel seam scores against observed positive pairs");
    cal->add_option("--tlo", cal_tlo, "decoded strategic TLO")->required();
    cal->add_option("--rules", cal_rules, "learned v3 rules file")->required();

    std::string   infer_tlo, infer_semantic, infer_out, infer_refinements;
    std::uint32_t infer_h = 0, infer_v = 0;
    CLI::App     *infer = app.add_subcommand("infer", "derive RGB565 seams from an explicit semantic/macro template");
    infer->add_option("--tlo", infer_tlo, "decoded strategic TLO")->required();
    infer->add_option("--semantic", infer_semantic, "v3 author template with no h/v records")->required();
    infer->add_option("--h-limit", infer_h, "author-selected horizontal score limit")->required();
    infer->add_option("--v-limit", infer_v, "author-selected vertical score limit")->required();
    infer->add_option("--refinements", infer_refinements, "optional lines: +|- h|v tileA tileB");
    infer->add_option("--out", infer_out, "output validated v3 rules")->required();

    std::string   gen_rules, gen_tileset, gen_out, gen_name, gen_desc, gen_tune;
    std::uint32_t gen_size = 128, gen_players = 4;
    std::uint64_t gen_seed   = 1;
    bool          gen_nowrap = false, gen_nopaths = false, gen_ensure = false, gen_verbose = false;
    CLI::App     *gen = app.add_subcommand("gen", "generate a procedural strategic map (.mpm) from learned rules");
    gen->add_option("--rules", gen_rules, "rules directory from `learn`")->required();
    gen->add_option("--tileset", gen_tileset, "tileset, e.g. skaly256 (diuna256 jungle ksie256 lod256 metal256 ruins256 skaly256)")->required();
    gen->add_option("--size", gen_size, "map width = height: 64, 128 or 256")->required();
    gen->add_option("--players", gen_players, "landing spots: 2 on a 64 map, 4/6/8 on 128/256")->required();
    gen->add_option("--seed", gen_seed, "seed (same arguments -> same bytes)")->required();
    gen->add_option("--out", gen_out, "output .mpm file")->required();
    gen->add_flag("--no-wrap", gen_nowrap, "do not require the wrap seam pairs to be legal (wrap is the default; the plan is torus-legal either way)");
    gen->add_flag("--ensure-paths", gen_ensure, "every landing spot reachable from every other by a no-wrap walk (the default)");
    gen->add_flag("--no-paths", gen_nopaths, "allow island maps: landing spots need not be connected");
    gen->add_flag("--verbose", gen_verbose, "print every failed attempt (why a retry was needed)");
    gen->add_option("--tune", gen_tune, "experimental terrain overrides, key=value,... (reg_div line_div line_q no_lines contours contour_div contour_oct levels fit maj minc spot_mix cshare cfit decal_pct decal_big)");
    gen->add_option("--name", gen_name, "first description line (max 26 characters)");
    gen->add_option("--desc", gen_desc, "description text, word-wrapped (the whole description is max 4 lines x 26 characters)");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e) == 0 ? 0 : EXIT_ERROR;
    }
    try {
        if (learn->parsed()) return run_learn(argv[0], learn_in, learn_out, learn_stock, learn_allow);
        if (cal->parsed()) return run_calibrate(cal_tlo, cal_rules);
        if (infer->parsed()) return run_infer(infer_tlo, infer_semantic, infer_out, infer_h, infer_v, infer_refinements);
        if (prev->parsed()) return run_preview(prev_map, prev_tlo, prev_out, prev_size);
        if (check->parsed()) return run_check(check_map_path, check_rules, check_wrap);
        if (gen->parsed()) {
            if (gen_nopaths && gen_ensure) {
                std::fprintf(stderr, "gen: --ensure-paths and --no-paths contradict each other\n");
                return EXIT_ERROR;
            }
            return run_gen(gen_rules, gen_tileset, gen_size, gen_players, gen_seed, gen_out, !gen_nowrap, !gen_nopaths, gen_name, gen_desc, gen_tune, gen_verbose);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "mapgen: %s\n", e.what());
        return EXIT_ERROR;
    }
    return EXIT_ERROR;
}
