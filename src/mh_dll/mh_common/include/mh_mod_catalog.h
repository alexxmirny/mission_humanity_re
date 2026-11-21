//
// mh_mod_catalog.h -- DATAEXT1/2: the pure, immutable boot-time mod catalog.
//
// One boot preparation turns the launcher's ordered enabled list plus the on-disk
// `<game>/mods/<id>/` trees into an owned Snapshot: ordered mod identities, every accepted payload
// as an actual byte copy (shadowed ones included), the per-name winners with provenance, and the
// version-framed SHA-256 content identity. Publication happens only after preparation finishes:
// nothing in here touches the filesystem after `prepare` returns, and there is no tick/draw I/O,
// ever. Callers hold const refs and look up; the snapshot is immutable for the process lifetime.
//
// THE FAILURE MODEL IS WHOLE-SET. Any problem with ANY enabled mod -- a missing folder, a malformed
// manifest, an alias collision, a reparse point, a DIRECT-file class, a bad bank/tileset/map --
// fails the ENTIRE active set: no half-applied resources/config. The failure Snapshot
// carries a readable `error`, a `failed` class, and the requested ids (kept visible for
// correction); its `digest` is the actual no-mod stock digest, because stock is what will really
// mount. A caller that wants the stock behaviour mounts the failure snapshot unchanged.
//
// WHAT IS NOT HERE. No game addresses, no resource adapter, no CFG parsing (raw INIT.CFG /
// INITLANG.CFG overrides are refused), no launcher, no MP wire format. The
// adapter publishes `mount_snapshot`'s result at the boot seam; the shared C export
// MH_Mods_ContentDigest(out) copies `mounted_snapshot()->digest` and lives with the adapter.
//
// ---- THE CLASS SURFACE (measured against the pristine EN NAM census, 2026-11-01: mh.rsr 843
// entries -- gfx 292, dmp 224, sam 144, bnk 64, mp 31, scr 26, dat 16, flc 12, tlo 9, fnt 8,
// prz 7, wav 4, map 3, cfg 2, txt 1; mh_ex.rsr 71) ------------------------------------------------
//
// VALIDATED (a byte that fails its own format fails the whole active set before publication):
//   banki.dat / bank_NN.bnk  sprite banks -- the ASSEMBLED PAIR over the winner set, with mh::bnk's
//                            own validators (LoadBanki rules + SplitBnk): every WINNING overridden
//                            bank must satisfy the WINNING BANKI descriptor. A typical mod ships a
//                            full BANKI.DAT plus only changed banks, untouched banks riding the
//                            base pack -- so descriptors of UN-overridden banks under a MODIFIED
//                            winning BANKI, the engine's fixed bank count and the sprite budgets
//                            are the ADAPTER's pre-publish checks (against stock BANKI on the pack
//                            side), whole-set refusals all the same.
//   *.tlo                    tilesets -- boundary-safe only: 512-byte RGB565 palette + tile bytes
//                            at stride 1024 (32x32 planet) or 768 (32x24 tactical floor)
//                            with 1..8192 tiles under EVERY stride the size
//                            divides by. Deep TLO semantics arrive with TILESET1.
//   *.mpm                    MP/picker maps -- FULL mh_mapgen::read_mpm parse (the existing codec;
//                            never a hand-written second header). Loose .mpm is not in the pack
//                            but is the required new-map format.
//   *.mp                     strategic CAMPAIGN planet maps -- the loader's own ARITHMETIC only
//                            with a "ver" prefix, dimensions within 1..1024 so the plane
//                            formula cannot overflow, f3 >= 1, exact plane-formula size with the
//                            footer inside its [0,255] slack. No invented square-only or
//                            shipped-tier rule: the retail loader stays the semantic authority.
// ACCEPTED AS PLAIN BYTES (the ordinary loader parses them; NOT refused, NOT re-parsed here):
//   .gfx .dmp .sam .scr .dat .flc .fnt .prz .wav .map .txt and any other data/ file -- menu
//   backgrounds, AI scripts, fonts, sounds. AI-size or sprite-budget exceptions are the
//   publication side's business.
// DIAGNOSED, NEVER MOUNTED (engine reaches these OUTSIDE the pack overlay, so a "mounted" claim
// would be a lie):
//   msgs.dat, *.avi          DIRECT-file classes (utils_open_file / media seams) -- unsupported
//                            direct classes, active-set failure naming the file.
//   init.cfg, initlang.cfg, init\\init.cfg, init\\initlang.cfg   RESERVED; a raw
//                            whole-config override is never an unchecked bypass.
//   cfg/patch.cfg and lang/<locale>.cfg are controlled patch inputs, never raw resources.
//   any other mod-root entry outside {mod.ini, data/, cfg/, lang/} is refused.
//
// IDENTITY CONTRACT (frozen with the launcher, 2026-11-01):
//   mod.ini is UTF-8 (optional BOM), standard INI: first `[mod]` section, FIRST occurrence of a
//   duplicated key wins, `;` comments (full-line and trailing). Required keys: format=1, id
//   (lowercase [a-z0-9_-], 1..32, exactly the folder name; '-' and '_' are distinct), name and
//   version (each 1..NAME_MAX bytes UTF-8, no control bytes). Optional: description (<=
//   DESCRIPTION_MAX bytes). BOUNDS FROZEN WITH THE LAUNCHER 2026-11-01: name 64 / version 64 /
//   description 256 UTF-8 bytes, control CODEPOINTS forbidden -- ASCII controls AND C1
//   (U+0080..U+009F), matching the launcher's char::is_control so both accept the same metadata.
//   Canonical resource names are ASCII lowercase with backslash
//   separators; `/` vs `\` (and letter case) are aliases, so two files of ONE mod that fold to
//   the same canonical name are an alias failure. Canonical names are capped at 46 bytes -- the
//   rsr nam table's own name field (nam_entry.name is 46 chars + NUL; measured pack names max 21).
//
// THE DIGEST (the one MP identity). SHA-256 over a version-framed, length-prefixed stream:
//   "MHMODSDIGEST1" | u32 mod_count | per mod (enabled order):
//       u32 id_len id | u32 name_len name | u32 version_len version
//   u32 payload_count | per payload (sorted by canonical name, then mod index):
//       u32 name_len name | u32 mod_index | u32 byte_len | bytes
// All integers little-endian. Included: every ACCEPTED payload of every enabled mod, INCLUDING
// shadowed copies (two installs that differ in shadowed bytes play different content -- cosmetic
// differences count under the strict-match rule), and the PARSED canonical manifest identity
// INCLUDING the description: comments and surrounding whitespace normalize away, but any content
// difference in id/name/version/description is a different identity. Excluded: absolute paths,
// mtimes, cache bytes, and any locale-derived text (human metadata does not vary with the
// selected language). The empty set hashes the frame with both
// counts zero -- an explicit constant, never a zero bypass, and it is also every failure's digest.
// For nonempty config_inputs, MHMODSCFG1 frames this digest, all ordered source config inputs,
// and generated INIT bytes; selected INITLANG bytes are excluded. No-input identity is unchanged.
// The stream is fed piecewise through mh_net_proto's streaming SHA-256 (same function as the
// one-shot; no second multi-megabyte concatenation buffer is allocated).
//
// BOUNDS (DoS safety, not format law; the largest retail resource measures 2,302,348 bytes):
// PAYLOAD_BYTES_MAX per file, SET_BYTES_MAX / SET_PAYLOADS_MAX for the whole accepted set,
// MOD_PAYLOADS_MAX files per mod. Exceeding one is an active-set failure with the reason spelled
// out. The NAME_MAX / DESCRIPTION_MAX manifest bounds are proposed and sent to the launcher for
// freezing -- they are the only two numbers in this contract the launcher has not yet acked.
//
// THREADING. `prepare` runs once at the boot splice, single-threaded. `mount_snapshot` publishes
// once; `mounted_snapshot` is then a lock-free const read. There is no re-mount, no hot reload.
//
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "mh_tileset.h"

