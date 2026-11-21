#include "ui/target_line_intents.h"
#include "ui/target_line_routes.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <cstdlib>
#include <limits>
#include "addr/mh_calls.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "caps/orders_layout.gen.h"
#include "gfx/wrap_window.h"
#include "seams/storage_policy.h"
#include "state/region_runtime.h"
#include "ui/target_line_attack_provenance.h"
#include "../../libmh/include/libmh_host_events.h"
#include "../../mh_common/include/mh_order_tasks_host.h"
#include "../../mh_common/include/mh_order_witness.h"

namespace mh::ui::selected_targets::detail {
namespace {
namespace sv = mh::gfx::stratview;
using namespace mh::game;
using namespace mh::state;
using sv::point;
namespace W = mh::order_witness;

std::unique_ptr<uint8_t[]> order_witness_memory;
W::store                  *order_witness_owner = nullptr;
libmh_order_observation    metadata_before{};
uint64_t                   metadata_amendment_admission = 0;

void clear_intent_rows();

uint32_t witnessed_actor_life(
    void *, uint32_t player, uint32_t slot) {
    return mh::storage_policy::hosted::unit_identity(
               player, slot)
        .incarnation;
}

void deliver_native_order(
    void *, const libmh_order_observation *observation) {
    if (observation->kind == LIBMH_EVK_ORDER_ADMITTED)
        admit_attack_order(*observation);
    observe_order_lifecycle(*observation);
}
using sv::world_view;
using unit                         = mh_map_object_unit;
using order                        = mh_llm_strat_order;
constexpr unsigned history_players = 8;
constexpr unsigned queue_capacity  = mh::caps::orders_layout::ARRAYS[mh::caps::orders_layout::QUEUE].count;
enum class command : uint8_t { unknown,
                               move,
                               patrol,
                               attack,
                               enter,
                               exit,
                               deploy };
enum class move_phase : uint8_t {
    none,
    admitted,
    applied,
    suppressed
};

struct move_preview {
    uint64_t   admission_id     = 0;
    uint64_t   command_id       = 0;
    uint32_t   world_generation = 0;
    point      goal{};
    move_phase phase            = move_phase::none;
    bool       ordinary_primary = false;
};
struct history {
    uint32_t     incarnation = 0;
    uint64_t     token       = 0;
    command      verb        = command::unknown;
    point        goal{}, home{};
    uint64_t     secondary_token = 0;
    uint32_t     primary_entity = 0, primary_target_incarnation = 0;
    uint32_t     secondary_entity = 0, secondary_target_incarnation = 0;
    uint16_t     secondary_ref = 0, secondary_id = 0;
    uint32_t     fired_target_incarnation = 0;
    double       fired_at = 0, fired_until = 0;
    uint64_t     latest_primary_admission = 0;
    move_preview preview{};
    // One synchronous admission-to-metadata rollback slot, never a task queue.
    move_preview previous_preview{};
};
std::unique_ptr<history[]>  histories;
unsigned                    history_rows = 0;
uint64_t                    next_token   = 0;
std::unique_ptr<uint32_t[]> building_lives;
unsigned                    building_rows      = 0;
uint32_t                    next_building_life = 0;
bool                        dispatch_open      = false;
unsigned                    pending_count      = 0;
uint32_t                    world_base         = 0;
int                         world_width = 0, world_height = 0;
double                      last_clock = 0;
struct unit_command_state {
    int32_t values[17]{};
};
struct pending {
    order              record{};
    unit_command_state before{};
    uint32_t           incarnation  = 0;
    uint64_t           token        = 0;
    unsigned           queue_index  = 0;
    uint64_t           admission_id = 0;
};
pending pending_orders[queue_capacity];
struct retained_order {
    order    record{};
    uint32_t incarnation  = 0;
    uint64_t token        = 0;
    bool     consumed     = false;
    uint64_t admission_id = 0;
};
retained_order retained_orders[queue_capacity];
unsigned       retained_count = 0;
uint16_t       command_sort[queue_capacity];

int mod(int x, int span) {
    const int r = x % span;
    return r < 0 ? r + span : r;
}
int  delta(int a, int b, int span) { return mh::gfx::wrapwin::stock_axis(a, b, span); }
bool valid_slot(mh::caps::arr a, unsigned player, unsigned id) {
    const auto &g = mh::caps::g(a);
    return g.base && player < g.players && id > 0 && id < g.limit;
}
const unit *live_unit(unsigned player, unsigned id) {
    return valid_slot(mh::caps::UNITS, player, id) ? mh::caps::rec<const unit>(mh::caps::UNITS, player, id) : nullptr;
}
const mh_cfg_final_struct_Unit *unit_config(unsigned id) {
    const auto *p = ptr<const mh_cfg_final_struct_Unit>(RID_UNIT);
    return p && id && id < live_size(RID_UNIT) / sizeof(*p) ? p + id : nullptr;
}
const mh_cfg_final_struct_Building *building_config(unsigned id) {
    const auto *p = ptr<const mh_cfg_final_struct_Building>(RID_BUILDING);
    return p && id && id < live_size(RID_BUILDING) / sizeof(*p) ? p + id : nullptr;
}
const mh_map_object_building *allocated_building(unsigned player, unsigned id) {
    if (!building_lives || player >= history_players || id >= building_rows ||
        !valid_slot(mh::caps::BUILDINGS, player, id)) return nullptr;
    const auto *b = mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, player, id);
    return b->index > 0 && b->building_id && b->state != 2 ? b : nullptr;
}
uint32_t mint_building_life() {
    return next_building_life == std::numeric_limits<uint32_t>::max() ? 0 : ++next_building_life;
}
void reset_building_lifetimes() {
    if (!building_lives) return;
    std::fill_n(building_lives.get(), history_players * building_rows, 0u);
    const auto &g = mh::caps::g(mh::caps::BUILDINGS);
    for (unsigned p = 0; p < std::min(g.players, history_players); ++p)
        for (unsigned id = 1; id < std::min(g.limit, building_rows); ++id)
            if (allocated_building(p, id))
                building_lives[p * building_rows + id] = mint_building_life();
}
double game_clock() {
    const auto *p = ptr<const double>(RID_STRAT_GAME_CLOCK);
    return p && std::isfinite(*p) ? *p : 0;
}
unit_command_state command_state(const unit &u) {
    return {{u.state, u.order, u.goal_x, u.goal_y, u.home_storage_slot,
             u.target_fine_x, u.target_fine_y, u.target_ref, u.target_index,
             u.selected_weapon, u.target2_fine_x, u.target2_fine_y,
             u.target2_ref, u.target2_index, u.move_group_id, u.home_x, u.home_y}};
}
bool primary_changed(const unit_command_state &a, const unit_command_state &b) {
    for (unsigned n = 0; n < 17; ++n) {
        if (n >= 10 && n <= 13) continue; // independent target2 channel
        if (a.values[n] != b.values[n]) return true;
    }
    return false;
}
history *remember(unsigned p, unsigned id, bool create) {
    if (!histories || p >= history_players || !id || id >= history_rows) return nullptr;
    const uint32_t incarnation = mh::storage_policy::hosted::unit_identity(p, id).incarnation;
    if (!incarnation) return nullptr;
    auto &h = histories[p * history_rows + id];
    if (h.incarnation != incarnation) {
        h = {};
        if (!create) return nullptr;
        h.incarnation = incarnation;
    }
    return &h;
}
bool enter_order(uint16_t s) {
    return s == 0x24 || s == 0x25 || s == 0x26 || s == 0x29 || s == 0x2a || s == 0x2b || s == 0x38;
}
bool travel_state(uint16_t s) {
    switch (s) {
        case 0x0a:
        case 0x0b:
        case 0x0c:
        case 0x0f:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x15:
        case 0x16:
        case 0x18:
        case 0x21:
        case 0x27:
        case 0x2c:
        case 0x2d:
        case 0x30:
        case 0x36:
        case 0x37: return true;
        default: return false;
    }
}
bool ground_path(const unit &u) {
    // Marshal EN482d35 normalizes order0f ->1 without cancelling the path. Idle STATE1 is
    // deliberately not here. Planning0a and move continuation0c are meaningful even at microstep31.
    switch (u.state) {
        case 0x0a:
        case 0x0c:
        case 0x0f:
        case 0x11:
        case 0x12:
            return u.path_cursor >= 0 && (u.state == 0x0a || u.path_slot_id != 0xff || u.move_microstep < 31);
        default: return false;
    }
}
bool displaced(const unit &u) { return u.goal_x != u.x || u.goal_y != u.y || u.move_microstep < 31; }
bool attack_state(const unit &u) {
    return ((u.state >= 0x1a && u.state <= 0x1c) || travel_state(u.state) || u.state == 0x2e || u.state == 0x2f) &&
           ((u.state >= 0x1a && u.state <= 0x1c) || (u.order >= 0x1a && u.order <= 0x1c));
}
template <typename Record>
command verb_of(const Record &q, const mh_cfg_final_struct_Unit &cfg) {
    const uint16_t op = static_cast<uint16_t>(q.param0);
    if (op == 0x10) return command::patrol;
    if ((op >= 0x1a && op <= 0x1c) || (q.order_code >= 0x1a && q.order_code <= 0x1c))
        return command::attack;
    if (enter_order(op) || enter_order(q.order_code)) return command::enter;
    if (op == 0x20 || q.order_code == 0x20) return command::exit;
    if (op == 0x17 || op == 0x18 || q.order_code == 0x17) return command::deploy;
    if (op == cfg.move_op_code || op == 0x33 || op == 0x36) return command::move;
    return command::unknown;
}
template <typename Record>
bool dual(const Record &q) { return q.order_code == 0x1d || q.order_code == 0x1e; }
bool ordinary_actor(const libmh_order_observation &o) {
    const unsigned kind = o.owner_and_kind & 0xf0;
    return o.owner_and_kind <= 0xff &&
           (kind == 0x20 || kind == 0x80);
}

template <typename Record>
bool strict_plain_move(
    const Record                   &o,
    const mh_cfg_final_struct_Unit &cfg) {
    if (static_cast<uint16_t>(o.param0) != cfg.move_op_code ||
        o.order_code != cfg.move_op_arg ||
        static_cast<uint16_t>(o.param0) == 0x10 ||
        o.args[0] < 0 || o.args[0] > 255 ||
        o.args[1] < 0 || o.args[1] > 255)
        return false;

    for (unsigned i = 2; i < 12; ++i)
        if (o.args[i] != -1)
            return false;

    const auto *width  = ptr<const uint32_t>(RID_WIDTH);
    const auto *height = ptr<const uint32_t>(RID_HEIGHT);
    return width && height &&
           *width > 0 && *width <= 256 &&
           *height > 0 && *height <= 256 &&
           static_cast<uint32_t>(o.args[0]) < *width &&
           static_cast<uint32_t>(o.args[1]) < *height;
}

bool owns_preview(
    const history &h, const libmh_order_observation &o) {
    return h.preview.phase != move_phase::none &&
           h.incarnation == o.actor_incarnation &&
           h.preview.admission_id == o.admission_id &&
           h.preview.world_generation == o.world_generation;
}

// Private helpers for target_line_intents.cpp, after owns_preview().
// Requires move_preview::ordinary_primary and history::previous_preview.
template <typename Record>
bool native_primary_preview(const Record &record) {
    const uint16_t param = static_cast<uint16_t>(record.param0);
    // The admission caller already proved this payload supersedes the primary.
    return record.owner_and_kind <= 0xff &&
           (record.owner_and_kind & 0xf0) == 0x80 &&
           record.order_code != 0x1d && record.order_code != 0x1e &&
           param != 0x1d && param != 0x1e &&
           record.order_code != mh::order_tasks::COMMAND &&
           param != mh::order_tasks::COMMAND;
}
template <typename Record>
bool managed_primary_preview(const Record &record) {
    namespace P           = mh::order_tasks::host_protocol;
    const uint32_t marker = static_cast<uint32_t>(record.args[8]);
    return native_primary_preview(record) &&
           P::is_primary_marker(marker);
}
template <typename Record>
bool ordinary_primary_preview(const Record &record) {
    return native_primary_preview(record) && !managed_primary_preview(record);
}
bool pending_ordinary_preview(const move_preview &preview, uint32_t generation) {
    return preview.ordinary_primary && preview.admission_id &&
           preview.world_generation == generation &&
           (preview.phase == move_phase::admitted || preview.phase == move_phase::suppressed);
}

void amend_preview_metadata(const W::canonical_order &before,
                            const W::canonical_order &after,
                            uint64_t admission_id, uint32_t actor_life) {
    // Called ONLY from the cache's certified consecutive metadata BEFORE/AFTER
    // pair after exact world/actor/admission/header/unchanged-payload validation.
    // A physical queue slot or raw-identical record is not an admission identity.
    if (!order_witness_owner || !admission_id || !actor_life) return;
    history *h = remember(before.owner_and_kind & 15u, before.unit_index, false);
    if (!h || h->incarnation != actor_life ||
        h->preview.admission_id > h->latest_primary_admission ||
        h->preview.admission_id != admission_id ||
        h->preview.world_generation != W::generation(*order_witness_owner) ||
        (h->preview.phase != move_phase::admitted && h->preview.phase != move_phase::suppressed))
        return;
    if (ordinary_primary_preview(after)) {
        h->preview.ordinary_primary = true;
        return;
    }
    // An untagged QOPA admission can temporarily replace a user's older pending
    // preview before the synchronous source wrapper stamps its managed origin.
    // Restore only that still-live pending ordinary preview. QOEX instead means
    // this exact head was captured into the canonical route, whose whole tail
    // must remain visible; never resurrect an older optimistic request for it.
    const bool restore = (static_cast<uint32_t>(after.args[8]) == mh::order_tasks::host_protocol::AUTONOMOUS_PRIMARY ||
                          static_cast<uint32_t>(after.args[8]) == mh::order_tasks::host_protocol::PHYSICAL_PRIMARY) &&
                         pending_ordinary_preview(h->previous_preview, W::generation(*order_witness_owner));
    h->preview          = restore ? h->previous_preview : move_preview{};
    h->previous_preview = {};
}


bool has_primary_payload(const libmh_order_observation &o) {
    // args[6] is guarded by args[5]; args[7] is weapon selection;
    // args[8..11] belong to the independent target2 channel.
    return o.args[0] != -1 ||
           o.args[1] != -1 ||
           o.args[2] != -1 ||
           o.args[3] != -1 ||
           o.args[4] != -1 ||
           o.args[5] != -1 ||
           o.args[12] != -1;
}

bool same_header_auxiliary_only(
    const libmh_order_observation &o, const unit &u) {
    if (static_cast<uint16_t>(o.param0) != u.order ||
        o.order_code != u.state ||
        has_primary_payload(o))
        return false;

    // args[6] alone is inert: assignment is guarded by args[5].
    // args[7] is weapon selection; args[8..11] are target2 payload.
    return o.args[6] != -1 ||
           o.args[7] != -1 ||
           o.args[8] != -1 ||
           o.args[9] != -1 ||
           o.args[10] != -1 ||
           o.args[11] != -1;
}

bool supersedes_primary(
    const libmh_order_observation  &o,
    const unit                     &u,
    const mh_cfg_final_struct_Unit &cfg) {
    if (!ordinary_actor(o) || dual(o))
        return false;

    if (o.order_code == 0x32)
        return false;

    if (o.order_code == 0x23)
        return u.state == 0x22;

    if (o.order_code == 0x20)
        return u.state >= 0x1f && u.state <= 0x2b;

    // Must precede default/stop recognition. A weapon-only update may
    // happen to carry the currently active default/stop header.
    if (same_header_auxiliary_only(o, u))
        return false;

    const uint16_t requested_order =
        static_cast<uint16_t>(o.param0);

    if ((requested_order == 1 && o.order_code == 1) ||
        (requested_order == cfg.default_op_code &&
         o.order_code == cfg.default_op_code))
        return true;

    if (has_primary_payload(o))
        return true;

    if (requested_order != u.order || o.order_code != u.state)
        return true;

    if (u.target_ref != 0)
        return true;

    return false;
}
template <typename Record>
bool applied(const Record &q, const unit &u, bool require_state = true) {
    if (dual(q)) {
        return (q.args[8] == -1 || u.target2_fine_x == q.args[8]) &&
               (q.args[9] == -1 || u.target2_fine_y == q.args[9]) &&
               (q.args[10] == -1 || (u.target2_ref == static_cast<int16_t>(q.args[10]) &&
                                     u.target2_index == static_cast<int16_t>(q.args[11]))) &&
               (q.args[7] == -1 || u.selected_weapon == static_cast<uint8_t>(q.args[7]));
    }
    if (u.order_queued || (require_state && u.state != q.order_code) || u.order != static_cast<uint16_t>(q.param0)) return false;
    return (q.args[0] == -1 || u.goal_x == static_cast<uint8_t>(q.args[0])) &&
           (q.args[1] == -1 || u.goal_y == static_cast<uint8_t>(q.args[1])) &&
           (q.args[2] == -1 || u.home_storage_slot == static_cast<uint8_t>(q.args[2])) &&
           (q.args[3] == -1 || u.target_fine_x == q.args[3]) &&
           (q.args[4] == -1 || u.target_fine_y == q.args[4]) &&
           (q.args[5] == -1 || (u.target_ref == static_cast<int16_t>(q.args[5]) &&
                                u.target_index == static_cast<int16_t>(q.args[6]))) &&
           (q.args[7] == -1 || u.selected_weapon == static_cast<uint8_t>(q.args[7])) &&
           (q.args[12] == -1 || u.move_group_id == q.args[12]);
}
template <typename Left, typename Right>
bool same_command(const Left &a, const Right &b) {
    return a.unit_index == b.unit_index && a.owner_and_kind == b.owner_and_kind &&
           a.param0 == b.param0 && a.order_code == b.order_code && std::memcmp(a.args, b.args, sizeof a.args) == 0;
}
template <typename Left, typename Right>
bool order_less(const Left &a, const Right &b) {
    // Both layouts carry the same padding-free canonical60 bytes; omit scheduling time.
    return std::memcmp(&a.unit_index, &b.unit_index, sizeof(order) - sizeof(double)) < 0;
}

bool metadata_amendment(const W::canonical_order &before,
                        const W::canonical_order &after) {
    constexpr size_t prefix = offsetof(W::canonical_order, args) +
                              8 * sizeof(before.args[0]);
    return std::memcmp(&before, &after, prefix) == 0 &&
           before.args[12] == after.args[12] &&
           std::memcmp(&before, &after, sizeof before) != 0;
}

void observe_metadata_pair(const libmh_order_observation &o) {
    if (!order_witness_owner ||
        o.world_generation != W::generation(*order_witness_owner)) {
        metadata_before = {};
        return;
    }
    if (o.kind == LIBMH_EVK_ORDER_METADATA_BEFORE) {
        metadata_before = {};
        if (o.admission_id && o.actor_incarnation) metadata_before = o;
        return;
    }
    const libmh_order_observation before = metadata_before;
    metadata_before                      = {};
    if (o.kind != LIBMH_EVK_ORDER_METADATA_AFTER ||
        before.kind != LIBMH_EVK_ORDER_METADATA_BEFORE ||
        !before.admission_id || before.admission_id != o.admission_id ||
        before.command_id != o.command_id || before.flags != o.flags ||
        before.world_generation != o.world_generation ||
        before.actor_incarnation != o.actor_incarnation ||
        before.owner_and_kind != o.owner_and_kind ||
        before.unit_index != o.unit_index)
        return;
    const unsigned player     = o.owner_and_kind & 15;
    const unsigned actor_kind = o.owner_and_kind & 0xf0;
    if (o.owner_and_kind > 0xff ||
        (actor_kind != 0x20 && actor_kind != 0x80 && actor_kind != 0xa0) ||
        !valid_slot(mh::caps::UNITS, player, o.unit_index) ||
        witnessed_actor_life(nullptr, player, o.unit_index) != o.actor_incarnation)
        return;
    const auto old_key = W::canonical(before);
    const auto new_key = W::canonical(o);
    if (!metadata_amendment(old_key, new_key)) return;
    metadata_amendment_admission = o.admission_id;
    amend_attack_metadata(old_key, new_key);
    amend_intent_metadata(old_key, new_key);
    metadata_amendment_admission = 0;
}
bool common_marker(const order &q) {
    // EN444fcb stamps40xx; EN444e5f's live world-input callers pass8000 before its seq OR.
    // Arbitrary AI planner tags and unstamped move_group_id values are NOT common-command proof.
    const uint32_t modifier = static_cast<uint32_t>(q.args[12]) & 0xffffff00u;
    return (modifier == 0x4000u || modifier == 0x8000u) && (q.args[12] & 255) != 0;
}
bool bundle_less(const pending &a, const pending &b) {
    const auto &x = a.record, &y = b.record;
    if ((x.owner_and_kind & 15) != (y.owner_and_kind & 15)) return (x.owner_and_kind & 15) < (y.owner_and_kind & 15);
    if (x.args[12] != y.args[12]) return x.args[12] < y.args[12];
    const bool x_patrol = x.param0 == 0x10, y_patrol = y.param0 == 0x10;
    if (x_patrol != y_patrol) return x_patrol < y_patrol;
    if (x.args[0] != y.args[0]) return x.args[0] < y.args[0];
    return x.args[1] < y.args[1];
}
void check_world(const world_view &v) {
    const uint32_t base = mh::caps::g(mh::caps::UNITS).base;
    const double   now  = game_clock();
    if ((world_base && world_base != base) || (world_width && (world_width != v.map_width || world_height != v.map_height)) || now < last_clock)
        reset_intents();
    world_base   = base;
    world_width  = v.map_width;
    world_height = v.map_height;
    last_clock   = now;
}
point world_point(const world_view &v, point p) {
    return {mod(p.x + v.camera_x, v.span_x), mod(p.y + v.camera_y, v.span_y)};
}
bool ground(const world_view &v, int x, int y, point &out) {
    if (x < 0 || x >= v.map_width || y < 0 || y >= v.map_height) return false;
    out = {x * 32 + 16, y * 32 + 16};
    return true;
}
bool building_anchor(const world_view &v, unsigned player, unsigned id, point &out) {
    if (!valid_slot(mh::caps::BUILDINGS, player, id)) return false;
    const auto &b     = *mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, player, id);
    const auto *cfg   = building_config(b.building_id);
    const auto *tiles = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    if (!cfg || !tiles || !(b.energy > 0) || b.state == 2 || b.x >= v.map_width || b.y >= v.map_height || !cfg->width || !cfg->height) return false;
    bool mapped = false, visible = false;
    for (int x = 0; x < 5; ++x)
        for (int y = 0; y < 5; ++y) {
            if (!cfg->area[x][y]) continue;
            const auto &tile = tiles[(mod(b.x + x, v.map_width) << 8) | mod(b.y + y, v.map_height)];
            mapped           = mapped || (tile.class_owner == (0x40 | player) && tile.building == id);
            visible          = visible || tile.visibility != 0;
        }
    if (!mapped || !visible) return false;
    out = {mod(b.x * 32 + cfg->width * 16, v.span_x), mod(b.y * 32 + cfg->height * 16, v.span_y)};
    return true;
}
uint32_t entity_incarnation(uint32_t entity) {
    const unsigned player = (entity >> 16) & 15, id = entity & 65535;
    if (entity & 0x80000000u) return mh::storage_policy::hosted::unit_identity(player, id).incarnation;
    if (entity & 0x40000000u) return building_incarnation(player, id);
    return 0;
}
bool unit_anchor(const world_view &v, sv::world_units &mapped, unsigned p, unsigned id,
                 point &out, sv::unit_projection *projection = nullptr, point *ground_out = nullptr) {
    const unit *u = live_unit(p, id);
    if (!u || !mapped.contains(v, p, id) || (u->state >= 2 && u->state <= 4) ||
        u->state == 0x1f || u->state == 0x23 || u->state == 0xcd) return false;
    sv::unit_projection projected;
    if (!sv::project_unit(v, p, id, projected) || !projected.tile_visible) return false;
    out = world_point(v, projected.anchor);
    if (projection) *projection = projected;
    if (ground_out) {
        int32_t x = 0, y = 0;
        mh::call::llm_strat_unit_get_coords(p, id, &x, &y);
        *ground_out = {mod(x, v.span_x), mod(y, v.span_y)};
    }
    return true;
}
bool entity_anchor(const world_view &v, sv::world_units &mapped, uint16_t ref, uint16_t id,
                   point &out, point &ground_out, uint32_t &identity) {
    const unsigned p = ref & 15;
    if ((ref & 0xf0) == 0x40) {
        if (!building_anchor(v, p, id, out)) return false;
        ground_out = out;
        identity   = 0x40000000u | (p << 16) | id;
        return true;
    }
    if ((ref & 0xf0) == 0x20 || (ref & 0xf0) == 0x80 || (ref & 0xf0) == 0xa0) {
        if (!unit_anchor(v, mapped, p, id, out, nullptr, &ground_out)) return false;
        identity = 0x80000000u | (p << 16) | id;
        return true;
    }
    return false;
}
bool storage_destination(const world_view &v, unsigned p, const unit &u, point &out, uint32_t &entity) {
    if (!valid_slot(mh::caps::UNIT_STORAGE, p, u.home_storage_slot)) return false;
    const auto &st = *mh::caps::rec<const mh_map_object_unit_storage>(mh::caps::UNIT_STORAGE, p, u.home_storage_slot);
    if (st.b_index <= 0 || !building_anchor(v, p, static_cast<unsigned>(st.b_index), out)) return false;
    const auto &b = *mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, p, st.b_index);
    if (b.sub_id != u.home_storage_slot) return false;
    entity = 0x40000000u | (p << 16) | static_cast<unsigned>(st.b_index);
    return true;
}
struct collector {
    const world_view &v;
    sv::world_units  &mapped;
    intent           *items;
    unsigned          count, capacity;
    void              add(point source, point origin, point ground_origin, point dest, point ground_dest, uint32_t entity, kind type,
                          unsigned p, unsigned id, unsigned mover, bool visible, history *h, bool building_source = false) {
        if (count >= capacity) return;
        auto &i              = items[count];
        i                    = {};
        i.source             = source;
        i.origin             = origin;
        i.group_origin       = ground_origin;
        i.destination        = dest;
        i.ground_destination = ground_dest;
        i.entity             = entity;
        i.target_incarnation = entity_incarnation(entity);
        i.type               = type;
        i.source_id          = static_cast<uint16_t>(id);
        i.player             = static_cast<uint8_t>(p);
        i.mover              = static_cast<uint8_t>(mover);
        i.visible            = visible;
        i.building_source    = building_source;
        if (h) {
            const bool matching = (type == kind::move && (h->verb == command::move || h->verb == command::attack || h->verb == command::exit)) ||
                                  ((type == kind::patrol || type == kind::patrol_home) && h->verb == command::patrol) ||
                                  (type == kind::attack && h->verb == command::attack) ||
                                  (type == kind::enter && h->verb == command::enter) ||
                                  (type == kind::exit && h->verb == command::exit) ||
                                  (type == kind::deploy && h->verb == command::deploy);
            i.command_token = matching ? h->token : 0;
            if (type == kind::attack &&
                (h->primary_entity != entity || h->primary_target_incarnation != i.target_incarnation))
                i.command_token = 0;
            if (type == kind::secondary)
                i.command_token = h->secondary_entity == entity &&
                                          h->secondary_target_incarnation == i.target_incarnation
                                                   ? h->secondary_token
                                                   : 0;
            i.incarnation = h->incarnation;
        }
        ++count;
    }
    void unit_intents(unsigned p, unsigned id) {
        point               origin, group_origin;
        sv::unit_projection projection;
        if (!unit_anchor(v, mapped, p, id, origin, &projection, &group_origin)) return;
        const auto &u   = *live_unit(p, id);
        const auto *cfg = unit_config(u.unit_proto_id);
        if (!cfg) return;
        auto      *h       = remember(p, id, true);
        const bool visible = sv::visible_body(v, projection);
        point      dest, ground_dest;
        uint32_t   entity        = 0;
        const bool primary       = attack_state(u);
        const bool recorded_move = h && (h->verb == command::move || h->verb == command::attack || h->verb == command::exit) &&
                                   ((u.goal_x == h->goal.x && u.goal_y == h->goal.y) ||
                                    (h->verb == command::attack && u.goal_x == h->home.x && u.goal_y == h->home.y));
        const bool recorded_patrol = h && h->verb == command::patrol &&
                                     ((u.goal_x == h->goal.x && u.goal_y == h->goal.y && u.home_x == h->home.x && u.home_y == h->home.y) ||
                                      (u.goal_x == h->home.x && u.goal_y == h->home.y && u.home_x == h->goal.x && u.home_y == h->goal.y));
        const auto add = [&](point target, uint32_t exact, kind type) {
            this->add(projection.anchor, origin, group_origin, target, exact ? ground_dest : target, exact, type, p, id, cfg->move_op_arg, visible, h);
        };
        bool preview_primary = false;

        if (h && h->preview.phase != move_phase::none) {
            auto &preview = h->preview;

            if (!order_witness_owner ||
                preview.world_generation !=
                    W::generation(*order_witness_owner)) {
                preview = {};
            } else if (preview.phase == move_phase::suppressed) {
                // Hide the superseded actual primary while this exact admitted
                // incompatible command awaits its own application/discard outcome.
                preview_primary = true;
            } else {
                if (preview.phase == move_phase::applied) {
                    const bool ordinary_order =
                        u.order == cfg->move_op_code ||
                        (cfg->move_op_arg == 0x0a && u.order == 1);

                    const bool terminal =
                        u.state == 1 || u.state == 0x13 ||
                        u.state == 0x2e || u.state == 0x2f;

                    if (!ordinary_order || terminal)
                        preview = {};
                }

                if (preview.phase != move_phase::none &&
                    ground(v, preview.goal.x, preview.goal.y, dest)) {
                    const unsigned before = count;
                    add(dest, 0, kind::move);

                    if (count != before) {
                        items[count - 1].command_token =
                            preview.command_id
                                ? preview.command_id |
                                      W::presentation_token_namespace
                                : 0;
                    }

                    preview_primary = true;
                }
            }
        }
        if (preview_primary) {
            // Already drawn or intentionally suppressed by an admitted primary.
        } else if (primary) {
            if (entity_anchor(v, mapped, static_cast<uint16_t>(u.target_ref), static_cast<uint16_t>(u.target_index), dest, ground_dest, entity)) add(dest, entity, kind::attack);
        } else if (enter_order(u.state) || (travel_state(u.state) && enter_order(u.order))) {
            if (storage_destination(v, p, u, dest, entity)) {
                ground_dest = dest;
                add(dest, entity, kind::enter);
            }
        } else if (u.state == 0x17 || (u.state == 0x7c && u.order != 0x31) ||
                   (travel_state(u.state) && (u.order == 0x17 || u.order == 0x18))) {
            const bool converting = u.state == 0x17 || u.state == 0x7c;
            if (ground(v, converting ? u.x : u.goal_x, converting ? u.y : u.goal_y, dest)) add(dest, 0, kind::deploy);
        } else if (u.order == 0x10 && (travel_state(u.state) || u.state == 0x2e || u.state == 0x2f) &&
                   (cfg->move_op_arg == 0x0a || recorded_patrol)) {
            // Aircraft's autonomous scatter uses the same opcode. Only a genuinely observed
            // command with its still-valid two-leg pair distinguishes that ambiguous lifecycle.
            if (ground(v, u.goal_x, u.goal_y, dest)) add(dest, 0, kind::patrol);
            if ((u.home_x != u.goal_x || u.home_y != u.goal_y) && ground(v, u.home_x, u.home_y, dest)) add(dest, 0, kind::patrol_home);
        } else if (u.state == 0x20 || u.state == 0x21 || u.state == 0x22 || u.state == 0x27 || u.state == 0x28 ||
                   (u.state == 0x15 && (u.order == cfg->default_op_code || recorded_move))) {
            point    receiver;
            uint32_t receiver_id = 0;
            if (storage_destination(v, p, u, receiver, receiver_id) && displaced(u) && ground(v, u.goal_x, u.goal_y, dest)) add(dest, 0, kind::exit);
        } else if (displaced(u)) {
            const bool ground_move = cfg->move_op_arg == 0x0a && ground_path(u) &&
                                     (u.order == cfg->move_op_code || u.order == 1 || u.order == 0x33 || u.order == 0x36);
            const bool air_move = cfg->move_op_arg != 0x0a && travel_state(u.state) &&
                                  (u.order == cfg->move_op_code || u.order == 0x33 || u.order == 0x36) &&
                                  (recorded_move || (u.state != 0x0b && u.state != 0x15 && u.state != 0x16));
            if ((ground_move || air_move) && ground(v, u.goal_x, u.goal_y, dest)) add(dest, 0, kind::move);
        }
        // A target acquired in range is not evidence of firing. Only a successful projectile or
        // direct-damage action may set this record, including concurrent primary/secondary fire.
        if (h && u.target2_ref && u.selected_weapon <= 4) {
            const double   now              = game_clock();
            const uint16_t ref              = static_cast<uint16_t>(u.target2_ref);
            const uint16_t target_id        = static_cast<uint16_t>(u.target2_index);
            const uint32_t target_entity    = ((ref & 0xa0) ? 0x80000000u : 0x40000000u) | ((ref & 15u) << 16) | target_id;
            const bool     same_incarnation = entity_incarnation(target_entity) == h->fired_target_incarnation;
            if (h->secondary_ref != ref || h->secondary_id != target_id || !same_incarnation || now < h->fired_at)
                h->fired_until = 0;
            if (now < h->fired_until && entity_anchor(v, mapped, ref, target_id, dest, ground_dest, entity)) {
                const bool duplicate_drawn_primary =
                    count != 0 &&
                    items[count - 1].player == p &&
                    items[count - 1].source_id == id &&
                    items[count - 1].type == kind::attack &&
                    items[count - 1].entity == entity &&
                    items[count - 1].target_incarnation ==
                        entity_incarnation(entity);

                if (duplicate_drawn_primary) {
                    items[count - 1].also_secondary = true;
                    if (h->secondary_entity == entity &&
                        h->secondary_target_incarnation ==
                            items[count - 1].target_incarnation) {
                        items[count - 1].secondary_command_token = h->secondary_token;
                        items[count - 1].command_token =
                            std::max(items[count - 1].command_token,
                                     h->secondary_token);
                    }
                } else {
                    add(dest, entity, kind::secondary);
                }
            }
        }
    }
    void building_intents(unsigned p, unsigned id) {
        point origin;
        if (!building_anchor(v, p, id, origin)) return;
        const auto &b       = *mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, p, id);
        const point source  = sv::project_fine(v, origin.x, origin.y);
        const bool  visible = source.x >= v.shown.left && source.y >= v.shown.top && source.x < v.shown.left + v.shown.width && source.y < v.shown.top + v.shown.height;
        point       dest, ground_dest;
        const auto  rally = mh::storage_policy::hosted::read_rally(p, id);
        if (rally.enabled && ground(v, rally.x, rally.y, dest)) add(source, origin, origin, dest, dest, 0, kind::rally, p, id, 0, visible, nullptr, true);
        const auto *cfg = building_config(b.building_id);
        if (!cfg || b.state != 0x7b || !(b.efficiency > 0) || (cfg->type != 5 && cfg->type != 0x19) || !valid_slot(mh::caps::TURRETS, p, b.sub_id)) return;
        const auto &t = *mh::caps::rec<const mh_map_object_turret>(mh::caps::TURRETS, p, b.sub_id);
        if (t.b_index != id || t.counter_target_slot <= 0 || t.counter_target_slot > 65535) return;
        const uint16_t ref    = static_cast<uint16_t>((t.counter_ref & 15) | ((t.counter_ref & 0x80) ? 0x80 : 0x40));
        uint32_t       entity = 0;
        if (!entity_anchor(v, mapped, ref, static_cast<uint16_t>(t.counter_target_slot), dest, ground_dest, entity)) return;
        if (ref & 0x80) {
            int32_t x = 0, y = 0;
            mh::call::llm_strat_unit_get_coords(ref & 15, t.counter_target_slot, &x, &y);
            if (mh::call::llm_strat_dist_out_of_range(0, t.attack_range + 1, origin.x / 32, origin.y / 32, x / 32, y / 32)) return;
        }
        add(source, origin, origin, dest, ground_dest, entity, kind::attack, p, id, 0, visible, nullptr, true);
    }
};

} // namespace
// Public detail-namespace function, after the private namespace closes.
// Read-only DRAW precedence; never cancels, adopts, advances or mutates a task.
bool pending_ordinary_primary(unsigned player, unsigned slot, uint32_t actor_life) {
    if (!order_witness_owner || !actor_life) return false;
    const history *h = remember(player, slot, false);
    if (!h || h->incarnation != actor_life) return false;
    const auto &preview = h->preview;
    return preview.admission_id <= h->latest_primary_admission &&
           pending_ordinary_preview(preview, W::generation(*order_witness_owner));
}

