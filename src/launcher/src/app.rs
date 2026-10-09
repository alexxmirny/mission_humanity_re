//! The window: a menu column on the left, a framed page on the right (dist RL12/RL14).
//!
//! This file is the CONTROL half: the `App` state, the child process and its crash channel, the
//! update/report/upload jobs and their polls, launch and install. Everything that DRAWS lives in
//! `app/`: `app/shell.rs` (menu column, Start button, frame, footer, modals) and
//! `app/pages/{play,settings,report,about,diagnostics}.rs`, one function per page that takes
//! `&mut App` -- so a page can reach the state it shows (child modules see this file's private
//! fields) without this file knowing what a page looks like.
//!
//! Pages (dist RL14; the old Status/Launch/Report tabs, LA1/LA6, were folded in):
//! **Play** (the front page: what is installed, the update, the game folder), **Settings** (the
//! declarative engine in `settings/`), **Report a problem** (a match list, a description, a consent
//! screen -- nothing is sent without it), **About** (licences, folders) and **Diagnostics** (log
//! level, Compatibility, build info). The one **Start game** button lives in the shell, not on a
//! page: it writes the relay the signed manifest names into the game's `mh_net.ini` + `mh_key.txt`
//! (`relay.rs`) and starts the game. Hosting or joining is decided in the game's own menu
//! afterwards (create a game, or browse the relay's list -- mp:R2/R7).
//!
//! Everything here is immediate-mode: there is no retained widget tree, so a view is a function of
//! `self` and the frame draws whatever the state currently says. The only thing that has to be
//! remembered across frames is the running child (`launch::Session`), which is polled once per
//! frame rather than waited on -- blocking the UI thread on `wait()` would freeze the window for
//! the length of the game session and make the launcher look like the thing that hung.

mod pages;
mod shell;

use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::cfgdir;
use crate::config::Config;
use crate::crash::{self, Marker};
use crate::discord;
use crate::elevate;
use crate::i18n::{self, tr, trf, Locale};
use crate::install;
use crate::launch::{self, Finished};
use crate::log;
use crate::migrate; // RL4
use crate::paths::{self, Layout};
use crate::procs; // RL9
use crate::relay::{self, Relay};
use crate::report;
use crate::settings::ini_io;
use crate::settings::model::Model;
use crate::settings::providers;
use crate::settings::render::RenderState;
use crate::settings::schema::{Provider, Schema};
use crate::settings::store::ConfigStore;
use crate::update::{self, Applied, Offer, Readiness, Releases, SelfUpdate};
use crate::upload::{self, Outbox, Prepared};

/// The pages (dist RL14). One menu entry each; the window opens on `Play`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Page {
    Play,
    Settings,
    Report,
    About,
    Diagnostics,
}

/// What `main.rs` and the restart strip call a page. (It was `View` -- Status / Launch / Report --
/// until RL14; the scripted `--view` names and the old variant spelling still work.)
pub type View = Page;

impl Page {
    /// Menu order. Play first: it is the front page (dist LA6).
    pub const ALL: [Page; 5] = [
        Page::Play,
        Page::Settings,
        Page::Report,
        Page::About,
        Page::Diagnostics,
    ];

    /// The old name of `Play` (dist LA6 called the front page the "launch" view); `main.rs` still
    /// spells it this way.
    #[allow(non_upper_case_globals)]
    pub const Launch: Page = Page::Play;

    /// The English name, which is also the `--view` spelling (`restart_argv` lowercases it). The
    /// window shows `tr("menu.<x>")`, never this.
    pub fn title(self) -> &'static str {
        match self {
            Page::Play => "Play",
            Page::Settings => "Settings",
            Page::Report => "Report",
            Page::About => "About",
            Page::Diagnostics => "Diagnostics",
        }
    }

    /// `--view <name>`. `status`, `update`, `launch` and `home` are the pre-RL14 names; the Status
    /// tab's contents now live on Play (and its maintenance bits on Diagnostics).
    pub fn parse(s: &str) -> Option<Page> {
        match s.trim().to_ascii_lowercase().as_str() {
            "play" | "launch" | "status" | "update" | "home" => Some(Page::Play),
            "settings" | "options" => Some(Page::Settings),
            "report" | "reports" => Some(Page::Report),
            "about" => Some(Page::About),
            "diagnostics" | "diag" => Some(Page::Diagnostics),
            _ => None,
        }
    }

    /// The i18n key of the menu entry.
    pub fn menu_key(self) -> &'static str {
        match self {
            Page::Play => "menu.play",
            Page::Settings => "menu.settings",
            Page::Report => "menu.report",
            Page::About => "menu.about",
            Page::Diagnostics => "menu.diagnostics",
        }
    }

    /// The i18n key of the title in the frame's hazard bar.
    pub fn title_key(self) -> &'static str {
        match self {
            Page::Play => "page.play.title",
            Page::Settings => "page.settings.title",
            Page::Report => "page.report.title",
            Page::About => "page.about.title",
            Page::Diagnostics => "page.diagnostics.title",
        }
    }
}

/// What an update run is being asked to do (dist LA2).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UpdateAction {
    /// Fetch and verify the manifest, and say what it offers. Downloads nothing else. `--check-update`.
    Check,
    /// dist LA12: the same fetch, made by EVERY launcher start, whose answer is the Play page's
    /// "<version> is available -- Update" line. Never an error line: "up to date" is an answer.
    AutoCheck,
    /// The above, then download, verify and install the chosen configuration when the manifest
    /// is newer than the receipt (or the configuration is missing). `--update`, and the game half
    /// of `Update` -- the half a replacement launcher runs after a self-update (dist LA11).
    Apply,
    /// The above, but for the launcher executable itself. `--self-update`.
    SelfUpdate,
    /// dist LA12: THE ONE UPDATE BUTTON, and `--update --self-update`: the launcher first (health
    /// gate, restart with `--update` owed), then the game -- one press, one progress line, one
    /// verdict. When the launcher is current the game half runs in this process.
    Update,
    /// dist LA8: Play on a directory that does not hold the chosen configuration --
    /// install it (uninstalling another one first), provision the relay, then launch.
    MakeReady,
    /// dist RL9: the SILENT update (`App::tick_auto_update`): stage a newer launcher, install a
    /// newer game over the installed one, never while `mh.exe` runs from this folder.
    AutoUpdate,
}

impl UpdateAction {
    /// The player's-language form of `describe` (the Play page's progress line).
    fn describe_t(self) -> &'static str {
        tr(match self {
            UpdateAction::Check | UpdateAction::AutoCheck => "action.check",
            UpdateAction::Apply => "action.apply",
            UpdateAction::SelfUpdate => "action.self_update",
            UpdateAction::Update => "action.update",
            UpdateAction::MakeReady => "action.make_ready",
            UpdateAction::AutoUpdate => "action.auto_update",
        })
    }

    /// English, for the log (a support reader must be able to grep it, whatever the UI language).
    fn describe(self) -> &'static str {
        match self {
            UpdateAction::Check => "checking for an update",
            UpdateAction::AutoCheck => "checking for an update (every start)",
            UpdateAction::Apply => "updating the game",
            UpdateAction::SelfUpdate => "updating the launcher",
            UpdateAction::Update => "updating the launcher, then the game",
            UpdateAction::MakeReady => "getting the game ready to play",
            UpdateAction::AutoUpdate => "updating silently",
        }
    }
}

/// What an update run produced. Richer than a string because the UI thread has to WRITE something
/// down afterwards -- the new installed version, the marker decision -- and a run that reported only
/// prose would leave that to be re-derived from a sentence.
enum Outcome {
    Checked(Box<Releases>),
    Applied(Box<Applied>),
    SelfUpdated(SelfUpdate),
    /// dist LA8: `MakeReady` found the chosen configuration already installed.
    Ready,
    /// dist LA12: the automatic check's answer -- what the manifest offers over what is here.
    Offered(Offer),
    /// dist LA12: `Update`/`Apply` found the launcher AND the game current; nothing was touched.
    UpToDate,
    /// dist RL9: what one silent run found and did (a staged launcher, an installed game, a
    /// deferral, non-fatal errors).
    Auto(Box<update::AutoRun>),
}

/// One update run, on its own thread.
///
/// NOT ON THE UI THREAD, and that is not a nicety: a release zip is several megabytes over whatever
/// connection the player has, and `run_native` cannot paint while a frame callback is inside a
/// socket read. A launcher that froze for the length of its own download would look exactly like
/// the thing that hung, which is the impression this program exists to avoid giving.
struct Job {
    action: UpdateAction,
    rx: Receiver<Result<Outcome, String>>,
    done: bool,
    /// dist LA8: what the thread is doing right now ("downloading …"), for the Play page to show
    /// in place. Written by the thread through `update::Progress`, read every repaint.
    progress: Arc<Mutex<String>>,
    /// Started from the Play page (the Play button, or the Update button beside the offer), so
    /// its progress and its verdict belong there.
    from_play: bool,
}

/// Startup actions, from the command line. See `main.rs` for why they exist.
#[derive(Clone, Default)]
pub struct Startup {
    pub install_zip: Option<PathBuf>,
    pub launch: bool,
    pub uninstall: bool,
    pub exit_after_launch: bool,
    pub requested_size: [f32; 2],
    /// dist LA2: the update run to start with, if any.
    pub update: Option<UpdateAction>,
    pub exit_after_update: bool,
    /// What a replacement launcher is started with after a self-update. Computed in `main.rs`.
    pub restart_args: Vec<String>,
    /// dist LA4: build a report into this file at startup and close. The description comes from
    /// `--description`; an empty one is refused exactly as the view refuses it.
    pub report_to: Option<PathBuf>,
    pub description: String,
    pub with_minidump: bool,
    pub exit_after_report: bool,
    /// dist RP1: the `--report-url`/`--report-ca`/`--report-pin` set, already resolved (and
    /// already failed, if it is going to). `None` means "use the collector compiled into this
    /// build" -- which is what a released launcher always does.
    pub collector_override: Option<Result<upload::Collector, String>>,
    /// dist LA7: the saved game directory no longer holds `mh.exe`. Computed in `main.rs` by
    /// `paths::resolve_game_dir`; shown on the status line from the first frame, so a path that
    /// stopped working is named rather than silently carried.
    pub game_dir_notice: Option<String>,
}

pub struct App {
    layout: Layout,
    config: Config,
    view: View,
    /// The theme (and the locale) are installed on the first frame, once.
    theme_done: bool,
    /// Free-text edit buffer for the game directory, so typing a path is possible without a dialog.
    game_dir_edit: String,
    zip_edit: String,
    /// The last thing that happened, good or bad, shown under the controls.
    status_line: String,
    status_is_error: bool,
    session: Option<launch::Session>,
    /// dist RL20: the Discord presence worker, alive exactly as long as `session`.
    presence: Option<discord::Handle>,
    last_run: Option<Finished>,
    /// dist RL9 / RL4: the silent auto-update's schedule, the staged launcher awaiting its restart
    /// (`auto.pending_restart`), and the migration's once-per-run latch. One field on purpose.
    auto: update::AutoState,
    description: String,
    startup: Startup,
    startup_done: bool,
    /// The update run in flight, if any.
    job: Option<Job>,
    /// The last thing the update half said, kept separate from `status_line` so a launch message
    /// does not wipe out "an update is available".
    update_line: String,
    update_is_error: bool,
    update_base_edit: String,

