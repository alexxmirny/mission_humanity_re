//! The window: three views in one, switched by a tab strip.
//!
//! STATUS / LAUNCH / REPORT is the shape dist LA1 was scoped to, and the split is by what the
//! player is DOING, not by what the code does: pointing the launcher at a game and getting the
//! files there (status), playing (launch), and telling us it went wrong (report). LA2 fills the
//! update half of the status view and LA4 fills the report view; both were given their own tab now
//! so that arriving does not reshuffle a layout players have learned.
//!
//! **dist LA6 made the launch view the FRONT PAGE** -- the first tab, the one the window opens on,
//! titled *Play* -- with one button, **Play**. It writes the relay the signed manifest names into
//! the game's `mh_net.ini` + `mh_key.txt` (`relay.rs`) and starts the game. Hosting or joining is
//! decided in the game's own menu afterwards (create a game, or browse the relay's list --
//! mp:R2/R7). It was two buttons, Host and Join, until 2026-09-20: they ran identical code and
//! differed only in the log line, so the user ruled them one. Status (install, update) and Report
//! keep their tabs.
//!
//! Everything here is immediate-mode: there is no retained widget tree, so a view is a function of
//! `self` and the frame draws whatever the state currently says. The only thing that has to be
//! remembered across frames is the running child (`launch::Session`), which is polled once per
//! frame rather than waited on -- blocking the UI thread on `wait()` would freeze the window for
//! the length of the game session and make the launcher look like the thing that hung.

use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::config::Config;
use crate::crash::{self, Marker};
use crate::elevate;
use crate::install;
use crate::launch::{self, Finished};
use crate::log;
use crate::paths::{self, Layout};
use crate::relay::{self, Relay};
use crate::report;
use crate::update::{self, Applied, Manifest, Offer, Readiness, SelfUpdate};
use crate::upload::{self, Outbox, Prepared};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum View {
    Status,
    Launch,
    Report,
}

impl View {
    /// Tab order. Play first: it is the front page (dist LA6).
    pub const ALL: [View; 3] = [View::Launch, View::Status, View::Report];

    pub fn title(self) -> &'static str {
        match self {
            View::Status => "Status",
            View::Launch => "Play",
            View::Report => "Report",
        }
    }

    pub fn parse(s: &str) -> Option<View> {
        match s.to_ascii_lowercase().as_str() {
            "status" | "update" => Some(View::Status),
            "launch" | "play" => Some(View::Launch),
            "report" => Some(View::Report),
            _ => None,
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
}

impl UpdateAction {
    fn describe(self) -> &'static str {
        match self {
            UpdateAction::Check => "checking for an update",
            UpdateAction::AutoCheck => "checking for an update (every start)",
            UpdateAction::Apply => "updating the game",
            UpdateAction::SelfUpdate => "updating the launcher",
            UpdateAction::Update => "updating the launcher, then the game",
            UpdateAction::MakeReady => "getting the game ready to play",
        }
    }
}

/// What an update run produced. Richer than a string because the UI thread has to WRITE something
/// down afterwards -- the new installed version, the marker decision -- and a run that reported only
/// prose would leave that to be re-derived from a sentence.
enum Outcome {
    Checked(Box<Manifest>),
    Applied(Box<Applied>),
    SelfUpdated(SelfUpdate),
    /// dist LA8: `MakeReady` found the chosen configuration already installed.
    Ready,
    /// dist LA12: the automatic check's answer -- what the manifest offers over what is here.
    Offered(Offer),
    /// dist LA12: `Update`/`Apply` found the launcher AND the game current; nothing was touched.
    UpToDate,
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

/// The three configurations the picker offers (dist LA8) -- the manifest's own tags, with one
/// line each on what they are for, in `docs/release.md`'s words. `net` first: it is preselected.
const CONFIGURATIONS: [(&str, &str); 3] = [
    (
        "net",
        "the game as shipped, plus the restored multiplayer -- what most players want",
    ),
    (
        "net-debug",
        "the same, with mh_harness.dll and the diagnostic logging keys switched on in mh_net.ini",
    ),
    (
        "brokered-debug",
        "the hosted / brokered build (libmh.dll) -- for testing the reimplementation, not for play",
    ),
];

fn configuration_line(tag: &str) -> &'static str {
    CONFIGURATIONS
        .iter()
        .find(|(t, _)| *t == tag)
        .map(|(_, line)| *line)
        .unwrap_or("not one of this launcher's configurations")
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
    /// Free-text edit buffer for the game directory, so typing a path is possible without a dialog.
    game_dir_edit: String,
    zip_edit: String,
    /// The last thing that happened, good or bad, shown under the controls.
    status_line: String,
    status_is_error: bool,
    session: Option<launch::Session>,
    last_run: Option<Finished>,
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
    /// The configuration the picker shows: `config.update_tag()` at start, then whatever the
    /// player clicks (written to `config.chosen_tag` at once).
    tag_pick: String,
    /// The picker is open. Forced open while nothing is installed; a button opens it otherwise.
    picker_open: bool,
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
    /// The session picker is open (mirrors `picker_open`, the configuration picker's own flag).
    session_picker_open: bool,
}

/// One upload, on its own thread.
struct UploadJob {
    zip: PathBuf,
    rx: Receiver<Result<upload::Accepted, String>>,
    done: bool,
}

