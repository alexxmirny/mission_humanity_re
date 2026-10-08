//! Discord Rich Presence over the raw local IPC pipe (dist RL20, plan decision D14).
//!
//! No Discord SDK and no new dependency: Discord listens on `\\?\pipe\discord-ipc-{0..9}` and
//! speaks frames of an 8-byte header (`u32` opcode, `u32` payload length, both little-endian) plus
//! a JSON payload. The game writes `presence.json` (schema in docs/launcher.md); this module polls
//! it from a worker thread, maps it to an activity and sends `SET_ACTIVITY`. The UI thread never
//! touches the pipe, and an absent Discord is silence, not an error.

use serde::Deserialize;
use serde_json::json;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

/// The file the game writes under the log root.
pub const PRESENCE_FILE: &str = "presence.json";
/// The art key uploaded to the Discord application (user action, RL20).
pub const LARGE_IMAGE: &str = "mh_logo";
const POLL: Duration = Duration::from_secs(5);

pub const OP_HANDSHAKE: u32 = 0;
pub const OP_FRAME: u32 = 1;
pub const OP_CLOSE: u32 = 2;

// ---- framing ------------------------------------------------------------------------------

pub fn encode_frame(op: u32, payload: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(8 + payload.len());
    v.extend_from_slice(&op.to_le_bytes());
    v.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    v.extend_from_slice(payload.as_bytes());
    v
}

pub fn read_frame(r: &mut impl Read) -> std::io::Result<(u32, String)> {
    let mut h = [0u8; 8];
    r.read_exact(&mut h)?;
    let op = u32::from_le_bytes([h[0], h[1], h[2], h[3]]);
    let len = u32::from_le_bytes([h[4], h[5], h[6], h[7]]) as usize;
    if len > 1 << 20 {
        return Err(std::io::Error::other("oversized discord frame"));
    }
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    Ok((op, String::from_utf8_lossy(&buf).into_owned()))
}

