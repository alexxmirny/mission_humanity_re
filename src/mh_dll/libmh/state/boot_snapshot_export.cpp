// The byte engine is also compiled into mh.dll. Only libmh's public import
// boundary owns the selected presentation witness store and its publication reset.
#include "state/boot_snapshot.h"
#include "state/host_events.h"

extern "C" int libmh_import_snapshot(const void *blob, size_t n) {
    const uint32_t before = mh::order_witness::generation(mh::state::order_witnesses());
    const int      rc     = mh::state::boot::import(blob, n);
    if (rc == mh::state::boot::BOOT_OK)
        mh::state::reset_order_witnesses_if_unchanged(before);
    return rc;
}
