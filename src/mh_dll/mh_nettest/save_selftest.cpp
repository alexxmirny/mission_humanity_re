//
// save_selftest.cpp -- `net_selftest.exe savetest`: the save file's VERSION GATE over plain buffers,
// with no file, no LZW and no game.
//
// WHY BUFFERS AND NOT A SHADOW SITE. docs/save-format.md's shadowability section: every function in
// this subsystem touches a live file handle, and a file write cannot be rolled back between two arms,
// so shadowing one would write the file twice or desync the read cursor. SV1's own done_when
// therefore asks for the format logic to be "unit-tested over BUFFERS with no file and no game" --
// this is that test. The version gate is the part of the format that is pure to begin with, which is
// why it is batch A.
//
// THE NEGATIVE CASE IS THE LOAD-BEARING ONE, and it is the reason these assertions exist at all: a
// silent misparse of a mismatched save is the failure mode that costs a user their game. "Refuses"
// has to be tested as hard as "accepts".
//
#include "save/save_block.h"
#include "save/save_driver.h"
#include "save/save_ext.h"
#include "save/save_format.h"
#include "state/host_api.h" // SIMABI-VFS: the negative arm binds a short-reading host table
#include "sv1_lzw_fixtures.gen.h"

#include "hostapi_selftest_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int  g_checks, g_fails;
void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

using mh::save::VERSION_REJECT;
using mh::save::VERSION_SLOTS;
using mh::save::VERSION_STRING_LEN;
using mh::save::version_table;

// The real table, byte-for-byte as read out of /eng/mh.exe at 0x005d08f4: six 40-byte entries, SPACE
// PADDED and not nul-terminated within the table. The padding matters -- it is why the compare is a
// fixed-length memcmp and not a string compare, and a test built from C strings would not catch a
// translation that used strcmp.
struct table_fixture {
    char    slots[VERSION_SLOTS * VERSION_STRING_LEN];
    int32_t build_index = 5;

    table_fixture() {
        static const char *const src[VERSION_SLOTS] = {
            " Extermination, demo version",
            " Extermination, ver 1.0",
            " Extermination, ver 1.01",
            " Extermination, ver 1.02",
            " Extermination, ver 1.03",
            " 'Mission: Humanity' ver 1.00",
        };
        std::memset(slots, ' ', sizeof(slots));
        for (int i = 0; i < VERSION_SLOTS; ++i) {
            char *p = slots + i * VERSION_STRING_LEN;
            std::memcpy(p, src[i], std::strlen(src[i]));
            // THE LAST SIX BYTES ARE NUL, NOT SPACE. Re-read out of the exe (DGROUP 0x005d08f4, file
            // offset 0x1b70f4) while building the driver tests, because a fixture that pads all 40
            // bytes with spaces is self-consistent -- every test here still passes -- but matches no
            // real save file, so the "differs only in the final padding byte" case was exercising a
            // byte value the format never uses.
            std::memset(p + 34, 0, 6);
        }
    }
    version_table vt() const { return version_table{slots, &build_index}; }
    const char   *header_for(int slot) const { return slots + slot * VERSION_STRING_LEN; }
};

void test_accepts_every_supported_slot() {
    table_fixture f;
    for (int i = 1; i < VERSION_SLOTS; ++i) {
        char what[64];
        std::snprintf(what, sizeof(what), "SV: slot %d is accepted and reported as %d", i, i);
        check(what, mh::save::detect_version(f.vt(), f.header_for(i)) == i);
    }
}

void test_refuses_unknown_header() {
    table_fixture f;
    char          bogus[VERSION_STRING_LEN];
    std::memset(bogus, ' ', sizeof(bogus));
    std::memcpy(bogus, " Some Other Game, ver 9.9", 25);
    check("SV: an unknown header is REFUSED, not accepted as slot 0",
          mh::save::detect_version(f.vt(), bogus) == VERSION_REJECT);

    // The nastiest realistic case: a header that shares a long prefix with a real entry and differs
    // only in the last byte. A translation that compared a shortened length would accept this.
    char nearly[VERSION_STRING_LEN];
    std::memcpy(nearly, f.header_for(5), VERSION_STRING_LEN);
    nearly[VERSION_STRING_LEN - 1] = 'X';
    check("SV: a header differing only in the FINAL padding byte is refused",
          mh::save::detect_version(f.vt(), nearly) == VERSION_REJECT);

    char nearly2[VERSION_STRING_LEN];
    std::memcpy(nearly2, f.header_for(4), VERSION_STRING_LEN);
    nearly2[22] = '9'; // " Extermination, ver 1.03" -> "...1.93"
    check("SV: a header differing in one middle byte is refused",
          mh::save::detect_version(f.vt(), nearly2) == VERSION_REJECT);
}

void test_demo_slot_is_not_reachable_from_a_real_build() {
    table_fixture f; // build_index 5, i.e. the shipped configuration
    check("SV: a DEMO save is refused by a non-demo build (the scan starts at 1, not 0)",
          mh::save::detect_version(f.vt(), f.header_for(0)) == VERSION_REJECT);
}

void test_demo_build_path() {
    table_fixture f;
    f.build_index = 0;
    check("SV: a demo build accepts its own header", mh::save::detect_version(f.vt(), f.header_for(0)) ==
                                                         mh::save::VERSION_DEMO_OK);
    check("SV: a demo build REFUSES a Mission: Humanity save",
          mh::save::detect_version(f.vt(), f.header_for(5)) == VERSION_REJECT);
    check("SV: a demo build refuses an Extermination 1.03 save",
          mh::save::detect_version(f.vt(), f.header_for(4)) == VERSION_REJECT);
}

void test_compat_gates() {
    // What each gate decides, stated as the behaviour rather than the comparison, so a flipped
    // inequality fails by name.
    check("SV: slot 1 has the legacy extra block", mh::save::has_legacy_extra_block(1));
    check("SV: slot 2 does NOT have the legacy extra block", !mh::save::has_legacy_extra_block(2));
    check("SV: slot 3 still has the narrow message queue", mh::save::has_narrow_message_queue(3));
    check("SV: slot 4 does NOT have the narrow message queue", !mh::save::has_narrow_message_queue(4));
    check("SV: slot 4 lacks the invasion/advisor blocks", !mh::save::has_invasion_advisor_blocks(4));
    check("SV: slot 5 has the invasion/advisor blocks", mh::save::has_invasion_advisor_blocks(5));

    // The shipped build stamps slot 5, so a save it wrote must take NO compatibility path at all.
    // This is the assertion that would catch a gate accidentally left inclusive.
    check("SV: a save this build wrote takes no compat path",
          !mh::save::has_legacy_extra_block(5) && !mh::save::has_narrow_message_queue(5) &&
              mh::save::has_invasion_advisor_blocks(5));
}

void test_last_match_wins() {
    // Faithfulness detail with no observable effect on the real table (all six entries are distinct),
    // reproduced because "unobservable given today's data" is not "equivalent": the original loop has
    // no early exit, so a duplicated entry resolves to the HIGHEST matching slot.
    table_fixture f;
    std::memcpy(f.slots + 2 * VERSION_STRING_LEN, f.header_for(5), VERSION_STRING_LEN);
    check("SV: with a duplicated entry the LAST match wins (no early exit)",
          mh::save::detect_version(f.vt(), f.header_for(5)) == 5);
}


using mh::save::block_header;
using mh::save::block_io;
using mh::save::BLOCK_READ_CAP;

// Big enough for the largest block in any save on hand (block 98 of 11.sav expands to 1329120).
constexpr uint32_t BIG = 2u << 20;

unsigned char          g_staging[mh::save::STAGING_BYTES];
unsigned char          g_dst[BIG];
unsigned char          g_plain[BIG];
unsigned char          g_file[BIG];
uint32_t               g_max_seen;
mh::lzw::encoder_state g_enc;
mh::lzw::decoder_state g_dec;

mh::save::block_workspace ws() { return {g_staging, &g_max_seen, &g_enc, &g_dec}; }

// A file in memory. The read side mimics the CRT's short read (fewer bytes, no error) because the
// original DISCARDS read_from_file's result -- reproducing that is the point of one test below.
struct mem_file {
    unsigned char *buf;
    uint32_t       cap, len, pos;
    bool           write_fails;
};

int mem_write(void *ctx, const void *data, uint32_t size) {
    mem_file *f = static_cast<mem_file *>(ctx);
    if (f->write_fails || f->len + size > f->cap) return 0; // anything but 1 is a failure
    // A ZERO SIZE RETURNS 0, NOT 1 -- the seam is write_to_file(data, size, count=1, handle) over
    // fwrite, and fwrite writes zero items when the item size is zero. Modelled because
    // write_extension's zero-length guard exists for exactly this, and without it here the guard's
    // mutation is unobservable: the harness would report success where the CRT reports failure.
    if (size == 0) return 0;
    std::memcpy(f->buf + f->len, data, size);
    f->len += size;
    return 1;
}

// Returns the byte count, as the CRT's read_from_file does -- and a SHORT count when the file ends
// early, which is the case the original cannot see because it discards this value.
int mem_read(void *ctx, void *dst, uint32_t size) {
    mem_file      *f         = static_cast<mem_file *>(ctx);
    const uint32_t available = f->pos < f->len ? f->len - f->pos : 0;
    const uint32_t n         = size < available ? size : available;
    std::memcpy(dst, f->buf + f->pos, n);
    f->pos += n;
    return static_cast<int>(n);
}

uint64_t fnv1a64(const void *data, size_t n) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    uint64_t             h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// A deterministic filler, so a failure is reproducible and no test depends on rand().
void fill_pattern(unsigned char *p, uint32_t n, uint32_t seed, int entropy) {
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        // entropy 0: long runs (compresses hard). 1: mixed. 2: incompressible.
        p[i] = entropy == 0   ? static_cast<unsigned char>((i / 977) & 0xff)
               : entropy == 1 ? static_cast<unsigned char>((x & 0x0f) + (i & 0x30))
                              : static_cast<unsigned char>(x >> 24);
    }
}

// ---------------------------------------------------------------------------------------------

// The load-bearing pair. Decode a real block and check the plaintext against the INDEPENDENT
// oracle's hash; then re-encode that plaintext and require the original's bytes back, exactly.
void test_real_blocks_round_trip_byte_identically() {
    for (int i = 0; i < sv1_fixtures::COUNT; ++i) {
        const sv1_fixtures::block_fixture &f = sv1_fixtures::ALL[i];
        char                               what[128];

        mem_file in{const_cast<unsigned char *>(f.block), f.block_bytes, f.block_bytes, 0, false};
        block_io io{mem_write, mem_read, &in};

        std::memset(g_dst, 0xcd, f.uncompressed_size);
        const int status = mh::save::read_block(io, ws(), g_dst, f.uncompressed_size);

        std::snprintf(what, sizeof what, "SV: %s reads back with status 0", f.name);
        check(what, status == 0);
        std::snprintf(what, sizeof what, "SV: %s decodes to the INDEPENDENT oracle's plaintext", f.name);
        check(what, fnv1a64(g_dst, f.uncompressed_size) == f.plaintext_fnv1a64);

        // Now the encoder, fed a plaintext the line above just certified.
        mem_file  out{g_file, BIG, 0, 0, false};
        block_io  out_io{mem_write, mem_read, &out};
        const int wstatus = mh::save::write_block(out_io, ws(), g_dst, f.uncompressed_size);

        std::snprintf(what, sizeof what, "SV: %s writes with status 0", f.name);
        check(what, wstatus == 0);
        std::snprintf(what, sizeof what, "SV: %s re-encodes to the same BYTE COUNT", f.name);
        check(what, out.len == f.block_bytes);
        std::snprintf(what, sizeof what, "SV: %s re-encodes BYTE-IDENTICALLY to the original", f.name);
        check(what, out.len == f.block_bytes && std::memcmp(out.buf, f.block, f.block_bytes) == 0);
    }
}

