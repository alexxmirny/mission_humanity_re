//! Strategic binding editor: shared Settings model, capture toolbar and scrolling action/role grid.
use crate::i18n::{tr, trf};
use crate::settings::{
    binding_capture::{Capture, Kind, Result as CaptureResult},
    bindings::{self, Action},
    model::Model,
};
use crate::{theme, widgets};
use egui::{vec2, Button, RichText, ScrollArea, TextEdit, Ui};

#[derive(Default)]
pub struct State {
    search: String,
    detailed: bool,
    pub(crate) category: String,
    capture: Option<Capture>,
    reset_prompt: bool,
    note: Option<String>,
    block_rows_this_frame: bool,
}
impl State {
    pub fn cancel(&mut self) {
        self.capture = None;
        self.reset_prompt = false;
    }
    pub fn active(&self) -> bool {
        self.capture.is_some()
    }
    /// Shell entry, before menu/search/button rendering. Raw capture owns this frame's key.
    pub fn pre_input(&mut self, ctx: &egui::Context, model: &mut Model) {
        self.block_rows_this_frame = self.capture.is_some();
        let result = self.capture.as_mut().and_then(|capture| capture.poll(ctx));
        let id = self.capture.as_ref().map(|capture| capture.id.clone());
        match (result, id) {
            (Some(CaptureResult::Value(value)), Some(id)) => {
                model.set(&id, &value);
                self.capture = None;
                self.note = None;
            }
            (Some(CaptureResult::Cancel), _) => self.capture = None,
            (Some(CaptureResult::Unsupported), _) => {
                self.note = Some(tr("keys.unsupported_capture").into())
            }
            _ => (),
        }
    }
}

