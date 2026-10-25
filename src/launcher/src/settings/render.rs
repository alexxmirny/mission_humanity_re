//! The generic settings renderer (dist RL13): rows in, stock egui widgets out.
//!
//! NOTHING HERE NAMES A SETTING. A row is drawn from its schema fields alone:
//!
//! | schema                                   | control                                              |
//! |------------------------------------------|------------------------------------------------------|
//! | `provider`                               | combo over the provider's options                    |
//! | `control: relay`                         | segmented mode + the companion address box on custom |
//! | `bool`                                   | Off / On segments                                    |
//! | `enum`, <= 4 options, `control` not combo| segmented                                            |
//! | `enum` otherwise, `control: combo`       | combo                                                |
//! | `int` with `options`                     | segmented over the options                           |
//! | `int` without                            | text box, red when outside `min`..`max`              |
//! | `hotkey`                                 | click-to-capture (Esc cancels) + Clear               |
//! | `string`                                 | text box                                             |
//!
//! Labels, hints and option names come from the string table (`setting.<label>[.hint|.opt.<v>]`),
//! so adding a setting is a registry row plus strings. Badges: `experimental` -> alert pill,
//! `pending` (debug builds only) -> dim pill. `restart` has no per-row badge (user 2026-10-14: on
//! every row it was noise): a page with any restart row says it ONCE, in the bar above the rows.
//! A value the row does not list gets an extra `<v> (custom)` segment/entry and is preserved until
//! the player picks another.
//!
//! COMPACT BY DEFAULT (user 2026-10-14). A row's description (`.hint`, and the host-dictated note)
//! is a hover tooltip over the whole row -- label and control -- unless the bar's Compact /
//! Detailed switch (`RenderState::detailed`, remembered in launcher.toml) asks for it inline. The
//! label column is as wide as the page's widest label in the current font, so values line up
//! without the fixed 150 px column wrapping long Russian labels.
//!
//! The renderer edits `Model` only; Apply/Revert are returned from `footer` as an `Action` for the
//! page to carry out (it owns the launcher store and the ini path).

use egui::{Align, Button, ComboBox, Event, Grid, Key, Layout, RichText, Sense, TextEdit, Ui};

use super::model::{hotkey_named, parse_hotkey_named, Model, Problem};
use super::schema::{Control, Kind, Row};
use crate::i18n::{humanise, tr, tr_opt, trf};
use crate::theme;
use crate::widgets;

/// Per-page UI state (not persisted).
#[derive(Clone, Debug, Default)]
pub struct RenderState {
    /// The selected Settings sub-tab id; empty = the first one.
    pub subtab: String,
    /// The hotkey row currently waiting for a key press.
    pub capturing: Option<String>,
    /// Row descriptions inline (true) or on hover only (false, the default).
    pub detailed: bool,
}

/// What the footer asks the page to do. The launcher's pages use the shell's save bar instead; this
/// strip stays for the engine's own harness tests.
#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    None,
    Apply,
    Revert,
}

/// The English-or-translated name of one option value.
pub fn option_label(row: &Row, value: &str) -> String {
    let key = row.option_key(value);
    match tr_opt(&key) {
        Some(s) => s.to_string(),
        None => humanise(value),
    }
}

/// The text of a `Problem`.
pub fn problem_text(p: &Problem) -> String {
    match p {
        Problem::NotInteger => tr("problem.not_integer").to_string(),
        Problem::NotNumber => tr("problem.not_number").to_string(),
        Problem::OutOfRange {
            min: Some(a),
            max: Some(b),
        } => trf(
            "problem.out_of_range",
            &[("min", &a.to_string()), ("max", &b.to_string())],
        ),
        Problem::OutOfRange { min: Some(a), .. } => {
            trf("problem.at_least", &[("min", &a.to_string())])
        }
        Problem::OutOfRange { max: Some(b), .. } => {
            trf("problem.at_most", &[("max", &b.to_string())])
        }
        Problem::OutOfRange { .. } => tr("problem.not_integer").to_string(),
        Problem::BadHotkey => tr("problem.bad_hotkey").to_string(),
        Problem::BadChars => tr("problem.bad_chars").to_string(),
        Problem::BadAddress => tr("problem.bad_address").to_string(),
    }
}

