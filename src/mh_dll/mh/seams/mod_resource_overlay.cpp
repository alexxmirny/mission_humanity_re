//
// seams/mod_resource_overlay.cpp -- mods:DATAEXT1: the mod snapshot SHADOWS the
// loaded resource packs at the two public resource entry points. The mechanism, the front-pack
// rejection and the boot-timing argument are in include/mh_mod_export.h; this file carries them
// out on top of mh::mods (mh_common/include/mh_mod_catalog.h -- the pure catalog; this TU builds
// no catalog of its own).
//
// EN sites: the boot byte splice is registered as mods_boot_splice in
// tools/data/dll_patch_manifest.json. Resource entry trampolines use the runtime exclusive-entry
// registry and the manifest's dynamic mods_resource_entry_detours wrapper record:
//
//   0x004c3dbf  E8 9F F7 FF FF   call rsr_TryReadRsrFile   llm_boot_progress_draw -- the FIRST
//                                pack registration (G_RSR_FILE_PTRS is still empty); 5-byte
//                                patch_bytes_guarded splice, the combat_proto site-splice shape.
//   0x004cf069  55 89 E5 68 44 00 00 00   GetResourseFilePtr entry:  push ebp; mov ebp,esp;
//                                push 44h  -- the standard 8-byte Watcom prologue, stolen by
//                                mh::hook::install_trampoline (the CALL at entry+8 executes in
//                                place from the thunk's jmp; nothing is relocated by hand).
//   0x004cf15e  55 89 E5 68 2C 00 00 00   rsr_GetFileRealSize entry: the same shape, push 2Ch.
//
// Stock/no-mod keeps the byte path: with no `[mods] enabled` nothing is written at all.
//
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/mh_mod_export.h"
#include "include/mh_run_context.h"     // mh_run_path
#include "include/mh_config_dir.h"      // RL3: the ini is the player's -- <config dir>mh_net.ini
#include "include/mh_log_sink.h"        // LOG1: async log sink
#include "config/ini_read.h"            // read_ini_string -- strips a trailing `;comment`
#include "hook/patch.h"                 // patch_bytes_guarded -- the boot splice
#include "hook/detour.h"                // install_trampoline -- the two resource entry detours
#include "addr/mh_addrs.gen.h"          // mh::addr menu and window globals
#include "addr/mh_calls.gen.h"          // mh::call::utils_malloc / ToLower
#include "en_guard.h"                   // mh::en_build_ok -- EN-only: all three sites are EN VAs
#include "include/mh_langpack_export.h" // explicit selected locale; never infer from folder or codepage
#include "mh_cfg_patch.h"               // private prepare; no engine parse before success
#include "ui/player_strings.h"
#include "ui/text_wrap.h"
#include "mh_mod_catalog.h" // mh::mods -- prepare / mount_snapshot / mounted_snapshot
#include "mh_tileset.h"
#include <array>
#include "mh_mapgen/sprite_extent.h"
#include "seams/tileset_runtime.h"
#include "mh_bnk.h"             // mh::bnk::BankiEntry -- the BANKI record the guard compares
                                // (header-only use; the bnk CODEC stays in the catalog's
                                // validation, nothing here links SplitBnk)
#include "mh_lzw.h"             // mh::lzw -- the pure codec (mh_common): the game's own
                                // 0x004f4b7e, reimplemented and byte-verified by savetest's
                                // fixtures. The BANKI guard's stock-side read unwraps through
                                // THIS, not through the game: the original's call site is
                                // stack-arg (PUSH size,dst,src / CALL / ADD ESP,0xC) and its
                                // generated prototype is MH_UNAVAILABLE -- a hand shim around
                                // that refusal is exactly what not to build.
#include "seams/map_transfer.h" // Candidate map preparation and hook readiness, before publication.
#include "mods/save_meta.h"     // Save hook readiness and the mounted-set identity source.

// Owned by net_lockstep's present detour; false if the guarded hook did not install.
bool mh_mod_picker_present_ready();

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- the three sites and their exact expected bytes ----------------------------------------------

// llm_boot_progress_draw:  MOV EAX, s_mh_ex (0x004c3dba, lang_pack's operand) / CALL rsr_TryReadRsrFile.
constexpr uintptr_t ADDR_BOOT_CALL = 0x004c3dbfu; // resume 0x004c3dc4: MOV [_G_LLM_RSR_SOURCE_MODE], AL
constexpr uint8_t   BOOT_EXPECT[5] = {0xE8, 0x9F, 0xF7, 0xFF, 0xFF};

// The entries' full 8 stolen bytes -- the Watcom prologue PLUS the assert-size byte, a strictly
// stronger guard than the toolkit's 4-byte WATCOM_PROLOGUE (the documented expect_prologue=0
// opt-out shape). The displaced CALL at entry+8 is never copied: the thunk jumps to it in place.
constexpr uintptr_t ADDR_RES_PTR   = 0x004cf069u; // resume = the thunk's jmp to entry+8 (0x004cf071)
constexpr uint8_t   PTR_EXPECT[8]  = {0x55, 0x89, 0xE5, 0x68, 0x44, 0x00, 0x00, 0x00};
constexpr uintptr_t ADDR_RES_SIZE  = 0x004cf15eu; // resume = the thunk's jmp to entry+8 (0x004cf166)
constexpr uint8_t   SIZE_EXPECT[8] = {0x55, 0x89, 0xE5, 0x68, 0x2C, 0x00, 0x00, 0x00};

// Fixed VAs the two stubs reach indirectly (no generated thunks needed for any of them).
constexpr uintptr_t FN_TRY_READ = 0x004c3563u; // rsr_TryReadRsrFile (the displaced CALL's target)
constexpr uintptr_t BOOT_RESUME = 0x004c3dc4u; // the instruction after the displaced CALL

// ---- state ---------------------------------------------------------------------------------------

char g_ids[2114]    = {0}; // the ini's enabled list, verbatim; "" = stock (nothing was patched)
bool g_splice_done  = false;
bool g_splice_armed = false;
bool g_arm_done     = false;


void *g_fn_try_read = reinterpret_cast<void *>(FN_TRY_READ);
void *g_boot_tramp  = reinterpret_cast<void *>(BOOT_RESUME);
void *g_ptr_tramp   = nullptr; // install_trampoline publishes these LAST, only on a successful arm
void *g_size_tramp  = nullptr; // (U31 atomicity) -- a patched entry always has its thunk first

volatile LONG g_hits_logged = 0; // first-hit logs are capped across threads

char          g_log[MAX_PATH];
unsigned long g_log_gen = 0;

