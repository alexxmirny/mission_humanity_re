//! The launcher's look (dist RL12): the game's palette on egui's stock widgets.
//!
//! Two layers, deliberately separate:
//!
//! * `apply(ctx)` sets egui's `Visuals`/`Style` so every STOCK widget (buttons, text edits, combo
//!   boxes, scroll bars, checkboxes) already looks like the game's phosphor-green menu. The settings
//!   renderer uses only stock widgets and gets the theme for free.
//! * The painters (`paint_*`) draw what no stock widget does: the starfield window background, the
//!   phosphor grid behind a panel, the hazard-stripe title bar and the riveted metal frame. The grid
//!   and hazard tiles are 16x16 textures generated here at startup (so the binary carries no game
//!   art), sampled with `Repeat` + nearest filtering and drawn as ONE quad each via UVs that run
//!   0..rect/16 -- the same trick the mockup's CSS repeating gradients play.
//!
//! Nothing in this module reads a game file. The optional runtime art from the player's own
//! `mh.rsr` (RL12 WU-D) will replace `paint_background`'s output, never this palette.
#![allow(dead_code)]

use egui::{
    epaint::{Mesh, Shape, StrokeKind},
    pos2, vec2, Color32, Context, CornerRadius, FontFamily, FontId, Id, Margin, Painter, Rect,
    Stroke, TextStyle, TextureHandle, TextureOptions, ThemePreference, Visuals,
};

// ---- palette (mockup CSS variables) --------------------------------------------------------------

pub const SPACE: Color32 = Color32::from_rgb(0x00, 0x00, 0x00);
pub const BTN: Color32 = Color32::from_rgb(0x0a, 0x0f, 0x0a);
pub const GRID: Color32 = Color32::from_rgb(0x00, 0x28, 0x08);
pub const GRIDLINE: Color32 = Color32::from_rgb(0x0b, 0x4a, 0x14);
pub const LINE: Color32 = Color32::from_rgb(0x39, 0x71, 0x39);
pub const LINE_HI: Color32 = Color32::from_rgb(0x84, 0xff, 0x00);
pub const TEXT: Color32 = Color32::from_rgb(0x63, 0xe3, 0x63);
pub const TEXT_DIM: Color32 = Color32::from_rgb(0x5a, 0x86, 0x4a);
pub const TEXT_HI: Color32 = Color32::from_rgb(0xd8, 0xff, 0xc0);
pub const LINK: Color32 = Color32::from_rgb(0xa6, 0xff, 0x7a);
pub const BOX_BG: Color32 = Color32::from_rgb(0x00, 0x1a, 0x05);
pub const SEL_BG: Color32 = Color32::from_rgb(0x0c, 0x3a, 0x0c);
pub const PLAY_TOP: Color32 = Color32::from_rgb(0x0f, 0x2a, 0x0a);
pub const PLAY_BOT: Color32 = Color32::from_rgb(0x07, 0x15, 0x06);
pub const GBTN_BG: Color32 = Color32::from_rgb(0x03, 0x20, 0x08);
pub const METAL_LT: Color32 = Color32::from_rgb(0x5a, 0x36, 0x32);
pub const METAL: Color32 = Color32::from_rgb(0x4a, 0x2c, 0x29);
pub const METAL_DK: Color32 = Color32::from_rgb(0x2a, 0x18, 0x16);
pub const METAL_EDGE: Color32 = Color32::from_rgb(0x6a, 0x44, 0x40);
pub const METAL_INSET: Color32 = Color32::from_rgb(0x1a, 0x0e, 0x0d);
pub const HAZARD: Color32 = Color32::from_rgb(0xe8, 0xb4, 0x00);
pub const HAZARD_DARK: Color32 = Color32::from_rgb(0x11, 0x11, 0x11);
pub const ALERT: Color32 = Color32::from_rgb(0xff, 0x55, 0x31);
pub const OK: Color32 = LINK;
pub const TITLE_PLATE: Color32 = Color32::from_rgb(0x0b, 0x0d, 0x0b);
pub const GROUP_LINE: Color32 = Color32::from_rgb(0x0e, 0x5a, 0x18);

/// Height of the hazard title bar and the gap under it.
pub const TITLE_H: f32 = 22.0;
/// Padding between the frame's outer edge and its content.
pub const FRAME_PAD: f32 = 10.0;
/// Edge length of the generated tiles.
pub const TILE: usize = 16;

