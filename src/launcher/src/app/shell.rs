//! The window's chrome (dist RL12/RL14): the starfield, the menu column with the Start game button
//! and the version line, the framed page with its hazard-stripe title, the status footer and the
//! modals. It draws no page content -- `pages::*` do, one function each, called from here.
//!
//! The layout is fixed rectangles rather than egui panels: the same arithmetic at 800x600 (the
//! smallest window `main.rs` allows) and at 1280x720, with nothing that can grow out of the frame.

use egui::{pos2, vec2, Align, Color32, Layout, Rect, RichText, Ui, UiBuilder};

use super::{pages, App, Page, StartAnswer};
use crate::i18n::{tr, trf};
use crate::{theme, widgets};

/// Space around the window's content.
const PAD: f32 = 18.0;
/// The menu column's width.
const SIDE_W: f32 = 200.0;
/// The footer strip under the frame.
const FOOT_H: f32 = 22.0;

pub fn show(app: &mut App, ui: &mut Ui) {
    let full = ui.max_rect();
    theme::paint_background(ui.painter(), full);
    let side = Rect::from_min_size(
        full.min + vec2(PAD, PAD),
        vec2(SIDE_W, (full.height() - 2.0 * PAD).max(0.0)),
    );
    let main = Rect::from_min_max(
        pos2(side.right() + PAD, side.top()),
        (full.max - vec2(PAD, PAD)).max(pos2(side.right() + PAD, side.top())),
    );
    let frame_rect = Rect::from_min_max(main.min, pos2(main.max.x, main.max.y - FOOT_H));
    let foot_rect = Rect::from_min_max(pos2(main.min.x, main.max.y - FOOT_H + 4.0), main.max);

    ui.scope_builder(UiBuilder::new().max_rect(side), |ui| menu_column(app, ui));

    let page = app.view;
    ui.scope_builder(UiBuilder::new().max_rect(frame_rect), |ui| {
        widgets::game_frame(ui, Some(tr(page.title_key())), true, |ui| match page {
            Page::Play => pages::play::show(app, ui),
            Page::Settings => pages::settings::show(app, ui),
            Page::Report => pages::report::show(app, ui),
            Page::About => pages::about::show(app, ui),
            Page::Diagnostics => pages::diagnostics::show(app, ui),
        });
    });
    ui.scope_builder(UiBuilder::new().max_rect(foot_rect), |ui| footer(app, ui));

    modals(app, ui);
}

/// Logo, the five menu entries, and -- pinned to the bottom -- Start game and the version line.
fn menu_column(app: &mut App, ui: &mut Ui) {
    ui.spacing_mut().item_spacing.y = 10.0;
    ui.label(
        RichText::new(format!(
            "{}\n{}",
            tr("app.logo_line1"),
            tr("app.logo_line2")
        ))
        .size(20.0)
        .color(Color32::from_rgb(0xcc, 0xff, 0xee))
        .extra_letter_spacing(3.0),
    );
    ui.label(
        RichText::new(tr("app.logo_sub").to_uppercase())
            .size(10.0)
            .color(theme::TEXT_DIM)
            .extra_letter_spacing(3.0),
    );
    for page in Page::ALL {
        // The unsaved-settings marker sits on Settings, and also on Diagnostics, whose rows are
        // settings too.
        let dot = matches!(page, Page::Settings | Page::Diagnostics) && app.model.is_dirty();
        if widgets::menu_button(ui, tr(page.menu_key()), app.view == page, dot).clicked() {
            if page == Page::Settings && app.view != Page::Settings {
                app.reload_model_if_clean();
            }
            app.view = page;
        }
    }
    ui.with_layout(Layout::bottom_up(Align::Center), |ui| {
        ui.label(
            RichText::new(trf(
                "app.version_line",
                &[
                    ("version", crate::version::VERSION),
                    // The channel's NAME is the name (`stable`, `latest`), in every language.
                    ("channel", app.config.channel()),
                ],
            ))
            .monospace()
            .size(11.0)
            .color(theme::TEXT_DIM),
        );
        let reason = app.start_blocked();
        if widgets::start_button(ui, tr("button.start_game"), reason).clicked() {
            app.press_start();
        }
    });
}

