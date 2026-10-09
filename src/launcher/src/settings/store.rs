//! `launcher.toml` as a settings store (dist RL14): the `LauncherStore` the model reads and writes
//! launcher-owned rows through (`channel`, `discord`, `relay_mode`, `relay_custom`, and the
//! `ui_lang` mirror of the Language row).
//!
//! It borrows the App's live `Config` instead of re-reading the file, so an Apply changes what the
//! rest of the launcher sees at once, and `flush` is the one place the file is written.

use std::path::PathBuf;

use super::model::LauncherStore;
use crate::config::{Config, RELAY_MODES};

pub struct ConfigStore<'a> {
    pub cfg: &'a mut Config,
    /// Where `flush` saves (`Layout::config()`).
    pub path: PathBuf,
}

impl<'a> ConfigStore<'a> {
    pub fn new(cfg: &'a mut Config, path: PathBuf) -> Self {
        Self { cfg, path }
    }
}

impl LauncherStore for ConfigStore<'_> {
    fn get(&self, field: &str) -> Option<String> {
        Some(match field {
            "channel" => self.cfg.channel().to_string(),
            "discord" => self.cfg.discord.to_string(),
            "relay_mode" => self.cfg.relay_mode().to_string(),
            "relay_custom" => self.cfg.relay_custom.clone(),
            "ui_lang" => self.cfg.ui_lang_code(),
            _ => return None,
        })
    }

    fn set(&mut self, field: &str, value: &str) -> Result<(), String> {
        match field {
            "channel" => {
                self.cfg.set_channel(value)?;
            }
            "discord" => {
                self.cfg.discord = matches!(value.trim(), "true" | "1" | "on");
            }
            "relay_mode" => {
                let v = value.trim().to_ascii_lowercase();
                if !RELAY_MODES.contains(&v.as_str()) {
                    return Err(format!("{value:?} is not a relay mode ({RELAY_MODES:?})"));
                }
                // `auto` is the default and stays out of the file.
                self.cfg.relay_mode = if v == "auto" { String::new() } else { v };
            }
            "relay_custom" => self.cfg.relay_custom = value.trim().to_string(),
            "ui_lang" => {
                let v = value.trim().to_ascii_lowercase();
                self.cfg.ui_lang = if v == "en" { String::new() } else { v };
            }
            other => return Err(format!("launcher.toml has no setting {other:?}")),
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<(), String> {
        self.cfg.save(&self.path)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::model::Model;
    use crate::settings::schema::Schema;

    #[test]
    fn rows_read_and_write_through_the_config_and_flush_saves_the_file() {
        let dir = std::env::temp_dir().join("mh_launcher_test_config_store");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("mh_net.ini");
        std::fs::write(&ini, "[lang]\r\npack=\r\n").unwrap();
        let toml_path = dir.join("launcher.toml");
        let mut cfg = Config::default();
        let mut m = Model::new(Schema::builtin());
        m.load(
            Some(b"[lang]\r\npack=\r\n"),
            &ConfigStore::new(&mut cfg, toml_path.clone()),
        );
        assert_eq!(m.value("launcher.channel"), "stable");
        assert_eq!(m.value("launcher.relay_mode"), "auto");
        assert_eq!(m.value("launcher.discord"), "1");
        m.set("launcher.channel", "latest");
        m.set("launcher.relay_mode", "custom");
        m.set("launcher.relay_custom", "relay.example.org:7100");
        m.set("launcher.discord", "0");
        m.set("lang.pack", "ru");
        let mut store = ConfigStore::new(&mut cfg, toml_path.clone());
        m.apply(&ini, &mut store).unwrap();
        assert_eq!(cfg.channel(), "latest");
        assert_eq!(cfg.relay_mode(), "custom");
        assert_eq!(cfg.relay_custom, "relay.example.org:7100");
        assert!(!cfg.discord);
        assert_eq!(cfg.ui_lang_code(), "ru", "the Language row drives ui_lang");
        let (back, note) = Config::load(&toml_path);
        assert!(note.is_none());
        assert_eq!(back, cfg);
        assert!(std::fs::read_to_string(&ini).unwrap().contains("pack=ru"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn unknown_fields_and_bad_modes_are_refused() {
        let mut cfg = Config::default();
        let mut s = ConfigStore::new(&mut cfg, PathBuf::from("unused.toml"));
        assert!(s.set("nope", "x").is_err());
        assert!(s.set("relay_mode", "sideways").is_err());
        assert!(s.set("channel", "nightly").is_err());
        assert_eq!(s.get("nope"), None);
    }
}
