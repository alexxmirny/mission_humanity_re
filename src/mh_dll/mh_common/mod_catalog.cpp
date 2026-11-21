//
// mod_catalog.cpp -- see include/mh_mod_catalog.h. Pure preparation: the only OS surface is the
// Win32 adapter in the anonymous namespace below, rooted entirely at the game_dir `prepare` is
// given, so the behavior suite points the exact same code at temporary trees.
//
// ORDER OF OPERATIONS (the FIRST problem wins; every one of them fails the whole active set):
//   1. parse the enabled list (ids validated, duplicates refused)
//   2. per mod, in enabled order: folder exists -> mod.ini parses and agrees with the folder ->
//      root inventory matches the folder contract -> data/, cfg/ and lang/ walked
//   3. payloads classified: validated classes format-checked; generic packed classes accepted as
//      bytes; DIRECT-file classes refused (the banki/bnk PAIR once the whole set is known)
//   4. payloads sorted by (name, mod_index); winners = last payload per name (later enabled wins
//      by construction, never by enumeration order)
//   5. digest fed piecewise through mh_net_proto's streaming SHA-256
//
#include "include/mh_mod_catalog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/mh_bnk.h"          // the bank validators this layer reuses
#include "mh_mapgen/mpm.h"           // read_mpm -- the .mpm validator (never a second header)
#include "mh_net_proto/net_crypto.h" // streaming SHA-256 (same function as the one-shot)

namespace mh::mods {

namespace {

// ---- the first failure wins --------------------------------------------------------------------

struct PrepError {
    failure     why;
    std::string msg;
    bool        config_error = false;
};
[[noreturn]] void fail(failure why, const std::string &msg, bool config_error = false) {
    throw PrepError{why, msg, config_error};
}
void check(bool ok, failure why, const std::string &msg) {
    if (!ok) fail(why, msg);
}

// ---- Win32 adapter (the only OS surface) -------------------------------------------------------

std::string join_path(const std::string &dir, std::string_view leaf) {
    std::string p = dir;
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += '\\';
    p += leaf;
    return p;
}

bool dir_exists(const std::string &path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// A junction/symlink ANYWHERE on the walked tree is refused, including the anchors themselves: a
// reparse-pointed mods/ or mod folder would let the whole tree resolve somewhere else.
bool is_reparse(const std::string &path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool read_file_bytes(const std::string &path, std::vector<std::uint8_t> &out, std::string &err) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = std::format("{}: cannot open (error {})", path, GetLastError());
        return false;
    }
    LARGE_INTEGER sz;
    bool          ok = GetFileSizeEx(h, &sz) != FALSE;
    if (!ok && err.empty()) err = std::format("{}: cannot size (error {})", path, GetLastError());
    if (ok && (sz.QuadPart < 0 || (unsigned long long)sz.QuadPart > PAYLOAD_BYTES_MAX)) {
        err = std::format("{}: {} bytes exceeds the {}-byte payload bound", path,
                          (unsigned long long)(sz.QuadPart < 0 ? 0 : sz.QuadPart),
                          (unsigned long long)PAYLOAD_BYTES_MAX);
        ok  = false;
    }
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        std::size_t done = 0;
        while (ok && done < out.size()) {
            const DWORD want = (DWORD)((out.size() - done) > 0x40000000ull ? 0x40000000ull
                                                                           : (out.size() - done));
            DWORD       got  = 0;
            ok               = ReadFile(h, out.data() + done, want, &got, nullptr) != FALSE && got == want;
            done += got;
        }
        if (!ok && err.empty()) err = std::format("{}: read failed (error {})", path, GetLastError());
    }
    CloseHandle(h);
    return ok;
}

struct DirEntry {
    std::string name;
    bool        is_dir  = false;
    bool        reparse = false;
};

// Sorted by name, so error selection and traversal are deterministic regardless of NTFS order.
bool list_dir(const std::string &dir, std::vector<DirEntry> &out, std::string &err) {
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        err = std::format("{}: cannot enumerate (error {})", dir, GetLastError());
        return false;
    }
    BOOL more = TRUE;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        DirEntry e;
        e.name    = fd.cFileName;
        e.is_dir  = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        out.push_back(e);
    } while ((more = FindNextFileA(h, &fd)) != FALSE);
    const DWORD find_err = GetLastError();
    FindClose(h);
    // A REAL enumeration error (not plain exhaustion) must never pass as a complete listing --
    // a partial set would mount without the files FindNextFile could not return.
    if (!more && find_err != ERROR_NO_MORE_FILES) {
        err = std::format("{}: enumeration failed (error {})", dir, find_err);
        return false;
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    return true;
}

// ---- text helpers ------------------------------------------------------------------------------

