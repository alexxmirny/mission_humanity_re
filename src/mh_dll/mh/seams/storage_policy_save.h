// Cap-independent storage policy save carrier. FILE handles are opaque Watcom CRT objects.
#pragma once
#include <cstdint>
#include "caps/ovf_save.h"
#include "include/mh_storage_policy_save.h"

namespace mh::storage_policy::save {
using file_io                              = mh::caps::ovf::file_io;
inline constexpr uint32_t POLICY_FOOTER    = savefile::POLICY_FOOTER;
inline constexpr uint32_t CONTAINER_FOOTER = savefile::CONTAINER_FOOTER;
inline constexpr uint32_t REQUIRE_POLICY   = savefile::REQUIRE_POLICY;
inline constexpr uint32_t REQUIRE_CAPS     = savefile::REQUIRE_CAPS;
inline constexpr uint32_t REQUIRE_QUEUE    = savefile::REQUIRE_QUEUE;
void                      bind_order_context(const savefile::order_context &);
using requirements     = savefile::requirements;
using preflight_result = savefile::preflight_result;
// Writes ONLY at the current fresh native planet EOF; refuses an already-present extension.
// Appends SPOL v1 or required composite v2 [policy,QORD], then SPFT and optional MHOV/MHOF.
// Any failure is a failed save, including an MHOV write failure. No caps.raise dependency.
bool write_planet(const file_io &, void *file);
// Read-only structural validation (CRC, shape, envelope), also for unsupported capabilities.
// offset/length describe a standalone planet or a verbatim embedded container member.
bool inspect_planet(const file_io &, void *file, int32_t offset, uint32_t length,
                    dimensions capacity, bool &has_policy, bool &has_caps);
// Call BEFORE the first planet block read (read-only). A malformed carrier refuses before mutation.
bool preflight_planet(const file_io &, void *file);
// Call after ALL planet blocks and dimensions loaded, before any loaded-world callbacks.
// Import validates before publishing. Legacy files reset to empty/default policy.
bool load_planet(const file_io &, void *file);
// Shared preflight for utils_open_file(.sav,rb), BEFORE any game loader reads/mutates state.
// Validates every embedded planet and exact required-feature manifest. Never publishes policy.
// versions points at the original six 40-byte native strings. Restores position on all paths.
preflight_result preflight_container(const file_io &, void *file, const uint8_t *versions,
                                     dimensions capacity, requirements *out = nullptr);
// At successful container EOF, BEFORE append_origin(MHM1). Inspects actual embedded members,
// Appends required-feature manifest; new queue builds use MSQ2/MBQ2 and container-only QCAM.
// Rejects existing metadata rather than blindly appending behind an old footer. Restores EOF.
bool finish_container(const file_io &, void *file, const uint8_t *versions,
                      dimensions capacity);
// Normalize only a recognized/supportable required-feature stamp to its exact native slot.
// Must be called in BOTH native header-read and promoted detect_version paths, AFTER preflight.
bool normalize_version(uint8_t (&header)[40], const uint8_t *versions);

// Concrete mh.dll adapters for host-table twins. Handles remain opaque 32-bit Watcom FILE values.
// Each returns 1 on success, 0 on refusal/failure. No native-body splice is assumed to be active.
int32_t host_write_planet(int32_t handle);
int32_t host_preflight_planet(int32_t handle);
int32_t host_begin_load();
int32_t host_load_planet(int32_t handle);
int32_t host_preflight_container(int32_t handle);
int32_t host_load_container(int32_t handle);
int32_t host_finish_container(int32_t handle);
int32_t host_normalize_version(void *header40);
} // namespace mh::storage_policy::save
