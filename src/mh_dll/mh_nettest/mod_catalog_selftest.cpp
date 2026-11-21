//
// mod_catalog_selftest.cpp -- DATAEXT1 behavior suite for the pure mod catalog
// (mh_common/mod_catalog.cpp, mh::mods). THE SUITE IS PURE BEHAVIOR, no retail fixtures: every
// arm builds its own temporary mod tree under %TEMP%, drives `prepare` against it, and cleans up.
//
// What is pinned (tracker mods:DATAEXT1 acceptance, catalog slice):
//   - no-mod canonical digest: an explicit version-framed constant, never zero, shared by every
//     whole-set failure (stock is what actually mounts);
//   - strict enabled order: later enabled mod wins a name, shadowed payloads stay in the snapshot
//     (all accepted bytes count in the digest), byte identity survives disk mutation after prep;
//   - whole-set fallback: a missing or invalid enabled mod fails the ENTIRE set with the reason
//     classified and the requested ids still visible;
//   - alias refusal inside one mod (case/separator folds), traversal/reparse escape refusal;
//   - manifest contract: first [mod], first duplicate key wins, ';' comments, UTF-8 + BOM, the
//     digest hashes PARSED canonical fields (comments/whitespace normalized, content differences
//     not);
//   - class surface: banki/bnk assembled pair over winners, TLO stride/tile bounds, .mpm through
//     the real mh_mapgen parser, campaign .mp loader arithmetic, generic packed classes accepted
//     as bytes, DIRECT-file classes and raw INIT* overrides refused; controlled patch inputs accepted.
//
#include "mh_mod_catalog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mh_mapgen/mpm.h"           // the real .mpm codec for fixtures
#include "mh_net_proto/net_crypto.h" // the one-shot, to pin the empty-set frame

namespace {

int g_checks = 0, g_failures = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

std::string hex_of(const std::uint8_t d[32]) {
    static const char *H = "0123456789abcdef";
    std::string        s(64, '0');
    for (int i = 0; i < 32; ++i) {
        s[2 * i]     = H[d[i] >> 4];
        s[2 * i + 1] = H[d[i] & 15];
    }
    return s;
}

// ---- the temporary forest ----------------------------------------------------------------------

std::string g_base; // <temp>\mh_modcat_<pid>

bool mkdir_p(const std::string &dir) {
    if (dir.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == '\\' || dir[i] == '/' || i + 1 == dir.size()) {
            if (cur.size() > 3 && cur[1] == ':') CreateDirectoryA(cur.c_str(), nullptr);
        }
    }
    const DWORD a = GetFileAttributesA(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool write_file(const std::string &path, const void *data, std::size_t n) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      wrote = 0;
    const BOOL ok    = WriteFile(h, data, (DWORD)n, &wrote, nullptr) && wrote == n;
    CloseHandle(h);
    return ok;
}

bool write_file(const std::string &path, const std::string &text) {
    return write_file(path, text.data(), text.size());
}

void rm_rf(const std::string &dir) {
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            const std::string p = dir + "\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA(p.c_str());
                else DeleteFileA(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_rf(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str()); // a junction link is removed as a link, never its target
}

// A directory junction (NTFS mount point). Failure is diagnosed and the corresponding assertion
// fails: name canonicalization does not prove filesystem reparse-point refusal.
bool make_junction(const std::string &link_dir, const std::string &target) {
    WCHAR tgt[MAX_PATH * 2];
    MultiByteToWideChar(CP_ACP, 0, target.c_str(), -1, tgt, MAX_PATH * 2);
    WCHAR subst[MAX_PATH * 2 + 8];
    swprintf(subst, MAX_PATH * 2 + 8, L"\\??\\%s", tgt);
    const WORD sub_bytes   = (WORD)(wcslen(subst) * 2);
    const WORD print_bytes = (WORD)(wcslen(tgt) * 2);

#pragma pack(push, 1)
    struct Buf {
        DWORD tag; // IO_REPARSE_TAG_MOUNT_POINT
        WORD  len; // ReparseDataLength
        WORD  res;
        WORD  sub_off, sub_len, print_off, print_len;
        WCHAR path[1];
    };
#pragma pack(pop)
    std::vector<std::uint8_t> raw(sizeof(Buf) + sub_bytes + 2 + print_bytes + 2, 0);
    Buf                      *b = (Buf *)raw.data();
    b->tag                      = 0xA0000003u;
    b->len                      = (WORD)(8 + sub_bytes + 2 + print_bytes + 2);
    b->sub_off                  = 0;
    b->sub_len                  = sub_bytes;
    b->print_off                = (WORD)(sub_bytes + 2);
    b->print_len                = print_bytes;
    std::memcpy(b->path, subst, sub_bytes);
    std::memcpy((std::uint8_t *)b->path + sub_bytes + 2, tgt, print_bytes);

    if (!CreateDirectoryA(link_dir.c_str(), nullptr)) {
        std::printf("  junction CreateDirectory %s failed: winerr=%lu\n", link_dir.c_str(), GetLastError());
        return false;
    }
    HANDLE h = CreateFileA(link_dir.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        RemoveDirectoryA(link_dir.c_str());
        std::printf("  junction open %s failed: winerr=%lu\n", link_dir.c_str(), error);
        return false;
    }
    DWORD       got   = 0;
    const BOOL  ok    = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, raw.data(),
                                        (DWORD)(8 + b->len), nullptr, 0, &got, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryA(link_dir.c_str());
        std::printf("  junction FSCTL_SET_REPARSE_POINT %s failed: winerr=%lu\n", link_dir.c_str(), error);
    }
    return ok != FALSE;
}

// ---- fixture builders --------------------------------------------------------------------------

std::string mod_dir(const char *id) { return g_base + "\\mods\\" + id; }

void make_mod(const char *id, const std::string &ini) {
    if (!mkdir_p(mod_dir(id)) || !write_file(mod_dir(id) + "\\mod.ini", ini)) {
        std::printf("  FIXTURE ERROR: cannot create manifest for %s (winerr=%lu)\n", id, GetLastError());
        std::abort();
    }
}

std::string data_dir(const char *id) { return mod_dir(id) + "\\data"; }

void put(const char *id, const char *rel, const void *data, std::size_t n) {
    // rel uses '\\' separators; create every level.
    std::string dir = data_dir(id), cur = dir;
    if (!mkdir_p(dir)) {
        std::printf("  FIXTURE ERROR: cannot create %s (winerr=%lu)\n", dir.c_str(), GetLastError());
        std::abort();
    }
    const char *slash = std::strrchr(rel, '\\');
    if (slash) {
        std::string layers(rel, (std::size_t)(slash - rel));
        std::size_t at = 0;
        while (at <= layers.size()) {
            const std::size_t next = layers.find('\\', at);
            const std::string part = layers.substr(at, next == std::string::npos ? std::string::npos
                                                                                 : next - at);
            cur += "\\" + part;
            if (!mkdir_p(cur)) {
                std::printf("  FIXTURE ERROR: cannot create %s (winerr=%lu)\n", cur.c_str(), GetLastError());
                std::abort();
            }
            if (next == std::string::npos) break;
            at = next + 1;
        }
    }
    const std::string path = cur + "\\" + std::string(slash ? slash + 1 : rel);
    if (!write_file(path, data, n)) {
        std::printf("  FIXTURE ERROR: cannot write %s (winerr=%lu)\n", path.c_str(), GetLastError());
        std::abort();
    }
}

void put(const char *id, const char *rel, const std::string &text) {
    put(id, rel, text.data(), text.size());
}

void put(const char *id, const char *rel, const std::vector<std::uint8_t> &b) {
    put(id, rel, b.data(), b.size());
}

void put_ini(const char *id, const char *name, const char *version,
             const char *description = nullptr, const char *id_override = nullptr) {
    std::string ini = "[mod]\nformat=1\nid=" + std::string(id_override ? id_override : id) +
                      "\nname=" + name + "\nversion=" + version;
    if (description) ini += "\ndescription=" + std::string(description);
    ini += "\n";
    make_mod(id, ini);
}

// A minimal V2 bank: one sprite, indices1 = {0}, 24 bytes of metadata (52 bytes total).
std::vector<std::uint8_t> make_bank_v2() {
    std::vector<std::uint8_t> b(52, 0);
    const std::uint32_t       hdr[5] = {4, 4, 24, 0, 0}; // spriteData, indices1, metadata, palette, indices2
    std::memcpy(b.data(), hdr, 20);
    const std::uint8_t sprite[4] = {1, 2, 3, 4};
    std::memcpy(b.data() + 20, sprite, 4);
    const std::uint32_t zero = 0; // indices1[0] = 0 (first valid offset must be 0)
    std::memcpy(b.data() + 24, &zero, 4);
    return b;
}

// BANKI.DAT bytes from {indicesSize, spriteDataSize, paletteDataSize} triples.
std::vector<std::uint8_t> make_banki(const std::vector<std::array<std::uint32_t, 3>> &entries) {
    std::vector<std::uint8_t> b(entries.size() * 12, 0);
    for (std::size_t i = 0; i < entries.size(); ++i)
        std::memcpy(b.data() + i * 12, entries[i].data(), 12);
    return b;
}

std::vector<std::uint8_t> make_mpm(std::uint32_t w) {
    mh_mapgen::MpmModel m;
    mh_mapgen::mpm_init(m, "diuna256.tlo", w, w);
    mh_mapgen::mpm_set_tail(m, "selftest fixture");
    return mh_mapgen::write_mpm(m);
}

// Campaign .MP fixture: 97-byte header, 7 B/tile, 28 B/object, then a length-derived footer.
std::vector<std::uint8_t> make_mp(std::uint32_t w, std::uint32_t h, std::uint32_t f1,
                                  std::uint32_t f2, std::int32_t f3, int slack) {
    const std::uint64_t footer = (f3 >= 1) ? (12ull * (std::uint64_t)f3 - 4ull) : 0ull;
    const std::uint64_t size   = 0x61ull + 7ull * w * h + 28ull * (f1 + f2) + 4ull + footer +
                               (std::uint64_t)slack;
    std::vector<std::uint8_t> b((std::size_t)size, 0);
    std::memcpy(b.data(), "ver02", 5);
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        b[at]     = (std::uint8_t)v;
        b[at + 1] = (std::uint8_t)(v >> 8);
        b[at + 2] = (std::uint8_t)(v >> 16);
        b[at + 3] = (std::uint8_t)(v >> 24);
    };
    put32(0x05, w);
    put32(0x09, h);
    std::memcpy(b.data() + 0x0d, "diuna256.tlo", 12);
    put32(0x35, f1);
    put32(0x39, f2);
    put32(0x41, (std::uint32_t)f3);
    return b;
}

