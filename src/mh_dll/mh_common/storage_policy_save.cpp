#include "include/mh_storage_policy_save.h"
#include "include/mh_lzw.h"
#include <cstring>
#include <vector>

namespace mh::storage_policy::savefile {
namespace {
constexpr uint32_t MANIFEST_MAGIC  = 0x4d435053u; // SPCM
constexpr uint32_t MOD_FOOTER      = 0x314d484du; // MHM1
constexpr uint32_t MANIFEST_BYTES  = 32;
constexpr uint32_t MANIFEST2_BYTES = 204;         // header40 + QCAM160 + CRC32(first200)
constexpr uint32_t POLICY_MAGIC    = 0x4c4f5053u; // SPOL
namespace OT                       = mh::order_tasks;
uint32_t get(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = ~0u;
    for (uint32_t i = 0; i != n; ++i) {
        c ^= p[i];
        for (unsigned b = 0; b != 8; ++b) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
bool valid_io(const file_io &io, void *f) { return f && io.tell && io.seek && io.read; }
// Explicit restoration after the exception boundary: seek failure must affect the returned verdict.
template <class F>
bool preserve(const file_io &io, void *f, F fn) {
    if (!valid_io(io, f)) return false;
    const int32_t pos = io.tell(f);
    if (pos < 0) return false;
    bool ok = false;
    try {
        ok = fn();
    } catch (...) { ok = false; }
    return io.seek(f, pos, 0) == 0 && ok;
}
bool valid_layout(const layout &n) {
    if (!n.read_step || !n.packed_limit || !n.width_address || !n.height_address ||
        !n.planets_bytes || !n.planet_stride || !n.session_bytes || !n.member_first ||
        n.member_first >= n.member_limit || n.member_limit > 32 ||
        !n.region_record_bytes || !n.region_grid_bytes || !n.region_terrain_bytes ||
        !n.progress_count || !n.media_bytes) return false;
    const uint64_t last = uint64_t(n.member_limit - 1) * n.planet_stride + n.planet_system_offset;
    if (last + 4 > n.planets_bytes) return false;
    for (uint32_t address : {n.current_system_address, n.current_planet_address})
        if (address < n.session_address || uint64_t(address - n.session_address) + 4 > n.session_bytes) return false;
    return n.planet_status_address >= n.session_address &&
           uint64_t(n.planet_status_address - n.session_address) + uint64_t(n.member_limit) * 4 <= n.session_bytes;
}
struct reader {
    const context               &ctx;
    const file_io               &io;
    void                        *f;
    int32_t                      end;
    mutable std::vector<uint8_t> packed, unpacked;
    bool                         at(int32_t off, void *dst, uint32_t n) const {
        return off >= 0 && off <= end && n <= uint32_t(end - off) &&
               io.seek(f, off, 0) == 0 && io.read(f, dst, n) == n;
    }
    bool footer(int32_t end_at, uint32_t magic, uint32_t &n, int32_t &start, bool &found) const {
        found = false;
        if (end_at < 8) return true;
        uint8_t b[8];
        if (!at(end_at - 8, b, 8)) return false;
        if (get(b + 4) != magic) return true;
        found = true;
        n     = get(b);
        if (n > uint32_t(end_at - 8)) return false;
        start = end_at - 8 - int32_t(n);
        return true;
    }
    // Decoder uses the EXISTING generated block sequence. Every payload is bounded-decoded
    // before native mutation; only routing/geometry outputs are retained by the caller.
    bool block(int32_t &pos, uint32_t expected, std::vector<uint8_t> *decoded = nullptr) const {
        uint8_t h[8];
        if (!at(pos, h, 8) || get(h + 4) != expected) return false;
        const uint32_t n = get(h);
        if (!n || n > ctx.native.packed_limit || pos > end - 8 || n > uint32_t(end - pos - 8)) return false;
        packed.resize(size_t(n) + 8);
        memcpy(packed.data(), h, 8);
        if (!at(pos + 8, packed.data() + 8, n)) return false;
        auto &dst = decoded ? *decoded : unpacked;
        dst.resize(size_t(expected) + 8);
        if (n >= 4 && get(packed.data() + 8) == mh::lzw::MAGIC) {
            mh::lzw::decoder_state st{};
            if (!mh::lzw::decompress_bounded(st, packed.data() + 8, n, dst.data(), expected)) return false;
        } else if (!mh::lzss::decompress_block_bounded(packed.data(), packed.size(), dst.data(), dst.size())) return false;
        dst.resize(expected);
        pos += 8 + int32_t(n);
        return true;
    }
};
bool file_end(const file_io &io, void *f, int32_t &end) {
    if (io.seek(f, 0, 2)) return false;
    end = io.tell(f);
    return end >= 0;
}
struct planet_info {
    bool                 policy = false, caps = false;
    std::vector<uint8_t> encoded;
    dimensions           shape{};
    std::vector<uint8_t> routes; // Validated private schema2 RAW bytes, never an encoded/live alias.
    OT::world_key        world{};
};
bool planet(const reader &r, int32_t begin, uint32_t length, dimensions cap, planet_info &out) {
    if (!valid_layout(r.ctx.native)) return false;
    if (begin < 0 || begin > r.end || length > uint32_t(r.end - begin)) return false;
    int32_t  tail = begin + int32_t(length), start = 0;
    uint32_t n     = 0;
    bool     found = false;
    if (!r.footer(tail, OVERFLOW_FOOTER, n, start, found)) return false;
    if (found) {
        if (start < begin || n < 32 || n > r.ctx.overflow_max_bytes || !r.ctx.check_overflow) return false;
        std::vector<uint8_t> blob(n);
        if (!r.at(start, blob.data(), n) || !r.ctx.check_overflow(r.ctx.backend, blob.data(), n)) return false;
        out.caps = true;
        tail     = start;
    }
    if (!r.footer(tail, POLICY_FOOTER, n, start, found)) return false;
    if (found) {
        const uint32_t max_policy = state::required_bytes({cap.players, cap.buildings, cap.units, 256, 256}) + ENVELOPE_BYTES;
        const uint32_t max_route  = OT::state::required_bytes({cap.players, cap.units, 256, 256, cap.buildings}) + 16;
        if (start < begin || n < ENVELOPE_BYTES || uint64_t(n) > uint64_t(COMPOSITE_HEADER_BYTES) + max_policy + max_route) return false;
        std::vector<uint8_t> blob(n);
        if (!r.at(start, blob.data(), n) || get(blob.data()) != POLICY_MAGIC) return false;
        const auto *h = blob.data();
        if (get(h + 4) == SCHEMA) {
            if (n != max_policy) return false;
            out.encoded = std::move(blob);
        } else if (get(h + 4) == COMPOSITE_SCHEMA) {
            if (n < COMPOSITE_HEADER_BYTES || get(h + 8) != n - COMPOSITE_HEADER_BYTES ||
                get(h + 12) != crc32(h + COMPOSITE_HEADER_BYTES, n - COMPOSITE_HEADER_BYTES) ||
                get(h + 16) != (REQUIRE_POLICY | REQUIRE_QUEUE) || get(h + 20) != max_policy ||
                get(h + 24) < OT::ENVELOPE_BYTES || get(h + 24) > max_route || get(h + 28) ||
                uint64_t(COMPOSITE_HEADER_BYTES) + get(h + 20) + get(h + 24) != n) return false;
            out.encoded.assign(h + COMPOSITE_HEADER_BYTES, h + COMPOSITE_HEADER_BYTES + max_policy);
            out.routes.assign(h + COMPOSITE_HEADER_BYTES + max_policy, h + n);
        } else return false;
        out.policy = true;
        tail       = start;
    }
    // Walk and bounded-decode the COMPLETE native framing before trusting an extension or
    // allowing a legacy loader to mutate state. Retain only the required routing scalars.
    reader  native{r.ctx, r.io, r.f, tail};
    int32_t pos = begin;
    cap.width = cap.height = 0;
    std::vector<uint8_t> scalar;
    step                 s;
    for (uint32_t index = 0; r.ctx.native.read_step(r.ctx.native.context, stream::planet, index, s); ++index) {
        if (s.kind == step_kind::block) {
            const bool geom = s.address == r.ctx.native.width_address || s.address == r.ctx.native.height_address;
            if (geom && s.size != 4) return false;
            if (!native.block(pos, s.size, geom ? &scalar : nullptr)) return false;
            if (s.address == r.ctx.native.width_address) cap.width = get(scalar.data());
            if (s.address == r.ctx.native.height_address) cap.height = get(scalar.data());
        } else if (s.kind == step_kind::regions) {
            uint32_t records = 0;
            step     header;
            for (uint32_t index = 0; r.ctx.native.read_step(r.ctx.native.context, stream::regions, index, header); ++index) {
                const bool count = header.address == r.ctx.native.region_count_address;
                if (count && header.size != 4) return false;
                if (!native.block(pos, header.size, count ? &scalar : nullptr)) return false;
                if (count) records = get(scalar.data());
            }
            // Every record needs at least its 8-byte framing header plus one packed byte.
            // Derive the bound from this member's remaining extent, not an unchecked count.
            if (pos > tail || records > uint32_t(tail - pos) / 9u) return false;
            for (uint32_t i = 0; i != records; ++i)
                if (!native.block(pos, r.ctx.native.region_record_bytes)) return false;
            if (!native.block(pos, r.ctx.native.region_grid_bytes) ||
                !native.block(pos, r.ctx.native.region_terrain_bytes)) return false;
        } else if (s.kind == step_kind::progress) {
            // The native size is 3*u16(config), not another saved scalar. Its eight block
            // headers must agree; config compatibility remains the native block reader's gate.
            uint8_t header[8];
            if (!native.at(pos, header, sizeof(header))) return false;
            const uint32_t slice = get(header + 4);
            if (slice > 3u * 65535u || slice % 3u) return false;
            for (uint32_t i = 0; i != r.ctx.native.progress_count; ++i)
                if (!native.block(pos, slice)) return false;
        } else return false;
    }
    if (pos != tail || !cap.width || !cap.height || !state::required_bytes(cap)) return false;
    out.shape = cap;
    if (out.policy && !state::validate(out.encoded.data(), uint32_t(out.encoded.size()), cap)) return false;
    if (!out.routes.empty()) {
        OT::dimensions d{};
        if (!OT::state::inspect(out.routes.data(), uint32_t(out.routes.size()), d, out.world) ||
            d.players != cap.players || d.units != cap.units || d.buildings != cap.buildings ||
            d.width != cap.width || d.height != cap.height) return false;
        // Inspect accepts only the two exact layouts, including their inner/outer schema and
        // complete row grammar. Normalize privately once; live publication never migrates again.
        const uint32_t raw_bytes = OT::state::required_bytes(d);
        if (get(out.routes.data() + 4) == OT::LEGACY_SCHEMA) {
            std::vector<uint8_t> migrated(raw_bytes);
            if (!OT::state::migrate_schema1(migrated.data(), raw_bytes,
                                            out.routes.data() + OT::ENVELOPE_BYTES,
                                            uint32_t(out.routes.size()) - OT::ENVELOPE_BYTES)) return false;
            out.routes.swap(migrated);
        } else {
            // The current layout is already exact; remove only its envelope in this private span.
            std::memmove(out.routes.data(), out.routes.data() + OT::ENVELOPE_BYTES, raw_bytes);
            out.routes.resize(raw_bytes);
        }
        state     identities;
        OT::state route;
        if (!identities.bind(out.encoded.data() + ENVELOPE_BYTES, uint32_t(out.encoded.size()) - ENVELOPE_BYTES) ||
            !route.bind(out.routes.data(), uint32_t(out.routes.size()))) return false;
        auto matches = [](void *opaque, OT::object_kind kind, OT::actor_key key) {
            const auto &s  = *static_cast<state *>(opaque);
            const auto  id = kind == OT::object_kind::unit ? s.unit_identity(key.player, key.slot) : kind == OT::object_kind::building ? s.building_identity(key.player, key.slot)
                                                                                                                                       : identity{};
            return key.incarnation && id.incarnation == key.incarnation;
        };
        if (!route.validate_lifetimes(&identities, matches)) return false;
    }
    return true;
}
struct container_info {
    requirements  req{};
    uint32_t      stamp      = 0;
    int32_t       native_end = 0;
    bool          manifest = false, mod = false;
    uint32_t      system = 0, current = 0, known = 0;
    dimensions    current_shape{};
    uint8_t       campaign[160]{};
    bool          has_campaign = false;
    OT::world_key worlds[32]{};
};
bool container(const reader &r, const uint8_t *versions, dimensions cap, bool writing, container_info &out) {
    if (!versions || !valid_layout(r.ctx.native)) return false;
    uint8_t h[40];
    if (!r.at(0, h, 40)) return false;
    out.req.version = classify_header(h, versions, out.stamp);
    if (out.req.version < 0) return false;
    std::vector<uint8_t> planets, session;
    int32_t              pos = 40;
    step                 s;
    for (uint32_t index = 0; r.ctx.native.read_step(r.ctx.native.context, stream::container, index, s); ++index) {
        if (s.kind != step_kind::block) break;
        if (out.req.version < s.version_min || out.req.version > s.version_max) continue;
        std::vector<uint8_t> *dst = s.address == r.ctx.native.planets_address ? &planets : s.address == r.ctx.native.session_address ? &session
                                                                                                                                     : nullptr;
        if (!r.block(pos, s.size, dst)) return false;
    }
    if (planets.size() != r.ctx.native.planets_bytes || session.size() != r.ctx.native.session_bytes) return false;
    const uint32_t system  = get(session.data() + r.ctx.native.current_system_address - r.ctx.native.session_address);
    const uint32_t current = get(session.data() + r.ctx.native.current_planet_address - r.ctx.native.session_address);
    out.system             = system;
    out.current            = current;
    if (current < r.ctx.native.member_first || current >= r.ctx.native.member_limit) return false;
    for (uint32_t i = r.ctx.native.member_first; i < r.ctx.native.member_limit; ++i) {
        if (get(session.data() + r.ctx.native.planet_status_address - r.ctx.native.session_address + i * 4) || current == i)
            out.known |= uint32_t(1) << i;
        if (get(planets.data() + i * r.ctx.native.planet_stride + r.ctx.native.planet_system_offset) != system ||
            (!get(session.data() + r.ctx.native.planet_status_address - r.ctx.native.session_address + i * 4) && current != uint32_t(i))) continue;
        uint8_t len[4];
        if (!r.at(pos, len, 4)) return false;
        pos += 4;
        const uint32_t n = get(len);
        planet_info    p;
        if (!planet(r, pos, n, cap, p)) return false;
        if (i == current) out.current_shape = p.shape;
        const uint32_t bit = uint32_t(1) << i;
        out.req.members |= bit;
        if (p.policy) out.req.policy |= bit;
        if (p.caps) out.req.caps |= bit;
        if (!p.routes.empty()) {
            if (p.world.system != system || p.world.planet != i) return false;
            out.req.routes |= bit;
            out.worlds[i] = p.world;
        }
        pos += int32_t(n);
    }
    const uint32_t current_bit = uint32_t(1) << current;
    if (!(out.req.members & current_bit) ||
        ((writing || (out.stamp & REQUIRE_POLICY)) && !(out.req.policy & current_bit))) return false;
    if (!r.block(pos, r.ctx.native.media_bytes)) return false;
    out.native_end = pos;
    int32_t  tail = r.end, start = 0;
    uint32_t n     = 0;
    bool     found = false;
    if (!r.footer(tail, MOD_FOOTER, n, start, found)) return false;
    if (found) {
        if (n < 36 || n > 6308 || start < pos) return false;
        out.mod = true;
        tail    = start;
    }
    if (!r.footer(tail, CONTAINER_FOOTER, n, start, found)) return false;
    if (found) {
        if (start != pos || (n != MANIFEST_BYTES && n != MANIFEST2_BYTES)) return false;
        uint8_t m[MANIFEST2_BYTES]{};
        if (!r.at(start, m, n) || get(m) != MANIFEST_MAGIC ||
            get(m + 8) != out.req.members || get(m + 12) != out.req.policy ||
            get(m + 16) != out.req.caps || get(m + 20) != uint32_t(out.req.version)) return false;
        if (n == MANIFEST_BYTES) {
            if (get(m + 4) != SCHEMA || get(m + 24) || get(m + 28) != crc32(m, 28) || out.req.routes) return false;
        } else {
            const uint32_t actual = REQUIRE_POLICY | REQUIRE_QUEUE | (out.req.caps ? REQUIRE_CAPS : 0);
            if (get(m + 4) != COMPOSITE_SCHEMA || get(m + 24) != out.req.routes ||
                get(m + 28) != 160 || get(m + 32) != actual ||
                get(m + 36) != crc32(m + 40, 160) || get(m + 200) != crc32(m, 200) ||
                !OT::campaign_state::validate(m + 40, 160)) return false;
            memcpy(out.campaign, m + 40, 160);
            out.has_campaign = true;
        }
        out.manifest = true;
        tail         = start;
    }
    if (tail != pos) return false;
    // Every saved member belongs to the persisted allocation table, including parked members.
    // A legacy member may lack QORD; its empty owner is seeded from this same table on visit.
    if (out.has_campaign) {
        OT::campaign_state campaign;
        if (!campaign.bind(out.campaign, 160)) return false;
        for (uint32_t i = r.ctx.native.member_first; i != r.ctx.native.member_limit; ++i) {
            const uint32_t bit = uint32_t(1) << i;
            if ((out.known & bit) && !campaign.world_inc(i)) return false;
            if ((out.req.routes & bit) && out.worlds[i].incarnation != campaign.world_inc(i)) return false;
        }
    }
    if (writing) return !out.manifest && !out.mod;
    const uint32_t actual = (out.req.policy ? REQUIRE_POLICY : 0) | (out.req.caps ? REQUIRE_CAPS : 0) |
                            (out.req.routes ? REQUIRE_QUEUE : 0);
    if (actual != out.stamp) return false;
    // Existing MHC1 saves predate this manifest. Policy stamps always require exact member masks.
    if ((out.stamp & REQUIRE_QUEUE) && (!out.has_campaign || !(out.req.routes & current_bit))) return false;
    if (!(out.stamp & REQUIRE_QUEUE) && out.has_campaign) return false;
    return !(out.stamp & REQUIRE_POLICY) || out.manifest;
}
bool order_ready(const context &ctx) {
    const auto &q = ctx.orders;
    return ctx.queue_supported && q.owner && q.campaign && q.current_world && q.prepare_loaded && q.clear_pending &&
           q.building_exists && q.begin_load && q.finish_load && q.campaign->valid();
}
// Both container QCAM publication and the later member load must establish these same
// pre-reserved destination prerequisites before their first live import/preparation.
bool load_destinations_ready(const context &ctx, dimensions shape, bool has_routes, OT::world_key world) {
    if (!ctx.owner || !ctx.prepare_loaded_owner) return false;
    const auto have_policy = ctx.owner->shape();
    if (!ctx.owner->data() || ctx.owner->size() != state::required_bytes(shape) ||
        have_policy.players != shape.players || have_policy.buildings != shape.buildings ||
        have_policy.units != shape.units) return false;
    if (!ctx.queue_supported) return !has_routes;
    if (!order_ready(ctx)) return false;
    const OT::dimensions d{shape.players, shape.units, shape.width, shape.height, shape.buildings};
    auto                &q    = *ctx.orders.owner;
    const auto           have = q.shape();
    if (!q.data() || q.size() != OT::state::required_bytes(d) ||
        have.players != d.players || have.units != d.units || have.buildings != d.buildings) return false;
    const auto previous = q.world();
    // Preserve import's destination checkpoint precondition when prepare_loaded keeps the
    // existing owner. A different map/world is initialized by that callback instead.
    return !has_routes || have.width != d.width || have.height != d.height ||
           previous.system != world.system || previous.planet != world.planet ||
           previous.incarnation != world.incarnation || q.checkpoint_ready();
}
bool planet_world(const context &ctx, const planet_info &p, OT::world_key &world) {
    if (!ctx.queue_supported) return p.routes.empty();
    if (!order_ready(ctx) || !ctx.orders.current_world(ctx.orders.backend, world) ||
        !world.incarnation || ctx.orders.campaign->world_inc(world.planet) != world.incarnation) return false;
    return p.routes.empty() || (p.world.system == world.system && p.world.planet == world.planet &&
                                p.world.incarnation == world.incarnation);
}
} // namespace

bool inspect_planet(const context &ctx, const file_io &io, void *f, int32_t offset, uint32_t length,
                    bool &has_policy, bool &has_caps) {
    has_policy = has_caps = false;
    planet_info p;
    const bool  ok = preserve(io, f, [&] {
        int32_t end = 0;
        return file_end(io, f, end) && planet(reader{ctx, io, f, end}, offset, length, ctx.capacity, p);
    });
    if (ok) {
        has_policy = p.policy;
        has_caps   = p.caps;
    }
    return ok;
}
bool preflight_planet(const context &ctx, const file_io &io, void *f) {
    return preserve(io, f, [&] {
        int32_t       end = 0;
        planet_info   p;
        OT::world_key world{};
        return file_end(io, f, end) && planet(reader{ctx, io, f, end}, 0, uint32_t(end), ctx.capacity, p) &&
               (!p.caps || ctx.caps_supported) && planet_world(ctx, p, world);
    });
}
bool begin_load(const context &ctx) {
    if (!ctx.queue_supported) return true;
    if (!order_ready(ctx)) return false;
    try {
        return ctx.orders.begin_load(ctx.orders.backend);
    } catch (...) { return false; }
}
bool load_planet(const context &ctx, const file_io &io, void *f) {
    planet_info p;
    if (!preserve(io, f, [&] {
            int32_t end = 0;
            return file_end(io, f, end) && planet(reader{ctx, io, f, end}, 0, uint32_t(end), ctx.capacity, p);
        })) return false;
    OT::world_key world{};
    if ((p.caps && !ctx.caps_supported) || !planet_world(ctx, p, world)) return false;
    if (!load_destinations_ready(ctx, p.shape, !p.routes.empty(), world)) return false;
    uint8_t *route_destination = ctx.queue_supported ? ctx.orders.owner->data() : nullptr;
    if (ctx.queue_supported && p.policy && p.routes.empty()) {
        // Old SPOL1 persisted only storage-eligible building lives. Extend its temporary
        // identity region without resetting lives, rally, Auto or pending unit policy.
        // Exhaustion refuses before either live canonical owner can change.
        if (!ctx.orders.building_exists) return false;
        state legacy;
        if (!legacy.bind(p.encoded.data() + ENVELOPE_BYTES,
                         uint32_t(p.encoded.size()) - ENVELOPE_BYTES)) return false;
        for (uint32_t player = 0; player != p.shape.players; ++player) {
            for (uint32_t slot = 1; slot != p.shape.buildings; ++slot) {
                if (!ctx.orders.building_exists(ctx.orders.backend, player, slot) ||
                    legacy.building_identity(player, slot).incarnation) continue;
                identity born{};
                if (!legacy.birth_building(player, slot, born)) return false;
            }
        }
        if (!legacy.encode(p.encoded.data(), uint32_t(p.encoded.size()))) return false;
    }
    const OT::dimensions d{p.shape.players, p.shape.units, p.shape.width, p.shape.height, p.shape.buildings};
    // All new format/migration/lifetime/world/destination checks have completed privately.
    // Preparation and final backend rebind retain their existing failure/quarantine semantics;
    // this does not claim rollback for callbacks or the native loader's earlier bulk writes.
    if (ctx.queue_supported && !ctx.orders.prepare_loaded(ctx.orders.backend, d, world)) return false;
    if (!ctx.prepare_loaded_owner(ctx.backend, p.shape.width, p.shape.height, !p.policy)) return false;
    if (p.policy && !ctx.owner->import(p.encoded.data(), uint32_t(p.encoded.size()))) return false;
    if (ctx.queue_supported) {
        auto &q = *ctx.orders.owner;
        if (!p.routes.empty()) {
            // One infallible publication from the already bound/validated current raw stage.
            // No legacy envelope is re-imported, so schema1 expansion runs exactly once.
            std::memcpy(route_destination, p.routes.data(), p.routes.size());
        } else if (!q.initialize(q.data(), q.size(), d, world)) return false;
        ctx.orders.clear_pending(ctx.orders.backend);
    }
    if (!ctx.owner->valid()) return false;
    if (!ctx.queue_supported) return true;
    try {
        return ctx.orders.finish_load(ctx.orders.backend, p.routes.empty());
    } catch (...) { return false; }
}

bool write_planet(const context &ctx, const file_io &io, void *f) {
    if (!valid_io(io, f) || !io.write) return false;
    try {
        if (!ctx.owner || (ctx.caps_supported && !ctx.write_overflow) || (ctx.queue_supported && !order_ready(ctx))) return false;
        const state &owner = *ctx.owner;
        if (!owner.valid() || !owner.shape().width || !owner.shape().height ||
            !state::required_bytes(owner.shape())) return false;
        const int32_t pos = io.tell(f);
        int32_t       end = 0;
        if (pos < 0 || !file_end(io, f, end) || pos != end) return false;
        reader      r{ctx, io, f, end};
        planet_info native;
        if (!planet(r, 0, uint32_t(end), ctx.capacity, native) || native.policy || native.caps || !native.routes.empty() ||
            native.shape.width != owner.shape().width || native.shape.height != owner.shape().height ||
            native.shape.players != owner.shape().players || native.shape.buildings != owner.shape().buildings ||
            native.shape.units != owner.shape().units || io.seek(f, end, 0)) return false;
        const uint32_t policy_bytes = owner.encoded_bytes();
        if (policy_bytes < ENVELOPE_BYTES + HEADER_BYTES) return false;
        uint32_t route_bytes = 0;
        if (ctx.queue_supported) {
            OT::world_key world{};
            const auto   &q = *ctx.orders.owner;
            const auto    d = q.shape();
            if (!planet_world(ctx, native, world) || !q.valid() || q.world().system != world.system ||
                q.world().planet != world.planet || q.world().incarnation != world.incarnation ||
                d.players != owner.shape().players || d.units != owner.shape().units ||
                d.buildings != owner.shape().buildings || d.width != owner.shape().width ||
                d.height != owner.shape().height) return false;
            auto matches = [](void *opaque, OT::object_kind kind, OT::actor_key key) {
                const auto &s  = *static_cast<const state *>(opaque);
                const auto  id = kind == OT::object_kind::unit ? s.unit_identity(key.player, key.slot) : kind == OT::object_kind::building ? s.building_identity(key.player, key.slot)
                                                                                                                                           : identity{};
                return key.incarnation && id.incarnation == key.incarnation;
            };
            if (!q.validate_lifetimes(const_cast<state *>(&owner), matches)) return false;
            route_bytes = q.encoded_bytes();
            if (route_bytes < 16) return false;
        }
        const uint32_t head  = route_bytes ? COMPOSITE_HEADER_BYTES : 0;
        const uint64_t total = uint64_t(head) + policy_bytes + route_bytes;
        if (total > 0x7fffffffu) return false;
        const uint32_t       bytes = uint32_t(total);
        std::vector<uint8_t> encoded(bytes);
        if (!owner.encode(encoded.data() + head, policy_bytes)) return false;
        if (route_bytes) {
            if (!ctx.orders.owner->encode(encoded.data() + head + policy_bytes, route_bytes) ||
                !OT::state::validate(encoded.data() + head + policy_bytes, route_bytes,
                                     ctx.orders.owner->shape(), ctx.orders.owner->world())) return false;
            put(encoded.data(), POLICY_MAGIC);
            put(encoded.data() + 4, COMPOSITE_SCHEMA);
            put(encoded.data() + 8, policy_bytes + route_bytes);
            put(encoded.data() + 12, crc32(encoded.data() + head, policy_bytes + route_bytes));
            put(encoded.data() + 16, REQUIRE_POLICY | REQUIRE_QUEUE);
            put(encoded.data() + 20, policy_bytes);
            put(encoded.data() + 24, route_bytes);
        }
        uint8_t footer[8];
        put(footer, bytes);
        put(footer + 4, POLICY_FOOTER);
        if (io.write(f, encoded.data(), bytes) != bytes || io.write(f, footer, 8) != 8) return false;
        // This remains the sole MHOV writer. It must run AFTER SPFT so its EOF locator survives.
        return !ctx.caps_supported || ctx.write_overflow(ctx.backend, io, f) >= 0;
    } catch (...) { return false; }
}
preflight_result preflight_container(const context &ctx, const file_io &io, void *f, requirements *out) {
    if (out) *out = {};
    container_info c;
    const bool     ok = preserve(io, f, [&] {
        int32_t end = 0;
        return file_end(io, f, end) && container(reader{ctx, io, f, end}, ctx.versions, ctx.capacity, false, c);
    });
    if (!ok) return preflight_result::invalid;
    if (out) *out = c.req;
    if (c.req.caps && !ctx.caps_supported) return preflight_result::unsupported_caps;
    if (c.req.routes && !ctx.queue_supported) return preflight_result::unsupported_queue;
    return preflight_result::accepted;
}
bool normalize_version(const context &ctx, uint8_t (&h)[40]) {
    if (!ctx.versions) return false;
    uint32_t  required = 0;
    const int v        = classify_header(h, ctx.versions, required);
    if (v < 0 || ((required & REQUIRE_CAPS) && !ctx.caps_supported) ||
        ((required & REQUIRE_QUEUE) && !ctx.queue_supported)) return false;
    memcpy(h, ctx.versions + v * 40, 40);
    return true;
}
bool load_container(const context &ctx, const file_io &io, void *f) {
    container_info c;
    if (!preserve(io, f, [&] {
            int32_t end = 0;
            return file_end(io, f, end) && container(reader{ctx, io, f, end}, ctx.versions, ctx.capacity, false, c);
        }) ||
        (c.req.caps && !ctx.caps_supported) || (c.req.routes && !ctx.queue_supported)) return false;
    if (!ctx.queue_supported) return true;
    if (!order_ready(ctx)) return false;
    uint8_t            bytes[160]{};
    OT::campaign_state incoming;
    if (c.has_campaign) {
        if (!incoming.bind(c.campaign, 160)) return false;
    } else {
        uint32_t current_inc = 0;
        if (!incoming.initialize(bytes, 160) || !incoming.seed_legacy(c.known, c.current, current_inc)) return false;
    }
    // container() has already normalized and cross-validated every archived member, not just
    // the active one. Refuse missing/mismatched owner reservations before publishing QCAM.
    const OT::world_key world{c.system, c.current, incoming.world_inc(c.current)};
    if (!load_destinations_ready(ctx, c.current_shape,
                                 (c.req.routes & (uint32_t(1) << c.current)) != 0, world)) return false;
    // Exact incoming clock. Never retain a larger process-local value from the old campaign.
    // Finish the backend operation first: no fallible callback may run after publication.
    try {
        ctx.orders.clear_pending(ctx.orders.backend);
    } catch (...) { return false; }
    return ctx.orders.campaign->import(incoming.data(), 160);
}
bool finish_container(const context &ctx, const file_io &io, void *f) {
    if (!valid_io(io, f) || !io.write) return false;
    const int32_t pos = io.tell(f);
    if (pos < 0) return false;
    bool ok = false;
    try {
        ok = [&] {
            int32_t        end = 0;
            container_info c;
            if (file_end(io, f, end) && pos == end && container(reader{ctx, io, f, end}, ctx.versions, ctx.capacity, true, c) &&
                c.req.policy && io.seek(f, end, 0) == 0) {
                uint8_t    m[MANIFEST2_BYTES]{};
                const bool queued = c.req.routes != 0;
                uint32_t   bytes  = MANIFEST_BYTES;
                put(m, MANIFEST_MAGIC);
                put(m + 4, queued ? COMPOSITE_SCHEMA : SCHEMA);
                put(m + 8, c.req.members);
                put(m + 12, c.req.policy);
                put(m + 16, c.req.caps);
                put(m + 20, uint32_t(c.req.version));
                if (queued) {
                    if (!order_ready(ctx) || !(c.req.routes & (uint32_t(1) << c.current))) return false;
                    const auto &campaign = *ctx.orders.campaign;
                    for (uint32_t i = ctx.native.member_first; i != ctx.native.member_limit; ++i) {
                        const uint32_t bit = uint32_t(1) << i;
                        if ((c.known & bit) && !campaign.world_inc(i)) return false;
                        if ((c.req.routes & bit) && c.worlds[i].incarnation != campaign.world_inc(i)) return false;
                    }
                    bytes = MANIFEST2_BYTES;
                    put(m + 24, c.req.routes);
                    put(m + 28, 160);
                    put(m + 32, REQUIRE_POLICY | REQUIRE_QUEUE | (c.req.caps ? REQUIRE_CAPS : 0));
                    memcpy(m + 40, campaign.data(), 160);
                    put(m + 36, crc32(m + 40, 160));
                    put(m + 200, crc32(m, 200));
                } else {
                    if (ctx.queue_supported) return false; // queue-capable writer must persist even an empty owner/high-water
                    put(m + 28, crc32(m, 28));
                }
                uint8_t footer[8];
                put(footer, bytes);
                put(footer + 4, CONTAINER_FOOTER);
                uint8_t h[40];
                memcpy(h, ctx.versions + c.req.version * 40, 40);
                memcpy(h + 29, queued ? (c.req.caps ? "MBQ2" : "MSQ2") : (c.req.caps ? "MSB1" : "MSP1"), 4);
                ok = io.write(f, m, bytes) == bytes && io.write(f, footer, 8) == 8 &&
                     io.seek(f, 0, 0) == 0 && io.write(f, h, 40) == 40;
            }
            return ok;
        }();
    } catch (...) { ok = false; }
    // Caller's next operation appends MHM1. Never leave the cursor after the overwritten header.
    return io.seek(f, 0, 2) == 0 && ok;
}

} // namespace mh::storage_policy::savefile
