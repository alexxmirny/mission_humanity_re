// mapgen_test.exe -- mh_mapgen unit tests.
//   mapgen_test                              synthetic tests only (no retail data needed)
//   mapgen_test --preview <tlo_dir> <maps_dir>...  the 128 px preview CRC goldens of real maps (mods:MAPGEN2)
//   mapgen_test --retail <mh_dir> <maps_dir> + stock-map checks: byte-exact read->write, .mpm id,
//                                              deterministic learn, every map legal against its own rules,
//                                              generator matrix: 7 tilesets x {64,128,256} x 2 seeds
// Exit code 0 only when every check passed.
#include "mh_mapgen/check.h"
#include "mh_mapgen/calibrate.h"
#include "mh_mapgen/file_util.h"
#include "mh_mapgen/gen.h"
#include "mh_mapgen/infer.h"
#include "mh_mapgen/mpm.h"
#include "mh_mapgen/preview.h"
#include "mh_mapgen/rng.h"
#include "mh_mapgen/rules.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace mh_mapgen;
namespace fs = std::filesystem;

static int g_fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

namespace {

std::string hex_of(const std::string &s) { return sha256_hex(reinterpret_cast<const std::uint8_t *>(s.data()), s.size()); }

// A w x h synthetic map whose tile at (col,row) is fn(col,row); passable = tile is even.
MpmModel make_synth(std::uint32_t w, std::uint32_t h, const std::function<int(int, int)> &fn) {
    MpmModel m;
    mpm_init(m, "synth.tlo", w, h);
    for (std::uint32_t c = 0; c < w; ++c)
        for (std::uint32_t r = 0; r < h; ++r) {
            int t                    = fn(int(c), int(r));
            m.flags[m.cell(c, r)]    = std::uint16_t(TILE_FLAG_BASE | t);
            m.passable[m.cell(c, r)] = (t % 2 == 0) ? 1 : 0;
        }
    return m;
}

std::string sha_of(const MpmModel &m) {
    std::vector<std::uint8_t> b = write_mpm(m);
    return sha256_hex(b.data(), b.size());
}

void test_hashes() {
    CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex_of(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const std::uint8_t e[1] = {0};
    CHECK(crc32_ieee(e, 0) == 0u);
    CHECK(crc32_ieee(reinterpret_cast<const std::uint8_t *>("123456789"), 9) == 0xCBF43926u);
    CHECK(crc32_ieee(reinterpret_cast<const std::uint8_t *>("a"), 1) == 0xE8B7BE43u);
}

void test_rng() {
    // pcg32-demo reference: pcg32_srandom_r(&rng, 42u, 54u)
    Pcg32 p;
    p.seed(42, 54);
    const std::uint32_t pcg_ref[6] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
    for (std::uint32_t v : pcg_ref) CHECK(p.next_u32() == v);

    // SplitMix64 reference vectors (Vigna): seed 0, and seed 1234567 (the rand_xoshiro test vector).
    SplitMix64 s0(0);
    CHECK(s0.next() == 0xe220a8397b1dcdafull);
    CHECK(s0.next() == 0x6e789e6aa1b965f4ull);
    CHECK(s0.next() == 0x06c45d188009454full);
    SplitMix64          s1(1234567);
    const std::uint64_t sm_ref[5] = {6457827717110365317ull, 3203168211198807973ull, 9817491932198370423ull, 4593380528125082431ull, 16408922859458223821ull};
    for (std::uint64_t v : sm_ref) CHECK(s1.next() == v);

    // bounded(): in range, deterministic, bound 1 and 0 degenerate.
    Pcg32 a = make_rng(7, 0), b = make_rng(7, 0), c = make_rng(7, 1);
    bool  differs = false;
    for (int i = 0; i < 200; ++i) {
        std::uint32_t x = a.bounded(10), y = b.bounded(10), z = c.bounded(10);
        CHECK(x < 10 && x == y);
        differs |= (x != z);
    }
    CHECK(differs);
    CHECK(a.bounded(1) == 0 && a.bounded(0) == 0);
}

void test_writer_offsets() {
    MpmModel m = make_synth(8, 8, [](int c, int r) { return 1 + (c + r) % 5; });
    m.trees.push_back({1, 3, 4, 5, 0, 2, 3});
    m.trees.push_back({2, 4, 6, 7, 0, 5, 1});
    m.trees.push_back({3, 5, 8, 9, 0, 7, 7});
    m.landings.push_back({1, 1, 1});
    m.landings.push_back({6, 6, 1});
    m.ore[3 * 8 + 0]         = 0x1234;
    m.ore[3 * 8 + 3]         = 0xBEEF;
    m.flags[m.cell(2, 3)]    = TILE_FLAG_BASE | 9;
    m.building[m.cell(2, 3)] = 1;
    mpm_set_tail(m, "Line one\r\nLine two");

    const std::size_t n = 64, body = 0x61 + n * 5 + 16 * 4 + 3 * 28 + 4 * 16 + 2 * 12;
    auto              b = write_mpm(m);
    CHECK(b.size() == body + 4 + 18);
    CHECK(std::memcmp(b.data(), "ver02", 5) == 0);
    CHECK(b[5] == 8 && b[6] == 0 && b[9] == 8);
    CHECK(std::memcmp(b.data() + 0x0d, "synth.tlo", 10) == 0);
    CHECK(b[0x2d] == 128 && b[0x31] == 64 && b[0x35] == 0 && b[0x39] == 3 && b[0x3d] == 4 && b[0x41] == 2);
    // column-major: cell (2,3) is index 2*8+3 = 19
    const std::size_t tile_off = 0x61 + 19 * 4;
    CHECK(b[tile_off] == 9 && b[tile_off + 1] == 0x80 && b[tile_off + 2] == 1 && b[tile_off + 3] == 0);
    const std::size_t pass_off = 0x61 + n * 4;
    CHECK(b[pass_off + 19] == 0); // tile 9 is odd -> blocked
    const std::size_t tree_off = pass_off + n + 16 * 4;
    CHECK(b[tree_off] == 1 && b[tree_off + 4] == 3 && b[tree_off + 20] == 2 && b[tree_off + 24] == 3 && b[tree_off + 28] == 2);
    const std::size_t ore_off = tree_off + 3 * 28;
    CHECK(b[ore_off + 3 * 16] == 0x34 && b[ore_off + 3 * 16 + 1] == 0x12 && b[ore_off + 3 * 16 + 6] == 0xEF && b[ore_off + 3 * 16 + 7] == 0xBE);
    const std::size_t land_off = ore_off + 4 * 16;
    CHECK(b[land_off] == 1 && b[land_off + 4] == 1 && b[land_off + 8] == 1 && b[land_off + 12] == 6);
    CHECK(std::memcmp(b.data() + body + 4, "Line one\r\nLine two", 18) == 0);

    MpmModel    back;
    std::string err;
    CHECK(read_mpm(b.data(), b.size(), back, err));
    CHECK(back.has_tail && back.tail_id == m.tail_id && back.trees.size() == 3 && back.landings.size() == 2);
    CHECK(back.tileset() == "synth.tlo");
    CHECK(write_mpm(back) == b);

    // Truncation and bad magic are refused, never crash.
    CHECK(!read_mpm(b.data(), 10, back, err));
    CHECK(!read_mpm(b.data(), body - 1, back, err));
    std::vector<std::uint8_t> bad = b;
    bad[0]                        = 'X';
    CHECK(!read_mpm(bad.data(), bad.size(), back, err));
    CHECK(!read_mpm(b.data(), body + 2, back, err)); // 2 stray bytes: neither a body nor an id
    // .MP form: no tail.
    m.has_tail = false;
    auto mp    = write_mpm(m);
    CHECK(mp.size() == body && read_mpm(mp.data(), mp.size(), back, err) && !back.has_tail);

    // A twelve-visible-byte DOS 8.3 token retains its thirteenth NUL and
    // all following header bytes; neither the body nor the footer shifts.
    MpmModel named;
    mpm_init(named, "diuna256.tlo", 8, 8);
    named.header_raw[NAME_OFF + 13] = 0x5a;
    mpm_set_tail(named, "token fixture");
    const auto encoded = write_mpm(named);
    CHECK(encoded[NAME_OFF + 12] == 0 && encoded[NAME_OFF + 13] == 0x5a);
    CHECK(read_mpm(encoded.data(), encoded.size(), back, err));
    CHECK(back.tileset() == "diuna256.tlo");
    CHECK(write_mpm(back) == encoded);
}

void test_mpm_id() {
    MpmModel m = make_synth(8, 8, [](int c, int r) { return 2 + (c * 3 + r) % 4; });
    mpm_set_tail(m, "x");
    auto body = write_mpm_body(m);
    CHECK(m.tail_id == crc32_ieee(body.data(), body.size()));
    CHECK(mpm_compute_id(m) == m.tail_id);
    auto whole = write_mpm(m);
    CHECK(std::vector<std::uint8_t>(whole.begin(), whole.begin() + std::ptrdiff_t(body.size())) == body);
    // the id covers the body only: changing a tile changes it
    std::uint32_t before = m.tail_id;
    m.flags[0] ^= 1;
    CHECK(mpm_compute_id(m) != before);
}

std::vector<LearnInput> inputs_of(const std::vector<MpmModel> &ms, std::vector<std::string> &shas) {
    shas.clear();
    for (const MpmModel &m : ms) shas.push_back(sha_of(m));
    std::vector<LearnInput> in;
    for (std::size_t i = 0; i < ms.size(); ++i) in.push_back({&ms[i], shas[i]});
    return in;
}

void test_rules() {
    // period 3 across columns, period 2 down rows: interior pairs are exactly the pattern's transitions
    auto                  pat = [](int c, int r) { return 1 + (c % 3) + 3 * (r % 2); };
    std::vector<MpmModel> ms;
    ms.push_back(make_synth(8, 8, pat));
    ms.push_back(make_synth(16, 16, pat));
    ms[1].trees.push_back({1, 7, 0, 0, 0, 4, 4});
    ms[1].building[ms[1].cell(4, 4)] = 1;
    ms[1].passable[ms[1].cell(4, 4)] = 0;
    ms[1].landings.push_back({8, 8, 1});
    ms[1].ore[(1 * 4 + 1) * 8] = 500; // block (1,1): centre (6,6); distance to (8,8)^2 = 8 <= 400

    std::vector<std::string> shas;
    auto                     in = inputs_of(ms, shas);
    Rules                    r  = learn_tileset(in);
    CHECK(r.tileset == "synth.tlo" && r.maps == 2);
    CHECK(r.tiles.size() == 6);
    std::uint64_t freq = 0;
    for (const TileRule &t : r.tiles) freq += t.freq;
    CHECK(freq == 64 + 256);
    const TileRule *t5 = r.find_tile(5);
    CHECK(t5 && t5->treeok == 0);
    // tile at (4,4) under the pattern = 1 + 1 + 0 = 2 : tree sits on it
    CHECK(r.find_tile(2) && r.find_tile(2)->treeok == 1 && r.find_tile(2)->pass == 1);
    CHECK(r.pair_count('h', 1, 2) > 0 && r.pair_count('h', 3, 1) > 0 && r.pair_count('h', 2, 1) == 0);
    CHECK(r.pair_count('v', 1, 4) > 0 && r.pair_count('v', 4, 1) > 0 && r.pair_count('v', 1, 1) == 0);
    CHECK(r.kinds.size() == 1 && r.kinds[0].kind == 7 && r.kinds[0].n == 1);
    std::int64_t sv = 0;
    CHECK(r.find_stat(16, "maps", sv) && sv == 1);
    CHECK(r.find_stat(16, "ore_near_blocks_sum", sv) && sv == 1);
    CHECK(r.find_stat(16, "ore_near_total_sum", sv) && sv == 500);
    CHECK(r.find_stat(8, "landings_max", sv) && sv == 0);
    CHECK(r.find_stat(16, "trees_sum", sv) && sv == 1);

    // text round-trip, sortedness, and order/duplicate independence
    std::string text = rules_to_text(r);
    Rules       back;
    std::string err;
    CHECK(rules_from_text(text, back, err));
    CHECK(rules_to_text(back) == text);
    CHECK(text.rfind("mapgen-rules 3\ntileset synth.tlo\n", 0) == 0);
    std::vector<LearnInput> rev(in.rbegin(), in.rend());
    rev.push_back(in[0]); // duplicate
    CHECK(rules_to_text(learn_tileset(rev)) == text);
    CHECK(learn_all(in).size() == 1);
    CHECK(rules_file_name("diuna256.tlo") == "DIUNA256.mgr");
    Rules bogus;
    CHECK(!rules_from_text("nope 1\n", bogus, err));
    CHECK(!rules_from_text("mapgen-rules 3\ntileset a\nbogus line\n", bogus, err));
    CHECK(!rules_from_text("mapgen-rules 2\ntileset a\n", bogus, err)); // the decal catalogue needs a re-learn: old files are refused
    // decal lines: round trip, and a wrong cell count is refused
    Rules dec;
    CHECK(rules_from_text("mapgen-rules 3\ntileset a\ndecal 0 3 2 0 0 7 0 1 8\n", dec, err) && dec.decals.size() == 1 && dec.decals[0].n == 3 && dec.decals[0].cells.size() == 2 && dec.decals[0].cells[1].tile == 8);
    CHECK(rules_to_text(dec).find("decal 0 3 2 0 0 7 0 1 8\n") != std::string::npos);
    CHECK(!rules_from_text("mapgen-rules 3\ntileset a\ndecal 0 3 2 0 0 7\n", bogus, err));

    // tileset grouping: a second tileset gets its own rules
    ms.push_back(make_synth(8, 8, pat));
    ms[2].header_raw[0x0d] = 'q';
    std::vector<std::string> shas2;
    auto                     in2 = inputs_of(ms, shas2);
    CHECK(learn_all(in2).size() == 2);

    // stock list
    std::vector<StockEntry> sl;
    std::string             list = "# c\n" + shas[1] + "  b.mpm\r\n\n" + shas[0] + "  a.MP\n";
    CHECK(stock_list_parse(list, sl, err) && sl.size() == 2);
    CHECK(stock_list_contains(sl, shas[0]) && stock_list_contains(sl, shas[1]) && !stock_list_contains(sl, shas2[2].substr(1) + "0"));
    CHECK(!stock_list_parse("abc  x\n", sl, err));
}


void test_rules_safety_and_calibration() {
    Rules r;
    r.inputs_sha256    = std::string(64, '0');
    r.tileset          = "custom.tlo";
    r.gameplay_profile = "diuna256";
    r.tree_profile     = "jungle";
    TileRule tile;
    tile.id   = 0;
    tile.freq = 1;
    tile.pass = 1;
    tile.seen = 2;
    r.tiles.push_back(tile);
    r.h.push_back({0, 0, 1});
    r.v.push_back({0, 0, 1});
    r.classes.push_back({0, -1, 0, 1000000, 1});
    CaseRule c;
    c.tag = make_tag(TAG_CLEAR);
    c.n   = 1;
    r.cases.push_back(c);
    r.kinds.push_back({1, 1});
    r.tree_samples.push_back({0, 1, 16, 16, 1});
    OreRec ore;
    ore.ch[0] = 500;
    ore.n     = 1;
    r.ore_recs.push_back(ore);
    std::string err;
    CHECK(validate_rules(r, err));
    const auto text = rules_to_text(r);
    Rules      parsed;
    CHECK(rules_from_text(text, parsed, err));
    CHECK(parsed.gameplay_profile == "diuna256" && parsed.tree_profile == "jungle");
    CHECK(validate_rules(parsed, err));
    CHECK(rules_to_text(parsed) == text);
    CHECK(!rules_from_text(text + "profile diuna256 jungle\n", parsed, err));
    CHECK(!rules_from_text("mapgen-rules 3\ntileset a\nprofile bogus jungle\n", parsed, err));
    CHECK(!rules_from_text("mapgen-rules 3\ntileset a\nt 0 1 256 0 0 2\n", parsed, err));
    CHECK(!rules_from_text("mapgen-rules 3\ntileset a\nc 0 256 0 0 0\n", parsed, err));
    Rules bad  = r;
    bad.h[0].b = 1;
    CHECK(!validate_rules(bad, err));
    bad             = r;
    bad.tiles[0].id = 4096;
    CHECK(!validate_rules(bad, err));
    bad                   = r;
    bad.classes[0].parent = 0;
    CHECK(!validate_rules(bad, err));
    bad               = r;
    bad.cases[0].t[1] = 1;
    CHECK(!validate_rules(bad, err));
    bad                    = r;
    bad.tree_samples[0].px = 32;
    CHECK(!validate_rules(bad, err));
    bad               = r;
    bad.ore_recs[0].n = 1000001;
    CHECK(!validate_rules(bad, err));
    bad                   = r;
    bad.ore_recs[0].ch[0] = 3277;
    CHECK(!validate_rules(bad, err));
    bad              = r;
    bad.tree_profile = "none";
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.stamps.push_back({1, {{0, 0, 0}, {0, 0, 0}}});
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.decals.push_back({0, 1, {{0, 0, 1}}});
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.tiles.resize(1025);
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.h.push_back(bad.h.front());
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.v.push_back(bad.v.front());
    CHECK(!validate_rules(bad, err));
    bad     = r;
    tile.id = 1;
    bad.tiles.push_back(tile);
    bad.h = {{1, 0, 1}, {0, 1, 1}};
    CHECK(!validate_rules(bad, err));
    bad     = r;
    tile.id = 1;
    bad.tiles.push_back(tile);
    bad.v = {{1, 0, 1}, {0, 1, 1}};
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.classes.push_back({1, -1, -1, 0, 0});
    bad.adj = {{0, 1}, {0, 1}};
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.classes.push_back({1, -1, -1, 0, 0});
    bad.adj = {{1, 0}};
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.cases.push_back(bad.cases.front());
    CHECK(!validate_rules(bad, err));
    bad = r;
    {
        CaseRule earlier = bad.cases.front();
        earlier.tag      = make_tag(TAG_PLAIN);
        bad.cases.push_back(earlier);
    }
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.tree_samples.push_back(bad.tree_samples.front());
    CHECK(!validate_rules(bad, err));
    bad = r;
    {
        TreeSample earlier = bad.tree_samples.front();
        earlier.px         = 15;
        bad.tree_samples.push_back(earlier);
    }
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.classes.push_back({1, 0, 1, 0, 0});
    bad.classes[0].parent = 1;
    CHECK(!validate_rules(bad, err));
    bad = r;
    bad.classes.clear();
    bad.cases.clear();
    CHECK(!validate_rules(bad, err));
    // Distinct last coordinate and a duplicate at the end exercise both bitset boundaries.
    bad = r;
    bad.stamps.push_back({1, {{0, 0, 0}, {255, 255, 0}}});
    CHECK(validate_rules(bad, err));
    bad.stamps.back().cells.push_back({255, 255, 0});
    CHECK(!validate_rules(bad, err));
    Rules legacy = r;
    legacy.gameplay_profile.clear();
    legacy.tree_profile.clear();
    CHECK(rules_to_text(legacy).find("profile ") == std::string::npos);

    // Duplicate palette entries must give identical scores despite different indices.
    std::vector<std::uint8_t> art(512 + 2048, 0);
    art[2] = 0xff;
    art[3] = 0xff;
    art[4] = 0xff;
    art[5] = 0xff;
    std::fill(art.begin() + 512, art.begin() + 1536, std::uint8_t(1));
    std::fill(art.begin() + 1536, art.end(), std::uint8_t(2));
    Rules pixel;
    pixel.tileset = "pixels.tlo";
    tile.id       = 0;
    pixel.tiles.push_back(tile);
    tile.id = 1;
    pixel.tiles.push_back(tile);
    pixel.h.push_back({0, 1, 7});
    pixel.v.push_back({0, 1, 9});
    PixelCalibration cal;
    CHECK(calibrate_pixels(art.data(), art.size(), pixel, cal, err));
    CHECK(cal.horizontal.pairs.size() == 4 && cal.horizontal.positive_weight == 7);
    for (const auto &p : cal.horizontal.pairs) CHECK(p.score == 0);
    art[4] = 0;
    art[5] = 0;
    CHECK(calibrate_pixels(art.data(), art.size(), pixel, cal, err));
    bool found = false;
    for (const auto &p : cal.horizontal.pairs)
        if (p.a == 0 && p.b == 1) {
            found = true;
            CHECK(p.score > 0 && p.weight == 7);
        }
    CHECK(found);
    art.pop_back();
    CHECK(!calibrate_pixels(art.data(), art.size(), pixel, cal, err));
}

void test_pixel_inference() {
    Rules semantic;
    semantic.tileset          = "kam256.tlo";
    semantic.gameplay_profile = "diuna256";
    semantic.tree_profile     = "none";
    semantic.inputs_sha256    = std::string(64, '0');
    semantic.classes.push_back({0, -1, 0, 1000000, 1});
    for (std::uint16_t id = 0; id < 2; ++id) {
        TileRule t;
        t.id   = id;
        t.freq = 10;
        t.pass = 1;
        t.seen = 2;
        semantic.tiles.push_back(t);
        CornerRule corner;
        corner.tile = id;
        for (int q = 0; q < 4; ++q) corner.cls[q] = 0;
        semantic.corners.push_back(corner);
        CaseRule c;
        for (int q = 0; q < 4; ++q) c.t[q] = id;
        c.n = 1;
        semantic.cases.push_back(c);
    }
    CaseRule clear;
    clear.tag = make_tag(TAG_CLEAR);
    clear.n   = 1;
    semantic.cases.push_back(clear);
    OreRec ore;
    ore.ch[0] = 500;
    ore.ch[1] = 300;
    ore.ch[2] = 300;
    ore.ch[3] = 300;
    ore.n     = 10;
    semantic.ore_recs.push_back(ore);
    std::vector<std::uint8_t> art(512 + 2048, 0);
    art[2] = 0xff;
    art[3] = 0xff;
    std::fill(art.begin() + 1536, art.end(), std::uint8_t(1));
    InferenceOptions opts{0, 0, {}};
    Rules            out;
    InferenceReport  rep;
    std::string      err;
    CHECK(infer_rules(art.data(), art.size(), semantic, opts, out, rep, err));
    CHECK(out.h.size() == 2 && out.v.size() == 2 && out.pair_count('h', 0, 0) == 1 && out.pair_count('h', 0, 1) == 0);
    CHECK(out.gameplay_profile == "diuna256" && out.tree_profile == "none" && out.tiles[0].pmaj());
    const std::string first = rules_to_text(out);
    Rules             repeat;
    CHECK(infer_rules(art.data(), art.size(), semantic, opts, repeat, rep, err));
    CHECK(rules_to_text(repeat) == first);
    opts.refinements.push_back({'h', 0, 1, true});
    CHECK(infer_rules(art.data(), art.size(), semantic, opts, repeat, rep, err));
    CHECK(repeat.pair_count('h', 0, 1) == 1 && rep.added == 1);
    opts.refinements.push_back({'h', 0, 1, true});
    CHECK(!infer_rules(art.data(), art.size(), semantic, opts, repeat, rep, err));
    opts = {0, 0, {{'h', 0, 0, false}}};
    CHECK(!infer_rules(art.data(), art.size(), semantic, opts, repeat, rep, err));
    opts = {100000, 100000, {}};
    CHECK(!infer_rules(art.data(), art.size(), semantic, opts, repeat, rep, err));
    opts                        = {0, 0, {}};
    Rules ambiguous             = semantic;
    ambiguous.corners[0].cls[0] = -1;
    CHECK(!infer_rules(art.data(), art.size(), ambiguous, opts, repeat, rep, err));
    ambiguous = semantic;
    ambiguous.h.push_back({0, 0, 1});
    CHECK(!infer_rules(art.data(), art.size(), ambiguous, opts, repeat, rep, err));
    ambiguous = semantic;
    ambiguous.ore_recs.clear();
    CHECK(!infer_rules(art.data(), art.size(), ambiguous, opts, repeat, rep, err));
    Rules name_rules   = semantic;
    name_rules.tileset = "Diuna256.tlo";
    GenParams names;
    names.tileset = "diuna256.TLO";
    names.size    = 64;
    names.players = 2;
    CHECK(gen_validate(name_rules, names).empty());
    names.tileset = "123456789.tlo";
    CHECK(!gen_validate(name_rules, names).empty());
    names.tileset = "../x.tlo";
    CHECK(!gen_validate(name_rules, names).empty());
    names.tileset = "x.y.tlo";
    CHECK(!gen_validate(name_rules, names).empty());
    names.tileset      = "Diuna256.tlo";
    name_rules.tileset = "123456789.tlo";
    CHECK(!gen_validate(name_rules, names).empty());
    name_rules.tileset = "../x.tlo";
    CHECK(!gen_validate(name_rules, names).empty());
    // Actual map generation consumes the inferred rules, not canned expected source text.
    GenParams gp;
    gp.tileset          = "kam256.tlo";
    gp.size             = 64;
    gp.players          = 2;
    gp.seed             = 12345;
    GenResult generated = generate(out, gp);
    CHECK(generated.ok);
    if (generated.ok) {
        CHECK(check_map(generated.map, out, true).ok());
        std::string why;
        CHECK(map_gates(generated.map, true, why));
    }
}


void test_inferred_composite() {
    Rules s;
    s.tileset          = "parts.tlo";
    s.gameplay_profile = "diuna256";
    s.tree_profile     = "none";
    s.inputs_sha256    = std::string(64, '0');
    s.classes.push_back({0, -1, 0, 1000000, 1});
    for (std::uint16_t id = 0; id < 4; ++id) {
        TileRule t;
        t.id   = id;
        t.freq = 100;
        t.pass = 1;
        t.seen = 2;
        s.tiles.push_back(t);
        CornerRule c;
        c.tile = id;
        for (int q = 0; q < 4; ++q) c.cls[q] = 0;
        s.corners.push_back(c);
    }
    CaseRule plain;
    plain.n = 100;
    s.cases.push_back(plain);
    CaseRule piece;
    piece.n    = 25;
    piece.t[0] = 1;
    piece.t[1] = 2;
    s.cases.push_back(piece);
    CaseRule white;
    white.n = 1;
    for (int q = 0; q < 4; ++q) white.t[q] = 3;
    s.cases.push_back(white);
    CaseRule clear;
    clear.tag = make_tag(TAG_CLEAR);
    clear.n   = 100;
    s.cases.push_back(clear);
    s.stamps.push_back({1, {{0, 0, 1}, {0, 1, 2}}});
    s.stats.push_back({0, "stamp_rate_ppb", 1000000});
    OreRec ore;
    ore.ch[0] = 500;
    ore.ch[1] = 300;
    ore.ch[2] = 300;
    ore.ch[3] = 300;
    ore.n     = 1;
    s.ore_recs.push_back(ore);
    std::vector<std::uint8_t> art(512 + 4 * 1024, 0);
    art[2] = 0x04;
    art[3] = 0x21; // medium grey RGB565 foreground
    art[4] = 0xff;
    art[5] = 0xff; // distinct white background tile 3
    std::fill(art.end() - 1024, art.end(), std::uint8_t(2));
    for (int y = 12; y < 20; ++y) {
        art[512 + 1024 + y * 32 + 30] = art[512 + 1024 + y * 32 + 31] = 1;
        art[512 + 2 * 1024 + y * 32] = art[512 + 2 * 1024 + y * 32 + 1] = 1;
    }
    std::string                err;
    Rules                      out;
    InferenceReport            report;
    std::vector<std::uint16_t> decoded;
    CHECK(decode_tlo565(art.data(), art.size(), s, decoded, err));
    if (decoded.size() == 4 * 1024) {
        CHECK(pixel_seam_score(decoded.data() + 1024, decoded.data(), false) <= 1000);
        CHECK(pixel_seam_score(decoded.data(), decoded.data() + 2 * 1024, false) <= 1000);
    }
    CHECK(infer_rules(art.data(), art.size(), s, {1000, 1000, {}}, out, report, err));
    CHECK(out.pair_count('h', 1, 2) == 1 && out.pair_count('h', 1, 0) == 0 && out.pair_count('h', 0, 2) == 0);
    CHECK(out.pair_count('h', 0, 1) == 1 && out.pair_count('h', 2, 0) == 1 && out.pair_count('v', 1, 2) == 0);
    Rules bad                   = s;
    bad.stamps[0].cells[1].tile = 1;
    CHECK(!infer_rules(art.data(), art.size(), bad, {1000, 1000, {}}, out, report, err));
    bad                      = s;
    bad.stamps[0].cells[1].x = 2;
    CHECK(!infer_rules(art.data(), art.size(), bad, {1000, 1000, {}}, out, report, err));
    auto broken = art;
    std::fill(broken.begin() + 512 + 2 * 1024, broken.begin() + 512 + 3 * 1024, std::uint8_t(2));
    CHECK(!infer_rules(broken.data(), broken.size(), s, {1000, 1000, {}}, out, report, err));
    CHECK(infer_rules(art.data(), art.size(), s, {1000, 1000, {}}, out, report, err));
    GenParams gp;
    gp.tileset                = s.tileset;
    gp.size                   = 64;
    gp.players                = 2;
    gp.seed                   = 12345;
    const GenResult generated = generate(out, gp);
    CHECK(generated.ok);
    if (generated.ok) {
        std::size_t pieces = 0;
        for (std::uint32_t x = 0; x < gp.size; ++x)
            for (std::uint32_t y = 0; y < gp.size; ++y) {
                const auto t = generated.map.tile_at(x, y);
                if (t == 1) {
                    ++pieces;
                    CHECK(generated.map.tile_at((x + 1) % gp.size, y) == 2);
                }
                if (t == 2) CHECK(generated.map.tile_at((x + gp.size - 1) % gp.size, y) == 1);
            }
        CHECK(pieces > 0);
        CHECK(check_map(generated.map, out, true).ok());
    }
}

void test_checker() {
    auto                  pat = [](int c, int r) { return 1 + (c % 3) + 3 * (r % 2); };
    std::vector<MpmModel> ms;
    ms.push_back(make_synth(8, 8, pat));
    std::vector<std::string> shas;
    Rules                    r = learn_tileset(inputs_of(ms, shas));

    // clean map: green, non-wrap and the wrap seam is red (8 % 3 != 0 -> 8 horizontal seam pairs)
    Report ok = check_map(ms[0], r, false);
    CHECK(ok.ok() && ok.illegal_pairs.empty() && ok.pairs_checked == 2 * 7 * 8);
    Report wr = check_map(ms[0], r, true);
    CHECK(wr.illegal_pairs.size() == 8 && wr.pairs_checked == 2 * 8 * 8);
    for (const IllegalPair &p : wr.illegal_pairs) CHECK(p.seam && p.dir == 'h' && p.x == 7);

    // planted illegal tile 99 at (3,3): 4 broken pairs at known coordinates + 1 unknown tile
    MpmModel bad              = ms[0];
    bad.flags[bad.cell(3, 3)] = TILE_FLAG_BASE | 99;
    Report rep                = check_map(bad, r, false);
    CHECK(!rep.ok() && rep.unknown_tiles == 1 && rep.illegal_pairs.size() == 4);
    std::set<std::string> got;
    for (const IllegalPair &p : rep.illegal_pairs) got.insert(format_illegal(p));
    CHECK(got.count("2 3 h " + std::to_string(pat(2, 3)) + " 99") == 1);
    CHECK(got.count("3 3 h 99 " + std::to_string(pat(4, 3))) == 1);
    CHECK(got.count("3 2 v " + std::to_string(pat(3, 2)) + " 99") == 1);
    CHECK(got.count("3 3 v 99 " + std::to_string(pat(3, 4))) == 1);

    // a swapped-but-known pair is also red: (3,3) -> a legal tile that never follows its left neighbour
    MpmModel bad2               = ms[0];
    bad2.flags[bad2.cell(3, 3)] = TILE_FLAG_BASE | 3; // left neighbour (2,3) = 1+2+3 = 6 -> 3 never seen
    Report rep2                 = check_map(bad2, r, false);
    CHECK(!rep2.illegal_pairs.empty() && rep2.unknown_tiles == 0);

    // generation set: a pair seen once is in the checker's set (>=1) but not the generator's (>=2)
    PairSet p1 = pair_set(r, 1), p2 = pair_set(r, 2);
    CHECK(!p1.h.empty() && p2.h.size() <= p1.h.size());
    CHECK(p1.has_h(1, 2) && p2.has_h(1, 2) && !p1.has_h(2, 1));
    ms.push_back(ms[0]);
    ms[1].flags[ms[1].cell(0, 0)] = TILE_FLAG_BASE | 4; // creates a one-off (4,2) h pair and (4,5)... vs the unique-sha dedup
    std::vector<std::string> shas3;
    Rules                    r3 = learn_tileset(inputs_of(ms, shas3));
    PairSet                  q1 = pair_set(r3, 1), q2 = pair_set(r3, 2);
    CHECK(q1.h.size() > q2.h.size());
}

// ---- generator (synthetic rules, no retail data) ----------------------------------------------------------
// Six tiles, every pair legal: 1-4 passable ground (1 and 2 may carry trees), 5-6 blocked.
Rules make_open_rules() {
    Rules r;
    r.tileset = "synth.tlo";
    r.maps    = 1;
    for (std::uint16_t id = 1; id <= 6; ++id) {
        TileRule t;
        t.id     = id;
        t.freq   = id <= 4 ? 1000 : 40; // blocked tiles are rare: the build-fit gate needs open 6x6 windows around a spot
        t.pass   = id <= 4 ? 1 : 0;
        t.mixed  = 0;
        t.treeok = id <= 2 ? 1 : 0;
        r.tiles.push_back(t);
        for (std::uint16_t b = 1; b <= 6; ++b) {
            r.h.push_back({id, b, 5});
            r.v.push_back({id, b, 5});
        }
    }
    r.kinds.push_back({1, 10});
    r.kinds.push_back({2, 5});
    r.stats.push_back({64, "trees_max", 30});
    r.stats.push_back({64, "trees_per10k_max", 60});
    r.stats.push_back({64, "trees_per10k_min", 30});
    return r;
}

// A horizontal period-3 lattice (1 -> 2 -> 3 -> 1) with vertical self-pairs, plus an isolated blocked tile 4.
// Wrapping a width that is not a multiple of 3 is impossible, and nothing can sit next to tile 4 but itself.
Rules make_impossible_rules() {
    Rules r;
    r.tileset = "synth.tlo";
    r.maps    = 1;
    for (std::uint16_t id = 1; id <= 4; ++id) {
        TileRule t;
        t.id   = id;
        t.freq = 100;
        t.pass = id <= 3 ? 1 : 0;
        r.tiles.push_back(t);
        r.v.push_back({id, id, 5});
    }
    r.h.push_back({1, 2, 5});
    r.h.push_back({2, 3, 5});
    r.h.push_back({3, 1, 5});
    r.h.push_back({4, 4, 5});
    return r;
}

std::string bytes_sha(const MpmModel &m) {
    const std::vector<std::uint8_t> b = write_mpm(m);
    return sha256_hex(b.data(), b.size());
}

// The pinned output of generate(make_open_rules(), {64, 2 players, seed 12345, wrap}): identical on Win32 and x64,
// Debug and Release. A change here means the generator's output changed -- re-pin only on purpose.
constexpr const char *GOLDEN_SYNTH_SHA256 = "a5650d4ac01ffb374a35e662bc76115479b523b7a994a858fedf66c5a032c93b";

void check_generated(const MpmModel &m, const Rules &r, bool wrap, std::uint32_t size, std::uint32_t players) {
    std::string err, why;
    CHECK(mpm_validate(m, err));
    CHECK(m.width == size && m.height == size && m.has_tail);
    CHECK(m.landings.size() == players);
    CHECK(m.tail_id == mpm_compute_id(m));
    Report rep = check_map(m, r, wrap);
    CHECK(rep.ok());
    CHECK(landings_connected(m, why));
    CHECK(map_gates(m, true, why));
    // trees: one per tile, slot = i+1, no stacking, passability 0 under a tree, ore channels 4-7 zero
    CHECK(m.trees.size() <= 10000);
    for (std::size_t i = 0; i < m.trees.size(); ++i) {
        const MpmTree &t = m.trees[i];
        CHECK(t.slot == i + 1 && t.next == 0 && t.px_x >= 0 && t.px_x < 32 && t.px_y >= 0 && t.px_y < 32);
        const std::size_t c = m.cell(std::uint32_t(t.x), std::uint32_t(t.y));
        CHECK(m.building[c] == t.slot && m.passable[c] == 0);
    }
    std::size_t with_building = 0;
    for (std::size_t c = 0; c < m.building.size(); ++c) with_building += m.building[c] != 0;
    CHECK(with_building == m.trees.size());
    for (std::size_t b = 0; b * ORE_WORDS < m.ore.size(); ++b)
        for (std::size_t ch = 4; ch < ORE_WORDS; ++ch) CHECK(m.ore[b * ORE_WORDS + ch] == 0);
    for (std::uint8_t p : m.passable) CHECK(p <= 1);
    for (const MpmLanding &l : m.landings) CHECK(l.status == 1);
    // description: <= 4 lines x 26 columns, CRLF, printable ASCII
    const std::string d(m.tail_desc.begin(), m.tail_desc.end());
    CHECK(d.size() <= 254 && d.find('\0') == std::string::npos);
    std::size_t lines = 1, col = 0;
    for (std::size_t i = 0; i < d.size(); ++i) {
        if (d[i] == '\r' && i + 1 < d.size() && d[i + 1] == '\n') {
            ++lines;
            col = 0;
            ++i;
        } else {
            CHECK(d[i] >= 32 && d[i] < 127);
            CHECK(++col <= 26);
        }
    }
    CHECK(lines <= 4);
    // byte-exact round trip
    const std::vector<std::uint8_t> b = write_mpm(m);
    MpmModel                        back;
    CHECK(read_mpm(b.data(), b.size(), back, err) && write_mpm(back) == b);
}

void test_generator() {
    const Rules r = make_open_rules();
    GenParams   p;
    p.tileset   = "synth.tlo";
    p.size      = 64;
    p.players   = 2;
    p.seed      = 12345;
    GenResult a = generate(r, p), b = generate(r, p);
    CHECK(a.ok && b.ok);
    if (a.ok && b.ok) {
        check_generated(a.map, r, true, 64, 2);
        CHECK(write_mpm(a.map) == write_mpm(b.map));
        const std::string sha = bytes_sha(a.map);
        std::printf("synthetic golden sha256: %s (attempts %u, repairs %u, resets %u)\n", sha.c_str(), a.stats.attempts, a.stats.repair_passes, a.stats.resets);
        CHECK(sha == GOLDEN_SYNTH_SHA256);
        GenParams q = p;
        q.seed      = 12346;
        GenResult c = generate(r, q);
        CHECK(c.ok && bytes_sha(c.map) != sha);
        q.wrap      = false;
        GenResult d = generate(r, q);
        CHECK(d.ok);
        if (d.ok) check_generated(d.map, r, false, 64, 2);
        q.wrap      = true;
        q.name      = "A very long map name that must be cut";
        q.desc      = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen";
        GenResult e = generate(r, q);
        CHECK(e.ok);
        if (e.ok) check_generated(e.map, r, true, 64, 2);
    }
    // a larger synthetic map with more players
    p.size        = 128;
    p.players     = 6;
    GenResult big = generate(r, p);
    CHECK(big.ok);
    if (big.ok) check_generated(big.map, r, true, 128, 6);

    // envelope and tileset validation
    GenParams bad = p;
    bad.size      = 100;
    CHECK(!generate(r, bad).ok);
    bad         = p;
    bad.size    = 64;
    bad.players = 4;
    CHECK(!generate(r, bad).ok);
    bad         = p;
    bad.players = 5;
    CHECK(!generate(r, bad).ok);
    bad         = p;
    bad.tileset = "other.tlo";
    CHECK(!generate(r, bad).ok && !gen_validate(r, bad).empty());

    // a planted impossible ruleset fails cleanly, with a reason, after the attempt cap
    const Rules imp = make_impossible_rules();
    p.size          = 64;
    p.players       = 2;
    GenResult f     = generate(imp, p);
    CHECK(!f.ok && !f.reason.empty() && f.stats.attempts <= GEN_MAX_ATTEMPTS);
    std::printf("planted-impossible reason: %s\n", f.reason.c_str());
    // ... and so does a ruleset with no usable tile at all
    Rules empty = make_open_rules();
    empty.h.clear();
    empty.v.clear();
    GenResult g = generate(empty, p);
    CHECK(!g.ok && !g.reason.empty());
}

// Every hard gate goes red on a planted case (and the generated map is green before the plant).
void test_gates() {
    const Rules r = make_open_rules();
    GenParams   p;
    p.tileset   = "synth.tlo";
    p.size      = 64;
    p.players   = 2;
    p.seed      = 12345;
    GenResult g = generate(r, p);
    CHECK(g.ok);
    if (!g.ok) return;
    std::string why;
    CHECK(map_gates(g.map, true, why));
    auto red = [&](const char *label, const std::function<void(MpmModel &)> &plant, const char *needle) {
        MpmModel m = g.map;
        plant(m);
        std::string w;
        const bool  ok = map_gates(m, true, w);
        if (ok || w.find(needle) == std::string::npos) {
            std::printf("FAIL gate %s: ok=%d reason '%s' (wanted '%s')\n", label, ok ? 1 : 0, w.c_str(), needle);
            ++g_fails;
        }
    };
    const MpmLanding l0 = g.map.landings[0];
    red("odd spot count", [](MpmModel &m) { m.landings.push_back(m.landings[0]); }, "not even");
    red("spots too close", [&](MpmModel &m) { m.landings[1].x = l0.x + 4; m.landings[1].y = l0.y; }, "tiles apart");
    red("footprint blocked", [&](MpmModel &m) { m.passable[m.cell(std::uint32_t(l0.x + 2), std::uint32_t(l0.y + 1))] = 0; }, "footprint");
    red("clear radius 3", [&](MpmModel &m) { m.passable[m.cell(std::uint32_t(l0.x - 4), std::uint32_t(l0.y))] = 0; }, "clear radius");
    red("build-fit low", [&](MpmModel &m) {
            for (int dy = -20; dy <= 20; ++dy)
                for (int dx = -20; dx <= 20; ++dx)
                    if (std::max(std::abs(dx), std::abs(dy)) > 4 && (dx + dy + 100) % 5 == 0) m.passable[m.cell(std::uint32_t((l0.x + dx + std::int32_t(m.width)) % std::int32_t(m.width)), std::uint32_t((l0.y + dy + std::int32_t(m.height)) % std::int32_t(m.height)))] = 0; }, "build-fit");
    red("footprint leaves map", [&](MpmModel &m) { m.landings[0].x = std::int32_t(m.width) - 3; }, "inside the map");
    red("ore above 3276", [](MpmModel &m) { m.ore[0] = 3277; }, "3276");
    red("ore on the footprint", [&](MpmModel &m) { m.ore[std::size_t((l0.x + 2) / 4 * (std::int32_t(m.width) / 4) + (l0.y + 1) / 4) * ORE_WORDS] = 1; }, "overlaps the mother footprint");
    red("ore channel 4", [](MpmModel &m) { m.ore[4] = 1; }, "channel 4");
    red("no first-mine ore", [](MpmModel &m) { std::fill(m.ore.begin(), m.ore.end(), std::uint16_t(0)); }, "first-mine");
    // sealed base pocket (DESIGN sec. 14): a closed ring of ore blocks one block outside spot 0's footprint blocks leaves < MIN_ROOM16 buildable tiles
    red("base sealed by an ore ring", [&](MpmModel &m) {
            const std::int32_t B = std::int32_t(m.width) / 4;
            for (std::int32_t bc = l0.x / 4 - 1; bc <= (l0.x + 4) / 4 + 1; ++bc)
                for (std::int32_t br = l0.y / 4 - 1; br <= (l0.y + 2) / 4 + 1; ++br) {
                    const bool inner = bc >= l0.x / 4 && bc <= (l0.x + 4) / 4 && br >= l0.y / 4 && br <= (l0.y + 2) / 4;
                    if (inner) continue;
                    std::uint16_t *blk = &m.ore[std::size_t(((bc % B + B) % B) * B + ((br % B + B) % B)) * ORE_WORDS];
                    blk[0] = 150;
                    blk[1] = blk[2] = blk[3] = 100;
                } }, "build room");
    // ore reach (need_rich): every spot gets a good block 3-4 tiles away; the rich block is either near (green) or ~10 tiles away (red)
    {
        const std::int32_t W = std::int32_t(g.map.width), B = W / 4;
        auto               plant_ore = [&](MpmModel &m, std::int32_t rich_lo, std::int32_t rich_hi) {
            std::fill(m.ore.begin(), m.ore.end(), std::uint16_t(0));
            auto put_at = [&](const MpmLanding &l, std::int32_t lo, std::int32_t hi, std::array<std::uint16_t, 4> v) {
                for (std::int32_t bc = 0; bc < B; ++bc)
                    for (std::int32_t br = 0; br < B; ++br) {
                        std::int32_t dx = std::abs(l.x + 2 - (bc * 4 + 2)), dy = std::abs(l.y + 1 - (br * 4 + 2));
                        dx                       = std::min(dx, W - dx);
                        dy                       = std::min(dy, W - dy);
                        const std::int32_t d2    = dx * dx + dy * dy;
                        bool               clash = false;
                        for (const MpmLanding &o : m.landings) clash = clash || (4 * bc <= o.x + 4 && 4 * bc + 3 >= o.x && 4 * br <= o.y + 2 && 4 * br + 3 >= o.y);
                        std::uint16_t *blk = &m.ore[std::size_t(bc * B + br) * ORE_WORDS];
                        if (d2 < lo || d2 > hi || clash || blk[0]) continue;
                        for (int k = 0; k < 4; ++k) blk[k] = v[std::size_t(k)];
                        return;
                    }
            };
            for (const MpmLanding &l : m.landings) {
                put_at(l, 16, 30, {150, 100, 100, 100});
                put_at(l, rich_lo, rich_hi, {2000, 1000, 1000, 1000});
            }
        };
        MpmModel ok_map = g.map, far_map = g.map;
        plant_ore(ok_map, 31, 49);
        plant_ore(far_map, 90, 130);
        std::string w;
        if (!map_gates(ok_map, true, w, true)) {
            std::printf("FAIL ore reach: near rich block rejected: %s\n", w.c_str());
            ++g_fails;
        }
        w.clear();
        if (map_gates(far_map, true, w, true) || w.find("rich ore") == std::string::npos) {
            std::printf("FAIL ore reach: rich block at 10 tiles accepted or wrong reason '%s'\n", w.c_str());
            ++g_fails;
        }
        w.clear();
        if (!map_gates(far_map, true, w, false)) { // the reach gate is off when the rules have no rich records
            std::printf("FAIL ore reach: gate fired with need_rich=false: %s\n", w.c_str());
            ++g_fails;
        }
    }
    red("description > 79 bytes", [](MpmModel &m) { m.tail_desc.assign(80, 'x'); }, "description");
    red("tree slot order", [](MpmModel &m) { if (!m.trees.empty()) m.trees[0].slot = 7; else m.trees.push_back({7, 1, 0, 0, 0, 1, 1}); }, "tree slot");
    red("status not 1", [](MpmModel &m) { m.landings[1].status = 0; }, "status");
    // islands: --no-paths skips the connectivity gate only
    GenParams q    = p;
    q.ensure_paths = false;
    GenResult n    = generate(r, q);
    CHECK(n.ok);
    if (n.ok) CHECK(map_gates(n.map, false, why));
    // the generated description fits the loader's buffer
    CHECK(g.map.tail_desc.size() <= MAX_DESC_BYTES);
}

int retail_generator(const std::vector<Rules> &all) {
    std::size_t made = 0, bad = 0;
    for (const Rules &r : all) {
        for (std::uint32_t size : {64u, 128u, 256u})
            for (std::uint64_t seed : {1ull, 2ull}) {
                GenParams p;
                p.tileset     = r.tileset;
                p.size        = size;
                p.players     = size == 64 ? 2 : 4;
                p.seed        = seed;
                GenResult res = generate(r, p);
                if (!res.ok) {
                    std::printf("FAIL generate %s %u seed %llu: %s\n", r.tileset.c_str(), size, static_cast<unsigned long long>(seed), res.reason.c_str());
                    ++g_fails;
                    ++bad;
                    continue;
                }
                ++made;
                const int before = g_fails;
                check_generated(res.map, r, true, size, p.players);
                if (g_fails != before) {
                    std::printf("FAIL checks on %s %u seed %llu\n", r.tileset.c_str(), size, static_cast<unsigned long long>(seed));
                    ++bad;
                }
                std::printf("retail gen %-12s %3u seed %llu: attempts %u repairs %u resets %u ramps %u trees %u ore blocks %u\n", r.tileset.c_str(), size, static_cast<unsigned long long>(seed), res.stats.attempts, res.stats.repair_passes, res.stats.resets, res.stats.ramps, res.stats.trees, res.stats.ore_blocks);
            }
    }
    std::printf("retail generator: %zu maps generated, %zu bad\n", made, bad);
    return int(bad);
}

int retail(const std::string &mh_dir, const std::string &maps_dir) {
    std::vector<fs::path> files;
    auto                  gather = [&](const std::string &dir, const char *ext) {
        for (const auto &e : fs::directory_iterator(dir)) {
            std::string x = e.path().extension().string();
            for (char &c : x) c = char(std::tolower((unsigned char)c));
            if (e.is_regular_file() && x == ext) files.push_back(e.path());
        }
    };
    gather(mh_dir, ".mp");
    gather(maps_dir, ".mpm");
    std::sort(files.begin(), files.end());

    std::vector<MpmModel>    models(files.size());
    std::vector<std::string> shas(files.size());
    std::size_t              n_mp = 0, n_mpm = 0, roundtrip_ok = 0, id_ok = 0;
    std::set<std::string>    unique;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::vector<std::uint8_t> b;
        std::string               err;
        bool                      is_mpm = files[i].extension() == ".mpm";
        if (!read_file(files[i].string(), b) || !read_mpm(b.data(), b.size(), models[i], err)) {
            std::printf("FAIL read %s: %s\n", files[i].string().c_str(), err.c_str());
            ++g_fails;
            continue;
        }
        shas[i] = sha256_hex(b.data(), b.size());
        unique.insert(shas[i]);
        is_mpm ? ++n_mpm : ++n_mp;
        if (write_mpm(models[i]) == b) ++roundtrip_ok;
        else {
            std::printf("FAIL byte-exact: %s\n", files[i].string().c_str());
            ++g_fails;
        }
        CHECK(models[i].has_tail == is_mpm);
        if (is_mpm) {
            if (models[i].has_tail && models[i].tail_id == mpm_compute_id(models[i])) ++id_ok;
            else std::printf("note: .mpm id != CRC-32(body): %s (id %08x, crc %08x)\n", files[i].string().c_str(), models[i].tail_id, mpm_compute_id(models[i]));
        }
    }
    std::printf("retail: %zu .MP + %zu .mpm = %zu files, %zu unique; byte-exact read->write %zu/%zu\n", n_mp, n_mpm, files.size(), unique.size(), roundtrip_ok, files.size());
    std::printf("retail: .mpm id == CRC-32 of preceding bytes on %zu/%zu files\n", id_ok, n_mpm);
    CHECK(roundtrip_ok == files.size());
    CHECK(id_ok + 1 >= n_mpm); // Salt Lake.mpm ships with a stale id; it is stored, never recomputed

    std::vector<LearnInput> in, rev;
    for (std::size_t i = 0; i < files.size(); ++i) in.push_back({&models[i], shas[i]});
    rev.assign(in.rbegin(), in.rend());
    std::vector<Rules> a = learn_all(in), b = learn_all(rev);
    CHECK(a.size() == b.size());
    std::size_t illegal_total = 0, checked = 0, wrap_illegal = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        std::string ta = rules_to_text(a[i]), tb = rules_to_text(b[i]);
        CHECK(ta == tb);
        Rules       rt;
        std::string err;
        CHECK(rules_from_text(ta, rt, err) && rules_to_text(rt) == ta);
        std::printf("rules %-14s maps %2u tiles %4zu h-pairs %6zu v-pairs %6zu kinds %2zu text %zu bytes\n", a[i].tileset.c_str(), a[i].maps, a[i].tiles.size(), a[i].h.size(), a[i].v.size(), a[i].kinds.size(), ta.size());
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        for (const Rules &r : a)
            if (r.tileset == models[i].tileset()) {
                Report rep = check_map(models[i], r, false);
                illegal_total += rep.illegal_pairs.size() + rep.unknown_tiles;
                checked += std::size_t(rep.pairs_checked);
                wrap_illegal += check_map(models[i], r, true).illegal_pairs.size() - rep.illegal_pairs.size();
            }
    }
    std::printf("retail: %zu pairs checked against own rules, %zu illegal (non-wrap); %zu additional seam pairs illegal with --wrap\n", checked, illegal_total, wrap_illegal);
    CHECK(illegal_total == 0);
    retail_generator(a);
    return g_fails;
}