namespace mh::mods {

// ---- the frozen bounds ------------------------------------------------------------------------

inline constexpr unsigned    ID_MAX            = 32;           // [a-z0-9_-]{1..32}; '-' and '_' distinct
inline constexpr unsigned    NAME_MAX          = 64;           // mod.ini `name` / `version` bytes (frozen w/ launcher)
inline constexpr unsigned    DESCRIPTION_MAX   = 256;          // mod.ini `description` bytes (frozen w/ launcher)
inline constexpr unsigned    RESOURCE_NAME_MAX = 46;           // rsr nam_entry::name holds 46 chars + NUL
inline constexpr std::size_t PAYLOAD_BYTES_MAX = 64ull << 20;  // per file
inline constexpr std::size_t SET_BYTES_MAX     = 256ull << 20; // whole accepted set
inline constexpr unsigned    MOD_PAYLOADS_MAX  = 4096;         // per mod
inline constexpr unsigned    SET_PAYLOADS_MAX  = 8192;         // whole accepted set
inline constexpr unsigned    MODS_MAX          = 64;           // enabled mods; the SAME bound as the save
                                                               // carrier, so no valid set is ever mounted
                                                               // without the save metadata it needs

// Version frame of the digest stream (fed without the NUL).
inline constexpr char DIGEST_MAGIC[] = "MHMODSDIGEST1";

// ---- one enabled mod's identity ---------------------------------------------------------------
//
// `folder` (and Payload::source_path below) are absolute ANSI paths and are EXCLUDED from the
// digest; they exist for diagnostics and for the adapter (which may need a real path).
struct ModIdentity {
    std::string id;          // == folder name, [a-z0-9_-]{1..32}
    std::string name;        // human label from mod.ini
    std::string version;     // opaque, never interpreted
    std::string description; // optional, may be empty
    std::string folder;      // <game>/mods/<id>, absolute
};

// ---- one accepted file ------------------------------------------------------------------------
//
// `bytes` is the actual content read ONCE at preparation: a later edit on disk changes nothing the
// callers see, and the digest describes exactly these bytes. `mod_index` is the only provenance
// key needed (it indexes Snapshot::mods); no duplicated mod id string is carried.
struct Payload {
    std::string               name;        // canonical: lowercase ASCII, backslash separators
    std::uint32_t             mod_index;   // index into Snapshot::mods (which mod supplied it)
    std::string               source_path; // absolute, excluded from the digest
    std::vector<std::uint8_t> bytes;
};

// Why an active set failed. Behavioural tests match on this, never on `error` text.
enum class failure {
    none = 0,          // ok
    enabled_list,      // the enabled list itself is malformed (empty token, bad id, duplicate)
    mod_missing,       // an enabled id has no folder under <game>/mods
    manifest,          // mod.ini missing/unreadable/malformed, or id != folder name
    reserved,          // raw INIT*.CFG resource or unsupported config input
    alias,             // two files of ONE mod fold to the same canonical name
    escape,            // reparse point (junction/symlink) or a name that leaves the mod root
    unsupported_class, // a DIRECT-file class this build's overlay cannot honestly mount
    payload_format,    // a validated class whose bytes fail their own format/pair validation
    io,                // a read failed after enumeration (file locked, truncated, too large, ...)
};

// ---- the immutable snapshot -------------------------------------------------------------------
//
// On success: mods in enabled order (LATER WINS), source payloads sorted by (name, mod_index) -- shadowed
// copies included -- winners naming the mounted payload per canonical name, digest over all of it.
// On failure: mods/payloads/winners EMPTY, `failed`+`error` set, `requested` exactly as parsed,
// and `digest` == nomod_digest() because stock is what actually mounts.
struct Snapshot {
    bool                                              ok           = false;
    bool                                              config_error = false; // catalog refusal arose in cfg/lang or raw config
    failure                                           failed       = failure::none;
    std::string                                       error;     // one readable line; "" when ok
    std::vector<std::string>                          requested; // the enabled ids as parsed, in order
    std::vector<ModIdentity>                          mods;
    std::vector<Payload>                              payloads;
    std::vector<TilesetDescriptor>                    tilesets;
    std::vector<Payload>                              config_inputs; // immutable source bytes, including shadowed translations
    std::map<std::string, std::uint32_t, std::less<>> winners;       // canonical name -> payload index