// A fixture set can lose coverage silently when it is regenerated from a different save, and then
// every assertion above still passes while proving less. This is the check that goes red for that.
void test_fixtures_still_cover_the_codec() {
    uint32_t widths    = 0;
    bool     any_reset = false, any_expansion = false;
    for (int i = 0; i < sv1_fixtures::COUNT; ++i) {
        const sv1_fixtures::block_fixture &f = sv1_fixtures::ALL[i];
        widths |= 1u << f.max_code_bits;
        any_reset |= f.dict_resets > 0;
        any_expansion |= (f.block_bytes - sizeof(block_header)) > f.uncompressed_size;
    }
    const uint32_t nine_to_twelve = (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12);
    check("SV: fixtures still exercise 9, 10, 11 AND 12-bit codes",
          (widths & nine_to_twelve) == nine_to_twelve);
    check("SV: a fixture still fills the dictionary and resets MID-STREAM", any_reset);
    check("SV: a fixture still EXPANDS (compressed larger than raw)", any_expansion);
}

void test_synthetic_round_trips() {
    struct {
        const char *name;
        uint32_t    size;
        int         entropy;
    } cases[] = {
        {"1 byte", 1, 1},
        {"2 bytes", 2, 1},
        {"3 bytes", 3, 1},
        {"256 bytes", 256, 1},
        {"long runs, 300000 bytes", 300000, 0},
        {"mixed, 120000 bytes", 120000, 1},
        {"incompressible, 200000 bytes", 200000, 2},
    };
    for (const auto &c : cases) {
        char what[128];
        fill_pattern(g_plain, c.size, 0x5eed + c.size, c.entropy);

        const uint32_t len = mh::lzw::compress(g_enc, g_plain, c.size, g_file);
        std::snprintf(what, sizeof what, "SV: %s compresses within compressed_bound", c.name);
        check(what, len > 0 && len <= mh::lzw::compressed_bound(c.size));

        mh::lzw::payload_header h{};
        std::memcpy(&h, g_file, sizeof h);
        std::snprintf(what, sizeof what, "SV: %s gets a well-formed payload header", c.name);
        check(what, h.magic == mh::lzw::MAGIC && h.uncompressed_size == c.size &&
                        h.compressed_len == len);

        uint32_t io_size = len;
        std::memset(g_dst, 0xcd, c.size);
        mh::lzw::decompress(g_dec, g_file, g_dst, &io_size, c.size);
        std::snprintf(what, sizeof what, "SV: %s round-trips to the same %u bytes", c.name, c.size);
        check(what, io_size == c.size && std::memcmp(g_dst, g_plain, c.size) == 0);
    }
}

// SV1's done_when, the cap-raised half: a bigger-than-vanilla block is written and re-read, and a
// build expecting the vanilla size REFUSES it instead of misparsing it. The refusal is the clause
// that matters -- a silent misparse is what costs a user their game.
void test_cap_raised_block() {
    const uint32_t VANILLA = 0x2d820; // the units array, `MOV EDX,0x2d820` @0x00447ee5
    const uint32_t RAISED  = 500000;  // the same array in a 500-unit roster build

    fill_pattern(g_plain, RAISED, 0xca9, 1);
    mem_file f{g_file, BIG, 0, 0, false};
    block_io io{mem_write, mem_read, &f};
    check("SV: a cap-raised block is written", mh::save::write_block(io, ws(), g_plain, RAISED) == 0);
    check("SV: the cap-raised block is under the read cap after compression",
          f.len - sizeof(block_header) <= BLOCK_READ_CAP);

    f.pos = 0;
    std::memset(g_dst, 0xcd, RAISED);
    check("SV: the cap-raised block reads back",
          mh::save::read_block(io, ws(), g_dst, RAISED) == 0);
    check("SV: the cap-raised block reads back IDENTICALLY",
          std::memcmp(g_dst, g_plain, RAISED) == 0);

    // The same bytes, offered to a reader whose array is the vanilla size.
    f.pos = 0;
    std::memset(g_dst, 0xcd, VANILLA);
    const int refused = mh::save::read_block(io, ws(), g_dst, VANILLA);
    check("SV: a vanilla-sized reader REFUSES the cap-raised block", refused == 1);
    bool untouched = true;
    for (uint32_t i = 0; i < VANILLA; ++i)
        if (g_dst[i] != 0xcd) untouched = false;
    check("SV: and it wrote NOTHING into the destination before refusing", untouched);
}

void test_framing_refusals() {
    // A header whose uncompressed size disagrees with the caller's array, by one byte.
    {
        block_header h{64, 100};
        std::memcpy(g_file, &h, sizeof h);
        mem_file f{g_file, BIG, sizeof h + 64, 0, false};
        block_io io{mem_write, mem_read, &f};
        check("SV: a block whose size disagrees with the destination by 1 is refused",
              mh::save::read_block(io, ws(), g_dst, 99) == 1);
    }
    // The compressed-length cap. `JBE` at 0x00448744 makes it INCLUSIVE, so the boundary pair is
    // the whole test: exactly the cap must pass the guard, one more must not.
    {
        block_header h{BLOCK_READ_CAP + 1, 16};
        std::memcpy(g_file, &h, sizeof h);
        mem_file f{g_file, BIG, sizeof h, 0, false}; // the payload is never reached
        block_io io{mem_write, mem_read, &f};
        check("SV: a block claiming MORE than the cap is refused",
              mh::save::read_block(io, ws(), g_dst, 16) == 1);
    }
    {
        // Exactly the cap: a real payload, then padding out to the cap. This is also the test that
        // would fault without STAGING_SLACK -- the read puts cap bytes at staging+8, which is eight
        // bytes past where the ORIGINAL's buffer ends.
        fill_pattern(g_plain, 4096, 7, 1);
        const uint32_t payload = mh::lzw::compress(g_enc, g_plain, 4096, g_file + sizeof(block_header));
        block_header   h{BLOCK_READ_CAP, 4096};
        std::memcpy(g_file, &h, sizeof h);
        std::memset(g_file + sizeof(block_header) + payload, 0,
                    BLOCK_READ_CAP - payload); // padding the reader must tolerate
        mem_file f{g_file, BIG, sizeof(block_header) + BLOCK_READ_CAP, 0, false};
        block_io io{mem_write, mem_read, &f};
        check("SV: a block claiming EXACTLY the cap is accepted (the guard is inclusive)",
              mh::save::read_block(io, ws(), g_dst, 4096) == 0);
        check("SV: and it still decodes correctly", std::memcmp(g_dst, g_plain, 4096) == 0);
    }
    // A failing write is reported, and it is the WRITE's return that decides -- not the codec's.
    {
        mem_file f{g_file, BIG, 0, 0, true};
        block_io io{mem_write, mem_read, &f};
        fill_pattern(g_plain, 1024, 3, 1);
        check("SV: a failed write is reported as failure",
              mh::save::write_block(io, ws(), g_plain, 1024) == 1);
    }
}

// docs/save-format.md's open question 1, settled and pinned here: a payload with no 'LZW ' magic is
// not an error, it is the OTHER codec. The stream below is hand-assembled from the format at
// 0x004ddc70: one 16-bit flag word (MSB first) = four literals, then a match; a 3-bit length code of
// 2 meaning four bytes; and an offset byte of 252, i.e. a distance of 256-252 = 4.
void test_lzss_fallback() {
    const unsigned char payload[] = {
        0x00, 0x0a,         // flags 0b0000101000000000: 0,0,0,0 literal; 1 match; 010 length
        'A', 'B', 'C', 'D', // the four literals
        0xfc,               // offset 252 -> copy 4 bytes from out-4
    };
    block_header h{sizeof payload, 8};
    std::memcpy(g_file, &h, sizeof h);
    std::memcpy(g_file + sizeof h, payload, sizeof payload);

    mem_file f{g_file, BIG, sizeof h + sizeof payload, 0, false};
    block_io io{mem_write, mem_read, &f};
    std::memset(g_dst, 0xcd, 16);
    const int status = mh::save::read_block(io, ws(), g_dst, 8);

    check("SV: a non-LZW block reports SUCCESS (the -1 is 'not LZW', not an error)", status == 0);
    check("SV: a non-LZW block is decoded by the LZSS path",
          std::memcmp(g_dst, "ABCDABCD", 8) == 0);
}

// TRUNCATION, which is the sharp end of docs/save-format.md's open question 1. Getting this
// characterised took three attempts and both wrong answers are worth recording, because each looked
// obviously right: "a truncated payload is refused" (it is not, the read count is thrown away) and
// then "a truncated payload decodes to the WRONG data" (not necessarily -- if the staging leftovers
// happen to be the bytes that were cut off, it decodes perfectly). What is actually true takes two
// tests, because two different mechanisms are in play.
//
// One: the block layer cannot see a short read at all.
void test_short_read_goes_unreported() {
    fill_pattern(g_plain, 8192, 11, 1);
    const uint32_t full = mh::lzw::compress(g_enc, g_plain, 8192, g_file + sizeof(block_header));
    block_header   h{full, 8192};
    std::memcpy(g_file, &h, sizeof h);

    // Read it intact once, so the staging buffer holds this block's own payload -- the ordinary
    // situation when a load re-reads a block, and the most favourable case for the reader.
    {
        mem_file f{g_file, BIG, sizeof h + full, 0, false};
        block_io io{mem_write, mem_read, &f};
        check("SV: (setup) the intact block reads back",
              mh::save::read_block(io, ws(), g_dst, 8192) == 0);
    }

    // Now offer the same block with its payload cut in half. The header still claims the full
    // length, so the size check passes; the read returns half as many bytes and NOBODY LOOKS.
    mem_file f{g_file, BIG, sizeof h + full / 2, 0, false};
    block_io io{mem_write, mem_read, &f};
    std::memset(g_dst, 0xcd, 8192);
    check("SV: a truncated payload reports SUCCESS -- the read count is discarded",
          mh::save::read_block(io, ws(), g_dst, 8192) == 0);

    // The opt-in check, which is the hardening SV1-P has to decide on. Same bytes, same call, one
    // flag. Without this half `strict_reads` would be untested code.
    f.pos                            = 0;
    mh::save::block_workspace strict = ws();
    strict.strict_reads              = true;
    check("SV: with strict_reads the same truncated block is REFUSED",
          mh::save::read_block(io, strict, g_dst, 8192) == 1);
}

// SIMABI-VFS's NEGATIVE ARM (2026-09-10). The two tests above prove the block layer's half over a
// memory stub. This one proves the WHOLE CHAIN, through the real host table: a HOST that short-reads
// -> the byte count crossing the ABI -> `strict_reads` refusing. The io entry's own contract sentence
// is "a short read must be REPORTED, not padded", and an entry's sentence is worth what a test makes
// it worth.
//
// Only the middle link is restated here: `live_read` is static inside mh.dll's save_live.cpp and
// there is no game process to install it into, so the two lines it consists of are written out. Both
// ENDS are the shipped ones -- a real `libmh_host_api` binding, and the real `read_block`.
mem_file *g_vfs_file     = nullptr;
uint32_t  g_vfs_short_by = 0;