std::string trim(const std::string &s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

std::string ascii_lower(std::string_view s) {
    std::string r(s);
    for (char &c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return r;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Strict UTF-8: no overlongs, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char b0 = (unsigned char)s[i];
        std::size_t         n  = 1;
        std::uint32_t       cp = 0;
        unsigned            lo = 0x80, hi = 0xBF;
        if (b0 < 0x80) {
            ++i;
            continue;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            n  = 2;
            cp = b0 & 0x1F;
        } else if (b0 == 0xE0) {
            n  = 3;
            cp = 0;
            lo = 0xA0;
        } else if (b0 >= 0xE1 && b0 <= 0xEC) {
            n  = 3;
            cp = b0 & 0x0F;
        } else if (b0 == 0xED) {
            n  = 3;
            cp = 0x0D;
            hi = 0x9F; // exclude surrogates
        } else if (b0 >= 0xEE && b0 <= 0xEF) {
            n  = 3;
            cp = b0 & 0x0F;
        } else if (b0 == 0xF0) {
            n  = 4;
            cp = 0;
            lo = 0x90;
        } else if (b0 >= 0xF1 && b0 <= 0xF3) {
            n  = 4;
            cp = b0 & 0x07;
        } else if (b0 == 0xF4) {
            n  = 4;
            cp = 0x04;
            hi = 0x8F; // cap at U+10FFFF
        } else {
            return false;
        }
        if (i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k) {
            const unsigned char bk = (unsigned char)s[i + k];
            if (bk < lo || bk > hi) return false;
            cp = (cp << 6) | (bk & 0x3F);
            lo = 0x80; // later continuation bytes are plain 0x80..0xBF
            hi = 0xBF;
        }
        if (cp > 0x10FFFF) return false;
        i += n;
    }
    return true;
}

// A display field: bounded, valid UTF-8, no control CODEPOINTS. "Control" matches the launcher's
// char::is_control so both sides accept the same metadata: ASCII controls (0x00..0x1F, 0x7F) by
// byte, and the C1 range U+0080..U+009F by their only UTF-8 spelling (C2 80..C2 9F). U+00A0 and
// every other printable codepoint stay valid on both sides.
bool valid_field(std::string_view s, unsigned max_bytes) {
    if (s.empty() || s.size() > max_bytes) return false;
    if (!valid_utf8(s)) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
        if (c == 0xC2 && i + 1 < s.size()) {
            const unsigned char n = (unsigned char)s[i + 1];
            if (n >= 0x80 && n <= 0x9F) return false; // C1 controls are controls
        }
    }
    return true;
}

// The optional display field: empty is fine, present must obey the same rules.
bool valid_optional_field(std::string_view s, unsigned max_bytes) {
    return s.empty() ? valid_utf8(s) : valid_field(s, max_bytes);
}

bool valid_id(std::string_view s) {
    if (s.empty() || s.size() > ID_MAX) return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ---- the enabled list --------------------------------------------------------------------------

bool parse_enabled(const char *csv, std::vector<std::string> &out, std::string &err) {
    if (!csv || !*csv) return true;
    std::set<std::string> seen;
    std::string           cur;
    auto                  flush = [&]() -> bool {
        const std::string id = trim(cur);
        cur.clear();
        if (id.empty()) {
            err = "enabled list: empty entry (the launcher writes a canonical comma list)";
            return false;
        }
        if (!valid_id(id)) {
            err = std::format("enabled list: '{}' is not an id (lowercase [a-z0-9_-], 1..{})", id,
                                               ID_MAX);
            return false;
        }
        if (!seen.insert(id).second) {
            err = std::format("enabled list: '{}' listed twice", id);
            return false;
        }
        out.push_back(id);
        return true;
    };
    for (const char c : std::string_view(csv)) {
        if (c == ',') {
            if (!flush()) return false;
        } else {
            cur += c;
        }
    }
    return flush();
}

// ---- canonical resource names ------------------------------------------------------------------
//
// One path component: printable ASCII, not "." / "..", folded to lowercase. canon_component is the
// single definition; the public canonical_resource_name (below, namespace scope) and the data/
// traversal both go through it, so both refuse exactly the same names.

bool canon_component(std::string_view c, std::string &folded, std::string &err) {
    if (c.empty()) {
        err = "empty path component";
        return false;
    }
    if (c == "." || c == "..") {
        err = std::format("'{}' is not a resource name component", c);
        return false;
    }
    for (const unsigned char u : c) {
        if (u < 0x20 || u > 0x7E) {
            err = std::format("'{}' carries a byte outside printable ASCII", c);
            return false;
        }
    }
    folded = ascii_lower(c);
    return true;
}

// ---- mod.ini -----------------------------------------------------------------------------------

struct Manifest {
    std::string format, id, name, version, description;
};

bool parse_manifest(const std::string &path, Manifest &m, std::string &err) {
    std::vector<std::uint8_t> raw;
    if (!read_file_bytes(path, raw, err)) {
        err = "mod.ini: " + err;
        return false;
    }
    std::map<std::string, std::string, std::less<>> fields;
    if (!parse_ini_section(raw, "mod", fields, err)) return false;
    auto get      = [&](const char *key) -> std::string { auto i = fields.find(key); return i == fields.end() ? std::string() : i->second; };
    m.format      = get("format");
    m.id          = get("id");
    m.name        = get("name");
    m.version     = get("version");
    m.description = get("description");
    return true;
}
bool validate_manifest(const Manifest &m, std::string_view id, std::string &err) {
    if (m.format != "1") {
        err = std::format("mod.ini: [mod] format must be 1 (found '{}')", m.format);
        return false;
    }
    if (!valid_id(m.id)) {
        err = std::format("mod.ini: id '{}' is not an id (lowercase [a-z0-9_-], 1..{})", m.id, ID_MAX);
        return false;
    }
    if (m.id != id) {
        err = std::format("mod.ini: id '{}' does not match the folder name '{}'", m.id, id);
        return false;
    }
    if (!valid_field(m.name, NAME_MAX)) {
        err = std::format("mod.ini: name must be 1..{} bytes of UTF-8 without control bytes",
                          NAME_MAX);
        return false;
    }
    if (!valid_field(m.version, NAME_MAX)) {
        err = std::format("mod.ini: version must be 1..{} bytes of UTF-8 without control bytes",
                          NAME_MAX);
        return false;
    }
    if (!valid_optional_field(m.description, DESCRIPTION_MAX)) {
        err = std::format("mod.ini: description must be empty or at most {} bytes of UTF-8 without "
                          "control bytes",
                          DESCRIPTION_MAX);
        return false;
    }
    return true;
}

// ---- payload classes ---------------------------------------------------------------------------

std::uint32_t le32(const std::uint8_t *p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) |
           ((std::uint32_t)p[3] << 24);
}

