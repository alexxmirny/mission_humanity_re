//
// save/save_ext.h -- APPENDED CAPACITY AT EOF (RI-SAVE / SV1, batch C).
//
// SV1's scope asks for one thing beyond the block layer: "keep vanilla blocks byte-compatible and
// append enlarged capacity at EOF". Batch C's reading of the drivers (docs/save-format.md, "The
// container") settled both halves of how to do that, and the answer is simpler than the earlier plan
// assumed -- the earlier plan named the self-describing REGION GRAPH as the model to copy, when in
// fact the OUTER FORMAT ALREADY HAS THE IDIOM.
//
// WHAT THE CONTAINER ALREADY DOES. `game::SaveGame` embeds each per-planet file in the .sav as
//
//     u32 length            (a plain write_to_file, NOT a block)
//     length bytes verbatim  (streamed in <=0x927c0 chunks through the LZW staging buffer)
//
// so a length-prefixed opaque member is not an invention: it is how the format carries its own
// largest payloads. An extension is the same shape, appended after the last vanilla byte.
//
// WHY APPENDING IS SAFE, AND IT IS A PROPERTY OF THE READER, NOT A HOPE. `llm_game_load` never
// consults EOF. It reads the version header, a fixed block list, and then loops `i = 1..0x1f`
// extracting one embedded member per planet that satisfies
//
//     Planets[i].system_index == CurrentSystem && (G_PLANET_STATUS[i] != 0 || G_PLANET_INDEX == i)
//
// and stops. It terminates on a DERIVED condition, so anything past the last member it wanted is
// simply never read. (The vanilla writer's own trailing media block -- 0x400 bytes of
// llm_build_media_diag_report output -- is already such an unread trailer, which is the existence
// proof.) So trailing bytes cost nothing and break nothing.
//
// WHICH IS EXACTLY WHY AN EXTENSION MUST NOT BE SILENT. "A vanilla build ignores it" is the same
// sentence as "a vanilla build loads a 500-unit save as if it had 400 units" -- a SILENT MISPARSE,
// the one failure mode SV1 and SV1-P both call load-bearing. Appending capacity is therefore only
// half a feature; the other half is that a file carrying an extension must stamp a version string
// the vanilla table does not contain, so batch A's gate refuses it at the header. That pairing is
// the whole point, and `extension_requires_version_refusal()` below states it as a checkable claim
// rather than a comment. It is also why the version gate was chosen as batch A.
//
#pragma once
#include <cstdint>

#include "save_block.h"

namespace mh::save {

// The trailer's own header. Deliberately NOT a block: a block pays an LZW round trip and, more to the
// point, a block's framing gives no way to tell "an extension" from "the trailing media block" or from
// whatever a future vanilla patch appends. A magic makes the distinction explicit.
//
// Layout, matching the container's own u32-length idiom with a tag in front of it:
//     u32 magic ('MHX1')   u32 tag   u32 length   length bytes
struct extension_header {
    uint32_t magic;
    uint32_t tag;
    uint32_t length;
};
static_assert(sizeof(extension_header) == 12);

inline constexpr uint32_t EXT_MAGIC = 0x3158484D; // 'MHX1', little-endian

// Tags are ours, not the game's. One per enlarged array, so a reader can take the ones it understands
// and skip the rest -- which is the property the vanilla format lacks and the reason a cap raise
// currently has to bump every consumer at once.
inline constexpr uint32_t EXT_TAG_UNITS     = 0x554E4954; // 'UNIT'
inline constexpr uint32_t EXT_TAG_BUILDINGS = 0x424C4447; // 'BLDG'

// Append one extension member. Returns 0 on success, 1 if a write failed -- the same 0/1 convention as
// write_block, because a driver accumulates these into the one failure counter the original keeps.
//
// The payload is written VERBATIM, not compressed. That is a deliberate departure and worth stating:
// the vanilla format compresses everything, but the whole reason a cap raise has a ceiling at all is
// that `Compress` writes into a 600 000-byte staging buffer with no destination bound (see
// docs/save-format.md). An extension exists precisely to carry data the vanilla path could not, so
// routing it back through the buffer that imposes the limit would defeat it. A caller that wants
// compression can compress into its own buffer and hand the result over as an opaque payload.
int write_extension(const block_io &io, uint32_t tag, const void *data, uint32_t length);

// Read the next extension member's header. Returns 0 and fills `out` if one is there, 1 if the bytes
// at the cursor are not an extension (including "the file ended") -- in which case the cursor may have
// advanced and the caller should treat the trailer as finished.
int read_extension_header(const block_io &io, extension_header *out);

// Read `hdr.length` bytes of payload into `dst`, which must be at least `dst_size` bytes.
// Returns 0 on success, 1 on refusal.
//
// UNLIKE THE VANILLA READER, THIS ONE REFUSES A SHORT READ. The vanilla block reader cannot detect
// truncation (it discards the read count -- a dead store at 0x00448764), and `strict_reads` exists to
// opt into detecting it. There is no faithfulness argument here: this framing is ours, so it checks.
int read_extension_payload(const block_io &io, const extension_header &hdr, void *dst,
                           uint32_t dst_size);

// The pairing rule, as a predicate a test can drive rather than a paragraph nobody re-reads.
//
// Given the version slot a build would stamp into a file that carries an extension, would a VANILLA
// build refuse that file at the header? True is the only acceptable answer for any writer that appends
// capacity. `detect_version` returns VERSION_REJECT for a header matching no slot, so the rule is
// simply: an extension-carrying build must not stamp one of the six vanilla strings.
bool extension_requires_version_refusal(int32_t vanilla_detect_result);

} // namespace mh::save
