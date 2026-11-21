// state/host_events.cpp -- see host_events.h and libmh/include/libmh_host_events.h for the
// contract (order, overflow, threading). The ring is deliberately dumb: fixed 256 records,
// head/tail indexes, drop-newest on overflow with a monotonic counter -- every record on this
// surface is notify-class (R8), so overflow degrades presentation, never the sim, and the
// counter keeps the degradation observable instead of silent (the D24 lesson: the one silent
// loss path in a design is the one that bites).
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include "orders/order_queue.h"
#include "state/storage_policy_runtime.h"

#include "state/region_runtime.h" // map_cam_col/_row -- the only place allowed to bind an address
#include "state/host_events.h"

namespace {

constexpr uint32_t RING_CAP = 256;
static_assert(sizeof(libmh_order_observation) == 88, "order observation ABI size");
static_assert(offsetof(libmh_order_observation, unit_index) == 4, "order observation headers");
static_assert(offsetof(libmh_order_observation, args) == 12, "order observation arguments");

libmh_event_sink             g_sink = nullptr;
libmh_event                  g_ring[RING_CAP];
uint32_t                     g_head       = 0; // next slot to drain
uint32_t                     g_count      = 0;
uint32_t                     g_overflow   = 0;
uint32_t                     g_dispatched = 0;
libmh_order_observation_sink g_order_sink = nullptr;
libmh_order_observation      g_order_ring[RING_CAP];
uint32_t                     g_order_head = 0, g_order_count = 0;

// The SCREEN channel's answer slots (R9). Indexed by LIBMH_SCR_ANS_*; -1 means nothing pending.
// One slot today, sized so adding an answer id does not reshape anything.
constexpr uint32_t ANSWER_SLOTS            = 8;
int32_t            g_answers[ANSWER_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};

// The text surface's ring (see libmh_host_events.h): full texts, copied at emit in queue mode.
libmh_text_sink g_text_sink = nullptr;
uint16_t        g_text_kinds[LIBMH_TEXT_RING_CAP];
uint16_t        g_text_ring[LIBMH_TEXT_RING_CAP][LIBMH_EVT_TEXT_MAX];
uint32_t        g_text_head     = 0;
uint32_t        g_text_count    = 0;
uint32_t        g_text_overflow = 0;
void            deliver_order_value(
               void *, const libmh_order_observation *value) {
    if (g_order_sink) {
        ++g_dispatched;
        g_order_sink(value);
        return;
    }

    if (g_order_count == RING_CAP) {
        ++g_overflow;
        return;
    }

    g_order_ring[(g_order_head + g_order_count) % RING_CAP] = *value;
    ++g_order_count;
}

uint32_t standalone_actor_life(
    void *, uint32_t player, uint32_t slot) {
    return mh::storage_policy::runtime::bound_state()
        .unit_identity(player, slot)
        .incarnation;
}

mh::order_witness::store &standalone_order_owner() {
    namespace W = mh::order_witness;

    constexpr uint32_t capacities[3] = {
        static_cast<uint32_t>(mh::orders::QUEUE_CAP),
        static_cast<uint32_t>(mh::orders::PENDING_CAP),
        static_cast<uint32_t>(mh::orders::STAGING_CAP)};

    constexpr size_t bytes = W::fixed_slab_bytes(
        capacities[0], capacities[1], capacities[2]);

    alignas(8) static uint8_t slab[bytes];
    static W::store          *owner = nullptr;

    if (!owner) {
        owner = W::initialize(
            slab, sizeof slab, capacities,
            standalone_actor_life, nullptr,
            deliver_order_value, nullptr);

        if (!owner)
            std::abort();
    }

    return *owner;
}

mh::order_witness::store *g_order_owner = nullptr;

} // namespace
namespace mh::state {

mh::order_witness::store &order_witnesses() {
    if (!g_order_owner)
        g_order_owner = &standalone_order_owner();
    return *g_order_owner;
}

} // namespace mh::state