    // ---- dist LA8 -------------------------------------------------------------------------
    /// Play was pressed on a directory that first needed an install: the launch that is
    /// waiting for the `MakeReady` job, holding the text the log will say the player pressed.
    pending_launch: Option<String>,
    /// The last finished job was started from the Play page, so its verdict is shown there.
    last_job_from_play: bool,
    /// dist LA12: what the last accepted manifest offers this machine (`update::Offer`), from the
    /// check every start makes or from any later job. `None` until a check has answered.
    offer: Option<Offer>,
    /// Logged once, and again whenever the window is resized: the size clause of dist LA1 is about
    /// what the window ACTUALLY became, which is not always what was asked for.
    last_logged_size: Option<[u32; 2]>,

    // ---- dist LA6 -------------------------------------------------------------------------
    /// The relay of the last ACCEPTED manifest (`update::load_accepted`), re-read whenever a check
    /// succeeds. Cached because reading it means re-verifying a signature, which is not a per-frame
    /// operation; `None` means "no manifest accepted yet, or it names no relay" -- either way the
    /// game's configuration is left alone.
    relay: Option<Relay>,
    /// Which version's manifest that relay came from, for the front page to say.
    relay_from: String,

    // ---- dist LA4 -------------------------------------------------------------------------
    /// The crash channel of the run in flight. Created just before the game starts and dropped
    /// when the session ends, so a game started without the launcher's knowledge never blocks on
    /// an acknowledgement.
    channel: Option<crash::Channel>,
    /// What the last run's crash handler wrote, if it crashed.
    marker: Option<Marker>,
    /// The dump written for that crash, if one was written.
    dump: Option<PathBuf>,
    /// The checkbox. Defaults ON when there is a dump to send, because the dump is the only part
    /// of a crash report that says WHERE -- but it is the player's call, which is why it is a
    /// checkbox and not an assumption (plan decision D13).
    include_dump: bool,
    /// The last report built, so the view can point at it.
    built: Option<report::Built>,

    // ---- dist RP1 -------------------------------------------------------------------------
    /// The report the player has been shown and has not yet agreed to send. **Nothing is uploaded
    /// while this is `Some`** -- it IS the consent gate: the send button does not exist until the
    /// contents have been listed, and pressing it is the only thing that starts a socket.
    consent: Option<Prepared>,
    /// The upload in flight, on its own thread for the reason the update `Job` is: a 40 MB POST
    /// inside a frame callback is a window that has stopped painting.
    upload_job: Option<UploadJob>,
    /// What the outbox held when this launcher started, re-read after every send. This is the
    /// "re-offered on the next launch" half of RP1: the Report view shows it unprompted.
    outbox_pending: Vec<PathBuf>,
    upload_line: String,
    upload_is_error: bool,

    // ---- dist LA9 -------------------------------------------------------------------------
    /// This launcher process's own UTC start stamp (`stamp_for_file`'s shape), so a report can
    /// tell `report::plan_logs` "everything since I started" instead of falling back to the
    /// newest-N. Captured once, at construction -- it names WHEN, not where, so nothing about a
    /// later game directory change invalidates it.
    started_utc: String,
    /// The session directory the player picked in the Report view's picker, by leaf name. `None`
    /// means "use the newest", `report::default_session_dir`'s existing meaning -- a player who
    /// never opens the picker gets exactly that.
    session_pick: Option<String>,
    /// dist RL14: the match list the Report page shows, newest first, and when it was read. Re-read
    /// every couple of seconds while that page is up, and whenever a game exits or a report lands.
    matches: Vec<report::MatchRow>,
    matches_at: Option<std::time::Instant>,

    // ---- dist LA17 ------------------------------------------------------------------------
    /// The report build in flight, on its own thread (a state recording is 100-250 MB to deflate;
    /// on the UI thread that was a window "Not responding"). `None` when idle.
    report_job: Option<report::Job>,
    /// `--report <zip> --exit-after-report`: close once the build that was started for it finishes.
    exit_after_report_job: bool,

    // ---- dist RL14 ------------------------------------------------------------------------
    /// The settings engine's model over the game's `mh_net.ini` + `launcher.toml`, and the per-page
    /// state of its renderer. `model_for` is the game directory it was loaded for (`None` = reload).
    model: Model,
    settings_state: RenderState,
    model_for: Option<PathBuf>,
    /// What the last Apply said (`true` = it failed).
    settings_note: Option<(String, bool)>,
    /// Start was pressed with unsaved settings: the Apply / Discard / Cancel prompt is up.
    start_prompt: bool,
    /// The window was closed with unsaved settings: the same three answers, then the close.
    close_prompt: bool,
    /// The close prompt was answered (or nothing was unsaved): the next close goes through.
    close_confirmed: bool,
    /// About: the licence document being read, if one is open.
    about_doc: Option<pages::about::Doc>,
    /// The crash marker file of this session's crash (`marker` is its parsed text): kept so Dismiss
    /// and a sent report can put the `.reported` flag beside it (the RL5 prune contract).
    marker_path: Option<PathBuf>,
    /// From the last accepted game manifest (`refresh_relay`): what the Play page and Diagnostics
    /// say about it.
    accepted: Accepted,
    /// Cached Diagnostics facts that cost a read: the receipt is cheap, these are not.
    last_copied: Option<std::time::Instant>,
}

/// What the Play and Diagnostics pages show of the last accepted game manifest.
#[derive(Clone, Debug, Default)]
pub struct Accepted {
    pub version: String,
    pub issued_at: String,
    pub notes_url: String,
}

/// One upload, on its own thread.
struct UploadJob {
    zip: PathBuf,
    rx: Receiver<Result<upload::Accepted, String>>,
    done: bool,
}

// ================================================================================================
// dist RL9 (silent auto-update, restart modal, Diagnostics back-ends) and RL4 (migration).
// Kept in one block of its own so the view rewrite (RL14) and this lane touch different hunks.
// ================================================================================================
impl App {
    /// RL9: called every frame from `ui()`. Starts the silent update when one is due: on start,
    /// every 30 minutes, and every 10 seconds while a game running from this folder is holding one
    /// back -- and only with no job, no session of ours, and no `mh.exe` running from the folder.
    fn tick_auto_update(&mut self, ctx: &egui::Context) {
        if !self.auto.enabled {
            return;
        }
        // An idle immediate-mode window does not repaint by itself, and the 10 s cadence needs
        // frames to run on.
        ctx.request_repaint_after(Duration::from_secs(5));
        let now = std::time::Instant::now();
        if !self.auto.due(now) {
            return;
        }
        if self.job.is_some() || self.session.is_some() || self.pending_launch.is_some() {
            return; // due stays due; the next frame asks again
        }
        let Some(dir) = self.game_dir().filter(|d| paths::is_game_dir(d)) else {
            self.auto.schedule(now, false);
            return;
        };
        if procs::game_running_here(&dir) {
            if !self.auto.waiting_logged {
                self.auto.waiting_logged = true;
                log::line(
                    "update: mh.exe is running from this folder -- the silent update waits, \
                     looking again every 10 s",
                );
            }
            self.auto.schedule(now, true);
            return;
        }
        self.auto.waiting_logged = false;
        // Tentative; `poll_job` reschedules with what the run found.
        self.auto.schedule(now, false);
        self.start_update(UpdateAction::AutoUpdate);
    }

    /// RL9: a game update the last check offered that Play should install first: something is
    /// installed in this configuration, the offer is a version, and it is not at or below the
    /// version the player rolled back from.
    fn pending_game_offer(&self, dir: &Path) -> Option<String> {
        let offered = self.offer.as_ref()?.game.clone()?;
        if update::installed_version_of(dir, &self.config.update_tag()).is_empty() {
            return None;
        }
        let skip = self.config.rollback_skip.trim();
        if !skip.is_empty() && update::check_newer(&offered, skip).is_err() {
            return None;
        }
        Some(offered)
    }

    /// RL9: Play with a game update pending starts it as an `Apply` job and resumes the launch from
    /// `poll_job`. True = the launch is parked behind that job.
    fn start_play_update(&mut self, dir: &Path, how: &str) -> bool {
        let Some(version) = self.pending_game_offer(dir) else {
            return false;
        };
        if self.job.is_some() || procs::game_running_here(dir) {
            return false;
        }
        log::line(format!(
            "launch: {how} -- {version} is available; installing it first"
        ));
        self.pending_launch = Some(how.to_string());
        self.auto.play_update = true;
        self.start_update(UpdateAction::Apply);
        true
    }

    /// The game update landed (an `Apply`, a switch, or a silent run): record it, and put the new
    /// layout in order (RL4) before anything is launched.
    fn on_applied(&mut self, a: &Applied) {
        self.config.installed_version = a.version.clone();
        self.config.installed_tag = a.tag.clone();
        // dist RL8: the install ends a channel switch -- the game on disk now came from
        // the channel it was fetched from.
        self.config.installed_channel = a.channel.clone();
        // RL9: a version newer than the one rolled back from ends the pin.
        let skip = self.config.rollback_skip.trim().to_string();
        if !skip.is_empty() && update::check_newer(&a.version, &skip).is_ok() {
            self.config.rollback_skip.clear();
        }
        self.persist();
        if let Some(o) = self.offer.as_mut() {
            o.game = None;
        }
        self.update_say(
            format!(
                "{} -- versions kept: {} (the previous one stays until {} has started once)",
                a.summary,
                a.kept.join(", "),
                a.version
            ),
            false,
        );
        self.sync_installed_from_receipt();
        self.migrate_legacy(); // RL4
        self.provision_relay_quietly(); // RL4
    }

    /// RL4: move the legacy files out of the game folder, once. Never while the game runs from it,
    /// elevating only when the folder needs it, and not again after a declined prompt this run.
    fn migrate_legacy(&mut self) {
        if self.auto.migrate_failed {
            return;
        }
        let Some(dir) = self.game_dir().filter(|d| paths::is_game_dir(d)) else {
            return;
        };
        let running = self.session.is_some() || procs::game_running_here(&dir);
        match migrate::migrate_if_needed(&self.layout, &dir, running) {
            Ok(Some(line)) => {
                self.update_say(line, false);
            }
            Ok(None) => {}
            Err(e) => {
                self.auto.migrate_failed = true;
                self.update_say(format!("migration to user storage failed: {e}"), true);
            }
        }
    }

    /// RL4: write the relay plan into the config directory after an install, logging (not
    /// raising) a failure -- Play provisions again before every launch and reports it there.
    fn provision_relay_quietly(&mut self) {
        let Some(dir) = self.game_dir().filter(|d| paths::is_game_dir(d)) else {
            return;
        };
        let plan = self.relay_plan();
        match relay::provision_for_game(&self.layout, &dir, &plan) {
            Ok(done) if done != relay::Provisioned::NONE => {
                log::line(format!("install: {}", done.summary()))
            }
            Ok(_) => {}
            Err(e) => log::line(format!("install: relay not provisioned yet ({e})")),
        }
    }

    // ---- the launcher restart modal (rendered by the shell at `// RL9: restart modal`) -----------

    /// The version of a staged launcher awaiting "Restart now / Later", until it has been answered.
    pub(crate) fn restart_prompt(&self) -> Option<&str> {
        self.auto.restart_prompt()
    }

    fn current_view_arg(&self) -> String {
        self.view.title().to_ascii_lowercase()
    }

