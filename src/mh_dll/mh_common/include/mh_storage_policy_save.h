// The one storage-policy file grammar, shared by hosted and standalone save carriers.
#pragma once
#include "mh_storage_policy.h"
#include "mh_order_tasks.h"
#include <cstring>

namespace mh::storage_policy::savefile {
inline constexpr uint32_t POLICY_FOOTER    = 0x54465053u; // SPFT
inline constexpr uint32_t CONTAINER_FOOTER = 0x46435053u; // SPCF
inline constexpr uint32_t OVERFLOW_FOOTER  = 0x464f484du; // MHOF
inline constexpr uint32_t REQUIRE_POLICY = 1, REQUIRE_CAPS = 2, REQUIRE_QUEUE = 4;
inline constexpr uint32_t COMPOSITE_SCHEMA = 2, COMPOSITE_HEADER_BYTES = 32;
// SPOLv2 header32 LE: magic,schema,payloadbytes,CRC32(payload),required(POLICY|QUEUE),
// policy-envelope-bytes,route-envelope-bytes,reserved0; payload=[SPOLv1][QORD].
// SPCMv2 LE204: magic,schema,members,policy,caps,native-version,routes,160,required,
// CRC32(QCAM); raw QCAM160 at40; CRC32(first200) at200. Existing SPFT/SPCF locators
// stay length+magic8. QCAM is container-only; a planet member never resets its clock.
// MSQ2/MBQ2 header stamps make queue-required saves unrecognizable to older loaders.
// Binding to the SAME canonical owners used by execution/hash/resync, not a save-only singleton.
struct order_context {
    mh::order_tasks::state          *owner                      = nullptr;
    mh::order_tasks::campaign_state *campaign                   = nullptr;
    void                            *backend                    = nullptr;
    bool (*current_world)(void *, mh::order_tasks::world_key &) = nullptr;
    // Reshape only; leave callbacks quarantined until finish_load after final imports.
    bool (*prepare_loaded)(void *, mh::order_tasks::dimensions, mh::order_tasks::world_key) = nullptr;
    void (*clear_pending)(void *)                                                           = nullptr;
    // Read the loaded canonical native roster, NOT storage-command eligibility.
    // Legacy SPOL1 lacks ordinary-building lives; migration stages missing lives only.
    bool (*building_exists)(void *, uint32_t player, uint32_t slot) = nullptr;
    // Actual loader lifecycle, never called by read-only preflight or arbitrary file open.
    // begin_load idempotently quarantines old execution/accounting WITHOUT native subtraction
    // or canonical QORD/QCAM mutation. Native token API binds as token!=UINT32_MAX; token0
    // succeeds. Actual block-load failure stays quarantined; only atomic rollback restores.
    bool (*begin_load)(void *) = nullptr;
    // Validate/rebind final incoming owner and accounting; then reactivate. Failure stays off.
    // legacy_routes means decoded QORD bytes were ABSENT, not merely zero route counts.
    // Import existing native primary booking baselines only at that genuine migration.
    bool (*finish_load)(void *, bool legacy_routes) = nullptr;
};
struct file_io {
    int32_t (*tell)(void *);
    int (*seek)(void *, int32_t, int);
    uint32_t (*read)(void *, void *, uint32_t);
    uint32_t (*write)(void *, const void *, uint32_t);
};
enum class stream : uint8_t { planet,
                              container,
                              regions };
enum class step_kind : uint8_t { block,
                                 regions,
                                 progress,
                                 members,
                                 media,
                                 tail };
struct step {
    uint32_t  address = 0, size = 0;
    step_kind kind        = step_kind::block;
    uint8_t   version_min = 0, version_max = 255;
};
// Adapts the EXISTING generated native save table. No second handwritten block sequence.
struct layout {
    void *context                                             = nullptr;
    bool (*read_step)(void *, stream, uint32_t index, step &) = nullptr;
    uint32_t packed_limit = 0, width_address = 0, height_address = 0;
    uint32_t planets_address = 0, planets_bytes = 0, planet_stride = 0, planet_system_offset = 0;
    uint32_t session_address = 0, session_bytes = 0;
    uint32_t current_system_address = 0, current_planet_address = 0, planet_status_address = 0;
    uint32_t member_first = 0, member_limit = 0;
    uint32_t region_count_address = 0, region_record_bytes = 0, region_grid_bytes = 0, region_terrain_bytes = 0;
    uint32_t progress_count = 0, media_bytes = 0;
};
struct context {
    layout         native;
    order_context  orders{};
    bool           queue_supported = false;
    dimensions     capacity{};                                            // real players + roster ROWs; planet dimensions come from its own blocks
    const uint8_t *versions                                    = nullptr; // six native 40-byte strings
    state         *owner                                       = nullptr; // caller-owned canonical region; never a second process singleton
    void          *backend                                     = nullptr;
    bool           caps_supported                              = false;
    uint32_t       overflow_max_bytes                          = 0;
    bool (*check_overflow)(void *, const void *, uint32_t)     = nullptr; // read-only full MHOV validation
    int32_t (*write_overflow)(void *, const file_io &, void *) = nullptr; // existing MHOV encoder, bytes/negativefailure
    // Called only after complete file validation. Reshape owner's existing region to map dimensions;
    // legacy=true also resets/seeds actual loaded live roster identities, no qualify/launch/sweep.
    bool (*prepare_loaded_owner)(void *, uint32_t width, uint32_t height, bool legacy) = nullptr;
};
struct requirements {
    uint32_t members = 0, policy = 0, caps = 0, routes = 0;
    int32_t  version = -1;
};
enum class preflight_result : uint8_t { accepted,
                                        invalid,
                                        unsupported_caps,
                                        unsupported_queue };
// Classify all 40 bytes against an exact native slot, including required-support variants.
inline int32_t classify_header(const uint8_t *h, const uint8_t *versions, uint32_t &required) {
    required = 0;
    if (!h || !versions) return -1;
    int32_t answer = -1;
    for (int32_t i = 1; i != 6; ++i) {
        const uint8_t *v = versions + 40 * i;
        if (!std::memcmp(h, v, 40)) {
            answer   = i;
            required = 0;
            continue;
        }
        if (std::memcmp(h, v, 29) || std::memcmp(h + 33, v + 33, 7)) continue;
        uint32_t bits = 0;
        if (!std::memcmp(h + 29, "MHC1", 4)) bits = REQUIRE_CAPS;
        else if (!std::memcmp(h + 29, "MSP1", 4)) bits = REQUIRE_POLICY;
        else if (!std::memcmp(h + 29, "MSB1", 4)) bits = REQUIRE_POLICY | REQUIRE_CAPS;
        else if (!std::memcmp(h + 29, "MSQ2", 4)) bits = REQUIRE_POLICY | REQUIRE_QUEUE;
        else if (!std::memcmp(h + 29, "MBQ2", 4)) bits = REQUIRE_POLICY | REQUIRE_CAPS | REQUIRE_QUEUE;
        if (bits) {
            answer   = i;
            required = bits;
        }
    }
    return answer;
}
bool inspect_planet(const context &, const file_io &, void *, int32_t offset, uint32_t length,
                    bool &has_policy, bool &has_caps);
bool preflight_planet(const context &, const file_io &, void *);
// Call once admission succeeds, before ACTUAL native bulk writes. Nested container->member
// begins are idempotent; only final member import calls finish_load. Read-only probes never do.
bool             begin_load(const context &);
bool             load_planet(const context &, const file_io &, void *);
bool             write_planet(const context &, const file_io &, void *);
preflight_result preflight_container(const context &, const file_io &, void *, requirements *out = nullptr);
// After native container blocks/extraction succeed, BEFORE loading the current member.
// Revalidates without native mutation, imports exact QCAM (or deterministic legacy seed),
// and clears unadmitted local transport. A planet load NEVER installs a campaign clock.
bool load_container(const context &, const file_io &, void *);
bool finish_container(const context &, const file_io &, void *);
bool normalize_version(const context &, uint8_t (&header)[40]);
} // namespace mh::storage_policy::savefile