namespace mh::state {
void emit_primary_terminal(
    uint32_t player, uint32_t slot, uint16_t state) {
    if (state == 1 || state == 0x13)
        mh::order_witness::primary_terminal(
            order_witnesses(), player, slot);
}

} // namespace mh::state

namespace mh::state {

void reset_order_witnesses_if_unchanged(uint32_t generation_before_publication) {
    auto &owner = order_witnesses();

    if (mh::order_witness::generation(owner) ==
        generation_before_publication) {
        mh::order_witness::reset_world(owner);
    }
}

} // namespace mh::state

extern "C" int libmh_set_event_sink(libmh_event_sink sink_or_null, uint32_t events_version) {
    if (events_version != LIBMH_HOST_EVENTS_VERSION) return -1;
    g_sink = sink_or_null;
    return 0;
}

extern "C" uint32_t libmh_poll_events(libmh_event *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max && g_count > 0) {
        out[n++] = g_ring[g_head];
        g_head   = (g_head + 1) % RING_CAP;
        --g_count;
    }
    return n;
}

extern "C" uint32_t libmh_event_overflow_count(void) { return g_overflow; }

extern "C" int libmh_set_order_observation_sink(
    libmh_order_observation_sink sink_or_null,
    uint32_t                     events_version,
    libmh_order_witness_store   *shared_store_or_null) {
    namespace W = mh::order_witness;

    if (events_version != LIBMH_HOST_EVENTS_VERSION)
        return -1;

    W::store *candidate = shared_store_or_null
                              ? shared_store_or_null
                              : &standalone_order_owner();

    if (!W::valid(candidate) || !W::quiescent(*candidate))
        return -2;

    if (g_order_owner) {
        if (!W::quiescent(*g_order_owner))
            return -2;

        if (candidate != g_order_owner &&
            (!W::unused(*g_order_owner) || g_order_count != 0))
            return -3;
    } else if (g_order_count != 0) {
        return -3;
    }

    if (!W::set_delivery(
            *candidate, deliver_order_value, nullptr))
        return -2;

    g_order_owner = candidate;
    g_order_sink  = sink_or_null;
    return 0;
}

extern "C" uint32_t libmh_poll_order_observations(libmh_order_observation *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max && g_order_count > 0) {
        out[n++]     = g_order_ring[g_order_head];
        g_order_head = (g_order_head + 1) % RING_CAP;
        --g_order_count;
    }
    return n;
}

extern "C" int libmh_set_text_sink(libmh_text_sink sink_or_null, uint32_t events_version) {
    if (events_version != LIBMH_HOST_EVENTS_VERSION) return -1;
    g_text_sink = sink_or_null;
    return 0;
}

extern "C" uint32_t libmh_poll_texts(uint16_t *kinds_out, uint16_t (*texts_out)[LIBMH_EVT_TEXT_MAX],
                                     uint32_t  max) {
    uint32_t n = 0;
    while (n < max && g_text_count > 0) {
        kinds_out[n] = g_text_kinds[g_text_head];
        std::memcpy(texts_out[n], g_text_ring[g_text_head], sizeof(g_text_ring[0]));
        g_text_head = (g_text_head + 1) % LIBMH_TEXT_RING_CAP;
        --g_text_count;
        ++n;
    }
    return n;
}

extern "C" uint32_t libmh_text_overflow_count(void) { return g_text_overflow; }

extern "C" void libmh_submit_screen_answer(uint32_t answer_id, int32_t value) {
    if (answer_id < ANSWER_SLOTS) g_answers[answer_id] = value;
}