const std::vector<std::uint8_t> g_gfx = {'M', 'H', 'G', 'F', 'X', 1, 2, 3};

// ---- arms --------------------------------------------------------------------------------------

void arm_canonical_names() {
    using mh::mods::canonical_resource_name;
    std::string out, err;
    ck(canonical_resource_name("Maps\\T1.MPM", out, err) && out == "maps\\t1.mpm",
       "canonical: case + separator folding");
    ck(canonical_resource_name("maps/t1.mpm", out, err) && out == "maps\\t1.mpm",
       "canonical: forward slash folds to backslash");
    ck(canonical_resource_name("a..b", out, err) && out == "a..b", "canonical: interior dots fine");
    ck(canonical_resource_name(std::string(46, 'x').c_str(), out, err) &&
           out == std::string(46, 'x'),
       "canonical: 46-byte name is the inclusive bound");
    ck(!canonical_resource_name(std::string(47, 'x').c_str(), out, err), "canonical: 47 refused");
    // The bound covers SEPARATORS too: "maps\\" (5) + 41 x's = 46 total passes; one more byte of
    // component fails even though its own component bytes are under the limit.
    const std::string tail41(41, 'x');
    ck(canonical_resource_name(("maps\\" + tail41).c_str(), out, err) && out.size() == 46,
       "canonical: nested name of exactly 46 bytes including separators passes");
    ck(!canonical_resource_name(("maps\\" + tail41 + "x").c_str(), out, err),
       "canonical: 47 bytes including separators refused");
    ck(!canonical_resource_name("", out, err), "canonical: empty refused");
    ck(!canonical_resource_name("\\abs", out, err), "canonical: leading separator (absolute) refused");
    ck(!canonical_resource_name("C:data\\x.mp", out, err), "canonical: drive prefix refused");
    ck(!canonical_resource_name("a\\b:x.mp", out, err), "canonical: colon (ADS) refused");
    ck(!canonical_resource_name("maps\\t1.mpm\\", out, err), "canonical: trailing separator refused");
    ck(!canonical_resource_name("maps\\\\t1.mpm", out, err), "canonical: empty component refused");
    ck(!canonical_resource_name("maps\\..\\escape.mpm", out, err), "canonical: '..' refused");
    ck(!canonical_resource_name("maps\\.\\t1.mpm", out, err), "canonical: '.' refused");
    ck(!canonical_resource_name("maps\t1.mpm", out, err), "canonical: control byte refused");
    const std::string hi = "maps\xff.mpm";
    ck(!canonical_resource_name(hi.c_str(), out, err), "canonical: non-ASCII byte refused");
}

void arm_nomod_digest() {
    const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "");
    ck(s.ok && s.mods.empty() && s.payloads.empty() && s.winners.empty(),
       "no-mod: an empty enabled list prepares an empty success");
    bool zero = true;
    for (std::uint8_t b : s.digest) zero = zero && b == 0;
    ck(!zero, "no-mod: the digest is an explicit constant, never a zero bypass");
    ck(std::memcmp(s.digest, mh::mods::nomod_digest(), 32) == 0,
       "no-mod: prepare's digest == nomod_digest()");
    // Pin the frame itself: "MHMODSDIGEST1" | u32 0 | u32 0 through the one-shot.
    const std::uint8_t frame[13 + 4 + 4] = {'M', 'H', 'M', 'O', 'D', 'S', 'D', 'I', 'G', 'E', 'S', 'T',
                                            '1', 0, 0, 0, 0, 0, 0, 0, 0};
    std::uint8_t       want[32];
    mh_net_proto::sha256(frame, sizeof(frame), want);
    ck(std::memcmp(s.digest, want, 32) == 0, "no-mod: the digest is SHA256 of the empty version frame");
}

void arm_missing_mod() {
    const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "ghost");
    ck(!s.ok && !s.config_error && s.failed == mh::mods::failure::mod_missing,
       "missing mod: the WHOLE set fails, classified");
    ck(s.requested.size() == 1 && s.requested[0] == "ghost",
       "missing mod: the requested id stays visible");
    ck(s.mods.empty() && s.payloads.empty() && s.winners.empty(),
       "missing mod: nothing half-mounted");
    ck(std::memcmp(s.digest, mh::mods::nomod_digest(), 32) == 0,
       "missing mod: the failure digest is the actual stock digest");
}

