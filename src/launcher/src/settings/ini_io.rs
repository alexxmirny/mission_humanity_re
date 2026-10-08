//! Byte-preserving `mh_net.ini` reads and edits (dist RL13).
//!
//! The settings page never re-serialises the ini. It re-reads the file's BYTES at Apply time and
//! splices the changed values into them, so a player's comments, hand-aligned `; notes`, unknown
//! sections, line endings and encoding survive an Apply untouched. `relay.rs` does the same for its
//! two keys with a purpose-built editor; this is the general form (relay is re-based on it later).
//!
//! ## Rules (each is a unit test)
//!
//! * The FIRST `[section]` header wins (case-insensitive, whitespace inside the brackets trimmed) and
//!   the first `key=` line inside it wins (case-insensitive, key trimmed). This is what
//!   `GetPrivateProfileString` does, so a later duplicate is dead text and is never rewritten.
//! * Only the value bytes change. A value is the text after `=` up to the first `;` (the same cut
//!   `mh::config::strip_ini_comment` makes in the DLL), trimmed. Whitespace around `=` and the
//!   trailing `; comment` stay exactly as they were.
//! * A missing key is inserted right after the section's last key line (or after the header when the
//!   section has none). A missing section is appended at the end of the file.
//! * Line endings: lines this module adds use the file's first line ending (CRLF for a new file); a
//!   file without a final newline keeps not having one.
//! * Only keys whose value actually differs are touched, so an Apply of an unchanged page is a
//!   byte-for-byte no-op and the file's mtime does not move.

use std::path::{Path, PathBuf};

/// One requested change: set `[section] key = value`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Edit {
    pub section: String,
    pub key: String,
    pub value: String,
}

impl Edit {
    pub fn new(section: &str, key: &str, value: &str) -> Self {
        Self {
            section: section.to_string(),
            key: key.to_string(),
            value: value.to_string(),
        }
    }
}

/// What `apply_file` did to the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Change {
    /// Nothing differed; the file was not written (and not created).
    Unchanged,
    /// Edited in place.
    Written,
    /// Did not exist and was created.
    Created,
}

const BOM: &[u8] = b"\xEF\xBB\xBF";

fn is_ws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n')
}

fn trim(b: &[u8]) -> &[u8] {
    let start = b.iter().position(|c| !is_ws(*c)).unwrap_or(b.len());
    let end = b.iter().rposition(|c| !is_ws(*c)).map_or(start, |i| i + 1);
    &b[start..end]
}

/// `"key=value\r\n"` -> (`"key=value"`, `"\r\n"`).
fn split_terminator(raw: &[u8]) -> (&[u8], &[u8]) {
    if raw.ends_with(b"\r\n") {
        raw.split_at(raw.len() - 2)
    } else if raw.ends_with(b"\n") {
        raw.split_at(raw.len() - 1)
    } else {
        (raw, &[])
    }
}

/// The section name when `line` (no terminator) is a header.
fn header_name(line: &[u8], first_line: bool) -> Option<&[u8]> {
    let line = if first_line {
        line.strip_prefix(BOM).unwrap_or(line)
    } else {
        line
    };
    let t = trim(line);
    if t.len() >= 2 && t[0] == b'[' && t[t.len() - 1] == b']' {
        Some(trim(&t[1..t.len() - 1]))
    } else {
        None
    }
}

/// A `key = value` line: (byte range of the key, offset of the value start, value end), all
/// relative to `line`. `None` for blank lines, comments and lines without `=`.
struct KeyLine {
    key: (usize, usize),
    value: (usize, usize),
}

fn key_line(line: &[u8]) -> Option<KeyLine> {
    let lead = line.iter().position(|c| !is_ws(*c))?;
    if line[lead] == b';' || line[lead] == b'[' {
        return None;
    }
    let eq = line.iter().position(|c| *c == b'=')?;
    if eq < lead {
        return None;
    }
    let mut key_end = eq;
    while key_end > lead && is_ws(line[key_end - 1]) {
        key_end -= 1;
    }
    let mut vstart = eq + 1;
    while vstart < line.len() && is_ws(line[vstart]) {
        vstart += 1;
    }
    // The DLL cuts a value at the first `;`; everything from there is the player's comment.
    let cut = line[vstart..]
        .iter()
        .position(|c| *c == b';')
        .map_or(line.len(), |i| vstart + i);
    let mut vend = cut;
    while vend > vstart && is_ws(line[vend - 1]) {
        vend -= 1;
    }
    Some(KeyLine {
        key: (lead, key_end),
        value: (vstart, vend),
    })
}

