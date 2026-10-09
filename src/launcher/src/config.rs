//! `launcher.toml` -- the two facts the launcher has to remember between runs: where the game is,
//! and what it last installed there.
//!
//! TOML rather than JSON because a player who opens it should be able to fix it, and a missing or
//! unparseable file is not an error: it is a first run. `load` therefore never fails -- it returns
//! the defaults and says in the log why -- while `save` does report failure, because a setting that
//! silently did not persist is a bug the player would only meet on the next launch.

use serde::{Deserialize, Serialize};
use std::path::Path;

/// The configuration a fresh machine gets when nobody has picked one.
pub const DEFAULT_TAG: &str = "net";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    /// The game directory the player picked. Empty until they pick one.
    pub game_dir: String,
    /// The version last installed into that directory, as the zip name claimed it.
    pub installed_version: String,
    /// Which of the three release zips it was (`net`, `net-debug`, `brokered-debug`).
    pub installed_tag: String,
    /// The site the channel manifests are fetched from (dist LA2; `channels/<channel>/launcher.json`
    /// and `game.json` since RL8, `update::manifest_url`). Empty means "the built-in default", which
    /// is how a config file written before LA2 keeps working and how the field stays absent from a
    /// file nobody has changed -- an empty string in the TOML would look like a setting.
    ///
    /// It is a SETTING rather than a constant because the acceptance run points it at a local HTTP
    /// stand-in, and because a fork of this project publishes its own releases. It is not a
    /// security boundary: the signature is, and moving the URL does not move the key.
    pub update_base_url: String,
    /// dist LA8: the configuration the player picked on the Play page (`net`, `net-debug`,
    /// `brokered-debug`). Empty until they pick one -- the picker then shows what is installed,
    /// or `net`. Kept apart from `installed_tag` because the two disagree for exactly as long as a
    /// switch takes, and the Play page has to know which one the player MEANT.
    pub chosen_tag: String,
    /// dist RL20: Discord Rich Presence on/off (default on, but inert without a client id).
    pub discord: bool,
    /// dist RL20: an override for the Discord application id. Empty (the default) = the built-in
    /// `DISCORD_CLIENT_ID` (`effective_discord_client_id`).
    pub discord_client_id: String,
    /// dist RL8: the release channel this launcher follows -- `stable` or `latest`. Empty (the
    /// default, and every config written before RL8) means `stable`; anything else is logged at
    /// start-up and treated as `stable` too (`channel`, `channel_note`). A SETTING, written only by
    /// an explicit choice (`set_channel`, the Settings page).
    pub channel: String,
    /// dist RL8: the channel the game now on disk was installed from. STATE, not a setting: written
    /// on every game install, empty until the first one. It is what tells an explicit channel switch
    /// (`installed_channel` set and different from `channel`) from a plain update, because only a
    /// switch may install an OLDER game version (`update::check_game_install`). Empty -- a launcher
    /// that arrived through the bridge, or a game dir nothing has been installed into -- is NOT a
    /// switch.
    pub installed_channel: String,
    /// dist RL16: the launcher's own language (`en`, `ru`). Mirrors the Language setting's `[lang]
    /// pack` (one choice drives both); empty = English. Written by the settings page's Apply.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub ui_lang: String,
    /// dist RL4/RL14: how the multiplayer relay is chosen -- `auto` (the signed manifest's, default;
    /// also what an empty value means), `off` (none: direct play by address) or `custom`
    /// (`relay_custom`). Read by `relay::plan_for` at provision time; the Settings page writes it.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub relay_mode: String,
    /// dist RL14: `host:port` of the player's own relay, used when `relay_mode` is `custom`.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub relay_custom: String,
    /// dist RL9: the game version the player ROLLED BACK from (Diagnostics "Roll back"). The silent
    /// auto-update never installs this version or anything not newer than it; the manual Update
    /// button ignores it. Empty = no pin.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub rollback_skip: String,
}

/// dist RL20: the project's Discord application (registered by the maintainer, 2026-10-09). Its Rich
/// Presence art asset is named `mh_logo`.
pub const DISCORD_CLIENT_ID: &str = "1557767154559090749";