// Tilesets have a 512-byte RGB565 palette, then 8-bit tiles at stride 1024 (32x32 planet) or
// 768 (32x24 tactical floor); the tile count is derived from the file size and the tile index is a
// 13-bit quantity, so every stride the size divides by must land in 1..8192 tiles.
void check_tlo(const std::vector<std::uint8_t> &b, std::string_view name) {
    if (b.size() <= 512)
        fail(failure::payload_format, std::format("{}: no tiles below the 512-byte palette", name));
    const std::uint64_t n          = b.size() - 512;
    bool                any_stride = false;
    for (const std::uint64_t stride : {1024ull, 768ull}) {
        if (n % stride != 0) continue;
        const std::uint64_t tiles = n / stride;
        if (tiles >= 1 && tiles <= 8192) any_stride = true;
    }
    if (!any_stride)
        fail(failure::payload_format,
             std::format("{}: tile bytes are neither 1024- nor 768-stride multiples", name));
}

// Strategic CAMPAIGN planet maps (the pack's .mp class): "ver" + version text,
// the 92-byte header at 0x05, planes totalling 7 B/tile, a trailing dword, then
// the f3-sized footer inside a [0,255]-byte slack. The catalog checks exactly that ARITHMETIC
// (plus w/h <= 1024 and f3 >= 1 so the formula cannot overflow or underflow) and invents nothing
// else: no square-only rule, no shipped-tier membership -- the retail loader remains the semantic
// authority. (The loose picker map is .mpm and goes through the real mh_mapgen parser, check_mpm
// below -- never a hand-written second header.)
void check_mp(const std::uint8_t *b, std::size_t size, std::string_view name) {
    if (!b || size < 0x61) fail(failure::payload_format, std::format("{}: truncated map header", name));
    if (std::memcmp(b, "ver", 3) != 0)
        fail(failure::payload_format, std::format("{}: missing \"ver\" magic", name));
    if (b[3] != '0' || b[4] != '2')
        fail(failure::payload_format,
             std::format("{}: map version '{}{}' is not the loader's 02", name, (char)b[3],
                         (char)b[4]));
    const std::uint32_t w = le32(b + 0x05), h = le32(b + 0x09);
    if (w == 0 || h == 0 || w > 1024 || h > 1024)
        fail(failure::payload_format,
             std::format("{}: dimensions {}x{} are outside the arithmetic bound 1..1024", name, w, h));
    const std::uint32_t f1 = le32(b + 0x35);
    const std::uint32_t f2 = le32(b + 0x39);
    const std::int32_t  f3 = (std::int32_t)le32(b + 0x41);
    if (f3 < 1)
        fail(failure::payload_format, std::format("{}: f3 (region count) {} is not a positive count",
                                                  name, f3));
    const std::uint64_t minimum = 0x61ull + 7ull * w * h + 28ull * ((std::uint64_t)f1 + f2) + 4ull +
                                  (12ull * (std::uint64_t)f3 - 4ull);
    if (size < minimum || size > minimum + 255)
        fail(failure::payload_format,
             std::format("{}: {} bytes is outside the loader's plane formula [{}, {}]", name,
                         size, minimum, minimum + 255));
}

// The loose MP/picker map: the FULL existing codec (mh_mapgen::read_mpm), not a boundary skim.
void check_mpm(const std::vector<std::uint8_t> &b, std::string_view name) {
    std::string err;
    if (!mh_mapgen::validate_mpm(b.data(), b.size(), err))
        fail(failure::payload_format, std::format("{}: {}", name, err));
}