// Honest about the 8-byte header (a garbled header would refuse for the wrong reason), deliberately
// short on the payload -- which is exactly the shape a truncated file, a partial socket read or a
// mapped-file boundary produces in a real host.
int32_t short_vfs_read(int32_t handle, void *dst, uint32_t n) {
    (void)handle;
    const uint32_t want =
        (n > sizeof(mh::save::block_header) && n > g_vfs_short_by) ? n - g_vfs_short_by : n;
    return mem_read(g_vfs_file, dst, want);
}

// save_live.cpp's `live_read`, restated: a byte count in, a byte count out, no padding, no retry.
int host_backed_read(void *ctx, void *dst, uint32_t size) {
    return mh::host().vfs_read(static_cast<int32_t>(reinterpret_cast<uintptr_t>(ctx)), dst, size);
}

void test_short_vfs_read_is_reported() {
    fill_pattern(g_plain, 8192, 23, 1);
    const uint32_t full = mh::lzw::compress(g_enc, g_plain, 8192, g_file + sizeof(block_header));
    block_header   h{full, 8192};
    std::memcpy(g_file, &h, sizeof h);

    mem_file f{g_file, BIG, sizeof h + full, 0, false};
    g_vfs_file     = &f;
    g_vfs_short_by = 0;

    libmh_host_api shorty = mh_hostapi_selftest_table();
    shorty.vfs_read       = &short_vfs_read;
    check("SV/VFS: the short-reading host table binds",
          libmh_set_host_api(&shorty, LIBMH_HOST_API_VERSION) == 0);

    block_io io{mem_write, host_backed_read, reinterpret_cast<void *>(static_cast<uintptr_t>(1))};

    // Read it intact through the same host first, so the staging buffer holds THIS block's own
    // payload. Same setup as test_short_read_goes_unreported, and for the same reason: it makes the
    // control below decode successfully, so the only thing separating the two arms is the flag
    // rather than the decoder running off the end of some other block's leftovers.
    std::memset(g_dst, 0xcd, 8192);
    check("SV/VFS: (setup) the intact block reads back through the host ABI",
          mh::save::read_block(io, ws(), g_dst, 8192) == 0);

    f.pos          = 0;
    g_vfs_short_by = 64; // now the host hands back 64 bytes fewer than asked -- and says so
    std::memset(g_dst, 0xcd, 8192);
    mh::save::block_workspace strict = ws();
    strict.strict_reads              = true;
    const int status                 = mh::save::read_block(io, strict, g_dst, 8192);
    printf("  [savetest] vfs_read short by %u of %u payload bytes -> read_block status %d "
           "(1 = REPORTED/refused)\n",
           (unsigned)g_vfs_short_by, (unsigned)full, status);
    check("SV/VFS: a short vfs_read is REPORTED through the host ABI, not padded", status == 1);

    // And the control: the SAME host, the same file, `strict_reads` off. The count still crosses the
    // ABI intact -- what changes is only whether anybody looks. Without this half the test would
    // equally pass a host that failed the read outright, which is a different bug.
    f.pos                                = 0;
    mh::save::block_workspace lax        = ws();
    const int                 lax_status = mh::save::read_block(io, lax, g_dst, 8192);
    check("SV/VFS: the same short read is silently accepted with strict_reads off (the original's "
          "behaviour, unchanged)",
          lax_status == 0);

    check("SV/VFS: the selftest host is restored",
          libmh_set_host_api(&mh_hostapi_selftest_table(), LIBMH_HOST_API_VERSION) == 0);
    g_vfs_file     = nullptr;
    g_vfs_short_by = 0;
}

// Two: when the leftovers are some OTHER block's bytes -- the real case, since a load reads a
// different block each time -- the code stream runs out of claimed length before it finds an
// end-of-stream code, and our input bound turns that into a refusal. THIS IS A DIVERGENCE: the
// original has no input bound, so it keeps reading past the staging buffer into whatever follows it
// in .bss until it finds a 0x101 or faults. Refusing is the only serviceable choice, and it is
// asserted here so it cannot change silently.
void test_truncation_with_foreign_leftovers_is_bounded() {
    const uint32_t other = 16384;
    fill_pattern(g_plain, other, 0x9a1, 2); // incompressible, so its payload outruns the short one
    const uint32_t olen = mh::lzw::compress(g_enc, g_plain, other, g_file + sizeof(block_header));
    {
        block_header oh{olen, other};
        std::memcpy(g_file, &oh, sizeof oh);
        mem_file f{g_file, BIG, sizeof oh + olen, 0, false};
        block_io io{mem_write, mem_read, &f};
        check("SV: (setup) an unrelated block reads back, leaving ITS bytes in staging",
              mh::save::read_block(io, ws(), g_dst, other) == 0);
    }

    fill_pattern(g_plain, 8192, 11, 1);
    const uint32_t full = mh::lzw::compress(g_enc, g_plain, 8192, g_file + sizeof(block_header));
    block_header   h{full, 8192};
    std::memcpy(g_file, &h, sizeof h);
    mem_file f{g_file, BIG, sizeof h + full / 2, 0, false};
    block_io io{mem_write, mem_read, &f};
    check("SV: a truncated payload with foreign leftovers is refused by the input bound",
          mh::save::read_block(io, ws(), g_dst, 8192) == 1);
}

void test_high_water_statistic() {
    g_max_seen = 0;
    mem_file f{g_file, BIG, 0, 0, false};
    block_io io{mem_write, mem_read, &f};
    fill_pattern(g_plain, 40000, 5, 1);
    mh::save::write_block(io, ws(), g_plain, 40000);
    const uint32_t first = g_max_seen;
    check("SV: the high-water statistic tracks the first block", first > 0);

    f.len = 0;
    fill_pattern(g_plain, 4000, 5, 1);
    mh::save::write_block(io, ws(), g_plain, 4000);
    check("SV: a SMALLER block does not lower the high-water mark", g_max_seen == first);
}

// THE WRITE SIDE HAS NO CAP, WHICH IS THE REAL CEILING ON A TIER-C CAP RAISE. `0x927c0` appears
// exactly ONCE in the whole binary -- the read guard at 0x00448744 -- so the original happily
// compresses into G_LZW_TEMP_DATA+8 with no bound and overflows its own 600 000-byte staging buffer
// once an array's COMPRESSED form gets that big. Ours refuses instead. The number to watch for a cap
// raise is therefore not the array size but its compressed size, and nothing in the game warns you.
void test_writer_refuses_what_would_overflow_staging() {
    // Incompressible, so the payload is slightly LARGER than the input and comfortably over the cap.
    const uint32_t huge = BLOCK_READ_CAP + 4096;
    fill_pattern(g_plain, huge, 0xf00d, 2);
    mem_file f{g_file, BIG, 0, 0, false};
    block_io io{mem_write, mem_read, &f};
    check("SV: a block whose COMPRESSED form would overflow staging is refused, not written",
          mh::save::write_block(io, ws(), g_plain, huge) == 1);
    check("SV: and nothing was written to the file", f.len == 0);

    // The boundary in the other direction: a big block that DOES fit is still written. Without this
    // the refusal above could be a blanket size limit and the test would not notice.
    const uint32_t big_ok = 400000;
    fill_pattern(g_plain, big_ok, 0xbeef, 1);
    f.len = 0;
    check("SV: a large block that fits is still written",
          mh::save::write_block(io, ws(), g_plain, big_ok) == 0);
    f.pos = 0;
    check("SV: and reads back identically",
          mh::save::read_block(io, ws(), g_dst, big_ok) == 0 &&
              std::memcmp(g_dst, g_plain, big_ok) == 0);
}

// The decoder's output bound, which is what stops a corrupt stream writing past the game's array. The
// header check cannot catch this case: the header agrees with the caller, and the PAYLOAD lies.
void test_decoder_output_bound() {
    fill_pattern(g_plain, 20000, 0x1234, 1);
    const uint32_t payload = mh::lzw::compress(g_enc, g_plain, 20000, g_file + sizeof(block_header));

    // A header claiming the block expands to far less than the payload actually decodes to. A reader
    // whose array is that smaller size must refuse rather than overrun it.
    block_header h{payload, 4096};
    std::memcpy(g_file, &h, sizeof h);
    mem_file f{g_file, BIG, sizeof h + payload, 0, false};
    block_io io{mem_write, mem_read, &f};

    const unsigned char guard = 0x5a;
    std::memset(g_dst, guard, 8192);
    check("SV: a payload that decodes past the destination is refused",
          mh::save::read_block(io, ws(), g_dst, 4096) == 1);
    bool past_untouched = true;
    for (uint32_t i = 4096; i < 8192; ++i)
        if (g_dst[i] != guard) past_untouched = false;
    check("SV: and it wrote NOTHING past the destination's end", past_untouched);
}

// The other way a corrupt payload escapes: a PREFIX CYCLE in the dictionary, which makes the chain
// walk non-terminating. Without the cap this hangs -- in the original as much as here -- and a hang
// is a much worse failure than a refusal. Mutation-checking the cap needs a stream that actually
// builds a cycle, and a corrupt stream cannot be relied on to produce one, so the cycle is planted
// directly: `decoder_state` is a plain struct and `decompress` deliberately does NOT clear the node
// array (the original does not either), which is exactly what makes a stale chain reachable.
void test_prefix_cycle_is_refused_not_hung() {
    // Entry 0's prefix is code 0x102, which IS entry 0 -- a self-cycle.
    g_dec.nodes[0].prefix = mh::lzw::CODE_FIRST;
    g_dec.nodes[0].ch     = 'X';

    // 9-bit codes, LSB-first: 0x100 (reset), 0x102 (the cyclic entry), 0x101 (end).
    const uint32_t                stream  = mh::lzw::CODE_RESET | (mh::lzw::CODE_FIRST << 9) | (mh::lzw::CODE_END << 18);
    unsigned char                *payload = g_file + sizeof(block_header);
    const mh::lzw::payload_header ph{mh::lzw::MAGIC, 64, 16};
    std::memcpy(payload, &ph, sizeof ph);
    std::memset(payload + sizeof ph, 0, 16 - sizeof ph);
    std::memcpy(payload + sizeof ph, &stream, 4);

    const block_header bh{16, 64};
    std::memcpy(g_file, &bh, sizeof bh);
    mem_file f{g_file, BIG, sizeof bh + 16, 0, false};
    block_io io{mem_write, mem_read, &f};

    check("SV: a payload whose dictionary chain is a CYCLE is refused (and terminates)",
          mh::save::read_block(io, ws(), g_dst, 64) == 1);
}

// ---------------------------------------------------------------------------------------------
// Opt-in: run every block of a real save through the layer. Not part of the default run -- the
// hermetic fixtures above are the gate -- but this is what backs the claim that the framing and the
// codec agree with the original over a whole file rather than six chosen blocks.
// ---------------------------------------------------------------------------------------------
// SV1 batch C: APPENDED CAPACITY AT EOF. See save_ext.h for the design; the two claims worth testing
// are (1) a vanilla reader that stops on its own derived condition leaves a trailer untouched, which
// is what makes appending safe, and (2) appending is only half a feature -- the other half is that the
// file must then be REFUSED by a vanilla build, or "safe to ignore" becomes "silently misparsed".