impl Config {
    /// The Discord application id the launcher uses: the launcher.toml override if one is set,
    /// otherwise the built-in `DISCORD_CLIENT_ID`.
    pub fn effective_discord_client_id(&self) -> &str {
        let own = self.discord_client_id.trim();
        if own.is_empty() {
            DISCORD_CLIENT_ID
        } else {
            own
        }
    }
}

impl Default for Config {
    fn default() -> Self {
        Self {
            game_dir: String::new(),
            installed_version: String::new(),
            installed_tag: String::new(),
            update_base_url: String::new(),
            chosen_tag: String::new(),
            discord: true,
            discord_client_id: String::new(),
            channel: String::new(),
            installed_channel: String::new(),
            ui_lang: String::new(),
            relay_mode: String::new(),
            relay_custom: String::new(),
            rollback_skip: String::new(),
        }
    }
}

/// The relay modes (dist RL14).
pub const RELAY_MODES: [&str; 3] = ["auto", "off", "custom"];

/// The two release channels (dist RL8). `stable` is what a player who chose nothing follows;
/// `latest` is what a release lands on first and is promoted from.
pub const CHANNELS: [&str; 2] = ["stable", "latest"];

/// The channel a config that names none follows.
pub const DEFAULT_CHANNEL: &str = "stable";

