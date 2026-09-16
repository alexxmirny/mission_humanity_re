//
// save/save_block.cpp -- SV1 batch B: the save file's block layer. Contract and the address map are
// in save_block.h; this file is the translation, and the comments are about what the disassembly does
// that a reasonable-looking reimplementation would not.
//
#include "save/save_block.h"

#include <cstring>

namespace mh::save {

int write_block(const block_io &io, const block_workspace &ws, const void *src, uint32_t size) {
    block_header h{};

    // 0x004cecf5: the uncompressed size goes in first, then Compress fills base+8 and reports how
    // many bytes it wrote, which becomes the first field (0x004ced1b).
    h.uncompressed_size = size;
    h.compressed_len    = lzw::compress(*ws.encoder, src, size, ws.staging + sizeof(block_header),
                                        STAGING_BYTES - sizeof(block_header));
    // 0 means the encoder would have run past the staging buffer. THE ORIGINAL HAS NO SUCH CHECK --
    // Compress writes into G_LZW_TEMP_DATA+8 unbounded, so an array whose compressed form approaches
    // 600 000 bytes overflows it. Refusing is not faithful; overflowing is not a behaviour worth
    // reproducing. Note the reader has the only cap in the binary, which is why this asymmetry is a
    // real ceiling on cap raises rather than a curiosity -- see docs/save-format.md.
    //
    // THE LIMIT PASSED ABOVE IS `STAGING_BYTES - 8`, WHICH IS EXACTLY BLOCK_READ_CAP. That makes the
    // two directions agree -- the reader accepts a block of exactly BLOCK_READ_CAP compressed bytes
    // (the test is inclusive), so the writer must be able to produce one. It is in bounds only
    // because STAGING_BYTES carries the eight bytes of slack the ORIGINAL's buffer does not have.
    // A NOTE FOR SV1-P: binding `ws.staging` to the real G_LZW_TEMP_DATA reintroduces the original's
    // 8-byte overrun at the boundary, because that buffer is exactly BLOCK_READ_CAP bytes with the
    // payload at +8. Either keep our own slack-carrying buffer, or lower this limit to
    // BLOCK_READ_CAP - 8 and accept that a block at exactly the cap can then be read but not written.
    if (h.compressed_len == 0) return 1;
    std::memcpy(ws.staging, &h, sizeof h);

    // 0x004486a2: write_to_file(buffer, compressed_len + 8, 1, handle), and anything but 1 is a
    // failure. Note the size written is the payload plus the header, i.e. one contiguous block.
    int result = 0;
    if (io.write(io.ctx, ws.staging, h.compressed_len + sizeof(block_header)) != 1) result = 1;

    // 0x004486c3: a high-water mark of the largest compressed block this run has produced. Nothing
    // in the binary reads it -- the only two references are this read and this write -- so it is a
    // statistic, not a control input. Maintained anyway: it costs nothing and a future reader of the
    // original's value would otherwise see it stop moving the moment this module is promoted.
    if (ws.max_compressed_seen != nullptr && *ws.max_compressed_seen < h.compressed_len)
        *ws.max_compressed_seen = h.compressed_len;

    return result;
}

int read_block(const block_io &io, const block_workspace &ws, void *dst, uint32_t dst_size) {
    // 0x0044870d: exactly 8 bytes, into the base of the staging buffer.
    io.read(io.ctx, ws.staging, sizeof(block_header));

    block_header h{};
    std::memcpy(&h, ws.staging, sizeof h);

    // 0x0044872f. THIS IS THE PER-BLOCK SELF-CHECK, and it is the only reason a mismatched save gets
    // refused below the version gate: the writer stamped how many bytes this block expands to, and
    // the reader knows how many the caller has room for. A cap-raised build's block does not fit a
    // vanilla build's array, and this is where that is caught.
    if (h.uncompressed_size != dst_size) return 1;

    // 0x00448744, `JBE` -- inclusive, so exactly BLOCK_READ_CAP passes. See STAGING_SLACK.
    if (h.compressed_len > BLOCK_READ_CAP) return 1;

    // 0x0044875f, and THE RESULT IS DISCARDED, faithfully -- `MOV [EBP-0x18],EAX` is a dead store.
    // A truncated file therefore hands the decoder a payload that stops early, and it carries on
    // over whatever the previous block left in the staging buffer. Neither the header check above
    // (already passed) nor the decoder's status distinguishes that from a valid block, which is the
    // real answer to docs/save-format.md's open question 1: refusal is a HEADER-level property, not
    // a payload-level one. `strict_reads` is the opt-in fix; see the header.
    const int got = io.read(io.ctx, ws.staging + sizeof(block_header), h.compressed_len);
    if (ws.strict_reads && static_cast<uint32_t>(got) != h.compressed_len) return 1;

    // UnpackSaveData 0x004ced2b: seed the size with the payload length, decode, then classify.
    //
    // dst_size is passed as the output bound. The original has none, so a corrupt payload writes past
    // the caller's array -- which in the promoted path is one of the game's. The header check above
    // has already established that dst_size is what this block claims to expand to, so bounding by it
    // cannot reject anything a valid block would produce.
    uint32_t io_size = h.compressed_len;
    lzw::decompress(*ws.decoder, ws.staging + sizeof(block_header), dst, &io_size, dst_size);

    // 0x004ced6e, and the order of the tests is the order the original wrote them, SIGNED:
    //   < -1  -> 0 (success), which is a decoded length so large it reads as negative
    //   == -1 -> -1, "not LZW", handled below
    //   == 0  -> 1 (failure), the reset counter ran out
    //   else  -> 0 (success)
    const int32_t signed_size = static_cast<int32_t>(io_size);
    int           status;
    if (signed_size < -1) status = 0;
    else if (signed_size == -1) status = -1;
    else if (io_size == 0) status = 1;
    else status = 0;

    // 0x0044877d. Not an error path: -1 means the payload was not LZW, so the block is in the other
    // format and the LZSS decoder runs over the same buffer, overwriting the raw copy `decompress`
    // just made. The forced success afterwards is therefore correct, not a swallowed failure.
    if (status == -1) {
        lzss::decompress_block(ws.staging, dst);
        status = 0;
    }

    return status;
}

} // namespace mh::save