libmh_order_witness_store *
selected_targets_order_witness_store() {
    if (order_witness_owner)
        return order_witness_owner;

    const uint32_t capacities[3] = {
        mh::caps::orders::wide_cap(mh::caps::orders::QUEUE),
        mh::caps::orders::wide_cap(mh::caps::orders::PENDING),
        mh::caps::orders::wide_cap(mh::caps::orders::STAGING)};

    const size_t bytes = W::required_bytes(capacities);
    if (!bytes)
        return nullptr;

    std::unique_ptr<uint8_t[]> memory(
        new (std::nothrow) uint8_t[bytes]);
    if (!memory)
        return nullptr;

    W::store *created = W::initialize(
        memory.get(), bytes, capacities,
        witnessed_actor_life, nullptr,
        deliver_native_order, nullptr);

    if (!created)
        return nullptr;

    order_witness_memory = std::move(memory);
    order_witness_owner  = created;
    return order_witness_owner;
}

void amend_intent_metadata(const W::canonical_order &before,
                           const W::canonical_order &after) {
    // A raw payload never identifies an admission. The synchronous paired
    // lifecycle consumer supplies the exact admission certificate for this call.
    if (!metadata_amendment_admission || !metadata_amendment(before, after)) return;
    const unsigned player     = before.owner_and_kind & 15;
    const unsigned actor_kind = before.owner_and_kind & 0xf0;
    if (before.owner_and_kind > 0xff ||
        (actor_kind != 0x20 && actor_kind != 0x80 && actor_kind != 0xa0) ||
        !valid_slot(mh::caps::UNITS, player, before.unit_index))
        return;
    const uint32_t life = witnessed_actor_life(nullptr, player, before.unit_index);
    if (!life) return;
    amend_preview_metadata(before, after, metadata_amendment_admission, life);
    const auto rekey = [&](auto &entry) {
        if (entry.admission_id != metadata_amendment_admission ||
            entry.incarnation != life || !same_command(entry.record, before))
            return false;
        // Same-node copies may all rekey; raw-identical different admissions
        // never do. Preserve token, incarnation, before snapshot and consumed.
        std::memcpy(entry.record.args + 8, after.args + 8, 4 * sizeof(after.args[0]));
        return true;
    };
    for (unsigned i = 0; i < pending_count; ++i) rekey(pending_orders[i]);
    bool retained_changed = false;
    for (unsigned i = 0; i < retained_count; ++i)
        retained_changed = rekey(retained_orders[i]) || retained_changed;
    if (retained_changed) {
        // Stable, allocation-free repair of the existing bounded array.
        // Already ordered rows are not copied.
        for (unsigned i = 1; i < retained_count; ++i) {
            if (!order_less(retained_orders[i].record, retained_orders[i - 1].record)) continue;
            const retained_order value    = retained_orders[i];
            unsigned             position = i;
            do {
                retained_orders[position] = retained_orders[position - 1];
                --position;
            } while (position && order_less(value.record, retained_orders[position - 1].record));
            retained_orders[position] = value;
        }
    }
    // Open-dispatch command_sort uses header, args0/1/12 and queue_index,
    // all unchanged. POST rebuilds exact-key ordering before searching it.
}