impl Config {
    /// The channel to follow: `latest` only when the config says exactly so, `stable` otherwise
    /// (empty, unknown, or any case/space variant of either name is normalised first).
    pub fn channel(&self) -> &'static str {
        let t = self.channel.trim().to_ascii_lowercase();
        CHANNELS
            .iter()
            .find(|c| **c == t)
            .copied()
            .unwrap_or(DEFAULT_CHANNEL)
    }

    /// The log line for a `channel` value that is set but is not one of `CHANNELS`, so a typo in
    /// `launcher.toml` is a sentence in the log rather than a silent fall-back. `None` when the
    /// value is empty or fine.
    pub fn channel_note(&self) -> Option<String> {
        let t = self.channel.trim();
        if t.is_empty() || CHANNELS.contains(&t.to_ascii_lowercase().as_str()) {
            return None;
        }
        Some(format!(
            "launcher.toml: channel = {t:?} is not one of {CHANNELS:?}; following {DEFAULT_CHANNEL}"
        ))
    }

    /// Is an explicit channel switch in progress -- the game on disk came from a different channel
    /// than the one now followed? Empty `installed_channel` is "unknown", which is not a switch.
    pub fn switch_in_progress(&self) -> bool {
        crate::update::is_switch(&self.installed_channel, self.channel())
    }

    /// Follow `channel` from now on. `Ok(true)` when that changed anything; `Err` for a name that
    /// is not a channel (nothing is changed). `installed_channel` is deliberately left alone: it
    /// is what makes the next game install a switch.
    pub fn set_channel(&mut self, channel: &str) -> Result<bool, String> {
        let wanted = channel.trim().to_ascii_lowercase();
        if !CHANNELS.contains(&wanted.as_str()) {
            return Err(format!(
                "{channel:?} is not a release channel (expected one of {CHANNELS:?})"
            ));
        }
        let changed = self.channel() != wanted || self.channel.trim() != wanted;
        self.channel = wanted;
        Ok(changed)
    }

    /// The relay mode: one of `RELAY_MODES`; empty or unknown is `auto`.
    pub fn relay_mode(&self) -> &'static str {
        let t = self.relay_mode.trim().to_ascii_lowercase();
        RELAY_MODES
            .iter()
            .find(|m| **m == t)
            .copied()
            .unwrap_or("auto")
    }

    /// The launcher's own language code (`en` unless `ui_lang` names another).
    pub fn ui_lang_code(&self) -> String {
        let t = self.ui_lang.trim().to_ascii_lowercase();
        if t.is_empty() {
            "en".to_string()
        } else {
            t
        }
    }

    /// The base URL to fetch the manifest from, with the build-time default filled in. Empty when
    /// neither `launcher.toml` nor the build named one: the caller reports "no update source" and
    /// fetches nothing, rather than guessing a publisher.
    pub fn update_base_url_or_default(&self) -> String {
        let t = self.update_base_url.trim();
        if t.is_empty() {
            crate::update::DEFAULT_BASE_URL.to_string()
        } else {
            t.to_string()
        }
    }

    /// Which configuration to install or update: the one the player picked (dist LA8), else
    /// whatever is installed, and `net` on a machine where neither says anything -- the plain
    /// multiplayer build is what a player who has expressed no preference wants (INSTALL.md's own
    /// table says so). The picker FOLLOWS this and this follows the picker: a pick is written to
    /// `chosen_tag`, and a Play on a directory holding a different tag is a switch, not a launch.
    ///
    /// dist RL7: there is ONE configuration now (`net`). A stored `net-debug` / `brokered-debug`
    /// (an older pick, or an older install's receipt) is read as `net`, with one log line per run
    /// -- the field survives, the retired value does not. Play then SWITCHES such a folder to `net`.
    pub fn update_tag(&self) -> String {
        for t in [self.chosen_tag.trim(), self.installed_tag.trim()] {
            if t.is_empty() {
                continue;
            }
            if t != DEFAULT_TAG {
                static ONCE: std::sync::Once = std::sync::Once::new();
                ONCE.call_once(|| {
                    crate::log::line(format!(
                        "config: configuration {t:?} is retired -- treating it as {DEFAULT_TAG}"
                    ))
                });
            }
            return DEFAULT_TAG.to_string();
        }
        DEFAULT_TAG.to_string()
    }

    /// Read it, or the defaults. The second element is a note for the log when the file was there
    /// and unusable -- distinguishing "first run" from "your settings did not load", which look the
    /// same from inside the UI and are very different things to be told.
    pub fn load(path: &Path) -> (Self, Option<String>) {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return (Self::default(), None),
            Err(e) => {
                return (
                    Self::default(),
                    Some(format!("cannot read {}: {e}", path.display())),
                )
            }
        };
        let (cfg, note) = Self::load_str(&text);
        (
            cfg,
            note.map(|e| {
                format!(
                    "{} is not valid TOML ({e}); starting from defaults",
                    path.display()
                )
            }),
        )
    }

    /// The parse half of `load`, over text. The note is the parse error, if any.
    pub fn load_str(text: &str) -> (Self, Option<String>) {
        match toml::from_str::<Self>(text) {
            Ok(c) => (c, None),
            Err(e) => (Self::default(), Some(e.to_string())),
        }
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)
                .map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
        }
        let text = toml::to_string_pretty(self).map_err(|e| format!("cannot serialise: {e}"))?;
        std::fs::write(path, text).map_err(|e| format!("cannot write {}: {e}", path.display()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_file_is_a_first_run_not_an_error() {
        let (c, note) = Config::load(Path::new("no/such/dir/launcher.toml"));
        assert_eq!(c, Config::default());
        assert!(note.is_none(), "a missing file must not read as a failure");
        assert!(c.game_dir.is_empty());
    }

    #[test]
    fn garbage_loads_as_defaults_and_says_so() {
        let p = std::env::temp_dir().join("mh_launcher_test_bad.toml");
        std::fs::write(&p, "this is not toml = = =").unwrap();
        let (c, note) = Config::load(&p);
        assert_eq!(c, Config::default());
        assert!(note.is_some(), "an unparseable file must be reported");
        std::fs::remove_file(&p).ok();
    }

    #[test]
    fn round_trips() {
        let p = std::env::temp_dir().join("mh_launcher_test_rt.toml");
        let c = Config {
            game_dir: "C:/games/mh".into(),
            installed_version: "0.1.0".into(),
            installed_tag: "net".into(),
            update_base_url: "http://127.0.0.1:8099/".into(),
            chosen_tag: "net-debug".into(),
            discord: false,
            discord_client_id: "42".into(),
            channel: "latest".into(),
            installed_channel: "stable".into(),
            ui_lang: "ru".into(),
            relay_mode: "custom".into(),
            relay_custom: "relay.example.org:7100".into(),
            rollback_skip: "0.3.0".into(),
        };
        c.save(&p).unwrap();
        let (back, note) = Config::load(&p);
        assert!(note.is_none());
        assert_eq!(c, back);
        std::fs::remove_file(&p).ok();
    }

    /// dist LA8: the tag the update path installs is the picker's, then the installed one, then
    /// `net` -- and a config written before the field existed still loads.
    #[test]
    fn the_update_tag_is_net_whatever_retired_value_is_stored() {
        let mut c = Config::default();
        assert_eq!(c.update_tag(), "net");
        c.installed_tag = "brokered-debug".into();
        assert_eq!(c.update_tag(), "net", "a retired install tag reads as net");
        c.chosen_tag = "net-debug".into();
        assert_eq!(c.update_tag(), "net", "a retired pick reads as net");
        c.chosen_tag = "  ".into();
        assert_eq!(c.update_tag(), "net");
        let (old, note) = Config::load_str("game_dir = 'C:/g'\ninstalled_tag = 'net'\n");
        assert!(note.is_none());
        assert_eq!(old.chosen_tag, "");
        assert_eq!(old.update_tag(), "net");
    }

    /// dist RL8: empty and unknown channels follow `stable` (the unknown one with a log line), a
    /// config written before the field existed loads, and only an exact `latest` follows latest.
    #[test]
    fn the_channel_defaults_to_stable_and_unknown_names_are_noted() {
        let mut c = Config::default();
        assert_eq!(c.channel(), "stable");
        assert_eq!(c.channel_note(), None, "empty is the default, not a typo");
        c.channel = "latest".into();
        assert_eq!(c.channel(), "latest");
        c.channel = " Latest ".into();
        assert_eq!(c.channel(), "latest");
        assert_eq!(c.channel_note(), None);
        c.channel = "nightly".into();
        assert_eq!(c.channel(), "stable");
        assert!(c.channel_note().unwrap().contains("nightly"));
        let (old, note) = Config::load_str("game_dir = 'C:/g'\n");
        assert!(note.is_none());
        assert_eq!(
            (old.channel(), old.installed_channel.as_str()),
            ("stable", "")
        );
    }

    /// dist RL8: a switch is "installed from X, now following Y". An empty `installed_channel`
    /// (bridge arrival, nothing installed yet) is not a switch -- the strict NOT NEWER applies.
    #[test]
    fn a_switch_needs_a_known_installed_channel_that_differs() {
        let mut c = Config::default();
        assert!(!c.switch_in_progress());
        c.set_channel("latest").unwrap();
        assert!(
            !c.switch_in_progress(),
            "nothing known to have been installed"
        );
        c.installed_channel = "latest".into();
        assert!(!c.switch_in_progress());
        c.set_channel("stable").unwrap();
        assert!(
            c.switch_in_progress(),
            "installed from latest, following stable"
        );
        c.installed_channel = "stable".into();
        assert!(!c.switch_in_progress());
    }

    /// dist RL14: the three new fields stay out of a file nobody changed, default sensibly and load
    /// from a config written before they existed.
    #[test]
    fn the_language_and_relay_fields_are_optional_and_default_quietly() {
        let text = toml::to_string_pretty(&Config::default()).unwrap();
        for k in ["ui_lang", "relay_mode", "relay_custom"] {
            assert!(
                !text.contains(k),
                "{k} written for a default config:\n{text}"
            );
        }
        let (c, note) = Config::load_str("game_dir = 'C:/g'\n");
        assert!(note.is_none());
        assert_eq!((c.relay_mode(), c.ui_lang_code().as_str()), ("auto", "en"));
        let (c, _) = Config::load_str("relay_mode = ' OFF '\nui_lang = 'RU'\n");
        assert_eq!((c.relay_mode(), c.ui_lang_code().as_str()), ("off", "ru"));
        let (c, _) = Config::load_str("relay_mode = 'weird'\n");
        assert_eq!(c.relay_mode(), "auto");
    }

    #[test]
    fn set_channel_refuses_a_name_that_is_not_a_channel() {
        let mut c = Config::default();
        assert!(c.set_channel("nightly").is_err());
        assert_eq!(c.channel, "", "a refused switch changes nothing");
        assert_eq!(c.set_channel("LATEST"), Ok(true));
        assert_eq!(c.channel, "latest");
        assert_eq!(c.set_channel("latest"), Ok(false));
    }
}