void test_extension_round_trips() {
    static unsigned char payload[300000];
    fill_pattern(payload, sizeof payload, 0x5e11, 1);

    mem_file f{g_file, BIG, 0, 0, false};
    block_io io{mem_write, mem_read, &f};

    // One vanilla block first, so the extension really is a TRAILER and not the whole file.
    fill_pattern(g_plain, 4096, 0x11, 0);
    check("SVX: the vanilla block still writes", mh::save::write_block(io, ws(), g_plain, 4096) == 0);
    const uint32_t vanilla_end = f.len;

    check("SVX: an extension is appended",
          mh::save::write_extension(io, mh::save::EXT_TAG_UNITS, payload, sizeof payload) == 0);
    check("SVX: it added exactly header + payload",
          f.len == vanilla_end + sizeof(mh::save::extension_header) + sizeof payload);

    // A VANILLA READER, i.e. one that reads only the blocks its table names and then stops.
    f.pos = 0;
    std::memset(g_dst, 0, 4096);
    check("SVX: a vanilla reader still reads its block", mh::save::read_block(io, ws(), g_dst, 4096) == 0);
    check("SVX: and gets its bytes back unaffected by the trailer",
          std::memcmp(g_dst, g_plain, 4096) == 0);
    check("SVX: and stops exactly at the end of the vanilla region", f.pos == vanilla_end);

    // An EXTENSION-AWARE reader picks up where the vanilla one stopped.
    mh::save::extension_header hdr{};
    check("SVX: the trailer is found at the vanilla cursor",
          mh::save::read_extension_header(io, &hdr) == 0);
    check("SVX: with the tag it was written under", hdr.tag == mh::save::EXT_TAG_UNITS);
    check("SVX: and the payload length", hdr.length == sizeof payload);
    static unsigned char got[sizeof payload];
    std::memset(got, 0xcd, sizeof got);
    check("SVX: the payload reads back", mh::save::read_extension_payload(io, hdr, got, sizeof got) == 0);
    check("SVX: BYTE-IDENTICALLY", std::memcmp(got, payload, sizeof payload) == 0);

    // The payload is larger than the 600 000-byte staging buffer would allow a single COMPRESSED block
    // to be at the boundary, and larger than every vanilla immediate except the roster arrays -- the
    // point being that an extension is not subject to the codec's ceiling at all.
    check("SVX: the extension carried a payload the block path bounds", sizeof payload > 262144);
}

void test_extension_is_not_confused_with_other_trailers() {
    mem_file f{g_file, BIG, 0, 0, false};
    block_io io{mem_write, mem_read, &f};

    // The vanilla writer's OWN trailer: a 0x400 media-diag block that no reader ever reads
    // (llm_build_media_diag_report -> write_block, and llm_game_load stops before it). An
    // extension-aware reader must not mistake that for an extension.
    fill_pattern(g_plain, 0x400, 0xda9, 1);
    mh::save::write_block(io, ws(), g_plain, 0x400);
    f.pos = 0;
    mh::save::extension_header hdr{};
    check("SVX: a vanilla block is NOT read as an extension",
          mh::save::read_extension_header(io, &hdr) == 1);

    // A file that simply ends.
    mem_file empty{g_file, BIG, 0, 0, false};
    block_io eio{mem_write, mem_read, &empty};
    check("SVX: end-of-file is NOT read as an extension",
          mh::save::read_extension_header(eio, &hdr) == 1);

    // A truncated extension: a good header, then a payload that stops short. The vanilla block reader
    // cannot see truncation (it discards the count); this framing is ours, so it must.
    mem_file             t{g_file, BIG, 0, 0, false};
    block_io             tio{mem_write, mem_read, &t};
    static unsigned char small[64];
    fill_pattern(small, sizeof small, 0x77, 1);
    mh::save::write_extension(tio, mh::save::EXT_TAG_BUILDINGS, small, sizeof small);
    t.len -= 16; // lose the tail
    t.pos = 0;
    check("SVX: the truncated file's header still parses",
          mh::save::read_extension_header(tio, &hdr) == 0);
    unsigned char out64[sizeof small];
    check("SVX: but a SHORT PAYLOAD IS REFUSED, unlike the vanilla block reader",
          mh::save::read_extension_payload(tio, hdr, out64, sizeof out64) == 1);

    // A HEADER truncated mid-way, with its magic intact. This is the only input that separates the
    // header's byte-count check from the magic check: an EMPTY file leaves the whole struct zeroed so
    // the magic test catches it either way, and a mutation deleting the count check would pass. Here
    // the first four bytes ARE the magic and the tag/length are simply missing.
    mem_file h8{g_file, BIG, 0, 0, false};
    block_io h8io{mem_write, mem_read, &h8};
    mh::save::write_extension(h8io, mh::save::EXT_TAG_UNITS, small, sizeof small);
    h8.len = 8; // magic + tag present, length field cut off
    h8.pos = 0;
    check("SVX: a header truncated mid-struct is REFUSED even though its magic is valid",
          mh::save::read_extension_header(h8io, &hdr) == 1);

    // A member declaring more than the caller's array. Same shape as the block reader's size
    // self-check, and the reason an extension cannot be used to overrun a consumer.
    mem_file b{g_file, BIG, 0, 0, false};
    block_io bio{mem_write, mem_read, &b};
    mh::save::write_extension(bio, mh::save::EXT_TAG_UNITS, small, sizeof small);
    b.pos = 0;
    mh::save::read_extension_header(bio, &hdr);
    // THE DESTINATION HAS SLACK ON PURPOSE. The array is 256 bytes but only 32 are DECLARED to the
    // reader, so a reader that skipped the size check over-reads into slack rather than off the end
    // of a 32-byte stack object -- which is what happened the first time this was mutated: /GS
    // aborted the process and the mutation registered as "not caught" when the logic was in fact
    // fine. An assertion that can only fail by crashing is a poor instrument.
    static unsigned char roomy[256];
    std::memset(roomy, 0xcd, sizeof roomy);
    check("SVX: a member larger than the DECLARED destination size is REFUSED",
          mh::save::read_extension_payload(bio, hdr, roomy, 32) == 1);
    bool roomy_untouched = true;
    for (unsigned char c : roomy)
        if (c != 0xcd) roomy_untouched = false;
    check("SVX: and it wrote nothing at all before refusing", roomy_untouched);

    // Zero length is legal and means "the tag is present, the array is empty".
    mem_file z{g_file, BIG, 0, 0, false};
    block_io zio{mem_write, mem_read, &z};
    check("SVX: a zero-length member writes",
          mh::save::write_extension(zio, mh::save::EXT_TAG_UNITS, nullptr, 0) == 0);
    z.pos = 0;
    check("SVX: and reads back", mh::save::read_extension_header(zio, &hdr) == 0 && hdr.length == 0);
    check("SVX: with a no-op payload read",
          mh::save::read_extension_payload(zio, hdr, roomy, sizeof roomy) == 0);
}

void test_appending_capacity_requires_a_refused_version() {
    table_fixture f;

    // THE RULE. A build that appends capacity must stamp a version string the vanilla table does not
    // contain, so a vanilla build refuses the file at the header instead of loading the vanilla part
    // and silently dropping the extension. Without this, "a vanilla reader ignores the trailer" IS the
    // silent misparse SV1 calls load-bearing.
    char raised[VERSION_STRING_LEN];
    std::memset(raised, ' ', sizeof raised);
    std::memcpy(raised, " 'Mission: Humanity' ver 1.00-R500", 34);
    const int32_t v = mh::save::detect_version(f.vt(), raised);
    check("SVX: a cap-raised version string is REFUSED by a vanilla build", v == VERSION_REJECT);
    check("SVX: and the pairing rule agrees", mh::save::extension_requires_version_refusal(v));

    // The negative direction, so the rule is not vacuously true: a file stamped with a VANILLA string
    // would be accepted, which is exactly the combination an extension-writing build must never emit.
    const int32_t vanilla = mh::save::detect_version(f.vt(), f.header_for(5));
    check("SVX: a vanilla string is accepted, so the rule can fail", vanilla == 5);
    check("SVX: and the pairing rule REJECTS that combination",
          !mh::save::extension_requires_version_refusal(vanilla));
}

// ==================================================== SV1-DRIVERS: the table-driven save/load walk
//
// The format IS an ordered list of (address, size) block calls, extracted from the disassembly into
// save_table.gen.h. These tests drive that table in both directions with NO GAME AND NO FILE: the
// block layer already takes an injected `block_io`, and the driver adds an injected `state_io`, so a
// whole save file can be loaded into heap slabs and written back out.
//
// The strong oracle is BYTE IDENTITY of a re-save. It proves the table, the ORDER, the version gates,
// the framing and the three variable-length parts at once -- a wrong size or a dropped block shifts
// every following byte. What it does NOT prove is the block payloads' meaning, which is batch B's job
// (501 real blocks re-encoded byte-identically) and is not repeated here.

namespace drivers {

using mh::save::container_image;
using mh::save::driver_env;
using mh::save::driver_stats;
using mh::save::planet_image;
namespace tbl = mh::save::table;

// --------------------------------------------------------------------------------------------
// A SPARSE ADDRESS SPACE. `state_io::resolve` maps a game address to a heap slab, so the driver runs
// unmodified against memory that is nowhere near where the game would put it. In the live build the
// same resolver is the identity function -- that substitution is SV1-P.
constexpr int      SLAB_MAX   = 160;
constexpr uint32_t POOL_BYTES = 6u << 20;

struct slab {
    uint32_t base, len, off;
};

struct sparse_space {
    slab           r[SLAB_MAX];
    int            n = 0;
    unsigned char *pool;
    uint32_t       used = 0;

    void add(uint32_t base, uint32_t len) {
        // Merge into an existing slab when the ranges touch: the container's `progress` block and the
        // planet file's eight progress slices are the same memory, and two slabs for one region would
        // silently give the two drivers different state.
        for (int i = 0; i < n; ++i) {
            if (base >= r[i].base && base + len <= r[i].base + r[i].len) return;
        }
        if (n >= SLAB_MAX) {
            printf("  *** SLAB_MAX too small\n");
            return;
        }
        r[n].base = base;
        r[n].len  = len;
        r[n].off  = used;
        used += len;
        ++n;
    }