// DIRECT-file classes: the engine reaches these OUTSIDE the pack overlay (utils_open_file's
// Msgs.dat replacement, the media seams), so a mounted claim would be a lie. Diagnosed refusal.
bool is_direct_class(std::string_view canon) {
    if (canon == "msgs.dat") return true;
    if (ends_with(canon, ".avi")) return true;
    return false;
}

bool bank_slot_of(std::string_view canon, std::uint64_t &slot) {
    if (canon.size() < 10 || canon.compare(0, 5, "bank_") != 0) return false;
    const std::size_t dot = canon.rfind(".bnk");
    if (dot == std::string::npos || dot != canon.size() - 4 || dot < 6) return false;
    std::uint64_t v = 0;
    for (std::size_t i = 5; i < dot; ++i) {
        const char c = canon[i];
        if (c < '0' || c > '9') return false;
        if (v > 100000000ull) return false; // absurd; surfaces as "unreferenced" in the pair pass
        v = v * 10 + (std::uint64_t)(c - '0');
    }
    slot = v;
    return true;
}

// Classify + validate one payload. VALIDATED classes fail loudly; every other packed class is
// accepted as plain bytes for the ordinary loader (the NAM census's .gfx/.dmp/.sam/.scr/...); a
// bank FILE only gets its name checked here -- the assembled banki/bnk PAIR is proven once the
// whole winner set is known (check_bank_pair).
void check_payload(std::string_view name, const std::vector<std::uint8_t> &b) {
    if (is_direct_class(name))
        fail(failure::unsupported_class,
             std::format("{}: a DIRECT-file class (engine reads it outside the pack overlay); "
                         "the catalog cannot honestly mount it",
                         name));
    if (name == "init.cfg" || name == "initlang.cfg" || name == "init\\init.cfg" ||
        name == "init\\initlang.cfg")
        fail(failure::reserved,
             std::format("{}: raw config overrides require the validated transaction", name), true);
    if (name == "banki.dat") {
        if (b.size() % sizeof(bnk::BankiEntry) != 0)
            fail(failure::payload_format,
                 std::format("banki.dat: size {} is not a multiple of 12", b.size()));
        return;
    }
    std::uint64_t slot;
    if (bank_slot_of(name, slot)) return; // validated in the pair pass
    if (ends_with(name, ".tlo")) {
        check_tlo(b, name);
        return;
    }
    if (ends_with(name, ".mpm")) {
        check_mpm(b, name);
        return;
    }
    if (ends_with(name, ".mp")) {
        check_mp(b.data(), b.size(), name);
        return;
    }
    // Generic packed class: bytes only, the ordinary loader parses it.
}

// The assembled pair: the winning BANKI.DAT governs every WINNING bank file. Only winners are
// pair-validated -- a fully SHADOWED bank (an earlier mod's copy under the same canonical name)
// never mounts, so its descriptor compatibility is irrelevant; its bytes still count in the
// digest. A shadowed bank whose own mod's BANKI was also shadowed is likewise just bytes.
//
// THE EXPLICIT DUTY SPLIT (a typical mod ships a full BANKI.DAT plus only CHANGED banks; untouched
// banks keep coming from the base pack). This layer, purely from winner-set bytes:
//   - every WINNING overridden bank validates against the WINNING BANKI descriptor (SplitBnk) --
//     no mismatched size reaches publication for an overridden bank;
//   - a winning bank with no descriptor / an empty descriptor behind it is a failure, not dead
//     bytes.
// What this layer CANNOT see, and the ADAPTER enforces BEFORE publish (whole-set refusal):
//   - descriptors of UN-overridden banks under a MODIFIED winning BANKI, compared against the
//     stock BANKI read on the pack side;
//   - the engine's fixed bank count and per-bank sprite budgets.
void check_bank_pair(const std::vector<Payload>                              &payloads,
                     const std::map<std::string, std::uint32_t, std::less<>> &winners) {
    const auto banki = winners.find("banki.dat");
    if (banki == winners.end()) {
        std::uint64_t dummy;
        for (const auto &w : winners)
            if (bank_slot_of(payloads[w.second].name, dummy))
                fail(failure::payload_format,
                     std::format("{}: no mounted BANKI.DAT to pair it with", payloads[w.second].name));
        return;
    }
    const std::vector<std::uint8_t> &dat = payloads[banki->second].bytes;
    const std::size_t                n   = dat.size() / sizeof(bnk::BankiEntry);
    for (const auto &w : winners) {
        const Payload &p = payloads[w.second];
        std::uint64_t  slot;
        if (!bank_slot_of(p.name, slot)) continue;
        if (slot >= n)
            fail(failure::payload_format,
                 std::format("{}: slot {} is not referenced by the mounted BANKI.DAT ({} entries)",
                             p.name, slot, n));
        bnk::BankiEntry e;
        std::memcpy(&e, dat.data() + slot * sizeof(bnk::BankiEntry), sizeof(e));
        if (bnk::IsEmpty(e))
            fail(failure::payload_format,
                 std::format("{}: BANKI.DAT entry {} is empty and mounts no bank", p.name, slot));
        try {
            bnk::SplitBnk(p.bytes, e, p.name);
        } catch (const bnk::BnkError &ex) {
            fail(failure::payload_format, std::format("{}: {}", p.name, ex.what()));
        }
    }
}

