//
// mh_lzw.h -- the game's LZW codec and its LZSS sibling, reimplemented (RI-SAVE / SV1, batch B).
//
// Spec, read out of /eng/mh.exe's disassembly directly:
//
//   Compress                 0x004f48e0   the encoder driver
//   emit_code                0x004f49cc   the bit packer (FUN_004f49cc)
//   llm_lzw_encode_core      0x004f49fd   dictionary insert + widen + the full-dictionary reset
//   find_match               0x004f4b31   hash-chain lookup (FUN_004f4b31)
//   Decompress               0x004f4b7e   the decoder driver
//   decompressImpl           0x004f4c1f   read one code
//   handleCode               0x004f4ca7   emit one code's string
//   updateDict               0x004f4ce9   decoder dictionary insert
//   llm_lzss_decompress_block 0x004ddc70  the OTHER codec (see below)
//
// WHY THIS LAYER IS IN mh_common AND NOT mh/save: it is pure buffer logic with no game address in
// it. The save-format framing that calls it (the 8-byte block header, the staging buffer, the file
// handle) is libmh/save/save_block.h.
//
// TWO CODECS, NOT ONE. A block payload starting with 'LZW ' is LZW. Anything else is the format at
// 0x004ddc70 -- a byte-oriented LZSS with a 256-byte window. docs/save-format.md's open question 1
// asked whether ReadCompressedFromFile swallows a decode error when UnpackSaveData returns -1; the
// answer is that -1 does not mean "error", it means "not LZW", and the forced success is correct.
//
// THE DICTIONARY ARRAY IS A UNION OF TWO DIFFERENT NODE LAYOUTS. The original points both the
// encoder and the decoder at 0x0066f6b7, but the encoder's node is {u16 prefix, u16 ch, u32 next}
// and the decoder's is {u32 prefix, u32 ch}. They are never live at the same time. Unifying them
// into one struct is the obvious refactor and it is wrong.
//
#pragma once
#include <cstdint>