/// The Settings page body: the sub-tab strip and the rows of the selected sub-tab.
pub fn show_settings(ui: &mut Ui, model: &mut Model, state: &mut RenderState) {
    let tabs: Vec<(String, String)> = model
        .schema()
        .settings_subtabs()
        .iter()
        .map(|t| (t.id.clone(), t.label.clone()))
        .collect();
    if tabs.is_empty() {
        widgets::hint(ui, tr("settings.nothing"));
        return;
    }
    if !tabs.iter().any(|(id, _)| *id == state.subtab) {
        state.subtab = tabs[0].0.clone();
    }
    let labels: Vec<String> = tabs
        .iter()
        .map(|(id, label)| {
            let edited = model
                .schema()
                .rows_in_subtab(id)
                .any(|r| model.is_edited(&r.id));
            let name = tr(label);
            if edited {
                format!("{name} •")
            } else {
                name.to_string()
            }
        })
        .collect();
    let sel = tabs.iter().position(|(id, _)| *id == state.subtab);
    if let Some(i) = widgets::seg(ui, &labels, sel) {
        state.subtab = tabs[i].0.clone();
        state.capturing = None;
    }
    ui.add_space(4.0);
    let groups: Vec<(String, Vec<Row>)> = model
        .schema()
        .groups_in_subtab(&state.subtab)
        .into_iter()
        .map(|(g, rows)| (g, rows.into_iter().cloned().collect()))
        .collect();
    let id = format!("settings_{}", state.subtab);
    show_grouped(ui, model, state, &groups, &id);
}

/// A page of rows under their group headers: the bar (restart note, Compact / Detailed), the
/// `[Host]` legend when a shown row is host-dictated, then one headed grid per group. One label
/// column serves the whole page, so the values of every group line up. A group with no shown row
/// (its rows hidden by `visible_if`) draws nothing.
fn show_grouped(
    ui: &mut Ui,
    model: &mut Model,
    state: &mut RenderState,
    groups: &[(String, Vec<Row>)],
    id: &str,
) {
    let shown: Vec<(&String, Vec<Row>)> = groups
        .iter()
        .map(|(g, rows)| {
            (
                g,
                rows.iter()
                    .filter(|r| model.is_visible(r))
                    .cloned()
                    .collect::<Vec<Row>>(),
            )
        })
        .filter(|(_, rows)| !rows.is_empty())
        .collect();
    let all: Vec<Row> = shown.iter().flat_map(|(_, rows)| rows.clone()).collect();
    if all.is_empty() {
        return;
    }
    view_bar(ui, state, all.iter().any(|r| r.restart));
    if all.iter().any(|r| r.host) {
        widgets::hint(ui, tr("settings.host_note"));
    }
    let label_w = label_width(ui, &all);
    for (i, (group, rows)) in shown.iter().enumerate() {
        // A group of nothing but experimental rows says so once, on its header, not on every row.
        let all_experimental = !group.is_empty() && rows.iter().all(|r| r.experimental);
        if !group.is_empty() {
            group_header(ui, tr(&format!("group.{group}")), all_experimental);
            if let Some(h) = tr_opt(&format!("group.{group}.hint")) {
                widgets::hint(ui, h);
            }
            ui.add_space(2.0);
        }
        show_rows(
            ui,
            model,
            state,
            rows,
            &format!("{id}_{group}_{i}"),
            label_w,
            all_experimental,
        );
    }
}

/// A group header: the plain rule + heading, plus the EXPERIMENTAL pill when the whole group is.
fn group_header(ui: &mut Ui, text: &str, experimental: bool) {
    if !experimental {
        widgets::group_header(ui, text);
        return;
    }
    ui.add_space(4.0);
    ui.separator();
    ui.horizontal(|ui| {
        widgets::heading(ui, text);
        widgets::pill(ui, tr("badge.experimental"), theme::ALERT);
    });
}

