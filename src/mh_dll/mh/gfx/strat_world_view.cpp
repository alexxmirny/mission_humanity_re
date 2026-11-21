#include "gfx/strat_world_view.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "addr/mh_addrs.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "caps/caps.h"
#include "gfx/wrap_window.h"
#include "include/mh_mapzoom_export.h"
#include "include/mh_viewwrap_export.h"
#include "state/region_runtime.h"

namespace mh::gfx::stratview {
namespace {
using namespace mh::game;
using namespace mh::state;

template <class T>
T load(uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
    return value;
}
int mod(int x, int span) {
    const int r = x % span;
    return r < 0 ? r + span : r;
}
int axis(int camera, int fine, int span, int viewport, bool biased, int floor) {
    return biased ? wrapwin::biased_axis(camera, fine, span, wrapwin::bias_for(span, viewport, floor))
                  : wrapwin::stock_axis(camera, fine, span);
}
point place(const world_view &v, int x, int y, bool biased) {
    return {axis(v.camera_x, x, v.span_x, v.width, biased, wrapwin::K_X),
            axis(v.camera_y, y, v.span_y, v.height, biased, wrapwin::K_Y)};
}
bool fog(const world_view &, int x, int y) {
    const auto *tiles = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    // Renderers test the adjusted fine coordinate directly, not the unit's nominal tile.
    return tiles && x >= 0 && y >= 0 && x < 8192 && y < 8192 && tiles[((x >> 5) << 8) | (y >> 5)].visibility != 0;
}
bool sprite_rect(int sprite, point dst, rect &out) {
    if (sprite < 0 || sprite >= 22000) return false;
    const uintptr_t pixels = load<uint32_t>(mh::addr::_G_LLM_GFX_BANK_PIXELS);
    if (!pixels) return false;
    const uintptr_t header = pixels + load<uint32_t>(mh::addr::_G_LLM_SPRITE_PIX_OFFSETS + sprite * 4);
    const int       w = load<uint16_t>(header + 8), h = load<uint16_t>(header + 10);
    if (!w || !h) return false;
    out.left   = dst.x + load<uint16_t>(header + 4);
    out.top    = dst.y + load<uint16_t>(header + 6);
    out.right  = out.left + w;
    out.bottom = out.top + h;
    return true;
}
void unite(rect &a, const rect &b) {
    a.left   = std::min(a.left, b.left);
    a.top    = std::min(a.top, b.top);
    a.right  = std::max(a.right, b.right);
    a.bottom = std::max(a.bottom, b.bottom);
}
const mh_gfx_sprite_meta *meta(int sprite) {
    if (sprite < 0 || sprite >= 22000) return nullptr;
    return reinterpret_cast<const mh_gfx_sprite_meta *>(mh::addr::_G_LLM_SPRITE_META) + sprite;
}
bool body_sprite(const world_view &v, int sprite, int x, int y, bool biased, unit_projection &out) {
    const auto *m = meta(sprite);
    if (!m) return false;
    const point dst = place(v, x - m->origin_x, y - m->origin_y, biased);
    if (!sprite_rect(sprite, dst, out.body)) return false;
    out.anchor = {dst.x + m->origin_x, dst.y + m->origin_y};
    out.sprite = sprite;
    return true;
}
// 46185b / cfg_GetAnimTime: pure linked-frame lookup, no native draw or RNG calls.
int animation_sprite(int start, double progress) {
    const auto    *anims = ptr<const mh_cfg_final_struct_Anim>(RID_ANIM);
    const unsigned cap   = REGIONS[RID_ANIM].size / sizeof(*anims);
    if (!anims || start < 0 || static_cast<unsigned>(start + 1) >= cap) return -1;
    int      at       = start;
    double   duration = 0.0;
    unsigned visited  = 0;
    for (;;) {
        if (at < 0 || static_cast<unsigned>(at + 1) >= cap || ++visited > cap) return -1;
        const auto &a = anims[at + 1];
        duration += a.time;
        if (!a.next) break;
        at += a.next;
    }
    at             = start;
    double elapsed = 0.0;
    visited        = 0;
    for (;;) {
        if (at < 0 || static_cast<unsigned>(at + 1) >= cap || ++visited > cap) return -1;
        const auto &a = anims[at + 1];
        if (!a.next || elapsed + a.time > duration * progress) return a.sprite_id;
        elapsed += a.time;
        at += a.next;
    }
}
bool mapped_body(const world_view &v, unsigned player, const mh_map_object_unit &u,
                 const mh_cfg_final_struct_Unit &cfg, unit_projection &out) {
    // 45229a's 0x80 tile branch uses tile offsets, not pixel_delta_wrapped.
    const int cx = *ptr<const int32_t>(RID_MAP_CAM_COL), cy = *ptr<const int32_t>(RID_MAP_CAM_ROW);
    int       tx = mod(static_cast<int>(u.x) - cx, v.map_width);
    int       ty = mod(static_cast<int>(u.y) - cy, v.map_height);
    if (tx >= v.width / 32 + 2) tx -= v.map_width;
    if (ty >= v.height / 32 + 1) ty -= v.map_height;
    const bool  drawn     = tx >= -1 && tx <= v.width / 32 + 1 && ty >= -1 && ty < v.height / 32 + 1;
    const auto *signs     = reinterpret_cast<const mh_llm_vec2i *>(0x00ae4768u);
    const auto &sign      = signs[u.facing_target];
    const int   remaining = 31 - u.move_microstep;
    const point base      = {tx * 32 + sign.dx * remaining, ty * 32 + sign.dy * remaining};
    out.anchor            = {base.x + 16, base.y + 16};
    out.tile_visible      = fog(v, u.x * 32, u.y * 32);
    // Keep a live offscreen endpoint without inventing a selectable body outside45229a's tile gate.
    if (!drawn) return true;
    if (cfg.soldier_count > 0) {
        const unsigned planet = *ptr<const int32_t>(RID_G_PLANET_INDEX);
        if (planet >= 32 || player >= mh::caps::g(mh::caps::SOLDIERS).players) return false;
        const auto *planets = ptr<const mh_cfg_final_struct_Planet>(RID_PLANETS);
        uint16_t    soldier;
        std::memcpy(&soldier, u.unit_above, sizeof(soldier));
        bool     have    = false;
        unsigned visited = 0;
        while (soldier) {
            if (soldier >= mh::caps::g(mh::caps::SOLDIERS).limit || ++visited >= mh::caps::g(mh::caps::SOLDIERS).limit) return false;
            const auto &s = *mh::caps::rec<mh_llm_strat_crew_soldier>(mh::caps::SOLDIERS, player, soldier);
            // 45229a loads the old byte for the frame, then increments the live byte.
            const unsigned anim_count = s.walk_duration != 0.0 ? static_cast<uint8_t>(s.anim_change_count - 1) : s.anim_change_count;
            const int      sprite     = cfg.sprite + ((static_cast<int>(s.sprite_frame) % 24) / 3) * 8 +
                               ((static_cast<int>(anim_count) + u.move_microstep) / 2) % 8 +
                               planets[planet].soldier_sprite_bank_offset;
            const auto *m = meta(sprite);
            rect        r;
            if (!m || !sprite_rect(sprite, {base.x + s.cur_x - m->origin_x, base.y + s.cur_y - m->origin_y}, r)) return false;
            if (have) unite(out.body, r);
            else {
                out.body   = r;
                out.sprite = sprite;
                have       = true;
            }
            soldier = s.next_soldier;
        }
        return have;
    }
    int sprite = cfg.sprite + u.facing_target - 1;
    if (cfg.type == 11 || cfg.type == 12)
        sprite = cfg.sprite + load<int32_t>(0x00ae1df8u + u.facing_target * 128 + u.move_microstep * 4);
    const auto *m = meta(sprite);
    if (!m || !sprite_rect(sprite, {out.anchor.x - m->origin_x, out.anchor.y - m->origin_y}, out.body)) return false;
    out.sprite = sprite;
    if (cfg.independent) {
        const int   attachment = cfg.sprite + u.facing_current + 23;
        const auto *am         = meta(attachment);
        rect        r;
        if (!am || !sprite_rect(attachment, {out.anchor.x - m->origin_x + m->submount_x - am->origin_x, out.anchor.y - m->origin_y + m->submount_y - am->origin_y}, r)) return false;
        unite(out.body, r);
    }
    return true;
}

bool read_body_phases(world_view &v) {
    const auto &geometry = mh::caps::g(mh::caps::UNITS);
    const auto *cfg      = ptr<const mh_cfg_final_struct_Unit>(RID_UNIT);
    const auto *tiles    = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    const auto *steps    = reinterpret_cast<const mh_llm_strat_move_microstep *>(mh::addr::_G_LLM_STRAT_MOVE_MICROSTEPS);
    if (!cfg || !tiles || !geometry.base || geometry.players > 8 || geometry.limit > 4096) return false;
    const int cx = *ptr<const int32_t>(RID_MAP_CAM_COL), cy = *ptr<const int32_t>(RID_MAP_CAM_ROW);
    unsigned  phase = load<uint8_t>(0x005d0c54u) & 1;
    // 44e7a8 initializes PHASE from PARITY, then the 453002 pass walks exactly this
    // row/descending-column/stack order. Reconstruct without toggling game memory.
    for (int row = -3; row < v.height / 32 + 5; ++row) {
        for (int col = v.width / 32 + 2; col >= -2; --col) {
            uint16_t link;
            std::memcpy(&link, tiles[(mod(cx + col, v.map_width) << 8) | mod(cy + row, v.map_height)].unit, sizeof(link));
            unsigned visited = 0;
            while (link) {
                const unsigned player = link >> 12, id = link & 4095;
                if (player >= geometry.players || !id || id >= geometry.limit || ++visited > geometry.limit * geometry.players) return false;
                const auto &u = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, id);
                if (!u.unit_proto_id || u.unit_proto_id >= REGIONS[RID_UNIT].size / sizeof(*cfg)) return false;
                const auto &c = cfg[u.unit_proto_id];
                if (c.sprite_type == 1) {
                    bool toggles = u.state == 21 || u.state == 22;
                    if (!toggles) {
                        if (u.move_heading >= 24 || u.move_microstep < 0 || u.move_microstep >= 32) return false;
                        const auto &s = steps[u.move_heading * 32 + u.move_microstep];
                        toggles       = fog(v, u.x * 32 + s.x_off, mod(u.y * 32 + s.y_off - u.elevation, v.span_y));
                    }
                    if (toggles) {
                        const uint32_t bit  = 1u << (id & 31);
                        auto          &word = v.body_phase[player][id >> 5];
                        if (phase) word |= bit;
                        else word &= ~bit;
                        phase ^= 1;
                    }
                }
                std::memcpy(&link, u.unit_above, sizeof(link));
            }
        }
    }
    return true;
}
} // namespace