    std::uint8_t digest[32] = {}; // always valid; failure/no-mod == nomod_digest()

    // Winning payload for a canonical name ("maps\\t1.mpm"). Transparent comparator: a
    // string_view argument performs NO allocation, so per-call runtime lookups are stack-only.
    // ASCII case-insensitive lookup; no allocation.
    const TilesetDescriptor *tileset(std::string_view name) const;
    const Payload           *winner(std::string_view canonical_name) const;
    const Payload           *payload(std::uint32_t index) const; // nullptr when out of range
};

// ---- the API ----------------------------------------------------------------------------------

// Shared INI first-section/first-key parser and metadata validation.
bool parse_ini_section(const std::vector<std::uint8_t> &raw, std::string_view section,
                       std::map<std::string, std::string, std::less<>> &fields, std::string &err);
bool valid_metadata_field(std::string_view value, unsigned max_bytes);

// Prepare the active set for `<game_dir_ansi>/mods/<id>/` in the given enabled order.
// `enabled_csv` is the launcher's canonical comma list ("a,b,c"); null/empty means no mods.
// Never throws: every problem comes back as a failure Snapshot (whole-set rule).
Snapshot prepare(const char *game_dir_ansi, const char *enabled_csv);

// Pure canonicalizer for one data/-relative resource path ("maps\\t1.mpm" or "MAPS/T1.MPM").
// False + `err` on anything that is not a bounded ASCII relative path: no drive/absolute/empty
// components, no "." / "..", no trailing separator, <= RESOURCE_NAME_MAX after folding.
// Public because the adapter reuses it and the behavior suite pins it without a filesystem.
bool canonical_resource_name(const char *data_relative_path, std::string &out, std::string &err);

// Finalize a private, successfully prepared snapshot after the CFG merger succeeds.
// Adds both generated resource winners and recomputes the digest over all source inputs and
// locale-independent INIT bytes. Call only for a nonempty config_inputs set, before publication.
void finalize_config(Snapshot &s, std::vector<std::uint8_t> init, std::vector<std::uint8_t> lang);

// Validate map token and tile IDs against a described/overridden TLO, or actual stock counts
// supplied in native-index order by the pre-publication pack adapter. Zero refuses missing counts.
bool validate_tileset_map(const Snapshot &s, const std::uint8_t *bytes, std::size_t size,
                          bool campaign, const std::array<std::uint16_t, 8> &stock_tiles,
                          std::string &error);

// Validate the same campaign-map bytes as data/ payload classification, with a diagnostic.
bool validate_campaign_map(const std::uint8_t *bytes, std::size_t size, std::string_view name,
                           std::string &error);
bool validate_campaign_map(const std::vector<std::uint8_t> &bytes, std::string_view name,
                           std::string &error);

// The no-mod content identity: the version-framed digest of an empty set. Constant for the
// process; it is also every failure snapshot's digest ("failure mounts stock").
const std::uint8_t *nomod_digest();

// Publish one prepared snapshot for the process lifetime (the boot splice, single-threaded).
// Passing the failure snapshot is legitimate: it publishes the stock digest and no payloads.
void mount_snapshot(Snapshot &&s);

// The published snapshot, or nullptr before the first mount. Never null again afterwards.
const Snapshot *mounted_snapshot();

} // namespace mh::mods