/// Install the theme. Idempotent; call once on the first frame (and again after a font change).
pub fn apply(ctx: &Context) {
    // The look is one dark look on purpose (it mirrors the game); a light OS theme must not leak in.
    ctx.set_theme(ThemePreference::Dark);
    ctx.set_visuals_of(egui::Theme::Dark, visuals());
    ctx.style_mut_of(egui::Theme::Dark, |style| {
        style.spacing.item_spacing = vec2(8.0, 6.0);
        style.spacing.button_padding = vec2(10.0, 4.0);
        style.spacing.interact_size.y = 22.0;
        style.spacing.combo_width = 160.0;
        style.spacing.text_edit_width = 180.0;
        let prop = |size: f32| FontId::new(size, FontFamily::Proportional);
        style.text_styles.insert(TextStyle::Body, prop(13.0));
        style.text_styles.insert(TextStyle::Button, prop(13.0));
        style.text_styles.insert(TextStyle::Small, prop(11.0));
        style.text_styles.insert(
            TextStyle::Monospace,
            FontId::new(12.0, FontFamily::Monospace),
        );
        style.text_styles.insert(TextStyle::Heading, prop(12.0));
    });
}

/// The `Visuals` the design contract specifies (UI_design.md section 3).
pub fn visuals() -> Visuals {
    let mut v = Visuals::dark();
    v.panel_fill = SPACE;
    v.window_fill = GRID;
    v.extreme_bg_color = BOX_BG;
    v.faint_bg_color = SEL_BG;
    v.code_bg_color = BOX_BG;
    v.hyperlink_color = LINK;
    v.warn_fg_color = HAZARD;
    v.error_fg_color = ALERT;
    v.weak_text_color = Some(TEXT_DIM);
    v.window_stroke = Stroke::new(1.0, LINE);
    v.window_corner_radius = CornerRadius::same(3);
    v.menu_corner_radius = CornerRadius::same(3);
    v.window_shadow = egui::Shadow::NONE;
    v.popup_shadow = egui::Shadow::NONE;
    v.selection.bg_fill = SEL_BG;
    v.selection.stroke = Stroke::new(1.0, LINE_HI);
    let r = CornerRadius::same(2);

    let w = &mut v.widgets;
    w.noninteractive.bg_fill = GRID;
    w.noninteractive.weak_bg_fill = GRID;
    w.noninteractive.bg_stroke = Stroke::new(1.0, GROUP_LINE);
    w.noninteractive.fg_stroke = Stroke::new(1.0, TEXT);
    w.noninteractive.corner_radius = r;

    w.inactive.bg_fill = BOX_BG;
    w.inactive.weak_bg_fill = BOX_BG;
    w.inactive.bg_stroke = Stroke::new(1.0, LINE);
    w.inactive.fg_stroke = Stroke::new(1.0, TEXT);
    w.inactive.corner_radius = r;

    w.hovered.bg_fill = SEL_BG;
    w.hovered.weak_bg_fill = BOX_BG;
    w.hovered.bg_stroke = Stroke::new(1.0, LINE_HI);
    w.hovered.fg_stroke = Stroke::new(1.0, TEXT_HI);
    w.hovered.corner_radius = r;

    w.active.bg_fill = SEL_BG;
    w.active.weak_bg_fill = SEL_BG;
    w.active.bg_stroke = Stroke::new(1.0, LINE_HI);
    w.active.fg_stroke = Stroke::new(1.0, TEXT_HI);
    w.active.corner_radius = r;

    w.open.bg_fill = SEL_BG;
    w.open.weak_bg_fill = SEL_BG;
    w.open.bg_stroke = Stroke::new(1.0, LINE_HI);
    w.open.fg_stroke = Stroke::new(1.0, TEXT_HI);
    w.open.corner_radius = r;
    v
}

// ---- generated tiles -----------------------------------------------------------------------------

/// The two repeating tiles, uploaded once per `Context`.
#[derive(Clone)]
pub struct Textures {
    pub grid: TextureHandle,
    pub hazard: TextureHandle,
}

fn repeat_nearest() -> TextureOptions {
    TextureOptions {
        magnification: egui::TextureFilter::Nearest,
        minification: egui::TextureFilter::Nearest,
        wrap_mode: egui::TextureWrapMode::Repeat,
        mipmap_mode: None,
    }
}