bool read_native_crop(crop &out) {
    out            = {};
    const int    w = *ptr<const int32_t>(RID_G_WIN_W), h = *ptr<const int32_t>(RID_G_WIN_H);
    const double sx = load<double>(mh::addr::_G_LLM_MAP_ZOOM_SCALE_X), sy = load<double>(0x005d8b0cu);
    if (w <= 0 || h <= 0 || !std::isfinite(sx) || !std::isfinite(sy) || sx < 1.0 || sy < 1.0 || sx >= 8.0 || sy >= 8.0) return false;
    int cw = w, ch = h;
    if (sx != 1.0 || sy != 1.0) {
        cw = static_cast<int>(w / sx) & ~1;
        ch = static_cast<int>(h / sy) & ~1;
    }
    if (cw <= 0 || ch <= 0) return false;
    out = {(w - cw) / 2, (h - ch) / 2, cw, ch};
    return true;
}

bool read_world_view(world_view &out, const crop *native_fallback) {
    out              = {};
    const auto *geom = ptr<const mh_llm_strat_map_geom>(RID_GENERAL);
    if (!geom) return false;
    out.width      = *ptr<const int32_t>(RID_G_WIN_W);
    out.height     = *ptr<const int32_t>(RID_G_WIN_H);
    out.map_width  = *ptr<const int32_t>(RID_WIDTH);
    out.map_height = *ptr<const int32_t>(RID_HEIGHT);
    out.span_x     = static_cast<int>(geom->big_width);
    out.span_y     = static_cast<int>(geom->big_height);
    out.camera_x   = load<int32_t>(0x00e15e38u);
    out.camera_y   = load<int32_t>(0x00e15e3cu);
    if (out.width <= 0 || out.height <= 0 || out.map_width <= 0 || out.map_width > 256 || out.map_height <= 0 || out.map_height > 256 ||
        out.span_x != out.map_width * 32 || out.span_y != out.map_height * 32) return false;
    if (!MH_MapZoom_ReadCrop(&out.shown.left, &out.shown.top, &out.shown.width, &out.shown.height)) {
        if (!native_fallback) return false;
        out.shown = *native_fallback;
    }
    const auto &c = out.shown;
    if (c.width <= 0 || c.height <= 0 || c.left < 0 || c.top < 0 || c.left + c.width > out.width || c.top + c.height > out.height) return false;
    out.biased_wrap = MH_ViewWrap_IsBiased(0x004502e9u) != 0;
    return read_body_phases(out);
}