// ---- the digest --------------------------------------------------------------------------------
//
// Frame (all integers little-endian), fed piecewise -- no second multi-megabyte buffer:
//   "MHMODSDIGEST1" | u32 mod_count | per mod (enabled order):
//       u32 len id | u32 len name | u32 len version | u32 len description
//   | u32 payload_count | per payload (sorted by name, then mod_index):
//       u32 len name | u32 mod_index | u32 len bytes | bytes
// The mod side hashes the PARSED CANONICAL mod.ini fields (trimmed values, comments/whitespace
// normalisation already applied by the reader) -- two semantically identical mod.ini files hash
// the same, a different description hashes differently: plan §3 counts every accepted metadata
// byte, so cosmetic metadata differences are strict-match differences too. Locale-independent by
// construction (human metadata strings do not vary with the selected language).

void feed_u32(mh_net_proto::sha256_stream &s, std::uint32_t v) {
    const std::uint8_t b[4] = {(std::uint8_t)v, (std::uint8_t)(v >> 8), (std::uint8_t)(v >> 16),
                               (std::uint8_t)(v >> 24)};
    mh_net_proto::sha256_feed(s, b, 4);
}

void feed_str(mh_net_proto::sha256_stream &s, std::string_view x) {
    feed_u32(s, (std::uint32_t)x.size());
    mh_net_proto::sha256_feed(s, (const std::uint8_t *)x.data(), x.size());
}

void compute_digest(const std::vector<ModIdentity> &mods, const std::vector<Payload> &payloads,
                    std::uint8_t out[32]) {
    mh_net_proto::sha256_stream s;
    mh_net_proto::sha256_reset(s);
    mh_net_proto::sha256_feed(s, (const std::uint8_t *)DIGEST_MAGIC, sizeof(DIGEST_MAGIC) - 1);
    feed_u32(s, (std::uint32_t)mods.size());
    for (const ModIdentity &m : mods) {
        feed_str(s, m.id);
        feed_str(s, m.name);
        feed_str(s, m.version);
        feed_str(s, m.description);
    }
    feed_u32(s, (std::uint32_t)payloads.size());
    for (const Payload &p : payloads) {
        feed_str(s, p.name);
        feed_u32(s, p.mod_index);
        feed_u32(s, (std::uint32_t)p.bytes.size());
        mh_net_proto::sha256_feed(s, p.bytes.data(), p.bytes.size());
    }
    mh_net_proto::sha256_finish(s, out);
}

void compute_config_digest(const Snapshot &snap, const std::vector<std::uint8_t> &init, std::uint8_t out[32]) {
    mh_net_proto::sha256_stream s;
    mh_net_proto::sha256_reset(s);
    constexpr char frame[] = "MHMODSCFG1";
    mh_net_proto::sha256_feed(s, reinterpret_cast<const std::uint8_t *>(frame), sizeof(frame) - 1);
    // The pre-config digest frames every mod and every ordinary resource, including shadowed ones.
    mh_net_proto::sha256_feed(s, snap.digest, sizeof(snap.digest));
    feed_u32(s, static_cast<std::uint32_t>(snap.config_inputs.size()));
    for (const auto &p : snap.config_inputs) {
        feed_str(s, p.name);
        feed_u32(s, p.mod_index);
        feed_u32(s, static_cast<std::uint32_t>(p.bytes.size()));
        mh_net_proto::sha256_feed(s, p.bytes.data(), p.bytes.size());
    }
    feed_u32(s, static_cast<std::uint32_t>(init.size()));
    mh_net_proto::sha256_feed(s, init.data(), init.size());
    mh_net_proto::sha256_finish(s, out);
}

// ---- the data/ walk ----------------------------------------------------------------------------

struct WalkCtx {
    std::vector<Payload> payloads;
    std::vector<Payload> config_inputs;
    std::uint64_t        set_bytes    = 0;
    std::uint32_t        set_payloads = 0;
};