impl App {
    pub fn new(layout: Layout, config: Config, view: View, startup: Startup) -> Self {
        Self {
            game_dir_edit: config.game_dir.clone(),
            zip_edit: String::new(),
            update_base_edit: config.update_base_url_or_default(),
            tag_pick: config.update_tag(),
            picker_open: false,
            pending_launch: None,
            last_job_from_play: false,
            offer: None,
            layout,
            config,
            view,
            status_line: String::new(),
            status_is_error: false,
            session: None,
            last_run: None,
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
            session_picker_open: false,
        }
        .with_startup_flags()
    }

    fn with_startup_flags(mut self) -> Self {
        self.include_dump = self.startup.with_minidump;
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

    /// Re-read the accepted manifest's relay (dist LA6). Once at startup and after every accepted
    /// check -- the two moments the copy on disk can have changed.
    fn refresh_relay(&mut self) {
        match update::load_accepted(&self.layout) {
            Some(m) => {
                self.relay_from = m.version.clone();
                self.relay = m.relay;
            }
            None => {
                self.relay_from.clear();
                self.relay = None;
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

    /// dist LA9: which match the report is about -- "let the description form name the match the
    /// player means" (the row's scope). The player's pick from `session_picker_block`, if it still
    /// exists; otherwise the newest, `report::default_session_dir`'s existing default.
    fn chosen_session_dir(&self) -> Option<PathBuf> {
        let log_root = self.log_root()?;
        let dirs = report::session_dirs(&log_root);
        if let Some(name) = self.session_pick.as_deref() {
            if let Some(p) = dirs
                .iter()
                .find(|p| p.file_name().is_some_and(|n| n == name))
            {
                return Some(p.clone());
            }
        }
        dirs.into_iter()
            .next()
            .or_else(|| report::default_session_dir(Some(&log_root)))
    }

    /// dist LA9: "Match: <name> [Change...]", or the open picker -- one radio line per session
    /// directory, newest first, mirroring `configuration_block`'s own open/closed shape. Hidden
    /// when there is nothing to pick between (zero or one match on disk).
    fn session_picker_block(&mut self, ui: &mut egui::Ui) {
        let Some(log_root) = self.log_root() else {
            return;
        };
        let dirs = report::session_dirs(&log_root);
        if dirs.len() < 2 {
            return;
        }
        let chosen_name = self
            .chosen_session_dir()
            .as_deref()
            .and_then(Path::file_name)
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        if !self.session_picker_open {
            ui.horizontal(|ui| {
                ui.label("Match:");
                ui.monospace(&chosen_name);
                if ui.small_button("Change...").clicked() {
                    self.session_picker_open = true;
                }
            });
        } else {
            egui::Frame::group(ui.style()).show(ui, |ui| {
                ui.label(egui::RichText::new("Which match is this report about?").strong());
                for dir in &dirs {
                    let name = dir
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .to_string();
                    if ui.radio(chosen_name == name, &name).clicked() {
                        self.session_pick = Some(name);
                        self.session_picker_open = false;
                    }
                }
                if ui.small_button("Done").clicked() {
                    self.session_picker_open = false;
                }
            });
        }
    }

    fn say(&mut self, msg: impl Into<String>, is_error: bool) {
        let msg = msg.into();
        log::line(format!("ui: {msg}"));
        self.status_line = msg;
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
            self.say("pick the game directory first", true);
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
            install::install(&self.layout, &zip, &dir, self.relay.as_ref()).map(|r| r.summary())
        };
        match result {
            Ok(summary) => {
                self.sync_installed_from_receipt();
                self.say(summary, false);
            }
            Err(e) => self.say(format!("install failed: {e}"), true),
        }
    }

    fn do_uninstall(&mut self) {
        let Some(dir) = self.game_dir() else {
            self.say("pick the game directory first", true);
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
            Err(e) => self.say(format!("uninstall failed: {e}"), true),
        }
    }

    /// Start the game. `how` is only what the log says the player pressed -- the Play button and
    /// `--launch` run identical code, because the relay lines are what make either work and the
    /// choice between hosting and joining is made in the game's own menu (dist LA6).
    fn do_launch(&mut self, how: &str) {
        if self.session.is_some() {
            self.say("the game is already running", true);
            return;
        }
        let Some(dir) = self.game_dir() else {
            self.say("pick the game directory first", true);
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
                    self.say("an update is already running", true);
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
        log::line(format!("launch: {how}"));
        // dist LA6: the relay lines go in BEFORE the process exists, every time -- an ini the player
        // (or an older install) changed since is put right again, and one that already says it is
        // not rewritten. A failure here is a refusal to launch, not a warning: a player who pressed
        // Play and got a game that quietly plays direct would blame the relay.
        match relay::provision(&dir, self.relay.as_ref()) {
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
                        self.say(
                            format!("cannot set the relay up in {}: {e}", dir.display()),
                            true,
                        );
                        return;
                    }
                }
            }
            Err(e) => {
                self.say(
                    format!("cannot set the relay up in {}: {e}", dir.display()),
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
            self.say(
                format!("cannot create the logs root {}: {e}", log_root.display()),
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
        self.dump = None;
        match launch::start(&dir, Some(&log_root), &env) {
            Ok(s) => {
                let pid = s.pid();
                self.session = Some(s);
                self.last_run = None;
                self.view = View::Launch;
                self.say(
                    format!("game started, pid {pid} -- waiting for it to exit"),
                    false,
                );
            }
            Err(e) => self.say(format!("launch failed: {e}"), true),
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
                self.say(text, is_err);
                self.startup.exit_after_launch
            }
            Err(e) => {
                self.session = None;
                self.channel = None;
                self.say(format!("lost track of the game process: {e}"), true);
                self.startup.exit_after_launch
            }
        }
    }

    /// Build a report from whatever the launcher currently knows. dist LA4.
    fn do_report(&mut self, dest: Option<PathBuf>) {
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
        let game_dir = self.game_dir();
        let input = report::Input {
            game_dir: game_dir.as_deref(),
            logs_root: self.log_root(),
            session_dir,
            launcher_started_utc: Some(self.started_utc.clone()),
            description: &self.description,
            last_run: self.last_run.as_ref(),
            crash: self.marker.as_ref(),
            minidump: dump.as_deref(),
            launcher_log: log::path(),
        };
        match report::build(&dest, &input) {
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
                self.upload_say("read what this would send, below", false);
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
            self.upload_say("a report is already being sent", true);
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
        self.upload_say(format!("sending {}...", zip.display()), false);
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
                self.upload_say(a.summary(), false);
            }
            Err(e) => self.upload_say(e, true),
        }
        // Whatever happened, the outbox is the truth about what is still unsent.
        self.outbox_pending = self.outbox().pending();
    }

    fn upload_say(&mut self, msg: impl Into<String>, is_error: bool) {
        let msg = msg.into();
        log::line(format!("ui: {msg}"));
        self.upload_line = msg;
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
            self.update_say("an update is already running", true);
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
            UpdateAction::Apply | UpdateAction::MakeReady | UpdateAction::Update
        );
        if needs_dir && game_dir.is_none() {
            self.update_say("pick the game directory first", true);
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
        let from_play = action == UpdateAction::MakeReady || pressed_on == Some(View::Launch);
        log::line(format!(
            "update: {} from {base} (installed {installed:?}, configuration {tag})",
            action.describe()
        ));
        if action != UpdateAction::AutoCheck {
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
                if action == UpdateAction::MakeReady {
                    let dir = game_dir.expect("checked above");
                    return match update::make_ready(&layout, &fetch, &base, &dir, &tag, &report)? {
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
                let manifest = update::check(&fetch, &base, gate, Some(&layout))?;
                match action {
                    UpdateAction::Check => Ok(Outcome::Checked(Box::new(manifest))),
                    UpdateAction::AutoCheck => {
                        Ok(Outcome::Offered(update::offer_for(&manifest, &installed)))
                    }
                    UpdateAction::Apply => {
                        let dir = game_dir.expect("checked above");
                        match update::apply_if_needed(
                            &layout, &fetch, &manifest, &tag, &dir, &base, &report,
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
                            SelfUpdate::Restarted(v) => {
                                return Ok(Outcome::SelfUpdated(SelfUpdate::Restarted(v)))
                            }
                            SelfUpdate::NotNeeded(msg) => report(&msg),
                        }
                        let dir = game_dir.expect("checked above");
                        match update::apply_if_needed(
                            &layout, &fetch, &manifest, &tag, &dir, &base, &report,
                        )? {
                            Some(applied) => Ok(Outcome::Applied(Box::new(applied))),
                            None => Ok(Outcome::UpToDate),
                        }
                    }
                    UpdateAction::MakeReady => unreachable!("handled above"),
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
            _ if action == UpdateAction::AutoCheck => pending,
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
        match result {
            Ok(Outcome::Checked(m)) => {
                let mut line = format!(
                    "version {} is available (issued {}, launcher {})",
                    m.version, m.issued_at, m.launcher.version
                );
                if !m.notes_url.is_empty() {
                    line.push_str(&format!(" -- notes: {}", m.notes_url));
                }
                self.update_say(line, false);
            }
            Ok(Outcome::Applied(a)) => {
                self.config.installed_version = a.version.clone();
                self.config.installed_tag = a.tag.clone();
                self.persist();
                self.picker_open = false;
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
            }
            Ok(Outcome::Ready) => self.update_say("already installed", false),
            Ok(Outcome::UpToDate) => {
                self.offer = Some(Offer::default());
                self.update_say(
                    "up to date -- the launcher and the game are both current",
                    false,
                )
            }
            Ok(Outcome::Offered(offer)) => {
                log::line(format!(
                    "update: start-up check -- {}",
                    offer.line().unwrap_or_else(|| "up to date".to_string())
                ));
                self.offer = Some(offer);
            }
            Ok(Outcome::SelfUpdated(SelfUpdate::NotNeeded(msg))) => self.update_say(msg, false),
            Ok(Outcome::SelfUpdated(SelfUpdate::Restarted(v))) => {
                self.update_say(
                    format!("replaced by launcher {v}; this window is closing"),
                    false,
                );
                // The replacement is already running. Two launchers of different versions sharing
                // one state directory is not a state to leave a player in.
                close = true;
            }
            Err(e) => {
                self.update_say(e, true);
                crate::set_exit_code(1);
            }
        }
        if matches!(
            action,
            UpdateAction::Apply | UpdateAction::MakeReady | UpdateAction::Update
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

    /// dist LA8: the progress line of the job in flight, when it started from the Play page.
    fn play_progress(&self) -> Option<String> {
        let job = self.job.as_ref().filter(|j| j.from_play)?;
        let p = job.progress.lock().ok()?.clone();
        Some(if p.is_empty() {
            format!("{}...", job.action.describe())
        } else {
            p
        })
    }

    /// dist LA8: the player picked a configuration. Remembered at once; nothing is installed
    /// until Play.
    fn pick_tag(&mut self, tag: &str) {
        if self.tag_pick == tag {
            return;
        }
        self.tag_pick = tag.to_string();
        self.config.chosen_tag = tag.to_string();
        self.persist();
        log::line(format!("play: configuration {tag} picked"));
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
        if self.session.is_some() || self.job.is_some() {
            return false;
        }
        let Some(dest) = self.startup.report_to.take() else {
            return false;
        };
        self.do_report(Some(dest));
        self.startup.exit_after_report
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
        let ctx = ui.ctx().clone();
        self.log_size_once(&ctx);
        if !self.startup_done {
            self.startup_done = true;
            self.run_startup_actions();
        }
        self.poll_upload();
        if self.poll_session() || self.poll_job() || self.poll_startup_report() {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
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

        egui::Panel::top("tabs").show(ui, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.heading("Mission Humanity");
                ui.separator();
                for view in View::ALL {
                    if ui
                        .selectable_label(self.view == view, view.title())
                        .clicked()
                    {
                        self.view = view;
                    }
                }
            });
            ui.add_space(4.0);
        });

        egui::Panel::bottom("status").show(ui, |ui| {
            ui.add_space(4.0);
            if self.status_line.is_empty() {
                ui.label(egui::RichText::new("ready").weak());
            } else if self.status_is_error {
                ui.colored_label(egui::Color32::from_rgb(200, 70, 70), &self.status_line);
            } else {
                ui.label(&self.status_line);
            }
            ui.add_space(4.0);
        });

        egui::CentralPanel::default().show(ui, |ui| match self.view {
            View::Status => self.status_view(ui),
            View::Launch => self.launch_view(ui),
            View::Report => self.report_view(ui),
        });
    }
}

impl App {
    fn status_view(&mut self, ui: &mut egui::Ui) {
        ui.heading("Status");
        ui.add_space(6.0);

        ui.label("Game directory");
        ui.horizontal(|ui| {
            let changed = ui
                .add(
                    egui::TextEdit::singleline(&mut self.game_dir_edit)
                        .desired_width(f32::INFINITY)
                        .hint_text("the folder holding mh.exe"),
                )
                .changed();
            if changed {
                self.persist();
            }
        });
        ui.horizontal(|ui| {
            if ui.button("Browse...").clicked() {
                self.browse_game_dir();
            }
            match self.game_dir() {
                None => ui.label(egui::RichText::new("not set").weak()),
                Some(d) if paths::is_game_dir(&d) => ui.colored_label(
                    egui::Color32::from_rgb(70, 160, 90),
                    format!("OK -- {} is here", paths::GAME_EXE),
                ),
                Some(_) => ui.colored_label(
                    egui::Color32::from_rgb(200, 70, 70),
                    format!("no {} in this folder", paths::GAME_EXE),
                ),
            };
        });

        ui.add_space(10.0);
        ui.separator();
        ui.add_space(6.0);

        ui.label("Installed");
        let installed = self.game_dir().and_then(|d| install::read_manifest(&d));
        match &installed {
            Some(m) => {
                ui.monospace(format!(
                    "version {}   configuration {}   ({} file(s), installed {})",
                    m.version,
                    m.tag,
                    m.files.len(),
                    m.installed_at
                ));
                ui.label(
                    egui::RichText::new(format!("from {}", m.package))
                        .weak()
                        .small(),
                );
            }
            None => {
                ui.label(egui::RichText::new("nothing installed in this folder yet").weak());
            }
        }

        ui.add_space(10.0);
        ui.label("Install from a release zip");
        ui.horizontal(|ui| {
            ui.add(
                egui::TextEdit::singleline(&mut self.zip_edit)
                    .desired_width(f32::INFINITY)
                    .hint_text("mission_humanity_re-<version>-<configuration>.zip"),
            );
        });
        ui.horizontal(|ui| {
            if ui.button("Choose zip...").clicked() {
                if let Some(p) = rfd::FileDialog::new()
                    .add_filter("release zip", &["zip"])
                    .set_title("Pick a Mission Humanity release zip")
                    .pick_file()
                {
                    self.zip_edit = p.display().to_string();
                }
            }
            let can = !self.zip_edit.trim().is_empty() && self.game_dir_ok();
            if ui
                .add_enabled(can, egui::Button::new("Install"))
                .on_disabled_hover_text("needs a game directory with mh.exe and a zip")
                .clicked()
            {
                let zip = PathBuf::from(self.zip_edit.trim());
                self.do_install(zip);
            }
            if ui
                .add_enabled(installed.is_some(), egui::Button::new("Uninstall"))
                .on_disabled_hover_text("nothing here was installed by this launcher")
                .clicked()
            {
                self.do_uninstall();
            }
        });

        ui.add_space(10.0);
        ui.separator();
        ui.add_space(6.0);
        self.update_block(ui);

        ui.add_space(8.0);
        ui.collapsing("Launcher files", |ui| {
            ui.monospace(self.layout.root.display().to_string());
            match log::path() {
                Some(p) => ui.monospace(format!("log: {}", p.display())),
                None => ui.monospace("log: (not open)"),
            };
            if let Some(problem) = log::problem() {
                ui.colored_label(egui::Color32::from_rgb(200, 70, 70), problem);
            }
            for line in log::recent(12) {
                ui.small(line);
            }
        });
    }

    /// The update half of the status view (dist LA2).
    ///
    /// It says out loud what it is about to trust, because "where does this come from and who
    /// signed it" is the one question a program that downloads executables owes its user an answer
    /// to without being asked.
    fn update_block(&mut self, ui: &mut egui::Ui) {
        ui.label("Updates");
        ui.horizontal(|ui| {
            let changed = ui
                .add(
                    egui::TextEdit::singleline(&mut self.update_base_edit)
                        .desired_width(f32::INFINITY)
                        .hint_text(if update::DEFAULT_BASE_URL.is_empty() {
                            "https://<your pages site>/  (no build-time default)"
                        } else {
                            update::DEFAULT_BASE_URL
                        }),
                )
                .changed();
            if changed {
                self.config.update_base_url = self.update_base_edit.trim().to_string();
                if let Err(e) = self.config.save(&self.layout.config()) {
                    log::line(format!("config: {e}"));
                }
            }
        });
        ui.label(
            egui::RichText::new(format!(
                "{MANIFEST}, signed with minisign and checked against the key built into this \
                 launcher. Nothing is unpacked before its SHA-256 matches what that signature \
                 covers, and the previous version stays on disk until the new one has run once.",
                MANIFEST = update::MANIFEST_NAME
            ))
            .weak()
            .small(),
        );

        let busy = self.job.is_some();
        ui.add_space(4.0);
        // dist LA12: ONE BUTTON. It was three (Check for updates / Update the game / Update the
        // launcher) until 2026-09-21, and a player who pressed the middle one first was then on a
        // launcher that could not update itself from the same page without a restart. The check
        // is automatic now (every start); the button does the launcher first, then the game.
        ui.horizontal(|ui| {
            self.update_button(ui, busy);
            if busy {
                ui.spinner();
            }
        });

        if !self.update_line.is_empty() {
            ui.add_space(4.0);
            if self.update_is_error {
                ui.colored_label(egui::Color32::from_rgb(200, 70, 70), &self.update_line);
            } else {
                ui.label(&self.update_line);
            }
        }

        let kept = update::version_dirs(&self.layout);
        if !kept.is_empty() {
            ui.label(
                egui::RichText::new(format!("version sets on this machine: {}", kept.join(", ")))
                    .weak()
                    .small(),
            );
        }
    }

    /// dist LA12: the one Update button, on both pages. Enabled whenever nothing is running and a
    /// game directory is known; its label carries the offer when there is one.
    fn update_button(&mut self, ui: &mut egui::Ui, busy: bool) {
        let label = match self.offer.as_ref().and_then(Offer::line) {
            Some(line) => format!("Update  ({line})"),
            None => "Update".to_string(),
        };
        if ui
            .add_enabled(!busy && self.game_dir_ok(), egui::Button::new(label))
            .on_hover_text(
                "The launcher first -- a newer one is downloaded, made to prove it starts, swapped \
                 in and restarted -- then the game for the chosen configuration. One press.",
            )
            .on_disabled_hover_text(if busy {
                "the launcher is busy"
            } else {
                "needs a game directory with mh.exe"
            })
            .clicked()
        {
            self.start_update_from(UpdateAction::Update, Some(self.view));
        }
    }

    /// dist LA12: what the start-up check found, on the Play page. Silent until it has answered;
    /// "up to date" is said once it has.
    fn offer_line(&self) -> String {
        match self.offer.as_ref() {
            None if self
                .job
                .as_ref()
                .is_some_and(|j| j.action == UpdateAction::AutoCheck) =>
            {
                "checking for updates...".to_string()
            }
            None => String::new(),
            Some(o) => o.line().unwrap_or_else(|| "up to date".to_string()),
        }
    }

    /// The folder picker behind every "Browse..." button. One function, because the Status tab
    /// and the Play tab's prompt (dist LA7) must set the same field and say the same thing.
    fn browse_game_dir(&mut self) {
        let start = self.game_dir().filter(|d| d.is_dir());
        let mut dlg = rfd::FileDialog::new().set_title("Where is mh.exe?");
        if let Some(d) = start {
            dlg = dlg.set_directory(d);
        }
        if let Some(picked) = dlg.pick_folder() {
            self.game_dir_edit = picked.display().to_string();
            self.persist();
            let ok = self.game_dir_ok();
            self.say(
                format!(
                    "game directory set to {}{}",
                    self.game_dir_edit,
                    if ok {
                        ""
                    } else {
                        " -- but there is no mh.exe in it"
                    }
                ),
                !ok,
            );
        }
    }

    /// dist LA7: the prompt, shown on the front page ONLY when no directory was found -- the
    /// launcher's own folder, the current directory and the saved setting were all tried first
    /// (`paths::resolve_game_dir`). It is the same field the Status tab edits.
    fn game_dir_prompt(&mut self, ui: &mut egui::Ui) {
        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.label(egui::RichText::new(format!("Where is {}?", paths::GAME_EXE)).strong());
            ui.label(
                egui::RichText::new(
                    "Put this launcher into the game folder and it finds the game by itself. \
                     Otherwise, point it there once:",
                )
                .weak()
                .small(),
            );
            ui.horizontal(|ui| {
                // The button FIRST: an infinitely wide field placed before it would push it off
                // the right edge of the window (seen on the first live run).
                if ui.button("Browse...").clicked() {
                    self.browse_game_dir();
                }
                let changed = ui
                    .add(
                        egui::TextEdit::singleline(&mut self.game_dir_edit)
                            .desired_width(f32::INFINITY)
                            .hint_text("the folder holding mh.exe"),
                    )
                    .changed();
                if changed {
                    self.persist();
                }
            });
            if let Some(d) = self.game_dir() {
                if !paths::is_game_dir(&d) {
                    ui.colored_label(
                        egui::Color32::from_rgb(200, 70, 70),
                        format!("no {} in {}", paths::GAME_EXE, d.display()),
                    );
                }
            }
        });
    }

    /// dist LA8: "Configuration: net -- …" with a *Change...* button, or the open picker: three
    /// radio lines, one per manifest tag, `net` preselected on a fresh machine. Picking writes
    /// `launcher.toml`; nothing is installed until Play, which installs (or switches to)
    /// the picked one before starting the game.
    fn configuration_block(&mut self, ui: &mut egui::Ui, installed: Option<&install::Manifest>) {
        let picked = self.tag_pick.clone();
        let picked_line = configuration_line(&picked);
        if !self.picker_open {
            ui.horizontal(|ui| {
                ui.label("Configuration:");
                ui.label(egui::RichText::new(&picked).strong());
                ui.label(
                    egui::RichText::new(format!("-- {picked_line}"))
                        .weak()
                        .small(),
                );
                if ui.small_button("Change...").clicked() {
                    self.picker_open = true;
                }
            });
        } else {
            egui::Frame::group(ui.style()).show(ui, |ui| {
                ui.label(egui::RichText::new("Which configuration?").strong());
                for (tag, line) in CONFIGURATIONS {
                    ui.horizontal(|ui| {
                        if ui.radio(picked == tag, tag).clicked() {
                            self.pick_tag(tag);
                        }
                        ui.label(egui::RichText::new(line).weak().small());
                    });
                }
                ui.label(
                    egui::RichText::new(match installed {
                        None => format!(
                            "Nothing is installed in this folder yet: Play downloads \
                             and installs the {picked} configuration first, then starts the game.",
                        ),
                        Some(m) if m.tag == picked => format!(
                            "{} {} is installed here. Play starts it.",
                            m.tag, m.version
                        ),
                        Some(m) => format!(
                            "{} {} is installed here: Play removes it and installs \
                             {picked} first (the game's own files are put back, then parked again).",
                            m.tag, m.version
                        ),
                    })
                    .weak()
                    .small(),
                );
                if installed.is_some() && ui.small_button("Done").clicked() {
                    self.picker_open = false;
                }
            });
        }
        if let Some(m) = installed {
            ui.label(
                egui::RichText::new(format!(
                    "installed: {} {} ({} file(s))",
                    m.tag,
                    m.version,
                    m.files.len()
                ))
                .weak()
                .small(),
            );
        }
    }

    /// The front page (dist LA6): the Play button, the relay it will use, and the last run.
    fn launch_view(&mut self, ui: &mut egui::Ui) {
        ui.heading("Play");
        ui.add_space(6.0);

        if self.game_dir_ok() {
            if let Some(d) = self.game_dir() {
                ui.monospace(d.display().to_string());
            }
        } else {
            self.game_dir_prompt(ui);
        }
        ui.add_space(8.0);

        // dist LA8: the configuration. What is installed (from the receipt), what is picked, and
        // the picker itself -- open when nothing is installed, or when the player opens it.
        let installed = self.game_dir().and_then(|d| install::read_manifest(&d));
        let busy = self.job.is_some();
        if installed.is_none() {
            self.picker_open = true;
        }
        self.configuration_block(ui, installed.as_ref());
        ui.add_space(8.0);

        let running = self.session.is_some();
        let can = !running && !busy && self.game_dir_ok();
        let why_not = if running {
            "the game is running"
        } else if busy {
            "the launcher is busy with the install / update shown below"
        } else {
            "point the launcher at the folder holding mh.exe first (above)"
        };
        ui.horizontal(|ui| {
            if ui
                .add_enabled(
                    can,
                    egui::Button::new(egui::RichText::new("Play").size(22.0)),
                )
                .on_hover_text(
                    "Starts the game. Then NETWORK GAME -> Create game to host (it is listed on \
                     the relay under your name), or Refresh list to pick a game on the relay.",
                )
                .on_disabled_hover_text(why_not)
                .clicked()
            {
                self.do_launch("Play pressed");
            }
            if running {
                ui.spinner();
            }
        });
        // dist LA12: what the start-up check offers, and the one button that takes it.
        let offer_line = self.offer_line();
        if !offer_line.is_empty() {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new(&offer_line).weak());
                if self.offer.as_ref().is_some_and(Offer::any) {
                    self.update_button(ui, busy || running);
                }
            });
        }
        // dist LA8: the install that Play started, in place -- what the thread is doing
        // now, and, when it is over, what it said.
        if let Some(progress) = self.play_progress() {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(progress);
            });
        } else if self.pending_launch.is_none() && !self.update_line.is_empty() && !busy {
            // The verdict of the last job, on the page it was started from. `update_line` is
            // also the Status tab's line, so this shows only what a Play-started job said.
            if self.last_job_from_play {
                ui.add_space(6.0);
                if self.update_is_error {
                    ui.colored_label(egui::Color32::from_rgb(200, 70, 70), &self.update_line);
                } else {
                    ui.label(egui::RichText::new(&self.update_line).weak().small());
                }
            }
        }
        ui.add_space(6.0);
        match self.relay.as_ref() {
            Some(r) => {
                ui.label(format!(
                    "Relay: {}  (from the signed manifest, version {})",
                    r.addr, self.relay_from
                ));
                ui.label(
                    egui::RichText::new(format!(
                        "Play puts [net] transport=udp and [net] relay= into {} and the \
                         relay's key into {} before the game starts; the lobby itself is the \
                         game's. Nothing else in the configuration is touched.",
                        relay::INI_NAME,
                        relay::KEY_NAME
                    ))
                    .weak()
                    .small(),
                );
            }
            None => {
                ui.label(
                    egui::RichText::new(
                        "No relay: no accepted manifest names one. Play starts the game \
                         as it is configured -- direct play by address. The next start-up check \
                         (or Update) picks one up when the manifest names it.",
                    )
                    .weak()
                    .small(),
                );
            }
        }

        ui.add_space(10.0);
        if let Some(s) = self.session.as_ref() {
            ui.label(format!(
                "running -- pid {}, {:.0}s so far, token {}",
                s.pid(),
                s.started.elapsed().as_secs_f64(),
                launch::describe_elevation(s.elevated)
            ));
            ui.label(
                egui::RichText::new(
                    "The launcher stays open while the game runs: it is holding the process \
                     handle, which is where the exit code lives.",
                )
                .weak()
                .small(),
            );
        } else if let Some(f) = self.last_run.as_ref() {
            ui.separator();
            ui.label("Last run");
            let text = f.outcome.describe();
            if f.outcome.is_crash() {
                ui.colored_label(egui::Color32::from_rgb(200, 70, 70), &text);
                ui.label(format!("after {:.1}s", f.seconds));
                ui.add_space(6.0);
                if ui.button("Report this crash...").clicked() {
                    self.view = View::Report;
                }
            } else {
                ui.label(&text);
                ui.label(format!("after {:.1}s", f.seconds));
            }
        } else {
            ui.label(egui::RichText::new("nothing has been launched yet").weak());
        }
    }

