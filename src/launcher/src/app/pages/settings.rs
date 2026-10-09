//! The Settings page: the declarative engine's sub-tabs and rows, Apply / Revert, and a hint naming
//! the file the edits land in and why that file (`cfgdir`).

use egui::{RichText, Ui};

use crate::app::App;
use crate::i18n::{tr, trf};
use crate::settings::render;
use crate::{theme, widgets};

pub fn show(app: &mut App, ui: &mut Ui) {
    let Some((ini, source)) = app.settings_ini() else {
        widgets::hint(ui, tr("play.no_folder"));
        return;
    };
    app.ensure_model();
    render::show_settings(ui, &mut app.model, &mut app.settings_state);
    ui.add_space(6.0);
    footer(app, ui, &ini, source, true);
}

/// The last Apply's result and where the file is. Shared with the Diagnostics page, whose rows are
/// settings in the same model. Apply / Revert themselves live in ONE place, the save bar above Start
/// game, on every page (shell.rs; user 2026-10-09).
pub fn footer(
    app: &mut App,
    ui: &mut Ui,
    ini: &std::path::Path,
    source: crate::cfgdir::Source,
    show_path: bool,
) {
    if let Some((note, is_err)) = app.settings_note.as_ref() {
        let color = if *is_err { theme::ALERT } else { theme::OK };
        ui.label(RichText::new(note).size(11.0).color(color));
    }
    if !show_path {
        return;
    }
    widgets::hint(
        ui,
        &trf(
            "settings.saved_to",
            &[
                ("path", &ini.display().to_string()),
                ("source", tr(source.i18n_key())),
            ],
        ),
    );
}
