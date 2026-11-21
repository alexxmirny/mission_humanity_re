#include "seams/storage_policy_save.h"
#include "seams/storage_policy.h"
#include "include/mh_storage_policy_save_layout.h"
#include "caps/caps.h"
#include "addr/mh_calls.gen.h"
#include "mods/save_meta.h"
#include "seams/lobby_speed.h"
#include "include/mh_unitselect_export.h"
#include "ui/selected_targets.h"
#include "state/region_runtime.h"
#include <cstdio>

// Existing cross-TU sink; avoid pulling the entire transport header into save IO.
void seam_log(const char *s);

namespace mh::storage_policy::save {
namespace {
namespace portable = mh::storage_policy::savefile;
namespace ovf      = mh::caps::ovf;
portable::order_context order_binding{};
portable::file_io       portable_io(const file_io &io) { return {io.tell, io.seek, io.read, io.write}; }
bool                    check_overflow(void *, const void *data, uint32_t bytes) {
    return (mh::caps::raised() ? ovf::check(data, bytes) : ovf::check_format(data, bytes)) == ovf::OVF_OK;
}
int32_t write_overflow(void *, const portable::file_io &io, void *f) {
    const file_io original{io.tell, io.seek, io.read, io.write};
    const auto   *planet = mh::state::ptr<int32_t>(mh::state::RID_G_PLANET_INDEX);
    if (!planet) return -1;
    return ovf::save_tail(original, f, *planet);
}
bool prepare_loaded(void *, uint32_t width, uint32_t height, bool legacy) {
    return hosted::setup(width, height) && (!legacy || hosted::reset_world());
}
dimensions policy_capacity() {
    return hosted::owner().shape();
}
const uint8_t *native_versions() {
    return portable::native_versions();
}
portable::context context(dimensions cap, const uint8_t *versions) {
    portable::context c;
    c.native               = portable::native_layout();
    c.orders               = order_binding;
    c.queue_supported      = order_binding.owner && order_binding.campaign;
    c.capacity             = cap;
    c.versions             = versions;
    c.owner                = &hosted::owner();
    c.caps_supported       = mh::caps::raised();
    c.overflow_max_bytes   = static_cast<uint32_t>(ovf::max_bytes_save());
    c.check_overflow       = check_overflow;
    c.write_overflow       = write_overflow;
    c.prepare_loaded_owner = prepare_loaded;
    return c;
}
void   *handle_file(int32_t h) { return reinterpret_cast<void *>(static_cast<uintptr_t>(h)); }
int32_t crt_tell(void *f) { return mh::call::file_tell(f); }
int     crt_seek(void *f, int32_t offset, int whence) {
    return mh::call::file_seek(f, offset, static_cast<uint32_t>(whence));
}
uint32_t crt_read(void *f, void *dst, uint32_t n) { return mh::call::utils_read_from_file(dst, 1u, n, f); }
uint32_t crt_write(void *f, const void *src, uint32_t n) {
    return mh::call::utils_write_to_file(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f)),
                                         1u, const_cast<void *>(src), n) == 1
               ? n
               : 0;
}
const file_io CRT_IO{crt_tell, crt_seek, crt_read, crt_write};
} // namespace
void bind_order_context(const savefile::order_context &binding) { order_binding = binding; }
bool inspect_planet(const file_io &io, void *f, int32_t offset, uint32_t length,
                    dimensions cap, bool &has_policy, bool &has_caps) {
    return portable::inspect_planet(context(cap, native_versions()), portable_io(io), f, offset,
                                    length, has_policy, has_caps);
}
bool preflight_planet(const file_io &io, void *f) {
    return portable::preflight_planet(context(policy_capacity(), native_versions()), portable_io(io), f);
}
bool load_planet(const file_io &io, void *f) {
    return portable::load_planet(context(policy_capacity(), native_versions()), portable_io(io), f);
}
bool write_planet(const file_io &io, void *f) {
    return portable::write_planet(context(policy_capacity(), native_versions()), portable_io(io), f);
}
preflight_result preflight_container(const file_io &io, void *f, const uint8_t *versions,
                                     dimensions cap, requirements *out) {
    return portable::preflight_container(context(cap, versions), portable_io(io), f, out);
}
bool finish_container(const file_io &io, void *f, const uint8_t *versions, dimensions cap) {
    return portable::finish_container(context(cap, versions), portable_io(io), f);
}
bool normalize_version(uint8_t (&header)[40], const uint8_t *versions) {
    return portable::normalize_version(context(policy_capacity(), versions), header);
}
int32_t host_write_planet(int32_t h) { return write_planet(CRT_IO, handle_file(h)) ? 1 : 0; }
int32_t host_preflight_planet(int32_t h) { return preflight_planet(CRT_IO, handle_file(h)) ? 1 : 0; }
int32_t host_begin_load() {
    return portable::begin_load(context(policy_capacity(), native_versions())) ? 1 : 0;
}
int32_t host_load_planet(int32_t h) {
    if (!load_planet(CRT_IO, handle_file(h))) return 0;
    MH_UnitSelect_Reset();
    mh::ui::selected_targets::reset();
    mh::seams::lobby_speed::enforce();
    return 1;
}
int32_t host_preflight_container(int32_t h) {
    ovf::set_refused_raised(false);
    requirements req;
    const auto   result = preflight_container(CRT_IO, handle_file(h), native_versions(), policy_capacity(), &req);
    if (result != preflight_result::accepted) {
        if (result == preflight_result::unsupported_caps) {
            ovf::set_refused_raised(true);
            seam_log("; [caps] load REFUSED: the save is a raised-caps save and [caps] raise is OFF\n");
        } else if (result == preflight_result::unsupported_queue) {
            seam_log("; [storage-save] load REFUSED: required strategic routes are unsupported\n");
        } else {
            seam_log("; [storage-save] container invalid\n");
        }
        return 0;
    }
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "; [storage-save] container accepted members=%08X policy=%08X caps=%08X version=%d\n",
                req.members, req.policy, req.caps, req.version);
    seam_log(line);
    if (req.caps) seam_log("; [caps] load: raised save accepted\n");
    const mh::mods::save::file_io mod_io{CRT_IO.tell, CRT_IO.seek, CRT_IO.read, CRT_IO.write};
    mh::mods::save::probe_container_io(mod_io, handle_file(h));
    return 1;
}
int32_t host_load_container(int32_t h) {
    return portable::load_container(context(policy_capacity(), native_versions()), portable_io(CRT_IO), handle_file(h)) ? 1 : 0;
}
int32_t host_finish_container(int32_t h) {
    if (!finish_container(CRT_IO, handle_file(h), native_versions(), policy_capacity())) return 0;
    const mh::mods::save::file_io mod_io{CRT_IO.tell, CRT_IO.seek, CRT_IO.read, CRT_IO.write};
    return mh::mods::save::append_origin(mod_io, handle_file(h)) >= 0 ? 1 : 0;
}
int32_t host_normalize_version(void *header40) {
    if (!header40 || !normalize_version(*static_cast<uint8_t(*)[40]>(header40), native_versions())) return 0;
    mh::seams::lobby_speed::clear();
    MH_UnitSelect_Reset();
    return 1;
}
} // namespace mh::storage_policy::save
