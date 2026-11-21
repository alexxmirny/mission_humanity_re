//! The Mods page (DATAEXT1): the installed mods, the active list and its priority, and the one
//! ini key they live in.
//!
//! The launcher's job here is EDITING, not loading: mh.dll re-reads the same `[mods] enabled=`
//! line and re-validates every folder at boot; this page shows its metadata and problems, and
//! never installs, deletes or downloads anything. The heavy work -- the folder enumeration and
//! the manifest reads -- runs on a worker thread (`App::ensure_mods` starts one per game
//! directory, `App::poll_mods` takes the result); what a frame itself does is one
//! directory-equality check and, when the directory changed, one small ini read. With no game
//! folder the page is the same hint every other page shows.

use egui::{RichText, Ui};

use crate::app::App;
use crate::i18n::{tr, trf};
use crate::{theme, widgets};

pub fn show(app: &mut App, ui: &mut Ui) {
    let Some((ini, source)) = app.settings_ini() else {
        widgets::hint(ui, tr("play.no_folder"));
        return;
    };
    app.ensure_mods();

    // Header: where the mods live, and Refresh -- the only other way a rescan starts. No frame
    // ever enumerates the folder.
    ui.horizontal(|ui| {
        let dir = app
            .game_dir()
            .map(|g| g.join(crate::mods::DIR_NAME))
            .map(|d| d.display().to_string())
            .unwrap_or_default();
        // Allocate Refresh first so an arbitrarily long install path cannot push it away.
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            if widgets::gbtn(ui, tr("mods.refresh")).clicked() {
                app.refresh_mods();
            }
            let text = trf("mods.folder_hint", &[("dir", &dir)]);
            ui.add(
                egui::Label::new(RichText::new(&text).size(11.0).color(theme::TEXT_DIM)).truncate(),
            )
            .on_hover_text(text);
        });
    });
    ui.add_space(2.0);
    if app.mods_job.is_some() {
        widgets::hint(ui, tr("mods.scan.scanning"));
    }

    // Whether ANY active row makes the game fall back to stock: a row that cannot mount
    // (missing folder, bad manifest, malformed id) or a duplicate entry -- the DLL refuses the
    // whole set for either. Computed before the rows (the rows may edit the list; egui catches
    // up next frame).
    let too_many = app.mods.pending.ids.len() > crate::mods::ENABLED_MAX;
    if too_many {
        ui.label(
            RichText::new(tr("mods.too_many"))
                .size(11.0)
                .color(theme::ALERT),
        );
    }
    let active_broken = (0..app.mods.pending.ids.len()).any(|i| {
        app.mods.pending.is_duplicate(i) || row_reason(app, &app.mods.pending.ids[i]).is_some()
    });

    active_section(app, ui);
    available_section(app, ui);

    if active_broken {
        ui.add_space(4.0);
        ui.label(
            RichText::new(tr("mods.active_warning"))
                .size(11.0)
                .color(theme::ALERT),
        );
    }

    ui.add_space(6.0);
    // The common Apply/Revert footer (the buttons live in the save bar, on every page) plus the
    // next-boot rule, which is the whole point of the page.
    let detailed = app.settings_state.detailed;
    super::settings::footer(app, ui, &ini, source, detailed);
    widgets::hint(ui, tr("settings.restart_note"));
}

/// The pending list, in file order: the top row is overridden by the one under it, the bottom
/// row wins. Unresolvable ids keep their place (and their reason) until the player removes
/// them -- Apply never silently drops, dedupes or reorders.
fn active_section(app: &mut App, ui: &mut Ui) {
    widgets::heading(ui, tr("mods.active_header"));
    widgets::hint(ui, tr("mods.later_wins"));

    if app.mods.pending.ids.is_empty() && app.mods.catalog.entries.is_empty() {
        let dir = app
            .game_dir()
            .map(|g| g.join(crate::mods::DIR_NAME))
            .map(|d| d.display().to_string())
            .unwrap_or_default();
        widgets::hint(ui, &trf("mods.none", &[("dir", &dir)]));
        return;
    }

    // egui is immediate mode: draw every row, remember the one click, mutate after the loop.
    let mut uncheck = None;
    let mut move_req = None;
    for i in 0..app.mods.pending.ids.len() {
        let id = app.mods.pending.ids[i].clone();
        let duplicate = app.mods.pending.is_duplicate(i);
        let reason = row_reason(app, &id);
        let (name, detail, description) = row_label(app, &id);
        ui.push_id(("active", i), |ui| {
            ui.horizontal(|ui| {
                let mut on = true;
                let shown = bounded_name(&name);
                if ui
                    .checkbox(&mut on, shown.as_str())
                    .on_hover_text(&name)
                    .clicked()
                {
                    uncheck = Some(i); // unticking is how a row leaves the list
                }
                if widgets::gbtn(ui, tr("mods.move_up"))
                    .on_hover_text(tr("mods.move_up_hint"))
                    .clicked()
                {
                    move_req = Some((i, -1));
                }
                if widgets::gbtn(ui, tr("mods.move_down"))
                    .on_hover_text(tr("mods.move_down_hint"))
                    .clicked()
                {
                    move_req = Some((i, 1));
                }
            });
            if !detail.is_empty() {
                hint_line(ui, &detail);
            }
            if duplicate {
                widgets::pill(ui, tr("mods.row.duplicate"), theme::HAZARD);
            }
            if !description.is_empty() {
                hint_line(ui, &description);
            }
            if let Some(reason) = reason {
                ui.label(RichText::new(reason).size(11.0).color(theme::ALERT));
            }
        });
    }
    if let Some(i) = uncheck {
        app.mods.pending.remove(i);
    }
    if let Some((i, delta)) = move_req {
        app.mods.pending.move_row(i, delta);
    }
    ui.add_space(2.0);
}

