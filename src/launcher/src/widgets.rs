//! The launcher's small widget kit (dist RL12): the pieces the mockup draws that stock egui does not.
//!
//! Everything composes stock widgets over the theme (`theme::apply`) except the three that are
//! genuinely painted: the menu column button, the Start button and the game frame. The settings
//! renderer (`settings/render.rs`) deliberately uses none of this -- only stock widgets -- so it
//! stays generic; the pages (WU-C) use the kit for chrome around it.

#![allow(dead_code)]

use egui::{
    epaint::{Shape, StrokeKind},
    pos2, vec2, Align2, Button, Color32, Context, CornerRadius, FontId, Frame, Id, InnerResponse,
    Margin, Mesh, Response, RichText, ScrollArea, Sense, Stroke, Ui, WidgetInfo, WidgetType,
};

use crate::i18n::tr;
use crate::theme::{self, FRAME_PAD, PANEL_MARGIN, TITLE_H};

const MENU_H: f32 = 34.0;
const START_H: f32 = 46.0;

fn alpha(c: Color32, a: u8) -> Color32 {
    Color32::from_rgba_unmultiplied(c.r(), c.g(), c.b(), a)
}

/// One entry of the left-hand menu column. `dot` draws the "unsaved" marker (Settings with edits).
pub fn menu_button(ui: &mut Ui, label: &str, selected: bool, dot: bool) -> Response {
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), MENU_H), Sense::click());
    resp.widget_info(|| WidgetInfo::selected(WidgetType::Button, true, selected, label));
    if ui.is_rect_visible(rect) {
        let hot = resp.hovered() || resp.has_focus();
        let (line, text, inner) = if selected {
            (
                theme::LINE_HI,
                Color32::from_rgb(0xc8, 0xff, 0xb0),
                Color32::from_rgb(0x2f, 0x5a, 0x1f),
            )
        } else if hot {
            (
                theme::LINE_HI,
                theme::TEXT,
                Color32::from_rgb(0x1c, 0x2e, 0x1c),
            )
        } else {
            (
                theme::LINE,
                theme::TEXT_DIM,
                Color32::from_rgb(0x1c, 0x2e, 0x1c),
            )
        };
        let p = ui.painter();
        let r = CornerRadius::same(3);
        p.rect_filled(rect, r, theme::BTN);
        if selected {
            p.rect_stroke(
                rect.expand(1.5),
                CornerRadius::same(4),
                Stroke::new(2.0, alpha(theme::LINE_HI, 0x55)),
                StrokeKind::Outside,
            );
        }
        p.rect_stroke(rect, r, Stroke::new(1.0, line), StrokeKind::Inside);
        p.rect_stroke(
            rect.shrink(1.5),
            CornerRadius::ZERO,
            Stroke::new(2.0, Color32::from_rgb(0x05, 0x08, 0x05)),
            StrokeKind::Inside,
        );
        p.rect_stroke(
            rect.shrink(3.0),
            CornerRadius::ZERO,
            Stroke::new(1.0, inner),
            StrokeKind::Inside,
        );
        p.text(
            rect.center(),
            Align2::CENTER_CENTER,
            label.to_uppercase(),
            FontId::proportional(14.0),
            text,
        );
        if dot {
            p.circle_filled(
                pos2(rect.right() - 12.0, rect.center().y),
                3.0,
                theme::ALERT,
            );
        }
    }
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

/// The big "Start game" button. `disabled_reason` greys it out and explains why on hover; it is
/// also returned as the hover text so a test can read it.
pub fn start_button(ui: &mut Ui, label: &str, disabled_reason: Option<&str>) -> Response {
    let enabled = disabled_reason.is_none();
    let sense = if enabled {
        Sense::click()
    } else {
        Sense::hover()
    };
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), START_H), sense);
    resp.widget_info(|| WidgetInfo::labeled(WidgetType::Button, enabled, label));
    if ui.is_rect_visible(rect) {
        let hot = enabled && (resp.hovered() || resp.has_focus());
        let k = if enabled { 1.0 } else { 0.45 };
        let dim = |c: Color32| c.gamma_multiply(k);
        let (top, bot) = if hot {
            (theme::SEL_BG, theme::PLAY_TOP)
        } else {
            (theme::PLAY_TOP, theme::PLAY_BOT)
        };
        let mut mesh = Mesh::default();
        mesh.colored_vertex(rect.left_top(), dim(top));
        mesh.colored_vertex(rect.right_top(), dim(top));
        mesh.colored_vertex(rect.right_bottom(), dim(bot));
        mesh.colored_vertex(rect.left_bottom(), dim(bot));
        mesh.add_triangle(0, 1, 2);
        mesh.add_triangle(0, 2, 3);
        let p = ui.painter();
        p.add(mesh);
        p.rect_stroke(
            rect,
            CornerRadius::same(3),
            Stroke::new(1.0, dim(theme::LINE_HI)),
            StrokeKind::Inside,
        );
        p.text(
            rect.center(),
            Align2::CENTER_CENTER,
            label,
            FontId::proportional(18.0),
            dim(theme::TEXT_HI),
        );
    }
    match disabled_reason {
        Some(reason) => resp.on_hover_text(reason),
        None => resp.on_hover_cursor(egui::CursorIcon::PointingHand),
    }
}