bool world_units::contains(const world_view &view, unsigned player, unsigned unit_id) {
    const auto *tiles = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    const auto &units = mh::caps::g(mh::caps::UNITS);
    if (!tiles || !units.base || units.players > 8 || units.limit > 4096 || player >= units.players ||
        !unit_id || unit_id >= units.limit || view.map_width <= 0 || view.map_width > 256 ||
        view.map_height <= 0 || view.map_height > 256) return false;
    const auto    &queried = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, unit_id);
    const unsigned x = queried.x, y = queried.y;
    if (x >= static_cast<unsigned>(view.map_width) || y >= static_cast<unsigned>(view.map_height)) return false;
    const unsigned tile_id  = (x << 8) | y;
    const uint32_t tile_bit = 1u << (tile_id & 31);
    if (!(checked_tiles[tile_id >> 5] & tile_bit)) {
        checked_tiles[tile_id >> 5] |= tile_bit;
        const auto &t = tiles[tile_id];
        uint16_t    link;
        std::memcpy(&link, t.unit, sizeof(link));
        while (link) {
            const unsigned owner = link >> 12, id = link & 4095;
            if (owner >= units.players || !id || id >= units.limit || (bits[owner][id >> 5] & (1u << (id & 31)))) break;
            const auto &u = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, owner, id);
            if (u.x != x || u.y != y) break;
            bits[owner][id >> 5] |= 1u << (id & 31);
            std::memcpy(&link, u.unit_above, sizeof(link));
        }
        const unsigned owner = t.class_owner & 15, id = t.building;
        if ((t.class_owner & 0xf0) == 0x80 && owner < units.players && id > 0 && id < units.limit) {
            const auto &u = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, owner, id);
            if (u.x == x && u.y == y) bits[owner][id >> 5] |= 1u << (id & 31);
        }
    }
    return (bits[player][unit_id >> 5] & (1u << (unit_id & 31))) != 0;
}

