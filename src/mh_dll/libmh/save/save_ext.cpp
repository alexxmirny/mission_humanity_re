//
// save/save_ext.cpp -- see save_ext.h for why an extension is a length-prefixed opaque member and why
// it must be paired with a version the vanilla table refuses.
//
#include "save_ext.h"

#include <cstring>

#include "save_format.h"

namespace mh::save {

int write_extension(const block_io &io, uint32_t tag, const void *data, uint32_t length) {
    const extension_header h{EXT_MAGIC, tag, length};
    if (io.write(io.ctx, &h, sizeof h) != 1) return 1;
    // A zero-length member is legal and means "the tag is present, the array is empty". Guarding the
    // write matters because the CRT helper is called as (data, size, count=1) over fwrite, and fwrite
    // writes zero ITEMS when the item size is zero -- so it returns 0, not 1, which the 0/1 convention
    // would then report as a failure. The mem_file harness models that return specifically, because a
    // harness that returned 1 here would make this guard's mutation unobservable.
    if (length == 0) return 0;
    return io.write(io.ctx, data, length) == 1 ? 0 : 1;
}

// NOTE THE ASYMMETRY IN block_io, which is the CRT's and not ours: `write` mirrors
// write_to_file(data, size, count=1, handle) and returns the ITEM count, so 1 means success; `read`
// mirrors read_from_file and returns the BYTE count. Getting that backwards here cost one build --
// worth the comment, because "!= 1" reads as correct on both.
int read_extension_header(const block_io &io, extension_header *out) {
    extension_header h{};
    if (io.read(io.ctx, &h, sizeof h) != static_cast<int>(sizeof h)) return 1;
    if (h.magic != EXT_MAGIC) return 1;
    *out = h;
    return 0;
}

int read_extension_payload(const block_io &io, const extension_header &hdr, void *dst,
                           uint32_t dst_size) {
    if (hdr.length > dst_size) return 1;
    if (hdr.length == 0) return 0;
    if (io.read(io.ctx, dst, hdr.length) != static_cast<int>(hdr.length)) return 1;
    return 0;
}

bool extension_requires_version_refusal(int32_t vanilla_detect_result) {
    return vanilla_detect_result == VERSION_REJECT;
}

} // namespace mh::save