/// A segmented choice (the mockup's `.seg`): one stock button per label, the selected one lit.
/// Returns the index clicked this frame. `selected = None` lights nothing.
pub fn seg<S: AsRef<str>>(ui: &mut Ui, labels: &[S], selected: Option<usize>) -> Option<usize> {
    let mut clicked = None;
    ui.horizontal_wrapped(|ui| {
        ui.spacing_mut().item_spacing.x = 4.0;
        ui.spacing_mut().button_padding = vec2(10.0, 2.0);
        for (i, l) in labels.iter().enumerate() {
            let b = Button::new(RichText::new(l.as_ref()).size(12.0))
                .wrap_mode(egui::TextWrapMode::Extend)
                .selected(selected == Some(i));
            if ui.add(b).clicked() {
                clicked = Some(i);
            }
        }
    });
    clicked
}

/// Off / On. Returns the new state when the player clicked the other one.
pub fn toggle(ui: &mut Ui, on: bool) -> Option<bool> {
    let labels = [tr("value.off"), tr("value.on")];
    seg(ui, &labels, Some(usize::from(on))).and_then(|i| {
        let now = i == 1;
        (now != on).then_some(now)
    })
}

/// A small outlined status badge ("EXPERIMENTAL", "desync").
pub fn pill(ui: &mut Ui, text: &str, color: Color32) -> Response {
    Frame::NONE
        .stroke(Stroke::new(1.0, color))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(6, 1))
        .show(ui, |ui| {
            // `extend`: a badge is one line, however narrow the cell it sits in.
            ui.add(egui::Label::new(RichText::new(text).size(10.0).color(color)).extend());
        })
        .response
}

/// A status card: dim small-caps key over a larger value.
pub fn card(ui: &mut Ui, key: &str, value: &str, value_color: Color32) -> Response {
    Frame::NONE
        .fill(alpha(Color32::from_rgb(0x00, 0x1c, 0x06), 0xcc))
        .stroke(Stroke::new(1.0, theme::LINE))
        .corner_radius(CornerRadius::same(3))
        .inner_margin(Margin::same(8))
        .show(ui, |ui| {
            ui.set_min_width(ui.available_width());
            ui.spacing_mut().item_spacing.y = 2.0;
            ui.label(
                RichText::new(key.to_uppercase())
                    .size(11.0)
                    .color(theme::TEXT_DIM)
                    .extra_letter_spacing(1.2),
            );
            ui.label(RichText::new(value).size(15.0).color(value_color));
        })
        .response
}

fn scoped_button(
    ui: &mut Ui,
    label: &str,
    line: Color32,
    fill: Color32,
    text: Color32,
) -> Response {
    ui.scope(|ui| {
        let w = &mut ui.visuals_mut().widgets;
        w.inactive.weak_bg_fill = fill;
        w.inactive.bg_stroke = Stroke::new(1.0, line);
        w.hovered.weak_bg_fill = theme::SEL_BG;
        w.hovered.bg_stroke = Stroke::new(1.0, theme::LINE_HI);
        ui.spacing_mut().button_padding = vec2(14.0, 4.0);
        ui.add(
            Button::new(RichText::new(label).size(12.0).color(text))
                .wrap_mode(egui::TextWrapMode::Extend),
        )
    })
    .inner
}

/// The boxed secondary button (`.gbtn`).
pub fn gbtn(ui: &mut Ui, label: &str) -> Response {
    scoped_button(ui, label, theme::LINE_HI, theme::GBTN_BG, theme::TEXT)
}

/// A developer/diagnostic action (`.gbtn.dev`): alert-red outline.
pub fn gbtn_dev(ui: &mut Ui, label: &str) -> Response {
    scoped_button(
        ui,
        label,
        theme::ALERT,
        theme::GBTN_BG,
        Color32::from_rgb(0xff, 0xb1, 0x9e),
    )
}

/// Small dim helper text.
pub fn hint(ui: &mut Ui, text: &str) -> Response {
    ui.label(RichText::new(text).size(11.0).color(theme::TEXT_DIM))
}