// ---- presence.json -> activity ------------------------------------------------------------

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct Presence {
    pub schema: u32,
    pub pid: u32,
    pub state: String,
    pub mode: Option<String>,
    pub map: Option<String>,
    pub players: Option<u32>,
    pub max_players: Option<u32>,
    pub system: Option<String>,
    pub planet: Option<String>,
    pub started_unix: Option<u64>,
    pub updated_unix: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Activity {
    pub details: String,
    pub state: Option<String>,
    pub start: Option<u64>,
    pub party: Option<(u32, u32)>,
}

fn nonempty(s: &Option<String>) -> Option<&str> {
    s.as_deref().map(str::trim).filter(|t| !t.is_empty())
}

/// Map a parsed presence to an activity. `None` means "show nothing" (unknown schema/state).
pub fn activity_for(p: &Presence) -> Option<Activity> {
    if p.schema != 1 {
        return None;
    }
    let party = match (p.players, p.max_players) {
        (Some(n), Some(m)) if m > 0 => Some((n.min(m), m)),
        _ => None,
    };
    let start = p.started_unix.filter(|&s| s > 0);
    let a = match p.state.as_str() {
        "menu" => Activity {
            details: "In the menu".into(),
            state: None,
            start: None,
            party: None,
        },
        "lobby" => Activity {
            details: "In a lobby".into(),
            state: party.map(|(n, m)| format!("{n}/{m} players")),
            start: None,
            party,
        },
        "match" => Activity {
            details: match p.mode.as_deref() {
                Some("skirmish") => "Skirmish",
                Some("network") => "Multiplayer match",
                Some("tactical") => "Tactical battle",
                _ => "In a match",
            }
            .into(),
            state: nonempty(&p.map).map(str::to_string),
            start,
            party,
        },
        "campaign" => Activity {
            details: "Campaign".into(),
            state: match (nonempty(&p.system), nonempty(&p.planet)) {
                (Some(s), Some(pl)) => Some(format!("{s} \u{b7} {pl}")),
                (Some(x), None) | (None, Some(x)) => Some(x.to_string()),
                (None, None) => None,
            },
            start,
            party: None,
        },
        "tutorial" => Activity {
            details: "Tutorial".into(),
            state: None,
            start,
            party: None,
        },
        _ => return None,
    };
    Some(a)
}

/// Read and map the file. Missing, unparseable, or written by another process -> `None` (clear).
pub fn read_activity(path: &Path, game_pid: u32) -> Option<Activity> {
    let text = std::fs::read_to_string(path).ok()?;
    let p: Presence = serde_json::from_str(&text).ok()?;
    if p.pid != 0 && game_pid != 0 && p.pid != game_pid {
        return None;
    }
    activity_for(&p)
}

pub fn set_activity_json(pid: u32, act: Option<&Activity>, nonce: u64) -> String {
    let mut args = json!({ "pid": pid });
    if let Some(a) = act {
        let mut o = json!({
            "details": a.details,
            "assets": { "large_image": LARGE_IMAGE, "large_text": "Mission Humanity" },
        });
        if let Some(s) = &a.state {
            o["state"] = json!(s);
        }
        if let Some(t) = a.start {
            o["timestamps"] = json!({ "start": t });
        }
        if let Some((n, m)) = a.party {
            o["party"] = json!({ "id": "mh-party", "size": [n, m] });
        }
        args["activity"] = o;
    }
    json!({ "cmd": "SET_ACTIVITY", "args": args, "nonce": nonce.to_string() }).to_string()
}

// ---- the pipe -----------------------------------------------------------------------------

struct Conn {
    pipe: std::fs::File,
    nonce: u64,
}

impl Conn {
    fn open(client_id: &str) -> Option<Self> {
        for i in 0..10 {
            let name = format!(r"\\?\pipe\discord-ipc-{i}");
            let Ok(mut pipe) = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&name)
            else {
                continue;
            };
            let hs = json!({ "v": 1, "client_id": client_id }).to_string();
            if pipe.write_all(&encode_frame(OP_HANDSHAKE, &hs)).is_ok()
                && matches!(read_frame(&mut pipe), Ok((OP_FRAME, _)))
            {
                return Some(Self { pipe, nonce: 0 });
            }
        }
        None
    }

    fn set(&mut self, pid: u32, act: Option<&Activity>) -> std::io::Result<()> {
        self.nonce += 1;
        let body = set_activity_json(pid, act, self.nonce);
        self.pipe.write_all(&encode_frame(OP_FRAME, &body))?;
        let (op, _) = read_frame(&mut self.pipe)?;
        if op == OP_CLOSE {
            return Err(std::io::Error::other("discord closed the pipe"));
        }
        Ok(())
    }
}

