#include "include/mh_storage_policy.h"
#include "include/mh_storage_policy_save_layout.h"
#include "include/mh_lzw.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace mh::storage_policy;
struct trial {
    launch_result result = launch_result::deferred;
    uint32_t      calls = 0, x = 0, y = 0;
};
launch_result launch(void *context, identity, identity, rally_view r) {
    auto &t = *static_cast<trial *>(context);
    ++t.calls;
    t.x = r.x;
    t.y = r.y;
    return t.result;
}

namespace SF = mh::storage_policy::savefile;
namespace OT = mh::order_tasks;
struct queue_save_fixture {
    std::vector<uint8_t> file, policy_bytes, route_bytes;
    uint8_t              campaign_bytes[160]{};
    int32_t              position = 0;
    state                policy;
    OT::state            route;
    OT::campaign_state   campaign;
    OT::world_key        world{7, 1, 1};
    uint32_t             prepares = 0, clears = 0, begins = 0, finishes = 0;
    bool                 loading = false;
    SF::context          context;
    static void          put(uint8_t *p, uint32_t v) {
        for (uint32_t i = 0; i != 4; ++i) p[i] = uint8_t(v >> (8 * i));
    }
    static uint32_t get(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    static uint32_t crc(const uint8_t *p, uint32_t n) {
        uint32_t c = ~0u;
        for (uint32_t i = 0; i != n; ++i) {
            c ^= p[i];
            for (unsigned j = 0; j != 8; ++j) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
        return ~c;
    }
    static int32_t tell(void *v) { return static_cast<queue_save_fixture *>(v)->position; }
    static int     seek(void *v, int32_t n, int whence) {
        auto         &f = *static_cast<queue_save_fixture *>(v);
        const int64_t p = whence == 0 ? n : whence == 1 ? int64_t(f.position) + n
                                            : whence == 2   ? int64_t(f.file.size()) + n
                                                            : -1;
        if (p < 0 || p > 0x7fffffff) return -1;
        f.position = int32_t(p);
        return 0;
    }
    static uint32_t read_file(void *v, void *p, uint32_t n) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        if (uint64_t(f.position) + n > f.file.size()) return 0;
        memcpy(p, f.file.data() + f.position, n);
        f.position += int32_t(n);
        return n;
    }
    static uint32_t write_file(void *v, const void *p, uint32_t n) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        if (uint64_t(f.position) + n > 0x7fffffff) return 0;
        const size_t end = size_t(f.position) + n;
        if (end > f.file.size()) f.file.resize(end);
        memcpy(f.file.data() + f.position, p, n);
        f.position += int32_t(n);
        return n;
    }
    static inline const SF::file_io IO{tell, seek, read_file, write_file};
    static bool                     steps(void *, SF::stream stream, uint32_t i, SF::step &s) {
        // Tiny deliberately synthetic native layout exercises the common walker. The shipping
        // contexts still use native_layout()'s generated sequence, never this fixture sequence.
        if (stream == SF::stream::planet && i < 2) {
            s = {i + 1, 4, SF::step_kind::block};
            return true;
        }
        if (stream == SF::stream::container && i < 2) {
            s = {i ? 20u : 10u, i ? 32u : 16u, SF::step_kind::block};
            return true;
        }
        return false;
    }
    static bool current(void *v, OT::world_key &w) {
        w = static_cast<queue_save_fixture *>(v)->world;
        return true;
    }
    static bool prepare_policy(void *v, uint32_t width, uint32_t height, bool legacy) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        ++f.prepares;
        const dimensions d{1, 3, 3, width, height};
        if (!f.policy.initialize(f.policy_bytes.data(), uint32_t(f.policy_bytes.size()), d)) return false;
        if (legacy) {
            identity id;
            return f.policy.birth_unit(0, 1, id) && f.policy.birth_building(0, 2, id);
        }
        return true;
    }
    static bool prepare_route(void *v, OT::dimensions d, OT::world_key w) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        ++f.prepares;
        return f.route.initialize(f.route_bytes.data(), uint32_t(f.route_bytes.size()), d, w);
    }
    static void clear(void *v) { ++static_cast<queue_save_fixture *>(v)->clears; }
    static bool building_exists(void *, uint32_t player, uint32_t slot) { return player == 0 && slot == 2; }
    static bool begin(void *v) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        if (f.loading) return true;
        ++f.begins;
        f.loading = true;
        return true;
    }
    static bool finish(void *v, bool) {
        auto &f = *static_cast<queue_save_fixture *>(v);
        if (!f.loading || !f.policy.valid() || !f.route.valid()) return false;
        ++f.finishes;
        f.loading = false;
        return true;
    }
    bool actual_planet_load() {
        return SF::preflight_planet(context, IO, this) && SF::begin_load(context) &&
               SF::load_planet(context, IO, this);
    }
    bool actual_container_load() {
        return SF::preflight_container(context, IO, this) == SF::preflight_result::accepted &&
               SF::begin_load(context) && SF::load_container(context, IO, this);
    }
    queue_save_fixture() {
        policy_bytes.resize(state::required_bytes({1, 3, 3, 8, 8}));
        route_bytes.resize(OT::state::required_bytes({1, 3, 8, 8, 3}));
        policy.initialize(policy_bytes.data(), uint32_t(policy_bytes.size()), {1, 3, 3, 8, 8});
        route.initialize(route_bytes.data(), uint32_t(route_bytes.size()), {1, 3, 8, 8, 3}, world);
        campaign.initialize(campaign_bytes, 160);
        uint32_t inc = 0;
        campaign.fresh_world(1, inc);
        context.native.read_step              = steps;
        context.native.packed_limit           = 4096;
        context.native.width_address          = 1;
        context.native.height_address         = 2;
        context.native.planets_address        = 10;
        context.native.planets_bytes          = 16;
        context.native.planet_stride          = 4;
        context.native.planet_system_offset   = 0;
        context.native.session_address        = 20;
        context.native.session_bytes          = 32;
        context.native.current_system_address = 20;
        context.native.current_planet_address = 24;
        context.native.planet_status_address  = 28;
        context.native.member_first           = 1;
        context.native.member_limit           = 4;
        context.native.region_record_bytes = context.native.region_grid_bytes = context.native.region_terrain_bytes = 1;
        context.native.progress_count                                                                               = 1;
        context.native.media_bytes                                                                                  = 4;
        context.capacity                                                                                            = {1, 3, 3, 8, 8};
        context.versions                                                                                            = SF::native_versions();
        context.owner                                                                                               = &policy;
        context.backend                                                                                             = this;
        context.prepare_loaded_owner                                                                                = prepare_policy;
        context.queue_supported                                                                                     = true;
        context.orders                                                                                              = {&route, &campaign, this, current, prepare_route, clear, building_exists, begin, finish};
    }
    void block(const void *p, uint32_t n) {
        mh::lzw::encoder_state encoder{};
        std::vector<uint8_t>   packed(mh::lzw::compressed_bound(n));
        const uint32_t         bytes = mh::lzw::compress(encoder, p, n, packed.data(), uint32_t(packed.size()));
        uint8_t                header[8];
        put(header, bytes);
        put(header + 4, n);
        file.insert(file.end(), header, header + 8);
        file.insert(file.end(), packed.begin(), packed.begin() + bytes);
        position = int32_t(file.size());
    }
    static bool actor(void *, OT::actor_key key, OT::actor_view &view) {
        view = {key, 1, OT::executor::ground_run, {1, 1}, 0, 0, 0, true};
        return true;
    }
    static bool capture(void *, OT::actor_key, OT::active_capture &view) {
        view = {};
        return true;
    }
    static OT::target_status target(void *, OT::actor_key, OT::target_key) { return OT::target_status::alive_hostile; }
    static OT::apply_result  apply(void *, const OT::execution &) { return OT::apply_result::deferred; }
    static bool              cleanup(void *, const OT::ticket &) { return true; }
    bool                     seed_route() {
        identity actor_id{};
        if (!policy.birth_unit(0, 1, actor_id) || !route.observe_command_sequence(0, 17)) return false;
        const OT::backend backend{this, actor, capture, target, apply, cleanup};
        const OT::entry   entry{OT::action::move, {6, 5}, {OT::object_kind::none, {0, 0, 0}}, 9, 17};
        return route.apply_edit(backend, {world, {0, 1, actor_id.incarnation}, OT::edit_kind::append, entry}) == OT::refusal::none;
    }
    void native_planet() {
        file.clear();
        uint8_t value[4];
        put(value, 8);
        block(value, 4);
        block(value, 4);
    }
    void raw_policy_tail() {
        std::vector<uint8_t> encoded(policy.encoded_bytes());
        policy.encode(encoded.data(), uint32_t(encoded.size()));
        file.insert(file.end(), encoded.begin(), encoded.end());
        uint8_t f[8];
        put(f, uint32_t(encoded.size()));
        put(f + 4, SF::POLICY_FOOTER);
        file.insert(file.end(), f, f + 8);
        position = int32_t(file.size());
    }
    void native_container(const std::vector<uint8_t> &one, const std::vector<uint8_t> &two = {}) {
        file.assign(context.versions + 200, context.versions + 240);
        position = 40;
        uint8_t planets[16]{};
        put(planets + 4, 7);
        if (!two.empty()) put(planets + 8, 7);
        block(planets, 16);
        uint8_t session[32]{};
        put(session, 7);
        put(session + 4, 1);
        put(session + 12, 1);
        if (!two.empty()) put(session + 16, 1);
        block(session, 32);
        auto member = [&](const std::vector<uint8_t> &bytes) { uint8_t n[4]; put(n,uint32_t(bytes.size())); file.insert(file.end(),n,n+4); file.insert(file.end(),bytes.begin(),bytes.end()); };
        member(one);
        if (!two.empty()) member(two);
        position = int32_t(file.size());
        uint8_t media[4]{};
        block(media, 4);
    }
};

} // namespace
int run_storagepolicytest() {
    using namespace mh::storage_policy;
    uint32_t checks = 0, failures = 0;
    auto     check = [&](const char *name, bool ok) {
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("  FAIL: %s\n", name);
        }
    };
    const dimensions     d{2, 7, 11, 128, 64};
    std::vector<uint8_t> bytes(state::required_bytes(d));
    state                s;
    check("canonical initialization", s.initialize(bytes.data(), uint32_t(bytes.size()), d) && s.valid());
    identity b{}, u{}, receiver{};
    check("births", s.birth_building(1, 3, b) && s.birth_unit(1, 5, u));
    check("actor must own building", !s.apply_command(0, {b, command_kind::set_auto, 1, 0}));
    check("enable", s.apply_command(1, {b, command_kind::set_auto, 1, 0}));
    check("enable does not sweep", !s.pending(u, receiver));
    check("rally independent", s.apply_command(1, {b, command_kind::set_rally, 127, 63}));
    check("event qualifies", s.qualify(u, b, event_kind::production_inserted));
    trial t;
    s.retry(&t, launch);
    check("unavailable launch retains pending", t.calls == 1 && s.pending(u, receiver));
    t.result = launch_result::accepted;
    s.retry(&t, launch);
    s.retry(&t, launch);
    check("accepted exactly once", t.calls == 2 && !s.pending(u, receiver) && t.x == 127 && t.y == 63);
    check("repair qualifies", s.qualify(u, b, event_kind::repair_reached_full));
    check("Off cancels", s.apply_command(1, {b, command_kind::set_auto, 0, 0}) && !s.pending(u, receiver));
    check("Off preserves rally", s.read_rally(1, 3).enabled);
    s.apply_command(1, {b, command_kind::set_auto, 1, 0});
    s.qualify(u, b, event_kind::production_inserted);
    s.cancel_pending(u);
    check("damage or explicit order cancels", !s.pending(u, receiver));
    s.qualify(u, b, event_kind::production_inserted);
    identity replacement{};
    check("building reuse invalidates", s.birth_building(1, 3, replacement) && replacement.incarnation != b.incarnation &&
                                            !s.pending(u, receiver) && !s.apply_command(1, {b, command_kind::set_auto, 1, 0}));
    s.apply_command(1, {replacement, command_kind::set_auto, 1, 0});
    s.qualify(u, replacement, event_kind::production_inserted);
    identity new_unit{};
    s.birth_unit(1, 5, new_unit);
    check("unit reuse invalidates", !s.pending(new_unit, receiver) && !s.qualify(u, replacement, event_kind::production_inserted));
    s.qualify(new_unit, replacement, event_kind::production_inserted);
    std::vector<uint8_t> encoded(s.encoded_bytes());
    check("codec encode", s.encode(encoded.data(), uint32_t(encoded.size())));
    std::vector<uint8_t> copy(bytes.size());
    state                restored;
    restored.initialize(copy.data(), uint32_t(copy.size()), d);
    check("codec import and incarnations", restored.import(encoded.data(), uint32_t(encoded.size())) &&
                                               std::memcmp(bytes.data(), copy.data(), bytes.size()) == 0 && restored.pending(new_unit, receiver));
    const auto original = copy;
    encoded.back() ^= 0x80;
    check("corrupt import atomic refusal", !restored.import(encoded.data(), uint32_t(encoded.size())) && copy == original);
    encoded.back() ^= 0x80;
    check("truncation refuses", !restored.import(encoded.data(), uint32_t(encoded.size() - 1)));
    check("wrong capacity refuses", !state::validate(encoded.data(), uint32_t(encoded.size()), {2, 8, 11, 128, 64}));
    const dimensions unmapped{2, 7, 11, 0, 0};
    check("unmapped extent same span", state::required_bytes(unmapped) == bytes.size());
    check("single zero map axis refuses", !state::required_bytes({2, 7, 11, 128, 0}));
    s.initialize(bytes.data(), uint32_t(bytes.size()), unmapped);
    check("unmapped is valid and cannot birth", s.valid() && !s.birth_unit(1, 5, u));
    uint8_t empty[HEADER_BYTES];
    state   boot;
    check("boot empty header is honest", boot.initialize(empty, sizeof(empty), {}) && boot.valid());

    command c{{1, 3, 0x88776655u}, command_kind::set_rally, 17, 29}, decoded{};
    int32_t args[13];
    encode_command(c, args);
    check("command all unused words initialized", args[4] == 0 && args[12] == 0 && decode_command(1, 3, args, decoded) &&
                                                      decoded.building.incarnation == c.building.incarnation);
    args[12] = 1;
    check("command rejects hidden payload", !decode_command(1, 3, args, decoded));

    // Composite saves: bundled protocol boundaries, not one micro-test per wire field.
    {
        queue_save_fixture f;
        f.native_planet();
        const auto legacy = f.file;
        f.position        = 3;
        check("legacy empty route preflight preserves position and canonical bytes",
              SF::preflight_planet(f.context, queue_save_fixture::IO, &f) && f.position == 3 && f.prepares == 0 && f.begins == 0 && !f.loading);
        check("legacy planet seeds empty queue at matching campaign allocation",
              f.actual_planet_load() && f.route.world().incarnation == 1 && f.begins == 1 && f.finishes == 1 && !f.loading);
        f.native_planet();
        check("new planet writes composite policy and QORD", SF::write_planet(f.context, queue_save_fixture::IO, &f));
        const auto modern           = f.file;
        auto       unsupported      = f.context;
        unsupported.queue_supported = false;
        check("queue-unsupported planet refuses before preparation", !SF::preflight_planet(unsupported, queue_save_fixture::IO, &f));
        f.native_container(modern);
        check("new required container finishes", SF::finish_container(f.context, queue_save_fixture::IO, &f));
        check("new required marker cannot be mistaken for legacy MSP1", memcmp(f.file.data() + 29, "MSQ2", 4) == 0);
        f.position = 11;
        check("required support refused without publishing", SF::preflight_container(unsupported, queue_save_fixture::IO, &f) == SF::preflight_result::unsupported_queue && f.position == 11);
        uint8_t h[40];
        memcpy(h, f.file.data(), 40);
        const auto before = std::vector<uint8_t>(h, h + 40);
        check("required version normalization cannot drop routes", !SF::normalize_version(unsupported, h) && memcmp(h, before.data(), 40) == 0);
        f.native_container(legacy);
        f.position = 9;
        check("fully legacy container remains loadable", SF::preflight_container(f.context, queue_save_fixture::IO, &f) == SF::preflight_result::accepted && f.position == 9);
        uint32_t inc = 0;
        f.campaign.fresh_world(3, inc);
        check("legacy container reset seeds known worlds ascending, never max old clock", f.actual_container_load() && f.campaign.last_world_inc() == 1 && f.campaign.world_inc(1) == 1 && f.campaign.world_inc(3) == 0 && f.loading);
    }
    {
        queue_save_fixture f;
        identity           old_storage{}, old_unit{};
        check("seed old storage-only policy", f.policy.birth_building(0, 1, old_storage) &&
                                                  f.policy.birth_unit(0, 1, old_unit) &&
                                                  f.policy.apply_command(0, {old_storage, command_kind::set_rally, 6, 7}) &&
                                                  f.policy.apply_command(0, {old_storage, command_kind::set_auto, 1, 0}) &&
                                                  f.policy.qualify(old_unit, old_storage, event_kind::production_inserted));
        f.native_planet();
        f.raw_policy_tail();
        const auto parked_legacy = f.file;
        uint32_t   inc           = 0;
        f.campaign.fresh_world(2, inc);
        check("seed genuine accepted route plus command high-water", f.seed_route());
        const auto saved_route = f.route_bytes;
        f.native_planet();
        check("current route envelope encodes every accepted entry", SF::write_planet(f.context, queue_save_fixture::IO, &f));
        const auto current = f.file;
        f.native_container(current, parked_legacy);
        check("mixed parked legacy/current required saves compose", SF::finish_container(f.context, queue_save_fixture::IO, &f));
        const auto       container = f.file;
        SF::requirements req;
        check("manifest exact member/policy/route masks", SF::preflight_container(f.context, queue_save_fixture::IO, &f, &req) == SF::preflight_result::accepted && req.members == 6 && req.policy == 6 && req.routes == 2);
        f.campaign.fresh_world(3, inc);
        check("seed later outgoing same-world command watermark", f.route.observe_command_sequence(0, 99));
        const auto outgoing_route = f.route_bytes;
        check("container restores clock while preserving quarantined outgoing bytes", f.actual_container_load() && f.campaign.last_world_inc() == 2 && f.campaign.world_inc(3) == 0 && f.loading && f.begins == 1 && f.finishes == 0 && f.route_bytes == outgoing_route && f.route.command_high_water(0) == 99);
        f.file     = current;
        f.position = 7;
        check("same-world member restores exact route after idempotent nested begin", f.actual_planet_load() && f.route_bytes == saved_route && f.route.command_high_water(0) == 17 && !f.loading && f.finishes == 1 && f.begins == 1);
        f.file     = parked_legacy;
        f.position = 7;
        check("missing QORD resets stale metadata even identical world/incarnation/shape",
              f.actual_planet_load() && f.route.world().planet == 1 && f.route.world().incarnation == 1 &&
                  f.route.command_high_water(0) == 0 && !f.loading);
        f.world    = {7, 2, 2};
        f.file     = parked_legacy;
        f.position = 7;
        check("parked legacy owner resets route despite prior same-shape owner, preserves clock", f.actual_planet_load() && f.route.world().incarnation == 2 && f.route.command_high_water(0) == 0 && f.campaign.last_world_inc() == 2 && f.position == 7 && !f.loading);
        identity pending_receiver{};
        check("legacy SPOL1 adds ordinary building life without changing storage policy",
              f.policy.building_identity(0, 2).incarnation != 0 &&
                  f.policy.building_identity(0, 1).incarnation == old_storage.incarnation &&
                  f.policy.read_rally(0, 1).automatic && f.policy.read_rally(0, 1).enabled &&
                  f.policy.read_rally(0, 1).x == 6 && f.policy.read_rally(0, 1).y == 7 &&
                  f.policy.pending(old_unit, pending_receiver) &&
                  pending_receiver.incarnation == old_storage.incarnation);
        f.file           = current;
        f.position       = 5;
        const auto     p = f.policy_bytes, q = f.route_bytes, cam = std::vector<uint8_t>(f.campaign_bytes, f.campaign_bytes + 160);
        const uint32_t prepares = f.prepares;
        check("cross-world planet refused atomically", !f.actual_planet_load() && f.policy_bytes == p && f.route_bytes == q && f.prepares == prepares && memcmp(cam.data(), f.campaign_bytes, 160) == 0);
        f.file     = container;
        f.position = 13;
        // Structural corruption and recomputed outer CRC with wrong campaign identity exercise
        // both integrity and semantic preflight before native/executor callbacks can run.
        const size_t m = f.file.size() - 8 - 204;
        queue_save_fixture::put(f.file.data() + m + 40 + 16 + 4, 2);
        queue_save_fixture::put(f.file.data() + m + 40 + 16 + 8, 1);
        queue_save_fixture::put(f.file.data() + m + 36, queue_save_fixture::crc(f.file.data() + m + 40, 160));
        queue_save_fixture::put(f.file.data() + m + 200, queue_save_fixture::crc(f.file.data() + m, 200));
        check("campaign identity corruption with valid outer CRC refuses without publication", SF::preflight_container(f.context, queue_save_fixture::IO, &f) == SF::preflight_result::invalid && f.position == 13 && f.policy_bytes == p && f.route_bytes == q && memcmp(cam.data(), f.campaign_bytes, 160) == 0);
        f.file = container;
        f.file.pop_back();
        f.position = 13;
        check("truncated composite manifest refuses atomically", !f.actual_container_load() && f.position == 13 && f.prepares == prepares && memcmp(cam.data(), f.campaign_bytes, 160) == 0);
        f.world = {7, 1, 1};
        f.file  = current;
        f.file[f.file.size() - 9] ^= 0x80;
        f.position = 5;
        check("route/composite corruption never partially imports policy", !f.actual_planet_load() && f.position == 5 && f.policy_bytes == p && f.route_bytes == q && f.prepares == prepares);
    }
    std::printf("=== storagepolicytest: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