/// A section heading: 12 px, spaced capitals, light green. No rule (use for the first one on a page).
pub fn heading(ui: &mut Ui, text: &str) -> Response {
    ui.label(
        RichText::new(text.to_uppercase())
            .size(12.0)
            .color(theme::LINK)
            .extra_letter_spacing(1.6),
    )
}

/// A section heading under a thin rule (`.group`).
pub fn group_header(ui: &mut Ui, text: &str) -> Response {
    ui.add_space(4.0);
    ui.separator();
    heading(ui, text)
}

/// The riveted metal frame with an optional hazard-stripe title bar and the phosphor-grid panel.
///
/// `fill_height` stretches the panel to the space available (and scrolls its content); otherwise the
/// frame is as tall as its content. The metal and the grid are painted AFTER the content is laid
/// out, into z-slots reserved first, so neither needs the content's height in advance.
pub fn game_frame<R>(
    ui: &mut Ui,
    title: Option<&str>,
    fill_height: bool,
    add_contents: impl FnOnce(&mut Ui) -> R,
) -> InnerResponse<R> {
    let tex = theme::textures(ui.ctx());
    let avail = ui.available_rect_before_wrap();
    let metal_slot = ui.painter().add(Shape::Noop);
    let outer = Frame::NONE
        .inner_margin(Margin::same(FRAME_PAD as i8))
        .show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            let inner_w = ui.available_width();
            let mut used_h = 0.0;
            if let Some(t) = title {
                let (bar, _) = ui.allocate_exact_size(vec2(inner_w, TITLE_H), Sense::hover());
                let p = ui.painter();
                p.add(theme::hazard_shape(&tex, bar));
                let galley =
                    p.layout_no_wrap(t.to_string(), FontId::proportional(13.0), theme::TEXT);
                let plate =
                    egui::Rect::from_center_size(bar.center(), galley.size() + vec2(36.0, 4.0));
                p.rect_filled(plate, CornerRadius::ZERO, theme::TITLE_PLATE);
                p.rect_stroke(
                    plate,
                    CornerRadius::ZERO,
                    Stroke::new(1.0, Color32::from_rgb(0x2a, 0x2a, 0x1a)),
                    StrokeKind::Inside,
                );
                p.galley(plate.center() - galley.size() / 2.0, galley, theme::TEXT);
                ui.add_space(8.0);
                used_h = TITLE_H + 8.0;
            }
            let panel_slot = ui.painter().add(Shape::Noop);
            let panel_h = avail.height() - 2.0 * FRAME_PAD - used_h;
            let panel = Frame::NONE.inner_margin(PANEL_MARGIN).show(ui, |ui| {
                ui.spacing_mut().item_spacing.y = 6.0;
                ui.set_min_width(inner_w - 28.0);
                if fill_height {
                    ScrollArea::vertical()
                        .auto_shrink([false, false])
                        .max_height((panel_h - 24.0).max(40.0))
                        .show(ui, add_contents)
                        .inner
                } else {
                    add_contents(ui)
                }
            });
            let pr = panel.response.rect;
            ui.painter().set(
                panel_slot,
                Shape::Vec(vec![
                    theme::grid_shape(&tex, pr),
                    Shape::rect_stroke(
                        pr,
                        CornerRadius::same(4),
                        Stroke::new(1.0, theme::LINE),
                        StrokeKind::Inside,
                    ),
                ]),
            );
            panel.inner
        });
    ui.painter()
        .set(metal_slot, theme::metal_frame_shape(outer.response.rect));
    InnerResponse::new(outer.inner, outer.response)
}

/// What a modal reports.
pub struct ModalOutcome<R> {
    pub inner: R,
    /// Esc, or a click on the backdrop. The caller decides what "close" means.
    pub close_requested: bool,
}

/// A modal dialog in the game frame, centred over a dimmed backdrop. Reused by the dirty-settings
/// prompt and (RL9) the launcher-restart prompt.
pub fn modal<R>(
    ctx: &Context,
    id: &str,
    title: &str,
    add_contents: impl FnOnce(&mut Ui) -> R,
) -> ModalOutcome<R> {
    let m = egui::Modal::new(Id::new(id))
        .backdrop_color(Color32::from_black_alpha(0xbb))
        .frame(Frame::NONE)
        .show(ctx, |ui| {
            ui.set_width(380.0);
            game_frame(ui, Some(title), false, add_contents).inner
        });
    let close_requested = m.should_close();
    ModalOutcome {
        inner: m.inner,
        close_requested,
    }
}

/// Dim, right-aligned footer text helper (`.foot`).
pub fn footer_text(ui: &mut Ui, text: &str) -> Response {
    ui.label(
        RichText::new(text)
            .size(11.0)
            .monospace()
            .color(theme::TEXT_DIM),
    )
}