    /// "Restart now": swap the staged launcher in and start it on the page the player is on. Refused
    /// while a game this launcher started is running (the new process would not know the child, and
    /// the crash channel would be lost). On success the window closes.
    pub(crate) fn restart_now(&mut self, ctx: &egui::Context) {
        let Some(staged) = self.auto.pending_restart.clone() else {
            return;
        };
        if self.session.is_some() {
            self.update_say("close the game first, then restart the launcher", true);
            return;
        }
        let args = crate::restart_argv(
            &self.startup.restart_args,
            false,
            Some(&self.current_view_arg()),
        );
        match update::commit_self_update(&staged, Some(&args)) {
            Ok(_) => {
                self.auto.pending_restart = None;
                self.update_say(
                    format!(
                        "replaced by launcher {}; this window is closing",
                        staged.version
                    ),
                    false,
                );
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            Err(e) => self.update_say(format!("restart failed (nothing was replaced): {e}"), true),
        }
    }

    /// "Later": keep running this launcher; the staged one is swapped in when the window closes
    /// (`Drop`), and the modal is not shown again this run.
    pub(crate) fn restart_later(&mut self) {
        self.auto.restart_dismissed = true;
        if let Some(s) = self.auto.pending_restart.as_ref() {
            log::line(format!(
                "update: launcher {} will replace this one when the window closes",
                s.version
            ));
        }
    }

    // ---- Diagnostics back-ends (the buttons are hidden until these exist) ------------------------

    /// Why Re-verify / Roll back must not run right now, if they must not.
    fn diagnostics_blocker(&self, dir: &Path) -> Option<&'static str> {
        if self.job.is_some() {
            Some("an update is running -- try again when it has finished")
        } else if self.session.is_some() || procs::game_running_here(dir) {
            Some("the game is running -- close it first")
        } else {
            None
        }
    }

    /// Diagnostics "Re-verify": re-hash the installed files against the install record and put
    /// back any that differ from the kept copy. Returns (and shows) the status line.
    pub(crate) fn reverify(&mut self) -> String {
        let Some(dir) = self.game_dir() else {
            let m = "pick the game directory first".to_string();
            self.say(m.clone(), true);
            return m;
        };
        if let Some(why) = self.diagnostics_blocker(&dir) {
            self.say(why, true);
            return why.to_string();
        }
        match update::reverify(&self.layout, &dir, &|m: &str| {
            log::line(format!("reverify: {m}"))
        }) {
            Ok(line) => {
                self.sync_installed_from_receipt();
                self.say(line.clone(), false);
                line
            }
            Err(e) => {
                self.say(e.clone(), true);
                e
            }
        }
    }

    /// Diagnostics "Roll back": switch to the kept previous version. The version rolled back FROM
    /// is remembered (`rollback_skip`) so the silent update does not undo it. Returns the status
    /// line.
    pub(crate) fn rollback(&mut self) -> String {
        let Some(dir) = self.game_dir() else {
            let m = "pick the game directory first".to_string();
            self.say(m.clone(), true);
            return m;
        };
        if let Some(why) = self.diagnostics_blocker(&dir) {
            self.say(why, true);
            return why.to_string();
        }
        match update::rollback(&self.layout, &dir, &|m: &str| {
            log::line(format!("rollback: {m}"))
        }) {
            Ok(done) => {
                self.config.installed_version = done.to.clone();
                self.config.installed_tag = done.tag.clone();
                self.config.rollback_skip = done.from.clone();
                self.persist();
                if let Some(o) = self.offer.as_mut() {
                    o.game = None;
                }
                self.sync_installed_from_receipt();
                log::line(format!("rollback: {}", done.summary));
                let line = format!(
                    "rolled back from {} to {} -- automatic updates leave {} alone until a newer \
                     version is released",
                    done.from, done.to, done.from
                );
                self.say(line.clone(), false);
                line
            }
            Err(e) => {
                self.say(e.clone(), true);
                e
            }
        }
    }
}

impl Drop for App {
    /// RL9, "Later" and "just closed the window": a launcher that is staged and health-gated takes
    /// this executable's place now, without starting anything -- the next start runs it.
    fn drop(&mut self) {
        if let Some(staged) = self.auto.pending_restart.take() {
            match update::commit_self_update(&staged, None) {
                Ok(_) => log::line(format!(
                    "update: launcher {} is in place for the next start",
                    staged.version
                )),
                Err(e) => log::line(format!(
                    "update: the staged launcher was not swapped in: {e}"
                )),
            }
        }
    }
}

impl App {
    pub fn new(layout: Layout, config: Config, view: View, startup: Startup) -> Self {
        Self {
            game_dir_edit: config.game_dir.clone(),
            zip_edit: String::new(),
            update_base_edit: config.update_base_url_or_default(),
            pending_launch: None,
            last_job_from_play: false,
            offer: None,
            layout,
            config,
            view,
            theme_done: false,
            status_line: String::new(),
            status_is_error: false,
            session: None,
            presence: None,
            last_run: None,
            auto: update::AutoState::default(),
            // `--description` seeds the box rather than bypassing it: the scripted path and the
            // typed path then meet at exactly the same value, which is what makes the "an empty
            // description is refused" clause provable from a command line.
            description: startup.description.clone(),
            startup,
            startup_done: false,
            job: None,
            update_line: String::new(),
            update_is_error: false,
            last_logged_size: None,
            relay: None,
            relay_from: String::new(),
            channel: None,
            marker: None,
            dump: None,
            include_dump: false,
            built: None,
            consent: None,
            upload_job: None,
            outbox_pending: Vec::new(),
            upload_line: String::new(),
            upload_is_error: false,
            started_utc: stamp_for_file(),
            session_pick: None,
            matches: Vec::new(),
            matches_at: None,
            report_job: None,
            exit_after_report_job: false,
            model: Model::new(Schema::builtin()),
            settings_state: RenderState::default(),
            model_for: None,
            settings_note: None,
            start_prompt: false,
            close_prompt: false,
            close_confirmed: false,
            about_doc: None,
            marker_path: None,
            accepted: Accepted::default(),
            last_copied: None,
        }
        .with_startup_flags()
    }

    fn with_startup_flags(mut self) -> Self {
        // dist RL16: the launcher speaks the language `ui_lang` names (the Language setting writes it
        // together with the game's `[lang] pack`).
        i18n::set_locale(Locale::from_code(&self.config.ui_lang_code()));
        self.include_dump = self.startup.with_minidump;
        // RL9: the silent update belongs to an interactive start. Scripted work (--launch,
        // --update, --check-update, --report, --exit-after-*) does its own fetching and must not
        // find a background job it did not ask for; MH_LAUNCHER_NO_AUTO_UPDATE=1 is the harness's
        // off switch, and no update source means nothing to look at.
        self.auto.enabled = self.startup.update.is_none()
            && !self.startup.launch
            && !self.startup.uninstall
            && self.startup.install_zip.is_none()
            && self.startup.report_to.is_none()
            && !self.startup.exit_after_update
            && !self.startup.exit_after_launch
            && !self.startup.exit_after_report
            && !self.config.update_base_url_or_default().is_empty()
            && std::env::var_os("MH_LAUNCHER_NO_AUTO_UPDATE").is_none();
        // dist LA7: already logged by main.rs; here it only has to be SEEN.
        if let Some(notice) = self.startup.game_dir_notice.take() {
            self.status_line = notice;
            self.status_is_error = true;
        }
        self.refresh_relay();
        // Read on construction rather than on the first frame: "N reports are waiting" has to be
        // true of the launch, not of whenever the player happens to open the Report tab.
        self.outbox_pending = self.outbox().pending();
        if !self.outbox_pending.is_empty() {
            log::line(format!(
                "upload: {} report(s) waiting in {} -- offering them again",
                self.outbox_pending.len(),
                self.outbox().dir().display()
            ));
        }
        self
    }

    fn outbox(&self) -> Outbox {
        Outbox::new(&self.layout.reports())
    }

    /// THE relay decision, for provisioning and for display alike (dist RL4 + RL14): `relay_mode`
    /// `auto` = the signed manifest's relay, `off` = none (the config-dir ini's `relay=` line is
    /// removed), `custom` = `relay_custom` (`host:port`, an open relay -- the key line is `open`).
    /// A custom address that does not validate is `Untouched` (and the Settings page refuses to
    /// save one).
    fn relay_plan(&self) -> relay::RelayPlan {
        relay::plan_for(
            &self.config.relay_mode,
            &self.config.relay_custom,
            self.relay.as_ref(),
        )
    }

    /// The relay the plan makes the game use, for the Play page; `None` = direct play (or an
    /// unusable custom address).
    fn effective_relay(&self) -> Option<Relay> {
        self.relay_plan().relay()
    }

    /// Re-read the accepted manifest's relay (dist LA6). Once at startup and after every accepted
    /// check -- the two moments the copy on disk can have changed.
    fn refresh_relay(&mut self) {
        match update::load_accepted(&self.layout) {
            Some(m) => {
                self.relay_from = m.version.clone();
                self.accepted = Accepted {
                    version: m.version.clone(),
                    issued_at: m.issued_at.clone(),
                    notes_url: m.notes_url.clone(),
                };
                // dist RL8: a pick the channel does not offer falls back to `net` (and says so
                // in the log); written back so Play's readiness check and the picker agree with
                // the install `make_ready` will do, instead of looping on a tag nobody can supply.
                let chosen = self.config.chosen_tag.trim().to_string();
                if !chosen.is_empty() && !m.game.contains_key(&chosen) {
                    if let Ok(t) = update::resolve_tag(&m, &chosen) {
                        self.config.chosen_tag = t;
                        self.persist();
                    }
                }
                self.relay = m.relay;
            }
            None => {
                self.relay_from.clear();
                self.relay = None;
                self.accepted = Accepted::default();
            }
        }
        log::line(format!(
            "relay: {}",
            if self.relay.is_some() {
                format!("the accepted manifest ({}) names a relay", self.relay_from)
            } else {
                "no accepted manifest names a relay -- the game's configuration is left alone"
                    .into()
            }
        ));
    }

    /// The destination: the overridden one when the command line gave a whole set, else the one
    /// compiled into this build. `Err` is a message the Report view shows instead of a button.
    fn collector(&self) -> Result<upload::Collector, String> {
        match &self.startup.collector_override {
            Some(r) => r.clone(),
            None => upload::Collector::baked(),
        }
    }

    /// Where reports and dumps are kept: `%LOCALAPPDATA%\MissionHumanity\reports\`. See
    /// `Layout::reports` -- it lives there now because `main.rs`'s `--send` path needs the same
    /// directory without an `App` to ask.
    fn reports_dir(&self) -> PathBuf {
        self.layout.reports()
    }

    fn game_dir(&self) -> Option<PathBuf> {
        let t = self.game_dir_edit.trim();
        if t.is_empty() {
            None
        } else {
            Some(PathBuf::from(t))
        }
    }

    fn game_dir_ok(&self) -> bool {
        self.game_dir().is_some_and(|d| paths::is_game_dir(&d))
    }

    /// dist LA13: the launcher-owned logs root for the current game directory -- where the game
    /// is told to write (`launch::ENV_LOG_ROOT`), where the crash marker goes, and where the
    /// Report view and `report::build` read sessions from. `None` without a game directory.
    fn log_root(&self) -> Option<PathBuf> {
        self.game_dir().map(|d| self.layout.game_log_root(&d))
    }

    /// Every directory sessions can be in for this game: the launcher-owned root (`log_root`) and
    /// the game's config-dir `logs\` -- where a hand launch writes and where the RL4 migration
    /// moved the old ones. De-duplicated (the config dir can be the same folder).
    pub(crate) fn log_roots(&self) -> Vec<PathBuf> {
        let Some(dir) = self.game_dir() else {
            return Vec::new();
        };
        let mut roots = vec![self.layout.game_log_root(&dir)];
        let cfg_logs = crate::cfgdir::game_config_dir(&self.layout, &dir)
            .0
            .join("logs");
        let same = |a: &Path, b: &Path| {
            a.to_string_lossy()
                .trim_end_matches(['\\', '/'])
                .eq_ignore_ascii_case(b.to_string_lossy().trim_end_matches(['\\', '/']))
        };
        if !roots.iter().any(|r| same(r, &cfg_logs)) {
            roots.push(cfg_logs);
        }
        roots
    }