/// The line above the rows: the page's one "applies at the next game start" note (when it has a
/// restart row) on the left, the Compact / Detailed switch on the right.
fn view_bar(ui: &mut Ui, state: &mut RenderState, restart: bool) {
    ui.horizontal(|ui| {
        if restart {
            widgets::hint(ui, tr("settings.restart_note"));
        }
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.spacing_mut().item_spacing.x = 4.0;
            ui.spacing_mut().button_padding = egui::vec2(8.0, 1.0);
            // Right to left: the last-drawn button is the leftmost.
            for (detailed, key) in [
                (true, "settings.view.detailed"),
                (false, "settings.view.compact"),
            ] {
                let b = Button::new(RichText::new(tr(key)).size(11.0))
                    .wrap_mode(egui::TextWrapMode::Extend)
                    .selected(state.detailed == detailed);
                if ui.add(b).clicked() {
                    state.detailed = detailed;
                }
            }
        });
    });
    ui.add_space(2.0);
}

/// The Diagnostics page's setting rows, grouped by the schema's `group`.
pub fn show_diagnostics(ui: &mut Ui, model: &mut Model, state: &mut RenderState) {
    let groups: Vec<(String, Vec<Row>)> = model
        .schema()
        .diagnostics_groups()
        .into_iter()
        .map(|(g, rows)| (g, rows.into_iter().cloned().collect()))
        .collect();
    show_grouped(ui, model, state, &groups, "diag");
}

/// Narrowest label column.
const LABEL_MIN: f32 = 80.0;
/// Room after the widest label, so it never touches its control.
const LABEL_PAD: f32 = 6.0;
/// The controls keep at least this much of the row (a 200 px combo and its reset arrow); a label
/// longer than what is left wraps.
const CTRL_MIN: f32 = 210.0;
/// ...and the label column never takes more than this share of a wide row; a longer label wraps
/// (user 2026-10-14: cap rather than squeeze the controls of the long Russian labels).
const LABEL_MAX_SHARE: f32 = 0.55;
/// Space between grid columns.
const COL_GAP: f32 = 10.0;
/// Width of a row's description tooltip.
const TIP_W: f32 = 360.0;
/// How far a dependent row (one with a `visible_if`) is pushed in under its parent.
const DEPENDENT_INDENT: f32 = 16.0;

/// Left indent of a row's label: dependent rows sit under the row they depend on.
fn indent_of(row: &Row) -> f32 {
    if row.visible_if.is_some() {
        DEPENDENT_INDENT
    } else {
        0.0
    }
}

/// The label column's width for `rows`: the widest label, measured in the font the label is drawn
/// with and in the current language, plus padding -- capped so the controls keep most of the row.
pub fn label_width(ui: &Ui, rows: &[Row]) -> f32 {
    let font = egui::TextStyle::Body.resolve(ui.style());
    let widest = rows
        .iter()
        .map(|r| {
            ui.painter()
                .layout_no_wrap(tr(&r.label_key()).to_string(), font.clone(), theme::TEXT)
                .size()
                .x
                + indent_of(r)
        })
        .fold(0.0, f32::max);
    let avail = ui.available_width();
    let cap = (avail - CTRL_MIN)
        .min(avail * LABEL_MAX_SHARE)
        .max(LABEL_MIN);
    (widest.ceil() + LABEL_PAD).clamp(LABEL_MIN, cap)
}

/// The pills a row carries, as (text, colour), in the order they are drawn. `group_flagged`: the
/// group header already carries the EXPERIMENTAL pill, so the row does not repeat it.
fn badge_list(row: &Row, group_flagged: bool) -> Vec<(&'static str, egui::Color32)> {
    let mut v = Vec::new();
    if row.host {
        v.push((tr("badge.host"), theme::LINK));
    }
    if row.experimental && !group_flagged {
        v.push((tr("badge.experimental"), theme::ALERT));
    }
    if row.pending {
        v.push((tr("badge.pending"), theme::TEXT_DIM));
    }
    v
}