namespace mh::lzw {

// 'LZW ' as it sits in the file: `MOV EAX,0x20575a4c` / `CMP dword ptr [EDX],0x20575a4c`.
inline constexpr uint32_t MAGIC = 0x20575A4Cu;

// The 12-byte header Compress writes ahead of the code stream (three STOSDs at 0x004f492c).
struct payload_header {
    uint32_t magic;
    uint32_t uncompressed_size;
    uint32_t compressed_len; // total payload bytes, INCLUDING these 12
};
static_assert(sizeof(payload_header) == 12);

// 0x0066f6b7 is `uint[8192]` = 32768 bytes = 4096 eight-byte nodes. Both codecs share the extent.
inline constexpr uint32_t DICT_NODES = 4096;

// The encoder's chain tables: 0x2000 dwords cleared at 0x004f48e4, which is heads[4096] at
// 0x006776b7 followed by tails[4096] at 0x0067b6b7.
inline constexpr uint32_t CHAIN_SLOTS = 4096;

// The code alphabet. 0x000..0x0ff are literals, 0x100 is "reset the dictionary", 0x101 is
// end-of-stream, and dictionary entry i is code i + 0x102.
inline constexpr uint32_t CODE_RESET = 0x100;
inline constexpr uint32_t CODE_END   = 0x101;
inline constexpr uint32_t CODE_FIRST = 0x102;

// Widening points. The encoder widens one insertion LATER than the decoder (0xff vs 0xfe) because
// the decoder always learns entry N only after receiving code N+1 -- the standard LZW off-by-one,
// and reproducing BOTH numbers is what keeps the two in step. `DICT_FULL` is the encoder-only
// threshold at which it emits CODE_RESET and starts over.
inline constexpr uint32_t WIDEN_ENC_1 = 0x0ff, WIDEN_ENC_2 = 0x2ff, WIDEN_ENC_3 = 0x6ff;
inline constexpr uint32_t WIDEN_DEC_1 = 0x0fe, WIDEN_DEC_2 = 0x2fe, WIDEN_DEC_3 = 0x6fe;
inline constexpr uint32_t DICT_FULL = 0xeff;

// The encoder indexes heads[] by the CODE, so the widest code it can emit has to stay inside the
// table or a chain head would alias tails[0]. It does, but only just, and the chain is worth
// spelling out because one step either way breaks it:
//
//   insert() resets the instant the size REACHES DICT_FULL, so a lookup never sees more than
//   DICT_FULL-1 entries; a match against that dictionary yields at most index DICT_FULL-2; and the
//   emitted code is that index plus CODE_FIRST = 0xfff, the last slot in the table.
inline constexpr uint32_t MAX_LOOKUP_DICT_SIZE = DICT_FULL - 1;
inline constexpr uint32_t MAX_EMITTED_CODE     = MAX_LOOKUP_DICT_SIZE - 1 + CODE_FIRST;
static_assert(MAX_EMITTED_CODE == CHAIN_SLOTS - 1);

// ---------------------------------------------------------------------------- decoder

struct decoder_node {
    uint32_t prefix; // [node+0]
    uint32_t ch;     // [node+4]
};

struct decoder_state {
    decoder_node nodes[DICT_NODES];
    uint32_t     bit_count; // 0x0067f6c3
    uint32_t     mask;      // 0x0067f6c7
    uint32_t     shift;     // 0x0067f6cb -- the bit offset within the current input byte
    uint32_t     dict_size; // 0x0067f6cf
};

// What `*io_size` means on return -- the original writes it through the size pointer and
// UnpackSaveData (0x004ced2b) branches on the value:
//
//   > 0   the decoded byte count            -> UnpackSaveData returns 0 (success)
//   0     the reset counter ran out         -> UnpackSaveData returns 1 (failure)
//   -1    the payload was NOT LZW           -> UnpackSaveData returns -1, and
//                                              ReadCompressedFromFile runs the LZSS decoder
inline constexpr uint32_t SIZE_NOT_LZW = 0xFFFFFFFFu;

// Decompress 0x004f4b7e. `payload` is the block payload (the 'LZW ' header, not the block header).
// On entry *io_size is the payload byte count -- it is only read on the not-LZW path, where the
// original raw-copies that many bytes to `dst` before returning SIZE_NOT_LZW.
//
// `dst_limit` bounds the decoded output, and 0 means unbounded, which is what the original is. PASS
// THE REAL SIZE FROM ANYTHING THAT HAS IT: a corrupt stream otherwise writes past the destination,
// and in the promoted path that destination is one of the game's arrays. On exceeding the limit -- or
// on a prefix CYCLE, which is the other way a corrupt dictionary escapes -- it stops and reports 0,
// the status UnpackSaveData already maps to a refusal. Neither bound can fire on a stream Compress
// produced; see the note in the .cpp.
//
// `payload` must be readable to its own claimed compressed_len; the decoder reads up to 3 bytes at a
// time and zero-fills past that point rather than over-reading a caller's buffer.
void decompress(decoder_state &st, const void *payload, void *dst, uint32_t *io_size,
                uint32_t dst_limit = 0);

// ---------------------------------------------------------------------------- encoder

struct encoder_node {
    uint16_t prefix; // [node+0] -- written but never read back; the chain is keyed by prefix code
    uint16_t ch;     // [node+2] -- compared 16 bits wide at a time (`CMP BX,word ptr [EAX+2]`)
    uint32_t next;   // [node+4] -- 0 = end of chain
};
static_assert(sizeof(encoder_node) == 8);

struct encoder_state {
    encoder_node nodes[DICT_NODES];
    // The original stores POINTERS here and uses 0 for "empty", which is unambiguous because
    // &nodes[0] is not null. Storing a bare index would make node 0 -- a real, reachable node --
    // indistinguishable from empty, so these hold index+1 and 0 still means empty.
    uint32_t       heads[CHAIN_SLOTS]; // 0x006776b7
    uint32_t       tails[CHAIN_SLOTS]; // 0x0067b6b7
    uint32_t       dict_size;          // 0x0066f6b3
    uint32_t       width;              // 0x0066f6b0 -- current code width in bits
    uint32_t       pending_bits;       // 0x0066f6b1
    uint32_t       pending_byte;       // 0x0066f6b2
    uint32_t       input_counter;      // 0x0066f6a4 -- incremented per input byte; a statistic
    uint8_t       *out;                // the EDI cursor Compress keeps across emit_code calls
    const uint8_t *out_end;            // NOT in the original -- see compress()'s dst_limit
    bool           overflowed;
};

// Compress 0x004f48e0. Writes the 12-byte header plus the code stream to `dst` and returns the
// total byte count, which is also what it stores to dst[8].
//
// RETURNS 0 FOR "REFUSED", and both reasons below produce it: a zero-length input, and running past
// `dst_limit`. One value because callers want one answer -- `write_block` reports either as its
// ordinary failure return -- and because neither case has an output the original can be compared
// against (it runs away on the first and overflows on the second).
//
// `dst` MUST have room for the worst case. The original has no bound at all and one real save block
// does expand (block 84 of 11.sav: 262144 in, 263574 out), so a caller sizing dst at `size` is
// wrong; use compressed_bound(size).
//
// `dst_limit` bounds the output, and 0 means unbounded, which is what the original is. THE ORIGINAL
// HAS NO WRITE-SIDE BOUND AT ALL and its staging buffer is only 600 000 bytes, so an array whose
// COMPRESSED form approaches that overflows G_LZW_TEMP_DATA and smashes whatever follows it -- the
// write-side twin of the read-side cap overrun, and worse, because the reader at least has a cap to
// check. Compressing over the limit returns 0 here instead, which `write_block` reports as a failure.
// See docs/save-format.md's cap-raise ceiling.
//
// SIZE 0 IS REJECTED (returns 0) RATHER THAN REPRODUCED. The original reads its first input byte
// before its first end-of-input test (LODSB at 0x004f4940 precedes the `CMP ESI,end` at
// 0x004f494e), so a zero-length input makes it read past the buffer and then never hit its
// equality test again -- an unbounded runaway with no output to be equivalent to. See the
// same-shaped note on the reset path in the .cpp.
uint32_t compress(encoder_state &st, const void *src, uint32_t size, void *dst,
                  uint32_t dst_limit = 0);

// Header + one 12-bit code per input byte + the three-code tail, rounded up. Comfortably above the
// 1.005x worst case measured on real blocks.
inline constexpr uint32_t compressed_bound(uint32_t size) {
    return sizeof(payload_header) + (size * 3 + 1) / 2 + 8;
}

// ---------------------------------------------------------------------------- the other codec

} // namespace mh::lzw

namespace mh::lzss {

// llm_lzss_decompress_block 0x004ddc70 -- the format a non-'LZW ' block is in. Flag bits arrive 16
// at a time, MSB first; a 0 bit means "copy one literal byte", a 1 bit means "read a 3-bit length
// (+2) and a 1-byte offset, then copy length bytes from dst[-(256-offset)]".
//
// `block` is the BLOCK pointer, not the payload: this decoder reads the uncompressed size from the
// block header's second dword itself and then skips 8 bytes.
//
// IT CAN OVERRUN `dst` BY UP TO 8 BYTES. The end test is at the token boundary only, so a final
// back-reference token writes up to 9 bytes with one byte of room left. The original does this into
// the game's arrays; give `dst` 8 bytes of slack.
void decompress_block(const void *block, void *dst);

} // namespace mh::lzss
