//
// mods_save_selftest.cpp -- `net_selftest modsavetest`: DATAEXT1's outer .sav mod-metadata carrier
// (mh/mods/save_meta.*). Pure, over the same fake-file seam ovftest uses.
//
// WHAT IT PROVES (behavior only, no wiring):
//   * the WRITE side appends nothing for a fresh no-mod game, appends the mounted set for a modded
//     one, keeps the LOADED origin across a resave under another (or empty) mounted set, and keeps
//     an UNKNOWN provenance uncertain (no relabeling, no fake fallback);
//   * a session reset returns the provenance to the mounted set and kills a staged probe;
//   * the READ side validates footer bounds BEFORE reading into the scratch buffer (a fake file
//     asserts the read destination never exceeds the carrier bound), restores the file position on
//     EVERY path, stages MATCH / MISMATCH / LEGACY_UNCERTAIN / NONE correctly, rejects every
//     malformed or grammar-violating carrier as absent, and never lets a mod footer weaken
//     anything (an alien trailing footer in front keeps parsing untouched);
//   * legal boundary metadata (a 32-byte id, a 64-byte version, 64 entries) round-trips losslessly,
//     including through the mismatch log line with a bounded formatter;
//   * a torn tail (a short write before the footer) reads as "no carrier"; a null source is stock.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "mods/save_meta.h"

namespace {

using mh::mods::save::active_set_source;
using mh::mods::save::append_origin;
using mh::mods::save::commit_staged;
using mh::mods::save::file_io;
using mh::mods::save::MAX_BLOB_BYTES;
using mh::mods::save::MAX_ID_BYTES;
using mh::mods::save::MAX_VER_BYTES;
using mh::mods::save::MIN_BLOB_BYTES;
using mh::mods::save::on_new_game;
using mh::mods::save::probe_container_io;
using mh::mods::save::reset_for_test;
using mh::mods::save::set_active_set_source;
using mh::mods::save::verdict;

int  g_checks = 0, g_fails = 0;
void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("modsavetest FAIL: %s\n", what);
    }
}

// ---- a fake FILE (ovftest's shape): a byte vector with a position, counters and fault injectors -
struct fake_file {
    std::vector<uint8_t> d;
    int32_t              pos    = 0;
    int                  writes = 0, seeks = 0, reads = 0;
    bool                 seek_fail = false, write_fail = false;
};
int32_t ff_tell(void *f) { return static_cast<fake_file *>(f)->pos; }
int     ff_seek(void *f, int32_t off, int whence) {
    auto *x = static_cast<fake_file *>(f);
    ++x->seeks;
    if (x->seek_fail) return -1;
    const int64_t np = whence == 0 ? off : whence == 1 ? x->pos + (int64_t)off
                                                           : (int64_t)x->d.size() + off;
    if (np < 0 || np > INT32_MAX) return -1;
    x->pos = (int32_t)np;
    return 0;
}
uint32_t g_max_read = 0; // the largest single read the module ever requested: must stay <= the scratch bound
uint32_t ff_read(void *f, void *dst, uint32_t n) {
    auto *x = static_cast<fake_file *>(f);
    ++x->reads;
    if (n > g_max_read) g_max_read = n;
    const uint32_t have = (size_t)x->pos < x->d.size() ? (uint32_t)(x->d.size() - (size_t)x->pos) : 0;
    const uint32_t k    = n < have ? n : have;
    if (k) memcpy(dst, x->d.data() + x->pos, k);
    x->pos += (int32_t)k;
    return k;
}
uint32_t ff_write(void *f, const void *src, uint32_t n) {
    auto *x = static_cast<fake_file *>(f);
    ++x->writes;
    if (x->write_fail) return 0;
    if (x->pos + (int64_t)n > (int64_t)x->d.size()) x->d.resize((size_t)x->pos + n);
    memcpy(x->d.data() + x->pos, src, n);
    x->pos += (int32_t)n;
    return n;
}
const file_io FIO = {&ff_tell, &ff_seek, &ff_read, &ff_write};

// A container-shaped body: the 40-byte version header, some blocks, a "media block" tail.
void make_container(fake_file &f, size_t blocks) {
    f.d.assign(40 + blocks * 16, 0);
    memcpy(f.d.data(), " 'Mission: Humanity' ver 1.00", 29);
    for (size_t i = 40; i < f.d.size(); ++i) f.d[i] = (uint8_t)(i * 7u + 3u);
    f.pos = (int32_t)f.d.size(); // the writer sits at EOF after the media block
}