/// Lines of `bytes` with their byte ranges.
fn lines(bytes: &[u8]) -> Vec<&[u8]> {
    bytes.split_inclusive(|b| *b == b'\n').collect()
}

/// `[start, end)` line indices of the FIRST section called `name`, header included.
fn find_section(all: &[&[u8]], name: &str) -> Option<(usize, usize)> {
    let mut start = None;
    for (i, raw) in all.iter().enumerate() {
        let (line, _) = split_terminator(raw);
        if let Some(h) = header_name(line, i == 0) {
            if let Some(s) = start {
                return Some((s, i));
            }
            if h.eq_ignore_ascii_case(name.as_bytes()) {
                start = Some(i);
            }
        }
    }
    start.map(|s| (s, all.len()))
}

/// The value of `[section] key` in `bytes`: first section, first key, up to the first `;`.
/// `None` when the section or the key is absent. Non-UTF-8 bytes are replaced (a display value).
pub fn read_value(bytes: &[u8], section: &str, key: &str) -> Option<String> {
    let all = lines(bytes);
    let (s, e) = find_section(&all, section)?;
    for raw in &all[s + 1..e] {
        let (line, _) = split_terminator(raw);
        if let Some(kl) = key_line(line) {
            if line[kl.key.0..kl.key.1].eq_ignore_ascii_case(key.as_bytes()) {
                return Some(String::from_utf8_lossy(&line[kl.value.0..kl.value.1]).into_owned());
            }
        }
    }
    None
}

/// The line ending lines we add should use: the file's first line's.
fn eol_of(bytes: &[u8]) -> &'static [u8] {
    match bytes.iter().position(|b| *b == b'\n') {
        Some(i) if i > 0 && bytes[i - 1] == b'\r' => b"\r\n",
        Some(_) => b"\n",
        None => b"\r\n",
    }
}

/// Apply one edit to the bytes. Returns the new bytes.
fn apply_one(bytes: &[u8], edit: &Edit) -> Vec<u8> {
    if bytes.is_empty() {
        let mut out = Vec::new();
        out.extend_from_slice(
            format!("[{}]\r\n{}={}\r\n", edit.section, edit.key, edit.value).as_bytes(),
        );
        return out;
    }
    let eol = eol_of(bytes);
    let all = lines(bytes);
    let had_final_nl = bytes.ends_with(b"\n");

    let Some((s, e)) = find_section(&all, &edit.section) else {
        // Section missing: append. A blank separator line, and the new lines end like the file did.
        let mut out = bytes.to_vec();
        if !had_final_nl {
            out.extend_from_slice(eol);
        }
        out.extend_from_slice(eol);
        out.extend_from_slice(format!("[{}]", edit.section).as_bytes());
        out.extend_from_slice(eol);
        out.extend_from_slice(format!("{}={}", edit.key, edit.value).as_bytes());
        if had_final_nl {
            out.extend_from_slice(eol);
        }
        return out;
    };

    // The key, if present: replace the value bytes only.
    let mut last_key_line = None;
    for (i, raw) in all.iter().enumerate().take(e).skip(s + 1) {
        let (line, term) = split_terminator(raw);
        let Some(kl) = key_line(line) else { continue };
        last_key_line = Some(i);
        if line[kl.key.0..kl.key.1].eq_ignore_ascii_case(edit.key.as_bytes()) {
            let mut out = Vec::with_capacity(bytes.len() + edit.value.len());
            for l in &all[..i] {
                out.extend_from_slice(l);
            }
            out.extend_from_slice(&line[..kl.value.0]);
            out.extend_from_slice(edit.value.as_bytes());
            // `key= ; comment` (empty old value) -> keep the comment off the new value.
            if kl.value.0 == kl.value.1
                && !edit.value.is_empty()
                && line.get(kl.value.1) == Some(&b';')
            {
                out.push(b' ');
            }
            out.extend_from_slice(&line[kl.value.1..]);
            out.extend_from_slice(term);
            for l in &all[i + 1..] {
                out.extend_from_slice(l);
            }
            return out;
        }
    }

    // The key is missing: after the section's last key line, else straight after the header.
    let after = last_key_line.unwrap_or(s);
    let mut out = Vec::with_capacity(bytes.len() + edit.key.len() + edit.value.len() + 4);
    for l in &all[..=after] {
        out.extend_from_slice(l);
    }
    let new_line = format!("{}={}", edit.key, edit.value);
    if after == all.len() - 1 && !had_final_nl {
        // The last line had no terminator: give it one, and let the new last line have none, so
        // the file keeps its missing final newline.
        out.extend_from_slice(eol);
        out.extend_from_slice(new_line.as_bytes());
    } else {
        out.extend_from_slice(new_line.as_bytes());
        out.extend_from_slice(eol);
        for l in &all[after + 1..] {
            out.extend_from_slice(l);
        }
    }
    out
}