    /// dist LA9: which match the report is about -- "let the description form name the match the
    /// player means" (the row's scope). The player's pick from `session_picker_block`, if it still
    /// exists; otherwise the newest, `report::default_session_dir`'s existing default.
    fn chosen_session_dir(&self) -> Option<PathBuf> {
        let roots = self.log_roots();
        let dirs = report::session_dirs_in(&roots);
        if let Some(name) = self.session_pick.as_deref() {
            if let Some(p) = dirs
                .iter()
                .find(|p| p.file_name().is_some_and(|n| n == name))
            {
                return Some(p.clone());
            }
        }
        dirs.into_iter().next().or_else(|| {
            roots
                .iter()
                .find_map(|r| report::default_session_dir(Some(r)))
        })
    }

    /// dist RL14: re-read the match list when it is stale (`max_age`), or at once when `force`.
    /// Cheap (names and small JSON files), but not a per-frame cost.
    fn refresh_matches(&mut self, force: bool) {
        let fresh = self
            .matches_at
            .is_some_and(|t| t.elapsed() < Duration::from_secs(2));
        if fresh && !force {
            return;
        }
        self.matches_at = Some(std::time::Instant::now());
        self.matches = report::match_rows(&self.log_roots(), self.session.is_some());
    }

    fn say(&mut self, msg: impl Into<String>, is_error: bool) {
        let msg = msg.into();
        log::line(format!("ui: {msg}"));
        self.status_line = msg;
        self.status_is_error = is_error;
    }

    /// `say` for a message from the string table (dist RL16): the footer shows it in the player's
    /// language, the LOG gets the English text -- a support reader greps the log, whatever the UI
    /// speaks.
    fn say_t(&mut self, key: &str, args: &[(&str, &str)], is_error: bool) {
        log::line(format!("ui: {}", i18n::trf_in(Locale::En, key, args)));
        self.status_line = trf(key, args);
        self.status_is_error = is_error;
    }

    fn persist(&mut self) {
        self.config.game_dir = self.game_dir_edit.trim().to_string();
        if let Err(e) = self.config.save(&self.layout.config()) {
            log::line(format!("config: {e}"));
        }
    }

    fn do_install(&mut self, zip: PathBuf) {
        let Some(dir) = self.game_dir() else {
            self.say_t("msg.pick_game_dir", &[], true);
            return;
        };
        // dist LA13: a game directory this token cannot write (Program Files) gets the copy step
        // done by an elevated re-run -- the zip is still unpacked per-user, here, first.
        let result = if elevate::needs_elevation(&dir) {
            install::stage(&self.layout, &zip).and_then(|staged| {
                elevate::run_step_elevated(
                    &self.layout,
                    &dir,
                    &elevate::StepSpec::Install {
                        version: staged.pkg.version,
                        tag: staged.pkg.tag,
                    },
                )
            })
        } else {
            install::install(&self.layout, &zip, &dir).map(|r| r.summary())
        };
        match result {
            Ok(summary) => {
                self.sync_installed_from_receipt();
                self.migrate_legacy(); // RL4
                self.provision_relay_quietly(); // RL4
                self.say(summary, false);
            }
            Err(e) => self.say_t("msg.install_failed", &[("error", &e)], true),
        }
    }

    fn do_uninstall(&mut self) {
        let Some(dir) = self.game_dir() else {
            self.say_t("msg.pick_game_dir", &[], true);
            return;
        };
        let result = if elevate::needs_elevation(&dir) {
            elevate::run_step_elevated(&self.layout, &dir, &elevate::StepSpec::Uninstall)
        } else {
            install::uninstall(&dir).map(|r| r.summary())
        };
        match result {
            Ok(summary) => {
                self.sync_installed_from_receipt();
                self.say(summary, false);
            }
            Err(e) => self.say_t("msg.uninstall_failed", &[("error", &e)], true),
        }
    }

    /// Start the game. `how` is only what the log says the player pressed -- the Play button and
    /// `--launch` run identical code, because the relay lines are what make either work and the
    /// choice between hosting and joining is made in the game's own menu (dist LA6).
    fn do_launch(&mut self, how: &str) {
        if self.session.is_some() {
            self.say_t("msg.already_running", &[], true);
            return;
        }
        let Some(dir) = self.game_dir() else {
            self.say_t("msg.pick_game_dir", &[], true);
            return;
        };
        // dist LA8: the chosen configuration has to BE there. If it is not -- nothing installed,
        // or a different one -- the LA2 update path runs first (download, verify, install,
        // provision the relay) on its thread, and the launch resumes from `poll_job` when it has.
        let tag = self.config.update_tag();
        match update::readiness(&dir, &tag) {
            Readiness::Ready => {}
            need => {
                if let Some(job) = self.job.as_ref() {
                    // dist LA12: the start-up check is in flight for the first second or two of
                    // every launch. Play pressed inside that window waits for it (`poll_job`
                    // starts the install when the check answers) rather than being refused.
                    if job.action == UpdateAction::AutoCheck {
                        log::line(format!(
                            "launch: {how} -- {tag} is not installed ({need:?}); waiting for the \
                             start-up check to finish, then installing"
                        ));
                        self.pending_launch = Some(how.to_string());
                        return;
                    }
                    self.say_t("msg.update_running", &[], true);
                    return;
                }
                log::line(format!(
                    "launch: {how} -- {tag} is not installed ({need:?}), installing first"
                ));
                self.pending_launch = Some(how.to_string());
                self.start_update(UpdateAction::MakeReady);
                return;
            }
        }
        // RL9: a silent update is mid-flight (it may be copying files beside mh.exe): wait for it,
        // then `poll_job` presses Play again. And a game update that is already known to be
        // pending installs FIRST; if that fails the current version still launches.
        if self
            .job
            .as_ref()
            .is_some_and(|j| j.action == UpdateAction::AutoUpdate)
        {
            log::line(format!(
                "launch: {how} -- waiting for the silent update to finish"
            ));
            self.pending_launch = Some(how.to_string());
            return;
        }
        if self.start_play_update(&dir, how) {
            return;
        }
        // RL4: a game new enough to read its files from user storage has its legacy files moved
        // there (a no-op once done) before anything is provisioned or launched.
        self.migrate_legacy();
        log::line(format!("launch: {how}"));
        // dist LA6: the relay lines go in BEFORE the process exists, every time -- an ini the player
        // (or an older install) changed since is put right again, and one that already says it is
        // not rewritten. A failure here is a refusal to launch, not a warning: a player who pressed
        // Play and got a game that quietly plays direct would blame the relay.
        // dist RL4: into the CONFIG directory (never beside the exe), and the value comes from
        // `relay_mode` -- auto = the manifest's relay, off = none, custom = `relay_custom`.
        let plan = self.relay_plan();
        match relay::provision_for_game(&self.layout, &dir, &plan) {
            Ok(done) if done != relay::Provisioned::NONE => {
                log::line(format!("launch: {}", done.summary()))
            }
            Ok(_) => {}
            // dist LA13: the ini/key write was refused (Program Files, un-elevated) -- that ONE
            // step runs elevated; the launch below stays with this token.
            Err(e) if elevate::is_access_denied(&e) => {
                log::line(format!("launch: {e} -- provisioning elevated"));
                match elevate::run_step_elevated(&self.layout, &dir, &elevate::StepSpec::Provision)
                {
                    Ok(summary) => log::line(format!("launch: {summary}")),
                    Err(e) => {
                        let d = dir.display().to_string();
                        self.say_t(
                            "msg.relay_setup_failed",
                            &[("dir", &d), ("error", &e)],
                            true,
                        );
                        return;
                    }
                }
            }
            Err(e) => {
                let d = dir.display().to_string();
                self.say_t(
                    "msg.relay_setup_failed",
                    &[("dir", &d), ("error", &e)],
                    true,
                );
                return;
            }
        }
        // dist LA13: the launcher-owned logs root. The crash channel's marker goes there too, so a
        // game under Program Files -- UAC-virtualized beside its exe -- still leaves a marker where
        // this (never-virtualized, 64-bit) process can read it.
        let log_root = self.layout.game_log_root(&dir);
        if let Err(e) = std::fs::create_dir_all(&log_root) {
            let d = log_root.display().to_string();
            self.say_t(
                "msg.logs_root_failed",
                &[("dir", &d), ("error", &e.to_string())],
                true,
            );
            return;
        }
        // dist LA4: arm the crash channel BEFORE the game starts. It has to exist by the time
        // mh.dll's DllMain reads the environment, which is the first instruction of the process.
        self.channel = crash::Channel::create(&log_root);
        let env: Vec<(&'static str, String)> = match self.channel.as_ref() {
            Some(c) => c.env().to_vec(),
            None => Vec::new(),
        };
        self.marker = None;
        self.marker_path = None;
        self.dump = None;
        match launch::start(&dir, Some(&log_root), &env) {
            Ok(s) => {
                let pid = s.pid();
                self.presence = discord::start(
                    self.config.effective_discord_client_id(),
                    self.config.discord,
                    log_root.join(discord::PRESENCE_FILE),
                    pid,
                );
                self.session = Some(s);
                self.last_run = None;
                self.view = View::Play;
                self.say_t("msg.game_started", &[("pid", &pid.to_string())], false);
            }
            Err(e) => self.say_t("msg.launch_failed", &[("error", &e.to_string())], true),
        }
    }

    /// The crash half of the frame poll (dist LA4).
    ///
    /// ORDER MATTERS AND IT IS THE OPPOSITE OF THE OBVIOUS ONE: this runs BEFORE the child poll,
    /// every frame, while the game is still alive. The game is sitting inside its own vectored
    /// handler waiting for us, and everything the dump needs -- the thread's registers, the
    /// `EXCEPTION_POINTERS` the marker names -- exists only until we release it. A launcher that
    /// noticed the crash by seeing the process exit would be a launcher that always arrived too
    /// late.
    fn poll_crash(&mut self) {
        let Some(channel) = self.channel.as_ref() else {
            return;
        };
        if !channel.crashed() {
            return;
        }
        let marker_path = channel.marker_path().to_path_buf();
        let marker = match Marker::read(&marker_path) {
            Ok(m) => m,
            Err(e) => {
                log::line(format!("crash: {e}"));
                channel.release();
                return;
            }
        };
        log::line(format!(
            "crash: the game faulted -- 0x{:08x} in {} on thread {}",
            marker.code,
            marker.where_text(),
            marker.tid
        ));
        let dest = self
            .reports_dir()
            .join(format!("{}_{}.dmp", stamp_for_file(), marker.pid));
        match crash::write_dump(&marker, &marker_path, &dest) {
            Ok(_) => {
                self.dump = Some(dest);
                self.include_dump = true;
            }
            Err(e) => log::line(format!("crash: {e}")),
        }
        // Release LAST, whatever happened. A game left blocked because the dump failed would look
        // to the player like the launcher hung the game -- which, at that point, it would have.
        channel.release();
        self.marker = Some(marker);
        self.marker_path = Some(marker_path);
    }