/// The status strip: what the last action said (left), what the update half said (right).
fn footer(app: &mut App, ui: &mut Ui) {
    let r = ui.max_rect();
    let split = r.left() + r.width() * 0.6;
    let left = Rect::from_min_max(r.min, pos2(split - 8.0, r.max.y));
    let right = Rect::from_min_max(pos2(split, r.min.y), r.max);

    let (text, color) = if !app.status_line.is_empty() {
        (
            app.status_line.clone(),
            if app.status_is_error {
                theme::ALERT
            } else {
                theme::TEXT_DIM
            },
        )
    } else {
        (tr("footer.ready").to_string(), theme::TEXT_DIM)
    };
    ui.scope_builder(UiBuilder::new().max_rect(left), |ui| {
        let l = ui.add(
            egui::Label::new(RichText::new(&text).size(11.0).monospace().color(color)).truncate(),
        );
        l.on_hover_text(&text);
    });
    if !app.update_line.is_empty() {
        let color = if app.update_is_error {
            theme::ALERT
        } else {
            theme::TEXT_DIM
        };
        let text = app.update_line.clone();
        ui.scope_builder(
            UiBuilder::new()
                .max_rect(right)
                .layout(Layout::right_to_left(Align::Center)),
            |ui| {
                let l = ui.add(
                    egui::Label::new(RichText::new(&text).size(11.0).monospace().color(color))
                        .truncate(),
                );
                l.on_hover_text(&text);
            },
        );
    }
}

/// The modal dialogs, drawn last so they sit over everything.
fn modals(app: &mut App, ui: &mut Ui) {
    let ctx = ui.ctx().clone();
    if app.start_prompt {
        let problems = app.model.has_problems();
        let n = app.model.dirty_count();
        let out = widgets::modal(&ctx, "start_prompt", tr("modal.dirty.title"), |ui| {
            ui.label(tr("modal.dirty.body"));
            widgets::hint(ui, &trf("settings.unsaved", &[("n", &n.to_string())]));
            if problems {
                ui.label(
                    RichText::new(tr("modal.dirty.invalid"))
                        .size(11.0)
                        .color(theme::ALERT),
                );
            }
            ui.add_space(6.0);
            let mut answer = None;
            ui.horizontal_wrapped(|ui| {
                ui.add_enabled_ui(!problems, |ui| {
                    if widgets::gbtn(ui, tr("btn.apply")).clicked() {
                        answer = Some(StartAnswer::Apply);
                    }
                });
                if widgets::gbtn(ui, tr("btn.discard")).clicked() {
                    answer = Some(StartAnswer::Discard);
                }
                if widgets::gbtn(ui, tr("btn.cancel")).clicked() {
                    answer = Some(StartAnswer::Cancel);
                }
            });
            answer
        });
        if let Some(a) = out.inner {
            app.answer_start_prompt(a);
        } else if out.close_requested {
            app.answer_start_prompt(StartAnswer::Cancel);
        }
    }
    pages::about::doc_modal(app, &ctx);
    restart_modal(app, &ctx);
}

/// RL9: a staged launcher is waiting -- "Restart now" swaps it in and reopens this page; "Later"
/// (or Esc) keeps this window and swaps it in when the window closes.
fn restart_modal(app: &mut App, ctx: &egui::Context) {
    let Some(version) = app.restart_prompt().map(str::to_string) else {
        return;
    };
    let out = widgets::modal(
        ctx,
        "launcher_restart",
        tr("modal.launcher_updated.title"),
        |ui| {
            ui.label(trf("modal.launcher_updated.body", &[("version", &version)]));
            ui.add_space(6.0);
            let mut now = None;
            ui.horizontal_wrapped(|ui| {
                if widgets::gbtn(ui, tr("btn.restart_now")).clicked() {
                    now = Some(true);
                }
                if widgets::gbtn(ui, tr("btn.later")).clicked() {
                    now = Some(false);
                }
            });
            now
        },
    );
    match out.inner {
        Some(true) => app.restart_now(ctx),
        Some(false) => app.restart_later(),
        None if out.close_requested => app.restart_later(),
        None => {}
    }
}