// `seen` is the CURRENT MOD's alias table (canonical name -> disk path already claimed in THAT
// mod): two mods shipping the same canonical name is the normal shadowing case, only files of
// ONE mod folding together are the alias failure.
void walk_data(WalkCtx &ctx, std::map<std::string, std::string> &seen, const std::string &disk_dir,
               const std::string &canon_prefix, bool config = false) {
    std::vector<DirEntry> entries;
    std::string           err;
    check(list_dir(disk_dir, entries, err), failure::io, err);
    for (const DirEntry &e : entries) {
        std::string folded;
        check(canon_component(e.name, folded, err), failure::escape,
              std::format("{}: {}", disk_dir, err));
        const std::string canon = canon_prefix.empty() ? folded : canon_prefix + "\\" + folded;
        check(canon.size() <= RESOURCE_NAME_MAX, failure::escape,
              std::format("{}: canonical name exceeds the {}-byte bound", canon,
                          RESOURCE_NAME_MAX));
        check(!e.reparse, failure::escape,
              std::format("{}: reparse point (junction/symlink) refused", canon));
        if (e.is_dir) {
            if (config) fail(failure::reserved, std::format("{}: config directories must be flat", canon));
            walk_data(ctx, seen, join_path(disk_dir, e.name), canon);
            continue;
        }
        const std::string disk_path = join_path(disk_dir, e.name);
        const auto        claimed   = seen.find(canon);
        check(claimed == seen.end(), failure::alias,
              std::format("{}: folds onto {} (case/separator aliases are the same resource)",
                          disk_path, claimed == seen.end() ? std::string() : claimed->second));
        seen.emplace(canon, disk_path);

        if (config) {
            const bool cfg_file  = canon == "cfg\\patch.cfg";
            const bool lang_file = canon.starts_with("lang\\") && ends_with(canon, ".cfg") &&
                                   canon.size() > sizeof("lang\\.cfg") - 1;
            bool locale_ok = lang_file;
            if (locale_ok) {
                for (size_t i = 5; i < canon.size() - 4; ++i) {
                    const char ch = canon[i];
                    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                        locale_ok = false;
                }
            }
            check(cfg_file || locale_ok, failure::reserved,
                  std::format("{}: expected cfg/patch.cfg or lang/<locale>.cfg", canon));
        }
        Payload p;
        p.name        = canon;
        p.source_path = disk_path;
        check(read_file_bytes(p.source_path, p.bytes, err), failure::io, err);
        check(config || !p.bytes.empty(), failure::payload_format,
              std::format("{}: empty file", p.source_path)); // no zero-byte "mounted" claims
        ctx.set_bytes += p.bytes.size();
        check(ctx.set_bytes <= SET_BYTES_MAX, failure::payload_format,
              std::format("the active set exceeds the {}-byte bound",
                          (unsigned long long)SET_BYTES_MAX));
        ++ctx.set_payloads;
        check(ctx.set_payloads <= SET_PAYLOADS_MAX, failure::payload_format,
              std::format("the active set exceeds {} payload files", SET_PAYLOADS_MAX));
        (config ? ctx.config_inputs : ctx.payloads).push_back(std::move(p));
    }
}

} // namespace

// ---- the public API ----------------------------------------------------------------------------

bool parse_ini_section(const std::vector<std::uint8_t> &raw, std::string_view section,
                       std::map<std::string, std::string, std::less<>> &fields, std::string &err) {
    std::size_t bom = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) bom = 3;
    for (std::size_t i = bom; i < raw.size(); ++i)
        if (raw[i] == 0) {
            err = "INI: NUL byte";
            return false;
        }
    const std::string text =
        raw.size() > bom ? std::string((const char *)raw.data() + bom, raw.size() - bom)
                         : std::string(); // never construct from (nullptr, 0) on an empty file
    if (!valid_utf8(text)) {
        err = "INI: not valid UTF-8";
        return false;
    }

    bool                  in_mod = false, seen_mod = false;
    std::set<std::string> seen_keys;
    std::size_t           pos = 0, line_no = 0;
    while (pos < text.size()) {
        const std::size_t eol  = text.find('\n', pos);
        const std::size_t end  = (eol == std::string::npos) ? text.size() : eol;
        std::string       line = text.substr(pos, end - pos);
        pos                    = (eol == std::string::npos) ? text.size() : eol + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == ';' || line[0] == '#') continue; // comment line
        {
            const std::size_t sc = line.find(';'); // trailing same-line comment, repo convention
            if (sc != std::string::npos) {
                line = trim(line.substr(0, sc));
                if (line.empty()) continue;
            }
        }
        if (line.front() == '[' && line.back() == ']') {
            const std::string sec = trim(line.substr(1, line.size() - 2));
            in_mod                = !seen_mod && ascii_lower(sec) == section; // the FIRST [mod]
            if (in_mod) seen_mod = true;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = std::format("INI: line {}: not key=value", line_no);
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (!in_mod) continue;                        // outside [mod]: the launcher does not see it either
        const std::string lkey = ascii_lower(key);    // ONE folded spelling for dedup AND dispatch
        if (!seen_keys.insert(lkey).second) continue; // FIRST occurrence wins
        fields.emplace(lkey, val);
    }
    return true;
}


bool valid_metadata_field(std::string_view value, unsigned max_bytes) { return valid_field(value, max_bytes); }