    /// One poll of the running child. Returns true when the launcher should close itself.
    fn poll_session(&mut self) -> bool {
        self.poll_crash();
        let Some(session) = self.session.as_mut() else {
            return false;
        };
        match session.poll() {
            Ok(None) => false,
            Ok(Some(finished)) => {
                // dist LA10: read back where the game itself said its logs went, BEFORE dropping
                // the session (its `exe` path is game_dir\mh.exe, and this is the one place that
                // still has it) -- see `launch::resolved_log_root`'s doc comment for why every
                // launch gets this line rather than only a crashed one.
                let game_dir = session.exe.parent().map(Path::to_path_buf);
                self.session = None;
                self.presence = None;
                self.channel = None;
                if let Some(dir) = game_dir.as_deref() {
                    // dist LA13: the breadcrumb is INSIDE the launcher-owned root now.
                    let log_root = self.layout.game_log_root(dir);
                    match launch::resolved_log_root(&log_root) {
                        Some(root) => log::line(format!("launch: game's log root -> {root}")),
                        None => log::line(format!(
                            "launch: game's log root -> {}\\mh_run.txt is missing or empty (the \
                             game did not honour MH_LOG_ROOT -- an mh.dll older than dist LA13, or \
                             LA10: it fell back to writing beside its own exe)",
                            log_root.display()
                        )),
                    }
                }
                let text = finished.outcome.describe();
                let is_err = finished.outcome.is_crash();
                self.note_first_run(&finished);
                self.last_run = Some(finished);
                self.refresh_matches(true);
                self.say(text, is_err);
                self.migrate_legacy(); // RL4: it waits while the game runs; now is the moment
                self.startup.exit_after_launch
            }
            Err(e) => {
                self.session = None;
                self.presence = None;
                self.channel = None;
                self.say_t("msg.lost_game", &[("error", &e.to_string())], true);
                self.startup.exit_after_launch
            }
        }
    }

    /// Start building a report from whatever the launcher currently knows. dist LA4; since dist LA17
    /// the build runs on a worker thread and `poll_report` takes its result.
    fn do_report(&mut self, dest: Option<PathBuf>) {
        if self.report_job.is_some() {
            self.say_t("msg.report_running", &[], true);
            return;
        }
        // dist LA9: the match the description form names -- the player's pick, or the newest.
        let session_dir = self.chosen_session_dir();
        let dest = dest.unwrap_or_else(|| {
            self.reports_dir()
                .join(format!("mh_report_{}.zip", stamp_for_file()))
        });
        let dump = if self.include_dump {
            self.dump.clone()
        } else {
            None
        };
        let input = report::OwnedInput {
            game_dir: self.game_dir(),
            // The report reads the folder the chosen match lives in (either root).
            logs_root: session_dir
                .as_deref()
                .and_then(Path::parent)
                .map(Path::to_path_buf)
                .or_else(|| self.log_root()),
            session_dir,
            launcher_started_utc: Some(self.started_utc.clone()),
            description: self.description.clone(),
            last_run: self.last_run.clone(),
            crash: self.marker.clone(),
            minidump: dump,
            launcher_log: log::path(),
            // RL4: the ini lives in the config directory now, not beside the exe.
            ini_path: self
                .game_dir()
                .map(|d| crate::cfgdir::ini_path(&self.layout, &d).0),
        };
        report::set_game_running(self.session.is_some());
        log::line(format!("ui: building the report {}", dest.display()));
        self.say_t("msg.report_building", &[], false);
        self.report_job = Some(report::Job::start(dest, input));
    }

    /// One poll of the report thread. Returns true when the launcher should close itself
    /// (`--report ... --exit-after-report`, once that build has finished).
    fn poll_report(&mut self) -> bool {
        let Some(job) = self.report_job.as_mut() else {
            return false;
        };
        let result = match job.poll() {
            report::Poll::Running => return false,
            report::Poll::Done(r) => r,
        };
        self.report_job = None;
        match result {
            Ok(b) => {
                let summary = b.summary();
                self.built = Some(b);
                self.say(summary, false);
            }
            Err(e) => {
                self.built = None;
                self.say(e, true);
                crate::set_exit_code(1);
            }
        }
        std::mem::take(&mut self.exit_after_report_job)
    }

    // ---- dist RP1: consent, then send ---------------------------------------------------------

    /// Step one of two: read the zip back and SHOW what sending it would mean. Opens no socket.
    fn ask_consent(&mut self, zip: PathBuf) {
        let collector = match self.collector() {
            Ok(c) => c,
            Err(e) => {
                self.upload_say(e, true);
                return;
            }
        };
        match upload::prepare(&zip, &collector.endpoint()) {
            Ok(p) => {
                log::line(format!(
                    "upload: asking before sending {} ({} bytes, sha256 {})",
                    p.zip.display(),
                    p.size(),
                    p.sha256
                ));
                self.consent = Some(p);
                self.upload_say_t("msg.upload_read_below", &[], false);
            }
            Err(e) => self.upload_say(e, true),
        }
    }