#define PREVIEW_GOLDEN_MARKERS 0x0411829bu
#define PREVIEW_RETAIL_GOLDENS {"Blue Monday.mpm", 0x9ae2c204u}, {"Cold War.mpm", 0x38674f6du}, {"White Peace.mpm", 0xb2fa4808u}, {"gen_jungle_128_1.mpm", 0x10745797u}, {"gen_diuna256_256_1.mpm", 0xd6a5e08eu}, {"gen_skaly256_128_2.mpm", 0xa908595fu}
// ---- mods:MAPGEN2: the preview renderer ------------------------------------------------------------------------
// Colour of synthetic tile t: a distinct, easily predicted RGB565 value.
std::uint16_t synth_col(int t) { return std::uint16_t(((t * 5 % 32) << 11) | ((t * 9 % 64) << 5) | (t * 3 % 32)); }

std::vector<std::uint16_t> synth_table(std::size_t n) {
    std::vector<std::uint16_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = synth_col(int(i));
    return v;
}

std::uint32_t crc_of(const std::vector<std::uint16_t> &px) {
    std::vector<std::uint8_t> b(px.size() * 2);
    for (std::size_t i = 0; i < px.size(); ++i) {
        b[i * 2]     = std::uint8_t(px[i] & 255);
        b[i * 2 + 1] = std::uint8_t(px[i] >> 8);
    }
    return crc32_ieee(b.data(), b.size());
}