/// A running presence worker. Dropping it clears the status and ends the thread.
pub struct Handle {
    stop: Arc<AtomicBool>,
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

/// Start the worker. `None` when the feature is off (disabled or empty client id) -- silently.
pub fn start(client_id: &str, enabled: bool, file: PathBuf, game_pid: u32) -> Option<Handle> {
    let id = client_id.trim().to_string();
    if !enabled || id.is_empty() {
        return None;
    }
    let stop = Arc::new(AtomicBool::new(false));
    let flag = stop.clone();
    std::thread::Builder::new()
        .name("discord-presence".into())
        .spawn(move || run(&id, &file, game_pid, &flag))
        .ok()?;
    Some(Handle { stop })
}

fn run(id: &str, file: &Path, pid: u32, stop: &AtomicBool) {
    let mut conn: Option<Conn> = None;
    let mut last: Option<Option<Activity>> = None;
    while !stop.load(Ordering::Relaxed) {
        let want = read_activity(file, pid);
        if conn.is_none() {
            conn = Conn::open(id);
            last = None;
        }
        if let Some(c) = conn.as_mut() {
            if last.as_ref() != Some(&want) {
                match c.set(pid, want.as_ref()) {
                    Ok(()) => last = Some(want),
                    Err(_) => conn = None,
                }
            }
        }
        let mut waited = Duration::ZERO;
        while waited < POLL && !stop.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(100));
            waited += Duration::from_millis(100);
        }
    }
    if let Some(c) = conn.as_mut() {
        let _ = c.set(pid, None);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn p(json: &str) -> Presence {
        serde_json::from_str(json).unwrap()
    }

    #[test]
    fn frame_round_trips_little_endian() {
        let f = encode_frame(OP_FRAME, "{\"a\":1}");
        assert_eq!(&f[..8], &[1, 0, 0, 0, 7, 0, 0, 0]);
        let (op, s) = read_frame(&mut std::io::Cursor::new(f)).unwrap();
        assert_eq!((op, s.as_str()), (OP_FRAME, "{\"a\":1}"));
        assert!(read_frame(&mut std::io::Cursor::new(vec![1, 0, 0])).is_err());
    }

    #[test]
    fn menu_and_lobby() {
        let a = activity_for(&p(r#"{"schema":1,"state":"menu"}"#)).unwrap();
        assert_eq!(a.details, "In the menu");
        let a = activity_for(&p(
            r#"{"schema":1,"state":"lobby","players":2,"max_players":4}"#,
        ))
        .unwrap();
        assert_eq!(a.state.as_deref(), Some("2/4 players"));
        assert_eq!(a.party, Some((2, 4)));
    }

    #[test]
    fn match_has_map_and_start() {
        let a = activity_for(&p(
            r#"{"schema":1,"state":"match","mode":"network","map":"Dunes","started_unix":1000}"#,
        ))
        .unwrap();
        assert_eq!(a.details, "Multiplayer match");
        assert_eq!(a.state.as_deref(), Some("Dunes"));
        assert_eq!(a.start, Some(1000));
    }

    #[test]
    fn campaign_planet_switch_changes_state() {
        let a = activity_for(&p(
            r#"{"schema":1,"state":"campaign","system":"Sol","planet":"Mars"}"#,
        ))
        .unwrap();
        let b = activity_for(&p(
            r#"{"schema":1,"state":"campaign","system":"Sol","planet":"Venus"}"#,
        ))
        .unwrap();
        assert_eq!(a.details, "Campaign");
        assert_eq!(a.state.as_deref(), Some("Sol \u{b7} Mars"));
        assert_eq!(b.state.as_deref(), Some("Sol \u{b7} Venus"));
        assert_ne!(a, b);
    }

    #[test]
    fn missing_bad_or_foreign_file_clears() {
        let d = std::env::temp_dir().join("mh_discord_test_presence");
        std::fs::create_dir_all(&d).unwrap();
        let f = d.join(PRESENCE_FILE);
        std::fs::remove_file(&f).ok();
        assert_eq!(read_activity(&f, 10), None);
        std::fs::write(&f, r#"{"schema":1,"pid":11,"state":"menu"}"#).unwrap();
        assert_eq!(read_activity(&f, 10), None, "another pid's file");
        assert!(read_activity(&f, 11).is_some());
        std::fs::write(&f, "{not json").unwrap();
        assert_eq!(read_activity(&f, 11), None);
        assert_eq!(activity_for(&p(r#"{"schema":2,"state":"menu"}"#)), None);
        std::fs::remove_dir_all(&d).ok();
    }

    #[test]
    fn set_activity_shape() {
        let a = Activity {
            details: "x".into(),
            state: Some("y".into()),
            start: Some(5),
            party: Some((1, 2)),
        };
        let v: Value = serde_json::from_str(&set_activity_json(9, Some(&a), 3)).unwrap();
        assert_eq!(v["cmd"], "SET_ACTIVITY");
        assert_eq!(v["args"]["pid"], 9);
        assert_eq!(v["args"]["activity"]["party"]["size"], json!([1, 2]));
        assert_eq!(v["args"]["activity"]["timestamps"]["start"], 5);
        let c: Value = serde_json::from_str(&set_activity_json(9, None, 4)).unwrap();
        assert!(c["args"].get("activity").is_none());
    }

    #[test]
    fn off_without_client_id() {
        assert!(start("", true, PathBuf::from("x"), 1).is_none());
        assert!(start("123", false, PathBuf::from("x"), 1).is_none());
    }
}