    /// Step two: the player pressed the button under the list. This is the ONLY caller that opens
    /// a socket, and it can only be reached from a frame that displayed `consent_text` in full.
    fn send_consented(&mut self) {
        let Some(p) = self.consent.take() else {
            return;
        };
        if self.upload_job.is_some() {
            self.upload_say_t("msg.upload_running", &[], true);
            self.consent = Some(p);
            return;
        }
        let collector = match self.collector() {
            Ok(c) => c,
            Err(e) => {
                self.upload_say(e, true);
                return;
            }
        };
        let outbox = self.outbox();
        let zip = p.zip.clone();
        self.upload_say_t(
            "msg.upload_sending",
            &[("file", &zip.display().to_string())],
            false,
        );
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(upload::send(&collector, &p, &outbox));
        });
        self.upload_job = Some(UploadJob {
            zip,
            rx,
            done: false,
        });
    }

    /// One poll of the upload thread. Never closes the launcher -- an upload is not startup work.
    fn poll_upload(&mut self) {
        let Some(job) = self.upload_job.as_mut() else {
            return;
        };
        let result = match job.rx.try_recv() {
            Ok(r) => r,
            Err(mpsc::TryRecvError::Empty) => return,
            Err(mpsc::TryRecvError::Disconnected) => {
                if job.done {
                    return;
                }
                job.done = true;
                Err("the upload thread stopped without answering".to_string())
            }
        };
        let zip = job.zip.clone();
        self.upload_job = None;
        match result {
            Ok(a) => {
                if let Err(e) = self.outbox().done(&zip) {
                    log::line(format!("upload: {e}"));
                }
                self.mark_sent_markers(&zip);
                self.upload_say(a.summary(), false);
            }
            Err(e) => self.upload_say(e, true),
        }
        // Whatever happened, the outbox is the truth about what is still unsent.
        self.outbox_pending = self.outbox().pending();
    }

    /// dist RL14, the RL5 prune contract: the report `zip` was sent, so the crash markers it carried
    /// get their empty `<marker>.reported` sibling (and the crash prompt on the Play page clears).
    fn mark_sent_markers(&mut self, zip: &Path) {
        let Some(b) = self.built.as_ref().filter(|b| b.zip == zip) else {
            return;
        };
        for m in &b.markers {
            match report::mark_reported(m) {
                Ok(p) => log::line(format!("report: {} -- the crash was sent", p.display())),
                Err(e) => log::line(format!("report: {e}")),
            }
        }
        if let Some(mp) = self.marker_path.as_ref() {
            if b.markers.iter().any(|m| m == mp) {
                self.marker = None;
                self.marker_path = None;
            }
        }
        self.refresh_matches(true);
    }

    /// The player dismissed the crash prompt without sending anything: the same flag, so the folders
    /// it was keeping can be pruned again.
    fn dismiss_crash(&mut self) {
        if let Some(mp) = self.marker_path.take() {
            match report::mark_reported(&mp) {
                Ok(p) => log::line(format!(
                    "report: {} -- the crash was dismissed",
                    p.display()
                )),
                Err(e) => log::line(format!("report: {e}")),
            }
        }
        self.marker = None;
        self.dump = None;
        self.refresh_matches(true);
    }

    fn upload_say(&mut self, msg: impl Into<String>, is_error: bool) {
        let msg = msg.into();
        log::line(format!("ui: {msg}"));
        self.upload_line = msg;
        self.upload_is_error = is_error;
    }

    /// `upload_say` from the string table (see `say_t`).
    fn upload_say_t(&mut self, key: &str, args: &[(&str, &str)], is_error: bool) {
        log::line(format!("ui: {}", i18n::trf_in(Locale::En, key, args)));
        self.upload_line = trf(key, args);
        self.upload_is_error = is_error;
    }

    /// A finished game run decides whether the installed version has proved itself (dist LA2).
    ///
    /// This is where last-known-good is actually earned. Until the marker exists nothing prunes, so
    /// the version behind the current one stays on disk; once a run counts as "it started", the
    /// marker goes down and the prune trims back to the newest two. A crash in the first seconds --
    /// a missing DLL, a bad install -- deliberately does not count.
    fn note_first_run(&mut self, finished: &Finished) {
        let version = self.config.installed_version.trim().to_string();
        if version.is_empty() {
            return;
        }
        if !update::run_counts_as_started(finished.outcome.is_crash(), finished.seconds) {
            log::line(format!(
                "update: this run of {version} does not count as a first run ({} after {:.1}s), so \
                 the previous version stays as the way back",
                finished.outcome.describe(),
                finished.seconds
            ));
            return;
        }
        if let Err(e) = update::mark_started(&self.layout, &version) {
            log::line(format!("update: {e}"));
            return;
        }
        update::prune(&self.layout, &version);
    }

    /// Start an update run on a background thread. Startup actions and Play come through here;
    /// the Update BUTTON comes through `start_update_from`, which also remembers the page.
    fn start_update(&mut self, action: UpdateAction) {
        self.start_update_from(action, None);
    }

    /// `pressed_on`: the page the player pressed Update on, carried across a self-update restart
    /// so the window they get back opens where they were (dist LA12). `None` for a scripted run,
    /// whose `--view` -- if any -- is already in the restart strip.
    fn start_update_from(&mut self, action: UpdateAction, pressed_on: Option<View>) {
        if self.job.is_some() {
            self.update_say_t("msg.update_running", &[], true);
            return;
        }
        let base = self.config.update_base_url_or_default();
        let tag = self.config.update_tag();
        let layout = self.layout.clone();
        let game_dir = self.game_dir();
        // dist LA11: what a replacement launcher is started with. The strip (`restart_args`) plus
        // what is still owed: the game half when this is the one-button request, and the page the
        // player is on when they pressed it in the window (a scripted run has no page to return to
        // -- its `--view`, if any, is already in the strip).
        let restart = crate::restart_argv(
            &self.startup.restart_args,
            action == UpdateAction::Update,
            pressed_on
                .map(|v| v.title().to_ascii_lowercase())
                .as_deref(),
        );
        let needs_dir = matches!(
            action,
            UpdateAction::Apply
                | UpdateAction::MakeReady
                | UpdateAction::Update
                | UpdateAction::AutoUpdate
        );
        // RL9: what the silent run needs to know that the others do not.
        let skip_version = self.config.rollback_skip.clone();
        let launcher_pending = self.auto.pending_restart.is_some();
        if needs_dir && game_dir.is_none() {
            self.update_say_t("msg.pick_game_dir", &[], true);
            return;
        }
        // dist LA8: "is the offer newer" is asked against the RECEIPT's configuration. When the
        // directory holds a different one (or none), the chosen one is not installed at any
        // version, and an update to it is a switch, not a rollback.
        // The RECEIPT's version, not launcher.toml's (dist LA12): a fresh state directory pointed
        // at a game directory that already holds an install would otherwise offer the version
        // that is already there.
        let installed = match game_dir.as_deref().and_then(install::read_manifest) {
            Some(r) if r.tag == tag => r.version,
            _ => String::new(),
        };
        let from_play = action == UpdateAction::MakeReady || pressed_on == Some(View::Play);
        // dist RL8: which channel is followed, and which one the game on disk came from (a
        // difference is an explicit switch, the only thing that lets an OLDER game install).
        let channel = self.config.channel().to_string();
        let installed_channel = self.config.installed_channel.trim().to_string();
        log::line(format!(
            "update: {} from {base} (installed {installed:?}, configuration {tag}, channel \
             {channel}, installed from {installed_channel:?})",
            action.describe()
        ));
        // RL9: a silent run is silent -- no "updating..." line unless it finds something to say.
        if !matches!(action, UpdateAction::AutoCheck | UpdateAction::AutoUpdate) {
            self.update_say(format!("{}...", action.describe()), false);
        }
        let progress = Arc::new(Mutex::new(String::new()));
        let progress_thread = Arc::clone(&progress);
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let fetch = update::Http::new();
            let report = move |m: &str| {
                log::line(format!("update: {m}"));
                if let Ok(mut p) = progress_thread.lock() {
                    *p = m.to_string();
                }
            };
            let result = (|| -> Result<Outcome, String> {
                let env = update::Env::release(&channel, &installed_channel);
                if action == UpdateAction::AutoUpdate {
                    // RL9: plan -> stage the launcher -> apply the game, with the running guard.
                    let dir = game_dir.expect("checked above");
                    let ctx = update::AutoCtx {
                        layout: &layout,
                        base_url: &base,
                        game_dir: &dir,
                        tag: &tag,
                        env: &env,
                        skip_version: &skip_version,
                        launcher_pending,
                    };
                    let running = || procs::game_running_here(&dir);
                    return update::auto_update(&fetch, &ctx, &running, &report)
                        .map(|run| Outcome::Auto(Box::new(run)));
                }
                if action == UpdateAction::MakeReady {
                    let dir = game_dir.expect("checked above");
                    return match update::make_ready(
                        &layout, &fetch, &base, &dir, &tag, &env, &report,
                    )? {
                        Some(applied) => Ok(Outcome::Applied(Box::new(applied))),
                        None => Ok(Outcome::Ready),
                    };
                }
                // `Check` keeps LA2's strict gate (NOT NEWER is a refusal, exit 1 -- the scripted
                // "is there an update" question). Everything else accepts the manifest against
                // nothing and decides what to do from it: an offer that is not newer is an answer
                // ("up to date"), not a refusal, and the rollback rule is applied where the
                // install would happen (`apply_if_needed`, `self_update`).
                let gate = if action == UpdateAction::Check {
                    installed.as_str()
                } else {
                    ""
                };
                let manifest = update::check(&fetch, &base, &env, gate, Some(&layout))?;
                // dist RL8: a pick the channel does not offer is `net` from here on, and "what is
                // installed" is read for THAT configuration.
                let (tag, installed) = if action == UpdateAction::Check {
                    (tag.clone(), installed.clone())
                } else {
                    let t = update::resolve_tag(&manifest.game, &tag)?;
                    let i = game_dir
                        .as_deref()
                        .map(|d| update::installed_version_of(d, &t))
                        .unwrap_or_default();
                    (t, i)
                };
                match action {
                    UpdateAction::Check => Ok(Outcome::Checked(Box::new(manifest))),
                    UpdateAction::AutoCheck => {
                        let mut offer = update::offer_for(&manifest, &installed, &env);
                        offer.age_days = manifest.age_days(chrono::Utc::now());
                        Ok(Outcome::Offered(offer))
                    }
                    UpdateAction::Apply => {
                        let dir = game_dir.expect("checked above");
                        match update::apply_if_needed(
                            &layout, &fetch, &manifest, &tag, &dir, &base, &env, &report,
                        )? {
                            Some(applied) => Ok(Outcome::Applied(Box::new(applied))),
                            None => Ok(Outcome::UpToDate),
                        }
                    }
                    UpdateAction::SelfUpdate => {
                        let done =
                            update::self_update(&layout, &fetch, &manifest, &base, &restart)?;
                        Ok(Outcome::SelfUpdated(done))
                    }
                    UpdateAction::Update => {
                        // dist LA12: THE LAUNCHER FIRST. A newer launcher replaces this process
                        // and restarts with `--update` owed (LA11), so the game half runs there;
                        // a current launcher means the game half runs here, now.
                        report("checking the launcher...");
                        match update::self_update(&layout, &fetch, &manifest, &base, &restart)? {
                            SelfUpdate::Restarted(v) | SelfUpdate::Replaced(v) => {
                                return Ok(Outcome::SelfUpdated(SelfUpdate::Restarted(v)))
                            }
                            SelfUpdate::NotNeeded(msg) => report(&msg),
                        }
                        let dir = game_dir.expect("checked above");
                        match update::apply_if_needed(
                            &layout, &fetch, &manifest, &tag, &dir, &base, &env, &report,
                        )? {
                            Some(applied) => Ok(Outcome::Applied(Box::new(applied))),
                            None => Ok(Outcome::UpToDate),
                        }
                    }
                    UpdateAction::MakeReady | UpdateAction::AutoUpdate => {
                        unreachable!("handled above")
                    }
                }
            })();
            let _ = tx.send(result);
        });
        self.job = Some(Job {
            action,
            rx,
            done: false,
            progress,
            from_play,
        });
    }

    /// dist LA8: `launcher.toml`'s installed version / tag, re-read from the receipt. After an
    /// install or a switch the two must agree, and after a FAILED switch (the old set removed,
    /// the new one not yet copied) the receipt is the one that knows.
    fn sync_installed_from_receipt(&mut self) {
        let receipt = self.game_dir().and_then(|d| install::read_manifest(&d));
        let (version, tag) = match receipt {
            Some(r) => (r.version, r.tag),
            None => (String::new(), String::new()),
        };
        if self.config.installed_version != version || self.config.installed_tag != tag {
            self.config.installed_version = version;
            self.config.installed_tag = tag;
            self.persist();
        }
    }

    /// One poll of the update thread. Returns true when the launcher should close itself -- either
    /// because it was asked to (`--exit-after-update`) or because it has just been replaced.
    fn poll_job(&mut self) -> bool {
        let Some(job) = self.job.as_mut() else {
            return false;
        };
        let result = match job.rx.try_recv() {
            Ok(r) => r,
            Err(mpsc::TryRecvError::Empty) => return false,
            Err(mpsc::TryRecvError::Disconnected) => {
                if job.done {
                    return false;
                }
                job.done = true;
                Err("the update thread stopped without answering".to_string())
            }
        };
        let action = job.action;
        self.last_job_from_play = job.from_play;
        self.job = None;
        let mut close = self.startup.exit_after_update;
        if result.is_ok() {
            // Every successful run went through `check`, which remembered the manifest it accepted.
            self.refresh_relay();
        }
        // dist LA8: the launch that was waiting for this install, or its refusal. A launch that
        // was waiting for the start-up CHECK (dist LA12) is re-pressed whatever the check said:
        // `do_launch` then starts the install itself.
        let pending = self.pending_launch.take();
        let launch_after = match &result {
            Ok(Outcome::Applied(_)) | Ok(Outcome::Ready) | Ok(Outcome::UpToDate) => pending,
            _ if matches!(action, UpdateAction::AutoCheck | UpdateAction::AutoUpdate) => pending,
            // RL9: Play's own game update failed -- the current version still launches.
            Err(e) if std::mem::take(&mut self.auto.play_update) && pending.is_some() => {
                self.update_say(
                    format!("the update failed ({e}) -- starting the version you have"),
                    true,
                );
                if let Some(o) = self.offer.as_mut() {
                    o.game = None; // so the launch below does not try the same update again
                }
                pending
            }
            _ => {
                if let (Some(how), Err(e)) = (pending, &result) {
                    self.say(format!("{how}: not started -- {e}"), true);
                    // A scripted `--launch --exit-after-launch` whose install was refused has
                    // no game to wait for: close with the exit code already set to 1, the way
                    // a refused `--update --exit-after-update` does.
                    if self.startup.exit_after_launch {
                        close = true;
                    }
                }
                None
            }
        };
        self.auto.play_update = false;
        match result {
            Ok(Outcome::Checked(m)) => {
                let mut line = format!(
                    "version {} is available on {} (issued {}, launcher {})",
                    m.game.version,
                    m.game.channel,
                    m.game.issued_at,
                    m.launcher
                        .as_ref()
                        .map_or("none offered", |l| l.version.as_str())
                );
                if !m.game.notes_url.is_empty() {
                    line.push_str(&format!(" -- notes: {}", m.game.notes_url));
                }
                self.update_say(line, false);
            }
            Ok(Outcome::Applied(a)) => self.on_applied(&a),
            // RL9: the silent run. Each half reports for itself; none of it blocks the window.
            Ok(Outcome::Auto(run)) => {
                let run = *run;
                let waiting = run.deferred.is_some();
                if let Some(a) = run.applied {
                    self.on_applied(&a);
                }
                let mut offer = run.offer;
                if self.config.installed_version == offer.game.clone().unwrap_or_default() {
                    offer.game = None;
                }
                self.offer = Some(offer);
                if let Some(staged) = run.launcher {
                    self.update_say(
                        format!("launcher {} is ready -- restart to use it", staged.version),
                        false,
                    );
                    self.auto.restart_dismissed = false;
                    self.auto.pending_restart = Some(staged);
                }
                if let Some(why) = run.deferred {
                    self.update_say(format!("update waiting: {why}"), false);
                }
                if let Some(e) = run.errors.first() {
                    self.update_say(
                        format!("update failed (the current version is kept): {e}"),
                        true,
                    );
                }
                self.auto.schedule(std::time::Instant::now(), waiting);
            }
            Ok(Outcome::Ready) => self.update_say("already installed", false),
            Ok(Outcome::UpToDate) => {
                // dist RL8: a channel switch whose game is the very version already installed
                // installs nothing -- but it is over, or a later rollback of the channel would
                // still read as "switching" and be allowed to install an older game.
                if self.config.switch_in_progress() {
                    self.config.installed_channel = self.config.channel().to_string();
                    self.persist();
                }
                self.offer = Some(Offer::default());
                self.update_say(
                    "up to date -- the launcher and the game are both current",
                    false,
                )
            }
            Ok(Outcome::Offered(offer)) => {
                log::line(format!(
                    "update: start-up check -- {}{}",
                    offer.line().unwrap_or_else(|| "up to date".to_string()),
                    offer
                        .age_days
                        .map(|d| format!(" (manifest issued {d} days ago)"))
                        .unwrap_or_default()
                ));
                if let Some(b) = &offer.blocked {
                    self.update_say(b.clone(), true);
                }
                self.offer = Some(offer);
            }
            Ok(Outcome::SelfUpdated(SelfUpdate::NotNeeded(msg))) => self.update_say(msg, false),
            Ok(Outcome::SelfUpdated(SelfUpdate::Restarted(v) | SelfUpdate::Replaced(v))) => {
                self.update_say(
                    format!("replaced by launcher {v}; this window is closing"),
                    false,
                );
                // The replacement is already running. Two launchers of different versions sharing
                // one state directory is not a state to leave a player in.
                close = true;
            }
            Err(e) => {
                if action == UpdateAction::AutoUpdate {
                    // RL9: a failed silent run is a status line and a retry next interval -- not
                    // an exit code, and never a blocked Play.
                    self.update_say(format!("update check failed (will retry): {e}"), true);
                    self.auto.schedule(std::time::Instant::now(), false);
                } else {
                    self.update_say(e, true);
                    crate::set_exit_code(1);
                }
            }
        }
        if matches!(
            action,
            UpdateAction::Apply
                | UpdateAction::MakeReady
                | UpdateAction::Update
                | UpdateAction::AutoUpdate
        ) {
            self.sync_installed_from_receipt();
        }
        log::line(format!("update: {} finished", action.describe()));
        if let Some(how) = launch_after {
            // The install is in; now the launch the player asked for. `do_launch` re-checks
            // readiness, so an install that somehow left the wrong set in place is refused
            // there rather than started here.
            self.do_launch(&how);
        }
        close
    }

    /// dist LA8: the progress line of the job in flight, when it started from the Play page. The
    /// thread's own text (`update::Progress`) is English; until it says something the line is the
    /// action's, in the player's language.
    fn play_progress(&self) -> Option<String> {
        let job = self.job.as_ref().filter(|j| j.from_play)?;
        let p = job.progress.lock().ok()?.clone();
        Some(if p.is_empty() {
            format!("{}...", job.action.describe_t())
        } else {
            p
        })
    }

    /// dist RL8: follow release channel `ch` (`stable` or `latest`) from now on -- the Settings
    /// page's hook (RL9/RL14 wire the control; nothing in the window calls this yet).
    ///
    /// Remembers the choice at once and installs NOTHING itself. What makes the next game install a
    /// *switch* -- the one case that may install an OLDER version -- is the difference between this
    /// and `installed_channel` (the channel the game on disk came from, which is left alone), so
    /// the check this kicks off offers the other channel's game even when it is older, and the
    /// Update button installs it. A refused name changes nothing and is said on the update line.
    #[allow(dead_code)]
    pub fn switch_channel(&mut self, ch: &str) {
        let changed = match self.config.set_channel(ch) {
            Ok(changed) => changed,
            Err(e) => {
                self.update_say(e, true);
                return;
            }
        };
        if !changed {
            return;
        }
        self.persist();
        self.channel_changed();
    }

    /// The followed channel just changed: by `switch_channel`, or by the Settings page's Apply
    /// (dist RL14, whose `ConfigStore` has already written it to `launcher.toml`).
    fn channel_changed(&mut self) {
        log::line(format!(
            "update: channel {} chosen (the game on disk came from {:?}; switching: {})",
            self.config.channel(),
            self.config.installed_channel,
            self.config.switch_in_progress()
        ));
        // What was offered was offered by the OTHER channel's manifests.
        self.offer = None;
        self.refresh_relay();
        if self.job.is_none() {
            self.start_update(UpdateAction::AutoCheck);
        }
    }

    /// `update_say` from the string table (see `say_t`).
    fn update_say_t(&mut self, key: &str, args: &[(&str, &str)], is_error: bool) {
        log::line(format!("ui: {}", i18n::trf_in(Locale::En, key, args)));
        self.update_line = trf(key, args);
        self.update_is_error = is_error;
    }

    fn update_say(&mut self, msg: impl Into<String>, is_error: bool) {
        let msg = msg.into();
        log::line(format!("ui: {msg}"));
        self.update_line = msg;
        self.update_is_error = is_error;
    }

    fn run_startup_actions(&mut self) {
        if let Some(zip) = self.startup.install_zip.take() {
            self.do_install(zip);
        }
        if self.startup.uninstall {
            self.startup.uninstall = false;
            self.do_uninstall();
        }
        self.migrate_legacy(); // RL4: first v0.2.0 run on a folder that is already a 0.2.0 install
        if let Some(action) = self.startup.update.take() {
            self.start_update(action);
        } else if !self.startup.launch && self.startup.report_to.is_none() && self.job.is_none() {
            // dist LA12: EVERY launcher start checks the signed manifest and says what it offers
            // (the Play page's "<version> is available -- Update" line). It installs nothing by
            // itself. Skipped when the start is scripted work (`--launch` does its own manifest
            // fetch when the configuration is missing, and would otherwise find the thread busy;
            // a `--report` run is not the moment) -- those runs asked for something else.
            self.start_update(UpdateAction::AutoCheck);
        }
        if self.startup.launch {
            self.startup.launch = false;
            self.do_launch("--launch");
        }
        // AFTER the launch, and that is what makes `--launch --report x.zip` mean what it reads
        // like: start the game, wait for it, then report on the run that just happened. The
        // startup actions run once, in the first frame, but the report is deferred to
        // `poll_startup_report` below until no game is running.
        if self.startup.report_to.is_some() && !self.description.is_empty() {
            self.view = View::Report;
        }
    }

    /// `--report <zip>`: build once, the moment there is nothing left to wait for.
    ///
    /// Deferred rather than done in `run_startup_actions` because a report built while the game is
    /// still running would ship a session directory the game has not finished writing -- and,
    /// worse, would miss the crash that is the reason the flag was passed.
    fn poll_startup_report(&mut self) -> bool {
        if self.session.is_some() || self.job.is_some() || self.report_job.is_some() {
            return false;
        }
        let Some(dest) = self.startup.report_to.take() else {
            return false;
        };
        self.do_report(Some(dest));
        // Closing waits for the build (`poll_report`): the thread is still working here.
        self.exit_after_report_job = self.startup.exit_after_report;
        false
    }

    fn log_size_once(&mut self, ctx: &egui::Context) {
        let rect = ctx.viewport_rect();
        let ppp = ctx.pixels_per_point();
        let size = [rect.width().round() as u32, rect.height().round() as u32];
        if self.last_logged_size == Some(size) {
            return;
        }
        self.last_logged_size = Some(size);
        log::line(format!(
            "window: requested {}x{} points, rendering at {}x{} points ({}x{} physical px, \
             pixels_per_point {ppp:.2})",
            self.startup.requested_size[0].round() as u32,
            self.startup.requested_size[1].round() as u32,
            size[0],
            size[1],
            (rect.width() * ppp).round() as u32,
            (rect.height() * ppp).round() as u32,
        ));
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.show(ui);
    }
}