void arm_order_and_identity() {
    put_ini("alpha", "Alpha", "1.0");
    put_ini("beta", "Beta", "2.0", "second");
    const std::vector<std::uint8_t> m1 = make_mpm(64);
    const std::vector<std::uint8_t> m2 = make_mpm(68);
    put("alpha", "MAPS\\T1.MPM", m1);
    put("alpha", "menu\\logo.gfx", g_gfx);
    put("beta", "Maps\\T1.MPM", m2); // same canonical name, different bytes: the shadow
    put("beta", "maps\\t2.mpm", m1);

    const mh::mods::Snapshot ab = mh::mods::prepare(g_base.c_str(), "alpha,beta");
    if (!ab.ok) std::printf("  prepare alpha,beta failed: %s\n", ab.error.c_str());
    ck(ab.ok && ab.mods.size() == 2 && ab.mods[0].id == "alpha" && ab.mods[1].id == "beta",
       "order: mods listed in enabled order");
    const mh::mods::Payload *w1 = ab.winner("maps\\t1.mpm");
    ck(w1 && w1->bytes == m2 && w1->mod_index == 1,
       "order: LATER enabled mod wins the contested name");
    ck(ab.winner("maps\\t2.mpm") && ab.winner("maps\\t2.mpm")->mod_index == 1,
       "order: uncontested names resolve normally");
    ck(ab.winner("menu\\logo.gfx") && ab.winner("menu\\logo.gfx")->bytes == g_gfx,
       "generic packed class (.gfx) accepted and served");
    ck(ab.payloads.size() == 4 &&
           ab.payloads[0].name <= ab.payloads[1].name &&
           ab.payloads[1].name <= ab.payloads[2].name &&
           ab.payloads[2].name <= ab.payloads[3].name,
       "shadow: ALL accepted payloads ride along (shadowed included), sorted by canonical name");

    const mh::mods::Snapshot ba = mh::mods::prepare(g_base.c_str(), "beta,alpha");
    const mh::mods::Payload *w2 = ba.winner("maps\\t1.mpm");
    ck(w2 && w2->bytes == m1 && w2->mod_index == 1,
       "order: flipping the list flips the winner (never enumeration order)");
    ck(std::memcmp(ab.digest, ba.digest, 32) != 0, "order: the two orders are different identities");

    // Mutation after prepare must not touch the snapshot -- and must change the NEXT prepare.
    const std::string               mutated = mod_dir("alpha") + "\\data\\MAPS\\T1.MPM";
    const std::vector<std::uint8_t> m3      = make_mpm(72);
    if (!write_file(mutated, m3.data(), m3.size())) {
        std::printf("  FIXTURE ERROR: cannot mutate %s (winerr=%lu)\n", mutated.c_str(), GetLastError());
        std::abort();
    }
    ck(w1 && w1->bytes == m2 && ab.winner("maps\\t1.mpm") && ab.winner("maps\\t1.mpm")->bytes == m2,
       "identity: the mounted snapshot's bytes are the preparation-time bytes");
    bool shadow_stable = false;
    for (const mh::mods::Payload &p : ab.payloads)
        if (p.name == "maps\\t1.mpm" && p.mod_index == 0) shadow_stable = (p.bytes == m1);
    ck(shadow_stable,
       "identity: mutating the source on disk does not touch the snapshot (shadow keeps its "
       "preparation-time bytes)");
    const mh::mods::Snapshot ab2 = mh::mods::prepare(g_base.c_str(), "alpha,beta");
    ck(ab2.ok && std::memcmp(ab.digest, ab2.digest, 32) != 0,
       "identity: mutating a source file changes the NEXT preparation's identity");
}

void arm_alias() {
    put_ini("al", "Al", "1.0");
    const std::string root = data_dir("al");
    if (!mkdir_p(root)) {
        std::printf("  FIXTURE ERROR: cannot create case-sensitive alias directory\n");
        std::abort();
    }
    HANDLE                   dir     = CreateFileA(root.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILE_CASE_SENSITIVE_INFO flags   = {FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool               enabled = dir != INVALID_HANDLE_VALUE &&
                         SetFileInformationByHandle(dir, FileCaseSensitiveInfo, &flags, sizeof(flags));
    const DWORD error = enabled ? ERROR_SUCCESS : GetLastError();
    if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
    if (!enabled) {
        std::printf("  FIXTURE ERROR: cannot enable case-sensitive alias directory (winerr=%lu)\n", error);
        std::abort();
    }
    put("al", "MAPS\\T1.MPM", make_mpm(64));
    put("al", "maps\\t1.mpm", make_mpm(64)); // folds onto the same canonical name: alias
    const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "al");
    ck(!s.ok && s.failed == mh::mods::failure::alias,
       "alias: two files of ONE mod folding to one name fail the whole set");
    ck(s.requested.size() == 1 && s.requested[0] == "al", "alias: requested id visible");
}

void arm_escape() {
    put_ini("esc", "Esc", "1.0");
    mkdir_p(data_dir("esc") + "\\outside");
    put("esc", "MAPS\\OK.MPM", make_mpm(64));
    const bool linked =
        make_junction(data_dir("esc") + "\\outside\\loop", g_base + "\\mods");
    const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "esc");
    ck(linked && !s.ok && s.failed == mh::mods::failure::escape,
       "escape: a reparse point inside data/ refuses the whole set");

    // The MOD FOLDER ITSELF being a junction is the same refusal: the whole tree would resolve
    // somewhere else. Build a real folder elsewhere, link <mods>/esc2 at it, manifest inside.
    const std::string real_dir = g_base + "\\outside_real\\esc2";
    mkdir_p(real_dir);
    write_file(real_dir + "\\mod.ini",
               "[mod]\nformat=1\nid=esc2\nname=Linked\nversion=1\n");
    const bool               root_linked = make_junction(mod_dir("esc2"), real_dir);
    const mh::mods::Snapshot r           = mh::mods::prepare(g_base.c_str(), "esc2");
    ck(root_linked && !r.ok && r.failed == mh::mods::failure::escape,
       "escape: a junction at the mod folder root refuses the whole set");
}

