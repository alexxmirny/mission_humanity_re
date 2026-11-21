#include "storage_policy_save_host.h"
#include "include/mh_storage_policy_save_layout.h"
#include "state/storage_policy_runtime.h"
#include "state/order_tasks_runtime.h"
#include "state/region_runtime.h"
#include "state/roster_caps.h"
#include <climits>
#include <cstring>

namespace mh::refhost::storage_save {
namespace {
namespace carrier                   = mh::storage_policy::savefile;
file_resolver          resolve_file = nullptr;
carrier::order_context order_binding{};

int32_t tell(void *opaque) {
    if (!opaque) return -1;
    const long pos = std::ftell(static_cast<FILE *>(opaque));
    return pos < 0 || pos > INT32_MAX ? -1 : static_cast<int32_t>(pos);
}
int seek(void *opaque, int32_t offset, int whence) {
    if (!opaque || whence < 0 || whence > 2) return -1;
    const int origin = whence == 0 ? SEEK_SET : (whence == 1 ? SEEK_CUR : SEEK_END);
    return std::fseek(static_cast<FILE *>(opaque), offset, origin);
}
uint32_t read(void *opaque, void *dst, uint32_t bytes) {
    if (!opaque || (!dst && bytes)) return 0;
    return static_cast<uint32_t>(std::fread(dst, 1, bytes, static_cast<FILE *>(opaque)));
}
uint32_t write(void *opaque, const void *src, uint32_t bytes) {
    if (!opaque || (!src && bytes)) return 0;
    return static_cast<uint32_t>(std::fwrite(src, 1, bytes, static_cast<FILE *>(opaque)));
}
const carrier::file_io IO{tell, seek, read, write};

// The portable save driver serializes the generated native windows directly. It has no hosted
// relocated-array gather/scatter or MHOV backend. Refuse wider REAL pools, not a larger policy
// reservation: dead canonical high slots are valid even when active native pools are stock-sized.
bool native_windows_supported() {
    using namespace mh::state;
    constexpr region_id pools[] = {RID_BUILDINGS, RID_UNIT_STORAGE, RID_TURRETS, RID_MINES,
                                   RID_LABS, RID_PRODUCTIONS, RID_STRAT_PROJECTILE_POOL, RID_UNITS, RID_STRAT_SOLDIERS};
    for (region_id id : pools)
        if (!ptr<void>(id) || live_size(id) != REGIONS[id].reach) return false;
    const auto actual   = mh::storage_policy::runtime::roster_dimensions();
    const auto reserved = mh::storage_policy::runtime::bound_state().shape();
    return actual.players == ROSTER_PLAYERS && actual.buildings == STOCK_ROSTER_CAPS.buildings &&
           actual.units == STOCK_ROSTER_CAPS.units && reserved.players == actual.players &&
           reserved.buildings >= actual.buildings && reserved.units >= actual.units;
}
bool ready() {
    return resolve_file && mh::storage_policy::runtime::bound_state().valid() &&
           mh::order_tasks::runtime::bound_state().valid() &&
           mh::order_tasks::runtime::bound_campaign().valid() && native_windows_supported();
}
carrier::order_context runtime_order_context() {
    namespace R  = mh::state;
    namespace OT = mh::order_tasks;
    carrier::order_context c;
    c.owner         = &OT::runtime::bound_state();
    c.campaign      = &OT::runtime::bound_campaign();
    c.current_world = [](void *, OT::world_key &key) {
        const auto *system = R::ptr<const int32_t>(R::RID_CURRENTSYSTEM);
        const auto *planet = R::ptr<const int32_t>(R::RID_G_PLANET_INDEX);
        if (!system || !planet || *system < 0 || *planet < 0 || *planet >= 32) return false;
        const uint32_t incarnation = OT::runtime::bound_campaign().world_inc(uint32_t(*planet));
        if (!incarnation) return false;
        key = {uint32_t(*system), uint32_t(*planet), incarnation};
        return true;
    };
    c.prepare_loaded = [](void *, OT::dimensions d, OT::world_key key) {
        auto      &owner    = OT::runtime::bound_state();
        const auto reserved = owner.shape();
        return reserved.players == d.players && reserved.units == d.units &&
               reserved.buildings == d.buildings &&
               OT::runtime::bound_campaign().world_inc(key.planet) == key.incarnation &&
               owner.initialize(owner.data(), owner.size(), d, key);
    };
    c.clear_pending = [](void *) {
        // Actual begin_load is idempotent and clears source-origin/handler-budget stacks
        // without touching canonical bytes or admitted recorded future commands. These
        // calls happen only inside an actual load; leave execution quarantined until finish.
        (void)libmh_order_tasks_begin_load();
    };
    c.building_exists = [](void *, uint32_t player, uint32_t slot) {
        const auto world = R::storage_policy_tables(R::ROSTER_PLAYERS);
        if (player >= R::ROSTER_PLAYERS || !slot || slot >= world.building_rows ||
            !world.buildings || !world.building_types) return false;
        const auto &b = world.buildings[player * world.building_rows + slot];
        return b.index > 0 && b.state != 2 && b.energy > 0 && b.building_id &&
               b.building_id < world.building_types_count;
    };
    c.begin_load = [](void *) {
        // Token zero means successful quarantine of an already inactive runtime.
        return uint32_t(libmh_order_tasks_begin_load()) != UINT32_MAX;
    };
    c.finish_load = [](void *, bool legacy_routes) {
        if (legacy_routes && !OT::runtime::import_legacy_bindings()) return false;
        mh_order_tasks_request request{};
        mh_order_tasks_result  result{};
        request.operation = uint32_t(OT::host_protocol::operation::published);
        return libmh_order_tasks_invoke(&request, &result) == 1 && result.values[0] == 1;
    };
    return c;
}
bool prepare_loaded(void *, uint32_t width, uint32_t height, bool legacy) {
    mh_storage_policy_request request{};
    mh_storage_policy_result  result{};
    request.operation = static_cast<uint32_t>(mh::storage_policy::operation::map_extent);
    request.args[0]   = width;
    request.args[1]   = height;
    if (libmh_storage_policy_invoke(&request, &result) != 1) return false;
    if (!legacy) return true;
    request           = {};
    request.operation = static_cast<uint32_t>(mh::storage_policy::operation::reset_loaded_roster);
    return libmh_storage_policy_invoke(&request, &result) == 1;
}
carrier::context live_context() {
    carrier::context c;
    c.native          = carrier::native_layout();
    c.orders          = order_binding;
    c.queue_supported = order_binding.owner && order_binding.campaign;
    c.owner           = &mh::storage_policy::runtime::bound_state();
    c.capacity        = c.owner->shape();
    c.versions        = carrier::native_versions();
    // Explicit unsupported MHOV mode: the common preflight refuses MHC1/MSB1 and every MHOF
    // before native blocks load. Never report success while silently dropping overflow bytes.
    c.caps_supported       = false;
    c.prepare_loaded_owner = prepare_loaded;
    return c;
}
FILE       *file(int32_t handle) { return resolve_file ? resolve_file(handle) : nullptr; }
const char *basename(const char *path) {
    if (!path) return nullptr;
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
bool equal(const char *a, const char *b) {
    while (*a && *b && lower(*a) == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}
enum class path_kind { other,
                       planet,
                       container };
path_kind classify(const char *path) {
    const char *base = basename(path);
    if (!base) return path_kind::other;
    const size_t n = std::strlen(base);
    if (n >= 4 && equal(base + n - 4, ".sav")) return path_kind::container;
    const char *p = base;
    if (lower(*p) == 'q') ++p;
    if (std::strlen(p) != 10) return path_kind::other;
    if (lower(p[0]) != 's' || lower(p[1]) != 'a' || lower(p[2]) != 'v' || lower(p[3]) != 'e' ||
        p[4] < '0' || p[4] > '9' || p[5] < '0' || p[5] > '9' || !equal(p + 6, ".dat"))
        return path_kind::other;
    return path_kind::planet;
}
} // namespace

void bind_order_context(const carrier::order_context &binding) { order_binding = binding; }
bool initialize(file_resolver resolver) {
    if (!resolver || !mh::storage_policy::runtime::bound_state().valid() ||
        !mh::order_tasks::runtime::bound_state().valid() ||
        !mh::order_tasks::runtime::bound_campaign().valid()) return false;
    bind_order_context(runtime_order_context());
    resolve_file = resolver;
    return true;
}
bool is_save_output(const char *path) { return classify(path) != path_kind::other; }
bool preflight_open(FILE *opened, const char *path) {
    switch (classify(path)) {
        case path_kind::other: return true;
        case path_kind::planet:
            return ready() && carrier::preflight_planet(live_context(), IO, opened);
        case path_kind::container:
            return ready() && carrier::preflight_container(live_context(), IO, opened) == carrier::preflight_result::accepted;
    }
    return false;
}
int32_t write_planet(int32_t handle) {
    if (!ready()) return 0;
    FILE *const f = file(handle);
    return f && carrier::write_planet(live_context(), IO, f) &&
                   std::fflush(f) == 0 && !std::ferror(f)
               ? 1
               : 0;
}
int32_t preflight_planet(int32_t handle) {
    return ready() && carrier::preflight_planet(live_context(), IO, file(handle)) ? 1 : 0;
}
int32_t begin_load() {
    return ready() && carrier::begin_load(live_context()) ? 1 : 0;
}
int32_t load_planet(int32_t handle) {
    return ready() && carrier::load_planet(live_context(), IO, file(handle)) ? 1 : 0;
}
int32_t preflight_container(int32_t handle) {
    return ready() && carrier::preflight_container(live_context(), IO, file(handle)) == carrier::preflight_result::accepted ? 1 : 0;
}
int32_t load_container(int32_t handle) {
    return ready() && carrier::load_container(live_context(), IO, file(handle)) ? 1 : 0;
}
int32_t finish_container(int32_t handle) {
    if (!ready()) return 0;
    FILE *const f = file(handle);
    return f && carrier::finish_container(live_context(), IO, f) &&
                   std::fflush(f) == 0 && !std::ferror(f)
               ? 1
               : 0;
}
int32_t normalize_version(void *header40) {
    return header40 && ready() &&
                   carrier::normalize_version(live_context(), *static_cast<uint8_t(*)[40]>(header40))
               ? 1
               : 0;
}
int32_t planet_save_prepare() { return ready() ? 1 : 0; }
int32_t planet_load_extensions(int32_t handle) {
    // Native fixed windows ARE the live backend here. No scatter/path mutation is necessary; the
    // full common probe establishes that there is no unsupported overflow to apply.
    return preflight_planet(handle);
}
void planet_save_restore() {
    // prepare changes no native memory in this stock-window backend, so no scratch/path override
    // exists to restore. This pairs with the real refusal gate, not a pretend raised-caps gather.
}
} // namespace mh::refhost::storage_save