// THE one canonicalizer (no anonymous-namespace twin: the same body serves the traversal, which
// calls canon_component per enumerated component and applies the same bound).
bool canonical_resource_name(const char *data_relative_path, std::string &out, std::string &err) {
    out.clear();
    err.clear();
    if (!data_relative_path || !*data_relative_path) {
        err = "empty resource path";
        return false;
    }
    std::string comp, folded;
    auto        flush = [&]() -> bool {
        if (!canon_component(comp, folded, err)) return false;
        // The bound covers the WHOLE canonical name including separators, not just component bytes.
        const std::size_t prospective = out.empty() ? folded.size() : out.size() + 1 + folded.size();
        if (prospective > RESOURCE_NAME_MAX) {
            err = std::format("longer than the {}-byte resource-name bound", RESOURCE_NAME_MAX);
            return false;
        }
        if (!out.empty()) out += '\\';
        out += folded;
        comp.clear();
        return true;
    };
    for (const char *p = data_relative_path; *p; ++p) {
        const char c = *p;
        if (c == '/' || c == '\\') {
            if (!flush()) return false; // also refuses a leading separator (first flush is empty)
            continue;
        }
        if (c == ':') {
            err = "a resource name never carries a colon";
            return false;
        }
        comp += c;
    }
    if (comp.empty() && out.empty()) {
        err = "empty resource path";
        return false;
    }
    if (comp.empty()) {
        err = "trailing separator";
        return false;
    }
    return flush();
}

bool validate_campaign_map(const std::uint8_t *bytes, std::size_t size, std::string_view name,
                           std::string &error) {
    try {
        check_mp(bytes, size, name);
        return true;
    } catch (const PrepError &e) {
        error = e.msg;
        return false;
    }
}
bool validate_campaign_map(const std::vector<std::uint8_t> &bytes, std::string_view name,
                           std::string &error) {
    return validate_campaign_map(bytes.data(), bytes.size(), name, error);
}
const std::uint8_t *nomod_digest() {
    // Computed once through the SAME frame code path as every other digest (no zero bypass, no
    // special-cased constant): the empty set hashes the version frame with both counts zero.
    static const std::uint8_t *once = [] {
        static std::uint8_t            keep[32];
        const std::vector<ModIdentity> m;
        const std::vector<Payload>     p;
        compute_digest(m, p, keep);
        return keep;
    }();
    return once;
}