    unsigned char *find(uint32_t addr, uint32_t size) const {
        for (int i = 0; i < n; ++i)
            if (addr >= r[i].base && addr + size <= r[i].base + r[i].len)
                return pool + r[i].off + (addr - r[i].base);
        return nullptr;
    }
};

unsigned char g_pool[POOL_BYTES];
unsigned char g_pool_snapshot[POOL_BYTES];
sparse_space  g_space;

void *space_resolve(void *ctx, uint32_t addr, uint32_t size) {
    return static_cast<sparse_space *>(ctx)->find(addr, size);
}

// ---- SB-HOSTFREE: the RELOCATED arm's per-region resolver -------------------------------------
//
// Four save blocks span more than one region and are served run by run (save_table.gen.h
// SLICED_BLOCKS). What must be shown is that each run follows ITS OWN region -- not the region the
// block started in, which is what `covering()` would answer and is the bug the decomposition exists
// to fix. So this fixture answers one nominated region out of a SEPARATE buffer and leaves every
// other run in the pool; a gather that ignored the rid would miss it entirely.
//
// A fixture, not a second implementation: the live arm's region_resolve is save_live.cpp's, and it
// resolves through the registry. Both are the same two lines from the driver's point of view.
mh::state::region_id g_moved_rid = mh::state::RID_COUNT;
unsigned char        g_moved_buf[4096];

void *space_resolve_region(void *ctx, uint16_t rid, uint32_t off, uint32_t len) {
    if (rid == static_cast<uint16_t>(g_moved_rid) && off + len <= sizeof g_moved_buf)
        return g_moved_buf + off;
    // Everything else stays where the fixture put it, reached by its stock address -- the same
    // fall-through a resolver with no region view gets.
    return static_cast<sparse_space *>(ctx)->find(mh::state::REGIONS[rid].base + off, len);
}

// Compare state against the pre-save snapshot for exactly the regions ONE table names. The pool holds
// both tables' regions, and a container load never touches the planet's -- so comparing the whole
// pool would fail for a reason that is not the driver's.
bool state_matches(const tbl::step *steps, int count);

// The registered set comes from the TABLE, not from a hand-written list, so a re-derived table brings
// its own address space with it.
void build_space() {
    g_space.n                   = 0;
    g_space.used                = 0;
    g_space.pool                = g_pool;
    const tbl::step *programs[] = {tbl::CONTAINER, tbl::PLANET, tbl::REGIONS};
    const int        counts[]   = {tbl::CONTAINER_STEPS, tbl::PLANET_STEPS, tbl::REGIONS_STEPS};
    for (int p = 0; p < 3; ++p)
        for (int i = 0; i < counts[p]; ++i)
            if (programs[p][i].kind == tbl::STEP_BLOCK && programs[p][i].size != 0)
                g_space.add(programs[p][i].addr, programs[p][i].size);
    // The progress slices live inside the container's `progress` block, but the PLANET table alone
    // does not name them; and PROGRESS_SLICE_WORD is in NO block at all -- it comes from config, which
    // is the finding that makes a save readable only by a build whose config agrees.
    g_space.add(tbl::PROGRESS_BASE, tbl::PROGRESS_COUNT * tbl::PROGRESS_STRIDE);
    g_space.add(tbl::PROGRESS_SLICE_WORD, 2);
    if (g_space.used > POOL_BYTES) printf("  *** POOL_BYTES too small: need %u\n", g_space.used);
}

// --------------------------------------------------------------------------------------------
constexpr uint32_t ARENA_BYTES = 4u << 20;
constexpr uint32_t OUT_BYTES   = 2u << 20;
unsigned char      g_arena_mem[ARENA_BYTES];
unsigned char      g_out1[OUT_BYTES];
unsigned char      g_out2[OUT_BYTES];
unsigned char      g_savefile[OUT_BYTES];

// THE REAL VERSION TABLE, 240 bytes read straight out of /eng/mh.exe's DGROUP at 0x005d08f4 (file
// offset 0x1b70f4). Each entry is the string, SPACE-padded to 34 bytes, then SIX NUL BYTES -- which
// is not what a plausible fixture assumes, and is exactly the padding a real save file carries.
struct real_table {
    char    slots[VERSION_SLOTS * VERSION_STRING_LEN];
    int32_t build_index = 5; // _G_LLM_GAME_MISSION_VERSION_INDEX @0x005d09e4, a baked constant