/// A row's description: its hint. Empty = none. (The host-dictated note is the Host pill's hover
/// text and the page legend, not a line under every row.)
fn description(row: &Row) -> Vec<&'static str> {
    tr_opt(&row.hint_key()).into_iter().collect()
}

/// Draw `rows` as a grid: label | control group + reset arrow + badges, then (detailed view) the
/// description; in the compact view the description is a tooltip over the whole row.
///
/// THE ARROW AND THE PILLS BELONG TO THEIR ROW, so they are drawn straight after the row's last
/// control, in the same wrapping line, vertically centred on it. (They used to sit in a column of
/// their own at the far right edge, detached from a narrow control and, with long Russian option
/// groups, a line away from the control they described.) A control group that wraps takes the
/// arrow and the pills with it: they follow the LAST control of the row.
pub fn show_rows(
    ui: &mut Ui,
    model: &mut Model,
    state: &mut RenderState,
    rows: &[Row],
    id: &str,
    label_w: f32,
    group_flagged: bool,
) {
    let ctrl_w = (ui.available_width() - label_w - COL_GAP).max(140.0);
    let row_gap = if state.detailed { 8.0 } else { 6.0 };
    Grid::new(id)
        .num_columns(2)
        .spacing([COL_GAP, row_gap])
        .min_col_width(label_w)
        .show(ui, |ui| {
            for row in rows {
                let desc = description(row);
                let label_rect = label_cell(ui, model, row, label_w);
                let ctrl = ui.vertical(|ui| {
                    ui.set_max_width(ctrl_w);
                    let disabled = model.is_disabled(row);
                    ui.horizontal_wrapped(|ui| {
                        ui.add_enabled_ui(!disabled, |ui| control(ui, model, state, row));
                        if model.differs_from_default(&row.id) {
                            ui.add_space(2.0);
                            let b = Button::new(RichText::new("↺").color(theme::TEXT_DIM)).small();
                            if ui.add(b).on_hover_text(tr("settings.reset_row")).clicked() {
                                model.reset_row(&row.id);
                            }
                        }
                        for (text, color) in badge_list(row, group_flagged) {
                            let r = widgets::pill(ui, text, color);
                            if row.host && text == tr("badge.host") {
                                r.on_hover_text(tr("setting.host_dictated"));
                            }
                        }
                    });
                    if row.control == Control::Relay {
                        relay_address(ui, model, row);
                    }
                    if disabled {
                        widgets::hint(ui, tr(&format!("setting.{}.disabled", row.label)));
                    }
                    if let Some(p) = problem_for_row(model, row) {
                        ui.label(
                            RichText::new(problem_text(&p))
                                .size(11.0)
                                .color(theme::ALERT),
                        );
                    }
                    if state.detailed {
                        for d in &desc {
                            widgets::hint(ui, d);
                        }
                    }
                });
                if !state.detailed && !desc.is_empty() {
                    // A hover-only area over the whole row, registered AFTER its widgets so egui
                    // counts it as on top: it is hovered together with the button under the
                    // pointer and takes no clicks or focus from it.
                    let rect = label_rect.union(ctrl.response.rect);
                    ui.interact(rect, ui.id().with(("row_tip", &row.id)), Sense::hover())
                        .on_hover_ui(|ui| {
                            ui.set_max_width(TIP_W);
                            for (i, d) in desc.iter().enumerate() {
                                let mut t = RichText::new(*d);
                                if i > 0 {
                                    t = t.size(11.0).color(theme::TEXT_DIM);
                                }
                                ui.label(t);
                            }
                        });
                }
                ui.end_row();
            }
        });
}