fn category_label(category: &str) -> String {
    tr(&format!("keys.category.{category}")).to_string()
}
fn action_matches(action: &Action, category: &str, query: &str) -> bool {
    action.editable
        && (category.is_empty()
            || category == "all"
            || category == action.category
            || (category == "selection" && action.category == "groups")
            || (category == "commands"
                && !["camera", "groups"].contains(&action.category.as_str())))
        && (query.is_empty()
            || format!(
                "{} {} {} {} {}",
                action.label_en, action.label_ru, action.id, action.context, action.category
            )
            .to_lowercase()
            .contains(query))
}
fn begin(state: &mut State, ui: &mut Ui, id: String, kind: Kind, focus_id: egui::Id) {
    state.capture = None; // Unhook old capture BEFORE installing its replacement.
    match Capture::begin(id, kind, focus_id, ui.ctx()) {
        Ok(capture) => {
            state.capture = Some(capture);
            state.note = None;
            ui.memory_mut(|m| m.request_focus(focus_id));
        }
        Err(error) => state.note = Some(trf("keys.capture_failed", &[("error", &error)])),
    }
}
fn key_button(ui: &mut Ui, model: &Model, state: &mut State, id: String, kind: Kind, width: f32) {
    let active = state
        .capture
        .as_ref()
        .is_some_and(|capture| capture.id == id);
    let raw = model.value(&id);
    let value = bindings::normalize_row(&id, &raw).unwrap_or(raw);
    let text = if active {
        tr("keys.listening").to_string()
    } else if kind == Kind::Modifier && bindings::modifier(&value) == Some(0) {
        tr("keys.disabled").into()
    } else {
        bindings::display(&value)
    };
    let response = ui.add_sized([width, 23.0], Button::new(text).selected(active));
    if response.clicked() {
        if active {
            state.cancel();
        } else {
            begin(state, ui, id, kind, response.id);
        }
    }
}
fn row_reset(ui: &mut Ui, model: &mut Model, state: &mut State, ids: &[String]) {
    let changed = ids.iter().any(|id| model.differs_from_default(id));
    let reset = ui
        .add_enabled_ui(changed, |ui| {
            ui.spacing_mut().button_padding.x = 3.0;
            ui.add_sized([24.0, 23.0], Button::new("↻"))
        })
        .inner;
    if reset.on_hover_text(tr("keys.reset_row")).clicked() {
        state.cancel();
        for id in ids {
            model.reset_row(id);
        }
    }
}
fn key_columns(ui: &Ui, available: f32) -> (f32, f32) {
    let usable = available - 24.0 - 3.0 * ui.spacing().item_spacing.x;
    let button = ((usable - 110.0) * 0.5).clamp(85.0, 128.0);
    ((usable - 2.0 * button).max(80.0), button)
}
fn role_label_width(ui: &Ui, available: f32) -> f32 {
    (available - 175.0 - 24.0 - 2.0 * ui.spacing().item_spacing.x).max(90.0)
}
fn detail(action: &Action) -> String {
    let contexts = action
        .context
        .split('|')
        .map(|context| tr(&format!("keys.context.{context}")))
        .collect::<Vec<_>>()
        .join(", ");
    trf(
        "keys.action_detail",
        &[
            ("context", &contexts),
            ("kind", tr(&format!("keys.kind.{}", action.kind))),
        ],
    )
}
fn action_row(ui: &mut Ui, model: &mut Model, state: &mut State, action: &Action) {
    let (label_w, button_w) = key_columns(ui, ui.available_width());
    let ids: Vec<String> = action.bindings.iter().map(|slot| slot.row_id()).collect();
    let dirty = ids.iter().any(|id| model.is_edited(id));
    let label = if dirty {
        format!("{} •", action.label())
    } else {
        action.label().to_string()
    };
    let response = ui
        .vertical(|ui| {
            for (index, pair) in ids.chunks(2).enumerate() {
                ui.horizontal(|ui| {
                    ui.add_sized(
                        [label_w, 23.0],
                        egui::Label::new(if index == 0 {
                            label.as_str()
                        } else {
                            tr("keys.additional_slots")
                        })
                        .wrap(),
                    );
                    for id in pair {
                        key_button(ui, model, state, id.clone(), Kind::Key, button_w);
                    }
                    if pair.len() == 1 {
                        ui.allocate_space(vec2(button_w, 23.0));
                    }
                    if index == 0 {
                        row_reset(ui, model, state, &ids);
                    } else {
                        ui.allocate_space(vec2(24.0, 23.0));
                    }
                });
            }
        })
        .response;
    response.on_hover_text(detail(action));
    if state.detailed {
        widgets::hint(ui, &detail(action));
    }
}
fn derived_help(ui: &mut Ui, model: &Model) {
    widgets::hint(ui, tr("keys.derived_title"));
    widgets::hint(ui, tr("keys.disabled_help"));
    for (key, names) in [
        (
            "keys.gesture_queue",
            vec!["shift_modifier", "command_button"],
        ),
        ("keys.gesture_patrol", vec!["patrol", "command_button"]),
        (
            "keys.gesture_queue_patrol",
            vec!["shift_modifier", "patrol", "command_button"],
        ),
        ("keys.gesture_confirm", vec!["confirm", "command_button"]),
        (
            "keys.gesture_force",
            vec!["confirm", "shift_modifier", "command_button"],
        ),
        (
            "keys.gesture_repeat",
            vec!["shift_modifier", "select_button"],
        ),
        ("keys.gesture_add", vec!["shift_modifier", "select_button"]),
        (
            "keys.gesture_toggle",
            vec!["toggle_selection", "select_button"],
        ),
        ("keys.gesture_reveal", vec!["shift_modifier"]),
        ("keys.gesture_double", vec!["command_button"]),
    ] {
        let gesture = if names.iter().all(|id| bindings::role_enabled(model, id)) {
            names
                .iter()
                .map(|id| bindings::role_value(model, id))
                .collect::<Vec<_>>()
                .join(" + ")
        } else {
            tr("keys.disabled").to_string()
        };
        ui.label(
            RichText::new(trf(key, &[("gesture", &gesture)]))
                .size(11.0)
                .color(theme::TEXT_DIM),
        );
    }
}
fn capture_controls(ui: &mut Ui, model: &mut Model, state: &mut State) {
    let Some(capture) = state.capture.as_ref() else {
        return;
    };
    let id = capture.id.clone();
    let kind = capture.kind;
    ui.separator();
    let owner_min = ui.next_widget_position();
    let owner_width = ui.available_width();
    widgets::hint(
        ui,
        tr(match kind {
            Kind::Key => "keys.capture_key",
            Kind::Modifier => "keys.capture_modifier",
            Kind::Button => "keys.capture_button",
        }),
    );
    let focus = ui.add_sized(
        [ui.available_width(), 32.0],
        Button::new(tr("keys.listening")).selected(true),
    );
    if let Some(capture) = state.capture.as_mut() {
        capture.mouse_rect = focus.rect;
        if capture.focus_id != focus.id {
            capture.focus_id = focus.id;
            focus.request_focus();
        }
    }
    let mut selected = None;
    ui.horizontal_wrapped(|ui| {
        if kind == Kind::Key {
            if widgets::gbtn(ui, tr("keys.assign_escape")).clicked() {
                selected = Some("Escape");
            }
            if widgets::gbtn(ui, tr("keys.clear")).clicked() {
                selected = Some("none");
            }
        }
        if kind == Kind::Modifier && widgets::gbtn(ui, tr("keys.disable_role")).clicked() {
            selected = Some("none");
        }
        if widgets::gbtn(ui, tr("btn.cancel")).clicked() {
            selected = Some("");
        }
    });
    if let Some(capture) = state.capture.as_mut() {
        capture.owner_rect = egui::Rect::from_min_max(
            owner_min,
            egui::pos2(owner_min.x + owner_width, ui.next_widget_position().y),
        );
    }
    if let Some(value) = selected {
        if !value.is_empty() {
            model.set(&id, value);
        }
        state.capture = None;
    }
}
pub fn show_model(ui: &mut Ui, model: &mut Model, state: &mut State) {
    ui.label(RichText::new(tr("keys.heading")).small().color(theme::OK));
    widgets::hint(ui, tr("keys.next_start"));
    let old_search = state.search.clone();
    let search = ui.add_sized(
        [ui.available_width(), 24.0],
        TextEdit::singleline(&mut state.search).hint_text(tr("keys.search")),
    );
    if search.has_focus() || old_search != state.search {
        state.cancel();
    }
    ui.horizontal_wrapped(|ui| {
        for category in ["all", "camera", "commands", "selection", "modifiers"] {
            let selected =
                state.category == category || (category == "all" && state.category.is_empty());
            if ui
                .selectable_label(selected, category_label(category))
                .clicked()
            {
                state.cancel();
                state.category = category.to_string();
            }
        }
    });
    ui.horizontal(|ui| {
        ui.selectable_value(&mut state.detailed, false, tr("settings.view.compact"));
        ui.selectable_value(&mut state.detailed, true, tr("settings.view.detailed"));
    });
    let capturing_this_frame = state.active() || state.block_rows_this_frame;
    capture_controls(ui, model, state);
    if let Some(note) = state.note.as_ref() {
        ui.label(RichText::new(note).small().color(theme::HAZARD));
    }
    let problems = bindings::issues(model);
    let navigation_migration = problems
        .iter()
        .any(|issue| issue.ids.iter().any(|id| id == bindings::VERSION_ID));

    if navigation_migration {
        for issue in problems
            .iter()
            .filter(|issue| issue.ids.iter().any(|id| id == bindings::VERSION_ID))
        {
            ui.label(RichText::new(&issue.text).small().color(theme::ALERT));
        }
        if widgets::gbtn(ui, tr("keys.accept_navigation")).clicked() {
            state.cancel();
            model.set(bindings::VERSION_ID, "2");
        }
    }
    let query = state.search.trim().to_lowercase();
    ui.horizontal(|ui| {
        let available = ui.available_width() - 12.0;
        let roles = state.category == "modifiers";
        let (key_label_w, button_w) = key_columns(ui, available);
        let label_w = if roles {
            role_label_width(ui, available)
        } else {
            key_label_w
        };
        ui.add_sized(
            [label_w, 16.0],
            egui::Label::new(
                RichText::new(tr("keys.column_action"))
                    .small()
                    .color(theme::TEXT_DIM),
            ),
        );
        if roles {
            ui.add_sized(
                [175.0, 16.0],
                egui::Label::new(
                    RichText::new(tr("keys.column_assignment"))
                        .small()
                        .color(theme::TEXT_DIM),
                ),
            );
        } else {
            ui.add_sized(
                [button_w, 16.0],
                egui::Label::new(
                    RichText::new(tr("keys.column_primary"))
                        .small()
                        .color(theme::TEXT_DIM),
                ),
            );
            ui.add_sized(
                [button_w, 16.0],
                egui::Label::new(
                    RichText::new(tr("keys.column_alternate"))
                        .small()
                        .color(theme::TEXT_DIM),
                ),
            );
        }
    });
    let rows_h = (ui.available_height() - 66.0).max(30.0);
    ui.add_enabled_ui(!capturing_this_frame, |ui| {
        ui.allocate_ui_with_layout(
            vec2(ui.available_width(), rows_h),
            egui::Layout::top_down(egui::Align::Min),
            |ui| {
                ScrollArea::vertical()
                    .id_salt("strategic_bindings_scroll")
                    .max_height(rows_h)
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        if state.category != "modifiers" {
                            let mut category = "";
                            for action in &bindings::catalog().actions {
                                if !action_matches(action, &state.category, &query) {
                                    continue;
                                }
                                if action.category != category {
                                    category = &action.category;
                                    ui.separator();
                                    ui.label(
                                        RichText::new(category_label(category))
                                            .small()
                                            .color(theme::OK),
                                    );
                                }
                                ui.push_id(&action.id, |ui| action_row(ui, model, state, action));
                                for issue in problems.iter().filter(|issue| {
                                    action
                                        .bindings
                                        .iter()
                                        .any(|slot| issue.ids.contains(&slot.row_id()))
                                }) {
                                    ui.label(
                                        RichText::new(&issue.text).small().color(theme::ALERT),
                                    );
                                }
                            }
                        }
                        if state.category == "modifiers"
                            || state.category.is_empty()
                            || state.category == "all"
                        {
                            ui.separator();
                            for role in &bindings::catalog().roles {
                                if !query.is_empty()
                                    && !format!("{} {} {}", role.label_en, role.label_ru, role.id)
                                        .to_lowercase()
                                        .contains(&query)
                                {
                                    continue;
                                }
                                ui.push_id(&role.id, |ui| {
                                    ui.horizontal(|ui| {
                                        let label_w = role_label_width(ui, ui.available_width());
                                        let label = if model.is_edited(&role.row_id()) {
                                            format!("{} •", role.label())
                                        } else {
                                            role.label().to_string()
                                        };
                                        ui.allocate_ui_with_layout(
                                            vec2(label_w, 23.0),
                                            egui::Layout::left_to_right(egui::Align::Center),
                                            |ui| {
                                                ui.set_min_width(label_w);
                                                ui.add(egui::Label::new(label).wrap())
                                            },
                                        );
                                        key_button(
                                            ui,
                                            model,
                                            state,
                                            role.row_id(),
                                            if role.kind == "modifier" {
                                                Kind::Modifier
                                            } else {
                                                Kind::Button
                                            },
                                            175.0,
                                        );
                                        row_reset(ui, model, state, &[role.row_id()]);
                                    });
                                });
                                for issue in problems
                                    .iter()
                                    .filter(|issue| issue.ids.contains(&role.row_id()))
                                {
                                    ui.label(
                                        RichText::new(&issue.text).small().color(theme::ALERT),
                                    );
                                }
                            }
                            ui.separator();
                            derived_help(ui, model);
                        }
                        ui.separator();
                        widgets::hint(ui, tr("keys.reference"));
                    });
            },
        );
    });
    ui.horizontal(|ui| {
        let text = if problems.is_empty() {
            tr("keys.no_conflicts").to_string()
        } else {
            trf("keys.problem_count", &[("n", &problems.len().to_string())])
        };
        ui.label(RichText::new(text).small().color(if problems.is_empty() {
            theme::OK
        } else {
            theme::ALERT
        }));
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            if widgets::gbtn(ui, tr("keys.reset_all")).clicked() {
                state.capture = None;
                state.reset_prompt = true;
            }
        });
    });
    if state.reset_prompt {
        let out = widgets::modal(ui.ctx(), "reset_bindings", tr("keys.reset_all"), |ui| {
            ui.label(tr("keys.reset_confirm"));
            let mut answer = None;
            ui.horizontal(|ui| {
                if widgets::gbtn(ui, tr("keys.stage_defaults")).clicked() {
                    answer = Some(true);
                }
                if widgets::gbtn(ui, tr("btn.cancel")).clicked() {
                    answer = Some(false);
                }
            });
            answer
        });
        if let Some(answer) = out.inner {
            if answer {
                bindings::reset_all(model);
            }
            state.reset_prompt = false;
        } else if out.close_requested {
            state.reset_prompt = false;
        }
    }
    // Physical input causes an egui repaint through winit as usual. Also poll while capturing:
    // unsupported low-level keys may not have an egui Key equivalent.
    if state.active() {
        ui.ctx()
            .request_repaint_after(std::time::Duration::from_millis(16));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::i18n::{self, Locale};
    use crate::settings::{model::MapStore, schema::Schema};
    use egui_kittest::{kittest::Queryable, Harness};
    fn harness(
        locale: Locale,
        category: &str,
        ini: Option<&[u8]>,
    ) -> Harness<'static, (Model, State)> {
        i18n::set_locale(locale);
        let mut model = Model::new(Schema::builtin());
        model.load(ini, &MapStore::default());
        let state = State {
            category: category.into(),
            ..State::default()
        };
        Harness::builder()
            .with_size(vec2(540.0, 490.0))
            .build_ui_state(
                |ui, (model, state)| {
                    theme::apply(ui.ctx());
                    state.pre_input(ui.ctx(), model);
                    show_model(ui, model, state);
                },
                (model, state),
            )
    }
    #[test]
    fn both_locales_show_primary_alias_slots_and_separate_role_page() {
        for locale in Locale::ALL {
            let mut h = harness(locale, "camera", None);
            h.run();
            assert!(h.query_by_label("Up").is_some());
            assert!(h.query_by_label("Numpad8").is_some());
            assert!(h.query_by_label(tr("keys.column_primary")).is_some());
            h.state_mut().1.category = "modifiers".into();
            h.run();
            assert!(h.query_by_label(tr("keys.column_assignment")).is_some());
            assert!(h
                .query_by_label(bindings::catalog().roles[0].label())
                .is_some());
            assert!(!h.state().0.is_dirty(), "navigation writes nothing");
        }
    }
    #[test]
    fn reset_all_confirmation_stages_defaults_and_shared_revert_restores_disk_values() {
        let mut h = harness(
            Locale::En,
            "modifiers",
            Some(b"[keys]\r\nrole_shift_modifier=Ctrl\r\n"),
        );
        h.run();
        h.get_by_label(tr("keys.reset_all")).click();
        h.run();
        assert_eq!(
            h.state().0.value("keys.role_shift_modifier"),
            "Ctrl",
            "confirmation is not an edit"
        );
        h.get_by_label(tr("keys.stage_defaults")).click();
        h.run();
        assert_eq!(h.state().0.value("keys.role_shift_modifier"), "Shift");
        assert!(h.state().0.is_dirty());
        assert_eq!(h.state().0.raw("keys.role_shift_modifier"), Some("Ctrl"));
        h.state_mut().0.revert();
        h.run();
        assert_eq!(h.state().0.value("keys.role_shift_modifier"), "Ctrl");
    }
}