/// 16x16: phosphor fill with a one-pixel grid line on the top row and left column.
pub fn grid_tile() -> egui::ColorImage {
    let mut px = vec![GRID; TILE * TILE];
    for i in 0..TILE {
        px[i] = GRIDLINE; // row 0
        px[i * TILE] = GRIDLINE; // column 0
    }
    egui::ColorImage::new([TILE, TILE], px)
}

/// 16x16: 45-degree stripes, 8 px hazard yellow / 8 px near-black. The period (16 along x+y) divides
/// the tile edge, so it repeats seamlessly.
pub fn hazard_tile() -> egui::ColorImage {
    let mut px = vec![HAZARD_DARK; TILE * TILE];
    for y in 0..TILE {
        for x in 0..TILE {
            if (x + y) % TILE < TILE / 2 {
                px[y * TILE + x] = HAZARD;
            }
        }
    }
    egui::ColorImage::new([TILE, TILE], px)
}

/// The cached tiles; uploads them on first use.
pub fn textures(ctx: &Context) -> Textures {
    let id = Id::new("mh_launcher_theme_textures");
    if let Some(t) = ctx.data(|d| d.get_temp::<Textures>(id)) {
        return t;
    }
    let t = Textures {
        grid: ctx.load_texture("mh_grid_tile", grid_tile(), repeat_nearest()),
        hazard: ctx.load_texture("mh_hazard_tile", hazard_tile(), repeat_nearest()),
    };
    ctx.data_mut(|d| d.insert_temp(id, t.clone()));
    t
}

fn tiled_uv(rect: Rect) -> Rect {
    Rect::from_min_max(
        pos2(0.0, 0.0),
        pos2(rect.width() / TILE as f32, rect.height() / TILE as f32),
    )
}

// ---- painters ------------------------------------------------------------------------------------
//
// Each element is available as a `Shape` (so a caller can reserve a z-slot with `Shape::Noop` and
// fill it once content of unknown size has been laid out) and as a `paint_*` that adds it directly.

/// The phosphor grid fill behind a panel's content: ONE quad, UVs span `rect / 16`.
pub fn grid_shape(tex: &Textures, rect: Rect) -> Shape {
    Shape::image(tex.grid.id(), rect, tiled_uv(rect), Color32::WHITE)
}

/// The hazard-stripe bar of a title.
pub fn hazard_shape(tex: &Textures, rect: Rect) -> Shape {
    Shape::Vec(vec![
        Shape::image(tex.hazard.id(), rect, tiled_uv(rect), Color32::WHITE),
        Shape::rect_stroke(
            rect,
            CornerRadius::ZERO,
            Stroke::new(1.0, Color32::BLACK),
            StrokeKind::Inside,
        ),
    ])
}

/// The riveted metal frame: a 4-vertex gradient (135 degrees, light top-left to dark bottom-right),
/// an outer highlight stroke, an inner dark inset, and two rivets in the top corners.
pub fn metal_frame_shape(rect: Rect) -> Shape {
    let mut mesh = Mesh::default();
    mesh.colored_vertex(rect.left_top(), METAL_LT);
    mesh.colored_vertex(rect.right_top(), METAL);
    mesh.colored_vertex(rect.right_bottom(), METAL_DK);
    mesh.colored_vertex(rect.left_bottom(), METAL);
    mesh.add_triangle(0, 1, 2);
    mesh.add_triangle(0, 2, 3);
    let mut v = vec![
        Shape::mesh(mesh),
        Shape::rect_stroke(
            rect.shrink(0.5),
            CornerRadius::ZERO,
            Stroke::new(1.0, METAL_EDGE),
            StrokeKind::Inside,
        ),
        Shape::rect_stroke(
            rect.shrink(2.5),
            CornerRadius::ZERO,
            Stroke::new(2.0, METAL_INSET),
            StrokeKind::Inside,
        ),
    ];
    for x in [rect.left() + 7.0, rect.right() - 7.0] {
        let c = pos2(x, rect.top() + 7.0);
        v.push(Shape::circle_filled(
            c,
            3.0,
            Color32::from_rgb(0x33, 0x14, 0x11),
        ));
        v.push(Shape::circle_filled(
            c - vec2(0.8, 0.8),
            1.4,
            Color32::from_rgb(0xaa, 0x88, 0x88),
        ));
    }
    Shape::Vec(v)
}

pub fn paint_grid(painter: &Painter, tex: &Textures, rect: Rect) {
    painter.add(grid_shape(tex, rect));
}