// The advisory resource channel -- the same file and shape as lang_pack's lp_log, `; [mods] ` tags.
// BOUNDED, unlike lp_log's wvsprintfA: the lines here embed a catalog error string and the raw
// `[mods] enabled` value (an ini can carry hundreds of characters), so a truncation here is a
// shortened log line, never a smashed stack.
void mlog(const char *fmt, ...) {
    mh_run_path(g_log, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[400];
    va_list ap;
    va_start(ap, fmt);
    int len = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (len < 0) len = (int)sizeof(line) - 2; // _TRUNCATE: the buffer is full
    line[len]     = '\n';
    line[len + 1] = 0;
    mh_logq_write(g_log, line, len + 1);
}

// "<exe dir>\" (with the trailing separator) -- the install root the catalog reads mods/ under.
bool exe_dir(char *out, int cap) {
    const DWORD n = GetModuleFileNameA(nullptr, out, cap);
    if (!n || n >= static_cast<DWORD>(cap)) return false;
    char *s = nullptr;
    for (char *p = out; *p; ++p)
        if (*p == '\\' || *p == '/') s = p;
    if (!s) return false;
    s[1] = 0;
    return true;
}

// Fold a game-relative name to the catalog's canonical form: lowercase ASCII, backslash
// separators, <= RESOURCE_NAME_MAX bytes (the rsr nam field's own bound -- a longer name cannot
// be canonical, so it misses to retail by construction). False on empty/overlong.
bool fold_key(const char *in, char *out, int cap) {
    int n = 0;
    for (const char *p = in; *p; ++p) {
        char c = *p;
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (n >= cap - 1) return false;
        out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

// The mounted winner for one query, or nullptr. The catalog's winners map is transparent
// (std::less<>): the folded STACK key looks up with no allocation.
const mh::mods::Payload *overlay_winner(const char *name) {
    const mh::mods::Snapshot *s = mh::mods::mounted_snapshot();
    if (s == nullptr) return nullptr;
    char key[mh::mods::RESOURCE_NAME_MAX + 1];
    if (!fold_key(name, key, sizeof(key))) return nullptr;
    const auto *p = s->winner(key);
    if (p != nullptr) return p;
    const size_t len = std::strlen(key);
    if (len < 4 || std::strcmp(key + len - 4, ".mpm") != 0 || std::strchr(key, '\\') != nullptr) return nullptr;
    char map_key[mh::mods::RESOURCE_NAME_MAX + 1];
    if (len + 5 > mh::mods::RESOURCE_NAME_MAX) return nullptr;
    std::memcpy(map_key, "maps\\", 5);
    std::memcpy(map_key + 5, key, len + 1);
    return s->winner(map_key);
}

// ---- the two detour helpers (__cdecl, called only from the naked stubs) ---------------------------
//
// BOTH preserve retail's observable side effect on the caller's buffer: rsr_GetFileEntry lowers
// the query IN PLACE before it looks, so a hit that never reaches it lowers the name itself
// (mh::call::ToLower -- the game's own routine). A miss falls through to the original, which does
// the same. REGISTER CONTRACT, verified against EN disassembly of both entries (they push
// AND pop EBX/ECX/EDX/ESI/EDI/EBP; only EAX differs -- it is the return): the stubs therefore
// preserve the caller's ECX and EDX across the hit path too, not just the MSVC-preserved
// EBX/ESI/EDI/EBP. EAX carries the argument in and the answer out; flags are scratch.

// __cdecl, called only from the naked detour. THREE answers, and the distinction is the point:
//   0              miss -- the stub runs the original (stock packs answer);
//   1              MOUNTED, but the game-heap allocation FAILED -- the stub answers
//                  resource-absent to the retail caller. It must NOT fall through: a fall-through
//                  would read the STOCK pack bytes for a name the digest advertises as mod bytes.
//   anything else  the copy pointer.
// The log names every allocation failure; MP identity stays honest (a peer that cannot afford the
// bytes says so loudly instead of quietly loading the wrong content).
uintptr_t __cdecl overlay_lookup_copy(const char *name) {
    const mh::mods::Payload *p = overlay_winner(name);
    if (p == nullptr || p->bytes.empty()) return 0;
    const uint32_t size = static_cast<uint32_t>(p->bytes.size());
    // One first-hit line per load burst, then counts only: the log stays bounded on a real mod.
    if (InterlockedIncrement(&g_hits_logged) <= 32)
        mlog("; [mods] overlay hit %s %uB", name, (unsigned)size);
    mh::call::ToLower(const_cast<char *>(name));
    // THE GAME-HEAP RETURN. utils_malloc, and the RETAIL caller's utils_free at the instant retail
    // frees a pack read (hostapi_io_bind.cpp's R5 note) -- the immutable snapshot is never handed
    // out. (The catalog guarantees accepted payloads are non-empty, so size >= 1 here.)
    uint8_t *copy = static_cast<uint8_t *>(mh::call::utils_malloc(size));
    if (copy == nullptr) {
        mlog("; [mods] overlay ALLOC FAILED for %s (%uB) -- retail sees resource-absent, never stock bytes",
             name, (unsigned)size);
        return 1;
    }
    std::memcpy(copy, p->bytes.data(), size);
    // Resource-first loaders may never call the loose-file seam. Record the first actual
    // successful copy in each relevant class, independently of the general hit-log cap.
    static volatile LONG banki_logged = 0, bank_logged = 0, tileset_logged = 0, map_logged = 0;
    const size_t         name_len = std::strlen(name);
    volatile LONG       *once     = nullptr;
    const char          *kind     = nullptr;
    if (_stricmp(name, "banki.dat") == 0) {
        once = &banki_logged;
        kind = "banki";
    } else if (name_len >= 4 && _stricmp(name + name_len - 4, ".bnk") == 0) {
        once = &bank_logged;
        kind = "bank";
    } else if (name_len >= 4 && _stricmp(name + name_len - 4, ".tlo") == 0) {
        once = &tileset_logged;
        kind = "tileset";
    } else if (name_len >= 4 && _stricmp(name + name_len - 4, ".mpm") == 0) {
        once = &map_logged;
        kind = "map";
    }
    if (once && InterlockedCompareExchange(once, 1, 0) == 0)
        mlog("; [mods] overlay %s copy %.100s %uB", kind, name, (unsigned)size);
    return reinterpret_cast<uintptr_t>(copy);
}

uint32_t __cdecl overlay_lookup_size(const char *name) {
    const mh::mods::Payload *p = overlay_winner(name);
    if (p == nullptr || p->bytes.empty()) return 0; // 0 = miss: the stub falls through to retail
    mh::call::ToLower(const_cast<char *>(name));
    return static_cast<uint32_t>(p->bytes.size());
}

// ---- THE BANKI GUARD (adapter duty, BEFORE publish) ----------------------------------------------
//
// A mod may ship a full BANKI.DAT plus only the banks it changed; untouched banks keep coming from
// the base pack. The catalog validates every winning OVERRIDDEN bank against the winning BANKI
// (mh::bnk::SplitBnk). What only this adapter can check -- and must, before anything publishes --
// is the STOCK half the pure catalog cannot see:
//   * a bank FILE override must name a retail slot (bank_NN below the stock bank count);
//   * the winning BANKI must keep the stock BANK COUNT (the engine's bank table is fixed);
//   * every slot the set does NOT override must keep its stock 12-byte descriptor verbatim -- an
//     overridden BANKI that quietly resizes an untouched bank desyncs the engine's loader;
//   * an OVERRIDDEN slot must stay within its retail sprite budget (stock indices_size -- the
//     engine's tables are sized for the retail counts), enforced HERE before publication, not
//     deferred to the engine at load.
//
// At the boot arm the game's packs are NOT loaded yet -- that is the timing's whole point -- so
// the stock side is read straight off the install's .nam/.rsr pair on disk. Measured on the stock
// install 2026-11-01: BANKI.DAT lives in mh.rsr (not mh_ex), stored LZW (511 B -> 2400 B = 200
// x 12-byte slots). The unwrap goes through mh::lzw -- the PURE codec (mh_common/lzw.cpp, the
// game's own 0x004f4b7e reimplemented and byte-verified by savetest's fixtures) -- NOT through a
// hand shim of the game function: its generated prototype is MH_UNAVAILABLE for a reason (the
// function's own RET contradicts a C prototype) and its call site is stack-arg
// (PUSH size,dst,src / CALL / ADD ESP,0xC). The pure buffer codec is the proven path.
mh::lzw::decoder_state g_lzw_dec; // decompress re-seeds the dictionary per payload; nothing carries

#pragma pack(push, 1)
// The 64-byte .nam record (src/formats/unpack.py; lang_pack.cpp's nam_entry shape).
struct rsr_nam_entry {
    char     name[47];
    char     type[5];
    uint32_t offset, size, final_size;
};
#pragma pack(pop)
static_assert(sizeof(rsr_nam_entry) == 64, "rsr nam entry is 64 bytes");
// BANKI records are mh::bnk::BankiEntry (mh_bnk.h -- header-only use; no codec object is linked:
// SplitBnk stays in the catalog's validation, this TU only compares 12-byte descriptors).

// One pack payload, read straight off `<game_dir><pack>.nam/.rsr` (game_dir carries the trailing
// separator). BOUNDED end to end: the .nam name is copied through a 48-byte NUL-terminated local
// (a 47-byte record name has no NUL guarantee), the payload bounds are validated BEFORE any read
// or decode, and the LZW payload's own header must claim no more than the bytes actually stored
// (mh::lzw requires the payload readable to its claimed compressed_len). Raw when stored raw;
// unwrapped through mh::lzw when not. False on any absence/refusal -- the caller refuses with
// that fact, it never guesses.
bool rsr_read_payload(const char *game_dir, const char *pack, const char *name,
                      std::vector<uint8_t> &out, bool &found) {
    found = false;
    // The path compose is bounded too: wsprintfA has no bound, so the lengths are checked first.
    const bool  absolute = (pack[0] && pack[1] == ':') || (pack[0] == '\\' && pack[1] == '\\');
    const char *prefix   = absolute ? "" : game_dir;
    if (lstrlenA(prefix) + lstrlenA(pack) + 5 >= MAX_PATH) return false;
    char nam_path[MAX_PATH], rsr_path[MAX_PATH];
    wsprintfA(nam_path, "%s%s.nam", prefix, pack);
    wsprintfA(rsr_path, "%s%s.rsr", prefix, pack);
    HANDLE nam = CreateFileA(nam_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nam == INVALID_HANDLE_VALUE) return false;
    rsr_nam_entry e;
    bool          hit = false;
    for (DWORD got = 0; !hit && ReadFile(nam, &e, sizeof(e), &got, nullptr) && got == sizeof(e);) {
        char nm[48];                       // lang_pack's find_entry idiom: a record name is NOT
        lstrcpynA(nm, e.name, sizeof(nm)); // guaranteed NUL within its 47 bytes -- terminate here
        if (lstrcmpiA(nm, name) == 0) hit = true;
    }
    CloseHandle(nam);
    if (!hit) return false;
    found = true;
    // The record's own bounds, before anything is read: a sane stored size, and a final size that
    // is at least the stored one only when LZW demands room for it -- both capped at the catalog's
    // own per-file ceiling.
    if (e.size < 3 || e.size > (64u << 20) || e.final_size > (64u << 20)) return false;

    HANDLE rsr = CreateFileA(rsr_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (rsr == INVALID_HANDLE_VALUE) return false;
    // ONE buffer for the raw path: read straight into `out` and move it out. Only an LZW payload
    // pays a second buffer, and that one is the decode destination (decode cannot overlap src).
    out.resize(e.size);
    DWORD got = 0;
    SetFilePointer(rsr, (LONG)e.offset, nullptr, FILE_BEGIN);
    const bool read_ok = ReadFile(rsr, out.data(), e.size, &got, nullptr) && got == e.size;
    CloseHandle(rsr);
    if (!read_ok) {
        out.clear();
        return false;
    }

    if (out[0] != 'L' || out[1] != 'Z' || out[2] != 'W') return true;
    if (out.size() < sizeof(mh::lzw::payload_header)) return false;

    // LZW: the pure mh::lzw codec (the game's own 0x004f4b7e, reimplemented and byte-verified by
    // savetest's fixtures). The payload carries a 12-byte header {magic, uncompressed_size,
    // compressed_len-including-header}; mh::lzw reads to the CLAIMED compressed_len, so that claim
    // is validated against the bytes actually stored before the decode runs.
    mh::lzw::payload_header hdr;
    std::memcpy(&hdr, out.data(), sizeof(hdr));
    if (hdr.magic != mh::lzw::MAGIC || hdr.compressed_len < sizeof(hdr) || hdr.compressed_len > e.size ||
        hdr.uncompressed_size != e.final_size || e.final_size == 0) {
        out.clear();
        return false;
    }
    std::vector<uint8_t> final_buf(e.final_size);
    // *io enters as the PAYLOAD byte count (only the not-LZW path reads it) and leaves as the
    // decoded count, bounded by the .nam's final_size. Anything short is a corrupt read: refuse.
    uint32_t io = e.size;
    mh::lzw::decompress(g_lzw_dec, out.data(), final_buf.data(), &io, e.final_size);
    if (io != e.final_size) {
        out.clear();
        return false;
    }
    out = std::move(final_buf);
    return true;
}

// Stock packs are not part of Snapshot. Resolve them in the engine's pack order once,
// before publishing any candidate; array positions use the catalog's native stock index.
std::array<std::uint16_t, 8> g_boot_stock_tile_counts{};
bool                         stock_tiles_ok(const char *game_dir, const char *selected_pack, std::string &why) {
    static constexpr const char *names[] = {"jungle.tlo", "kam256.tlo", "metal256.tlo", "ruins256.tlo",
                                                                    "lod256.tlo", "ksie256.tlo", "diuna256.tlo", "skaly256.tlo"};
    std::array<std::uint16_t, 8> counts{};
    for (const char *name : names) {
        const int index = mh::mods::stock_tileset_index(name);
        if (index < 1 || index > 8 || counts[index - 1]) {
            why = "inconsistent stock tileset index";
            return false;
        }
        std::vector<uint8_t> bytes;
        bool                 found = false;
        bool                 ok    = rsr_read_payload(game_dir, selected_pack, name, bytes, found);
        if (!ok && !found) ok = rsr_read_payload(game_dir, "mh", name, bytes, found);
        if (!ok || bytes.size() < 512 + 1024 || (bytes.size() - 512) % 1024 ||
            (bytes.size() - 512) / 1024 > 8192) {
            why = std::string("invalid or missing stock tileset ") + name;
            return false;
        }
        counts[index - 1] = static_cast<std::uint16_t>((bytes.size() - 512) / 1024);
    }
    g_boot_stock_tile_counts = counts;
    return true;
}

bool candidate_tileset_maps_ok(const mh::mods::Snapshot &snap, std::string &why) {
    for (const auto &winner : snap.winners) {
        const auto &name        = winner.first;
        const auto  n           = name.size();
        const bool  campaign    = n >= 3 && name.compare(n - 3, 3, ".mp") == 0;
        const bool  multiplayer = n >= 4 && name.compare(n - 4, 4, ".mpm") == 0;
        if (!campaign && !multiplayer) continue;
        const auto *p = snap.payload(winner.second);
        if (!p) {
            why = name + ": invalid winner payload index";
            return false;
        }
        std::string error;
        if (!mh::mods::validate_tileset_map(snap, p->bytes.data(), p->bytes.size(), campaign,
                                            g_boot_stock_tile_counts, error)) {
            why = name + ": " + error;
            return false;
        }
    }
    return true;
}

// cfg_ReadMapFile queries MAP verbatim as a pack resource. Only on a miss does it open
// DAT_005d01f0 (initially "dane\\") plus MAP relative to the process working directory.
bool read_planet_loose(const std::string &name, std::vector<uint8_t> &out) {
    const char  *dir = reinterpret_cast<const char *>(0x005d01f0u);
    const size_t n   = strnlen_s(dir, 128);
    if (n == 128 || n + name.size() >= 256) return false;
    const std::string path = std::string(dir, n) + name;
    HANDLE            h    = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool          ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
              static_cast<unsigned long long>(size.QuadPart) <= mh::mods::PAYLOAD_BYTES_MAX;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok        = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool planet_maps_ok(const mh::mods::Snapshot &snap, const char *game_dir, const char *selected_pack,
                    const std::vector<std::string> &maps, std::string &why) {
    for (const auto &map : maps) {
        std::string key, err;
        if (!mh::mods::canonical_resource_name(map.c_str(), key, err)) {
            why = "PLANET.MAP '" + map + "': " + err;
            return false;
        }
        std::vector<uint8_t>        base;
        const auto                 *winner = snap.winner(key);
        const std::vector<uint8_t> *bytes  = nullptr;
        if (winner) bytes = &winner->bytes;
        else {
            bool found = false;
            bool ok    = rsr_read_payload(game_dir, selected_pack, map.c_str(), base, found);
            if (!ok && !found) ok = rsr_read_payload(game_dir, "mh", map.c_str(), base, found);
            if (!ok && !found) ok = read_planet_loose(map, base);
            if (!ok) {
                why = "PLANET.MAP '" + map + "': selected resource or loose map unreadable";
                return false;
            }
            bytes = &base;
        }
        if (!mh::mods::validate_campaign_map(*bytes, key, err) ||
            !mh::mods::validate_tileset_map(snap, bytes->data(), bytes->size(), true, g_boot_stock_tile_counts, err)) {
            why = "PLANET.MAP '" + map + "': " + err;
            return false;
        }
    }
    return true;
}

// The canonical bank-file slot of a winner name: -1 = not a bank file, -2 = BANKI itself.
int bank_slot_of(const char *name) {
    const std::string_view sv(name); // size first: a short canonical name must not overread
    if (sv == "banki.dat") return -2;
    if (sv.size() < sizeof("bank_00.bnk") - 1 || std::memcmp(name, "bank_", 5) != 0) return -1;
    const char *d = name + 5;
    int         n = 0, v = 0;
    for (; *d >= '0' && *d <= '9'; ++d) {
        v = v * 10 + (*d - '0');
        if (++n > 3) return -1;
    }
    if ((n != 2 && n != 3) || std::strcmp(d, ".bnk") != 0) return -1;
    if ((n == 2) != (v < 100)) return -1; // two digits below 100, three from 100
    return v;
}

// A tree-bank override must fit the same per-kind conservative rectangles the
// generator uses. Decode once while the candidate is private, never during drawing.
bool tree_bank_extents_ok(const mh::mods::Snapshot &snap, const char *game_dir,
                          const char *selected_pack, std::string &why) {
    static constexpr const char *profiles[] = {"diuna256", "jungle", "ksie256", "lod256",
                                               "ruins256", "metal256", "skaly256"};
    std::vector<uint8_t>         stock_banki;
    const auto                  *banki   = snap.winner("banki.dat");
    const std::vector<uint8_t>  *entries = banki ? &banki->bytes : nullptr;
    for (int slot = 50; slot <= 56; ++slot) {
        char name[16];
        wsprintfA(name, "bank_%02d.bnk", slot);
        const auto *bank = snap.winner(name);
        if (!bank) continue;
        if (!entries) {
            bool found = false;
            bool ok    = rsr_read_payload(game_dir, selected_pack, "banki.dat", stock_banki, found);
            if (!ok && !found) ok = rsr_read_payload(game_dir, "mh", "banki.dat", stock_banki, found);
            if (!ok) {
                why = "cannot read stock BANKI for tree extent check";
                return false;
            }
            entries = &stock_banki;
        }
        if (entries->size() < size_t(slot + 1) * sizeof(mh::bnk::BankiEntry)) {
            why = "missing tree BANKI slot";
            return false;
        }
        mh::bnk::BankiEntry entry;
        std::memcpy(&entry, entries->data() + slot * sizeof(entry), sizeof(entry));
        try {
            const auto sections = mh::bnk::SplitBnk(bank->bytes, entry, name);
            if (sections.isV2) {
                why = std::string(name) + ": unsupported V2 tree bank";
                return false;
            }
            for (size_t i = 0; i < sections.indices1.size(); ++i) {
                const auto sprite = mh::bnk::SpriteSlice(sections, i);
                if (sprite.empty()) continue;
                const auto decoded = mh::bnk::DecodeV1(sprite, name);
                const auto meta    = mh::bnk::MetaSlice(sections, i);
                int16_t    ox, oy;
                std::memcpy(&ox, meta.data() + 4, sizeof(ox));
                std::memcpy(&oy, meta.data() + 6, sizeof(oy));
                mh_mapgen::detail::SpriteExtent limit{};
                if (!mh_mapgen::detail::sprite_extent(profiles[slot - 50], int32_t(i + 1), limit)) {
                    why = std::string(name) + ": unknown tree profile";
                    return false;
                }
                for (int y = 0; y < decoded.height; ++y)
                    for (int x = 0; x < decoded.width; ++x) {
                        const auto &pixel = decoded.pixels[size_t(y) * decoded.width + x];
                        if (pixel.kind == uint8_t(mh::bnk::PxKindV1::Transparent)) continue;
                        if (oy - y > limit.up || ox - x > limit.left ||
                            x - ox > limit.right || y - oy > limit.down) {
                            why = std::string(name) + ": sprite " + std::to_string(i + 1) +
                                  " exceeds generator tree footprint";
                            return false;
                        }
                    }
            }
        } catch (const std::exception &e) {
            why = std::string(name) + ": " + e.what();
            return false;
        }
    }
    return true;
}

// The whole-set bank check. True = this set may publish. False = refuse, `why` names it.
bool bank_guard_ok(const mh::mods::Snapshot &snap, const char *game_dir, const char *selected_pack, std::string &why) {
    const mh::mods::Payload *win_banki = snap.winner("banki.dat");
    bool                     any_bank  = win_banki != nullptr;
    int                      max_slot  = -1;
    for (const auto &kv : snap.winners) {
        const int slot = bank_slot_of(kv.first.c_str());
        if (slot >= 0) {
            any_bank = true;
            if (slot > max_slot) max_slot = slot;
        }
    }
    if (!any_bank) return true; // the no-bank fast path (the proof's probe takes this one)

    std::vector<uint8_t> stock_raw;
    // mh_ex first -- the game's own load order -- then mh; first match wins, like rsr_GetFileEntry.
    bool found   = false;
    bool read_ok = rsr_read_payload(game_dir, selected_pack, "banki.dat", stock_raw, found);
    if (!read_ok && !found) read_ok = rsr_read_payload(game_dir, "mh", "banki.dat", stock_raw, found);
    if (!read_ok) {
        why = "a BANKI/bank payload is mounted but the stock BANKI.DAT is unreadable from the install's packs";
        return false;
    }
    if (stock_raw.size() < 12 || stock_raw.size() % 12 != 0) {
        why = "the stock BANKI.DAT is not a whole 12-byte slot table";
        return false;
    }
    const size_t stock_n = stock_raw.size() / 12;
    if (max_slot >= (int)stock_n) {
        why = "a bank_NN override names a slot past the stock bank count";
        return false;
    }

    // The descriptors the set would publish: the winner's when it overrides BANKI, stock's when
    // not (in which case the untouched-slot rule below is true by construction).
    const std::vector<uint8_t> *win_raw = &stock_raw;
    if (win_banki != nullptr) {
        if (win_banki->bytes.size() < 12 || win_banki->bytes.size() % 12 != 0) {
            why = "the winning BANKI.DAT is not a whole 12-byte slot table";
            return false;
        }
        if (win_banki->bytes.size() != stock_raw.size()) {
            why = "the winning BANKI.DAT changes the bank count (the engine's bank table is fixed)";
            return false;
        }
        win_raw = &win_banki->bytes;
    }
    for (size_t i = 0; i < stock_n; ++i) {
        char slot_name[16];
        wsprintfA(slot_name, i < 100 ? "bank_%02u.bnk" : "bank_%03u.bnk", (unsigned)i);
        mh::bnk::BankiEntry win, stock;
        std::memcpy(&win, win_raw->data() + i * 12, 12);
        std::memcpy(&stock, stock_raw.data() + i * 12, 12);
        if (snap.winner(slot_name) != nullptr) {
            // OVERRIDDEN (the catalog proved the file internally consistent): DATAEXT1 is
            // conservative about the sprite COUNT -- unchanged, or the set refuses. The stock
            // count is what the engine's per-slot tables and every unchanged cfg reference are
            // sized against; shrinking would leave references pointing past the new bank, growing
            // past the retail budget, and "the renderer bounds-checks" is engine evidence the
            // bank cut can add later -- this proof assumes neither. The section SIZES may differ
            // (redrawn sprites at the same count re-encode to different RLE lengths); SplitBnk
            // validated those against the winner's own descriptor.
            if (win.indicesSize != stock.indicesSize) {
                why = "bank slot " + std::string(slot_name) +
                      " changes its sprite count (DATAEXT1 keeps the retail count per slot)";
                return false;
            }
            continue;
        }
        if (std::memcmp(&win, &stock, 12) != 0) {
            why = "the winning BANKI.DAT resizes an untouched bank slot";
            return false;
        }
    }
    return true;
}

} // namespace

// ---- THE NAKED STUBS -----------------------------------------------------------------------------
//
// EAX is the __watcall argument. The HIT path RETs straight to the retail CALLER with the answer
// in EAX -- the original body never runs, so the overlay's game-heap copy is what retail's own
// utils_free disposes of. The MISS path lands on the toolkit thunk: the displaced 8-byte prologue
// re-executes and the jmp reaches entry+8, where the original CALL (assert) and the rest of the
// body run unmodified -- nothing was copied or relocated by hand.
// clang-format off
__declspec(naked) void rsr_ptr_detour() {
    // STACK DIAGRAM (the bug this layout exists to avoid: the __cdecl helper reads its argument
    // at [esp+4] after the CALL, so the ARG must be the LAST push -- a bare saved register there
    // hands the helper EDX as "name"):
    //   push eax : [esp+0]=name
    //   push ecx : [esp+0]=ecx   [esp+4]=name
    //   push edx : [esp+0]=edx   [esp+4]=ecx   [esp+8]=name
    //   push eax : [esp+0]=arg   [esp+4]=edx   [esp+8]=ecx   [esp+12]=name
    //   call/add : [esp+0]=edx   [esp+4]=ecx   [esp+8]=name     (EAX = the answer)
    //   xchg eax,[esp+8]         -> [esp+8]=answer, EAX = the name again
    __asm {
        push  eax                           // the name -- the miss path's EAX restore
        push  ecx                           // the native entries preserve these -- match them
        push  edx
        push  eax                           // the __cdecl argument (a copy of the name)
        call  overlay_lookup_copy           // -> EAX: 0 miss / 1 alloc-failed / else the copy
        add   esp, 4                        // __cdecl: drop the argument
        xchg  eax, [esp + 8]                // name slot = the answer; EAX = the name
        cmp   dword ptr [esp + 8], 0
        jne   p_test1
        pop   edx                           // miss: all registers native, EAX = the name
        pop   ecx
        add   esp, 4                        // drop the name slot
        jmp   p_retail
    p_test1:
        cmp   dword ptr [esp + 8], 1
        je    p_allocfail
        pop   edx                           // hit: restore, EAX = the copy
        pop   ecx
        pop   eax
        ret                                 // straight back to the retail caller
    p_allocfail:
        pop   edx                           // restore the native registers FIRST
        pop   ecx
        add   esp, 4                        // drop the name slot
        xor   eax, eax                      // resource-absent to retail: NEVER the stock bytes
        ret
    p_retail:
        jmp   dword ptr [g_ptr_tramp]       // thunk: push ebp / mov ebp,esp / push 44h / jmp +8
    }
}

__declspec(naked) void rsr_size_detour() {
    // Same layout as the ptr detour (same bug, same fix): the size helper is __cdecl(const char*),
    // so the argument is the last push, and ECX/EDX survive every exit.
    __asm {
        push  eax                           // the name -- the miss path's EAX restore
        push  ecx
        push  edx
        push  eax                           // the __cdecl argument
        call  overlay_lookup_size           // -> EAX: byte length, 0 = miss
        add   esp, 4                        // __cdecl: drop the argument
        xchg  eax, [esp + 8]                // name slot = the size; EAX = the name
        cmp   dword ptr [esp + 8], 0
        jne   s_hit
        pop   edx
        pop   ecx
        add   esp, 4
        jmp   s_retail
    s_hit:
        pop   edx
        pop   ecx
        pop   eax                           // EAX = the length; back to the retail caller
        ret
    s_retail:
        jmp   dword ptr [g_size_tramp]      // thunk: push ebp / mov ebp,esp / push 2Ch / jmp +8
    }
}
// clang-format on

namespace {

// ---- the installs --------------------------------------------------------------------------------

// The toolkit trampoline (stolen 8, exclusive claim) behind a STRICTER pre-check: the exact 8
// entry bytes including the assert-size byte. The pre-check is the documented expect_prologue=0
// opt-out shape; refusals still adjudicate through the toolkit (report_if_displaced names a
// promoted/owned entry as such instead of blaming the build -- U30/U31).
bool res_detour_install(uintptr_t site, const uint8_t *expect8, void *detour, void **tramp,
                        const char *what) {
    if (std::memcmp(reinterpret_cast<const void *>(site), expect8, 8) != 0) {
        if (!mh::hook::report_if_displaced(site, what))
            mlog("; [mods] %s detour at 0x%08x REFUSED: not the stock EN entry bytes -- overlay not armed",
                 what, (unsigned)site);
        return false;
    }
    // Refusals (promoted body, owned entry, protect failure) log and register inside the primitive.
    return mh::hook::install_trampoline(site, detour, tramp, 8, mh::hook::entry_claim::exclusive,
                                        what, 0);
}

} // namespace

// ---- THE HEAVY ARM -- runs from the boot stub: post-loader-lock, pre-first-pack --------------------
//
// Detours are transparent until the single release publication of the prepared snapshot.
// Map rows also test that same snapshot; readiness failure leaves the entire set inert.

// Boot failure is copied now and drawn once the normal present owner has fonts available.
// Keep the diagnostic bounded; no modal window and no resource/config publication on failure.
namespace {
wchar_t g_config_reason[200] = {};
bool    g_config_refused     = false;
bool    g_config_seen_root   = false;
bool    g_config_preparing   = false;

int config_notice_measure(void *, const wchar_t *text, int length) {
    wchar_t temp[256];
    if (length > 255) length = 255;
    for (int i = 0; i < length; ++i) temp[i] = text[i];
    temp[length] = 0;
    return static_cast<int>(mh::call::llm_gfx_font_measure_text(reinterpret_cast<uint16_t *>(temp)));
}
} // namespace
extern "C" void MH_Mods_ConfigRefused(const char *reason) {
    char short_reason[190];
    lstrcpynA(short_reason, reason ? reason : "", sizeof(short_reason));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, short_reason, -1, g_config_reason, 200))
        MultiByteToWideChar(CP_ACP, 0, short_reason, -1, g_config_reason, 200);
    g_config_refused   = true;
    g_config_seen_root = false;
}
extern "C" void MH_Mods_ConfigNoticePresent(void) {
    if (!g_config_refused) return;
    // Same root-menu predicate as net_lockstep.cpp's spectator_tick: an active dialog,
    // browser or submenu is not the root menu even though GAME_MODE remains 3.
    const bool root = *reinterpret_cast<const uint8_t *>(mh::addr::_G_LLM_GAME_MODE) == 3 &&
                      *reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_UI_ACTIVE_DIALOG) == 0 &&
                      *reinterpret_cast<const uint32_t *>(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST) == mh::addr::mp_menu_container;
    if (!root) {
        if (g_config_seen_root) g_config_refused = false;
        return;
    }
    g_config_seen_root = true;
    auto      fonts    = reinterpret_cast<const uint8_t *const *>(0x005d8198u);
    const int width    = *reinterpret_cast<const int *>(mh::addr::WindowWidth) - 16;
    if (!fonts[0] || width <= 0) return;
    // font_select changes these three render globals; restore the exact previous state.
    auto          &height     = *reinterpret_cast<uint32_t *>(0x005d6184u);
    auto          &glyphs     = *reinterpret_cast<uint32_t *>(0x005d81b4u);
    auto          &charmap    = *reinterpret_cast<uint32_t *>(0x005d81f4u);
    const uint32_t old_height = height, old_glyphs = glyphs, old_charmap = charmap;
    mh::call::llm_gfx_font_select(0);
    int line_h = mh::call::llm_gfx_font_get_line_height();
    if (line_h <= 0 || line_h > 64) line_h = 10;
    wchar_t message[256], wrapped[512];
    _snwprintf_s(message, _countof(message), _TRUNCATE, L"%s: %s",
                 mh::ui::tr(mh::ui::Str::MODS_CONFIG_REFUSED),
                 g_config_reason[0] ? g_config_reason : mh::ui::tr(mh::ui::Str::MODS_CONFIG_DETAILS));
    mh::ui::WrapReport report{};
    mh::ui::wrap_text(message, wrapped, _countof(wrapped), width, 4, config_notice_measure, nullptr, &report);
    wchar_t *line = wrapped;
    for (int row = 0; line && *line && row < 4; ++row) {
        wchar_t *next = std::wcschr(line, L'\n');
        if (next) *next++ = 0;
        mh::call::llm_gfx_draw_text_rgb(8, 8 + row * line_h, reinterpret_cast<uint16_t *>(line), 255, 96, 96);
        line = next;
    }
    height  = old_height;
    glyphs  = old_glyphs;
    charmap = old_charmap;
}

namespace mh::seams::maps {

// Runtime publication is independent of the boot candidate's private count array.
// InitOnce serializes readers and only publishes after all eight pack entries exist;
// FALSE leaves initialization pending so an early menu request can retry later.
static std::array<std::uint16_t, 8> g_runtime_stock_tile_counts{};
static BOOL CALLBACK                init_runtime_stock_counts(PINIT_ONCE, PVOID, PVOID *) {
    static constexpr const char *names[] = {"jungle.tlo", "kam256.tlo", "metal256.tlo", "ruins256.tlo",
                                                           "lod256.tlo", "ksie256.tlo", "diuna256.tlo", "skaly256.tlo"};
    std::array<std::uint16_t, 8> counts{};
    for (const char *name : names) {
        const int index = mh::mods::stock_tileset_index(name);
        if (index < 1 || index > 8 || counts[index - 1]) return FALSE;
        char query[32];
        lstrcpynA(query, name, sizeof(query));
        void          *pack  = nullptr;
        const uint8_t *entry = (const uint8_t *)mh::call::rsr_GetFileEntry(query, &pack);
        const uint32_t size  = entry ? *reinterpret_cast<const uint32_t *>(entry + 60) : 0;
        if (size < 512 + 1024 || (size - 512) % 1024 || (size - 512) / 1024 > 8192) return FALSE;
        counts[index - 1] = static_cast<std::uint16_t>((size - 512) / 1024);
    }
    g_runtime_stock_tile_counts = counts;
    return TRUE;
}

bool validate_runtime_map(const uint8_t *bytes, size_t size, bool campaign, std::string &error,
                          bool *retryable) {
    if (retryable) *retryable = false;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!InitOnceExecuteOnce(&once, init_runtime_stock_counts, nullptr, nullptr)) {
        error = "stock tileset counts unavailable";
        if (retryable) *retryable = true;
        return false;
    }
    const auto                     *snapshot = mh::mods::mounted_snapshot();
    static const mh::mods::Snapshot stock;
    return mh::mods::validate_tileset_map(snapshot ? *snapshot : stock, bytes, size, campaign,
                                          g_runtime_stock_tile_counts, error);
}

} // namespace mh::seams::maps

