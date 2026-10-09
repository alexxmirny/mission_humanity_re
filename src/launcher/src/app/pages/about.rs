//! The About page: what this is, the licences (embedded in the binary, shown in a scrolling modal),
//! and the two folders a player is asked to open when they report a problem.

use egui::{Context, RichText, ScrollArea, Ui};

use crate::app::App;
use crate::i18n::{tr, trf};
use crate::{theme, widgets};

/// The project's own licence, from the repository root -- built into the launcher so it is readable
/// with no files beside it.
const LICENSE: &str = include_str!("../../../../../LICENSE");
/// Third-party notices (what the project vendors or links, and under which terms).
const THIRD_PARTY: &str = include_str!("../../../../../THIRD_PARTY.md");

/// A document the About modal can show.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Doc {
    License,
    ThirdParty,
}

impl Doc {
    fn text(self) -> &'static str {
        match self {
            Doc::License => LICENSE,
            Doc::ThirdParty => THIRD_PARTY,
        }
    }

    fn title_key(self) -> &'static str {
        match self {
            Doc::License => "about.license",
            Doc::ThirdParty => "about.third_party",
        }
    }
}

pub fn show(app: &mut App, ui: &mut Ui) {
    widgets::heading(ui, tr("about.heading"));
    ui.label(trf("about.text", &[("version", crate::version::VERSION)]));

    widgets::group_header(ui, tr("about.licenses"));
    widgets::hint(ui, tr("about.licenses_text"));
    ui.horizontal_wrapped(|ui| {
        if widgets::gbtn(ui, tr("about.license")).clicked() {
            app.about_doc = Some(Doc::License);
        }
        if widgets::gbtn(ui, tr("about.third_party")).clicked() {
            app.about_doc = Some(Doc::ThirdParty);
        }
    });

    widgets::group_header(ui, tr("about.folders"));
    ui.horizontal_wrapped(|ui| {
        if widgets::gbtn(ui, tr("about.open_settings")).clicked() {
            open_settings_folder(app);
        }
        if widgets::gbtn(ui, tr("about.open_logs")).clicked() {
            open_logs_folder(app);
        }
    });
    if let Some((dir, source)) = app
        .game_dir()
        .map(|d| crate::cfgdir::game_config_dir(&app.layout, &d))
    {
        widgets::hint(
            ui,
            &trf(
                "about.settings_at",
                &[
                    ("path", &dir.display().to_string()),
                    ("source", tr(source.i18n_key())),
                ],
            ),
        );
    }
}

fn open_in_explorer(dir: &std::path::Path) {
    let _ = std::process::Command::new("explorer").arg(dir).spawn();
}

fn open_settings_folder(app: &mut App) {
    let Some(game) = app.game_dir() else {
        app.say_t("start.no_game", &[], true);
        return;
    };
    let (dir, source) = crate::cfgdir::game_config_dir(&app.layout, &game);
    // Opening the folder may create the user-storage one (nothing has written settings yet); it
    // never creates anything beside the exe, whose `mh_net.ini` would flip the game's mode.
    if source != crate::cfgdir::Source::Portable {
        let _ = std::fs::create_dir_all(&dir);
    }
    open_in_explorer(&dir);
}

fn open_logs_folder(app: &mut App) {
    let Some(root) = app.log_root() else {
        app.say_t("start.no_game", &[], true);
        return;
    };
    let _ = std::fs::create_dir_all(&root);
    open_in_explorer(&root);
}

/// The licence modal, drawn by the shell on every page while a document is open.
pub fn doc_modal(app: &mut App, ctx: &Context) {
    let Some(doc) = app.about_doc else {
        return;
    };
    let out = widgets::modal(ctx, "about_doc", tr(doc.title_key()), |ui| {
        ScrollArea::vertical()
            .id_salt("about_doc_text")
            .max_height(300.0)
            .auto_shrink([false, true])
            .show(ui, |ui| {
                ui.label(RichText::new(doc.text()).monospace().size(11.0));
            });
        ui.add_space(6.0);
        widgets::gbtn(ui, tr("btn.close")).clicked()
    });
    if out.inner || out.close_requested {
        app.about_doc = None;
    }
    let _ = theme::TEXT;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_licence_documents_are_built_into_the_binary() {
        assert!(LICENSE.contains("MIT"), "the licence text is the MIT one");
        assert!(THIRD_PARTY.contains("Third-party notices"));
    }
}