fn problem_for_row(model: &Model, row: &Row) -> Option<Problem> {
    model.problem_for(&row.id).or_else(|| {
        // The relay control reports its companion's problem under the mode row.
        row.companion
            .as_deref()
            .and_then(|c| model.problem_for(c))
            .filter(|_| row.control == Control::Relay)
    })
}

fn label_cell(ui: &mut Ui, model: &Model, row: &Row, label_w: f32) -> egui::Rect {
    ui.vertical(|ui| {
        ui.set_max_width(label_w);
        ui.horizontal_wrapped(|ui| {
            ui.add_space(indent_of(row));
            let mut text = RichText::new(tr(&row.label_key()));
            if model.is_edited(&row.id) {
                text = text.color(theme::TEXT_HI);
            }
            ui.label(text);
        });
    })
    .response
    .rect
}

/// The control for one row. Returns nothing; edits go through `model.set`.
fn control(ui: &mut Ui, model: &mut Model, state: &mut RenderState, row: &Row) {
    let id = row.id.as_str();
    if row.control == Control::Relay {
        return relay_control(ui, model, state, row);
    }
    if let Some(p) = row.provider {
        let opts: Vec<(String, String)> = model
            .provider_options(p)
            .iter()
            .map(|o| (o.value.clone(), o.label.clone()))
            .collect();
        if !opts.is_empty() {
            return combo(ui, model, row, opts);
        }
    }
    match row.kind {
        Kind::Bool => {
            let on = model.value(id).trim() == "1"
                || super::model::parse_bool(&model.value(id)) == Some(true);
            if let Some(now) = widgets::toggle(ui, on) {
                model.set(id, if now { "1" } else { "0" });
            }
        }
        Kind::Enum => {
            let opts: Vec<(String, String)> = row
                .options
                .iter()
                .map(|o| (o.clone(), option_label(row, o)))
                .collect();
            if row.control == Control::Combo || opts.len() > 4 {
                combo(ui, model, row, opts);
            } else {
                segmented(ui, model, row, opts);
            }
        }
        Kind::Int if !row.options.is_empty() => {
            let opts: Vec<(String, String)> = row
                .options
                .iter()
                .map(|o| (o.clone(), option_label(row, o)))
                .collect();
            segmented(ui, model, row, opts);
        }
        Kind::Int | Kind::Float | Kind::Str => text_box(ui, model, row, id, false),
        Kind::Hotkey => hotkey(ui, model, state, row),
    }
}

/// Index of `value` among `opts`, comparing like the model does (case-insensitive for enums).
fn position_of(row: &Row, opts: &[(String, String)], value: &str) -> Option<usize> {
    let v = value.trim();
    opts.iter().position(|(o, _)| {
        o == v
            || o.eq_ignore_ascii_case(v)
            || (row.kind == Kind::Int && o.trim().parse::<i64>().ok() == v.parse::<i64>().ok())
    })
}

fn segmented(ui: &mut Ui, model: &mut Model, row: &Row, mut opts: Vec<(String, String)>) {
    let cur = model.value(&row.id);
    let mut sel = position_of(row, &opts, &cur);
    if sel.is_none() {
        // An unlisted value (hand-written ini): show it as an extra, lit segment and keep it.
        opts.push((
            cur.trim().to_string(),
            trf("value.custom", &[("value", cur.trim())]),
        ));
        sel = Some(opts.len() - 1);
    }
    let labels: Vec<&str> = opts.iter().map(|(_, l)| l.as_str()).collect();
    if let Some(i) = widgets::seg(ui, &labels, sel) {
        model.set(&row.id, &opts[i].0);
    }
}