    fn report_view(&mut self, ui: &mut egui::Ui) {
        // THE CONSENT SCREEN REPLACES THE VIEW rather than sitting inside it. While a report is
        // waiting to be agreed to there is exactly one decision in front of the player, and the
        // form that built it is not a second thing to fiddle with (plan D13).
        if self.consent.is_some() {
            self.consent_view(ui);
            return;
        }

        ui.heading("Report a problem");
        ui.add_space(4.0);
        ui.label(
            egui::RichText::new(
                "This builds a zip on your machine and shows you every file in it. Nothing is \
                 sent until you have read that list and said so.",
            )
            .weak()
            .small(),
        );

        // dist RP1: the "kept, and offered again" half. Unprompted, at the top, on every launch
        // until the outbox is empty -- a report that is only re-offered if the player thinks to
        // look is a report that is never sent.
        if !self.outbox_pending.is_empty() {
            ui.add_space(8.0);
            egui::Frame::group(ui.style()).show(ui, |ui| {
                ui.label(
                    egui::RichText::new(format!(
                        "{} report(s) from an earlier session were never sent",
                        self.outbox_pending.len()
                    ))
                    .strong(),
                );
                for p in self.outbox_pending.clone() {
                    ui.horizontal(|ui| {
                        ui.monospace(
                            p.file_name()
                                .unwrap_or_default()
                                .to_string_lossy()
                                .to_string(),
                        );
                        if ui.button("Look at it and send it").clicked() {
                            self.ask_consent(p.clone());
                        }
                    });
                }
            });
        }
        if !self.upload_line.is_empty() {
            ui.add_space(6.0);
            if self.upload_is_error {
                ui.colored_label(egui::Color32::from_rgb(200, 70, 70), &self.upload_line);
            } else {
                ui.label(&self.upload_line);
            }
        }
        ui.add_space(10.0);

        ui.label(egui::RichText::new("Description (required)").strong());
        ui.add(
            egui::TextEdit::multiline(&mut self.description)
                .desired_width(f32::INFINITY)
                .desired_rows(5)
                .hint_text("What were you doing? What did you expect to happen?"),
        );
        let have_description = report::description_ok(&self.description);
        if !have_description {
            ui.colored_label(egui::Color32::from_rgb(200, 70, 70), report::NO_DESCRIPTION);
        }

        ui.add_space(10.0);
        // dist LA9: which match this report is about -- a picker when there is more than one.
        self.session_picker_block(ui);

        ui.add_space(10.0);
        ui.label("What goes in");
        match self.last_run.as_ref() {
            Some(f) => ui.monospace(format!("exit: {}", f.outcome.describe())),
            None => ui.monospace("exit: (no run in this launcher session)"),
        };
        let session = self.chosen_session_dir();
        match session.as_ref() {
            Some(dir) => {
                ui.monospace(format!("session log directory: {}", dir.display()));
                let count = std::fs::read_dir(dir)
                    .map(|r| r.flatten().count())
                    .unwrap_or(0);
                ui.label(
                    egui::RichText::new(format!("{count} file(s) from it"))
                        .weak()
                        .small(),
                );
            }
            None => {
                ui.monospace("session log directory: none found under the game folder");
            }
        };
        ui.label(
            egui::RichText::new(
                "every process and match directory written since this launcher started (or the \
                 newest handful, whichever is known), plus any crash marker -- dist LA9",
            )
            .weak()
            .small(),
        );
        ui.monospace("config: mh_net.ini, with any key/token setting blanked");
        ui.label(
            egui::RichText::new(
                "The multiplayer key never goes in a report: mh_key.txt is excluded outright and \
                 any 64-digit key printed into a log is blanked before the log is added.",
            )
            .weak()
            .small(),
        );

        if let Some(m) = self.marker.as_ref() {
            ui.add_space(6.0);
            ui.colored_label(
                egui::Color32::from_rgb(200, 70, 70),
                format!(
                    "crash: 0x{:08x} in {} (thread {})",
                    m.code,
                    m.where_text(),
                    m.tid
                ),
            );
        }

        ui.add_space(6.0);
        match self.dump.as_ref() {
            Some(p) => {
                let mb = std::fs::metadata(p).map(|m| m.len()).unwrap_or(0) as f64 / 1_048_576.0;
                ui.checkbox(
                    &mut self.include_dump,
                    format!("Include the memory snapshot ({mb:.1} MB)"),
                );
                ui.label(
                    egui::RichText::new(
                        "A minidump: the stacks, the loaded modules and the game's own data \
                         section at the moment it faulted. It is what turns \"it crashed\" into a \
                         line of code. It is not a full memory capture.",
                    )
                    .weak()
                    .small(),
                );
            }
            None => {
                ui.label(
                    egui::RichText::new("no memory snapshot for this run")
                        .weak()
                        .small(),
                );
            }
        }

        ui.add_space(10.0);
        ui.horizontal(|ui| {
            if ui
                .add_enabled(have_description, egui::Button::new("Build the report"))
                .on_disabled_hover_text(report::NO_DESCRIPTION)
                .clicked()
            {
                self.do_report(None);
            }
            if self.built.is_some() && ui.button("Show me the file").clicked() {
                if let Some(b) = self.built.as_ref() {
                    let dir = b.zip.parent().unwrap_or(&b.zip).to_path_buf();
                    // `explorer` rather than a shell association: the target is a folder, and a
                    // launcher that opened the zip itself would hand it to whatever the player has
                    // associated with .zip, which is not what "show me" means.
                    let _ = std::process::Command::new("explorer").arg(dir).spawn();
                }
            }
        });

        if let Some(b) = self.built.as_ref() {
            ui.add_space(8.0);
            ui.monospace(b.zip.display().to_string());
            ui.collapsing(format!("{} file(s) in the report", b.entries.len()), |ui| {
                for e in &b.entries {
                    ui.small(e);
                }
            });
            // The whole of report.json, on screen, before anything is sent. It is the one part of a
            // report that is ABOUT the player's machine rather than about the game, so showing it
            // is not a debugging affordance -- it is how a player finds out what they would be
            // handing over without having to open a zip to do it.
            ui.collapsing("What report.json says about this machine", |ui| {
                ui.monospace(&b.meta);
            });
        }

        // dist RP1: the send offer. It appears only once a zip EXISTS, and it does not send --
        // it opens the consent screen, which is the thing that can.
        if let Some(zip) = self.built.as_ref().map(|b| b.zip.clone()) {
            ui.add_space(10.0);
            match self.collector() {
                Ok(c) => {
                    ui.horizontal(|ui| {
                        if ui
                            .add_enabled(
                                self.upload_job.is_none(),
                                egui::Button::new("Send this report..."),
                            )
                            .on_hover_text(format!("to {}", c.endpoint()))
                            .clicked()
                        {
                            self.ask_consent(zip);
                        }
                        if self.upload_job.is_some() {
                            ui.label("sending...");
                        }
                    });
                    ui.label(
                        egui::RichText::new(
                            "You will see the whole list of files, and what they say about your \
                             machine, before anything leaves it.",
                        )
                        .weak()
                        .small(),
                    );
                }
                Err(e) => {
                    ui.label(
                        egui::RichText::new(format!("This build cannot send reports: {e}"))
                            .weak()
                            .small(),
                    );
                }
            }
        }
    }