point project_fine(const world_view &view, int fine_x, int fine_y) {
    return place(view, fine_x, fine_y, view.biased_wrap);
}

bool project_unit(const world_view &v, unsigned player, unsigned unit_id, unit_projection &out) {
    out                  = {};
    const auto &geometry = mh::caps::g(mh::caps::UNITS);
    if (!geometry.base || player >= geometry.players || player >= 8 || !unit_id || unit_id >= geometry.limit || unit_id >= 4096) return false;
    const auto &u       = *mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, unit_id);
    const auto *configs = ptr<const mh_cfg_final_struct_Unit>(RID_UNIT);
    if (!configs || !u.unit_proto_id || u.unit_proto_id > load<uint32_t>(mh::addr::G_UNIT_COUNT_TOTAL) ||
        u.unit_proto_id >= REGIONS[RID_UNIT].size / sizeof(*configs) || !(u.energy > 0) || (u.state >= 2 && u.state <= 4) ||
        u.x >= v.map_width || u.y >= v.map_height || u.move_microstep < 0 || u.move_microstep >= 32 ||
        u.facing_target < 1 || u.facing_target > 24) return false;
    const auto &cfg   = configs[u.unit_proto_id];
    const auto *tiles = ptr<const mh_map_tile_object_data>(RID_TILE_OBJECTS);
    const auto &tile  = tiles[(u.x << 8) | u.y];
    if (tile.building == unit_id && tile.class_owner == (0x80 | player)) return mapped_body(v, player, u, cfg, out);
    if (u.move_heading >= 24) return false;
    const auto *steps = reinterpret_cast<const mh_llm_strat_move_microstep *>(mh::addr::_G_LLM_STRAT_MOVE_MICROSTEPS);
    const auto &step  = steps[u.move_heading * 32 + u.move_microstep];
    int         x = u.x * 32 + step.x_off, y = mod(u.y * 32 + step.y_off - u.elevation, v.span_y);
    const int   phase  = cfg.sprite_type == 1 ? static_cast<int>((v.body_phase[player][unit_id >> 5] >> (unit_id & 31)) & 1) * 24 : 0;
    int         sprite = cfg.sprite + ((cfg.type >= 19 && cfg.type <= 22) ? 0 : u.facing_target - 1) + phase;
    bool        biased = v.biased_wrap;
    if (u.state == 21 || u.state == 22) {
        if (player >= mh::caps::g(mh::caps::UNIT_STORAGE).players || u.home_storage_slot >= mh::caps::g(mh::caps::UNIT_STORAGE).limit || cfg.elevation == 0) return false;
        const auto &storage = *mh::caps::rec<mh_map_object_unit_storage>(mh::caps::UNIT_STORAGE, player, u.home_storage_slot);
        if (storage.b_index <= 0 || static_cast<unsigned>(storage.b_index) >= mh::caps::g(mh::caps::BUILDINGS).limit) return false;
        const auto &b  = *mh::caps::rec<mh_map_object_building>(mh::caps::BUILDINGS, player, storage.b_index);
        const auto *bc = ptr<const mh_cfg_final_struct_Building>(RID_BUILDING);
        if (b.building_id >= REGIONS[RID_BUILDING].size / sizeof(*bc)) return false;
        const float ratio = static_cast<float>(cfg.elevation - u.elevation) / static_cast<float>(cfg.elevation);
        x                 = mod(x + static_cast<int>(bc[b.building_id].dock_lift_offset_x * static_cast<double>(ratio)) +
                                    static_cast<int>(u.elevation * load<double>(0x00500ff2u)) + 1,
                                v.span_x);
        y                 = u.y * 32 + step.y_off + static_cast<int>(bc[b.building_id].dock_lift_offset_y * static_cast<double>(ratio)) + 1;
        x                 = mod(x - static_cast<int>(u.elevation * load<double>(0x00500ffau)) + 1, v.span_x);
        y                 = mod(y - u.elevation + 1, v.span_y);
        sprite            = cfg.sprite + u.facing_target - 1 + phase;
        out.tile_visible  = fog(v, x, y);
        biased            = MH_ViewWrap_IsBiased(0x00451223u) != 0;
    } else {
        out.tile_visible = fog(v, x, y);
        if (u.state == 23 || u.state == 124) {
            const auto *bc = ptr<const mh_cfg_final_struct_Building>(RID_BUILDING);
            if (cfg.elevation == 0 || cfg.equivalent < 0 || static_cast<unsigned>(cfg.equivalent) >= REGIONS[RID_BUILDING].size / sizeof(*bc) || u.move_heading >= 8) return false;
            const auto     &b       = bc[cfg.equivalent];
            const float     ratio   = static_cast<float>(cfg.elevation - u.elevation) / static_cast<float>(cfg.elevation);
            const unsigned  offset  = u.state == 23 ? 0x796 : 0x7d6;
            const uintptr_t address = reinterpret_cast<uintptr_t>(&b) + offset + u.move_heading * 4;
            x += static_cast<int>(load<int32_t>(address) * static_cast<double>(ratio));
            y += static_cast<int>(load<int32_t>(address + 32) * static_cast<double>(ratio));
            if (cfg.type == 19 || cfg.type == 20 || cfg.type == 21) {
                int start;
                std::memcpy(&start, b.anim + (cfg.type == 21 ? 8 : 2) * 4, sizeof(start));
                sprite = animation_sprite(start, ratio);
            }
        }
    }
    if (!body_sprite(v, sprite, x, y, biased, out)) return false;
    return true;
}

bool visible_body(const world_view &view, const unit_projection &unit) {
    const auto &b = unit.body;
    const auto &c = view.shown;
    return unit.tile_visible && b.right > b.left && b.bottom > b.top && b.right > c.left && b.bottom > c.top &&
           b.left < c.left + c.width && b.top < c.top + c.height && b.right > 0 && b.bottom > 0 && b.left < view.width && b.top < view.height;
}

point prezoom_to_display(const world_view &view, point p) {
    return {static_cast<int>(static_cast<int64_t>(p.x - view.shown.left) * view.width / view.shown.width),
            static_cast<int>(static_cast<int64_t>(p.y - view.shown.top) * view.height / view.shown.height)};
}

} // namespace mh::gfx::stratview