pub fn paint_hazard(painter: &Painter, tex: &Textures, rect: Rect) {
    painter.add(hazard_shape(tex, rect));
}

pub fn paint_metal_frame(painter: &Painter, rect: Rect) {
    painter.add(metal_frame_shape(rect));
}

/// Window background: black, a few fixed star dots, and a planet rising in the bottom-right.
/// Drawn clipped to `rect`; the positions are fractions of it, so it is resize-stable.
pub fn paint_background(painter: &Painter, rect: Rect) {
    let p = painter.with_clip_rect(rect);
    p.rect_filled(rect, CornerRadius::ZERO, SPACE);
    let at = |fx: f32, fy: f32| {
        pos2(
            rect.left() + rect.width() * fx,
            rect.top() + rect.height() * fy,
        )
    };
    for (fx, fy, alpha) in [
        (0.12, 0.20, 0x88u8),
        (0.70, 0.08, 0x66),
        (0.40, 0.70, 0x55),
        (0.88, 0.55, 0x77),
        (0.22, 0.88, 0x44),
        (0.60, 0.35, 0x55),
    ] {
        p.circle_filled(at(fx, fy), 0.9, Color32::from_white_alpha(alpha));
    }
    let r = rect.width().min(rect.height() * 1.33) * 0.55;
    let centre = at(1.12, 1.18);
    p.circle_filled(centre, r * 1.21, Color32::from_rgb(0x0a, 0x1d, 0x38));
    p.circle_filled(centre, r, Color32::from_rgb(0x16, 0x3a, 0x6a));
}

/// A frame's content rect: inside the metal, below the hazard title bar.
pub fn frame_content_rect(outer: Rect, with_title: bool) -> Rect {
    let mut r = outer.shrink(FRAME_PAD);
    if with_title {
        r.min.y += TITLE_H + 8.0;
    }
    r
}

/// Padding used inside the grid panel.
pub const PANEL_MARGIN: Margin = Margin::symmetric(14, 12);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tiles_are_16_square_and_repeat_seamlessly() {
        for img in [grid_tile(), hazard_tile()] {
            assert_eq!(img.size, [TILE, TILE]);
            assert_eq!(img.pixels.len(), TILE * TILE);
        }
        let h = hazard_tile();
        // Shift by one tile in x and in y: the stripe function is periodic in both.
        for y in 0..TILE {
            for x in 0..TILE {
                let a = h.pixels[y * TILE + x];
                let on = ((x + TILE) + y) % TILE < TILE / 2;
                assert_eq!(a == HAZARD, on);
            }
        }
        // Half yellow, half dark.
        let yellow = h.pixels.iter().filter(|p| **p == HAZARD).count();
        assert_eq!(yellow, TILE * TILE / 2);
        let g = grid_tile();
        assert_eq!(g.pixels[0], GRIDLINE);
        assert_eq!(g.pixels[5 * TILE + 5], GRID);
    }

    #[test]
    fn the_visuals_follow_the_contract() {
        let v = visuals();
        assert_eq!(v.panel_fill, Color32::from_rgb(0, 0, 0));
        assert_eq!(v.window_fill, Color32::from_rgb(0x00, 0x28, 0x08));
        assert_eq!(v.extreme_bg_color, Color32::from_rgb(0x00, 0x1a, 0x05));
        assert_eq!(
            v.widgets.inactive.bg_stroke.color,
            Color32::from_rgb(0x39, 0x71, 0x39)
        );
        assert_eq!(
            v.widgets.hovered.bg_stroke.color,
            Color32::from_rgb(0x84, 0xff, 0x00)
        );
        assert_eq!(v.warn_fg_color, Color32::from_rgb(0xe8, 0xb4, 0x00));
        assert_eq!(v.error_fg_color, Color32::from_rgb(0xff, 0x55, 0x31));
    }

    #[test]
    fn apply_installs_a_dark_theme_and_the_text_sizes() {
        let ctx = Context::default();
        apply(&ctx);
        let style = ctx.style_of(egui::Theme::Dark);
        assert_eq!(style.text_styles[&TextStyle::Body].size, 13.0);
        assert_eq!(style.text_styles[&TextStyle::Small].size, 11.0);
        assert_eq!(style.spacing.item_spacing, vec2(8.0, 6.0));
        assert_eq!(style.visuals.panel_fill, SPACE);
    }
}