// ---- a fixture mounted set -----------------------------------------------------------------------------------
struct fixture_set {
    std::vector<std::string> ids, vers;
    uint8_t                  digest[32] = {};
};
fixture_set *g_cur           = nullptr; // the active fixture (the seam's ctx is unused by the fixture)
bool         g_null_snapshot = false;   // the production adapter's null (no-mod boot) arm
unsigned     fx_count(void *) { return g_null_snapshot ? 0u : (unsigned)g_cur->ids.size(); }
bool         fx_str(void *, unsigned i, char *out, unsigned cap, bool id) {
    if (g_null_snapshot || i >= g_cur->ids.size()) return false;
    const auto &s = id ? g_cur->ids[i] : g_cur->vers[i];
    if (s.size() + 1 > cap) return false;
    memcpy(out, s.c_str(), s.size() + 1);
    return true;
}
bool fx_id(void *c, unsigned i, char *out, unsigned cap) { return fx_str(c, i, out, cap, true); }
bool fx_ver(void *c, unsigned i, char *out, unsigned cap) { return fx_str(c, i, out, cap, false); }
void fx_digest(void *, unsigned char out[32]) {
    if (g_null_snapshot) memset(out, 0, 32);
    else memcpy(out, g_cur->digest, 32);
}

fixture_set make_set(char digest_fill) {
    fixture_set s;
    memset(s.digest, (uint8_t)digest_fill, 32);
    return s;
}
void use_source(fixture_set &s) {
    static const active_set_source src = {nullptr, &fx_count, &fx_id, &fx_ver, &fx_digest};
    g_cur                              = &s;
    g_null_snapshot                    = false;
    set_active_set_source(&src);
}
void use_no_source() { // what the production adapter reports with a null snapshot: stock, count 0
    g_null_snapshot = true;
    set_active_set_source(nullptr); // the seam null itself must also be safe
}

// Hand-built carrier blob (the reader's contract, byte for byte): [ver][flags][u16 count][digest]
// then per entry [id_len][ver_len][id][version].
std::vector<uint8_t> build_blob(uint8_t version, uint16_t count, const uint8_t *digest, uint8_t flags,
                                const std::vector<std::pair<std::string, std::string>> &entries,
                                bool exact = true, size_t slack = 0) {
    std::vector<uint8_t> b;
    b.push_back(version);
    b.push_back(flags);
    b.push_back((uint8_t)(count & 0xff));
    b.push_back((uint8_t)(count >> 8));
    b.insert(b.end(), digest, digest + 32);
    for (const auto &e : entries) {
        b.push_back((uint8_t)e.first.size());
        b.push_back((uint8_t)e.second.size());
        b.insert(b.end(), e.first.begin(), e.first.end());
        b.insert(b.end(), e.second.begin(), e.second.end());
    }
    if (!exact) b.resize(b.size() + slack, 0xEE); // trailing bytes a v1 writer never emits
    return b;
}
void append_footer(fake_file &f, const std::vector<uint8_t> &blob, uint32_t magic_le, uint32_t declared_len = 0xFFFFFFFFu) {
    const uint32_t n = declared_len == 0xFFFFFFFFu ? (uint32_t)blob.size() : declared_len;
    f.d.insert(f.d.end(), blob.begin(), blob.end());
    const uint8_t foot[8] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24),
                             (uint8_t)magic_le, (uint8_t)(magic_le >> 8), (uint8_t)(magic_le >> 16), (uint8_t)(magic_le >> 24)};
    f.d.insert(f.d.end(), foot, foot + 8);
}

const uint32_t   MHM1       = mh::mods::save::FOOTER_MAGIC;
const uint32_t   MHOF       = 0x464F484Du; // the caps planet-trailer magic, for the coexistence arm
constexpr size_t BLOB_FIXED = 4 + 32;      // version + flags + count + digest

std::vector<std::pair<std::string, std::string>> entries_of(const fixture_set &s) {
    std::vector<std::pair<std::string, std::string>> e;
    for (size_t i = 0; i < s.ids.size(); ++i) e.push_back({s.ids[i], s.vers[i]});
    return e;
}

} // namespace

