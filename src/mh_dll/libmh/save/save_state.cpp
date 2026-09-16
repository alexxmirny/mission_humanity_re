//
// save/save_state.cpp -- see save_state.h. The one place libmh/save/ names a non-block address.
//
#include "save/save_state.h"

#include "addr/mh_regions.gen.h" // the state region REGISTRY -- where this module's state lives

namespace mh::save {

// The `RID_*` enumerators live in `mh::state`, and this file exists to name them.
using namespace mh::state;

live_binds binds() {
    return live_binds{
        ptr<uint8_t>(RID_G_LZW_TEMP_DATA),
        ptr<uint32_t>(RID_G_LZW_MAX_COMPRESSED_SIZE),

        ptr<const char>(RID_G_SAVE_DIR),
        ptr<const char>(RID_G_SAVE_TEMP_DIR),

        ptr<double>(RID_PLANET_TIME),
        ptr<int32_t>(RID_G_PLANET_STATUS),
        ptr<const double>(RID_STRAT_GAME_CLOCK),
        ptr<const uint32_t>(RID_G_PLANET_INDEX),
        ptr<const double>(RID_LAST_GAME_TIME),

        ptr<uint32_t>(RID_BUILD_PLACEMENT_ID),
        ptr<uint32_t>(RID_STRAT_BUILD_PREVIEW_SUPPRESS_FLAG),
        ptr<const int32_t>(RID_MAP_CAM_COL),
        ptr<const int32_t>(RID_MAP_CAM_ROW),

        ptr<uint32_t>(RID_G_SAVE_MODE_RESET_DWORDS),

        ptr<const char>(RID_S_EXTERMINATION__DEMO_VERSION_005D08F4),
        ptr<const int32_t>(RID_GAME_MISSION_VERSION_INDEX),
    };
}

// ---- THREE UNDER-MEASURED REGIONS, RECORDED RATHER THAN QUIETLY BOUND ---------------------------
//
// Converting these 31 sites from stock addresses to registry lookups makes them follow a bind. It
// does NOT make the registry's idea of their SIZE correct, and for four of them it is not:
//
//   RID_G_SAVE_MODE_RESET_DWORDS   size 0, reach 0
//   RID_G_SAVE_DIR                 size 1, reach 1   (an ANSI path fragment, read to its NUL)
//   RID_G_SAVE_TEMP_DIR            size 1, reach 1   (likewise)
//   RID_G_LZW_TEMP_DATA            size 1, reach 1   (the whole compression staging buffer)
//
// A size is what the five manifests between them MEASURED, and an instruction scan attributes a
// byte to a symbol only where an instruction names that byte's address. A buffer walked by a
// pointer the code advances -- a string copied to its terminator, a compression window filled by a
// loop -- has exactly one named address, its base, so it measures as one byte however large it is.
//
// WHY THIS IS SAFE TODAY AND NOT SAFE UNDER SB-HOSTFREE. `ptr<T>(RID)` is `live_base(RID)` and does
// not consult the size, so on a stock or a whole-region bind these reads are exactly what they were.
// The size matters the moment a HOST binds a relocated copy: it declares how many bytes the host
// promised, `mh::state::translate` refuses a window that runs past it, and a host sizing its arena
// off the registry would allocate one byte for a path buffer. So this is a real gap in the
// measurement, not in this file, and SB-HOSTFREE's reach work is where it gets closed. It is
// written down here because a converted call site LOOKS complete, and the next reader deserves to
// know which four of these are complete only in the address and not yet in the extent.
//
// HOSTED-ONLY (LIB-REF-SPLIT). It asserts the stock base is non-zero, and standalone that
// column IS zero by construction (MH_STOCK_BASE) -- the assert would be checking the build
// configuration, not the registry. What it actually guards is a HOSTED property: that the
// save-dir region has a real address to resolve against.
#ifndef MH_LIBMH_BUILD
static_assert(REGIONS[RID_G_SAVE_DIR].base != 0, "the save-dir region resolves");
#endif

} // namespace mh::save