    real_table() {
        static const char *const src[VERSION_SLOTS] = {
            " Extermination, demo version",
            " Extermination, ver 1.0",
            " Extermination, ver 1.01",
            " Extermination, ver 1.02",
            " Extermination, ver 1.03",
            " 'Mission: Humanity' ver 1.00",
        };
        std::memset(slots, ' ', sizeof slots);
        for (int i = 0; i < VERSION_SLOTS; ++i) {
            char *p = slots + i * VERSION_STRING_LEN;
            std::memcpy(p, src[i], std::strlen(src[i]));
            std::memset(p + 34, 0, 6);
        }
    }
    version_table vt() const { return version_table{slots, &build_index}; }
    const char   *header_for(int slot) const { return slots + slot * VERSION_STRING_LEN; }
};

real_table g_vt;

driver_env env_for(mem_file *f, driver_stats *st) {
    driver_env e{};
    e.io    = block_io{mem_write, mem_read, f};
    e.ws    = ws();
    e.st    = mh::save::state_io{space_resolve, &g_space, space_resolve_region};
    e.vt    = g_vt.vt();
    e.stats = st;
    return e;
}

mh::save::arena g_arena;
mh::save::arena fresh_arena() { return mh::save::arena{g_arena_mem, ARENA_BYTES, 0}; }

// A SECOND arena, and the reason it exists. The container's `member::bytes` point INTO the arena the
// container load allocated from, so re-using that arena for the planet passes overwrites the members
// that have not been walked yet -- which showed up as member 0 round-tripping and member 1 failing to
// read its first block. It failed in the safe direction (a false FAILURE, not a false pass), but a
// harness whose own bookkeeping corrupts the input is not evidence either way.
unsigned char   g_arena_mem2[ARENA_BYTES];
mh::save::arena fresh_arena2() { return mh::save::arena{g_arena_mem2, ARENA_BYTES, 0}; }

// Seed every registered slab, then force the predicate's inputs to a known answer.
void seed_state(const int *included, int included_count) {
    // ENTROPY 0, NOT RANDOM: every block here is compressed, and the writer legitimately
    // REFUSES a block whose compressed form would overflow staging -- which random fill
    // guarantees for the 1 329 120-byte player-data block. Real game state is highly
    // compressible; incompressible fill would be testing the refusal, not the driver.
    fill_pattern(g_pool, g_space.used, 0x5A1E5EEDu, 0);
    // Every planet out of the current system by default...
    uint32_t cur = 0x11223344u;
    std::memcpy(g_space.find(tbl::CURRENT_SYSTEM_ADDR, 4), &cur, 4);
    for (int32_t i = 0; i < tbl::MEMBER_LIMIT; ++i) {
        const uint32_t other = 0x77777777u;
        std::memcpy(g_space.find(tbl::PLANETS_BASE + i * tbl::PLANET_STRIDE + tbl::PLANET_SYSTEM_OFF, 4),
                    &other, 4);
        const uint32_t zero = 0;
        std::memcpy(g_space.find(tbl::PLANET_STATUS_ADDR + i * 4, 4), &zero, 4);
    }
    const uint32_t none = 0xffffffffu;
    std::memcpy(g_space.find(tbl::PLANET_INDEX_ADDR, 4), &none, 4);
    // TWO PLANETS THAT SEPARATE THE PREDICATE'S CLAUSES, without which two mutations are
    // unobservable. Planet 0 satisfies the whole predicate and is excluded ONLY because the scan
    // starts at 1; planet 5 is in the current system but has status 0 and is not the current planet,
    // so it is excluded only by the second clause.
    {
        const uint32_t live = 1;
        std::memcpy(g_space.find(tbl::PLANETS_BASE + 0 * tbl::PLANET_STRIDE + tbl::PLANET_SYSTEM_OFF, 4),
                    &cur, 4);
        std::memcpy(g_space.find(tbl::PLANET_STATUS_ADDR + 0 * 4, 4), &live, 4);
        std::memcpy(g_space.find(tbl::PLANETS_BASE + 5 * tbl::PLANET_STRIDE + tbl::PLANET_SYSTEM_OFF, 4),
                    &cur, 4);
    }
    // ...and exactly these in it.
    for (int k = 0; k < included_count; ++k) {
        const int32_t i = included[k];
        std::memcpy(g_space.find(tbl::PLANETS_BASE + i * tbl::PLANET_STRIDE + tbl::PLANET_SYSTEM_OFF, 4),
                    &cur, 4);
        const uint32_t live = 1;
        std::memcpy(g_space.find(tbl::PLANET_STATUS_ADDR + i * 4, 4), &live, 4);
    }
    const uint16_t slice_word = 69; // measured in all 41 real saves; see the progress test below
    std::memcpy(g_space.find(tbl::PROGRESS_SLICE_WORD, 2), &slice_word, 2);
}

bool state_matches(const tbl::step *steps, int count) {
    for (int i = 0; i < count; ++i) {
        if (steps[i].kind != tbl::STEP_BLOCK || steps[i].size == 0 || steps[i].discard) continue;
        const unsigned char *now = g_space.find(steps[i].addr, steps[i].size);
        if (now == nullptr) return false;
        const uint32_t off = static_cast<uint32_t>(now - g_pool);
        if (std::memcmp(now, g_pool_snapshot + off, steps[i].size) != 0) {
            printf("      state differs in %s (%u bytes @ 0x%08x)\n", steps[i].name, steps[i].size,
                   steps[i].addr);
            return false;
        }
    }
    return true;
}

// A well-formed media trailer: one real block of MEDIA_BLOCK_BYTES, produced by the block layer.
uint32_t make_media_block(unsigned char *dst, uint32_t cap) {
    unsigned char payload[tbl::MEDIA_BLOCK_BYTES];
    fill_pattern(payload, sizeof payload, 0xCD70C1u, 1);
    mem_file out{dst, cap, 0, 0, false};
    block_io io{mem_write, mem_read, &out};
    if (mh::save::write_block(io, ws(), payload, sizeof payload) != 0) return 0;
    return out.len;
}

// --------------------------------------------------------------------------------------------
void test_container_round_trips_synthetically() {
    build_space();
    const int included[] = {3, 7};
    seed_state(included, 2);
    std::memcpy(g_pool_snapshot, g_pool, g_space.used);

    container_image img{};
    std::memcpy(img.header, g_vt.header_for(5), VERSION_STRING_LEN);
    img.version      = 5;
    img.member_count = 2;
    static unsigned char m0[9000], m1[13000];
    // Members are copied VERBATIM, never compressed, so entropy is free here.
    fill_pattern(m0, sizeof m0, 0xA11CE, 2);
    fill_pattern(m1, sizeof m1, 0xB0B, 2);
    img.members[0].planet = 3;
    img.members[0].length = sizeof m0;
    img.members[0].bytes  = m0;
    img.members[1].planet = 7;
    img.members[1].length = sizeof m1;
    img.members[1].bytes  = m1;
    static unsigned char media[8192];
    img.tail_len = make_media_block(media, sizeof media);
    img.tail     = media;
    check("SVD: the synthetic media trailer is a real block", img.tail_len > 8);

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    g_arena        = fresh_arena();
    e.mem          = &g_arena;
    check("SVD: save_container writes a whole container", mh::save::save_container(e, img) == 0);
    check("SVD: it wrote every non-legacy block once", st.blocks_written == 19);
    check("SVD: and both members", st.members == 2);

    // Wipe state, then read the file back into it.
    std::memset(g_pool, 0xAA, g_space.used);
    mem_file        in{g_out1, out.len, out.len, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&in, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    container_image img2{};
    const int       rc = mh::save::load_container(e2, &img2);
    check("SVD: load_container reads it back", rc == 0);
    if (rc != 0) return; // nothing below is meaningful, and some of it would fault
    check("SVD: consuming the file to EXACT EOF", in.pos == out.len);
    check("SVD: the version slot is 5", st2.version == 5);
    check("SVD: STATE round-trips bit-for-bit through the file", state_matches(tbl::CONTAINER,
                                                                               tbl::CONTAINER_STEPS));
    check("SVD: no legacy block was read at version 5",
          st2.blocks_discarded == 0 && st2.legacy_steps_selected == 0);
    check("SVD: it read exactly what was written", st2.blocks_read == st.blocks_written);

    // THE MEMBER COUNT IS IMPLICIT -- nothing in the file records it. This is the assertion that the
    // predicate recomputed it correctly, and the tail check below is what stops an under-count from
    // hiding in the opaque trailer.
    check("SVD: the implicit member predicate recovered both members", img2.member_count == 2);
    check("SVD: with the right planet indices and lengths",
          img2.members[0].planet == 3 && img2.members[0].length == sizeof m0 &&
              img2.members[1].planet == 7 && img2.members[1].length == sizeof m1);
    check("SVD: and the right bytes",
          std::memcmp(img2.members[0].bytes, m0, sizeof m0) == 0 &&
              std::memcmp(img2.members[1].bytes, m1, sizeof m1) == 0);

    // THE TAIL IS EXACTLY ONE MEDIA BLOCK. Without this, a predicate that selected too FEW members
    // would leave the surplus member bytes in the opaque tail and a verbatim re-emit would still be
    // byte-identical -- i.e. the headline oracle alone cannot see an under-counting predicate.
    block_header th{};
    if (img2.tail != nullptr && img2.tail_len >= sizeof th) std::memcpy(&th, img2.tail, sizeof th);
    check("SVD: the trailer is exactly one well-formed block",
          img2.tail != nullptr && img2.tail_len == 8u + th.compressed_len &&
              th.uncompressed_size == tbl::MEDIA_BLOCK_BYTES);

    mem_file        out2{g_out2, OUT_BYTES, 0, 0, false};
    driver_stats    st3{};
    driver_env      e3 = env_for(&out2, &st3);
    mh::save::arena a3 = fresh_arena();
    e3.mem             = &a3;
    check("SVD: and re-saves", mh::save::save_container(e3, img2) == 0);
    check("SVD: BYTE-IDENTICALLY",
          out2.len == out.len && std::memcmp(g_out2, g_out1, out.len) == 0);
}

void test_the_trailer_capture_survives_a_large_appended_extension() {
    // Batch C appends capacity AFTER the last vanilla byte, and the driver's trailer capture is what
    // has to carry it across a load/save cycle unread. A ~20 KB extension also makes the capture loop's
    // multi-chunk path observable: with only the 0x400 media block (231 bytes compressed) a
    // single-chunk read is indistinguishable from a correct one.
    build_space();
    const int included[] = {3};
    seed_state(included, 1);

    container_image img{};
    std::memcpy(img.header, g_vt.header_for(5), VERSION_STRING_LEN);
    img.version      = 5;
    img.member_count = 1;
    static unsigned char m0[777];
    fill_pattern(m0, sizeof m0, 0x77777, 2);
    img.members[0].planet = 3;
    img.members[0].length = sizeof m0;
    img.members[0].bytes  = m0;

    // The trailer: the media block, then an extension member, built with the real batch-C writer.
    static unsigned char trailer[40000];
    const uint32_t       media_len = make_media_block(trailer, sizeof trailer);
    static unsigned char payload[20000];
    fill_pattern(payload, sizeof payload, 0xE47E45, 2);
    mem_file ext_out{trailer, sizeof trailer, media_len, 0, false};
    block_io ext_io{mem_write, mem_read, &ext_out};
    check("SVD: an extension appends after the media block",
          mh::save::write_extension(ext_io, mh::save::EXT_TAG_UNITS, payload, sizeof payload) == 0);
    img.tail     = trailer;
    img.tail_len = ext_out.len;
    check("SVD: the trailer is now several chunks long", img.tail_len > 4096u);

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    g_arena        = fresh_arena();
    e.mem          = &g_arena;
    check("SVD: a container with an appended extension is written",
          mh::save::save_container(e, img) == 0);

    mem_file        in{g_out1, out.len, out.len, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&in, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    container_image img2{};
    const int       rc = mh::save::load_container(e2, &img2);
    check("SVD: and read back", rc == 0);
    if (rc != 0) return;
    check("SVD: with the WHOLE trailer captured, media block and extension alike",
          img2.tail_len == img.tail_len && img2.tail != nullptr &&
              std::memcmp(img2.tail, trailer, img.tail_len) == 0);

    mem_file        out2{g_out2, OUT_BYTES, 0, 0, false};
    driver_stats    st3{};
    driver_env      e3 = env_for(&out2, &st3);
    mh::save::arena a3 = fresh_arena();
    e3.mem             = &a3;
    check("SVD: and re-saved", mh::save::save_container(e3, img2) == 0);
    check("SVD: BYTE-IDENTICALLY, extension included",
          out2.len == out.len && std::memcmp(g_out2, g_out1, out.len) == 0);
}

void test_a_predicate_disagreement_is_refused() {
    // The member count is derived, so state and image can disagree -- and that must fail loudly
    // rather than write a file whose member list does not match its own predicate.
    build_space();
    const int included[] = {3, 7};
    seed_state(included, 2);
    container_image img{};
    std::memcpy(img.header, g_vt.header_for(5), VERSION_STRING_LEN);
    img.version      = 5;
    img.member_count = 2;
    static unsigned char m0[64], m1[64];
    img.members[0].planet = 3;
    img.members[0].length = sizeof m0;
    img.members[0].bytes  = m0;
    img.members[1].planet = 7;
    img.members[1].length = sizeof m1;
    img.members[1].bytes  = m1;
    static unsigned char media[8192];
    img.tail_len = make_media_block(media, sizeof media);
    img.tail     = media;

    const uint32_t elsewhere = 0x66666666u;
    std::memcpy(g_space.find(tbl::PLANETS_BASE + 7 * tbl::PLANET_STRIDE + tbl::PLANET_SYSTEM_OFF, 4),
                &elsewhere, 4);
    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    g_arena        = fresh_arena();
    e.mem          = &g_arena;
    check("SVD: a save whose state no longer selects the image's members is REFUSED",
          mh::save::save_container(e, img) != 0);
}

void test_a_legacy_header_takes_the_compat_path() {
    // A file stamped with an OLDER version string must select the legacy read steps. It cannot then
    // succeed -- this build's writer never emits those blocks, so the extra read lands on the next
    // block's header and is refused -- but "the gate fired" and "the gate never fired" must not look
    // the same, which is what legacy_steps_selected separates.
    build_space();
    const int included[] = {3};
    seed_state(included, 1);
    container_image img{};
    std::memcpy(img.header, g_vt.header_for(5), VERSION_STRING_LEN);
    img.version      = 5;
    img.member_count = 1;
    static unsigned char m0[512];
    img.members[0].planet = 3;
    img.members[0].length = sizeof m0;
    img.members[0].bytes  = m0;
    static unsigned char media[8192];
    img.tail_len = make_media_block(media, sizeof media);
    img.tail     = media;

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    g_arena        = fresh_arena();
    e.mem          = &g_arena;
    check("SVD: a modern container is written", mh::save::save_container(e, img) == 0);

    // Re-stamp it as slot 1 -- an Extermination 1.0 save -- and read it back.
    std::memcpy(g_out1, g_vt.header_for(1), VERSION_STRING_LEN);
    mem_file        in{g_out1, out.len, out.len, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&in, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    container_image img2{};
    const int       rc = mh::save::load_container(e2, &img2);
    check("SVD: a slot-1 header is ACCEPTED by the gate (it is a known version)", st2.version == 1);
    // ONE, not two: the ver<2 step is selected first and its read is refused, so the walk returns
    // before reaching the ver<4 step. Pinning the number that actually happens rather than the one
    // that sounds right is the difference between an assertion and a guess.
    check("SVD: and selects the ver<2 legacy read step", st2.legacy_steps_selected == 1);
    check("SVD: but the file has no legacy block, so the load is REFUSED not misparsed", rc != 0);
    check("SVD: refused after the three blocks that precede the first gate", st2.blocks_read == 3);
}

void test_a_region_count_that_disagrees_with_state_is_refused() {
    // G_LAST_MAP_INDEX is written from STATE while the records come from the linked list, and the
    // original never checks that the two agree -- a list longer or shorter than N desynchronises the
    // file from that block onward. Ours refuses.
    build_space();
    const int included[] = {3};
    seed_state(included, 1);
    uint32_t n = 4;
    std::memcpy(g_space.find(tbl::REGION_COUNT_ADDR, 4), &n, 4);

    g_arena = fresh_arena();
    planet_image img{};
    img.progress_slice  = 3u * 69u;
    img.regions.nodes   = 3; // the image disagrees with state
    img.regions.records = static_cast<uint8_t *>(
        mh::save::arena_alloc(g_arena, 4 * tbl::REGION_RECORD_BYTES));
    img.regions.grid_ids =
        static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_GRID_IDS_BYTES));
    img.regions.terrain =
        static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_TERRAIN_BYTES));
    std::memset(img.regions.records, 0, 4 * tbl::REGION_RECORD_BYTES);
    std::memset(img.regions.grid_ids, 0, tbl::REGION_GRID_IDS_BYTES);
    std::memset(img.regions.terrain, 0, tbl::REGION_TERRAIN_BYTES);

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    e.mem          = &g_arena;
    check("SVD: a region count that disagrees with the record list is REFUSED",
          mh::save::save_planet(e, img) != 0);
}

void test_planet_round_trips_synthetically() {
    build_space();
    const int included[] = {3};
    seed_state(included, 1);

    const uint32_t nodes = 5;
    std::memcpy(g_space.find(tbl::REGION_COUNT_ADDR, 4), &nodes, 4);
    std::memcpy(g_pool_snapshot, g_pool, g_space.used);

    g_arena = fresh_arena();
    planet_image img{};
    img.progress_slice = 3u * 69u;
    img.regions.nodes  = nodes;
    img.regions.records =
        static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, nodes * tbl::REGION_RECORD_BYTES));
    img.regions.grid_ids = static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_GRID_IDS_BYTES));
    img.regions.terrain  = static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_TERRAIN_BYTES));
    fill_pattern(img.regions.records, nodes * tbl::REGION_RECORD_BYTES, 0x2EC10u, 0);
    fill_pattern(img.regions.grid_ids, tbl::REGION_GRID_IDS_BYTES, 0x6217, 0);
    fill_pattern(img.regions.terrain, tbl::REGION_TERRAIN_BYTES, 0x7E44, 0);

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    e.mem          = &g_arena;
    check("SVD: save_planet writes a whole planet file", mh::save::save_planet(e, img) == 0);
    check("SVD: the region graph emitted N records from the file's own count", st.region_nodes == nodes);
    check("SVD: the progress slice is 3 x the config word", st.progress_slice == 207);

    std::memset(g_pool, 0xAA, g_space.used);
    // The two derived quantities are NOT in the blocks: N is (it is the fourth region header block),
    // but the progress slice word is config. Re-seed it exactly as a real load would have it.
    const uint16_t slice_word = 69;
    std::memcpy(g_space.find(tbl::PROGRESS_SLICE_WORD, 2), &slice_word, 2);

    mem_file        in{g_out1, out.len, out.len, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&in, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    planet_image img2{};
    const int    prc = mh::save::load_planet(e2, &img2);
    check("SVD: load_planet reads it back", prc == 0);
    if (prc != 0) return;
    check("SVD: consuming the planet file to EXACT EOF", in.pos == out.len);
    check("SVD: N came back from the file", img2.regions.nodes == nodes && st2.region_nodes == nodes);
    check("SVD: the eight progress blocks were consumed at the derived size", st2.progress_slice == 207);
    check("SVD: it read exactly what was written", st2.blocks_read == st.blocks_written);

    mem_file        out2{g_out2, OUT_BYTES, 0, 0, false};
    driver_stats    st3{};
    driver_env      e3 = env_for(&out2, &st3);
    mh::save::arena a3 = fresh_arena();
    e3.mem             = &a3;
    check("SVD: and re-saves the planet file", mh::save::save_planet(e3, img2) == 0);
    check("SVD: BYTE-IDENTICALLY", out2.len == out.len && std::memcmp(g_out2, g_out1, out.len) == 0);
}