static void mods_boot_arm_impl(const char *selected_pack) {
    if (g_arm_done) return;
    g_arm_done = true;
    if (!g_splice_armed || !g_ids[0]) return; // stock or already refused in DllMain: nothing to do
    if (!mh::en_build_ok()) return;

    // (1) THE DETOURS, inert. A refusal here leaves them unmounted-transparent and the boot stock.
    if (!res_detour_install(ADDR_RES_SIZE, SIZE_EXPECT, (void *)&rsr_size_detour, &g_size_tramp,
                            "rsr_GetFileRealSize") ||
        !res_detour_install(ADDR_RES_PTR, PTR_EXPECT, (void *)&rsr_ptr_detour, &g_ptr_tramp,
                            "GetResourseFilePtr")) {
        mlog("; [mods] enabled=%s REFUSED: a resource detour site refused -- stock packs serve everything",
             g_ids);
        return; // nothing mounted: MH_Mods_ContentDigest answers the stock digest from its null branch
    }

    // (2) PREPARE (pure) and (3) MOUNT. A failure snapshot mounts unchanged -- the stock digest and
    // no payloads, stock IS what runs -- with the requested ids kept visible in the refusal line.
    char game_dir[MAX_PATH];
    if (!exe_dir(game_dir, MAX_PATH)) {
        mlog("; [mods] executable directory unavailable -- stock fallback");
        return;
    }
    mh::mods::Snapshot snap = mh::mods::prepare(game_dir, g_ids);
    if (!snap.ok) {
        mlog("; [mods] enabled=%s REFUSED: %s -- stock packs serve everything", g_ids,
             snap.error.c_str());
        if (snap.config_error) MH_Mods_ConfigRefused(snap.error.c_str());
        return;
    }
    std::string tileset_why;
    if (!stock_tiles_ok(game_dir, selected_pack, tileset_why) ||
        !candidate_tileset_maps_ok(snap, tileset_why)) {
        mlog("; [mods] enabled=%s REFUSED: %s -- stock packs serve everything", g_ids, tileset_why.c_str());
        return;
    }
    // (2b) THE BANKI GUARD -- the adapter's stock-side half, before anything publishes. A refusal
    // here publishes NOTHING: the null-mounted stock digest is the one stock identity.
    std::string bank_why;
    if (!bank_guard_ok(snap, game_dir, selected_pack, bank_why)) {
        mlog("; [mods] enabled=%s REFUSED: %s -- stock packs serve everything", g_ids,
             bank_why.c_str());
        return;
    }
    if (!tree_bank_extents_ok(snap, game_dir, selected_pack, bank_why)) {
        mlog("; [mods] enabled=%s REFUSED: %s -- stock packs serve everything", g_ids, bank_why.c_str());
        return;
    }
    // Resolve the very same first-winning pack entries as the engine, before its first parse.
    // Neither raw data overrides nor the selected locale output may bypass this transaction.
    if (!snap.config_inputs.empty()) {
        g_config_preparing = true;
        std::string why;
        if (!mh_mod_picker_present_ready()) {
            mlog("; [mods] config REFUSED: present hook unavailable; cannot display a refusal notice");
            return;
        }
        {
            std::vector<uint8_t> init, lang;
            auto                 read_base = [&](const char *name, std::vector<uint8_t> &out) {
                bool found = false;
                bool ok    = rsr_read_payload(game_dir, selected_pack, name, out, found);
                if (!ok && !found) ok = rsr_read_payload(game_dir, "mh", name, out, found);
                return ok;
            };
            if (!read_base("init\\init.cfg", init)) why = "Cannot read selected base init\\init.cfg";
            else if (!read_base("init\\initlang.cfg", lang)) why = "Cannot read selected base init\\initlang.cfg";
            else {
                const char *locale              = MH_LangPack_Locale(lang.data(), static_cast<uint32_t>(lang.size()));
                bool        has_language_inputs = false;
                for (const auto &p : snap.config_inputs)
                    if (p.name.starts_with("lang\\")) has_language_inputs = true;
                if ((!locale || !*locale) && has_language_inputs)
                    why = "Unknown base language: set [lang] locale in mh_net.ini or [pack] locale in pack.ini";
                std::vector<std::string>          sources;
                std::vector<mh::mods::cfg::Input> inputs;
                sources.reserve(snap.config_inputs.size());
                inputs.reserve(snap.config_inputs.size());
                for (const auto &p : snap.config_inputs) {
                    sources.push_back("mods/" + snap.mods[p.mod_index].id + "/" + p.name);
                    inputs.push_back({sources.back(), p.name, p.bytes, p.mod_index});
                }
                if (why.empty()) {
                    auto result = mh::mods::cfg::prepare(init, lang, inputs, locale ? locale : "");
                    if (!result.ok) why = result.error.empty() ? "Config patch preparation failed" : result.error;
                    else if (result.passthrough || result.init.empty() || result.lang.empty())
                        why = "Config patch preparation returned no generated configuration";
                    else if (!planet_maps_ok(snap, game_dir, selected_pack, result.changed_planet_maps, why)) {
                        // The complete candidate remains private on any invalid map.
                    } else mh::mods::finalize_config(snap, std::move(result.init), std::move(result.lang));
                }
            }
        }
        if (!why.empty()) {
            mlog("; [mods] enabled=%s CONFIG REFUSED: %s -- stock packs serve everything", g_ids, why.c_str());
            MH_Mods_ConfigRefused(why.c_str());
            return;
        }
    }
    g_config_preparing = false;
    // (2c) THE REQUIRED SIDE HOOKS, composed DIRECTLY -- no callback surface. The map resolver
    // must arm for THIS candidate (picker/preview/transfer/JOIN admission share its winning
    // bytes), and the save carrier's hook sites must be in: a half-mounted set could neither keep
    // the map bytes one identity nor label its saves. Either refusal = the WHOLE set falls back:
    // nothing mounts, the digest stays nomod, the ids stay visible in the line.
    if (!mh::mods::save::hooks_ready()) {
        mlog("; [mods] enabled=%s REFUSED: save metadata hooks unavailable -- stock packs serve everything", g_ids);
        return;
    }
    mh::tileset_runtime::Prepared tileset_graphics;
    std::string                   graphics_why;
    if (!mh::tileset_runtime::prepare(snap, tileset_graphics, graphics_why) ||
        !mh::tileset_runtime::install()) {
        mlog("; [mods] enabled=%s REFUSED: tileset graphics preparation failed: %s", g_ids,
             graphics_why.c_str());
        return;
    }
    if (!mh::seams::maps::mod_arm(&snap)) {
        mlog("; [mods] enabled=%s REFUSED: the map resolver did not arm for this set -- stock packs serve everything",
             g_ids);
        return;
    }
    // The armed line's figures are read off the snapshot BEFORE it is handed to mount_snapshot
    // (the move). `files` is the winner count; `total` is the bytes the winners hold.
    const int          files = (int)snap.winners.size();
    unsigned long long total = 0;
    for (const auto &kv : snap.winners) {
        const mh::mods::Payload *p = snap.payload(kv.second);
        if (p != nullptr) total += p->bytes.size();
    }
    // The snapshot move allocates and may throw. Commit it first; graphics publication
    // is a nonthrowing vector move on this boot thread, before graphics can run.
    static_assert(noexcept(mh::tileset_runtime::publish(std::move(tileset_graphics))));
    mh::mods::mount_snapshot(std::move(snap));
    mh::tileset_runtime::publish(std::move(tileset_graphics));
    mh::mods::save::install_active_set_source();

    // Resource, map and digest readers now observe the same published snapshot.
    // Past this point both entry points answer from the snapshot.
    mlog("; [mods] overlay armed: ids=%s files=%d bytes=%uMiB -- GetResourseFilePtr/"
         "rsr_GetFileRealSize answer the snapshot (hits are logged as they happen)",
         g_ids, files, (unsigned)(total >> 20));
}