/// Which Diagnostics repair actions are available right now (dist RL9); the page draws their
/// buttons only when `App::repair_actions` says `Some`, and each only when its flag is set.
#[derive(Clone, Copy, Debug, Default)]
pub struct RepairActions {
    pub reverify: bool,
    pub rollback: bool,
}

impl App {
    /// One frame: the control half (polls), then the shell and the page. `eframe::App::ui` is a
    /// one-line forward to this, so a test can drive a frame without an `eframe::Frame`.
    fn show(&mut self, ui: &mut egui::Ui) {
        let ctx = ui.ctx().clone();
        if !self.theme_done {
            self.theme_done = true;
            crate::theme::apply(&ctx);
        }
        self.log_size_once(&ctx);
        if !self.startup_done {
            self.startup_done = true;
            self.run_startup_actions();
        }
        self.poll_upload();
        if self.poll_session()
            || self.poll_job()
            || self.poll_startup_report()
            || self.poll_report()
        {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
        // Closing the window with unsaved settings asks first (Apply / Discard / Cancel) instead of
        // dropping the edits silently. Scripted runs never have a dirty model, so their own closes pass.
        if ctx.input(|i| i.viewport().close_requested())
            && self.model.is_dirty()
            && !self.close_confirmed
        {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.close_prompt = true;
        }
        self.tick_auto_update(&ctx); // RL9
        if self.report_job.is_some() {
            // dist LA17: the build is on a thread; keep painting so the progress line moves and the
            // result is noticed, and so Windows never sees a window that has stopped pumping.
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        if self.upload_job.is_some() {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        if self.job.is_some() {
            // Same cadence as the game poll below, same reason: a fetch finishing has to be noticed
            // promptly, and an immediate-mode UI that is not repainting notices nothing.
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        if self.session.is_some() {
            // Not a render loop: ten polls a second is enough to notice an exit promptly and costs
            // nothing while a full-screen game has the GPU.
            ctx.request_repaint_after(Duration::from_millis(100));
        }
        if self.view == View::Report {
            self.refresh_matches(false);
            ctx.request_repaint_after(Duration::from_secs(2));
        }
        shell::show(self, ui);
    }

    // ---- the Start game button (dist RL14) ---------------------------------------------------

    /// Why Start game cannot be pressed right now, in the player's language. Unsaved settings are
    /// NOT a reason by themselves -- pressing Start then asks (Apply / Discard / Cancel) -- unless
    /// they hold values that cannot be applied, which leaves only Revert.
    fn start_blocked(&self) -> Option<&'static str> {
        if self.session.is_some() {
            Some(tr("start.running"))
        } else if self.job.is_some() {
            Some(tr("start.busy"))
        } else if !self.game_dir_ok() {
            Some(tr("start.no_game"))
        } else if self.model.is_dirty() && self.model.has_problems() {
            Some(tr("start.dirty"))
        } else {
            None
        }
    }

    fn press_start(&mut self) {
        if self.model.is_dirty() {
            self.start_prompt = true;
        } else {
            self.do_launch("Start game pressed");
        }
    }

    /// The Start prompt's three answers. `Apply` starts the game only when the save worked.
    fn answer_start_prompt(&mut self, answer: StartAnswer) {
        match answer {
            StartAnswer::Cancel => self.start_prompt = false,
            StartAnswer::Discard => {
                self.model.revert();
                self.start_prompt = false;
                self.do_launch("Start game pressed (settings discarded)");
            }
            StartAnswer::Apply => {
                if self.apply_settings() {
                    self.start_prompt = false;
                    self.do_launch("Start game pressed (settings applied)");
                }
            }
        }
    }

    /// The close prompt's three answers. `Apply` closes only when the save worked.
    fn answer_close_prompt(&mut self, answer: StartAnswer, ctx: &egui::Context) {
        match answer {
            StartAnswer::Cancel => self.close_prompt = false,
            StartAnswer::Discard => {
                self.model.revert();
                self.close_prompt = false;
                self.close_confirmed = true;
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            StartAnswer::Apply => {
                if self.apply_settings() {
                    self.close_prompt = false;
                    self.close_confirmed = true;
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            }
        }
    }

    // ---- settings (dist RL14) ----------------------------------------------------------------

    /// Where the settings page reads and writes `mh_net.ini`, and why there (`cfgdir`).
    fn settings_ini(&self) -> Option<(PathBuf, cfgdir::Source)> {
        let dir = self.game_dir()?;
        Some(cfgdir::ini_path(&self.layout, &dir))
    }

    /// Load the model for the current game directory -- once, and again when the directory changes
    /// or after an Apply. Pending edits are never dropped by a repaint, only by a different game.
    fn ensure_model(&mut self) {
        let dir = self.game_dir();
        if self.model_for == dir {
            return;
        }
        let Some(dir) = dir else {
            self.model_for = None;
            return;
        };
        let (ini, _) = cfgdir::ini_path(&self.layout, &dir);
        let bytes = match ini_io::read_file(&ini) {
            Ok(b) => b,
            Err(e) => {
                log::line(format!("settings: {e}"));
                None
            }
        };
        self.model
            .set_provider_options(Provider::LangPacks, providers::lang_packs(Some(&dir)));
        let store = ConfigStore::new(&mut self.config, self.layout.config());
        self.model.load(bytes.as_deref(), &store);
        self.settings_note = None;
        self.model_for = Some(dir);
    }

    /// Forget what was loaded, so the next `ensure_model` re-reads the ini and the language packs
    /// -- when the player opens Settings (a pack may have been built since) and there is nothing
    /// pending to lose.
    fn reload_model_if_clean(&mut self) {
        if !self.model.is_dirty() {
            self.model_for = None;
        }
    }

    /// Apply the pending edits: splice the ini, write `launcher.toml`, reload. `true` when it worked.
    fn apply_settings(&mut self) -> bool {
        let Some((ini, _)) = self.settings_ini() else {
            return false;
        };
        let old_channel = self.config.channel();
        let path = self.layout.config();
        let result = {
            let mut store = ConfigStore::new(&mut self.config, path);
            self.model.apply(&ini, &mut store)
        };
        match result {
            Ok(report) => {
                self.persist();
                i18n::set_locale(Locale::from_code(&self.config.ui_lang_code()));
                log::line(format!(
                    "settings: applied -- ini {:?}, launcher fields {:?}, restart needed {}",
                    report.ini, report.launcher_fields, report.restart_needed
                ));
                if self.config.channel() != old_channel {
                    self.channel_changed();
                }
                self.settings_note = Some((tr("settings.applied").to_string(), false));
                true
            }
            Err(e) => {
                log::line(format!("settings: apply failed: {e}"));
                self.settings_note = Some((trf("settings.apply_failed", &[("error", &e)]), true));
                false
            }
        }
    }

    // ---- shared by the pages -----------------------------------------------------------------

    /// The folder picker behind every "Change..." / "Browse..." button.
    fn browse_game_dir(&mut self) {
        let start = self.game_dir().filter(|d| d.is_dir());
        let mut dlg = rfd::FileDialog::new().set_title(tr("dialog.game_dir"));
        if let Some(d) = start {
            dlg = dlg.set_directory(d);
        }
        if let Some(picked) = dlg.pick_folder() {
            self.game_dir_edit = picked.display().to_string();
            self.persist();
            let ok = self.game_dir_ok();
            let d = self.game_dir_edit.clone();
            self.say_t(
                if ok {
                    "msg.game_dir_set"
                } else {
                    "msg.game_dir_set_no_exe"
                },
                &[("dir", &d)],
                !ok,
            );
        }
    }

    /// RL9: `Some` when the launcher can re-verify the installed files (an install record is in
    /// the game folder) and/or roll back (an older version is kept); `None` draws neither
    /// Diagnostics button.
    pub(crate) fn repair_actions(&self) -> Option<RepairActions> {
        let dir = self.game_dir().filter(|d| paths::is_game_dir(d))?;
        let receipt = install::read_manifest(&dir)?;
        let r = RepairActions {
            reverify: true,
            rollback: update::rollback_target(&self.layout, &receipt.version).is_some(),
        };
        Some(r)
    }
}

/// The Start prompt's answers.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum StartAnswer {
    Apply,
    Discard,
    Cancel,
}

/// `20260917T164346Z` -- the same UTC shape the game's session directories use, so a report file
/// sorts next to the session it is about.
fn stamp_for_file() -> String {
    chrono::Utc::now().format("%Y%m%dT%H%M%SZ").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// dist LA17: the `App`-level poll cycle for the report job -- the build is running on a thread
    /// that is not this one, `do_report` refuses a second start while it runs, and `poll_report`
    /// (what `ui()` calls each frame) takes the result without ever blocking.
    #[test]
    fn the_report_builds_off_the_ui_thread_and_a_second_start_is_refused() {
        let dir = std::env::temp_dir().join("mh_launcher_test_app_report_job");
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mh.exe"), b"x").unwrap();
        let layout = Layout::rooted(dir.join("app"));
        let cfg = Config {
            game_dir: dir.display().to_string(),
            ..Config::default()
        };
        let mut app = App::new(layout, cfg, View::Report, Startup::default());
        let sess = app
            .log_root()
            .unwrap()
            .join("20260927T090000Z_a1b2c3d4_1_host");
        std::fs::create_dir_all(&sess).unwrap();
        std::fs::write(
            sess.join("mh_net.log"),
            "hello
"
            .repeat(1000),
        )
        .unwrap();
        app.description = "it froze".to_string();
        let zip = dir.join("out").join("r.zip");
        app.do_report(Some(zip.clone()));
        let job = app.report_job.as_ref().expect("the job started");
        assert_ne!(
            job.worker_thread(),
            std::thread::current().id(),
            "the report is being built on the calling (UI) thread"
        );
        app.do_report(Some(dir.join("out").join("second.zip")));
        assert!(app.status_is_error, "a second start must be refused");
        assert!(app.status_line.contains("already"), "{}", app.status_line);

        let t0 = std::time::Instant::now();
        while app.report_job.is_some() {
            assert!(t0.elapsed().as_secs() < 60, "the report job never finished");
            assert!(!app.poll_report(), "no --exit-after-report was asked for");
            std::thread::sleep(Duration::from_millis(5));
        }
        let built = app.built.as_ref().expect("the finished report is kept");
        assert_eq!(built.zip, zip);
        assert!(zip.is_file());
        assert!(!dir.join("out").join("second.zip").exists());
        assert!(!app.status_is_error, "{}", app.status_line);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn every_view_has_a_name_that_parses_back() {
        for v in View::ALL {
            assert_eq!(View::parse(v.title()), Some(v), "{}", v.title());
        }
        assert_eq!(View::parse("REPORT"), Some(View::Report));
        assert_eq!(View::parse("nonsense"), None);
        // The pre-RL14 names keep working: scripts, and the restart strip of an older launcher.
        for old in ["status", "update", "launch", "play"] {
            assert_eq!(View::parse(old), Some(View::Play), "{old}");
        }
        assert_eq!(View::Launch, View::Play);
    }

    // ---- dist RL9 ------------------------------------------------------------------------------

    fn auto_app(name: &str) -> (App, PathBuf) {
        let dir = std::env::temp_dir().join(name);
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("mh.exe"), b"x").unwrap();
        let cfg = Config {
            game_dir: dir.display().to_string(),
            // a closed local port: the silent run fails fast, offline
            update_base_url: "http://127.0.0.1:1/".to_string(),
            ..Config::default()
        };
        let app = App::new(
            Layout::rooted(dir.join("app")),
            cfg,
            View::Launch,
            Startup::default(),
        );
        (app, dir)
    }

    /// RL9: a silent run that staged a launcher sets the restart flag (the modal's trigger); "Later"
    /// answers the modal without dropping the staged launcher.
    #[test]
    fn a_staged_launcher_sets_the_restart_flag_and_later_keeps_it_staged() {
        let (mut app, dir) = auto_app("mh_launcher_test_app_restart");
        assert!(app.auto.enabled, "an interactive start ticks");
        assert_eq!(app.restart_prompt(), None);
        let (tx, rx) = mpsc::channel();
        let run = update::AutoRun {
            launcher: Some(update::StagedLauncher {
                version: "0.2.0".into(),
                // not a file: the Drop commit at the end of the test must find nothing to replace
                candidate: dir.join("never_written.exe"),
            }),
            ..Default::default()
        };
        tx.send(Ok(Outcome::Auto(Box::new(run)))).unwrap();
        app.job = Some(Job {
            action: UpdateAction::AutoUpdate,
            rx,
            done: false,
            progress: Arc::new(Mutex::new(String::new())),
            from_play: false,
        });
        assert!(!app.poll_job());
        assert_eq!(app.restart_prompt(), Some("0.2.0"));
        assert!(app.update_line.contains("0.2.0"), "{}", app.update_line);
        assert!(!app.update_is_error);
        app.restart_later();
        assert_eq!(app.restart_prompt(), None, "answered");
        assert!(
            app.auto.pending_restart.is_some(),
            "Later keeps it staged for the window close"
        );
        // the silent run rescheduled itself 30 minutes out
        assert!(app
            .auto
            .next
            .is_some_and(|t| t > std::time::Instant::now() + Duration::from_secs(25 * 60)));
        app.auto.pending_restart = None;
        std::fs::remove_dir_all(&dir).ok();
    }

    /// RL9 done_when: a HAND-LAUNCHED mh.exe (a process this launcher never started) holds the
    /// silent update back: no job starts, and the next look is 10 seconds out.
    #[test]
    fn a_hand_launched_game_holds_the_tick_back() {
        let (mut app, dir) = auto_app("mh_launcher_test_app_tick_defer");
        let mut child = crate::procs::testing::spawn_copy_as_mh_exe(&dir);
        // wait until the OS lists it
        let t0 = std::time::Instant::now();
        while !procs::game_running_here(&dir) {
            assert!(t0.elapsed().as_secs() < 30, "the fake game never appeared");
            std::thread::sleep(Duration::from_millis(50));
        }
        let ctx = egui::Context::default();
        app.tick_auto_update(&ctx);
        assert!(
            app.job.is_none(),
            "no update may start while mh.exe runs from the folder"
        );
        assert!(app.auto.waiting_logged);
        let next = app.auto.next.expect("rescheduled");
        assert!(next <= std::time::Instant::now() + Duration::from_secs(10));
        // the game exits: the very next due tick starts the silent run
        child.kill().unwrap();
        child.wait().unwrap();
        let t0 = std::time::Instant::now();
        while procs::game_running_here(&dir) {
            assert!(t0.elapsed().as_secs() < 30);
            std::thread::sleep(Duration::from_millis(50));
        }
        app.auto.next = None; // "10 seconds later"
        app.tick_auto_update(&ctx);
        assert!(
            app.job
                .as_ref()
                .is_some_and(|j| j.action == UpdateAction::AutoUpdate),
            "the deferred update starts with no click once the game has exited"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    /// RL9 done_when: a failed silent run (here: nothing listening) is a status line and a retry
    /// 30 minutes out -- not an exit code, and Play's readiness is untouched.
    #[test]
    fn a_failed_silent_run_is_a_status_line_and_does_not_block_play() {
        let (mut app, dir) = auto_app("mh_launcher_test_app_tick_fail");
        let ctx = egui::Context::default();
        app.tick_auto_update(&ctx);
        assert!(
            app.job.is_some(),
            "a due tick with an idle window starts the run"
        );
        let t0 = std::time::Instant::now();
        while app.job.is_some() {
            assert!(t0.elapsed().as_secs() < 60, "the silent run never finished");
            assert!(
                !app.poll_job(),
                "a failed silent run must not close the window"
            );
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(app.update_is_error, "{}", app.update_line);
        assert!(
            app.update_line.contains("will retry"),
            "{}",
            app.update_line
        );
        assert!(app.pending_launch.is_none() && app.session.is_none());
        assert!(app
            .auto
            .next
            .is_some_and(|t| t > std::time::Instant::now() + Duration::from_secs(25 * 60)));
        // a second tick inside the interval starts nothing
        app.tick_auto_update(&ctx);
        assert!(app.job.is_none());
        std::fs::remove_dir_all(&dir).ok();
    }

    /// RL9: scripted starts never grow a background job of their own.
    #[test]
    fn scripted_starts_do_not_tick() {
        let dir = std::env::temp_dir().join("mh_launcher_test_app_scripted");
        std::fs::create_dir_all(&dir).unwrap();
        for startup in [
            Startup {
                launch: true,
                ..Startup::default()
            },
            Startup {
                update: Some(UpdateAction::Apply),
                ..Startup::default()
            },
            Startup {
                exit_after_update: true,
                ..Startup::default()
            },
            Startup {
                report_to: Some(dir.join("r.zip")),
                ..Startup::default()
            },
        ] {
            let cfg = Config {
                update_base_url: "http://127.0.0.1:1/".into(),
                ..Config::default()
            };
            let app = App::new(Layout::rooted(dir.join("app")), cfg, View::Launch, startup);
            assert!(!app.auto.enabled);
        }
        std::fs::remove_dir_all(&dir).ok();
    }

    /// RL9: Play's pending-offer rule honours the rollback pin and needs an installed game.
    #[test]
    fn play_installs_a_pending_offer_first_unless_pinned_or_not_installed() {
        let (mut app, dir) = auto_app("mh_launcher_test_app_offer");
        app.offer = Some(Offer {
            game: Some("0.3.0".into()),
            ..Offer::default()
        });
        // nothing installed: Play's own install path handles it, not the pending-offer rule
        assert_eq!(app.pending_game_offer(&dir), None);
        std::fs::write(
            dir.join(paths::INSTALL_MANIFEST),
            "version\t0.2.9\ntag\tnet\npackage\tp.zip\ninstalled_at\tx\n",
        )
        .unwrap();
        assert_eq!(app.pending_game_offer(&dir).as_deref(), Some("0.3.0"));
        app.config.rollback_skip = "0.3.0".into();
        assert_eq!(app.pending_game_offer(&dir), None, "rolled back from it");
        app.config.rollback_skip = "0.2.5".into();
        assert_eq!(app.pending_game_offer(&dir).as_deref(), Some("0.3.0"));
        std::fs::remove_dir_all(&dir).ok();
    }
}