fn combo(ui: &mut Ui, model: &mut Model, row: &Row, mut opts: Vec<(String, String)>) {
    let cur = model.value(&row.id);
    let mut sel = position_of(row, &opts, &cur);
    if sel.is_none() {
        opts.insert(
            0,
            (
                cur.trim().to_string(),
                trf("value.custom", &[("value", cur.trim())]),
            ),
        );
        sel = Some(0);
    }
    let shown = sel.map_or_else(|| tr("combo.pick").to_string(), |i| opts[i].1.clone());
    ComboBox::from_id_salt(("settings_combo", &row.id))
        .selected_text(shown)
        // Leave room for the reset arrow on the same line (a wide label column narrows the controls).
        .width(200.0_f32.min(ui.available_width() - 64.0).max(120.0))
        .show_ui(ui, |ui| {
            for (i, (value, label)) in opts.iter().enumerate() {
                if ui.selectable_label(sel == Some(i), label).clicked() {
                    model.set(&row.id, value);
                }
            }
        });
}

fn text_box(ui: &mut Ui, model: &mut Model, row: &Row, id: &str, wide: bool) {
    let mut buf = model.value(id);
    let bad = model.problem_for(id).is_some();
    ui.scope(|ui| {
        if bad {
            let w = &mut ui.visuals_mut().widgets;
            w.inactive.bg_stroke.color = theme::ALERT;
            w.hovered.bg_stroke.color = theme::ALERT;
            w.active.bg_stroke.color = theme::ALERT;
            ui.visuals_mut().selection.stroke.color = theme::ALERT;
        }
        let width = if wide || !matches!(row.kind, Kind::Int | Kind::Float) {
            220.0
        } else {
            90.0
        };
        let mut edit = TextEdit::singleline(&mut buf).desired_width(width);
        if bad {
            edit = edit.text_color(theme::ALERT);
        }
        if ui.add(edit).changed() {
            model.set(id, &buf);
        }
    });
}

fn relay_control(ui: &mut Ui, model: &mut Model, state: &mut RenderState, row: &Row) {
    let opts: Vec<(String, String)> = row
        .options
        .iter()
        .map(|o| (o.clone(), option_label(row, o)))
        .collect();
    segmented(ui, model, row, opts);
    let _ = state;
}

/// The custom relay's `host:port` box, on a line of its own under the mode choice (only in custom
/// mode).
fn relay_address(ui: &mut Ui, model: &mut Model, row: &Row) {
    let custom = super::model::canon(row, &model.value(&row.id)) == "custom";
    let (true, Some(comp)) = (custom, row.companion.clone()) else {
        return;
    };
    ui.horizontal_wrapped(|ui| {
        ui.label(
            RichText::new(tr("relay.address"))
                .size(11.0)
                .color(theme::TEXT_DIM),
        );
        text_box(ui, model, row, &comp, true);
    });
}

fn hotkey(ui: &mut Ui, model: &mut Model, state: &mut RenderState, row: &Row) {
    let id = row.id.clone();
    let cur = model.value(&id);
    let capturing = state.capturing.as_deref() == Some(id.as_str());
    let shown = if capturing {
        tr("hotkey.capture").to_string()
    } else {
        match parse_hotkey_named(&cur, hotkey_named(row)) {
            Ok(Some(h)) => h.format(),
            Ok(None) => tr("value.none").to_string(),
            Err(()) => cur.clone(),
        }
    };
    let resp = ui.add(
        Button::new(shown)
            .selected(capturing)
            .min_size(egui::vec2(120.0, 0.0)),
    );
    let resp = resp.on_hover_text(tr("hotkey.help"));
    if resp.clicked() {
        state.capturing = if capturing { None } else { Some(id.clone()) };
        if !capturing {
            resp.request_focus();
        }
    }
    if capturing {
        // Keep Tab (and the arrows) for the capture: without the lock egui's focus traversal eats
        // Tab before this widget sees it. Esc stays ours (it cancels).
        ui.memory_mut(|m| {
            m.set_focus_lock_filter(
                resp.id,
                egui::EventFilter {
                    tab: true,
                    horizontal_arrows: true,
                    vertical_arrows: true,
                    escape: true,
                },
            )
        });
        let events = ui.input(|i| i.events.clone());
        for ev in events {
            if let Event::Key {
                key,
                pressed: true,
                modifiers,
                ..
            } = ev
            {
                if key == Key::Escape {
                    state.capturing = None;
                    break;
                }
                let mut s = String::new();
                if modifiers.ctrl || modifiers.command {
                    s.push_str("Ctrl+");
                }
                if modifiers.alt {
                    s.push_str("Alt+");
                }
                if modifiers.shift {
                    s.push_str("Shift+");
                }
                s.push_str(key.name());
                if let Ok(Some(h)) = parse_hotkey_named(&s, hotkey_named(row)) {
                    model.set(&id, &h.format());
                    state.capturing = None;
                    break;
                }
            }
        }
    }
    if parse_hotkey_named(&cur, hotkey_named(row))
        .ok()
        .flatten()
        .is_some()
        && ui.small_button(tr("hotkey.clear")).clicked()
    {
        model.set(&id, "none");
        state.capturing = None;
    }
}

