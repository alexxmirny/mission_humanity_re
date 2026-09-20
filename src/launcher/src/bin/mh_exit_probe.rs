//! `mh_exit_probe <code>` -- a process that exits with the code you name, so the launcher's exit
//! classification can be proven against a real `wait()` rather than against a unit test.
//!
//! WHY A SEPARATE BINARY. `launch::classify` has unit tests, and they are not enough: they prove
//! the ARITHMETIC, not that a `STATUS_*` code survives the trip through `CreateProcess`,
//! `GetExitCodeProcess`, Rust's `i32`-typed `ExitStatus::code()` and back to a `u32`. That trip is
//! where the sign-reinterpretation bug would live, and only a process that really exited with
//! `0xC0000005` can show it did not.
//!
//! HOW IT IS USED. Copy it into an otherwise empty directory as `mh.exe` -- the launcher's only
//! test for a game directory is that name -- point `--game-dir` at it and press Play. The launcher
//! then waits on a genuine `STATUS_ACCESS_VIOLATION` exit with no game and no crash of ours.
//!
//! WHICH IS WHY THE CODE CAN COME FROM A FILE. The launcher runs `mh.exe` with NO ARGUMENTS, because
//! that is how the game is started; a fixture that could only be told its code on the command line
//! would be untestable through the very path it exists to test. So: `argv[1]` when there is one, and
//! otherwise `mh_exit_probe_code.txt` in the working directory -- which the launcher sets to the
//! game directory, so the fixture's answer sits in the fixture's own folder.
//!
//! It ships in no release zip: `tools/release_package.py` names the five modules a zip may hold and
//! this is not one of them.

/// Where the probe looks when nothing was passed on the command line.
const CODE_FILE: &str = "mh_exit_probe_code.txt";

fn main() {
    let from_file = || std::fs::read_to_string(CODE_FILE).ok();
    let text = std::env::args()
        .nth(1)
        .or_else(from_file)
        .unwrap_or_else(|| "0".to_string());
    let code = parse(&text).unwrap_or_else(|| {
        eprintln!(
            "usage: mh_exit_probe <exit code: 0, 3, 0xC0000005, -1073741819>\n\
             (or put one of those in {CODE_FILE} beside the working directory)"
        );
        std::process::exit(2);
    });
    eprintln!("mh_exit_probe: exiting with 0x{code:08X}");

    // `std::process::exit` takes an i32 and Windows hands the low 32 bits to the parent as a DWORD,
    // so the reinterpretation is exact in both directions: 0xC0000005 goes out as -1073741819 and
    // the launcher reads -1073741819 back as 0xC0000005. That round trip IS the thing under test.
    std::process::exit(code as i32);
}

/// Accept the code as hex (`0xC0000005`), as an unsigned decimal, or as the signed decimal Windows
/// tooling tends to print (`-1073741819`) -- all three name the same value and a fixture that
/// insisted on one spelling would be a trap of its own.
fn parse(text: &str) -> Option<u32> {
    let t = text.trim();
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        return u32::from_str_radix(hex, 16).ok();
    }
    if let Ok(v) = t.parse::<u32>() {
        return Some(v);
    }
    t.parse::<i32>().ok().map(|v| v as u32)
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn the_three_spellings_agree() {
        assert_eq!(parse("0xC0000005"), Some(0xC000_0005));
        assert_eq!(parse("-1073741819"), Some(0xC000_0005));
        assert_eq!(parse("3221225477"), Some(0xC000_0005));
        assert_eq!(parse("0"), Some(0));
        assert_eq!(parse("not a code"), None);
    }
}