namespace mh::state {

void emit_event(uint16_t channel, uint16_t kind, int32_t a, int32_t b, int32_t c, int32_t d) {
    const libmh_event e = {channel, kind, a, b, c, d};
    if (g_sink != nullptr) {
        ++g_dispatched;
        g_sink(&e);
        return;
    }
    if (g_count == RING_CAP) {
        ++g_overflow; // drop-NEWEST: the backlog's older records keep their order and survive
        return;
    }
    g_ring[(g_head + g_count) % RING_CAP] = e;
    ++g_count;
}

void emit_order_observation(
    uint16_t kind, uint16_t flags, uint16_t unit_index,
    uint16_t owner_and_kind, int16_t param0,
    uint16_t order_code, const int32_t *args) {
    if (kind == LIBMH_EVK_ATTACK_TARGET_ASSIGNED) {
        const unsigned owner_kind  = owner_and_kind & 0xf0;
        const auto     attack_code = [](uint16_t code) {
            return code >= 0x1a && code <= 0x1e;
        };
        if ((owner_kind != 0x20 && owner_kind != 0x80 &&
             owner_kind != 0xa0) ||
            (!attack_code(static_cast<uint16_t>(param0)) &&
             !attack_code(order_code)))
            return;
    }

    libmh_order_observation value{};
    value.kind           = kind;
    value.flags          = flags;
    value.unit_index     = unit_index;
    value.owner_and_kind = owner_and_kind;
    value.param0         = param0;
    value.order_code     = order_code;
    std::memcpy(value.args, args, sizeof value.args);
    value.world_generation =
        mh::order_witness::generation(order_witnesses());
    deliver_order_value(nullptr, &value);
}

uint32_t event_sink_dispatch_count() { return g_dispatched; }

int32_t screen_answer(uint32_t answer_id) {
    return answer_id < ANSWER_SLOTS ? g_answers[answer_id] : -1;
}

void screen_answer_clear(uint32_t answer_id) {
    if (answer_id < ANSWER_SLOTS) g_answers[answer_id] = -1;
}

void emit_text(uint16_t kind, const void *text) {
    if (g_text_sink != nullptr) {
        ++g_dispatched; // one dispatch counter across all notification surfaces
        g_text_sink(kind, static_cast<const uint16_t *>(text));
        return;
    }
    if (g_text_count == LIBMH_TEXT_RING_CAP) {
        ++g_text_overflow; // drop-NEWEST, same posture as the record ring
        return;
    }
    const uint32_t  slot = (g_text_head + g_text_count) % LIBMH_TEXT_RING_CAP;
    const uint16_t *src  = static_cast<const uint16_t *>(text);
    uint32_t        n    = 0;
    for (; n < LIBMH_EVT_TEXT_MAX - 1 && src[n] != 0; ++n) g_text_ring[slot][n] = src[n];
    g_text_ring[slot][n] = 0; // queue copy truncates at capacity; a sync sink gets the whole string
    g_text_kinds[slot]   = kind;
    ++g_text_count;
}

namespace evt {

// The one non-inline adapter: two doubles bit-packed into the record's four int32s (a,b =
// zoom_x lo,hi dwords; c,d = zoom_y). memcpy, not casts -- the payload is the bit pattern.
// The R3b hoist -- see the banner over the declarations in host_events.h for why these two write
// before they emit, and for the measurement that makes the resulting double write idempotent.
void cam_set_col(int32_t col) {
    map_cam_col() = col;
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_CAM_SET_COL, col);
}

void cam_set_row(int32_t row) {
    map_cam_row() = row;
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_CAM_SET_ROW, row);
}

void view_zoom_scale(double zoom_x, double zoom_y) {
    int32_t x[2], y[2];
    static_assert(sizeof(x) == sizeof(zoom_x), "double packs into two int32s");
    std::memcpy(x, &zoom_x, sizeof(x));
    std::memcpy(y, &zoom_y, sizeof(y));
    emit_event(LIBMH_EVC_EVENT, LIBMH_EVK_VIEW_ZOOM_SCALE, x[0], x[1], y[0], y[1]);
}

} // namespace evt

} // namespace mh::state