    /// **The consent screen (dist RP1, plan decision D13).** Every entry in the zip, its digest,
    /// the destination and the whole of `report.json`, then two buttons. It renders
    /// `upload::consent_text` -- the same text `--send` prints -- so the window and the command
    /// line cannot describe the same upload differently.
    fn consent_view(&mut self, ui: &mut egui::Ui) {
        let Some(p) = self.consent.clone() else {
            return;
        };
        ui.heading("Before this is sent");
        ui.add_space(4.0);
        ui.label(
            egui::RichText::new(
                "This is everything that would leave your machine. Nothing else is read, and \
                 nothing is sent unless you press Send.",
            )
            .strong(),
        );
        ui.add_space(8.0);
        egui::ScrollArea::vertical()
            .max_height(360.0)
            .auto_shrink([false, true])
            .show(ui, |ui| {
                ui.monospace(upload::consent_text(&p));
            });
        ui.add_space(10.0);
        ui.horizontal(|ui| {
            if ui.button(format!("Send it to {}", p.destination)).clicked() {
                self.send_consented();
            }
            if ui.button("Don't send it").clicked() {
                log::line("upload: the player declined to send");
                self.consent = None;
                self.upload_say(
                    format!(
                        "nothing was sent -- the report is still at {}",
                        p.zip.display()
                    ),
                    false,
                );
            }
        });
    }
}

/// `20260917T164346Z` -- the same UTC shape the game's session directories use, so a report file
/// sorts next to the session it is about.
fn stamp_for_file() -> String {
    chrono::Utc::now().format("%Y%m%dT%H%M%SZ").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_view_has_a_name_that_parses_back() {
        for v in View::ALL {
            assert_eq!(View::parse(v.title()), Some(v), "{}", v.title());
        }
        assert_eq!(View::parse("REPORT"), Some(View::Report));
        assert_eq!(View::parse("nonsense"), None);
    }
}