void test_a_wrong_progress_slice_is_refused_not_misparsed() {
    // The one size in the format that comes from CONFIG rather than from the file or an immediate.
    // A build whose config disagrees frames eight blocks differently -- and because the block header
    // carries the uncompressed size, the block layer REFUSES rather than misparsing. That refusal is
    // the only thing standing between a config change and a corrupted load.
    build_space();
    const int included[] = {3};
    seed_state(included, 1);
    const uint32_t nodes = 2;
    std::memcpy(g_space.find(tbl::REGION_COUNT_ADDR, 4), &nodes, 4);

    g_arena = fresh_arena();
    planet_image img{};
    img.progress_slice = 3u * 69u;
    img.regions.nodes  = nodes;
    img.regions.records =
        static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, nodes * tbl::REGION_RECORD_BYTES));
    img.regions.grid_ids = static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_GRID_IDS_BYTES));
    img.regions.terrain  = static_cast<uint8_t *>(mh::save::arena_alloc(g_arena, tbl::REGION_TERRAIN_BYTES));
    std::memset(img.regions.records, 0, nodes * tbl::REGION_RECORD_BYTES);
    std::memset(img.regions.grid_ids, 0, tbl::REGION_GRID_IDS_BYTES);
    std::memset(img.regions.terrain, 0, tbl::REGION_TERRAIN_BYTES);

    mem_file     out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats st{};
    driver_env   e = env_for(&out, &st);
    e.mem          = &g_arena;
    check("SVD: a planet file written with slice word 69", mh::save::save_planet(e, img) == 0);

    const uint16_t other = 70;
    std::memcpy(g_space.find(tbl::PROGRESS_SLICE_WORD, 2), &other, 2);
    mem_file        in{g_out1, out.len, out.len, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&in, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    planet_image img2{};
    check("SVD: is REFUSED by a build whose config word differs, not misparsed",
          mh::save::load_planet(e2, &img2) != 0);
}

// --------------------------------------------------------------------------------------------
// The region graph's node <-> record marshalling, and the reversal it hides.
constexpr uint32_t MARSHAL_NODES = 6;
constexpr uint32_t NEIGHBOURS    = 3;
unsigned char      g_recs[MARSHAL_NODES * tbl::REGION_RECORD_BYTES];
unsigned char      g_recs2[MARSHAL_NODES * tbl::REGION_RECORD_BYTES];
unsigned char      g_nodes[MARSHAL_NODES * tbl::REGION_NODE_BYTES];
unsigned char     *g_by_index[64];

void build_records(unsigned char *recs, uint32_t count, uint32_t neighbours) {
    std::memset(recs, 0, count * tbl::REGION_RECORD_BYTES);
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *r  = recs + i * tbl::REGION_RECORD_BYTES;
        const uint16_t id = static_cast<uint16_t>(10 + i); // ids are not 0..N-1 in a real file
        std::memcpy(r, &id, 2);
        r[2]              = static_cast<unsigned char>(0x20 + i);
        r[3]              = static_cast<unsigned char>(0x30 + i);
        const uint32_t d4 = 0xDEAD0000u + i;
        std::memcpy(r + 4, &d4, 4);
        std::memcpy(r + tbl::REGION_NEIGHBOUR_COUNT_OFF, &neighbours, 4);
        for (uint32_t j = 0; j < neighbours; ++j) {
            const uint32_t nid = 10 + ((i + j + 1) % count);
            const uint32_t dat = 0xB0000000u + i * 16 + j;
            std::memcpy(r + tbl::REGION_NEIGHBOUR_PTRS_OFF + j * 4, &nid, 4);
            std::memcpy(r + tbl::REGION_NEIGHBOUR_DATA_OFF + j * 4, &dat, 4);
        }
    }
}

void test_region_marshalling_reverses_the_list() {
    build_records(g_recs, MARSHAL_NODES, NEIGHBOURS);
    std::memset(g_by_index, 0, sizeof g_by_index);
    int32_t head = -2;
    check("SVD: records unmarshal into nodes",
          mh::save::unmarshal_region_records(g_recs, MARSHAL_NODES, g_nodes, g_by_index,
                                             64, &head) == 0);
    // THE FINDING. llm_map_load_regions pushes each node onto the head of the list, so the head is the
    // LAST record in the file and the list order is the reverse of the file order.
    check("SVD: the reconstructed list head is the LAST record, not the first",
          head == static_cast<int32_t>(MARSHAL_NODES) - 1);

    std::memset(g_recs2, 0, sizeof g_recs2);
    uint32_t walked = 0;
    check("SVD: nodes marshal back into records",
          mh::save::marshal_region_records(g_nodes, head, g_recs2, MARSHAL_NODES, &walked) == 0);
    check("SVD: walking exactly N nodes", walked == MARSHAL_NODES);

    bool reversed = true, identical = true;
    for (uint32_t i = 0; i < MARSHAL_NODES; ++i) {
        const unsigned char *a = g_recs2 + i * tbl::REGION_RECORD_BYTES;
        const unsigned char *b = g_recs + (MARSHAL_NODES - 1 - i) * tbl::REGION_RECORD_BYTES;
        if (std::memcmp(a, b, tbl::REGION_RECORD_BYTES) != 0) reversed = false;
        if (std::memcmp(a, g_recs + i * tbl::REGION_RECORD_BYTES, tbl::REGION_RECORD_BYTES) != 0)
            identical = false;
    }
    // Stated as BOTH directions so neither can pass vacuously: the re-marshalled records must be the
    // exact reverse, and must NOT be in the original order.
    check("SVD: a save/load/save cycle emits the region records in REVERSE file order", reversed);
    check("SVD: i.e. the original is NOT byte-stable across a cycle here", !identical);
}

void test_region_marshalling_carries_stale_neighbour_slots() {
    // THE LATENT DEFECT. llm_map_save_regions fills ONE 0x40c stack buffer per node and refills only
    // `count` neighbour slots, so a node with fewer neighbours than its predecessor writes the
    // PREDECESSOR'S values into the file for the slots it does not cover (and uninitialised stack for
    // the first node). Reproduced deliberately; asserted here so it cannot be "cleaned up" by accident.
    build_records(g_recs, 2, 3);
    // Make the SECOND record -- which the reversed walk emits FIRST -- carry three neighbours, and the
    // first record only one, so the file's second record inherits two slots it never wrote.
    const uint32_t one = 1;
    std::memcpy(g_recs + tbl::REGION_NEIGHBOUR_COUNT_OFF, &one, 4);

    std::memset(g_by_index, 0, sizeof g_by_index);
    int32_t head = -2;
    check("SVD: two records unmarshal",
          mh::save::unmarshal_region_records(g_recs, 2, g_nodes, g_by_index, 64, &head) == 0);
    std::memset(g_recs2, 0, sizeof g_recs2);
    uint32_t walked = 0;
    check("SVD: and marshal back",
          mh::save::marshal_region_records(g_nodes, head, g_recs2, 2, &walked) == 0 && walked == 2);

    uint32_t slot1 = 0, slot2 = 0, from1 = 0, from2 = 0;
    std::memcpy(&slot1, g_recs2 + tbl::REGION_RECORD_BYTES + tbl::REGION_NEIGHBOUR_PTRS_OFF + 4, 4);
    std::memcpy(&slot2, g_recs2 + tbl::REGION_RECORD_BYTES + tbl::REGION_NEIGHBOUR_PTRS_OFF + 8, 4);
    std::memcpy(&from1, g_recs2 + tbl::REGION_NEIGHBOUR_PTRS_OFF + 4, 4);
    std::memcpy(&from2, g_recs2 + tbl::REGION_NEIGHBOUR_PTRS_OFF + 8, 4);
    check("SVD: a short node's uncovered neighbour slots carry the PREVIOUS node's bytes",
          slot1 == from1 && slot2 == from2 && from1 != 0);
}

void test_more_nodes_than_the_header_claims_is_refused() {
    // The original writes every node in the list and the loader reads only G_LAST_MAP_INDEX of them,
    // so a list longer than the count desynchronises the file from that block onward. Ours refuses.
    build_records(g_recs, 4, 2);
    std::memset(g_by_index, 0, sizeof g_by_index);
    int32_t head = -2;
    mh::save::unmarshal_region_records(g_recs, 4, g_nodes, g_by_index, 64, &head);
    uint32_t walked = 0;
    check("SVD: a list longer than the header's N is REFUSED rather than truncated",
          mh::save::marshal_region_records(g_nodes, head, g_recs2, 3, &walked) != 0 && walked == 3);
}

// --------------------------------------------------------------------------------------------
// The opt-in half: a REAL save file, loaded into heap slabs through the table and written back out.
int run_over_real_save_via_drivers(const char *path) {
    FILE *fh = std::fopen(path, "rb");
    if (fh == nullptr) {
        printf("  cannot open %s\n", path);
        return 1;
    }
    const size_t n = std::fread(g_savefile, 1, sizeof g_savefile, fh);
    std::fclose(fh);

    build_space();
    std::memset(g_pool, 0, g_space.used);
    // The progress slice word is CONFIG, not file content, so a real load needs it seeded exactly as
    // the running game would have it. 69 in all 41 saves on disk; a wrong value makes the load refuse.
    const uint16_t slice_word = 69;
    std::memcpy(g_space.find(tbl::PROGRESS_SLICE_WORD, 2), &slice_word, 2);

    mem_file        in{g_savefile, static_cast<uint32_t>(n), static_cast<uint32_t>(n), 0, false};
    driver_stats    st{};
    driver_env      e = env_for(&in, &st);
    mh::save::arena a = fresh_arena();
    e.mem             = &a;
    container_image img{};
    const int       rc = mh::save::load_container(e, &img);

    mem_file        out{g_out1, OUT_BYTES, 0, 0, false};
    driver_stats    st2{};
    driver_env      e2 = env_for(&out, &st2);
    mh::save::arena a2 = fresh_arena();
    e2.mem             = &a2;
    const int rc2      = rc == 0 ? mh::save::save_container(e2, img) : 1;

    block_header th{};
    if (img.tail_len >= sizeof th) std::memcpy(&th, img.tail, sizeof th);
    const bool tail_ok =
        img.tail_len == 8u + th.compressed_len && th.uncompressed_size == tbl::MEDIA_BLOCK_BYTES;
    const bool eof_ok = in.pos == n;
    const bool same   = rc2 == 0 && out.len == n && std::memcmp(g_out1, g_savefile, n) == 0;

    printf("  %-24s %8zu bytes  ver=%d  blocks=%u  members=%d (%u bytes)  tail=%u  %s\n",
           path, n, st.version, st.blocks_read, img.member_count, st.member_bytes, img.tail_len,
           (rc == 0 && eof_ok && tail_ok && same) ? "ROUND-TRIP IDENTICAL" : "*** MISMATCH");
    if (rc != 0) printf("      load failed\n");
    if (!eof_ok) printf("      did not consume to EOF: %u of %zu\n", in.pos, n);
    if (!tail_ok) printf("      trailer is not exactly one 0x400 media block\n");
    if (!same) printf("      re-save differs (%u vs %zu bytes)\n", out.len, n);
    int bad = (rc == 0 && eof_ok && tail_ok && same) ? 0 : 1;

    // AND EACH EMBEDDED MEMBER THROUGH THE PLANET DRIVER. A member IS a save%02d.dat, so this walks
    // the 47-step PLANET table over real data -- the real region graph (whose N comes from the file)
    // and the real progress slice. It runs AFTER the container comparison because the two tables share
    // state (the container's `progress` block is the planet's eight slices), so loading a planet would
    // otherwise overwrite what the container re-save is about to emit.
    for (int m = 0; m < img.member_count && rc == 0; ++m) {
        static unsigned char member_copy[OUT_BYTES];
        if (img.members[m].length > sizeof member_copy) continue;
        std::memcpy(member_copy, img.members[m].bytes, img.members[m].length);

        build_space();
        std::memset(g_pool, 0, g_space.used);
        std::memcpy(g_space.find(tbl::PROGRESS_SLICE_WORD, 2), &slice_word, 2);

        mem_file        pin{member_copy, img.members[m].length, img.members[m].length, 0, false};
        driver_stats    pst{};
        driver_env      pe = env_for(&pin, &pst);
        mh::save::arena pa = fresh_arena2();
        pe.mem             = &pa;
        planet_image pimg{};
        const int    prc = mh::save::load_planet(pe, &pimg);

        mem_file        pout{g_out2, OUT_BYTES, 0, 0, false};
        driver_stats    pst2{};
        driver_env      pe2 = env_for(&pout, &pst2);
        mh::save::arena pa2 = fresh_arena2();
        pe2.mem             = &pa2;
        const int  prc2     = prc == 0 ? mh::save::save_planet(pe2, pimg) : 1;
        const bool psame    = prc2 == 0 && pout.len == img.members[m].length &&
                           std::memcmp(g_out2, member_copy, pout.len) == 0;
        const bool peof = pin.pos == img.members[m].length;
        printf("      member %d (planet %2d) %8u bytes  blocks=%u  regions N=%u  slice=%u  %s\n", m,
               img.members[m].planet, img.members[m].length, pst.blocks_read, pst.region_nodes,
               pst.progress_slice, (prc == 0 && peof && psame) ? "IDENTICAL" : "*** MISMATCH");
        if (!(prc == 0 && peof && psame)) bad = 1;
    }
    return bad;
}