/// Apply `edits` to `existing` (or to nothing). Only edits whose value differs from what the file
/// already says are made; the rest are skipped, so equal input gives byte-identical output.
pub fn apply_edits(existing: Option<&[u8]>, edits: &[Edit]) -> Vec<u8> {
    let mut cur: Vec<u8> = existing.map(<[u8]>::to_vec).unwrap_or_default();
    for e in edits {
        if read_value(&cur, &e.section, &e.key).as_deref() == Some(e.value.as_str()) {
            continue;
        }
        cur = apply_one(&cur, e);
    }
    cur
}

/// Read the whole file; `Ok(None)` when it does not exist.
pub fn read_file(path: &Path) -> Result<Option<Vec<u8>>, String> {
    match std::fs::read(path) {
        Ok(b) => Ok(Some(b)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("cannot read {}: {e}", path.display())),
    }
}

/// A rename over the target, so a launcher killed mid-write leaves the old file whole.
pub fn write_atomically(path: &Path, bytes: &[u8]) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)
            .map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    }
    let tmp: PathBuf = path.with_extension("mhtmp");
    std::fs::write(&tmp, bytes).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("cannot replace {}: {e}", path.display())
    })
}

/// Re-read `path`, apply `edits`, write back only when the bytes changed.
pub fn apply_file(path: &Path, edits: &[Edit]) -> Result<Change, String> {
    let existing = read_file(path)?;
    let edited = apply_edits(existing.as_deref(), edits);
    match existing {
        Some(old) if old == edited => Ok(Change::Unchanged),
        None if edited.is_empty() => Ok(Change::Unchanged),
        Some(_) => {
            write_atomically(path, &edited)?;
            Ok(Change::Written)
        }
        None => {
            write_atomically(path, &edited)?;
            Ok(Change::Created)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(b: &[u8]) -> String {
        String::from_utf8_lossy(b).into_owned()
    }

    const SAMPLE: &str = "; header\r\n\
        [video]\r\n\
        window = windowed   ; borderless | windowed\r\n\
        vsync=0\r\n\
        ; a comment between\r\n\
        \r\n\
        [Net]\r\n\
        port=6501                 ; the port\r\n\
        weird_key = \u{0416}\r\n\
        \r\n\
        [other]\r\n\
        x=1\r\n";

    #[test]
    fn an_unchanged_apply_is_a_byte_no_op() {
        let e = [
            Edit::new("video", "window", "windowed"),
            Edit::new("net", "port", "6501"),
        ];
        assert_eq!(apply_edits(Some(SAMPLE.as_bytes()), &e), SAMPLE.as_bytes());
        assert_eq!(apply_edits(Some(SAMPLE.as_bytes()), &[]), SAMPLE.as_bytes());
    }

    #[test]
    fn only_the_value_bytes_change() {
        let out = apply_edits(
            Some(SAMPLE.as_bytes()),
            &[Edit::new("video", "window", "borderless")],
        );
        let expect = SAMPLE.replace(
            "window = windowed   ; borderless",
            "window = borderless   ; borderless",
        );
        assert_eq!(s(&out), expect);
        // Section and key names are case-insensitive.
        let out = apply_edits(Some(SAMPLE.as_bytes()), &[Edit::new("NET", "PORT", "7000")]);
        assert_eq!(
            s(&out),
            SAMPLE.replace("port=6501                 ;", "port=7000                 ;")
        );
    }

    #[test]
    fn crlf_lf_and_a_missing_final_newline_survive() {
        let lf = "[a]\nx=1\ny=2";
        let out = apply_edits(Some(lf.as_bytes()), &[Edit::new("a", "x", "9")]);
        assert_eq!(s(&out), "[a]\nx=9\ny=2");
        // New key after the last key line of the LAST section of a file with no final newline.
        let out = apply_edits(Some(lf.as_bytes()), &[Edit::new("a", "z", "3")]);
        assert_eq!(s(&out), "[a]\nx=1\ny=2\nz=3");
        // New section on a file with no final newline: still none at the end.
        let out = apply_edits(Some(lf.as_bytes()), &[Edit::new("b", "k", "v")]);
        assert_eq!(s(&out), "[a]\nx=1\ny=2\n\n[b]\nk=v");
        // CRLF file: added lines are CRLF.
        let crlf = "[a]\r\nx=1\r\n";
        let out = apply_edits(Some(crlf.as_bytes()), &[Edit::new("b", "k", "v")]);
        assert_eq!(s(&out), "[a]\r\nx=1\r\n\r\n[b]\r\nk=v\r\n");
    }

    #[test]
    fn the_first_duplicate_section_and_key_win() {
        let dup = "[net]\nport=1\nport=2\n[net]\nport=3\n";
        assert_eq!(
            read_value(dup.as_bytes(), "net", "port").as_deref(),
            Some("1")
        );
        let out = apply_edits(Some(dup.as_bytes()), &[Edit::new("net", "port", "9")]);
        assert_eq!(s(&out), "[net]\nport=9\nport=2\n[net]\nport=3\n");
    }

    #[test]
    fn a_missing_key_goes_after_the_sections_last_key_line() {
        let out = apply_edits(
            Some(SAMPLE.as_bytes()),
            &[Edit::new("video", "scale", "fit")],
        );
        let expect = SAMPLE.replace("vsync=0\r\n", "vsync=0\r\nscale=fit\r\n");
        assert_eq!(s(&out), expect);
        // A section with no key lines at all: straight after the header.
        let only_comments = "[a]\n; nothing\n\n[b]\nq=1\n";
        let out = apply_edits(Some(only_comments.as_bytes()), &[Edit::new("a", "k", "v")]);
        assert_eq!(s(&out), "[a]\nk=v\n; nothing\n\n[b]\nq=1\n");
    }

    #[test]
    fn a_missing_section_is_appended_and_a_missing_file_is_created() {
        let out = apply_edits(Some(SAMPLE.as_bytes()), &[Edit::new("hud", "k", "1")]);
        assert_eq!(s(&out), format!("{SAMPLE}\r\n[hud]\r\nk=1\r\n"));
        let fresh = apply_edits(
            None,
            &[Edit::new("hud", "k", "1"), Edit::new("hud", "j", "2")],
        );
        assert_eq!(s(&fresh), "[hud]\r\nk=1\r\nj=2\r\n");
    }

    #[test]
    fn a_value_ends_at_the_first_semicolon_like_the_dll() {
        assert_eq!(
            read_value(SAMPLE.as_bytes(), "video", "window").as_deref(),
            Some("windowed")
        );
        assert_eq!(read_value(SAMPLE.as_bytes(), "video", "nope"), None);
        assert_eq!(read_value(SAMPLE.as_bytes(), "nope", "window"), None);
        // Empty value followed by a comment: the new value does not swallow into the comment.
        let out = apply_edits(Some(b"[a]\nk= ;note\n"), &[Edit::new("a", "k", "v")]);
        assert_eq!(s(&out), "[a]\nk= v ;note\n");
    }

    #[test]
    fn non_utf8_and_bom_bytes_on_other_lines_are_untouched() {
        let mut raw =
            b"\xEF\xBB\xBF[video]\r\n; caf\xE9 latin-1 comment\r\nwindow=windowed\r\n".to_vec();
        let out = apply_edits(Some(&raw), &[Edit::new("video", "window", "borderless")]);
        raw = raw.replace_window(b"windowed", b"borderless");
        assert_eq!(out, raw);
    }

    trait ReplaceWindow {
        fn replace_window(self, from: &[u8], to: &[u8]) -> Vec<u8>;
    }
    impl ReplaceWindow for Vec<u8> {
        fn replace_window(self, from: &[u8], to: &[u8]) -> Vec<u8> {
            let pos = self.windows(from.len()).position(|w| w == from).unwrap();
            let mut v = self[..pos].to_vec();
            v.extend_from_slice(to);
            v.extend_from_slice(&self[pos + from.len()..]);
            v
        }
    }

    #[test]
    fn apply_file_writes_only_on_change_and_creates_parents() {
        let dir = std::env::temp_dir().join("mh_launcher_test_ini_io_file");
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join("deep").join("mh_net.ini");
        // Nothing to change in a missing file: nothing is created.
        assert_eq!(apply_file(&path, &[]).unwrap(), Change::Unchanged);
        assert!(!path.exists());
        let e = [Edit::new("video", "window", "borderless")];
        assert_eq!(apply_file(&path, &e).unwrap(), Change::Created);
        assert_eq!(
            s(&std::fs::read(&path).unwrap()),
            "[video]\r\nwindow=borderless\r\n"
        );
        assert_eq!(apply_file(&path, &e).unwrap(), Change::Unchanged);
        let e2 = [Edit::new("video", "window", "windowed")];
        assert_eq!(apply_file(&path, &e2).unwrap(), Change::Written);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