static void __cdecl mods_boot_arm(const char *selected_pack) {
    try {
        mods_boot_arm_impl(selected_pack);
    } catch (...) {
        mlog("; [mods] preparation failed: stock fallback");
        if (g_config_preparing)
            MH_Mods_ConfigRefused(nullptr);
    }
}

// ---- THE BOOT SPLICE STUB ------------------------------------------------------------------------
//
// pushad/pushfd keep every register and the direction flag exactly as the retail MOV EAX,s_mh_ex
// left them (lang_pack may have repointed that operand -- this site is disjoint from it and
// composes with an armed language pack). The displaced CALL re-executes via its fixed target, EAX
// carrying the pack name exactly as retail left it, and the jmp lands on the untouched resume.
// clang-format off
__declspec(naked) void mods_boot_arm_stub() {
    __asm {
        pushad
        pushfd
        cld
        push  eax
        call  mods_boot_arm                 // selected pack passed in EAX at original CALL
        add   esp, 4
        popfd
        popad
        call  dword ptr [g_fn_try_read]     // the displaced `call rsr_TryReadRsrFile`, EAX untouched
        jmp   dword ptr [g_boot_tramp]      // 0x004c3dc4, right after the original call site
    }
}
// clang-format on

// ---- THE DLLMAIN-SIDE INSTALL (MH_Seam_Init, beside MH_LangPack_Install) --------------------------

