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
//! `restart` -> dim pill, `pending` (debug builds only) -> dim pill. A value the row does not list
//! gets an extra `<v> (custom)` segment/entry and is preserved until the player picks another.
//!
//! The renderer edits `Model` only; Apply/Revert are returned from `footer` as an `Action` for the
//! page to carry out (it owns the launcher store and the ini path).

use egui::{Button, ComboBox, Event, Grid, Key, RichText, TextEdit, Ui};

use super::model::{parse_hotkey, Model, Problem};
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
}

/// What the footer asks the page to do.
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
    let rows: Vec<Row> = model
        .schema()
        .rows_in_subtab(&state.subtab)
        .filter(|r| model.is_visible(r))
        .cloned()
        .collect();
    show_rows(
        ui,
        model,
        state,
        &rows,
        &format!("settings_{}", state.subtab),
    );
}

/// The Diagnostics page's setting rows, grouped by the schema's `group`.
pub fn show_diagnostics(ui: &mut Ui, model: &mut Model, state: &mut RenderState) {
    let groups: Vec<(String, Vec<Row>)> = model
        .schema()
        .diagnostics_groups()
        .into_iter()
        .map(|(g, rows)| (g, rows.into_iter().cloned().collect()))
        .collect();
    for (i, (group, rows)) in groups.iter().enumerate() {
        let rows: Vec<Row> = rows
            .iter()
            .filter(|r| model.is_visible(r))
            .cloned()
            .collect();
        if rows.is_empty() {
            continue;
        }
        if !group.is_empty() {
            let key = format!("diag.group.{group}");
            widgets::group_header(ui, tr(&key));
        }
        show_rows(ui, model, state, &rows, &format!("diag_{group}_{i}"));
        if !group.is_empty() {
            if let Some(h) = tr_opt(&format!("diag.group.{group}.hint")) {
                widgets::hint(ui, h);
            }
        }
    }
}

/// Draw `rows` as a two-column grid (label | control + hint).
pub fn show_rows(ui: &mut Ui, model: &mut Model, state: &mut RenderState, rows: &[Row], id: &str) {
    Grid::new(id)
        .num_columns(2)
        .spacing([10.0, 8.0])
        .min_col_width(150.0)
        .show(ui, |ui| {
            for row in rows {
                label_cell(ui, model, row);
                ui.vertical(|ui| {
                    ui.horizontal_wrapped(|ui| {
                        control(ui, model, state, row);
                        if model.differs_from_default(&row.id) {
                            let b = Button::new(RichText::new("↺").color(theme::TEXT_DIM)).small();
                            if ui.add(b).on_hover_text(tr("settings.reset_row")).clicked() {
                                model.reset_row(&row.id);
                            }
                        }
                    });
                    if let Some(p) = problem_for_row(model, row) {
                        ui.label(
                            RichText::new(problem_text(&p))
                                .size(11.0)
                                .color(theme::ALERT),
                        );
                    }
                    if let Some(h) = tr_opt(&row.hint_key()) {
                        widgets::hint(ui, h);
                    }
                });
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

fn label_cell(ui: &mut Ui, model: &Model, row: &Row) {
    ui.vertical(|ui| {
        ui.set_max_width(160.0);
        ui.horizontal_wrapped(|ui| {
            let mut text = RichText::new(tr(&row.label_key()));
            if model.is_edited(&row.id) {
                text = text.color(theme::TEXT_HI);
            }
            ui.label(text);
        });
        if row.experimental || row.restart || row.pending {
            ui.horizontal_wrapped(|ui| {
                ui.spacing_mut().item_spacing.x = 4.0;
                if row.experimental {
                    widgets::pill(ui, tr("badge.experimental"), theme::ALERT);
                }
                if row.restart {
                    widgets::pill(ui, tr("badge.restart"), theme::TEXT_DIM);
                }
                if row.pending {
                    widgets::pill(ui, tr("badge.pending"), theme::TEXT_DIM);
                }
            });
        }
    });
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
        Kind::Int | Kind::Str => text_box(ui, model, row, id, false),
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
        .width(200.0)
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
        let width = if wide || row.kind != Kind::Int {
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
    let custom = super::model::canon(row, &model.value(&row.id)) == "custom";
    if custom {
        if let Some(comp) = row.companion.clone() {
            ui.end_row();
            ui.horizontal(|ui| {
                ui.label(
                    RichText::new(tr("relay.address"))
                        .size(11.0)
                        .color(theme::TEXT_DIM),
                );
                text_box(ui, model, row, &comp, true);
            });
        }
    }
    let _ = state;
}

fn hotkey(ui: &mut Ui, model: &mut Model, state: &mut RenderState, row: &Row) {
    let id = row.id.clone();
    let cur = model.value(&id);
    let capturing = state.capturing.as_deref() == Some(id.as_str());
    let shown = if capturing {
        tr("hotkey.capture").to_string()
    } else {
        match parse_hotkey(&cur) {
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
    if resp.on_hover_text(tr("hotkey.help")).clicked() {
        state.capturing = if capturing { None } else { Some(id.clone()) };
    }
    if capturing {
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
                if let Ok(Some(h)) = parse_hotkey(&s) {
                    model.set(&id, &h.format());
                    state.capturing = None;
                    break;
                }
            }
        }
    }
    if parse_hotkey(&cur).ok().flatten().is_some() && ui.small_button(tr("hotkey.clear")).clicked()
    {
        model.set(&id, "none");
        state.capturing = None;
    }
}

/// The Apply / Revert strip under the rows. Apply is disabled while nothing changed or a value is
/// invalid.
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
                min: Some(1024),
                max: Some(65535)
            }),
            "Enter a number from 1024 to 65535."
        );
    }
}
