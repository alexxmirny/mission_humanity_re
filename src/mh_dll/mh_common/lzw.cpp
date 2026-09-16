//
// lzw.cpp -- the game's LZW codec, both directions, plus its LZSS sibling. See include/mh_lzw.h for
// the address map and the two structural traps (the shared dictionary array's two node layouts, and
// index+1 in the chain tables).
//
// PROVENANCE. The decoder started as the vector-based port that lived in this file before SV1 and is
// still the same algorithm -- FetchWord / HandleCode / UpdateDict, one for one. What changed for
// batch B: it writes through a raw cursor instead of a std::vector (the promoted path decodes
// straight into the game's array during a load and must not allocate), it returns the ORIGINAL's
// status values through `*io_size` rather than a convenient one, and 0x100 now restarts the OUTER
// loop rather than being swallowed inside the code reader -- see the comment on that loop, it is a
// real behavioural difference, not a tidy-up. The ENCODER is new; nothing in the tree could
// compress, only decompress.
//
// Reference: docs/save-format.md. Verified against 102 real blocks of a vanilla save; the byte
// identity assertions live in `net_selftest.exe savetest`.
//
#include "include/mh_lzw.h"

#include <cstddef>
#include <cstring>

namespace mh::lzw {

namespace {

// Read the three bytes a code can span (shift <= 7 plus a 12-bit code needs bits 0..18), zero-filled
// past `end`. The original issues a plain 32-bit `MOV EAX,[EDX]` into its 600 KB staging buffer,
// where over-reading is harmless; the fourth byte is masked off in every case, so a bounded 3-byte
// load is output-identical and does not read past a caller's buffer.
uint32_t load_bits(const uint8_t *p, const uint8_t *end) {
    uint32_t v = 0;
    for (int i = 0; i < 3; ++i)
        if (p + i < end) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

enum class code_status { code,
                         restart,
                         end_of_stream,
                         exhausted };

// decompressImpl 0x004f4c1f. Returns one code, or says why it could not.
code_status read_code(decoder_state &st, const uint8_t *&in, const uint8_t *end, uint32_t &counter,
                      uint32_t &out_code) {
    // NOT IN THE ORIGINAL, and the only guard in this file that can fire on a stream the original
    // would have accepted. The original has no input bound at all: a truncated or corrupt payload
    // walks off the end of the staging buffer reading whatever follows, and since its terminating
    // test is a 0x101 code that may never arrive, it can run away. Refusing here turns that into
    // ReadCompressedFromFile's ordinary failure return. Verified not to fire on any of the 102
    // blocks of a real save -- a well-formed stream's 0x101 is always inside the payload.
    if (in >= end) return code_status::exhausted;

    const uint32_t word = load_bits(in, end);
    out_code            = (word >> st.shift) & st.mask;

    // 0x004f4c2f: advance the byte cursor by at most two, carrying the leftover bit offset.
    uint32_t bits = st.shift + st.bit_count;
    if (bits >= 8) {
        ++in;
        bits -= 8;
        if (bits >= 8) {
            ++in;
            bits -= 8;
        }
    }
    st.shift = bits;

    if (out_code == CODE_RESET) {
        // 0x004f4c5a: DEC then test. The counter is seeded from the payload header's third dword --
        // the compressed byte count -- which makes running out of resets unreachable in practice for
        // anything Compress produced. Reproduced because it is the only path that yields the "0"
        // status, and UnpackSaveData maps 0 to failure.
        if (--counter == 0) return code_status::exhausted;
        st.dict_size = 0;
        st.bit_count = 9;
        st.mask      = 0x1ff;
        return code_status::restart;
    }
    if (out_code == CODE_END) return code_status::end_of_stream;
    return code_status::code;
}

// handleCode 0x004f4ca7. Walks the prefix chain to measure the string, then writes it backwards.
//
// TWO BOUNDS THE ORIGINAL DOES NOT HAVE, and unlike `strict_reads` these are NOT behind a flag,
// because there is no behaviour here worth being faithful to:
//
//   * `dst_end` -- the original writes to its destination with no bound at all, so a corrupt stream
//     writes past the caller's array. In the promoted path that array is the game's, which makes an
//     unbounded write a memory-corruption bug rather than a wrong answer. Returning false instead
//     turns it into ReadCompressedFromFile's ordinary failure return.
//   * the chain-length cap -- a corrupt dictionary can contain a prefix CYCLE, and then this walk
//     never terminates. It is non-terminating in the original too; it showed up for real during
//     mutation testing, where eight deliberately broken bodies HUNG savetest instead of failing it.
//
// Neither can fire on a stream Compress produced: a valid chain is at most dict_size long and a valid
// decode lands exactly on the declared size. The 501-block sweep and every fixture pass unchanged,
// which is the evidence that these are inert on real data.
bool handle_code(const decoder_state &st, uint32_t code, uint8_t *&out, const uint8_t *dst_end) {
    if (code < CODE_FIRST) {
        if (dst_end != nullptr && out + 1 > dst_end) return false;
        *out++ = static_cast<uint8_t>(code);
        return true;
    }
    int32_t  n    = 0;
    uint32_t walk = code;
    do { // 0x004f4cb2: counts one per link INCLUDING the link that lands on a literal
        walk = st.nodes[walk - CODE_FIRST].prefix;
        ++n;
    } while (walk > CODE_END && n <= static_cast<int32_t>(DICT_NODES));
    if (walk > CODE_END) return false; // ran the cap -> the chain is a cycle

    if (dst_end != nullptr && out + n + 1 > dst_end) return false;

    int32_t  i = n; // 0x004f4cc4: first byte goes at out[n], then downwards
    uint32_t c = code;
    while (c > CODE_END && i > 0) {
        out[i--] = static_cast<uint8_t>(st.nodes[c - CODE_FIRST].ch);
        c        = st.nodes[c - CODE_FIRST].prefix;
    }
    out[0] = static_cast<uint8_t>(c);
    out += n + 1;
    return true;
}

// updateDict 0x004f4ce9.
void update_dict(decoder_state &st, uint32_t prev, uint32_t cur) {
    // The original would write past its 4096-node array here on a stream that sends more than 4096
    // codes without a reset, landing in the encoder's chain table. Compress never emits such a
    // stream (it resets at 0xeff), so this bound is unreachable for real data.
    if (st.dict_size >= DICT_NODES) return;

    const uint32_t new_code = st.dict_size + CODE_FIRST;
    uint32_t       walk     = cur;
    // 0x004f4d00: the KwKwK substitution sits INSIDE the walk loop. Substituting once before the
    // loop happens to give the same answer -- prev can never itself be new_code -- but the loop form
    // is what the original does.
    //
    // Same cycle cap as handle_code, for the same reason and equally inert on valid data: a chain
    // longer than the whole array is necessarily a cycle. Without it a corrupt dictionary hangs here.
    for (uint32_t steps = 0; walk >= CODE_FIRST; ++steps) {
        if (steps > DICT_NODES) return; // cycle; leave the dictionary alone rather than spin
        if (walk == new_code) {
            walk = prev;
            continue;
        }
        walk = st.nodes[walk - CODE_FIRST].prefix;
    }

    st.nodes[st.dict_size].prefix = prev;
    st.nodes[st.dict_size].ch     = walk;
    ++st.dict_size;

    if (st.dict_size == WIDEN_DEC_1 || st.dict_size == WIDEN_DEC_2 || st.dict_size == WIDEN_DEC_3) {
        ++st.bit_count;
        st.mask = st.mask << 1 | 1;
    }
}

} // namespace

void decompress(decoder_state &st, const void *payload, void *dst, uint32_t *io_size,
                uint32_t dst_limit) {
    const uint8_t *p = static_cast<const uint8_t *>(payload);

    payload_header h{};
    std::memcpy(&h, p, sizeof h); // the payload is not guaranteed aligned

    // 0x004f4b82, and the ORDER is deliberate: the original resets these four BEFORE the magic
    // check at 0x004f4bbd, so they are reset even on a call that turns out not to be LZW. The
    // adversarial review (2026-07-30) raised the reversed order as a note -- it found no path by
    // which it changes a decoded byte, since decoder_state is private scratch and any real decode
    // resets it again before reading it, but matching the original costs nothing and removes a
    // difference that a future reader would have to re-derive.
    //
    // The node array is deliberately NOT cleared -- the original leaves whatever the previous decode
    // left in it, and only entries it has written this run are reachable from a well-formed stream.
    // A freshly constructed decoder_state is zeroed, which is what .bss gives the original at
    // process start.
    st.bit_count = 9;
    st.mask      = 0x1ff;
    st.shift     = 0;
    st.dict_size = 0;

    // 0x004f4bb5-0x004f4bb8 stores h.uncompressed_size to LZW_UNCOMPRESSED_SIZE 0x0066f6a4,
    // unconditionally and before the magic check. WE DO NOT REPRODUCE THAT WRITE, and it is inert:
    // `find-cross-references` on 0x0066f6a4 (2026-07-30) returns exactly FOUR references -- one
    // write in Decompress and three in Compress -- and nothing anywhere else in the image reads it.
    // It is the codec's own scratch, shared between the two directions like G_LZW_DICT is, so a
    // dead store on the decode side. Raised as a divergence by the review and resolved to a note by
    // that xref check; recorded here because "the draft omits a global write" is exactly the kind
    // of finding that must not be settled by assertion.

    // 0x004f4bbd. Not "the header is corrupt" -- "this block is in the other format". The caller
    // (ReadCompressedFromFile) reacts to SIZE_NOT_LZW by running mh::lzss::decompress_block over the
    // same block, which overwrites everything this copy just wrote.
    if (h.magic != MAGIC) {
        std::memcpy(dst, p, *io_size);
        *io_size = SIZE_NOT_LZW;
        return;
    }

    uint32_t       counter = h.compressed_len;
    const uint8_t *in      = p + sizeof(payload_header);
    const uint8_t *end     = p + (h.compressed_len < sizeof(payload_header) ? sizeof(payload_header)
                                                                            : h.compressed_len);
    uint8_t       *out     = static_cast<uint8_t *>(dst);
    const uint8_t *dst_end = dst_limit == 0 ? nullptr : out + dst_limit;

    // The two-level loop is the original's control flow, and the level matters. 0x100 jumps back to
    // 0x004f4be8 -- the OUTER entry -- so the code after a reset becomes a fresh `prev` and is NOT
    // fed to update_dict with the pre-reset `prev`. Letting the code reader swallow 0x100 and
    // continue would pair a stale prefix with the first code of the new dictionary: a plausible
    // simplification that corrupts the first entry after every reset, and one that only shows up on
    // inputs long enough to fill the dictionary.
    for (;;) {
        uint32_t prev = 0;
        switch (read_code(st, in, end, counter, prev)) {
            case code_status::end_of_stream: *io_size = static_cast<uint32_t>(out - static_cast<uint8_t *>(dst)); return;
            case code_status::exhausted: *io_size = 0; return;
            case code_status::restart: continue;
            case code_status::code: break;
        }
        if (!handle_code(st, prev, out, dst_end)) {
            *io_size = 0; // the failure status UnpackSaveData already maps to "refused"
            return;
        }

        for (;;) {
            uint32_t          cur = 0;
            const code_status s   = read_code(st, in, end, counter, cur);
            if (s == code_status::end_of_stream) {
                *io_size = static_cast<uint32_t>(out - static_cast<uint8_t *>(dst));
                return;
            }
            if (s == code_status::exhausted) {
                *io_size = 0;
                return;
            }
            if (s == code_status::restart) break; // back to the outer entry, deliberately
            update_dict(st, prev, cur);
            if (!handle_code(st, cur, out, dst_end)) {
                *io_size = 0;
                return;
            }
            prev = cur;
        }
    }
}

namespace {

// emit_code 0x004f49cc. Packs `width` bits little-endian, LSB-first within each byte, carrying a
// partial byte between calls.
void emit_code(encoder_state &st, uint32_t code) {
    uint32_t acc  = (code << st.pending_bits) | st.pending_byte;
    uint32_t bits = st.pending_bits + st.width;
    while (bits >= 8) { // `CMP CL,8 / JC` -- unsigned, so this is "while at least a whole byte"
        // The bound the original does not have. Encoding continues so the caller sees one clean
        // "overflowed" answer rather than a partial stream, but nothing more is written.
        if (st.out_end != nullptr && st.out >= st.out_end) {
            st.overflowed = true;
            return;
        }
        *st.out++ = static_cast<uint8_t>(acc);
        acc >>= 8;
        bits -= 8;
    }
    st.pending_byte = acc & 0xff;
    st.pending_bits = bits;
}

// find_match 0x004f4b31. Returns the dictionary index of (cur, next), or -1.
int32_t find_match(const encoder_state &st, uint32_t cur, uint32_t next) {
    if (st.dict_size == 0) return -1;
    uint32_t ref = st.heads[cur];
    if (ref == 0) return -1;
    for (;;) {
        const uint32_t idx = ref - 1;
        // `CMP BX,word ptr [EAX+2]` -- a 16-bit compare. Both sides are byte values here, so the
        // width is not observable, but the chain is keyed only by the prefix code: the stored
        // character is the ONLY thing distinguishing two nodes on one chain.
        if (static_cast<uint16_t>(next) == st.nodes[idx].ch) return static_cast<int32_t>(idx);
        ref = st.nodes[idx].next;
        if (ref == 0) return -1;
    }
}

// llm_lzw_encode_core 0x004f49fd. Appends (cur, next), widens at the three thresholds, and on a full
// dictionary emits the pending character plus CODE_RESET and starts over. Returns true if it reset.
bool insert(encoder_state &st, uint32_t cur, uint32_t next) {
    const uint32_t ref = st.dict_size + 1; // index+1; 0 means "no chain", see the header
    if (st.heads[cur] == 0) {
        st.heads[cur] = ref;
        st.tails[cur] = ref;
    } else {
        st.nodes[st.tails[cur] - 1].next = ref;
        st.tails[cur]                    = ref;
    }
    st.nodes[st.dict_size].prefix = static_cast<uint16_t>(cur);
    st.nodes[st.dict_size].ch     = static_cast<uint16_t>(next);
    st.nodes[st.dict_size].next   = 0;

    const uint32_t n = st.dict_size + 1;
    if (n == WIDEN_ENC_1 || n == WIDEN_ENC_2 || n == WIDEN_ENC_3) {
        ++st.width; // 0x004f4ad9, then falls through to store the new size
        st.dict_size = n;
        return false;
    }
    if (n != DICT_FULL) {
        st.dict_size = n;
        return false;
    }

    // 0x004f4a9b. `next` has not been emitted yet and there is no dictionary left to put it in, so
    // it goes out as a literal-or-code at the OLD width, then the reset marker, also at the old
    // width. The caller emits CODE_RESET a SECOND time at the new width -- see compress().
    emit_code(st, next);
    emit_code(st, CODE_RESET);
    st.dict_size = 0;
    st.width     = 9;
    std::memset(st.heads, 0, sizeof st.heads); // one REP STOSD over both tables at 0x004f4ac3
    std::memset(st.tails, 0, sizeof st.tails);
    return true;
}

} // namespace

uint32_t compress(encoder_state &st, const void *src, uint32_t size, void *dst,
                  uint32_t dst_limit) {
    // DIVERGENCE, DELIBERATE, and the original's behaviour here is a RUNAWAY, not an output.
    // 0x004f48f7 sets LZW_END_PTR = begin + size, so with size == 0 END == begin; then 0x004f4940
    // does an unconditional LODSB before any length test, leaving ESI == begin+1; and the loop's only
    // termination test (0x004f494e `CMP ESI,end / JZ`) is a bare EQUALITY, which ESI has already
    // passed and can never return to. So a zero-length block makes the ORIGINAL read forward until it
    // faults. There is no output to be equivalent to.
    //
    // IS IT REACHABLE? Measured, not assumed (SV1 batch C, 2026-07-30). Every size in the block table
    // is a hardcoded immediate >= 4 except ONE: SavePlanetToDisk's progress loop at 0x004480a8 takes
    // `3 * (u16)[0x00e16305]`, the format's only data-derived size. Across 41 real save files
    // (tools/oneoff/2026-07-30-sv1c-progress-block-size.py) that block is 207 bytes in every single
    // one -- i.e. the u16 is 69 everywhere -- and there are ZERO zero-size blocks in any of them. So
    // for the shipped drivers this early return is dead-code-equivalent. It still earns its place: the
    // u16 is a live runtime value (35 referrers, written by Construct 0x0045f004), so a config with no
    // progress entries would reach it, and a hang during save is the worst possible failure mode.
    if (size == 0) return 0;

    const uint8_t *in        = static_cast<const uint8_t *>(src);
    const uint8_t *input_end = in + size;
    uint8_t       *dst8      = static_cast<uint8_t *>(dst);

    std::memset(st.heads, 0, sizeof st.heads); // 0x004f48e4
    std::memset(st.tails, 0, sizeof st.tails);
    st.dict_size     = 0;
    st.pending_bits  = 0;
    st.pending_byte  = 0;
    st.width         = 9;
    st.input_counter = 0;
    st.out           = dst8;
    st.out_end       = dst_limit == 0 ? nullptr : dst8 + dst_limit;
    st.overflowed    = false;

    // 0x004f492c: magic, uncompressed size, and a zero placeholder that is overwritten at the end.
    // The header write is bounded too. The original has no dst_limit concept and writes its three
    // STOSDs unconditionally, so this is not a divergence either way -- but every other write in this
    // module checks st.out_end, and the review (2026-07-30) was right that leaving the header out of
    // that contract makes `dst_limit` mean "bounded except for the first twelve bytes".
    if (st.out_end != nullptr && st.out + sizeof(payload_header) > st.out_end) {
        st.overflowed = true;
        return 0;
    }
    const payload_header h{MAGIC, size, 0};
    std::memcpy(st.out, &h, sizeof h);
    st.out += sizeof h;

    emit_code(st, CODE_RESET); // 0x004f4936 -- every stream opens with one
    uint32_t cur = *in++;
    ++st.input_counter;

    bool cur_pending = true;
    while (in < input_end) { // `CMP ESI,end / JZ` -- see the divergence note below
        const uint32_t next = *in++;
        ++st.input_counter;

        const int32_t m = find_match(st, cur, next);
        if (m >= 0) {
            cur = static_cast<uint32_t>(m) + CODE_FIRST;
            continue;
        }
        emit_code(st, cur);
        if (insert(st, cur, next)) {
            // The reset path re-enters at 0x004f4936, which emits CODE_RESET again -- now at width
            // 9. The decoder tolerates the pair: the first resets it from the old width, the second
            // from the new one, and then it reads a fresh first code. Both are on the wire and both
            // have to be here for byte identity.
            emit_code(st, CODE_RESET);

            // DIVERGENCE, DELIBERATE. The original reads its next byte here without an
            // end-of-input test, so if the dictionary happens to fill on the token whose `next` is
            // the final input byte, ESI steps OVER `end` -- and since the loop test is an equality,
            // it never terminates. There is no output to be equivalent to on that path, so it is
            // bounded instead. `next` was just emitted and `cur` was emitted above, so the stream is
            // complete apart from its terminator: nothing is lost.
            if (in >= input_end) {
                cur_pending = false;
                break;
            }
            cur = *in++;
            ++st.input_counter;
            continue;
        }
        cur = next;
    }

    // 0x004f499d: the pending code, end-of-stream, then a zero code whose only job is to shift the
    // last partial byte out.
    if (cur_pending) emit_code(st, cur);
    emit_code(st, CODE_END);
    emit_code(st, 0);

    if (st.overflowed) return 0; // refused rather than written past the buffer

    const uint32_t total = static_cast<uint32_t>(st.out - dst8);
    std::memcpy(dst8 + offsetof(payload_header, compressed_len), &total, sizeof total);
    return total;
}

// ---------------------------------------------------------------------------- legacy entry point

// Kept so the exported C symbol keeps its contract: -1 for a payload that is not LZW, otherwise the
// decoded byte count. The static mirrors the original's globals -- one reusable dictionary, zeroed
// at first use.
int Decompress(char *input, char *output, size_t outputSize) {
    static decoder_state st{};
    uint32_t             io = static_cast<uint32_t>(outputSize);
    decompress(st, input, output, &io);
    return static_cast<int>(io);
}

} // namespace mh::lzw

namespace mh::lzss {

void decompress_block(const void *block, void *dst) {
    const uint8_t *in = static_cast<const uint8_t *>(block);
    uint32_t       uncompressed_size;
    std::memcpy(&uncompressed_size, in + 4, sizeof uncompressed_size); // the BLOCK header's size
    in += 8;

    uint8_t       *out = static_cast<uint8_t *>(dst);
    uint8_t *const end = out + uncompressed_size;

    // The flag word is refilled the moment its sixteenth bit is consumed, not lazily on the next
    // request (0x004ddcd7: DEC AH / JNZ, and the reload does not disturb the carry just taken).
    uint16_t flags = 0;
    int      have  = 0;
    std::memcpy(&flags, in, sizeof flags);
    in += sizeof flags;
    have = 16;

    const auto next_bit = [&]() -> bool {
        const bool bit = (flags & 0x8000u) != 0; // `SHL DX,1` takes the carry from before the shift
        flags          = static_cast<uint16_t>(flags << 1);
        if (--have == 0) {
            std::memcpy(&flags, in, sizeof flags);
            in += sizeof flags;
            have = 16;
        }
        return bit;
    };

    while (out < end) { // `CMP EDI,end / JNC` -- tested only between tokens, hence the 8-byte overrun
        if (!next_bit()) {
            *out++ = *in++;
            continue;
        }
        uint32_t len = 0;
        for (int i = 0; i < 3; ++i) len = (len << 1) | (next_bit() ? 1u : 0u); // three RCL CL,1
        len += 2;

        // `MOV BL,AL` into an EBX of 0xffffffff, so the offset byte is a distance of 256-offset
        // back: 0 is the furthest (256) and 255 is the nearest (1).
        const uint32_t offset = *in++;
        const uint8_t *copy   = out + offset - 256;
        for (uint32_t i = 0; i < len; ++i) *out++ = *copy++; // byte at a time: runs may overlap
    }
}

} // namespace mh::lzss

extern "C" int MH_LZW_Decompress(char *input, char *output, size_t outputSize) {
    return mh::lzw::Decompress(input, output, outputSize);
}
