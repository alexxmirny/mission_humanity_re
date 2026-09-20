//! The four machine facts `report.json` carries: OS, CPU, GPU, RAM (dist LA4, plan decision D13).
//!
//! WHY THIS IS FIVE REGISTRY READS AND ONE SYSCALL RATHER THAN A CRATE. `sysinfo` and its relatives
//! enumerate processes, disks, networks and temperatures; a bug report needs four strings. The
//! difference is not binary size, it is what a launcher that players download is allowed to read
//! about their machine: this module can be audited in one screen and it reads nothing it does not
//! print into a file the player is shown before it is sent.
//!
//! EVERY FIELD DEGRADES TO A STRING SAYING SO, and that is the contract the rest of the program
//! relies on. A machine with no GPU driver key, a locked-down registry, a Wine or ReactOS host --
//! none of them is an error worth failing a crash report over, and a report that omitted a field
//! silently would be indistinguishable from one whose field was genuinely empty. So the value is
//! always present and always a string; "unknown" is an answer.

use std::ffi::c_void;

/// One machine, as `report.json` states it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Machine {
    /// `Windows 11 Pro 24H2 (build 28000)`, or as much of that as the registry admits to.
    pub os: String,
    /// The processor's own marketing string, plus the logical-processor count.
    pub cpu: String,
    /// Every display adapter's `DriverDesc`, joined -- a laptop has two and the crash may be in
    /// either.
    pub gpu: String,
    /// Physical RAM in whole mebibytes. 0 when `GlobalMemoryStatusEx` would not answer.
    pub ram_mb: u64,
}

impl Machine {
    /// Ask the OS. Never fails; unanswered fields read "unknown" (or 0 for `ram_mb`).
    pub fn detect() -> Self {
        Self {
            os: os_string(),
            cpu: cpu_string(),
            gpu: gpu_string(),
            ram_mb: ram_mb(),
        }
    }
}

const CURRENT_VERSION: &str = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
const CPU0: &str = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0";
/// The display-adapter device class. Every installed GPU driver writes a `DriverDesc` under a
/// four-digit subkey of it; enumerating the first few covers the dual-GPU laptop case without
/// dragging in SetupAPI.
const DISPLAY_CLASS: &str =
    r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";

/// `ProductName` + `DisplayVersion` + `CurrentBuildNumber`.
///
/// `ProductName` STILL SAYS "Windows 10" ON WINDOWS 11 -- Microsoft never updated the value, and a
/// report that trusted it would mis-file every Windows 11 crash. The build number is the fact that
/// actually separates them (>= 22000 is 11), so the string is corrected here and the raw build is
/// printed beside it so the correction is auditable rather than invisible.
fn os_string() -> String {
    let product = reg_str(CURRENT_VERSION, "ProductName").unwrap_or_default();
    let display = reg_str(CURRENT_VERSION, "DisplayVersion").unwrap_or_default();
    let build = reg_str(CURRENT_VERSION, "CurrentBuildNumber").unwrap_or_default();
    let build_n: u32 = build.parse().unwrap_or(0);
    let product = if build_n >= 22000 {
        product.replace("Windows 10", "Windows 11")
    } else {
        product
    };
    let mut out = String::new();
    for part in [product.as_str(), display.as_str()] {
        if !part.is_empty() {
            if !out.is_empty() {
                out.push(' ');
            }
            out.push_str(part);
        }
    }
    if !build.is_empty() {
        out.push_str(&format!(" (build {build})"));
    }
    if out.is_empty() {
        "unknown".to_string()
    } else {
        out
    }
}

fn cpu_string() -> String {
    let name = reg_str(CPU0, "ProcessorNameString").unwrap_or_default();
    let threads = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(0);
    match (name.trim(), threads) {
        ("", 0) => "unknown".to_string(),
        ("", n) => format!("unknown ({n} logical)"),
        (n, 0) => n.to_string(),
        (n, t) => format!("{n} ({t} logical)"),
    }
}