void observe_order_lifecycle(
    const libmh_order_observation &o) {
    if (o.kind == LIBMH_EVK_ORDER_METADATA_BEFORE ||
        o.kind == LIBMH_EVK_ORDER_METADATA_AFTER) {
        observe_metadata_pair(o);
        return;
    }
    // The core promises consecutive synchronous BEFORE/AFTER delivery. Any
    // unrelated event abandons an incomplete pair instead of joining stale IDs.
    metadata_before = {};
    if (!order_witness_owner ||
        o.world_generation != W::generation(*order_witness_owner))
        return;

    if (o.kind == LIBMH_EVK_ORDER_WORLD_RESET) {
        clear_intent_rows();
        return;
    }

    if (o.kind == LIBMH_EVK_MOVE_GROUP_REVOKED) {
        if (!histories || !o.command_id)
            return;
        for (unsigned i = 0;
             i < history_players * history_rows; ++i) {
            auto &preview = histories[i].preview;
            if (preview.world_generation == o.world_generation &&
                preview.command_id == o.command_id)
                preview.command_id = 0;
        }
        return;
    }

    if (!ordinary_actor(o))
        return;

    const unsigned player = o.owner_and_kind & 15u;
    const unsigned id     = o.unit_index;
    const unit    *u      = live_unit(player, id);
    const auto    *cfg    = u ? unit_config(u->unit_proto_id) : nullptr;

    if (!u || !cfg || !(u->energy > 0) ||
        !o.actor_incarnation)
        return;

    history *h = remember(player, id, true);
    if (!h || h->incarnation != o.actor_incarnation)
        return;

    // Retire an exact older rollback candidate even while a newer untagged
    // managed admission is awaiting synchronous metadata classification.
    if ((o.kind == LIBMH_EVK_ORDER_APPLIED || o.kind == LIBMH_EVK_ORDER_DISCARDED) &&
        h->previous_preview.admission_id == o.admission_id &&
        h->previous_preview.world_generation == o.world_generation)
        h->previous_preview = {};

    switch (o.kind) {
        case LIBMH_EVK_ORDER_ADMITTED: {
            if (!o.admission_id || dual(o) ||
                o.admission_id <= h->latest_primary_admission)
                return;

            // Already-classified managed activity never replaces an ordinary
            // pending endpoint or hides it merely because it was admitted later.
            if (managed_primary_preview(o)) return;
            const bool move = strict_plain_move(o, *cfg);
            if (!move && !supersedes_primary(o, *u, *cfg))
                return;

            // Do not advance this watermark for auxiliary/admin/echo updates.
            h->latest_primary_admission = o.admission_id;
            h->previous_preview         = pending_ordinary_preview(h->preview, o.world_generation)
                                              ? h->preview
                                              : move_preview{};
            h->preview                  = {};
            h->preview.admission_id     = o.admission_id;
            h->preview.world_generation = o.world_generation;
            h->preview.ordinary_primary = ordinary_primary_preview(o);

            if (move) {
                h->preview.command_id = o.command_id;
                h->preview.goal       = {o.args[0], o.args[1]};
                h->preview.phase      = move_phase::admitted;
            } else {
                // A generation-owned tombstone, not a drawable movement request.
                h->preview.phase = move_phase::suppressed;
            }
            return;
        }

        case LIBMH_EVK_ORDER_APPLIED:
            if (!owns_preview(*h, o))
                return;

            h->previous_preview = {};
            if (h->preview.phase == move_phase::suppressed) {
                // The superseding command is now authoritative. Let its actual live
                // attack/patrol/storage/etc. presentation supply the primary connector.
                h->preview = {};
            } else {
                h->preview.phase = move_phase::applied;
            }
            return;

        case LIBMH_EVK_ORDER_DISCARDED:
            if (owns_preview(*h, o)) {
                h->previous_preview = {};
                // The pending request did not become authoritative. Reveal whatever
                // primary activity is actually still live; do not resurrect an old
                // optimistic preview.
                h->preview = {};
            }
            return;

        case LIBMH_EVK_UNIT_PRIMARY_TERMINAL:
            // A preceding activity can finish while a newer move or suppression
            // request remains admitted. Neither pending state is retired here.
            if (h->preview.phase == move_phase::applied)
                h->preview = {};
            return;

        default:
            return;
    }
}