Snapshot prepare(const char *game_dir_ansi, const char *enabled_csv) {
    Snapshot                 snap;
    const std::string        game_dir = game_dir_ansi ? game_dir_ansi : "";
    std::vector<std::string> ids;
    std::string              err;
    try {
        check(parse_enabled(enabled_csv, ids, err), failure::enabled_list, err);
        check(ids.size() <= MODS_MAX, failure::enabled_list,
              std::format("enabled list: {} mods exceeds the {}-mod bound shared with the save "
                          "carrier -- a valid set must never mount with its save metadata "
                          "silently dropped",
                          ids.size(), MODS_MAX));
        snap.requested = ids;

        WalkCtx           ctx;
        const std::string mods_root = join_path(game_dir, "mods");
        check(!is_reparse(mods_root), failure::escape,
              std::format("{}: the mods root itself is a reparse point", mods_root));
        for (std::uint32_t i = 0; i < ids.size(); ++i) {
            const std::string &id     = ids[i];
            const std::string  folder = join_path(mods_root, id);
            check(dir_exists(folder), failure::mod_missing,
                  std::format("enabled mod '{}': no folder at {}", id, folder));
            check(!is_reparse(folder), failure::escape,
                  std::format("{}: the mod folder itself is a reparse point (junction/symlink)",
                              folder));

            // A FRESH alias table per mod: the same canonical name in two mods is shadowing, not
            // an alias failure.
            std::map<std::string, std::string> seen;

            // The mod root must match the folder contract, and mod.ini must agree.
            std::vector<DirEntry> root;
            check(list_dir(folder, root, err), failure::io, err);
            bool        has_ini = false, has_data = false;
            std::string cfg_dir, lang_dir;
            for (const DirEntry &e : root) {
                const std::string low = ascii_lower(e.name);
                if (e.reparse)
                    fail(failure::escape,
                         std::format("{}: reparse point (junction/symlink) refused", join_path(folder, e.name)),
                         low == "cfg" || low == "lang");
                if (low == "mod.ini" && !e.is_dir) {
                    has_ini = true;
                    continue;
                }
                if (low == "data" && e.is_dir) {
                    has_data = true;
                    continue;
                }
                if (low == "cfg" && e.is_dir) {
                    if (!cfg_dir.empty()) fail(failure::alias, "multiple cfg directories fold to one name", true);
                    cfg_dir = e.name;
                    continue;
                }
                if (low == "lang" && e.is_dir) {
                    if (!lang_dir.empty()) fail(failure::alias, "multiple lang directories fold to one name", true);
                    lang_dir = e.name;
                    continue;
                }
                if (low == "cfg" || low == "lang")
                    fail(failure::reserved, std::format("{}: config root must be a directory", join_path(folder, e.name)), true);
                fail(failure::unsupported_class,
                     std::format("{}: outside the mod folder contract (expected mod.ini, data/, "
                                 "cfg/ or lang/)",
                                 join_path(folder, e.name)));
            }
            check(has_ini, failure::manifest, std::format("{}: missing mod.ini", folder));

            Manifest m;
            check(parse_manifest(join_path(folder, "mod.ini"), m, err), failure::manifest, err);
            check(validate_manifest(m, id, err), failure::manifest, err);

            ModIdentity ident;
            ident.id          = m.id;
            ident.name        = m.name;
            ident.version     = m.version;
            ident.description = m.description;
            ident.folder      = folder;
            snap.mods.push_back(std::move(ident));

            const std::uint32_t before     = (std::uint32_t)ctx.payloads.size();
            const std::uint32_t before_cfg = (std::uint32_t)ctx.config_inputs.size();
            if (has_data) walk_data(ctx, seen, join_path(folder, "data"), "");
            // Config paths use a separate alias namespace from data/ resource names.
            std::map<std::string, std::string> cfg_seen;
            try {
                if (!cfg_dir.empty()) walk_data(ctx, cfg_seen, join_path(folder, cfg_dir), "cfg", true);
                if (!lang_dir.empty()) walk_data(ctx, cfg_seen, join_path(folder, lang_dir), "lang", true);
            } catch (PrepError &e) {
                e.config_error = true;
                throw;
            } catch (const std::exception &e) {
                fail(failure::io, std::string("config discovery failed: ") + e.what(), true);
            }
            for (std::uint32_t k = before_cfg; k < ctx.config_inputs.size(); ++k)
                ctx.config_inputs[k].mod_index = i;
            for (std::uint32_t k = before; k < (std::uint32_t)ctx.payloads.size(); ++k) {
                ctx.payloads[k].mod_index = i;
                check_payload(ctx.payloads[k].name, ctx.payloads[k].bytes);
            }
            if ((ctx.payloads.size() - before) + (ctx.config_inputs.size() - before_cfg) > MOD_PAYLOADS_MAX)
                fail(failure::payload_format,
                     std::format("mod '{}': more than {} payload files", id, MOD_PAYLOADS_MAX),
                     ctx.config_inputs.size() != before_cfg);
        }

        // Deterministic payload order (name, then enabled order); winners fall out of the sort:
        // the last payload per name carries the highest enabled index, i.e. LATER ENABLED WINS.
        std::sort(ctx.payloads.begin(), ctx.payloads.end(), [](const Payload &a, const Payload &b) {
            if (a.name != b.name) return a.name < b.name;
            return a.mod_index < b.mod_index;
        });
        std::sort(ctx.config_inputs.begin(), ctx.config_inputs.end(), [](const Payload &a, const Payload &b) {
            if (a.name != b.name) return a.name < b.name;
            return a.mod_index < b.mod_index;
        });
        snap.config_inputs = std::move(ctx.config_inputs);
        snap.payloads      = std::move(ctx.payloads);
        for (std::uint32_t k = 0; k < snap.payloads.size(); ++k)
            snap.winners[snap.payloads[k].name] = k;

        check_bank_pair(snap.payloads, snap.winners);
        if (!prepare_tilesets(snap, err)) fail(failure::payload_format, err);

        snap.ok = true;
        compute_digest(snap.mods, snap.payloads, snap.digest);
        return snap;
    } catch (const PrepError &pe) {
        Snapshot out;
        out.requested    = ids;
        out.failed       = pe.why;
        out.config_error = pe.config_error;
        out.error        = pe.msg;
        std::memcpy(out.digest, nomod_digest(), 32); // a failure mounts stock: the stock digest
        return out;
    } catch (const std::exception &ex) { // std::bad_alloc and friends: still a whole-set failure
        Snapshot out;
        out.requested = ids;
        out.failed    = failure::io;
        out.error     = std::format("preparation failed: {}", ex.what());
        std::memcpy(out.digest, nomod_digest(), 32);
        return out;
    }
}

namespace {
// The published snapshot: stored ONCE by mount_snapshot at the boot splice with a release store;
// runtime readers on any thread go through the acquire load in mounted_snapshot(). The pointee is
// never mutated after construction and never freed (process lifetime).
std::atomic<Snapshot *> g_published{nullptr};
} // namespace

void finalize_config(Snapshot &s, std::vector<std::uint8_t> init, std::vector<std::uint8_t> lang) {
    compute_config_digest(s, init, s.digest);
    auto add = [&](const char *name, std::vector<std::uint8_t> bytes) {
        Payload p;
        p.name           = name;
        p.mod_index      = 0;
        p.bytes          = std::move(bytes);
        const auto index = static_cast<std::uint32_t>(s.payloads.size());
        s.payloads.push_back(std::move(p));
        s.winners[name] = index;
    };
    add("init\\init.cfg", std::move(init));
    add("init\\initlang.cfg", std::move(lang));
}

void mount_snapshot(Snapshot &&s) {
    g_published.store(new Snapshot(std::move(s)), std::memory_order_release);
}

const Snapshot *mounted_snapshot() {
    return g_published.load(std::memory_order_acquire);
}

const Payload *Snapshot::winner(std::string_view canonical_name) const {
    const auto it = winners.find(canonical_name);
    return it == winners.end() ? nullptr : &payloads[it->second];
}

const Payload *Snapshot::payload(std::uint32_t index) const {
    return index < payloads.size() ? &payloads[index] : nullptr;
}

} // namespace mh::mods