// ---- SB-HOSTFREE: a spanned run follows ITS OWN region ---------------------------------------
//
// `_G_LLM_GAME_SESSION_MODE`'s save block is 1623 bytes over FIFTY registry regions. Under a
// relocating host each must be read at its own live base; reading them at the block's HOST is the
// failure the decomposition removes, and it is SILENT -- those bytes are read only by the save
// format, so no determinism run can see it (dead-ends G135).
//
// PROVEN BY THREE FILES, WITHOUT PARSING ANY OF THEM. Save once with nothing relocated (A). Save
// again with one spanned region answered out of a separate buffer holding the SAME bytes (B): B must
// equal A, or the decomposition is not byte-transparent. Then change ONE byte in that buffer and save
// again (C): C must differ from A, or the gather never read the relocated buffer at all. The middle
// arm is what stops the third passing for a trivial reason.
void test_a_spanned_run_follows_its_own_region() {
    build_space();
    const int included[] = {3, 7};
    seed_state(included, 2);

    const tbl::sliced_block *blk = nullptr;
    for (int i = 0; i < tbl::SLICED_BLOCK_COUNT; ++i)
        if (tbl::SLICED_BLOCKS[i].size == 1623) blk = &tbl::SLICED_BLOCKS[i];
    check("SB-HOSTFREE: the 1623-byte SESSION_MODE block is decomposed", blk != nullptr);
    if (blk == nullptr) return;

    // The first region run PAST the host -- the one a single translate() would have got wrong.
    const tbl::block_slice *run = nullptr;
    for (int i = 1; i < blk->count && run == nullptr; ++i)
        if (tbl::BLOCK_SLICES[blk->first + i].rid != tbl::SLICE_GAP)
            run = &tbl::BLOCK_SLICES[blk->first + i];
    check("SB-HOSTFREE: it has a region run past its host -- the point of decomposing it",
          run != nullptr);
    if (run == nullptr) return;

    // The image the writer expects: `seed_state` makes planets 3 and 7 pass member_included, so a
    // zero-member image is refused before a single block is written -- which is how the first version
    // of this arm managed to fail all three saves for a reason that had nothing to do with slicing.
    container_image img{};
    std::memcpy(img.header, g_vt.header_for(5), VERSION_STRING_LEN);
    img.version      = 5;
    img.member_count = 2;
    static unsigned char sm0[512], sm1[512];
    fill_pattern(sm0, sizeof sm0, 0x5117E, 2);
    fill_pattern(sm1, sizeof sm1, 0x5118E, 2);
    img.members[0].planet = 3;
    img.members[0].length = sizeof sm0;
    img.members[0].bytes  = sm0;
    img.members[1].planet = 7;
    img.members[1].length = sizeof sm1;
    img.members[1].bytes  = sm1;
    static unsigned char smedia[8192];
    img.tail_len = make_media_block(smedia, sizeof smedia);
    img.tail     = smedia;

    auto save_once = [&](unsigned char *dst, uint32_t *len) {
        mem_file     out{dst, OUT_BYTES, 0, 0, false};
        driver_stats st{};
        driver_env   e = env_for(&out, &st);
        g_arena        = fresh_arena();
        e.mem          = &g_arena;
        const int rc   = mh::save::save_container(e, img);
        *len           = out.len;
        return rc;
    };

    static unsigned char fa[OUT_BYTES], fb[OUT_BYTES];
    uint32_t             la = 0, lb = 0;
    check("SB-HOSTFREE: control save (nothing relocated)", save_once(fa, &la) == 0);

    // B: the same bytes, answered from somewhere else.
    const unsigned char *live =
        static_cast<const unsigned char *>(g_space.find(run->stock, run->len));
    check("SB-HOSTFREE: fixture: the run's bytes are in the pool", live != nullptr);
    if (live == nullptr) return;
    std::memset(g_moved_buf, 0, sizeof g_moved_buf);
    std::memcpy(g_moved_buf + run->off, live, run->len);
    g_moved_rid = static_cast<mh::state::region_id>(run->rid);
    check("SB-HOSTFREE: relocated save with IDENTICAL bytes", save_once(fb, &lb) == 0);
    check("SB-HOSTFREE: ...produces a byte-identical file -- the decomposition is transparent",
          la == lb && std::memcmp(fa, fb, la) == 0);

    // C: one byte different in the relocated buffer.
    g_moved_buf[run->off] = static_cast<unsigned char>(g_moved_buf[run->off] ^ 0x5c);
    check("SB-HOSTFREE: relocated save with one byte CHANGED", save_once(fb, &lb) == 0);
    check("SB-HOSTFREE: ...changes the file -- so the run really was gathered from the RELOCATED "
          "region and not from the block's host, which is the silent corruption G135 records",
          !(la == lb && std::memcmp(fa, fb, la) == 0));

    g_moved_rid = mh::state::RID_COUNT;
}

void run_all() {
    test_container_round_trips_synthetically();
    test_a_spanned_run_follows_its_own_region();
    test_the_trailer_capture_survives_a_large_appended_extension();
    test_a_predicate_disagreement_is_refused();
    test_a_legacy_header_takes_the_compat_path();
    test_a_region_count_that_disagrees_with_state_is_refused();
    test_planet_round_trips_synthetically();
    test_a_wrong_progress_slice_is_refused_not_misparsed();
    test_region_marshalling_reverses_the_list();
    test_region_marshalling_carries_stale_neighbour_slots();
    test_more_nodes_than_the_header_claims_is_refused();
}

} // namespace drivers

int run_over_real_save(const char *path) {
    FILE *fh = std::fopen(path, "rb");
    if (fh == nullptr) {
        printf("  cannot open %s\n", path);
        return 1;
    }
    static unsigned char file[8u << 20];
    const size_t         n = std::fread(file, 1, sizeof file, fh);
    std::fclose(fh);
    printf("  %s: %zu bytes\n", path, n);

    uint32_t off = 40, blocks = 0, identical = 0, decoded = 0;
    while (off + 20 <= n) {
        block_header h{};
        std::memcpy(&h, file + off, sizeof h);
        uint32_t magic = 0;
        std::memcpy(&magic, file + off + sizeof h, sizeof magic);
        if (h.compressed_len < 12 || off + 8 + h.compressed_len > n || magic != mh::lzw::MAGIC) {
            off += 4; // the plain u32 between sections
            continue;
        }
        ++blocks;
        mem_file in{file + off, h.compressed_len + 8u, h.compressed_len + 8u, 0, false};
        block_io io{mem_write, mem_read, &in};
        if (mh::save::read_block(io, ws(), g_dst, h.uncompressed_size) == 0) ++decoded;

        mem_file out{g_file, BIG, 0, 0, false};
        block_io out_io{mem_write, mem_read, &out};
        mh::save::write_block(out_io, ws(), g_dst, h.uncompressed_size);
        if (out.len == h.compressed_len + 8u &&
            std::memcmp(out.buf, file + off, out.len) == 0)
            ++identical;
        off += 8 + h.compressed_len;
    }
    printf("  %u blocks: %u decoded, %u re-encoded BYTE-IDENTICALLY, ended at %u of %zu\n",
           blocks, decoded, identical, off, n);
    const bool ok = blocks > 0 && decoded == blocks && identical == blocks && off == n;
    printf("  %s\n", ok ? "ALL BLOCKS IDENTICAL" : "*** MISMATCH");
    return ok ? 0 : 1;
}

} // namespace

int run_savetest(int argc, char **argv) {
    printf("=== savetest (save format: version gate + block layer, no file/no game) ===\n");
    g_checks = g_fails = 0;
    test_accepts_every_supported_slot();
    test_refuses_unknown_header();
    test_demo_slot_is_not_reachable_from_a_real_build();
    test_demo_build_path();
    test_compat_gates();
    test_last_match_wins();
    test_real_blocks_round_trip_byte_identically();
    test_fixtures_still_cover_the_codec();
    test_synthetic_round_trips();
    test_cap_raised_block();
    test_framing_refusals();
    test_lzss_fallback();
    test_short_read_goes_unreported();
    test_short_vfs_read_is_reported();
    test_truncation_with_foreign_leftovers_is_bounded();
    test_high_water_statistic();
    test_writer_refuses_what_would_overflow_staging();
    test_decoder_output_bound();
    test_prefix_cycle_is_refused_not_hung();
    test_extension_round_trips();
    test_extension_is_not_confused_with_other_trailers();
    test_appending_capacity_requires_a_refused_version();
    drivers::run_all();
    printf("%d checks, %d failures\n", g_checks, g_fails);

    // Opt-in: every named .sav is walked twice -- once block by block (batch B's codec evidence)
    // and once through the reimplemented DRIVERS, which is SV1-DRIVERS' byte-identity oracle.
    int extra = 0;
    if (argc > 2) {
        printf("--- every block of a real save ---\n");
        for (int i = 2; i < argc; ++i) extra |= run_over_real_save(argv[i]);
        printf("--- the same saves through the reimplemented drivers ---\n");
        for (int i = 2; i < argc; ++i) extra |= drivers::run_over_real_save_via_drivers(argv[i]);
    }
    return (g_fails == 0 && extra == 0) ? 0 : 1;
}