void arm_manifest_contract() {
    // Missing version.
    make_mod("mv", "[mod]\nformat=1\nid=mv\nname=MissingVersion\n");
    ck(mh::mods::prepare(g_base.c_str(), "mv").failed == mh::mods::failure::manifest,
       "manifest: version is required");
    // id != folder.
    put_ini("mi", "Mismatch", "1.0", nullptr, "other");
    ck(mh::mods::prepare(g_base.c_str(), "mi").failed == mh::mods::failure::manifest,
       "manifest: id must equal the folder name");
    // format != 1.
    make_mod("mf", "[mod]\nformat=2\nid=mf\nname=F\nversion=1\n");
    ck(mh::mods::prepare(g_base.c_str(), "mf").failed == mh::mods::failure::manifest,
       "manifest: only format=1 is the contract");
    // Invalid UTF-8 in name.
    make_mod("mu", std::string("[mod]\nformat=1\nid=mu\nname=bad\xffUTF8\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "mu").failed == mh::mods::failure::manifest,
       "manifest: values must be valid UTF-8");
    // Valid multi-byte sequences: Cyrillic 2-byte, euro-style 3-byte, emoji 4-byte.
    make_mod("mc", std::string("[mod]\nformat=1\nid=mc\nname=\xD0\xBC\xD0\xBE\xD0\xB4 \xE2\x82\xAC \xF0\x9F\x99\x82\nversion=1\n"));
    {
        const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "mc");
        ck(s.ok && s.mods.size() == 1 && s.mods[0].name == "\xD0\xBC\xD0\xBE\xD0\xB4 \xE2\x82\xAC \xF0\x9F\x99\x82",
           "manifest: valid 2/3/4-byte UTF-8 sequences are accepted (Cyrillic/euro/emoji)");
    }
    // An unpaired surrogate (ED A0 80) and an overlong encoding (C0 80) are not UTF-8.
    make_mod("ms", std::string("[mod]\nformat=1\nid=ms\nname=\xED\xA0\x80\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "ms").failed == mh::mods::failure::manifest,
       "manifest: an unpaired surrogate is refused");
    make_mod("mo2", std::string("[mod]\nformat=1\nid=mo2\nname=\xC0\x80\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "mo2").failed == mh::mods::failure::manifest,
       "manifest: an overlong encoding is refused");
    // A truncated sequence at end of value.
    make_mod("mt", std::string("[mod]\nformat=1\nid=mt\nname=cut\xD0\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "mt").failed == mh::mods::failure::manifest,
       "manifest: a truncated multi-byte sequence is refused");
    // Description obeys the same rules when present (control bytes forbidden, empty allowed).
    make_mod("md0", "[mod]\nformat=1\nid=md0\nname=D\nversion=1\ndescription=\n");
    ck(mh::mods::prepare(g_base.c_str(), "md0").ok, "manifest: an empty description is allowed");
    make_mod("md1", std::string("[mod]\nformat=1\nid=md1\nname=D\nversion=1\ndescription=a\x01b\n"));
    ck(mh::mods::prepare(g_base.c_str(), "md1").failed == mh::mods::failure::manifest,
       "manifest: a control byte in the description is refused");
    // C1 controls count as controls on BOTH sides of the launcher contract; U+00A0 does not.
    make_mod("md2", std::string("[mod]\nformat=1\nid=md2\nname=D\xC2\x9F\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "md2").failed == mh::mods::failure::manifest,
       "manifest: a C1 control (U+0080..U+009F) is refused like an ASCII control");
    make_mod("md3", std::string("[mod]\nformat=1\nid=md3\nname=D\xC2\xA0x\nversion=1\n"));
    ck(mh::mods::prepare(g_base.c_str(), "md3").ok,
       "manifest: U+00A0 is not a control on either side of the contract");
    // BOM + CRLF + comments + duplicate key first-wins + unknown keys ignored.
    make_mod("mb", "\xEF\xBB\xBF[mod]\r\nformat=1\r\n; a comment line\r\n# another\r\nid=mb ; why not\r\n"
                   "name=First ; the first one wins\r\nname=Second\r\nfuture_field=ignored\r\n"
                   "version=9\r\n");
    const mh::mods::Snapshot b = mh::mods::prepare(g_base.c_str(), "mb");
    ck(b.ok && b.mods.size() == 1 && b.mods[0].name == "First" && b.mods[0].version == "9",
       "manifest: BOM/CRLF/comments accepted, FIRST duplicate key wins, unknown keys ignored");
    // Whitespace/comment normalization: semantically identical files hash identically.
    put_ini("nz1", "Same", "1.0");
    make_mod("nz2", "[mod]\nformat=1 ; trailer\nid=nz2\nname =  Same  ; trailing spaces trimmed\n"
                    "version=1.0\n");
    const mh::mods::Snapshot z1 = mh::mods::prepare(g_base.c_str(), "nz1");
    const mh::mods::Snapshot z2 = mh::mods::prepare(g_base.c_str(), "nz2");
    ck(z1.ok && z2.ok && std::memcmp(z1.digest, z2.digest, 32) != 0,
       "manifest: different ids are different identities (ids are in the frame)");
    // Same id, different description: metadata differences are strict-match differences.
    put_ini("md", "Same", "1.0", "one");
    make_mod("me", "[mod]\nformat=1\nid=md\nname=Same\nversion=1.0\ndescription=two\n");
    const mh::mods::Snapshot d1 = mh::mods::prepare(g_base.c_str(), "md");
    make_mod("md", "[mod]\nformat=1\nid=md\nname=Same\nversion=1.0\ndescription=one-other\n");
    const mh::mods::Snapshot d2 = mh::mods::prepare(g_base.c_str(), "md");
    ck(d1.ok && d2.ok && std::memcmp(d1.digest, d2.digest, 32) != 0,
       "manifest: a changed description changes the identity (no silent relax)");
}

void arm_reserved_and_direct() {
    put_ini("rc", "Cfg", "1.0");
    mkdir_p(mod_dir("rc") + "\\cfg");
    mkdir_p(mod_dir("rc") + "\\lang");
    write_file(mod_dir("rc") + "\\cfg\\patch.cfg", "@patch UNIT \"A\"\nEND\n");
    write_file(mod_dir("rc") + "\\lang\\en.cfg", "TEXT \"K\" \"english\"\n");
    write_file(mod_dir("rc") + "\\lang\\ru.cfg", "TEXT \"K\" \"russian\"\n");
    put("rc", "art.gfx", "resource");
    auto candidate = mh::mods::prepare(g_base.c_str(), "rc");
    ck(candidate.ok && candidate.config_inputs.size() == 3 && candidate.payloads.size() == 1,
       "config: source inputs are separate from resources");
    ck(candidate.winner("cfg\\patch.cfg") == nullptr && candidate.winner("lang\\en.cfg") == nullptr,
       "config: source files never mount as resource winners");
    auto original = candidate.config_inputs[0].bytes;
    write_file(candidate.config_inputs[0].source_path, "changed");
    ck(candidate.config_inputs[0].bytes == original, "config: input bytes are immutable after preparation");
    auto same = candidate;
    mh::mods::finalize_config(candidate, {1, 2, 3}, {4});
    mh::mods::finalize_config(same, {1, 2, 3}, {5});
    ck(candidate.winner("init\\init.cfg") && candidate.winner("init\\initlang.cfg"),
       "config: both generated winners are published together");
    ck(std::memcmp(candidate.digest, same.digest, 32) == 0,
       "config: selected translation output does not enter identity");
    auto changed = mh::mods::prepare(g_base.c_str(), "rc");
    mh::mods::finalize_config(changed, {1, 2, 3}, {4});
    ck(std::memcmp(candidate.digest, changed.digest, 32) != 0,
       "config: all input bytes affect identity");
    // Start from a fresh pre-finalization candidate; generated bytes are identity-bearing.
    auto gameplay_a = mh::mods::prepare(g_base.c_str(), "rc");
    auto gameplay_b = gameplay_a;
    mh::mods::finalize_config(gameplay_a, {1}, {9});
    mh::mods::finalize_config(gameplay_b, {2}, {9});
    ck(std::memcmp(gameplay_a.digest, gameplay_b.digest, 32) != 0,
       "config: emitted gameplay bytes affect identity");
    put_ini("later", "Later", "1");
    mkdir_p(mod_dir("later") + "\\lang");
    write_file(mod_dir("later") + "\\lang\\en.cfg", "TEXT \"K\" \"winner\"\n");
    auto layered_a = mh::mods::prepare(g_base.c_str(), "rc,later");
    write_file(mod_dir("rc") + "\\lang\\en.cfg", "TEXT \"K\" \"shadowed change\"\n");
    auto layered_b = mh::mods::prepare(g_base.c_str(), "rc,later");
    mh::mods::finalize_config(layered_a, {1}, {9});
    mh::mods::finalize_config(layered_b, {1}, {9});
    ck(std::memcmp(layered_a.digest, layered_b.digest, 32) != 0,
       "config: shadowed translations still affect identity");
    put_ini("ri", "Init", "1.0");
    put("ri", "INIT.CFG", "[x]\n");
    ck(mh::mods::prepare(g_base.c_str(), "ri").failed == mh::mods::failure::reserved,
       "reserved: bare INIT.CFG is refused");
    put_ini("ri2", "Init2", "1.0");
    put("ri2", "init\\INITLANG.CFG", "[x]\n");
    ck(mh::mods::prepare(g_base.c_str(), "ri2").failed == mh::mods::failure::reserved,
       "reserved: init/INITLANG.CFG is refused");
    put_ini("ri3", "Init3", "1.0");
    put("ri3", "init\\INIT.CFG", "[x]\n");
    ck(mh::mods::prepare(g_base.c_str(), "ri3").failed == mh::mods::failure::reserved,
       "reserved: init/INIT.CFG is refused");
    put_ini("ri4", "Init4", "1.0");
    put("ri4", "INITLANG.CFG", "[x]\n");
    ck(mh::mods::prepare(g_base.c_str(), "ri4").failed == mh::mods::failure::reserved,
       "reserved: bare INITLANG.CFG is refused");
    put_ini("linkcfg", "Link", "1.0");
    const bool cfg_linked    = make_junction(mod_dir("linkcfg") + "\\cfg", mod_dir("rc") + "\\cfg");
    auto       linked_config = mh::mods::prepare(g_base.c_str(), "linkcfg");
    ck(cfg_linked && !linked_config.ok && linked_config.failed == mh::mods::failure::escape,
       "config: reparse root is refused");
    put_ini("badcfg", "Bad", "1.0");
    mkdir_p(mod_dir("badcfg") + "\\cfg");
    write_file(mod_dir("badcfg") + "\\cfg\\other.cfg", "x");
    auto refused = mh::mods::prepare(g_base.c_str(), "rc,badcfg");
    ck(!refused.ok && refused.config_error && refused.payloads.empty() && refused.config_inputs.empty() &&
           std::memcmp(refused.digest, mh::mods::nomod_digest(), 32) == 0,
       "config: unsupported later file discards entire set");
    put_ini("rd", "Msgs", "1.0");
    put("rd", "MSGS.DAT", "direct file");
    ck(mh::mods::prepare(g_base.c_str(), "rd").failed == mh::mods::failure::unsupported_class,
       "direct: msgs.dat is a DIRECT-file class, refused");
    put_ini("ra", "Avi", "1.0");
    put("ra", "res\\intro.avi", "RIFF");
    ck(mh::mods::prepare(g_base.c_str(), "ra").failed == mh::mods::failure::unsupported_class,
       "direct: .avi is a DIRECT media class, refused");
    put_ini("rf", "Root", "1.0");
    write_file(mod_dir("rf") + "\\notes.txt", "hello");
    ck(mh::mods::prepare(g_base.c_str(), "rf").failed == mh::mods::failure::unsupported_class,
       "contract: a mod-root entry outside mod.ini + data/ is diagnosed");
}

void arm_bank_pair() {
    const std::vector<std::uint8_t> bank = make_bank_v2();
    // Good: two entries, first live, second empty; the referenced bank present.
    put_ini("bk1", "Banks", "1.0");
    put("bk1", "BANKI.DAT", make_banki({{4, 4, 0}, {0, 0, 0}}));
    put("bk1", "BANK_00.BNK", bank);
    ck(mh::mods::prepare(g_base.c_str(), "bk1").ok,
       "bank pair: a live entry with its bank file passes through SplitBnk");
    // Bank without BANKI.
    put_ini("bk2", "Banks2", "1.0");
    put("bk2", "BANK_00.BNK", bank);
    ck(mh::mods::prepare(g_base.c_str(), "bk2").failed == mh::mods::failure::payload_format,
       "bank pair: a bank file with no mounted BANKI.DAT fails");
    // A full descriptor with unchanged banks is valid here; the adapter checks those entries
    // against the stock BANKI before publication and the engine reads their bytes from the pack.
    put_ini("bk3", "Banks3", "1.0");
    const std::vector<std::uint8_t> descriptor = make_banki({{4, 4, 0}});
    put("bk3", "BANKI.DAT", descriptor);
    const mh::mods::Snapshot stock_bank = mh::mods::prepare(g_base.c_str(), "bk3");
    ck(stock_bank.ok && stock_bank.winner("banki.dat") &&
           stock_bank.winner("banki.dat")->bytes == descriptor && !stock_bank.winner("bank_00.bnk"),
       "bank pair: descriptor-only catalog leaves an unchanged bank to the stock pack");
    // Unreferenced slot.
    put_ini("bk4", "Banks4", "1.0");
    put("bk4", "BANKI.DAT", make_banki({{4, 4, 0}}));
    put("bk4", "BANK_07.BNK", bank);
    ck(mh::mods::prepare(g_base.c_str(), "bk4").failed == mh::mods::failure::payload_format,
       "bank pair: a bank beyond the mounted descriptor count fails (no silent dead bytes)");
    // Size mismatch against the descriptor.
    put_ini("bk5", "Banks5", "1.0");
    put("bk5", "BANKI.DAT", make_banki({{8, 4, 0}})); // descriptor says 2 sprites, file has 1
    put("bk5", "BANK_00.BNK", bank);
    ck(mh::mods::prepare(g_base.c_str(), "bk5").failed == mh::mods::failure::payload_format,
       "bank pair: SplitBnk refuses a size-mismatched pair (no mismatch reaches retail)");
    // BANKI not a multiple of 12.
    put_ini("bk6", "Banks6", "1.0");
    put("bk6", "BANKI.DAT", std::vector<std::uint8_t>(13, 0));
    put("bk6", "BANK_00.BNK", bank);
    ck(mh::mods::prepare(g_base.c_str(), "bk6").failed == mh::mods::failure::payload_format,
       "bank pair: a truncated BANKI.DAT fails");
    // CROSS-MOD assembly: alpha ships BANKI + bank 0, beta shadows bank 1; the winner set pairs.
    put_ini("bka", "BankA", "1.0");
    put("bka", "BANKI.DAT", make_banki({{4, 4, 0}, {4, 4, 0}}));
    put("bka", "BANK_00.BNK", bank);
    put("bka", "BANK_01.BNK", bank);
    put_ini("bkb", "BankB", "1.0");
    std::vector<std::uint8_t> bank_b = make_bank_v2();
    bank_b[20]                       = 9; // different sprite bytes, same shape
    put("bkb", "bank_01.bnk", bank_b);
    const mh::mods::Snapshot cx = mh::mods::prepare(g_base.c_str(), "bka,bkb");
    ck(cx.ok && cx.winner("bank_01.bnk") && cx.winner("bank_01.bnk")->bytes == bank_b &&
           cx.winner("bank_00.bnk") && cx.winner("bank_00.bnk")->bytes == bank,
       "bank pair: the pair validates over WINNING bytes across mods (beta's bank under alpha's BANKI)");
    // A fully SHADOWED bank with a descriptor-incompatible shape must NOT refuse the set: only
    // winners are pair-validated; shadowed bytes still ride in the digest.
    put_ini("bkc", "BankC", "1.0");
    put("bkc", "BANKI.DAT", make_banki({{4, 4, 0}})); // 1-sprite descriptor, 52-byte banks
    put("bkc", "BANK_00.BNK", bank);
    put_ini("bkd", "BankD", "1.0");
    std::vector<std::uint8_t> wide_bank = make_bank_v2();
    wide_bank.resize(80); // shape of a 2-sprite {8,4,0} descriptor
    const std::uint32_t wide_hdr[5] = {4, 8, 48, 0, 0};
    std::memcpy(wide_bank.data(), wide_hdr, 20);
    put("bkd", "BANKI.DAT", make_banki({{8, 4, 0}})); // 2-sprite descriptor, expects 80 bytes
    put("bkd", "BANK_00.BNK", wide_bank);             // shadows bkc's bank AND its BANKI
    const mh::mods::Snapshot sh = mh::mods::prepare(g_base.c_str(), "bkc,bkd");
    ck(sh.ok && sh.payloads.size() == 4 && sh.winner("bank_00.bnk") &&
           sh.winner("bank_00.bnk")->bytes == wide_bank,
       "bank pair: only WINNING banks are pair-validated; the shadowed 52-byte copy passes");
    // Winners can still genuinely mismatch across mods: bke's bank wins over bkc's under bkc's
    // own BANKI and the shapes disagree -> the pair check refuses (bank, not shadow, bites).
    put_ini("bke", "BankE", "1.0");
    put("bke", "BANK_00.BNK", wide_bank); // 80 bytes and no BANKI.DAT of its own
    ck(!mh::mods::prepare(g_base.c_str(), "bkc,bke").ok,
       "bank pair: a winning bank incompatible with the winning BANKI refuses");
}

void arm_tlo_bounds() {
    put_ini("t1", "Tlo", "1.0");
    put("t1", "SETS\\A.TLO", std::vector<std::uint8_t>(512 + 1024, 7)); // one 32x32 tile
    put("t1", "SETS\\F.TLO", std::vector<std::uint8_t>(512 + 768, 7));  // one 32x24 tile
    ck(mh::mods::prepare(g_base.c_str(), "t1").ok, "tlo: palette + one tile passes at both strides");
    put_ini("t2", "Tlo2", "1.0");
    put("t2", "SETS\\A.TLO", std::vector<std::uint8_t>(512 + 100, 7));
    ck(mh::mods::prepare(g_base.c_str(), "t2").failed == mh::mods::failure::payload_format,
       "tlo: a size no stride explains fails");
    put_ini("t3", "Tlo3", "1.0");
    put("t3", "SETS\\A.TLO", std::vector<std::uint8_t>(512, 0));
    ck(mh::mods::prepare(g_base.c_str(), "t3").failed == mh::mods::failure::payload_format,
       "tlo: a bare palette with zero tiles fails");
    put_ini("t4", "Tlo4", "1.0");
    put("t4", "SETS\\A.TLO", std::vector<std::uint8_t>(512 + 1024ull * 8193ull, 0));
    ck(mh::mods::prepare(g_base.c_str(), "t4").failed == mh::mods::failure::payload_format,
       "tlo: past the 13-bit (8192) tile budget fails");
}

void arm_maps() {
    // .mpm: the real mh_mapgen parse -- a valid map passes, a corrupted one fails.
    put_ini("p1", "Mpm", "1.0");
    put("p1", "Maps\\Good.mpm", make_mpm(64));
    ck(mh::mods::prepare(g_base.c_str(), "p1").ok, "mpm: a real write_mpm map parses through read_mpm");
    put_ini("p2", "Mpm2", "1.0");
    std::vector<std::uint8_t> bad = make_mpm(64);
    bad.resize(0x61 + 2); // truncated tile plane, not the optional free-form description tail
    put("p2", "Maps\\Bad.mpm", bad);
    ck(mh::mods::prepare(g_base.c_str(), "p2").failed == mh::mods::failure::payload_format,
       "mpm: a truncated map is refused by the existing parser");
    // The adapter and catalog share the exact same campaign validator.
    std::string map_error;
    ck(mh::mods::validate_campaign_map(make_mp(128, 64, 0, 0, 2, 0), "good.mp", map_error),
       "campaign validator: valid map accepted");
    ck(!mh::mods::validate_campaign_map(make_mp(128, 64, 0, 0, 2, 300), "bad.mp", map_error) &&
           !map_error.empty(),
       "campaign validator: malformed map refused with reason");
    // Campaign .mp: loader arithmetic only (non-square allowed, exact window enforced).
    put_ini("p3", "Camp", "1.0");
    put("p3", "Maps\\C.mp", make_mp(128, 64, 0, 0, 2, 0));
    ck(mh::mods::prepare(g_base.c_str(), "p3").ok,
       "campaign .mp: exact plane formula accepted, non-square not invented away");
    put_ini("p4", "Camp2", "1.0");
    put("p4", "Maps\\C.mp", make_mp(128, 64, 0, 0, 2, 300)); // outside the footer slack
    ck(mh::mods::prepare(g_base.c_str(), "p4").failed == mh::mods::failure::payload_format,
       "campaign .mp: a size outside the loader's [size, size+255] window fails");
    put_ini("p5", "Camp3", "1.0");
    std::vector<std::uint8_t> v3 = make_mp(128, 64, 0, 0, 2, 0);
    v3[3]                        = '3'; // version 03
    put("p5", "Maps\\C.mp", v3);
    ck(mh::mods::prepare(g_base.c_str(), "p5").failed == mh::mods::failure::payload_format,
       "campaign .mp: the loader's version 02 is enforced");
    put_ini("p6", "Camp4", "1.0");
    std::vector<std::uint8_t> v4 = make_mp(128, 64, 0, 0, 0, 0); // f3 = 0: negative footer
    put("p6", "Maps\\C.mp", v4);
    ck(mh::mods::prepare(g_base.c_str(), "p6").failed == mh::mods::failure::payload_format,
       "campaign .mp: f3 must be a positive region count");
}

void arm_enabled_list() {
    put_ini("la", "La", "1.0");
    ck(mh::mods::prepare(g_base.c_str(), "la,la").failed == mh::mods::failure::enabled_list,
       "enabled: a duplicate id is invalid, never deduped");
    ck(mh::mods::prepare(g_base.c_str(), "LA").failed == mh::mods::failure::enabled_list,
       "enabled: ids are lowercase; '-' and '_' are distinct, 'A' is not 'a'");
    ck(mh::mods::prepare(g_base.c_str(), "la,,la").failed == mh::mods::failure::enabled_list,
       "enabled: an empty entry (double comma) is invalid");
    ck(mh::mods::prepare(g_base.c_str(), " la ").ok,
       "enabled: surrounding whitespace is trimmed");
    const mh::mods::Snapshot s = mh::mods::prepare(g_base.c_str(), "la,ghost2");
    ck(s.failed == mh::mods::failure::mod_missing && s.requested.size() == 2,
       "enabled: the requested list survives whole-set failure untouched");
    // The set-size bound is SHARED with the save carrier (its 64-mod manifest): a valid 65-mod
    // set must be refused, never mounted with its save metadata silently dropped.
    std::string many;
    for (int i = 0; i < 65; ++i) many += (i ? "," : "") + std::string("m") + std::to_string(i);
    ck(mh::mods::prepare(g_base.c_str(), many.c_str()).failed == mh::mods::failure::enabled_list,
       "enabled: more than 64 mods refuses the whole set (save-carrier agreement)");
    std::string exactly64;
    for (int i = 0; i < 64; ++i) {
        const std::string id = std::string("m") + std::to_string(i);
        put_ini(id.c_str(), id.c_str(), "1");
        exactly64 += (i ? "," : "") + id;
    }
    ck(mh::mods::prepare(g_base.c_str(), exactly64.c_str()).ok,
       "enabled: exactly 64 mods prepares (the shared bound is inclusive)");
}


std::string tileset_descriptor(const std::string &name, const std::string &label,
                               const std::string &trees = "jungle", const std::string &soldiers = "2") {
    return "[tileset]\nformat=1\nname=" + name + "\nlabel=" + label + "\ntrees=" + trees +
           "\nsoldiers=" + soldiers + "\nrules=tilesets/marsh.mgr\n";
}
std::string tileset_rules(unsigned tile = 0, const char *name = "marsh.tlo", const char *tree = "jungle") {
    const std::string id = std::to_string(tile);
    return "mapgen-rules 3\ntileset " + std::string(name) + "\nmaps 1\ninputs_sha256 " +
           std::string(64, '0') + "\nprofile diuna256 " + tree + "\nt " + id + " 1 1 0 1 2\nh " +
           id + " " + id + " 1\nv " + id + " " + id + " 1\nclass 0 -1 0 1000000 1\ncase 0 0 0 0 x0 " +
           id + " " + id + " " + id + " " + id + " 1\n";
}
void tileset_source(const char *mod, const std::string &label, std::size_t count = 2) {
    put_ini(mod, mod, "1");
    put(mod, "tilesets\\marsh.ini", tileset_descriptor("Marsh.tlo", label));
    put(mod, "tilesets\\marsh.mgr", tileset_rules());
    put(mod, "marsh.tlo", std::vector<std::uint8_t>(512 + count * 1024, 7));
}
void arm_tileset_catalog() {
    using namespace mh::mods;
    std::string token, err;
    const char *stock_names[] = {"Jungle.tlo", "Kam256.tlo", "Ksie256.tlo", "Lod256.tlo",
                                 "Metal256.tlo", "Ruins256.tlo", "Skaly256.tlo", "Diuna256.tlo"};
    for (std::uint8_t i = 0; i < 8; ++i)
        ck(stock_tileset_index(stock_names[i]) == i + 1, "tileset: native stock index order");
    ck(stock_tileset_index("JUNGLE.TLO") == 1 && stock_tileset_index("unknown.tlo") == 0,
       "tileset: case-insensitive stock lookup never falls back to Jungle");
    ck(tileset_token("Diuna256.tlo", token, err) && token == "diuna256.tlo", "tileset: 12 visible bytes accepted");
    ck(!tileset_token("toolong99.tlo", token, err), "tileset: overlong stem rejected");
    ck(!tileset_token("bad/name.tlo", token, err), "tileset: separators rejected");
    ck(!tileset_token(std::string_view("abc.tlo\0junk", 12), token, err), "tileset: embedded NUL rejected");
    tileset_source("tsa", "First");
    tileset_source("tsb", "Second");
    auto one = prepare(g_base.c_str(), "tsa");
    ck(one.ok && one.tilesets.size() == 1, "tileset: valid descriptor prepares");
    const auto *first = one.tileset("marsh.tlo");
    ck(first && first->label == "First" && first->wide_label == L"First" && first->trees == "jungle" &&
           first->soldier_variant == 2 && first->rules == "tilesets\\marsh.mgr" &&
           one.payload(first->tlo_payload) == one.winner("marsh.tlo") &&
           one.payload(first->rules_payload) == one.winner("tilesets\\marsh.mgr"),
       "tileset: descriptor metadata and checked owned payload indices");
    ck(one.tileset("MARSH.TLO") == first && one.tileset("mArSh.TlO") == first &&
           !one.tileset("marsh.tlo.extra") && !one.tileset("unknown.tlo") && !one.payload(UINT32_MAX),
       "tileset: allocation-free lookup folds ASCII and rejects prefixes and unknowns");

    put_ini("tssecond", "Second descriptor", "1");
    std::string alder = tileset_descriptor("alder.tlo", "Alder");
    alder.replace(alder.find("tilesets/marsh.mgr"), std::strlen("tilesets/marsh.mgr"), "tilesets/alder.mgr");
    put("tssecond", "tilesets\\alder.ini", alder);
    put("tssecond", "tilesets\\alder.mgr", tileset_rules(0, "alder.tlo"));
    put("tssecond", "alder.tlo", std::vector<std::uint8_t>(512 + 1024, 1));
    auto sorted = prepare(g_base.c_str(), "tsa,tssecond");
    ck(sorted.ok && sorted.tilesets.size() == 2 && sorted.tilesets[0].name == "alder.tlo" &&
           sorted.tilesets[1].name == "marsh.tlo" && sorted.tileset("alder.tlo") && sorted.tileset("marsh.tlo"),
       "tileset: owned descriptor lookup is sorted across resources");
    auto later = prepare(g_base.c_str(), "tsa,tsb");
    ck(later.ok && later.tileset("marsh.tlo") && later.tileset("marsh.tlo")->label == "Second" &&
           later.payload(later.tileset("marsh.tlo")->tlo_payload)->mod_index == 1,
       "tileset: later descriptor and referenced resource winners");
    auto earlier = prepare(g_base.c_str(), "tsb,tsa");
    ck(earlier.ok && earlier.tileset("marsh.tlo") && earlier.tileset("marsh.tlo")->label == "First",
       "tileset: reversing active order reverses precedence");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("Marsh.tlo", "First changed"));
    auto changed = prepare(g_base.c_str(), "tsa,tsb");
    ck(changed.ok && changed.tileset("marsh.tlo") && changed.tileset("marsh.tlo")->label == "Second" &&
           std::memcmp(changed.digest, later.digest, 32) != 0,
       "tileset: shadowed descriptor bytes remain in digest");
    put("tsa", "tilesets\\marsh.ini", "[tileset]\nformat=bad\nformat=1\n");
    auto shadow = prepare(g_base.c_str(), "tsa,tsb");
    ck(!shadow.ok && shadow.failed == failure::payload_format && shadow.payloads.empty() && shadow.tilesets.empty() &&
           std::memcmp(shadow.digest, nomod_digest(), 32) == 0,
       "tileset: malformed shadowed first occurrence rolls back set");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("Marsh.tlo", "First") + "format=bad\n[tileset]\nformat=bad\n");
    ck(prepare(g_base.c_str(), "tsa").ok, "tileset: first section and first key win");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("other.tlo", "First"));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: stem mismatch refused");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "First") + "unknown=value\n");
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: unknown descriptor field refused");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", std::string(64, 'x')));
    ck(prepare(g_base.c_str(), "tsa").ok, "tileset: 64-byte label accepted");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", std::string(65, 'x')));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: 65-byte label refused");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "a\xC2\x85"));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: UTF-8 control codepoint refused");

    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "First", "jungle", "3"));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: unsupported soldier profile refused");
    std::string traversal = tileset_descriptor("marsh.tlo", "First");
    traversal.replace(traversal.find("tilesets/marsh.mgr"), std::strlen("tilesets/marsh.mgr"), "../escape.mgr");
    put("tsa", "tilesets\\marsh.ini", traversal);
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: rules traversal refused");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "First", "none", "0"));
    put("tsa", "tilesets\\marsh.mgr", tileset_rules(0, "marsh.tlo", "none"));
    auto independent = prepare(g_base.c_str(), "tsa");
    ck(independent.ok && independent.tileset("marsh.tlo") && independent.tileset("marsh.tlo")->trees == "none" &&
           independent.tileset("marsh.tlo")->soldier_variant == 0,
       "tileset: independent explicit profiles");
    put("tsa", "tilesets\\marsh.mgr", tileset_rules());
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: rules tree profile mismatch refused");
    put("tsa", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "First"));
    put("tsa", "tilesets\\marsh.mgr", tileset_rules(2));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: rules tile at TLO count refused");
    put("tsa", "tilesets\\marsh.mgr", tileset_rules(0, "other.tlo"));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: rules token mismatch refused");
    put("tsa", "tilesets\\marsh.mgr", tileset_rules());
    put("tsa", "marsh.tlo", std::vector<std::uint8_t>(512 + 768, 1));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: tactical geometry refused for strategic set");
    put("tsa", "marsh.tlo", std::vector<std::uint8_t>(512 + 4097 * 1024, 1));
    put("tsa", "tilesets\\marsh.mgr", tileset_rules(4096));
    ck(!prepare(g_base.c_str(), "tsa").ok, "tileset: runtime tile ID 4096 refused despite format capacity");
    put_ini("tsformat", "TLO format", "1");
    put("tsformat", "atlas.tlo", std::vector<std::uint8_t>(512 + 8192 * 1024, 1));
    ck(prepare(g_base.c_str(), "tsformat").ok, "tileset: format ceiling of 8192 strategic tiles accepted");
    put_ini("tsmissing", "Missing", "1");
    put("tsmissing", "tilesets\\marsh.ini", tileset_descriptor("marsh.tlo", "Missing"));
    auto missing = prepare(g_base.c_str(), "tsmissing");
    ck(!missing.ok && missing.failed == failure::payload_format && missing.payloads.empty() &&
           std::memcmp(missing.digest, nomod_digest(), 32) == 0,
       "tileset: missing resources roll back whole set");

    put_ini("tsmap", "Map", "1");
    auto unknown = make_mpm(64);
    std::memset(unknown.data() + 0x0d, 0, 32);
    std::memcpy(unknown.data() + 0x0d, "unknown.tlo", 11);
    put("tsmap", "maps\\bad.mpm", unknown);
    auto badmap = prepare(g_base.c_str(), "tsmap");
    ck(!badmap.ok && badmap.failed == failure::payload_format && badmap.payloads.empty(),
       "tileset map: unknown source map rolls back before publication");

    // A shadowed map may use tile 1 while the winning one-tile TLO and map use tile 0.
    put_ini("tsmaplow", "Low map", "1");
    put_ini("tsmaphigh", "High map", "1");
    auto lower_map  = make_mpm(64);
    lower_map[0x61] = 1;
    put("tsmaplow", "maps\\same.mpm", lower_map);
    put("tsmaplow", "diuna256.tlo", std::vector<std::uint8_t>(512 + 2 * 1024, 1));
    put("tsmaphigh", "maps\\same.mpm", make_mpm(64));
    put("tsmaphigh", "diuna256.tlo", std::vector<std::uint8_t>(512 + 1024, 1));
    ck(prepare(g_base.c_str(), "tsmaplow,tsmaphigh").ok,
       "tileset map: shadowed map references do not constrain winning TLO");
    ck(prepare(g_base.c_str(), "tsmaphigh,tsmaplow").ok,
       "tileset map: lower map is valid when paired with its own winning TLO");
    std::vector<std::uint8_t> image  = make_mpm(64);
    const std::size_t         cuts[] = {0, 5, 0x60, 0x61, 1000, image.size() / 2, image.size() - 1, image.size()};
    for (const auto cut : cuts) {
        mh_mapgen::MpmModel decoded;
        std::string         parse_err, validate_err;
        const bool          parsed    = mh_mapgen::read_mpm(image.data(), cut, decoded, parse_err);
        const bool          validated = mh_mapgen::validate_mpm(image.data(), cut, validate_err);
        ck(parsed == validated && (parsed || parse_err == validate_err),
           "tileset map: zero-copy framing matches full parser");
    }
    auto empty = prepare(g_base.c_str(), "");
    ck(empty.ok && empty.tilesets.empty() && std::memcmp(empty.digest, nomod_digest(), 32) == 0,
       "tileset: no-mod identity unchanged");

    const std::array<std::uint16_t, 8> stock_counts = {0, 0, 0, 0, 0, 0, 0, 1};
    const std::array<std::uint16_t, 8> no_counts{};
    ck(!validate_tileset_map(one, nullptr, 0, false, no_counts, err), "tileset map: null input refused");
    auto m = make_mpm(64);
    ck(validate_tileset_map(one, m.data(), m.size(), false, stock_counts, err), "tileset map: stock identity and actual count accepted");
    ck(!validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: unresolved stock count refused");
    std::memset(m.data() + 0x0d, 0, 32);
    std::memcpy(m.data() + 0x0d, "marsh.tlo", 9);
    ck(validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: custom TLO resolved from snapshot");
    m[0x61] = 1;
    ck(validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: final available tile accepted");
    m[0x61] = 2;
    ck(!validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: tile equal to count refused");
    m[0x61] = 0;
    m[0x62] = 0x10;
    ck(!validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: runtime ID 4096 refused");
    m[0x62] = 0;
    std::memset(m.data() + 0x0d, 0, 32);
    std::memcpy(m.data() + 0x0d, "unknown.tlo", 11);
    ck(!validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: unknown token refused");
    std::memset(m.data() + 0x0d, 'a', 32);
    ck(!validate_tileset_map(one, m.data(), m.size(), false, no_counts, err), "tileset map: unterminated disk name refused");
    auto camp = make_mp(128, 64, 0, 0, 2, 0);
    ck(validate_tileset_map(one, camp.data(), camp.size(), true, stock_counts, err), "tileset map: campaign stock token accepted with count");
    camp[0x61] = 1;
    ck(!validate_tileset_map(one, camp.data(), camp.size(), true, stock_counts, err), "tileset map: campaign tile equal to count refused");
}

void arm_mounted() {
    put_ini("mo", "Mounted", "1.0");
    put("mo", "maps\\m.mpm", make_mpm(64));
    mh::mods::Snapshot ok = mh::mods::prepare(g_base.c_str(), "mo");
    ck(ok.ok, "mount: the success snapshot prepares");
    mh::mods::mount_snapshot(std::move(ok));
    const mh::mods::Snapshot *m = mh::mods::mounted_snapshot();
    ck(m && m->ok && m->winner("maps\\m.mpm") && m->winner("maps\\m.mpm")->bytes == make_mpm(64),
       "mount: the published snapshot serves const lookups");
    mh::mods::Snapshot bad = mh::mods::prepare(g_base.c_str(), "nope");
    ck(!bad.ok, "mount: the failure snapshot prepares");
    mh::mods::mount_snapshot(std::move(bad));
    const mh::mods::Snapshot *f = mh::mods::mounted_snapshot();
    ck(f && !f->ok && f->payloads.empty() && std::memcmp(f->digest, mh::mods::nomod_digest(), 32) == 0,
       "mount: mounting the failure snapshot publishes stock identity and no payloads");
}

} // namespace

int run_modcatalogtest() {
    // One fresh temporary forest per run; every arm reuses it and cleans up at the end.
    char       temp[MAX_PATH];
    const UINT n = GetTempPathA(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) {
        std::printf("modcatalogtest: no %%TEMP%%\n");
        return 1;
    }
    g_base = std::string(temp, n - (temp[n - 1] == '\\' ? 1 : 0)) + "\\mh_modcat_" +
             std::to_string(GetCurrentProcessId());
    rm_rf(g_base);
    if (!mkdir_p(g_base + "\\mods")) {
        std::printf("modcatalogtest: cannot create %s\n", g_base.c_str());
        return 1;
    }

    arm_canonical_names();
    arm_nomod_digest();
    arm_missing_mod();
    arm_order_and_identity();
    arm_alias();
    arm_escape();
    arm_manifest_contract();
    arm_reserved_and_direct();
    arm_bank_pair();
    arm_tlo_bounds();
    arm_maps();
    arm_enabled_list();
    arm_tileset_catalog();
    arm_mounted();

    rm_rf(g_base);
    std::printf("modcatalogtest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
