// mh_mapgen -- learned tile rules. Learned from the shipped maps at build/boot time (retail data is
// never committed), stored per tileset as sorted, diffable text:
//
//   mapgen-rules 3
//   tileset diuna256.tlo
//   maps 7
//   inputs_sha256 <hex>             sha256 of the sorted per-map sha256 hex list ("<hex>\n" each)
//   t <id> <freq> <pass> <mixed> <treeok>
//   h <a> <b> <n>                   tile b is immediately right (col+1) of tile a, n times
//   v <a> <b> <n>                   tile b is immediately below (row+1) of tile a, n times
//   kind <k> <n>                    tree-kind histogram
//   stat <size> <key> <value>       per map width; <key>_{n,min,max,sum} integer aggregates
//   decal <cls> <n> <cells> {y x tile}  a ground decal: a group of rare tiles of corner class <cls>, isolated in plain ground
//   (version 3; the stats decal_rate_c<k>_ppb per map width are its density, groups per 1e9 passable class-k tiles)
//
// Pairs are INTERIOR only (the wrap seam is never learned). pass is the majority over cells with no
// tree on them (tie -> blocked); a tile seen only under trees defaults to passable. Everything is
// integer, sorted and independent of input order.
#pragma once
#include "mh_mapgen/mpm.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mh_mapgen {

struct TileRule {
    std::uint16_t id   = 0;
    std::uint32_t freq = 0;
    std::uint8_t  pass = 0, mixed = 0, treeok = 0;
    std::uint8_t  seen = 3; // bit0: seen blocked off-tree, bit1: seen passable off-tree
    // Majority passability over the off-tree cells (tie -> blocked; never seen off-tree -> blocked).
    bool pmaj() const { return seen == 2 || (seen == 3 && pass); }
};

// ---- the macro model -------------------------------------------------------------------------------
// Macro-case tag kinds. tag = kind * 16 + bits.
enum : std::uint8_t {
    TAG_PLAIN  = 0, // pure macro; bits = which edge midpoints a ridge crosses (top, right, bottom, left = bit 0..3)
    TAG_MIXED  = 1, // corners of more than one class (a region edge)
    TAG_CLIFF  = 2, // mixed, with a blocked tile
    TAG_RAMP   = 3, // mixed, all four tiles passable
    TAG_CLEAR  = 4, // pure background, all four tiles passable (start clearings)
    TAG_ORIENT = 5, // pure background with a ridge; bits = the hidden side field at tl, tr, bl, br
};
constexpr std::uint8_t make_tag(std::uint8_t kind, std::uint8_t bits = 0) { return std::uint8_t(kind * 16 + bits); }

struct ClassRule {
    std::int32_t  id = 0, parent = -1, order = -1;
    std::uint32_t share_ppm = 0, pure_uses = 0;
};
struct AdjRule {
    std::int32_t a = 0, b = 0;
};
struct CornerRule {
    std::uint16_t tile = 0;
    std::int8_t   cls[4]{-1, -1, -1, -1};
};
struct CaseRule {
    std::uint8_t  cls[4]{0, 0, 0, 0};
    std::uint8_t  tag = 0;
    std::uint16_t t[4]{0, 0, 0, 0};
    std::uint32_t n = 0;
};
struct StampCell {
    std::int32_t  y = 0, x = 0;
    std::uint16_t tile = 0;
};
struct StampRule {
    std::uint32_t          n = 0;
    std::vector<StampCell> cells;
};
// A decal: a small (1-12 tile) 4-connected group of RARE ground tiles of one pure corner class, isolated in plain tiles of that class.
struct DecalRule {
    std::int32_t           cls = 0;
    std::uint32_t          n   = 0;
    std::vector<StampCell> cells;
};
struct TreeSample {
    std::uint16_t tile = 0;
    std::int32_t  kind = 0, px = 0, py = 0;
    std::uint32_t n = 0;
};
struct OreRec {
    std::uint16_t ch[4]{0, 0, 0, 0};
    std::uint32_t n = 0;
};
struct PairRule {
    std::uint16_t a = 0, b = 0;
    std::uint32_t n = 0;
};
struct KindRule {
    std::int32_t  kind = 0;
    std::uint32_t n    = 0;
};
struct StatRule {
    std::uint32_t size = 0;
    std::string   key;
    std::int64_t  value = 0;
};

struct Rules {
    std::string           tileset;
    std::string           gameplay_profile, tree_profile; // empty for legacy learned rules
    std::uint32_t         maps = 0;
    std::string           inputs_sha256;
    std::vector<TileRule> tiles; // sorted by id
    std::vector<PairRule> h, v;  // sorted by (a, b)
    std::vector<KindRule> kinds; // sorted by kind
    std::vector<StatRule> stats; // sorted by (size, key)

    // the macro model (empty for hand-made rules; the generator then falls back to a one-class model)
    std::vector<ClassRule>  classes; // sorted by id
    std::vector<AdjRule>    adj;
    std::vector<CornerRule> corners; // sorted by tile
    std::vector<CaseRule>   cases;   // sorted by (cls, tag, t)
    std::vector<StampRule>  stamps;
    std::vector<DecalRule>  decals; // sorted by (cls, cells)
    std::vector<TreeSample> tree_samples;
    std::vector<OreRec>     ore_recs;

    const TileRule *find_tile(std::uint16_t id) const;
    std::uint32_t   pair_count(char dir, std::uint16_t a, std::uint16_t b) const; // dir 'h' or 'v'
    bool            find_stat(std::uint32_t size, const std::string &key, std::int64_t &out) const;
    std::int64_t    stat_or(std::uint32_t size, const std::string &key, std::int64_t dflt) const;
};

struct LearnInput {
    const MpmModel *map = nullptr;
    std::string     sha256; // hex of the file bytes; also the dedup key
};

// Learn one tileset. All inputs must share a tileset (see learn_all). Input order is irrelevant.
Rules learn_tileset(const std::vector<LearnInput> &inputs);

// Group by tileset (exact header spelling), drop duplicate sha256, learn each. Sorted by tileset.
std::vector<Rules> learn_all(const std::vector<LearnInput> &inputs);

bool        validate_rules(const Rules &r, std::string &err);
std::string rules_to_text(const Rules &r);
bool        rules_from_text(std::string_view text, Rules &out, std::string &err);

// File stem for a tileset: name without extension, upper-cased, + ".mgr" ("diuna256.tlo" -> "DIUNA256.mgr").
std::string rules_file_name(const std::string &tileset);

// ---- hashing and the stock-map list -------------------------------------------------------------
std::string sha256_hex(const std::uint8_t *data, std::size_t size);

struct StockEntry {
    std::string sha256, name;
};
// Text: "<64 hex>  <filename>" per line; '#' lines and blanks ignored. Sorted by sha256 on return.
bool stock_list_parse(std::string_view text, std::vector<StockEntry> &out, std::string &err);
bool stock_list_contains(const std::vector<StockEntry> &list, const std::string &sha256);

} // namespace mh_mapgen