bool reserve_intents() {
    const unsigned rows   = mh::caps::RAISED_ROW[mh::caps::UNITS];
    const unsigned b_rows = mh::caps::RAISED_ROW[mh::caps::BUILDINGS];
    if (!rows || !b_rows || mh::caps::g(mh::caps::UNITS).players > history_players ||
        mh::caps::g(mh::caps::BUILDINGS).players > history_players) return false;
    if (!selected_targets_order_witness_store()) return false;
    if (histories) return history_rows == rows && building_rows == b_rows;
    std::unique_ptr<history[]>  new_histories(new (std::nothrow) history[history_players * rows]);
    std::unique_ptr<uint32_t[]> new_lives(new (std::nothrow) uint32_t[history_players * b_rows]);
    if (!new_histories || !new_lives) return false;
    histories      = std::move(new_histories);
    building_lives = std::move(new_lives);
    history_rows   = rows;
    building_rows  = b_rows;
    reset_building_lifetimes();
    return true;
}
namespace {
void clear_intent_rows() {
    metadata_before              = {};
    metadata_amendment_admission = 0;
    reset_attack_provenance();
    if (histories) std::memset(histories.get(), 0, sizeof(history) * history_players * history_rows);
    reset_building_lifetimes();
    dispatch_open  = false;
    pending_count  = 0;
    retained_count = 0;
    world_base     = 0;
    world_width = world_height = 0;
    last_clock                 = 0;
    // Presentation tokens remain process-unique across identical restored worlds.
}
} // namespace
void reset_intents() {
    if (order_witness_owner)
        W::reset_world(*order_witness_owner);
    else
        clear_intent_rows();
}
uint32_t building_incarnation(unsigned player, unsigned slot) {
    const auto *b = allocated_building(player, slot);
    return b && b->energy > 0 ? building_lives[player * building_rows + slot] : 0;
}
void observe_building_birth(unsigned player, unsigned slot) {
    if (allocated_building(player, slot))
        building_lives[player * building_rows + slot] = mint_building_life();
}
void observe_building_retirement(unsigned player, unsigned slot) {
    if (building_lives && player < history_players && slot && slot < building_rows)
        building_lives[player * building_rows + slot] = 0;
}
uint64_t allocate_intent_token() {
    if (next_token >= W::presentation_token_namespace - 1u)
        std::abort();
    return ++next_token;
}
void observe_dispatch(bool replay_replaced) {
    if (replay_replaced) reset_intents();
    dispatch_open        = false;
    pending_count        = 0;
    const unsigned count = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const auto    *queue = reinterpret_cast<const order *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (!queue || count > queue_capacity) return;
    for (unsigned n = 0; n < count; ++n) {
        const order   &q = queue[n];
        const unsigned k = q.owner_and_kind & 0xf0, p = q.owner_and_kind & 15;
        if (k != 0x20 && k != 0x80) continue;
        const unit    *u           = live_unit(p, q.unit_index);
        const uint32_t incarnation = mh::storage_policy::hosted::unit_identity(p, q.unit_index).incarnation;
        if (!u || !(u->energy > 0) || !incarnation) continue;
        auto &o         = pending_orders[pending_count];
        o.record        = q;
        o.before        = command_state(*u);
        o.incarnation   = incarnation;
        o.queue_index   = n;
        o.admission_id  = order_witness_owner
                              ? W::metadata_at(*order_witness_owner, W::container::queue, n).admission_id
                              : 0;
        const auto *old = std::lower_bound(
            retained_orders, retained_orders + retained_count, q,
            [](const retained_order &a, const order &b) { return order_less(a.record, b); });
        while (old != retained_orders + retained_count && same_command(old->record, q) &&
               (old->admission_id != o.admission_id || old->consumed ||
                old->incarnation != incarnation))
            ++old;
        o.token                     = o.admission_id && old != retained_orders + retained_count &&
                          same_command(old->record, q)
                                          ? old->token
                                          : 0;
        command_sort[pending_count] = static_cast<uint16_t>(pending_count);
        ++pending_count;
    }
    // The genuine issuer sequence is scoped to THIS dispatch, not retained as a planner group.
    // Repeated/deferred records cannot change a current command until a real commit is observed.
    std::sort(command_sort, command_sort + pending_count, [](uint16_t a, uint16_t b) {
        if (bundle_less(pending_orders[a], pending_orders[b])) return true;
        if (bundle_less(pending_orders[b], pending_orders[a])) return false;
        if (pending_orders[a].record.unit_index != pending_orders[b].record.unit_index)
            return pending_orders[a].record.unit_index < pending_orders[b].record.unit_index;
        return pending_orders[a].queue_index < pending_orders[b].queue_index;
    });
    for (unsigned first = 0; first < pending_count;) {
        unsigned    end       = first + 1;
        const auto &base      = pending_orders[command_sort[first]];
        bool        collision = false;
        while (end < pending_count && !bundle_less(base, pending_orders[command_sort[end]]) &&
               !bundle_less(pending_orders[command_sort[end]], base)) {
            collision = collision || pending_orders[command_sort[end - 1]].record.unit_index ==
                                         pending_orders[command_sort[end]].record.unit_index;
            ++end;
        }
        const unit *issuer     = live_unit(base.record.owner_and_kind & 15, base.record.unit_index);
        const auto *cfg        = issuer ? unit_config(issuer->unit_proto_id) : nullptr;
        const bool  plain_move = cfg && strict_plain_move(base.record, *cfg);
        if (!plain_move && common_marker(base.record)) {
            const uint64_t token = collision ? 0 : allocate_intent_token();
            for (unsigned n = first; n < end; ++n) {
                auto &o = pending_orders[command_sort[n]];
                if (collision || !o.token) o.token = token;
            }
        }
        first = end;
    }
    dispatch_open = true;
}
void observe_weapon_fired(unsigned player, unsigned id, uint16_t target_ref, uint16_t target_id) {
    const unit *u = live_unit(player, id);
    if (!u || !(u->energy > 0) || !target_ref || !target_id ||
        static_cast<uint16_t>(u->target2_ref) != target_ref || static_cast<uint16_t>(u->target2_index) != target_id) return;
    const uint32_t target_entity = ((target_ref & 0xa0) ? 0x80000000u : 0x40000000u) | ((target_ref & 15u) << 16) | target_id;
    const uint32_t target_life   = entity_incarnation(target_entity);
    if (!target_life) return;
    auto *h = remember(player, id, true);
    if (!h) return;
    h->secondary_ref            = target_ref;
    h->secondary_id             = target_id;
    h->fired_target_incarnation = target_life;
    h->fired_at                 = game_clock();
    h->fired_until              = h->fired_at + 0.25; // exact successful effect, not passive acquisition
}
void observe_after_dispatch() {
    if (!dispatch_open) return;
    dispatch_open = false; // close BEFORE any later policy/cancel/lifecycle work can run
    std::sort(command_sort, command_sort + pending_count, [](uint16_t a, uint16_t b) {
        const auto &x = pending_orders[a].record, &y = pending_orders[b].record;
        if ((x.owner_and_kind & 15) != (y.owner_and_kind & 15)) return (x.owner_and_kind & 15) < (y.owner_and_kind & 15);
        if (x.unit_index != y.unit_index) return x.unit_index < y.unit_index;
        return pending_orders[a].queue_index < pending_orders[b].queue_index;
    });
    for (unsigned first = 0; first < pending_count;) {
        unsigned       end = first + 1;
        const auto    &q   = pending_orders[command_sort[first]].record;
        const unsigned p = q.owner_and_kind & 15, id = q.unit_index;
        while (end < pending_count) {
            const auto &r = pending_orders[command_sort[end]].record;
            if ((r.owner_and_kind & 15) != p || r.unit_index != id) break;
            ++end;
        }
        const unit    *u         = live_unit(p, id);
        const auto    *cfg       = u ? unit_config(u->unit_proto_id) : nullptr;
        const pending *accepted  = nullptr;
        bool           ambiguous = false;
        if (u && cfg && u->energy > 0) {
            if (auto *h = remember(p, id, false)) {
                const auto entity = [](uint16_t ref, uint16_t slot) {
                    return ref && slot ? ((ref & 0xa0) ? 0x80000000u : 0x40000000u) |
                                             ((ref & 15u) << 16) | slot
                                       : 0u;
                };
                const uint32_t primary   = entity(u->target_ref, u->target_index);
                const uint32_t secondary = entity(u->target2_ref, u->target2_index);
                // Clearing a target has no bind CALL. Revoke its old bundle before unit AI
                // can reacquire the same live entity and make an old token appear current.
                if (h->verb == command::attack &&
                    (h->primary_entity != primary ||
                     h->primary_target_incarnation != entity_incarnation(primary)))
                    h->token = 0;
                if (h->secondary_entity != secondary ||
                    h->secondary_target_incarnation != entity_incarnation(secondary))
                    h->secondary_token = 0;
                if (h->secondary_ref != static_cast<uint16_t>(u->target2_ref) ||
                    h->secondary_id != static_cast<uint16_t>(u->target2_index))
                    h->fired_until = 0;
            }
            const auto after = command_state(*u);
            for (unsigned n = first; n < end; ++n) {
                const auto &o = pending_orders[command_sort[n]];
                if (dual(o.record)) continue; // independent target2 assignment never replaces the primary command
                // Exact bind observers own attack acceptance, including identical reissues.
                if (verb_of(o.record, *cfg) == command::attack) continue;
                if (o.incarnation != mh::storage_policy::hosted::unit_identity(p, id).incarnation || !applied(o.record, *u)) continue;
                if (!primary_changed(o.before, after)) continue;
                if (accepted && !same_command(accepted->record, o.record)) ambiguous = true;
                accepted = &o;
            }
            if (accepted && !ambiguous && !dual(accepted->record)) {
                if (auto *h = remember(p, id, true)) {
                    h->verb  = verb_of(accepted->record, *cfg);
                    h->token = accepted->token;
                    h->goal  = {u->goal_x, u->goal_y};
                    h->home  = {u->home_x, u->home_y};
                    if (h->verb == command::unknown) h->token = 0;
                }
            } else if (ambiguous) {
                if (auto *h = remember(p, id, false)) {
                    h->verb  = command::unknown;
                    h->token = 0;
                }
            }
        }
        first = end;
    }
    // Carry only records the dispatcher REALLY retained, including incarnation. A fresh identical
    // command later is not confused with a completed old command or a wrapped eight-bit sequence.
    std::sort(command_sort, command_sort + pending_count, [](uint16_t a, uint16_t b) {
        return order_less(pending_orders[a].record, pending_orders[b].record);
    });
    retained_count       = 0;
    const unsigned kept  = mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const auto    *queue = reinterpret_cast<const order *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (queue && kept <= queue_capacity)
        for (unsigned n = 0; n < kept; ++n) {
            const order   &q            = queue[n];
            const uint64_t admission_id = order_witness_owner
                                              ? W::metadata_at(*order_witness_owner, W::container::queue, n).admission_id
                                              : 0;
            if (!admission_id) continue;
            auto *found = std::lower_bound(
                command_sort, command_sort + pending_count, q,
                [](uint16_t a, const order &b) { return order_less(pending_orders[a].record, b); });
            // n is the CURRENT compacted slot. Match its actual mapped identity,
            // not the pending row's historical pre-dispatch queue_index.
            while (found != command_sort + pending_count &&
                   same_command(pending_orders[*found].record, q) &&
                   pending_orders[*found].admission_id != admission_id)
                ++found;
            if (found == command_sort + pending_count) continue;
            const auto &o = pending_orders[*found];
            if (!same_command(o.record, q)) continue;
            retained_orders[retained_count++] = {q, o.incarnation, o.token, false, admission_id};
        }
    std::sort(retained_orders, retained_orders + retained_count, [](const retained_order &a, const retained_order &b) {
        return order_less(a.record, b.record);
    });
    pending_count = 0;
}
namespace {

template <typename Record>
void observe_attack_assignment_record(unsigned player, unsigned id,
                                      const Record &q, bool secondary) {
    const unit *u   = live_unit(player, id);
    const auto *cfg = u ? unit_config(u->unit_proto_id) : nullptr;
    if (!u || !cfg || !(u->energy > 0)) return;

    const unsigned actor_kind = q.owner_and_kind & 0xf0;
    if (q.owner_and_kind > 0xff ||
        (actor_kind != 0x20 && actor_kind != 0x80 &&
         actor_kind != 0xa0) ||
        (q.owner_and_kind & 15) != player ||
        q.unit_index != id || dual(q) != secondary)
        return;
    if (!secondary && verb_of(q, *cfg) != command::attack) return;

    auto *h = remember(player, id, true);
    if (!h) return;

    const uint16_t ref =
        static_cast<uint16_t>(secondary ? u->target2_ref : u->target_ref);
    const uint16_t target_id =
        static_cast<uint16_t>(secondary ? u->target2_index : u->target_index);
    const uint32_t entity =
        ref && target_id
            ? ((ref & 0xa0) ? 0x80000000u : 0x40000000u) |
                  ((ref & 15u) << 16) | target_id
            : 0;
    const uint32_t target_life = entity_incarnation(entity);
    const auto     assignment  = attack_assignment_for(q);
    const bool     matching =
        assignment.secondary == secondary &&
        assignment.actor_incarnation == h->incarnation &&
        assignment.entity == entity && target_life &&
        assignment.target_incarnation == target_life;
    const uint64_t token = matching ? assignment.token : 0;

    if (secondary) {
        h->secondary_token              = token;
        h->secondary_entity             = entity;
        h->secondary_target_incarnation = target_life;
    } else {
        h->verb                       = command::attack;
        h->token                      = token;
        h->primary_entity             = entity;
        h->primary_target_incarnation = target_life;
        h->goal                       = {u->goal_x, u->goal_y};
        h->home                       = {u->home_x, u->home_y};
    }
}

template <typename Record>
void observe_deferred_commit_record(unsigned player, unsigned id,
                                    const Record &q, uint64_t admission_id) {
    const unit *u   = live_unit(player, id);
    const auto *cfg = u ? unit_config(u->unit_proto_id) : nullptr;
    if (!u || !cfg || !(u->energy > 0)) return;

    const unsigned actor_kind = q.owner_and_kind & 0xf0;
    if (q.owner_and_kind > 0xff ||
        (actor_kind != 0x20 && actor_kind != 0x80 &&
         actor_kind != 0xa0) ||
        (q.owner_and_kind & 15) != player ||
        q.unit_index != id || dual(q) || !applied(q, *u, false))
        return;

    if (verb_of(q, *cfg) == command::attack) {
        observe_attack_assignment_record(player, id, q, false);
        return;
    }

    auto *h = remember(player, id, true);
    if (!h) return;

    auto *at = std::lower_bound(
        retained_orders, retained_orders + retained_count, q,
        [](const retained_order &a, const Record &b) {
            return order_less(a.record, b);
        });
    uint64_t token = 0;
    bool     found = false, ambiguous = false;
    for (; at != retained_orders + retained_count &&
           same_command(at->record, q);
         ++at) {
        if (!admission_id || at->admission_id != admission_id ||
            at->consumed || at->incarnation != h->incarnation)
            continue;
        if (found && token != at->token) ambiguous = true;
        token        = at->token;
        found        = true;
        at->consumed = true;
    }

    h->verb  = verb_of(q, *cfg);
    h->token = ambiguous || h->verb == command::unknown ? 0 : token;
    h->goal  = {u->goal_x, u->goal_y};
    h->home  = {u->home_x, u->home_y};
}

} // namespace