void test_preview() {
    // tlo_tile_means: palette entry i -> a chosen colour; tile 0 solid index 5, tile 1 half 0 / half 1; a stray tail must not count
    std::vector<std::uint8_t> tlo(0x200 + 2 * 1024 + 100, 0);
    auto                      pal = [&](int i, int r, int g, int b) {
        const std::uint16_t v = std::uint16_t((r << 11) | (g << 5) | b);
        tlo[i * 2]            = std::uint8_t(v & 255);
        tlo[i * 2 + 1]        = std::uint8_t(v >> 8);
    };
    pal(0, 31, 63, 0);
    pal(1, 1, 1, 31);
    pal(5, 7, 21, 13);
    for (int i = 0; i < 1024; ++i) tlo[0x200 + i] = 5;
    for (int i = 0; i < 1024; ++i) tlo[0x200 + 1024 + i] = (i < 512) ? 0 : 1;
    std::vector<std::uint16_t> means;
    CHECK(tlo_tile_means(tlo.data(), tlo.size(), means));
    CHECK(means.size() == 2);
    if (means.size() == 2) {
        CHECK(means[0] == std::uint16_t((7 << 11) | (21 << 5) | 13));
        CHECK(means[1] == std::uint16_t((16 << 11) | (32 << 5) | 15)); // (31+1)/2, (63+1)/2, (0+31)/2 floored
    }
    CHECK(!tlo_tile_means(tlo.data(), 0x200 + 1023, means) && means.empty());
    CHECK(!tlo_tile_means(nullptr, 5000, means));

    const std::vector<std::uint16_t> tab = synth_table(64);
    std::vector<std::uint16_t>       out(128 * 128, 0xABCD);
    PreviewStats                     st;

    // 128 map -> 1:1
    MpmModel m128 = make_synth(128, 128, [](int c, int r) { return (c * 7 + r * 3) % 40; });
    CHECK(render_preview(m128, tab.data(), tab.size(), 128, out.data(), &st));
    CHECK(st.w == 128 && st.h == 128 && st.spots == 0 && st.ore_blocks == 0);
    bool ok = true;
    for (int r = 0; r < 128; ++r)
        for (int c = 0; c < 128; ++c) ok = ok && out[std::size_t(r) * 128 + c] == synth_col((c * 7 + r * 3) % 40);
    CHECK(ok);

    // 64 map -> 1:2 (each tile a 2x2 block), 256 map -> 2:1 (each pixel the mean of 2x2 tiles)
    MpmModel m64 = make_synth(64, 64, [](int c, int r) { return (c + r * 5) % 50; });
    CHECK(render_preview(m64, tab.data(), tab.size(), 128, out.data()));
    ok = true;
    for (int r = 0; r < 128; ++r)
        for (int c = 0; c < 128; ++c) ok = ok && out[std::size_t(r) * 128 + c] == synth_col((c / 2 + (r / 2) * 5) % 50);
    CHECK(ok);
    MpmModel m256 = make_synth(256, 256, [](int c, int r) { return ((c ^ r) & 1) ? 3 : 4; }); // a checkerboard
    CHECK(render_preview(m256, tab.data(), tab.size(), 128, out.data()));
    {
        const std::uint16_t a = synth_col(3), b = synth_col(4);
        const unsigned      r = (((a >> 11) & 31u) + ((b >> 11) & 31u) + 1) / 2, g = ((((a >> 5) & 63u) + ((b >> 5) & 63u)) + 1) / 2, bl = ((a & 31u) + (b & 31u) + 1) / 2;
        const std::uint16_t want = std::uint16_t((r << 11) | (g << 5) | bl);
        ok                       = true;
        for (std::uint16_t v : out) ok = ok && v == want;
        CHECK(ok);
    }

    // markers: a spot at (20,30) of a flat 128 map, a tree on (50,51), an ore block (bc 10, br 5)
    MpmModel mk = make_synth(128, 128, [](int, int) { return 10; });
    mk.landings.push_back({20, 30, 0});
    mk.landings.push_back({500, 3, 0}); // outside the map: not drawn, not counted
    MpmTree t;
    t.x = 50;
    t.y = 51;
    mk.trees.push_back(t);
    mk.ore[(std::size_t(10) * 32 + 5) * ORE_WORDS + 2] = 7;
    CHECK(render_preview(mk, tab.data(), tab.size(), 128, out.data(), &st));
    CHECK(st.spots == 1 && st.ore_blocks == 1 && st.trees == 1);
    CHECK(out[30 * 128 + 20] == PREVIEW_SPOT_IN && out[(30 - 3) * 128 + 20] == PREVIEW_SPOT_OUT && out[30 * 128 + 17] == PREVIEW_SPOT_OUT &&
          out[(30 + 2) * 128 + 22] == PREVIEW_SPOT_IN && out[30 * 128 + 24] == synth_col(10));
    {
        const std::uint16_t c = synth_col(10);
        const unsigned      r = ((c >> 11) & 31u) * 3 / 5, g = ((c >> 5) & 63u) * 3 / 5, b = (c & 31u) * 3 / 5;
        CHECK(out[51 * 128 + 50] == std::uint16_t((r << 11) | (g << 5) | b));
    }
    CHECK(out[(5 * 4 + 1) * 128 + 10 * 4 + 1] == PREVIEW_ORE && out[(5 * 4) * 128 + 10 * 4] == synth_col(10)); // 2x2 gold in the 4x4 block's middle
    const std::uint32_t golden_markers = crc_of(out);
    CHECK(render_preview(mk, tab.data(), tab.size(), 128, out.data())); // deterministic
    CHECK(crc_of(out) == golden_markers);
    std::printf("preview: synthetic markers crc %08x\n", golden_markers);
    CHECK(golden_markers == PREVIEW_GOLDEN_MARKERS);

    // a tile id past the table draws the flat unknown colour; so does a missing table
    MpmModel mu = make_synth(64, 64, [](int, int) { return 60; });
    CHECK(render_preview(mu, tab.data(), 10, 128, out.data()));
    CHECK(out[0] == PREVIEW_UNKNOWN && out[128 * 128 - 1] == PREVIEW_UNKNOWN);
    CHECK(render_preview(mu, nullptr, 0, 128, out.data()) && out[777] == PREVIEW_UNKNOWN);

    // refusals leave the buffer alone: planes that do not match width x height (a truncated read), no map, a silly size
    std::fill(out.begin(), out.end(), std::uint16_t(0x1234));
    MpmModel bad = make_synth(64, 64, [](int, int) { return 1; });
    bad.flags.resize(100);
    CHECK(!render_preview(bad, tab.data(), tab.size(), 128, out.data()) && out[5] == 0x1234);
    MpmModel empty;
    CHECK(!render_preview(empty, tab.data(), tab.size(), 128, out.data()));
    CHECK(!render_preview(m64, tab.data(), tab.size(), 4, out.data()) && !render_preview(m64, tab.data(), tab.size(), 128, nullptr));
    // hostile ore/landing/tree data never reads out of range: short ore plane, huge and negative coordinates
    MpmModel hostile = make_synth(64, 64, [](int, int) { return 2; });
    hostile.ore.assign(3, 0xFFFF);
    hostile.landings.push_back({-5, -5, 0});
    hostile.landings.push_back({0x7fffffff, 0x7fffffff, 0});
    MpmTree far;
    far.x = -1;
    far.y = 99999;
    hostile.trees.push_back(far);
    CHECK(render_preview(hostile, tab.data(), tab.size(), 128, out.data(), &st) && st.spots == 0 && st.ore_blocks == 0 && st.trees == 0);
    // garbage and truncated FILES never reach the renderer: read_mpm refuses them
    MpmModel                  g;
    std::string               err;
    std::vector<std::uint8_t> junk(300, 0x5a);
    CHECK(!read_mpm(junk.data(), junk.size(), g, err));
    std::vector<std::uint8_t> good = write_mpm(m64);
    CHECK(!good.empty() && !read_mpm(good.data(), good.size() / 2, g, err));
    // a non-square map keeps its aspect, centred on black
    MpmModel wide = make_synth(128, 64, [](int, int) { return 12; });
    CHECK(render_preview(wide, tab.data(), tab.size(), 128, out.data()));
    CHECK(out[0] == 0 && out[31 * 128 + 5] == 0 && out[32 * 128 + 5] == synth_col(12) && out[127 * 128 + 127] == 0 && out[95 * 128 + 127] == synth_col(12));
    // another output size
    std::vector<std::uint16_t> o64(64 * 64, 0);
    CHECK(render_preview(m128, tab.data(), tab.size(), 64, o64.data()));
}