/// The Apply / Revert strip under the rows. Apply is disabled while nothing changed or a value is
/// invalid.
#[cfg(test)]
pub fn footer(ui: &mut Ui, model: &Model) -> Action {
    let mut action = Action::None;
    ui.horizontal(|ui| {
        let dirty = model.is_dirty();
        let ok = dirty && !model.has_problems();
        if ui.add_enabled(ok, Button::new(tr("btn.apply"))).clicked() {
            action = Action::Apply;
        }
        if ui
            .add_enabled(dirty, Button::new(tr("btn.revert")))
            .clicked()
        {
            action = Action::Revert;
        }
        if dirty {
            widgets::hint(
                ui,
                &trf(
                    "settings.unsaved",
                    &[("n", &model.dirty_count().to_string())],
                ),
            );
        }
    });
    action
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::model::{MapStore, ProviderOption};
    use crate::settings::schema::{Provider, Schema};

    fn loaded() -> Model {
        let mut m = Model::new(Schema::builtin());
        m.set_provider_options(
            Provider::LangPacks,
            vec![
                ProviderOption {
                    value: String::new(),
                    label: "English".into(),
                },
                ProviderOption {
                    value: "ru".into(),
                    label: "Русский".into(),
                },
            ],
        );
        m.load(
            Some(b"[video]\r\nwindow=borderless\r\n"),
            &MapStore::default(),
        );
        m
    }

    /// Run one egui frame that draws the Settings page for `subtab` and return the model.
    fn frame(m: &mut Model, state: &mut RenderState) {
        let ctx = egui::Context::default();
        theme::apply(&ctx);
        let mut out = ctx.run_ui(egui::RawInput::default(), |ui| {
            show_settings(ui, m, state);
        });
        out.textures_delta.clear();
    }

    #[test]
    fn every_subtab_renders_without_panicking_and_leaves_the_model_clean() {
        let mut m = loaded();
        let mut st = RenderState::default();
        let tabs: Vec<String> = m
            .schema()
            .settings_subtabs()
            .iter()
            .map(|t| t.id.clone())
            .collect();
        assert!(tabs.len() >= 4);
        for t in tabs {
            st.subtab = t;
            frame(&mut m, &mut st);
        }
        assert!(!m.is_dirty(), "drawing alone must never create an edit");
    }

    #[test]
    fn option_labels_come_from_the_string_table_with_a_readable_fallback() {
        let m = loaded();
        let r = m.schema().row("video.window").unwrap();
        assert_eq!(option_label(r, "borderless"), "Borderless");
        assert_eq!(option_label(r, "some_new_mode"), "Some new mode");
    }

    #[test]
    fn problem_messages_are_filled() {
        assert_eq!(
            problem_text(&Problem::OutOfRange {
                min: Some(1024.0),
                max: Some(65535.0)
            }),
            "Enter a number from 1024 to 65535."
        );
        assert_eq!(
            problem_text(&Problem::OutOfRange {
                min: Some(0.1),
                max: Some(10.0)
            }),
            "Enter a number from 0.1 to 10."
        );
    }
}