void observe_attack_assignment(unsigned player, unsigned id,
                               unsigned queue_index, bool secondary) {
    const unsigned count =
        mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const auto *queue = reinterpret_cast<const order *>(
        mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (!queue || queue_index >= count) return;
    observe_attack_assignment_record(player, id, queue[queue_index], secondary);
}

void observe_deferred_commit(unsigned player, unsigned id,
                             unsigned queue_index) {
    const unsigned count =
        mh::caps::orders::live_count(mh::caps::orders::QUEUE);
    const auto *queue = reinterpret_cast<const order *>(
        mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (!queue || queue_index >= count) return;
    const uint64_t admission_id = order_witness_owner
                                      ? W::metadata_at(*order_witness_owner, W::container::queue, queue_index).admission_id
                                      : 0;
    observe_deferred_commit_record(player, id, queue[queue_index], admission_id);
}

void observe_attack_assignment(
    const libmh_order_observation &observation) {
    if (observation.kind != LIBMH_EVK_ATTACK_TARGET_ASSIGNED ||
        (observation.flags & LIBMH_ORDER_OBSERVATION_CONTAINER_MASK) !=
            LIBMH_ORDER_OBSERVATION_QUEUE)
        return;

    observe_attack_assignment_record(
        observation.owner_and_kind & 15u, observation.unit_index,
        observation,
        (observation.flags & LIBMH_ORDER_OBSERVATION_SECONDARY) != 0);
}

void observe_deferred_commit(
    const libmh_order_observation &observation) {
    if (observation.kind != LIBMH_EVK_UNIT_ORDER_COMMITTED ||
        (observation.flags & LIBMH_ORDER_OBSERVATION_CONTAINER_MASK) !=
            LIBMH_ORDER_OBSERVATION_QUEUE ||
        (observation.flags & LIBMH_ORDER_OBSERVATION_SECONDARY) != 0)
        return;

    observe_deferred_commit_record(
        observation.owner_and_kind & 15u, observation.unit_index,
        observation, observation.admission_id);
}
unsigned collect(const world_view &v, sv::world_units &mapped, unsigned player, const uint16_t *ids,
                 unsigned number_ids, unsigned selected_building, intent *out, unsigned capacity) {
    if (!out || !capacity || v.span_x <= 0 || v.span_y <= 0 || player >= mh::caps::g(mh::caps::UNITS).players) return 0;
    check_world(v);
    collector      c{v, mapped, out, 0, std::min(capacity, intent_capacity)};
    uint16_t       sorted[selection_capacity];
    const unsigned n = ids ? std::min(number_ids, selection_capacity) : 0;
    std::copy_n(ids, n, sorted);
    std::sort(sorted, sorted + n);
    for (unsigned i = 0; i < n; ++i)
        if (sorted[i] && (!i || sorted[i] != sorted[i - 1])) c.unit_intents(player, sorted[i]);
    if (selected_building) c.building_intents(player, selected_building);
    return c.count;
}
namespace {
route_reader      canonical_route_reader = nullptr;
intent            legacy_route_intents[intent_capacity];
route_snapshot    canonical_snapshots[selection_capacity];
route_read_result canonical_results[selection_capacity];
uint16_t          canonical_actors[selection_capacity];
unsigned          canonical_actor_count = 0;
line_role         role_for(kind type) {
    if (type == kind::attack || type == kind::secondary) return line_role::attack;
    if (type == kind::patrol || type == kind::patrol_home) return line_role::patrol;
    if (type == kind::rally) return line_role::rally;
    return line_role::movement;
}
void legacy_path(const intent &i, route_path &path) {
    path.count                   = 0;
    path.cycle_begin             = no_cycle;
    path.cycle_phase             = 0;
    path.generation              = 0;
    path.canonical               = false;
    path.nodes[0]                = {};
    path.source                  = i.source;
    path.origin                  = i.origin;
    path.ground_origin           = i.group_origin;
    path.actor                   = i.source_id;
    path.actor_life              = i.incarnation;
    path.family                  = i.building_source ? source_family::building : source_family::unit;
    path.player                  = i.player;
    path.mover                   = i.mover;
    path.source_visible          = i.visible;
    path.count                   = 1;
    path.nodes[0].visual         = i.destination;
    path.nodes[0].ground         = i.ground_destination;
    path.nodes[0].command.legacy = i.command_token;
    path.nodes[0].entity         = i.entity;
    path.nodes[0].target_life    = i.target_incarnation;
    path.nodes[0].grouping_life  = i.target_incarnation;
    path.nodes[0].role           = role_for(i.type);
    path.nodes[0].known          = true;
}
bool owned_primary(unsigned player, unsigned id) {
    for (unsigned n = 0; n < canonical_actor_count; ++n)
        if (canonical_actors[n] == id && player < history_players && canonical_results[n] != route_read_result::unowned) return true;
    return false;
}
bool snapshot_shape(const route_snapshot &s, uint32_t life) {
    if (s.actor_life != life || !s.generation || s.count > explicit_route_capacity) return false;
    if (s.cycle_begin != no_cycle && (s.cycle_begin >= s.count ||
                                      s.cycle_cursor < s.cycle_begin || s.cycle_cursor >= s.count)) return false;
    if (s.implicit_leg_active && !s.has_implicit_anchor) return false;
    if (s.has_implicit_anchor && (s.cycle_begin != 0 || (s.count != 1 && !s.implicit_leg_active))) return false;
    for (unsigned j = 0; j < s.count; ++j) {
        const auto &node = s.nodes[j];
        if (node.role != line_role::movement && node.role != line_role::attack && node.role != line_role::patrol) return false;
        if (s.cycle_begin != no_cycle && (node.role == line_role::patrol) != (j >= s.cycle_begin)) return false;
        if (!node.command.canonical || !node.command.world) return false;
    }
    return true;
}
route_node resolve_route_node(const world_view &v, sv::world_units &mapped, const route_snapshot_node &s) {
    route_node out;
    out.command = s.command;
    out.role    = s.role;
    if (s.role == line_role::attack) {
        if (!s.target_current || !s.target_ref || !s.target_slot || !s.target_life) return out;
        const unsigned family = s.target_ref & 0xf0;
        out.entity            = (family == 0x40 ? 0x40000000u : 0x80000000u) | ((s.target_ref & 15u) << 16) | s.target_slot;
        out.target_life       = s.target_life;
        out.life_domain       = target_life_domain::canonical;
        uint32_t resolved     = 0;
        if (!entity_anchor(v, mapped, s.target_ref, s.target_slot, out.visual, out.ground, resolved) || resolved != out.entity) return out;
        out.grouping_life = entity_incarnation(out.entity);
        out.known         = true;
    } else if (ground(v, s.x, s.y, out.visual)) {
        out.ground = out.visual;
        out.known  = true;
    }
    return out;
}
double visual_distance(const world_view &v, point a, point b) {
    return std::hypot(double(delta(a.x, b.x, v.span_x)), double(delta(a.y, b.y, v.span_y)));
}
} // namespace
void set_route_reader(route_reader reader) { canonical_route_reader = reader; }
void collect_routes(const world_view &v, sv::world_units &mapped, unsigned player,
                    const uint16_t *ids, unsigned number_ids, unsigned building, route_collection &out) {
    out.count                   = 0;
    const unsigned legacy_count = collect(v, mapped, player, ids, number_ids, building,
                                          legacy_route_intents, intent_capacity);
    canonical_actor_count       = 0;
    if (canonical_route_reader && ids && player < history_players) {
        const unsigned count = std::min(number_ids, selection_capacity);
        for (unsigned n = 0; n < count; ++n) {
            const unsigned id = ids[n];
            // Selection can contain duplicates; canonical routes are read once.
            bool duplicate = false;
            for (unsigned j = 0; j < canonical_actor_count; ++j) duplicate = duplicate || canonical_actors[j] == id;
            if (duplicate || !live_unit(player, id)) continue;
            const uint32_t life = mh::storage_policy::hosted::unit_identity(player, id).incarnation;
            if (!life) continue;
            const unsigned at    = canonical_actor_count++;
            canonical_actors[at] = static_cast<uint16_t>(id);
            auto &snapshot       = canonical_snapshots[at];
            // The reader writes every field of its bounded owned-value result.
            // Local DRAW precedence only. Keep the truthful canonical owner and
            // all task state intact until its actual safe application boundary.
            // collect() already supplies this exact latest ordinary endpoint or
            // incompatible-primary suppression, with independent secondary fire.
            if (pending_ordinary_primary(player, id, life)) {
                canonical_results[at] = route_read_result::unowned;
                continue;
            }
            canonical_results[at] = canonical_route_reader(player, id, life, snapshot);
            if (canonical_results[at] == route_read_result::owned && !snapshot_shape(snapshot, life))
                canonical_results[at] = route_read_result::invalid;
        }
    }
    for (unsigned i = 0; i < legacy_count; ++i) {
        const auto &item  = legacy_route_intents[i];
        const bool  owned = !item.building_source && item.type != kind::secondary && item.type != kind::rally &&
                           owned_primary(item.player, item.source_id);
        if (owned && !item.also_secondary) continue;
        // Genuine legacy patrol already has the proven ground/provenance guard
        // in collector::unit_intents. Preserve its actual goal/home, not an
        // inferred aircraft opcode or reconstructed original click history.
        if (item.type == kind::patrol_home && i && legacy_route_intents[i - 1].type == kind::patrol &&
            legacy_route_intents[i - 1].source_id == item.source_id && legacy_route_intents[i - 1].player == item.player) continue;
        auto &path = out.paths[out.count++];
        legacy_path(item, path);
        if (owned) path.nodes[0].command.legacy = item.secondary_command_token;
        if (item.type == kind::patrol && i + 1 < legacy_count) {
            const auto &home = legacy_route_intents[i + 1];
            if (home.type == kind::patrol_home && home.source_id == item.source_id && home.player == item.player) {
                path.nodes[1]                = path.nodes[0];
                path.nodes[1].visual         = home.destination;
                path.nodes[1].ground         = home.ground_destination;
                path.nodes[1].command.legacy = home.command_token;
                path.count                   = 2;
                path.cycle_begin             = 0;
                // Anchor at a stable endpoint ordering, not whichever endpoint is
                // currently goal: native patrol swaps goal/home each circuit leg.
                const point a = path.nodes[0].ground, b = path.nodes[1].ground;
                if (a.x > b.x || (a.x == b.x && a.y > b.y))
                    path.cycle_phase = visual_distance(v, path.nodes[1].visual, path.nodes[0].visual);
            }
        }
    }
    for (unsigned n = 0; n < canonical_actor_count; ++n) {
        if (canonical_results[n] != route_read_result::owned) continue;
        const auto &s = canonical_snapshots[n];
        if (!s.count) continue;
        const unsigned      id = canonical_actors[n];
        point               origin, ground_origin;
        sv::unit_projection projection;
        if (!unit_anchor(v, mapped, player, id, origin, &projection, &ground_origin)) continue;
        const auto *u   = live_unit(player, id);
        const auto *cfg = u ? unit_config(u->unit_proto_id) : nullptr;
        if (!cfg) continue;
        auto &path            = out.paths[out.count++];
        path.count            = 0;
        path.cycle_begin      = no_cycle;
        path.cycle_phase      = 0;
        path.source           = projection.anchor;
        path.origin           = origin;
        path.ground_origin    = ground_origin;
        path.actor            = static_cast<uint16_t>(id);
        path.actor_life       = s.actor_life;
        path.generation       = s.generation;
        path.family           = source_family::unit;
        path.player           = static_cast<uint8_t>(player);
        path.mover            = static_cast<uint8_t>(cfg->move_op_arg);
        path.canonical        = true;
        path.source_visible   = sv::visible_body(v, projection);
        const unsigned prefix = s.cycle_begin == no_cycle ? s.count : s.cycle_begin;
        for (unsigned j = 0; j < prefix; ++j) path.nodes[path.count++] = resolve_route_node(v, mapped, s.nodes[j]);
        if (s.cycle_begin == no_cycle) continue;
        path.cycle_begin = path.count;
        // An append during the implicit return preserves that one in-flight leg,
        // then removes its anchor from the new explicit circuit.
        const bool     transient_anchor = s.has_implicit_anchor && s.count > 1;
        const unsigned cycle_cursor     = transient_anchor ? (s.cycle_cursor + 1) % s.count : s.cycle_cursor;
        if (s.has_implicit_anchor) {
            route_node anchor;
            anchor.role    = line_role::patrol;
            anchor.command = s.nodes[s.cycle_cursor].command;
            anchor.known   = ground(v, s.implicit_x, s.implicit_y, anchor.visual);
            anchor.ground  = anchor.visual;
            if (transient_anchor) {
                path.nodes[path.count++] = anchor;
            } else {
                const route_node destination = resolve_route_node(v, mapped, s.nodes[prefix]);
                if (s.implicit_leg_active) {
                    path.nodes[path.count++] = anchor;
                    path.nodes[path.count++] = destination;
                    if (anchor.known && destination.known) path.cycle_phase = visual_distance(v, destination.visual, anchor.visual);
                } else {
                    path.nodes[path.count++] = destination;
                    path.nodes[path.count++] = anchor;
                }
            }
        }
        if (!s.has_implicit_anchor || transient_anchor) {
            path.cycle_begin = path.count;
            // Rotate the whole suffix, never discard visited circuit nodes.
            const unsigned length = s.count - prefix;
            for (unsigned j = prefix; j < cycle_cursor; ++j) {
                const auto a = resolve_route_node(v, mapped, s.nodes[j]);
                const auto b = resolve_route_node(v, mapped, s.nodes[j + 1]);
                if (a.known && b.known) path.cycle_phase += visual_distance(v, a.visual, b.visual);
            }
            for (unsigned j = 0; j < length; ++j) {
                const unsigned index     = prefix + (cycle_cursor - prefix + j) % length;
                path.nodes[path.count++] = resolve_route_node(v, mapped, s.nodes[index]);
            }
        }
    }
}

} // namespace mh::ui::selected_targets::detail