/// Discovered mods that are not in the pending list. Ticking one appends it to the END of the
/// active list -- later wins, so a freshly enabled mod is the top priority (and easy to demote).
fn available_section(app: &mut App, ui: &mut Ui) {
    let ids: Vec<String> = app
        .mods
        .catalog
        .entries
        .iter()
        .filter(|e| !app.mods.pending.ids.iter().any(|id| id == &e.id))
        .map(|e| e.id.clone())
        .collect();
    if ids.is_empty() {
        return;
    }
    widgets::group_header(ui, tr("mods.available_header"));
    let mut enable = None;
    for id in ids {
        let Some(e) = app.mods.catalog.find(&id) else {
            continue;
        };
        let name = e.name.clone();
        let version = e.version.clone();
        let description = e.description.clone();
        let problem = e.problem.clone();
        ui.push_id(("available", id.clone()), |ui| {
            ui.horizontal(|ui| {
                let mut on = false;
                let shown = bounded_name(&name);
                if ui
                    .checkbox(&mut on, shown.as_str())
                    .on_hover_text(&name)
                    .clicked()
                {
                    enable = Some(id.clone());
                }
            });
            let detail = if version.is_empty() {
                id.clone()
            } else {
                format!("{id} · v{version}")
            };
            hint_line(ui, &detail);
            if !description.is_empty() {
                hint_line(ui, &description);
            }
            if let Some(p) = problem {
                // Bound the args first: `p.args()` is a temporary, and the collected &strs
                // must not outlive it.
                let raw = p.args();
                let args: Vec<(&str, &str)> = raw.iter().map(|(k, v)| (*k, v.as_str())).collect();
                ui.label(
                    RichText::new(trf(p.key(), &args))
                        .size(11.0)
                        .color(theme::ALERT),
                );
            }
        });
    }
    if let Some(id) = enable {
        app.mods.pending.enable(&id);
    }
    ui.add_space(2.0);
}

/// Why this requested id cannot mount: a malformed token, the catalog entry's manifest problem,
/// or -- for an id the scan did not find -- "the folder does not exist".
fn row_reason(app: &App, id: &str) -> Option<String> {
    if !crate::mods::valid_id(id) {
        return Some(tr("mods.problem.bad_id").to_string());
    }
    match app.mods.catalog.find(id) {
        Some(e) => e.problem.as_ref().map(|p| {
            let raw = p.args();
            let args: Vec<(&str, &str)> = raw.iter().map(|(k, v)| (*k, v.as_str())).collect();
            trf(p.key(), &args)
        }),
        None => Some(trf("mods.row.missing", &[("id", id)])),
    }
}

/// (checkbox text, dim detail, description) for a pending row: a healthy mod shows its manifest
/// name and `id · version`; anything else shows the raw id, because that is the token the player
/// must recognise (and fix) in the ini.
fn row_label(app: &App, id: &str) -> (String, String, String) {
    let Some(e) = app.mods.catalog.find(id) else {
        return (id.to_string(), String::new(), String::new());
    };
    if e.problem.is_some() {
        return (id.to_string(), String::new(), String::new());
    }
    let mut detail = Vec::new();
    if e.name != e.id {
        detail.push(e.id.clone());
    }
    if !e.version.is_empty() {
        detail.push(format!("v{}", e.version));
    }
    (e.name.clone(), detail.join(" · "), e.description.clone())
}

// Keep the controls reachable even for legal 64-byte names and malformed ini tokens.
fn bounded_name(name: &str) -> String {
    if name.is_empty() {
        return tr("mods.empty_token").to_string();
    }
    let mut chars = name.chars();
    let prefix: String = chars.by_ref().take(18).collect();
    if chars.next().is_some() {
        format!("{prefix}…")
    } else {
        prefix
    }
}

fn hint_line(ui: &mut Ui, text: &str) {
    ui.add(egui::Label::new(RichText::new(text).size(11.0).color(theme::TEXT_DIM)).truncate())
        .on_hover_text(text);
}