// `mapgen_test --preview <tlo_dir> <maps_dir>`: the real data. Goldens are the CRC-32 of the 128 px RGB565 output.
int retail_preview(const std::string &tlo_dir, const std::vector<std::string> &map_dirs) {
    struct G {
        const char   *map;
        std::uint32_t crc;
    };
    const G goldens[] = {PREVIEW_RETAIL_GOLDENS};
    int     checked   = 0;
    for (const G &g : goldens) {
        std::vector<std::uint8_t> b;
        MpmModel                  m;
        std::string               err;
        bool                      found = false;
        for (const std::string &d : map_dirs) found = found || (read_file(d + "/" + g.map, b) && read_mpm(b.data(), b.size(), m, err));
        if (!found) {
            std::printf("preview: skip %s (not present)\n", g.map);
            continue;
        }
        std::vector<std::uint8_t> tlo;
        std::string               tn = m.tileset();
        for (char &c : tn) c = char(std::tolower((unsigned char)c));
        if (!read_file(tlo_dir + "/" + tn, tlo)) {
            std::printf("FAIL preview: no tileset %s\n", tn.c_str());
            ++g_fails;
            continue;
        }
        std::vector<std::uint16_t> tab, out(128 * 128);
        CHECK(tlo_tile_means(tlo.data(), tlo.size(), tab));
        CHECK(render_preview(m, tab.data(), tab.size(), 128, out.data()));
        const std::uint32_t crc = crc_of(out);
        std::printf("preview: %-28s crc %08x %s\n", g.map, crc, crc == g.crc ? "ok" : "MISMATCH");
        CHECK(crc == g.crc);
        ++checked;
    }
    CHECK(checked > 0);
    return g_fails;
}

} // namespace

int main(int argc, char **argv) {
    test_hashes();
    test_rng();
    test_writer_offsets();
    test_mpm_id();
    test_rules();
    test_rules_safety_and_calibration();
    test_pixel_inference();
    test_inferred_composite();
    test_checker();
    test_generator();
    test_gates();
    test_preview();
    std::printf("synthetic: %s\n", g_fails ? "FAILED" : "ok");
    if (argc >= 2 && std::strcmp(argv[1], "--retail") == 0) {
        if (argc != 4) {
            std::printf("usage: mapgen_test --retail <mh_dir> <maps_dir>\n");
            return 2;
        }
        retail(argv[2], argv[3]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--preview") == 0) {
        if (argc < 4) {
            std::printf("usage: mapgen_test --preview <tlo_dir> <maps_dir>...\n");
            return 2;
        }
        retail_preview(argv[2], std::vector<std::string>(argv + 3, argv + argc));
    }
    std::printf("%s (%d failures)\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
