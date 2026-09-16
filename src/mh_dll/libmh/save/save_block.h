//
// save/save_block.h -- the save file's BLOCK layer, reimplemented (RI-SAVE / SV1, batch B).
//
// Spec, read out of /eng/mh.exe's disassembly directly:
//
//   llm_lzw_compress_and_write_block  0x00448661   write one block
//   llm_lzw_compress_block_with_header 0x004cecd6  fill the 8-byte header + call Compress
//   ReadCompressedFromFile            0x004486e6   read one block
//   UnpackSaveData                    0x004ced2b   decode it and classify the result
//
// A save file is a 40-byte version string (batch A's gate) followed by a flat chain of these blocks.
// Verified end to end: 11.sav's 378352 bytes resolve into exactly 102 blocks landing precisely on
// EOF, every block's inner LZW header agreeing with its outer one, with a single plain u32 written
// between the game-level section and the per-planet section. Both drivers -- game::SaveGame and
// SavePlanetToDisk -- speak only this layer, which is why owning it is what unblocks a state-layout
// change (SV1's scope note).
//
// EVERYTHING IS INJECTED, NOTHING IS BOUND. The file handle arrives as a `block_io`, and the staging
// buffer and codec state arrive as a `block_workspace`. That is the whole reason this layer can be
// tested: docs/save-format.md's shadowability section shows every function here touches a live file
// handle, so a shadow site would write the file twice or desync the read cursor, and SV1's done_when
// therefore asks for buffer-level evidence instead. Binding these to the game's real
// read_from_file/write_to_file and to G_LZW_TEMP_DATA is promotion, i.e. SV1-P, not this item.
//
// RETURN VALUES ARE THE ORIGINAL'S: 0 = success, 1 = failure. Both directions.
//
#pragma once
#include <cstdint>

#include "include/mh_lzw.h"

namespace mh::save {

// The on-disk block header. `compressed_len` counts the payload only -- but the payload's own LZW
// header is inside it, so the block occupies 8 + compressed_len bytes.
struct block_header {
    uint32_t compressed_len;
    uint32_t uncompressed_size;
};
static_assert(sizeof(block_header) == 8);

// `CMP dword ptr [EBP + -0x20],0x927c0 / JBE` at 0x00448744 -- 600000, and the test is INCLUSIVE, so
// a block claiming exactly this much is accepted.
inline constexpr uint32_t BLOCK_READ_CAP = 0x927c0;

// THE ORIGINAL'S STAGING BUFFER IS EIGHT BYTES TOO SMALL FOR ITS OWN CAP. G_LZW_TEMP_DATA at
// 0x00a4f128 runs to 0x00ae18e8, where _G_LLM_STRAT_GROUP_STEP_HEADING_REMAP begins: exactly
// BLOCK_READ_CAP bytes, header included. The payload is read to base+8, so a block at the cap writes
// through 0x00ae18f0 and smashes eight bytes of that table. Unreachable from a file the game wrote
// (the largest block measured in a real save is 263574) but reachable from a corrupt or crafted one.
// Our own buffer gets the slack, so the same accept/reject boundary is faithful AND in bounds.
inline constexpr uint32_t STAGING_SLACK = 8;
inline constexpr uint32_t STAGING_BYTES = BLOCK_READ_CAP + STAGING_SLACK;

// The file handle, abstracted to the two calls the original makes.
struct block_io {
    // write_to_file 0x004cff57, called as (data, size, count=1, handle) returning the count. The
    // original treats any return other than 1 as a failure.
    int (*write)(void *ctx, const void *data, uint32_t size);
    // read_from_file 0x004cfd58, called the same way and returning a count just as it does. THE
    // ORIGINAL DISCARDS THE RESULT -- `MOV [EBP-0x18],EAX` at 0x00448764 is a dead store -- so a
    // short read goes unnoticed. Reproduced; `block_workspace::strict_reads` is the way out.
    int (*read)(void *ctx, void *dst, uint32_t size);
    void *ctx;
};

// The mutable state the original keeps in globals, plus one policy flag that is not in the original
// at all.
struct block_workspace {
    uint8_t            *staging;             // STAGING_BYTES; G_LZW_TEMP_DATA 0x00a4f128
    uint32_t           *max_compressed_seen; // G_LZW_MAX_COMPRESSED_SIZE 0x005d08f0, may be null
    lzw::encoder_state *encoder;             // may be null if only reading
    lzw::decoder_state *decoder;             // may be null if only writing

    // NOT A FAITHFUL SETTING -- and off by default for that reason. The original cannot detect a
    // truncated file: it discards the read count, and its only structural check (the block header's
    // uncompressed size against the caller's array) has already passed by then, so a short payload
    // decodes to whatever the stale staging bytes happen to say. Turning this on compares the count
    // and refuses. It is the concrete hardening SV1-P has to decide on, kept here behind a flag
    // rather than smuggled into the default path, because changing when a load fails is a
    // behaviour change and belongs in the promotion item with the evidence for it.
    bool strict_reads = false;
};

// Compress `src` and append it as one block. Returns 0 on success, 1 if the write failed.
int write_block(const block_io &io, const block_workspace &ws, const void *src, uint32_t size);

// Read one block into `dst`, which must be `dst_size` bytes. Returns 0 on success, 1 on refusal.
//
// The refusals are the load-bearing part of SV1: a header whose uncompressed size disagrees with
// `dst_size` is refused (0x0044872f), and so is one claiming more than BLOCK_READ_CAP compressed
// bytes (0x0044873d). What is NOT refused is a truncated or corrupt PAYLOAD -- see the .cpp.
int read_block(const block_io &io, const block_workspace &ws, void *dst, uint32_t dst_size);

} // namespace mh::save