int run_modsavetest() {
    printf("modsavetest -- the outer .sav mod-metadata carrier (DATAEXT1)\n");
    reset_for_test();

    // ---- (1) the WRITE side ----------------------------------------------------------------------------------
    printf("modsavetest (1) -- append_origin\n");
    {
        // (1a) fresh no-mod game: no byte, no call
        fixture_set none = make_set('0');
        use_source(none);
        fake_file f;
        make_container(f, 4);
        const std::vector<uint8_t> before = f.d;
        ck(append_origin(FIO, &f) == 0 && f.d == before && f.writes == 0,
           "(1a) BOOT, no mods: 0 bytes, no write (an ordinary save stays byte-vanilla)");

        // (1a') the null-snapshot arm (an ordinary no-mod boot): same stock behavior, no crash
        use_no_source();
        fake_file f0;
        make_container(f0, 4);
        const std::vector<uint8_t> before0 = f0.d;
        ck(append_origin(FIO, &f0) == 0 && f0.d == before0, "(1a') a null snapshot is stock: 0 bytes, no write");

        // (1b) boot with mods: [blob][len]['MHM1'] exactly, the last 8 bytes are the footer
        fixture_set ab = make_set('1');
        ab.ids         = {"alpha", "beta"};
        ab.vers        = {"1", "2"};
        use_source(ab);
        fake_file g;
        make_container(g, 4);
        const size_t body = g.d.size();
        const int    w    = append_origin(FIO, &g);
        ck(w == (int)(BLOB_FIXED + (2 + 5 + 1) + (2 + 4 + 1) + 8) && g.d.size() == body + (size_t)w,
           "(1b) BOOT {alpha@1,beta@2}: the carrier is appended, sized exactly");
        ck(g.d[body] == 1 && g.d[body + 1] == 1 && g.d[body + 2] == 2 && g.d[body + 3] == 0,
           "(1b) v1 blob: version, known flag, count 2 LE");
        ck(memcmp(&g.d[body + 4], ab.digest, 32) == 0, "(1b) the mounted digest is carried verbatim");
        uint32_t magic = 0, len = 0;
        memcpy(&magic, &g.d[g.d.size() - 4], 4);
        memcpy(&len, &g.d[g.d.size() - 8], 4);
        ck(magic == MHM1 && len + 8 == (uint32_t)w && body + len + 8 == g.d.size(),
           "(1b) the LAST 8 bytes are {blob_len, 'MHM1'}");
        const size_t e0 = body + BLOB_FIXED;
        ck(g.d[e0] == 5 && g.d[e0 + 1] == 1 && memcmp(&g.d[e0 + 2], "alpha", 5) == 0 && g.d[e0 + 7] == '1',
           "(1b) entry 0 = alpha@1");
        ck(g.d[e0 + 8] == 4 && g.d[e0 + 9] == 1 && memcmp(&g.d[e0 + 10], "beta", 4) == 0 && g.d[e0 + 14] == '2',
           "(1b) entry 1 = beta@2, order preserved");

        // (1c) the same file re-reads as its own origin: probe -> commit -> resave keeps the set
        ck(probe_container_io(FIO, &g) == verdict::MATCH, "(1c) the carrier just written matches the mounted set");
        ck(commit_staged() == verdict::MATCH, "(1c) committed");
        make_container(g, 4);
        ck(append_origin(FIO, &g) > 0, "(1c) a resave after the commit still carries a carrier");
    }

    // (2) the LOADED origin beats the mounted set (saveA loaded under boot B must not relabel)
    printf("modsavetest (2) -- loaded provenance survives a resave\n");
    {
        fixture_set boot = make_set('B');
        boot.ids         = {"hostmod"};
        boot.vers        = {"9"};
        use_source(boot);

        fake_file s;
        make_container(s, 2);
        append_footer(s, build_blob(1, 1, make_set('A').digest, 1, {{"savemod", "3"}}), MHM1);
        ck(probe_container_io(FIO, &s) == verdict::MISMATCH, "(2) a different carrier mismatches the mounted set");
        ck(commit_staged() == verdict::MISMATCH, "(2) the mismatch warning is queued exactly once");

        fake_file r;
        make_container(r, 2);
        const size_t body = r.d.size();
        const int    w    = append_origin(FIO, &r);
        ck(w == (int)(BLOB_FIXED + (2 + 7 + 1) + 8), "(2) the resave appends exactly the origin carrier");
        ck(r.d[body + 4] == 'A', "(2) the digest is the LOADED origin's, not the mounted set's");
        ck(r.d[body + 36] == 7 && memcmp(&r.d[body + 38], "savemod", 7) == 0 && r.d[body + 45] == '3',
           "(2) the resaved carrier names savemod@3");

        fixture_set none = make_set('0');
        use_source(none);
        fake_file r2;
        make_container(r2, 2);
        ck(append_origin(FIO, &r2) > 0, "(2) an empty mounted set does not erase the loaded origin");
        ck(r2.d[r2.d.size() - 4] == (uint8_t)(MHM1 & 0xff), "(2) ...and it is a carrier, not silence");
    }

    // (3) UNKNOWN stays unknown (a legacy save resaved under mods is NOT relabeled)
    printf("modsavetest (3) -- unknown provenance\n");
    {
        fixture_set boot = make_set('7');
        boot.ids         = {"m"};
        boot.vers        = {"1"};
        use_source(boot);
        fake_file legacy;
        make_container(legacy, 3);
        ck(probe_container_io(FIO, &legacy) == verdict::LEGACY_UNCERTAIN, "(3) no carrier with mods active: uncertain");
        ck(commit_staged() == verdict::LEGACY_UNCERTAIN, "(3) the legacy warning is queued");
        fake_file r;
        make_container(r, 3);
        const std::vector<uint8_t> before = r.d;
        ck(append_origin(FIO, &r) == 0 && r.d == before,
           "(3) the resave appends NOTHING: absence is the unknown marker, the next load warns again");
    }

    // (4) a session rebuild returns to the mounted set (the next New Game is boot-derived)
    printf("modsavetest (4) -- session reset\n");
    {
        fixture_set boot = make_set('4');
        boot.ids         = {"fresh"};
        boot.vers        = {"1"};
        use_source(boot);
        on_new_game(true);
        fake_file r;
        make_container(r, 2);
        ck(append_origin(FIO, &r) > 0, "(4) after a session reset the mounted set is recorded again");
        fake_file s;
        make_container(s, 2);
        ck(probe_container_io(FIO, &s) == verdict::LEGACY_UNCERTAIN, "(4) a probe stages");
        on_new_game(true);
        ck(commit_staged() == verdict::NONE, "(4) the reset cleared the staged probe");
    }

    // ---- (5) the READ side -----------------------------------------------------------------------------------
    printf("modsavetest (5) -- probe_container_io\n");
    {
        fixture_set boot = make_set('5');
        boot.ids         = {"alpha", "beta"};
        boot.vers        = {"1", "2"};
        use_source(boot);

        // (5a) match / mismatch matrix over a real carrier
        {
            fake_file f;
            make_container(f, 2);
            append_footer(f, build_blob(1, 2, boot.digest, 1, entries_of(boot)), MHM1);
            f.pos = 41; // where the 40-byte header read leaves the file
            ck(probe_container_io(FIO, &f) == verdict::MATCH && f.pos == 41 && f.writes == 0,
               "(5a) identical set: MATCH, position and content untouched");
        }
        {
            fake_file f;
            make_container(f, 2);
            append_footer(f, build_blob(1, 2, boot.digest, 1, {{"beta", "2"}, {"alpha", "1"}}), MHM1); // order swapped
            ck(probe_container_io(FIO, &f) == verdict::MISMATCH, "(5a) reordered entries: MISMATCH");
        }
        {
            fake_file f;
            make_container(f, 2);
            append_footer(f, build_blob(1, 2, boot.digest, 1, {{"alpha", "1"}, {"beta", "3"}}), MHM1); // version bumped
            ck(probe_container_io(FIO, &f) == verdict::MISMATCH, "(5a) a version bump: MISMATCH");
        }
        {
            fake_file f;
            make_container(f, 2);
            append_footer(f, build_blob(1, 2, make_set('D').digest, 1, {{"alpha", "1"}, {"beta", "2"}}), MHM1); // bytes changed
            ck(probe_container_io(FIO, &f) == verdict::MISMATCH, "(5a) a different digest: MISMATCH");
        }
        {
            fake_file f;
            make_container(f, 2);
            fixture_set none = make_set('0');
            use_source(none);
            append_footer(f, build_blob(1, 1, boot.digest, 1, {{"alpha", "1"}}), MHM1);
            ck(probe_container_io(FIO, &f) == verdict::MISMATCH, "(5a) a modded save with nothing mounted: MISMATCH");
            use_source(boot);
        }
        {
            fake_file f;
            make_container(f, 2);
            fixture_set none = make_set('0');
            use_source(none);
            ck(probe_container_io(FIO, &f) == verdict::NONE, "(5a) no carrier, no mods: silent");
            use_source(boot);
        }

        // (5b) legal boundary metadata round-trips losslessly: 32-byte id + 64-byte version + 64 entries
        {
            fixture_set big = make_set('8');
            for (int i = 0; i < 64; ++i) {
                big.ids.push_back("m" + std::to_string(i));
                big.vers.push_back("v");
            }
            big.ids[0]  = std::string(MAX_ID_BYTES, 'x');  // a legal maximum id
            big.vers[0] = std::string(MAX_VER_BYTES, 'V'); // a legal maximum version
            use_source(big);
            fake_file f;
            make_container(f, 2);
            append_footer(f, build_blob(1, 64, big.digest, 1, entries_of(big)), MHM1);
            f.pos = 41;
            ck(probe_container_io(FIO, &f) == verdict::MATCH && f.pos == 41,
               "(5b) 64 entries incl. 32-byte id and 64-byte version: lossless MATCH");
            // the same metadata mismatching a 1-entry mounted set: the log line stays bounded, no crash
            fixture_set one = make_set('9');
            one.ids = {"a"}, one.vers = {"1"};
            use_source(one);
            fake_file g;
            make_container(g, 2);
            append_footer(g, build_blob(1, 64, big.digest, 1, entries_of(big)), MHM1);
            ck(probe_container_io(FIO, &g) == verdict::MISMATCH, "(5b) the long-metadata mismatch still decides MISMATCH");
            ck(commit_staged() == verdict::MISMATCH, "(5b) ...and keeps valid provenance (64 entries staged)");
            use_source(boot);
        }

        // (5c) malformed / grammar-violating carriers: treated as absent, never fatal, position restored.
        // The fake file records the largest requested read: it must never exceed the scratch bound.
        const char *names[] = {"bad magic", "len 0", "huge declared len", "len over bound", "len under minimum",
                               "version 2", "unknown flag bits", "count 300", "id_len 0", "id_len 33",
                               "ver_len 65", "id uppercase", "id other byte", "id embedded NUL", "duplicate ids",
                               "version control char", "version embedded NUL", "version overlong utf8",
                               "truncated entry", "trailing bytes", "blob over the header"};
        for (int k = 0; k < 21; ++k) {
            fake_file f;
            make_container(f, 2);
            const uint8_t dig[32] = {};
            const auto    one     = {std::make_pair(std::string("alpha"), std::string("1"))};
            switch (k) {
                case 0: append_footer(f, build_blob(1, 1, dig, 1, one), 0x5A5A5A5A); break;
                case 1: append_footer(f, {}, MHM1, 0); break;
                case 2: append_footer(f, build_blob(1, 1, dig, 1, one), MHM1, 0xFFFFFFFEu); break; // huge declared len
                case 3: {
                    std::vector<uint8_t> big(MAX_BLOB_BYTES + 1, 0x41);
                    append_footer(f, big, MHM1);
                    break;
                }
                case 4: append_footer(f, std::vector<uint8_t>(8, 0x41), MHM1, MIN_BLOB_BYTES - 1); break;
                case 5: append_footer(f, build_blob(2, 1, dig, 1, one), MHM1); break;
                case 6: append_footer(f, build_blob(1, 1, dig, 0x03, one), MHM1); break; // unknown flag bits
                case 7: append_footer(f, build_blob(1, 300, dig, 1, {}), MHM1); break;
                case 8: append_footer(f, build_blob(1, 1, dig, 1, {{"", "1"}}), MHM1); break;
                case 9: {
                    std::vector<uint8_t> b = build_blob(1, 1, dig, 1, {});
                    b.resize(38);
                    b[36] = 33, b[37] = 1; // id_len over the bound
                    b.resize(38 + 33 + 1, 'x');
                    append_footer(f, b, MHM1);
                    break;
                }
                case 10: {
                    std::vector<uint8_t> b = build_blob(1, 1, dig, 1, {});
                    b.resize(38);
                    b[36] = 5, b[37] = 65; // ver_len over the frozen 64
                    b.resize(38 + 5 + 65, 'V');
                    append_footer(f, b, MHM1);
                    break;
                }
                case 11: append_footer(f, build_blob(1, 1, dig, 1, {{"Alpha", "1"}}), MHM1); break; // grammar: uppercase
                case 12: {
                    std::string bad = "al/pha"; // '/' is outside [a-z0-9_-]
                    append_footer(f, build_blob(1, 1, dig, 1, {{bad, "1"}}), MHM1);
                    break;
                }
                case 13: {
                    std::vector<uint8_t> b = build_blob(1, 1, dig, 1, {});
                    b.resize(44);
                    b[36] = 5, b[37] = 1;
                    b[38] = 'a', b[39] = 0, b[40] = 'l', b[41] = 'p', b[42] = 'h'; // embedded NUL at [a\0lph]
                    b.resize(43 + 1, '1');
                    append_footer(f, b, MHM1);
                    break;
                }
                case 14: append_footer(f, build_blob(1, 2, dig, 1, {{"alpha", "1"}, {"alpha", "2"}}), MHM1); break; // duplicate
                case 15: {
                    std::string ctrl = "v\x01"; // a C0 control inside the version
                    append_footer(f, build_blob(1, 1, dig, 1, {{"alpha", ctrl}}), MHM1);
                    break;
                }
                case 16: {
                    std::string          bad = std::string("v") + char(0) + "2"; // embedded NUL (strlen would truncate)
                    std::vector<uint8_t> b   = build_blob(1, 1, dig, 1, {});
                    b.resize(38);
                    b[36] = 5, b[37] = 3;
                    b.resize(38 + 5 + 3, 'x');
                    b[43] = 'v', b[44] = 0, b[45] = '2';
                    append_footer(f, b, MHM1);
                    break;
                }
                case 17: {
                    const std::string bad = std::string("v") + char(0xE0) + char(0x80) + char(0x80); // overlong 3-byte UTF-8
                    append_footer(f, build_blob(1, 1, dig, 1, {{"alpha", bad}}), MHM1);
                    break;
                }
                case 18: {
                    std::vector<uint8_t> b = build_blob(1, 1, dig, 1, {}); // announces 1 entry, no entry bytes
                    b.resize(b.size() + 4, 'e');
                    append_footer(f, b, MHM1);
                    break;
                }
                case 19: {
                    std::vector<uint8_t> b = build_blob(1, 0, dig, 1, {}, false, 3); // a v1 blob with slack
                    append_footer(f, b, MHM1);
                    break;
                }
                case 20: {
                    std::vector<uint8_t> b = build_blob(1, 0, dig, 1, {});
                    append_footer(f, b, MHM1, 40 + (uint32_t)b.size() + 100); // the "blob" reaches over the header
                    break;
                }
            }
            f.pos           = 41;
            const verdict v = probe_container_io(FIO, &f);
            ck(v == verdict::LEGACY_UNCERTAIN && f.pos == 41 && f.writes == 0, names[k]);
            ck(g_max_read <= MAX_BLOB_BYTES, names[k]);
        }

        // (5d) position restoration on every path (ovftest 5c's matrix)
        {
            fake_file f;
            make_container(f, 2);
            f.pos = 777;
            ck(probe_container_io(FIO, &f) == verdict::LEGACY_UNCERTAIN && f.pos == 777 && f.writes == 0,
               "(5d) vanilla container: uncertain, position restored");
            fake_file tiny;
            make_container(tiny, 0);
            tiny.d.resize(5);
            tiny.pos = 2;
            ck(probe_container_io(FIO, &tiny) == verdict::LEGACY_UNCERTAIN && tiny.pos == 2,
               "(5d) a 5-byte file: no footer, position restored");
            fake_file noseek;
            make_container(noseek, 2);
            noseek.seek_fail = true;
            noseek.pos       = 44;
            ck(probe_container_io(FIO, &noseek) == verdict::LEGACY_UNCERTAIN && noseek.pos == 44,
               "(5d) every seek failing: treated as absent, position untouched");
        }

        // (5e) a torn tail (a short write between the blob and the footer) reads as absent
        {
            fake_file f;
            make_container(f, 2);
            std::vector<uint8_t> blob = build_blob(1, 1, boot.digest, 1, {{"alpha", "1"}});
            f.d.insert(f.d.end(), blob.begin(), blob.end()); // the blob landed, the footer write failed
            f.pos = (int32_t)f.d.size();
            ck(probe_container_io(FIO, &f) == verdict::LEGACY_UNCERTAIN, "(5e) a blob without its footer: no carrier");
        }

        // (5f) coexistence: an alien trailing footer in front (the caps MHOV shape) is untouched,
        // and the mod carrier goes AFTER it
        {
            fake_file f;
            make_container(f, 2);
            const size_t alien_at = f.d.size();
            append_footer(f, std::vector<uint8_t>(16, 0x42), MHOF); // [blob]['MHOF']
            append_footer(f, build_blob(1, 2, boot.digest, 1, entries_of(boot)), MHM1);
            f.pos = 41;
            ck(probe_container_io(FIO, &f) == verdict::MATCH && f.pos == 41,
               "(5f) the LAST footer wins; the alien tail in front is untouched");
            ck(memcmp(&f.d[alien_at + 16 + 4], &MHOF, 4) == 0, "(5f) the alien footer bytes are still where they were");
            fake_file g = f;
            g.pos       = (int32_t)g.d.size();
            ck(append_origin(FIO, &g) > 0, "(5f) appending after an alien tail works");
            uint32_t m2 = 0;
            memcpy(&m2, &g.d[g.d.size() - 4], 4);
            ck(m2 == MHM1, "(5f) the new last 8 bytes are the mod footer");
        }
    }

    // (6) a failed load discards the stage; a later successful one replaces it
    printf("modsavetest (6) -- staging discipline\n");
    {
        fixture_set boot = make_set('6');
        boot.ids         = {"alpha"};
        boot.vers        = {"1"};
        use_source(boot);
        fake_file f;
        make_container(f, 2);
        ck(probe_container_io(FIO, &f) == verdict::LEGACY_UNCERTAIN, "(6) probe one stages");
        fake_file g;
        make_container(g, 2);
        append_footer(g, build_blob(1, 1, boot.digest, 1, {{"alpha", "1"}}), MHM1);
        ck(probe_container_io(FIO, &g) == verdict::MATCH, "(6) probe two replaces the stage");
        ck(commit_staged() == verdict::MATCH, "(6) only the LAST probe commits");
        ck(commit_staged() == verdict::NONE, "(6) the stage is single-shot (a second pump is silent)");
    }

    // Fresh-process continue is uncertain; a same-process load's origin must survive.
    {
        fixture_set active = make_set('C');
        active.ids         = {"active"};
        active.vers        = {"1"};
        reset_for_test();
        use_source(active);
        mh::mods::save::on_campaign_continue();
        fake_file out;
        make_container(out, 2);
        ck(append_origin(FIO, &out) == 0, "unestablished continue remains unknown");
        mh::mods::save::on_new_game(true);
        make_container(out, 2);
        ck(append_origin(FIO, &out) > 0, "confirmed boot restore establishes origin");
        mh::mods::save::on_campaign_continue();
        make_container(out, 2);
        ck(append_origin(FIO, &out) > 0, "same-process continue preserves origin");
        mh::mods::save::on_new_game(false);
        make_container(out, 2);
        ck(append_origin(FIO, &out) == 0, "incomplete boot restore remains unknown");
        fake_file malformed;
        make_container(malformed, 2);
        append_footer(malformed, build_blob(1, 1, active.digest, 1, {{"alpha", std::string("v") + char(0xc2) + char(0x80)}}), MHM1);
        malformed.pos = 40;
        ck(probe_container_io(FIO, &malformed) == verdict::LEGACY_UNCERTAIN && malformed.pos == 40, "C1 controls rejected and position restored");
        fake_file too_many;
        make_container(too_many, 2);
        append_footer(too_many, build_blob(1, 65, active.digest, 1, {}), MHM1);
        too_many.pos = 40;
        ck(probe_container_io(FIO, &too_many) == verdict::LEGACY_UNCERTAIN && too_many.pos == 40, "65 entries rejected at catalog limit");
    }

    reset_for_test();
    printf("modsavetest: %d checks\n", g_checks);
    return g_fails;
}