/// The display adapters, by their driver's own description.
///
/// Probing `0000`..`0003` rather than enumerating the class key is deliberate: the subkeys are
/// consecutive four-digit strings by construction, a machine with four display adapters does not
/// exist outside a server, and `RegEnumKeyEx` would be a second API and a second failure mode for
/// a field whose whole job is to say "this crash was on an Intel iGPU".
fn gpu_string() -> String {
    let mut found: Vec<String> = Vec::new();
    for i in 0..4 {
        let sub = format!("{DISPLAY_CLASS}\\{i:04}");
        if let Some(desc) = reg_str(&sub, "DriverDesc") {
            let desc = desc.trim().to_string();
            if !desc.is_empty() && !found.contains(&desc) {
                found.push(desc);
            }
        }
    }
    if found.is_empty() {
        "unknown".to_string()
    } else {
        found.join(" + ")
    }
}

fn ram_mb() -> u64 {
    use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
    let mut st: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    st.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    // SAFETY: `st` is a correctly sized, zeroed MEMORYSTATUSEX with dwLength set, which is the
    // only precondition the call has.
    if unsafe { GlobalMemoryStatusEx(&mut st) } == 0 {
        return 0;
    }
    st.ullTotalPhys / (1024 * 1024)
}

/// One `HKEY_LOCAL_MACHINE` string value, or `None`.
///
/// `RegGetValueW` rather than open/query/close: it is one call, it follows `REG_EXPAND_SZ` for us,
/// and `RRF_RT_REG_SZ` makes a value of the wrong type a failure instead of a bag of bytes
/// reinterpreted as text. The two-pass size query is the documented shape -- a first call with a
/// null buffer fills `cb`, the second fills the buffer.
fn reg_str(subkey: &str, value: &str) -> Option<String> {
    use windows_sys::Win32::Foundation::ERROR_SUCCESS;
    use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ};

    let sub = wide(subkey);
    let val = wide(value);
    let mut cb: u32 = 0;
    // SAFETY: both strings are NUL-terminated UTF-16; a null data pointer with a valid size-out is
    // the documented way to ask for the size.
    let rc = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            sub.as_ptr(),
            val.as_ptr(),
            RRF_RT_REG_SZ,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut cb,
        )
    };
    if rc != ERROR_SUCCESS || cb == 0 {
        return None;
    }
    // `cb` is BYTES including the terminator; a u16 buffer needs half of it, rounded up.
    let mut buf: Vec<u16> = vec![0; (cb as usize).div_ceil(2)];
    let mut cb2 = cb;
    // SAFETY: `buf` is at least `cb2` bytes long, which is what the call was told.
    let rc = unsafe {
        RegGetValueW(
            HKEY_LOCAL_MACHINE,
            sub.as_ptr(),
            val.as_ptr(),
            RRF_RT_REG_SZ,
            std::ptr::null_mut(),
            buf.as_mut_ptr() as *mut c_void,
            &mut cb2,
        )
    };
    if rc != ERROR_SUCCESS {
        return None;
    }
    let n = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    Some(String::from_utf16_lossy(&buf[..n]))
}

/// A NUL-terminated UTF-16 copy of `s`, for the W-suffixed calls above.
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every field answers SOMETHING. The point is not the values -- they differ per machine -- but
    /// that no path returns an empty string, because an empty field in a bug report reads as "the
    /// launcher did not look" and is indistinguishable from "the machine did not say".
    #[test]
    fn every_field_is_answered() {
        let m = Machine::detect();
        assert!(!m.os.is_empty(), "os");
        assert!(!m.cpu.is_empty(), "cpu");
        assert!(!m.gpu.is_empty(), "gpu");
    }

    /// The one fact the registry gets wrong on purpose. Not a detect() test (this machine's build
    /// is whatever it is) but a test of the correction itself.
    #[test]
    fn windows_eleven_is_not_reported_as_ten() {
        let os = os_string();
        if let Some(build) = os
            .rsplit_once("(build ")
            .and_then(|(_, b)| b.trim_end_matches(')').parse::<u32>().ok())
        {
            if build >= 22000 {
                assert!(!os.contains("Windows 10"), "{os}");
            }
        }
    }

    #[test]
    fn a_missing_value_is_none_not_a_panic() {
        assert!(reg_str(r"SOFTWARE\no such key at all, really", "Nope").is_none());
        assert!(reg_str(CURRENT_VERSION, "NoSuchValueName").is_none());
    }
}