extern "C" int MH_Mods_Boot_Splice_Install(void) {
    if (g_splice_done) return g_splice_armed;
    g_splice_done = true;
    if (!mh::en_build_ok()) return 0; // EN-only: the site is an EN VA

    char ini[MAX_PATH];
    mh::cfgdir::ini_path(ini);
    const DWORD ids_len = mh::config::read_ini_string("mods", "enabled", "", g_ids, sizeof(g_ids), ini);
    if (ids_len >= sizeof(g_ids) - 1) {
        mlog("; [mods] enabled list too long -- stock fallback");
        return 0;
    }
    // STOCK: say nothing at all (the lang_pack rule). No key -- no write, no line, a byte-identical
    // boot. A key that names ids the catalog cannot mount is refused LATER, at the boot splice,
    // where the failure is logged and the boot stays stock.
    if (!g_ids[0]) return 0;

    uint8_t repl[5];
    repl[0] = 0xE9;
    const int32_t rel =
        (int32_t)(reinterpret_cast<uintptr_t>(&mods_boot_arm_stub) - (ADDR_BOOT_CALL + 5));
    std::memcpy(repl + 1, &rel, 4);
    if (!mh::hook::patch_bytes_guarded(ADDR_BOOT_CALL, BOOT_EXPECT, repl, 5)) {
        mlog("; [mods] enabled=%s NOT armed: the first-pack call at 0x%08x is not the expected "
             "bytes (already patched, or not the stock EN exe) -- stock boot",
             g_ids, (unsigned)ADDR_BOOT_CALL);
        return 0;
    }
    g_splice_armed = true;
    return 1;
}

// ---- THE EXPORTS ---------------------------------------------------------------------------------

extern "C" int MH_Mods_Mounted(void) { return mh::mods::mounted_snapshot() != nullptr; }

extern "C" int MH_Mods_Resource_Find(const char *game_rel_name, const uint8_t **out_bytes,
                                     uint32_t *out_size) {
    const mh::mods::Payload *p =
        overlay_winner(game_rel_name);
    if (p == nullptr || p->bytes.empty()) return 0;
    if (out_bytes != nullptr) *out_bytes = p->bytes.data();
    if (out_size != nullptr) *out_size = (uint32_t)p->bytes.size();
    return 1;
}

extern "C" void MH_Mods_ContentDigest(unsigned char out[32]) {
    const mh::mods::Snapshot *s = mh::mods::mounted_snapshot();
    const uint8_t            *d = (s != nullptr) ? s->digest : mh::mods::nomod_digest();
    std::memcpy(out, d, 32);
}
