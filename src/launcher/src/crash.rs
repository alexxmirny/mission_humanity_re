//! The launcher's half of the crash capture (dist LA4, plan decision D12).
//!
//! THE SHAPE, IN ONE PARAGRAPH. Before it starts the game the launcher mints a channel token,
//! creates two named events from it, and puts the token and a marker path in the child's
//! environment. `mh.dll`'s vectored handler picks both up at load time
//! (`src/mh_dll/mh_common/include/mh_crash_marker.h` is the shared contract). When the game takes a
//! fatal fault it writes the marker, signals the request event and BLOCKS. The launcher -- polling
//! that event once per frame alongside the child -- reads the marker, calls `MiniDumpWriteDump` on
//! the still-suspended-at-the-fault process from OUTSIDE it, and signals the acknowledgement, at
//! which point the game finishes dying.
//!
//! WHY THE GAME HAS TO WAIT, which is the part that looks like a rude design and is not. The
//! marker's `pointers=` is an address in the GAME's address space, and the thread context the dump
//! needs is the crashing thread's registers as they are right now. Both stop existing the moment
//! that thread returns. Crashpad solves this the same way, and it is why "write the dump from
//! another process" and "block the crashing thread" are one decision rather than two.
//!
//! A 64-BIT LAUNCHER DUMPING A 32-BIT GAME. This is the configuration plan decision D10 chose and
//! D12 relies on: `MiniDumpWriteDump` is documented to be called from a separate process, and a
//! 64-bit handler capturing a WOW64 target is Crashpad's own arrangement. One honest caveat is
//! recorded here rather than assumed away: dbghelp's handling of a 32-bit `EXCEPTION_POINTERS`
//! read across the WOW64 boundary is not something this project has been able to verify against a
//! debugger (no `cdb` on this machine), so `write_dump` RETRIES WITHOUT the exception record if the
//! first call fails. A dump with no exception stream still carries every thread, every module and
//! the stacks; and the marker already names the faulting module and offset independently of the
//! dump, which is what `tools/crash_report.py` resolves to a function name.

use std::ffi::c_void;
use std::path::{Path, PathBuf};

use crate::log;
use crate::machine::wide;

/// The environment variables the DLL reads. Spelled once here and once in `mh_crash_marker.h`;
/// `docs/launcher.md` is where the pair is documented as a contract.
pub const ENV_CHANNEL: &str = "MH_CRASH_CHANNEL";
pub const ENV_MARKER: &str = "MH_CRASH_MARKER";

/// The minidump flags plan decision D12 names: data segments (so globals are readable),
/// recently-unloaded modules (so a fault in one is still attributable) and the per-thread OS
/// information.
///
/// **`WithFullMemory` IS DELIBERATELY ABSENT AND MUST STAY ABSENT.** It is the flag that turns a
/// 2 MB dump into the game's entire address space -- hundreds of megabytes a player has to upload
/// and that contains whatever was in memory at the time. The whole report is capped at 64 MB at the
/// collector's edge (plan D14), and a full-memory dump of this game would not fit inside it even
/// once.
const FLAGS: u32 = 1 /* WithDataSegs */ | (1 << 5) /* WithUnloadedModules */ | (1 << 8) /* WithProcessThreadData */;

/// What the DLL wrote when it faulted.
///
/// Every field is optional in the parse and defaulted here, because a marker is written by a
/// process that is already broken: a truncated one must still yield what it managed to write
/// rather than failing whole.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Marker {
    pub version: u32,
    pub code: u32,
    pub pid: u32,
    pub tid: u32,
    pub address: u64,
    /// The `EXCEPTION_POINTERS` address **in the game's address space**. Meaningless once the game
    /// has exited, which is why `write_dump` is only ever called while it is still blocked.
    pub pointers: u64,
    pub module: String,
    pub module_base: u64,
    /// `address - module_base`: an RVA for `mh.dll`, image-relative for the exe. This is the value
    /// that goes into `report.json`'s `crash.offset` -- see `report.rs`.
    pub offset: u64,
    pub match_id: String,
    pub build: String,
    pub when: String,
    /// dist LA5. Set when mh.dll's handler also wrote a `<marker path>.ctx32` sidecar -- the raw
    /// x86 `EXCEPTION_RECORD` + `CONTEXT` bytes, captured natively inside the faulting process. See
    /// `ctx32_path()` and `append_exception_stream()` below.
    pub has_context: bool,
}

/// dist LA5. `<marker path>` + [`CTX32_SUFFIX`] -- the sidecar has no name of its own, only a
/// fixed relationship to the marker's. Spelled once here and once in
/// `src/mh_dll/mh_common/include/mh_crash_marker.h`'s `MH_CRASH_CTX_SUFFIX` (a Rust translation
/// unit cannot include that C++ header), the same duplication `ENV_CHANNEL`/`ENV_MARKER` already
/// accept.
pub const CTX32_SUFFIX: &str = ".ctx32";

impl Marker {
    /// Parse the marker text. Unknown keys are ignored (that is what `key=value` buys); a missing
    /// or unreadable `mh_crash=` line is the one hard error, because a file that does not say it
    /// is a marker may be anything at all.
    pub fn parse(text: &str) -> Result<Marker, String> {
        let mut m = Marker::default();
        let mut saw_version = false;
        for line in text.lines() {
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            let v = v.trim();
            match k.trim() {
                "mh_crash" => {
                    m.version = v.parse().unwrap_or(0);
                    saw_version = true;
                }
                "code" => m.code = hex32(v),
                "pid" => m.pid = v.parse().unwrap_or(0),
                "tid" => m.tid = v.parse().unwrap_or(0),
                "address" => m.address = hex64(v),
                "pointers" => m.pointers = hex64(v),
                "module" => m.module = v.to_string(),
                "module_base" => m.module_base = hex64(v),
                "offset" => m.offset = hex64(v),
                "match_id" => m.match_id = v.to_string(),
                "build" => m.build = v.to_string(),
                "when" => m.when = v.to_string(),
                "ctx" => m.has_context = v.trim() == "1",
                _ => {}
            }
        }
        if !saw_version {
            return Err("not a crash marker (no `mh_crash=` line)".to_string());
        }
        if m.version == 0 || m.version > 1 {
            return Err(format!(
                "crash marker version {} is not one this launcher understands",
                m.version
            ));
        }
        Ok(m)
    }

    pub fn read(path: &Path) -> Result<Marker, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        Marker::parse(&text)
    }

    /// The `.ctx32` sidecar's path, given the marker's own -- `has_context` says whether to expect
    /// one to exist. dist LA5.
    pub fn ctx32_path(marker_path: &Path) -> PathBuf {
        let mut p = marker_path.as_os_str().to_owned();
        p.push(CTX32_SUFFIX);
        PathBuf::from(p)
    }

    /// `mh.dll+0x000175b0` -- how the Launch view names the fault before any symbol resolution.
    pub fn where_text(&self) -> String {
        if self.module.is_empty() {
            format!("0x{:08x} (module unknown)", self.address)
        } else {
            format!("{}+0x{:x}", self.module, self.offset)
        }
    }
}

fn hex32(v: &str) -> u32 {
    hex64(v) as u32
}

fn hex64(v: &str) -> u64 {
    let t = v.trim();
    let t = t
        .strip_prefix("0x")
        .or_else(|| t.strip_prefix("0X"))
        .unwrap_or(t);
    u64::from_str_radix(t, 16).unwrap_or(0)
}

// ---- the channel -------------------------------------------------------------------------------

/// The two named events, plus the token and marker path the child is told about.
///
/// Owned by the launcher for the lifetime of one game run. Dropping it closes the handles, which is
/// safe at any time: a game that signals a request nobody is waiting on simply times out after
/// `MH_CRASH_ACK_TIMEOUT_MS` and dies the way it would have anyway.
pub struct Channel {
    token: String,
    marker: PathBuf,
    req: isize,
    ack: isize,
}

impl Channel {
    /// Mint a channel for a game about to be started, with its marker under `dir`.
    ///
    /// The token only has to be unique among the launchers running on one machine at one moment,
    /// which our own pid and a nanosecond clock settle -- this is a rendezvous name, not a secret
    /// (see `mh_crash_marker.h`: the worst a local process can do by guessing one is release a
    /// crashed game early, which it could do by killing the game instead).
    pub fn create(dir: &Path) -> Option<Channel> {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos() as u64 ^ d.as_secs())
            .unwrap_or(0);
        let token = format!("{:08x}{:08x}", std::process::id(), nanos as u32);
        let req = create_event(&format!("Local\\mh_crash_{token}_req"))?;
        let ack = create_event(&format!("Local\\mh_crash_{token}_ack"))?;
        let marker = dir.join(format!("mh_crash_{token}.marker"));
        if let Some(parent) = marker.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        // A marker left by a previous run under the SAME path would be read as this run's crash.
        // The token makes that essentially impossible; removing it anyway costs one syscall and
        // turns "essentially" into "by construction".
        let _ = std::fs::remove_file(&marker);
        log::line(format!(
            "crash: channel {token} armed, marker at {}",
            marker.display()
        ));
        Some(Channel {
            token,
            marker,
            req,
            ack,
        })
    }

    /// The two environment entries the child needs, ready for `Command::envs`.
    pub fn env(&self) -> [(&'static str, String); 2] {
        [
            (ENV_CHANNEL, self.token.clone()),
            (ENV_MARKER, self.marker.display().to_string()),
        ]
    }

    pub fn marker_path(&self) -> &Path {
        &self.marker
    }

    /// Has the game reported a crash? Non-blocking: this is called from the UI frame poll, where
    /// anything that waits is a frozen window.
    pub fn crashed(&self) -> bool {
        wait_zero(self.req)
    }

    /// Let the game finish dying.
    pub fn release(&self) {
        // SAFETY: `ack` is a handle this struct owns for its whole lifetime.
        unsafe { windows_sys::Win32::System::Threading::SetEvent(self.ack as *mut c_void) };
    }
}

impl Drop for Channel {
    fn drop(&mut self) {
        // A game still blocked on the acknowledgement when the launcher drops the channel would
        // wait out its whole timeout for nothing. Releasing first costs one syscall on a handle we
        // are about to close anyway.
        self.release();
        // SAFETY: both handles were created by this struct and are closed exactly once.
        unsafe {
            windows_sys::Win32::Foundation::CloseHandle(self.req as *mut c_void);
            windows_sys::Win32::Foundation::CloseHandle(self.ack as *mut c_void);
        }
    }
}

/// Auto-reset, initially clear, named. Created rather than opened so the launcher and the game may
/// start in either order -- whichever is second gets a handle to the existing object.
fn create_event(name: &str) -> Option<isize> {
    use windows_sys::Win32::System::Threading::CreateEventW;
    let w = wide(name);
    // SAFETY: `w` is a NUL-terminated UTF-16 name; null attributes means the default descriptor.
    let h = unsafe { CreateEventW(std::ptr::null(), 0, 0, w.as_ptr()) };
    if h.is_null() {
        log::line(format!(
            "crash: cannot create the event {name}: {}",
            std::io::Error::last_os_error()
        ));
        return None;
    }
    Some(h as isize)
}

fn wait_zero(h: isize) -> bool {
    use windows_sys::Win32::System::Threading::WaitForSingleObject;
    // SAFETY: a handle owned by the caller; a zero timeout never blocks.
    unsafe {
        WaitForSingleObject(h as *mut c_void, 0) == 0 /* WAIT_OBJECT_0 */
    }
}

// ---- the dump ------------------------------------------------------------------------------------

/// Write a minidump of the (still blocked) process the marker describes.
///
/// `marker_path` is where the marker itself was read from -- needed only to find its `.ctx32`
/// sidecar (dist LA5), never opened directly here.
///
/// Returns the size written. The two-attempt shape is the WOW64 caveat in the module header made
/// operational: the first call hands dbghelp the game's own `EXCEPTION_POINTERS`, and if that is
/// refused the second writes the same dump with no exception record rather than no dump at all.
/// Either way, if `marker.has_context` names a readable `.ctx32` sidecar, `append_exception_stream`
/// then gives the dump a REAL Exception stream built from what mh.dll captured directly -- dbghelp's
/// own attempt (the `with_exception` branch above) is not trusted for this even when it reports
/// success, because LA4 measured that a 64-bit `MiniDumpWriteDump` silently produces no Exception
/// stream for a WOW64 target regardless of which branch "worked".
pub fn write_dump(marker: &Marker, marker_path: &Path, dest: &Path) -> Result<u64, String> {
    use minidump_writer::crash_context::CrashContext;
    use minidump_writer::minidump_writer::MinidumpWriter;
    use minidump_writer::MinidumpType;

    if marker.pid == 0 {
        return Err("the marker names no process to dump".to_string());
    }
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("cannot create {}: {e}", parent.display()))?;
    }
    let flags = MinidumpType::from_bits_truncate(FLAGS);

    let mut last = String::new();
    for with_exception in [true, false] {
        let mut file = std::fs::File::create(dest)
            .map_err(|e| format!("cannot create {}: {e}", dest.display()))?;
        let cc = CrashContext {
            exception_pointers: if with_exception {
                marker.pointers as *const _
            } else {
                std::ptr::null()
            },
            exception_code: marker.code as i32,
            process_id: marker.pid,
            thread_id: marker.tid,
        };
        // SAFETY: `exception_pointers` is an address in the TARGET process, which is what
        // `ClientPointers` exists for, and the target is blocked inside its own handler for the
        // duration of this call -- that is the whole reason the handler waits.
        match MinidumpWriter::dump_crash_context(&cc, Some(flags), &mut file) {
            Ok(()) => {
                drop(file);
                // dist LA5. Regardless of which branch above "worked": append a real Exception
                // stream from mh.dll's own capture when there is one to append. See the sidecar
                // note on the function above for why this does not trust `with_exception`'s report.
                if marker.has_context {
                    let ctx_path = Marker::ctx32_path(marker_path);
                    match std::fs::read(&ctx_path) {
                        Ok(ctx32) => match append_exception_stream(dest, &ctx32, marker) {
                            Ok(()) => log::line(format!(
                                "crash: exception stream appended from {}",
                                ctx_path.display()
                            )),
                            Err(e) => log::line(format!(
                                "crash: could not append the exception stream from {}: {e}",
                                ctx_path.display()
                            )),
                        },
                        Err(e) => log::line(format!(
                            "crash: marker named a .ctx32 sidecar but {} could not be read: {e}",
                            ctx_path.display()
                        )),
                    }
                }
                let bytes = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
                log::line(format!(
                    "crash: minidump written to {} ({bytes} bytes, exception record: {})",
                    dest.display(),
                    if with_exception {
                        "yes"
                    } else {
                        "NO -- retried without it"
                    }
                ));
                return Ok(bytes);
            }
            Err(e) => {
                last = e.to_string();
                log::line(format!(
                    "crash: MiniDumpWriteDump failed{}: {last}",
                    if with_exception {
                        " with the game's exception record; retrying without it"
                    } else {
                        ""
                    }
                ));
            }
        }
    }
    let _ = std::fs::remove_file(dest);
    Err(format!("cannot write a minidump: {last}"))
}

// ---- dist LA5: giving the dump a real Exception stream -------------------------------------------
//
// A minidump is a header naming a stream DIRECTORY (an array of {type, size, offset} entries)
// somewhere in the file, plus whatever those entries point at. Nothing about that requires the
// directory to be contiguous with anything, or forbids adding a stream after `MiniDumpWriteDump` has
// already finished -- so this function does exactly that, on a file `write_dump` already wrote:
//
//   1. read the header and the existing directory
//   2. append the 32-bit CONTEXT bytes verbatim (a stream needs no format of its own; dbghelp
//      already knows how to read a raw CONTEXT at an RVA)
//   3. append a MINIDUMP_EXCEPTION_STREAM built from the marker + the CONTEXT's new RVA
//   4. append a NEW directory: the old entries, verbatim, plus one more pointing at (3)
//   5. rewrite the header's `StreamDirectoryRva`/`NumberOfStreams` to point at (4)
//
// The old directory array is left orphaned in the file; nothing reads it again once the header stops
// pointing there. This never calls MiniDumpWriteDump a third time -- the WOW64 EXCEPTION_POINTERS
// problem it cannot solve is exactly why the bytes come from mh.dll instead.
use windows_sys::Win32::System::Diagnostics::Debug::{
    ExceptionStream, EXCEPTION_RECORD32, MINIDUMP_DIRECTORY, MINIDUMP_EXCEPTION,
    MINIDUMP_EXCEPTION_STREAM, MINIDUMP_HEADER, MINIDUMP_LOCATION_DESCRIPTOR, WOW64_CONTEXT,
};

/// `'PMDM'` as Microsoft's minidumpapiset.h spells it: `(('P'<<24)|('M'<<16)|('D'<<8)|'M')`.
const MINIDUMP_SIGNATURE: u32 = 0x504d_444d;

/// Any `T: Copy` struct with no pointers and no padding -- true of every windows-sys minidump
/// struct used here (each is `repr(C)` or `repr(C, packed(4))` over primitives only). NOT a general
/// "any struct" cast: a type with padding bytes would read those bytes as uninitialised.
unsafe fn as_bytes<T: Copy>(v: &T) -> &[u8] {
    std::slice::from_raw_parts((v as *const T).cast::<u8>(), std::mem::size_of::<T>())
}

/// dist LA5. `ctx32` is the `.ctx32` sidecar's raw bytes: an `EXCEPTION_RECORD32` (the x86
/// `EXCEPTION_RECORD`'s layout, as the launcher's 64-bit compile names it) immediately followed by a
/// `WOW64_CONTEXT` (the x86 `CONTEXT`'s layout) -- see `mh/seams/crash_marker.cpp::write_context`,
/// the writer of those exact bytes.
fn append_exception_stream(dest: &Path, ctx32: &[u8], marker: &Marker) -> Result<(), String> {
    use std::io::{Read, Seek, SeekFrom, Write};

    const REC_LEN: usize = std::mem::size_of::<EXCEPTION_RECORD32>();
    const CTX_LEN: usize = std::mem::size_of::<WOW64_CONTEXT>();
    if ctx32.len() < REC_LEN + CTX_LEN {
        return Err(format!(
            "the .ctx32 sidecar is {} bytes, need at least {}",
            ctx32.len(),
            REC_LEN + CTX_LEN
        ));
    }
    // SAFETY: `read_unaligned` copies out of a byte slice already length-checked above; both types
    // are plain data (no pointers, no invariants beyond size/align), so any bit pattern is valid.
    let rec: EXCEPTION_RECORD32 = unsafe { std::ptr::read_unaligned(ctx32.as_ptr().cast()) };
    let ctx_bytes = &ctx32[REC_LEN..REC_LEN + CTX_LEN];

    let mut file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(dest)
        .map_err(|e| {
            format!(
                "cannot open {} to append the exception stream: {e}",
                dest.display()
            )
        })?;

    let mut header_bytes = [0u8; std::mem::size_of::<MINIDUMP_HEADER>()];
    file.read_exact(&mut header_bytes)
        .map_err(|e| format!("cannot read the minidump header: {e}"))?;
    // SAFETY: same POD justification as above; MINIDUMP_HEADER's one union field has no invariant
    // narrower than "some u32", so any bytes make a valid value for it.
    let header: MINIDUMP_HEADER = unsafe { std::ptr::read_unaligned(header_bytes.as_ptr().cast()) };
    if header.Signature != MINIDUMP_SIGNATURE {
        return Err(format!(
            "{} does not start with the minidump signature (got 0x{:08x})",
            dest.display(),
            header.Signature
        ));
    }

    let dir_entry_len = std::mem::size_of::<MINIDUMP_DIRECTORY>();
    let mut dir_bytes = vec![0u8; header.NumberOfStreams as usize * dir_entry_len];
    file.seek(SeekFrom::Start(header.StreamDirectoryRva as u64))
        .map_err(|e| format!("cannot seek to the stream directory: {e}"))?;
    file.read_exact(&mut dir_bytes)
        .map_err(|e| format!("cannot read the stream directory: {e}"))?;

    // Step 2: the CONTEXT bytes, appended whole.
    let ctx_rva = file
        .seek(SeekFrom::End(0))
        .map_err(|e| format!("cannot seek to the end of {}: {e}", dest.display()))?
        as u32;
    file.write_all(ctx_bytes)
        .map_err(|e| format!("cannot append the CONTEXT stream: {e}"))?;

    // Step 3: the MINIDUMP_EXCEPTION_STREAM, pointing at the CONTEXT bytes just written.
    let exc_rva =
        file.stream_position()
            .map_err(|e| format!("cannot get the current file position: {e}"))? as u32;
    let mut info = [0u64; 15];
    for (dst, src) in info.iter_mut().zip(rec.ExceptionInformation.iter()) {
        *dst = *src as u64;
    }
    let exc = MINIDUMP_EXCEPTION_STREAM {
        ThreadId: marker.tid,
        __alignment: 0,
        ExceptionRecord: MINIDUMP_EXCEPTION {
            ExceptionCode: rec.ExceptionCode as u32,
            ExceptionFlags: rec.ExceptionFlags,
            ExceptionRecord: rec.ExceptionRecord as u64,
            ExceptionAddress: rec.ExceptionAddress as u64,
            NumberParameters: rec.NumberParameters,
            __unusedAlignment: 0,
            ExceptionInformation: info,
        },
        ThreadContext: MINIDUMP_LOCATION_DESCRIPTOR {
            DataSize: CTX_LEN as u32,
            Rva: ctx_rva,
        },
    };
    // SAFETY: `exc` is a fully-initialised, padding-free (packed(4)) struct of plain data.
    file.write_all(unsafe { as_bytes(&exc) })
        .map_err(|e| format!("cannot append the exception stream: {e}"))?;

    // Step 4: the new directory -- the old entries, verbatim, plus one for the Exception stream.
    let new_dir_rva =
        file.stream_position()
            .map_err(|e| format!("cannot get the current file position: {e}"))? as u32;
    file.write_all(&dir_bytes)
        .map_err(|e| format!("cannot append the stream directory: {e}"))?;
    let new_entry = MINIDUMP_DIRECTORY {
        StreamType: ExceptionStream as u32,
        Location: MINIDUMP_LOCATION_DESCRIPTOR {
            DataSize: std::mem::size_of::<MINIDUMP_EXCEPTION_STREAM>() as u32,
            Rva: exc_rva,
        },
    };
    // SAFETY: same POD justification.
    file.write_all(unsafe { as_bytes(&new_entry) })
        .map_err(|e| format!("cannot append the new directory entry: {e}"))?;

    // Step 5: point the header at the new directory, one stream more than before.
    let mut new_header = header;
    new_header.NumberOfStreams = header.NumberOfStreams + 1;
    new_header.StreamDirectoryRva = new_dir_rva;
    file.seek(SeekFrom::Start(0))
        .map_err(|e| format!("cannot seek to the minidump header: {e}"))?;
    // SAFETY: same POD justification; `new_header` is a copy of a header this process just read.
    file.write_all(unsafe { as_bytes(&new_header) })
        .map_err(|e| format!("cannot rewrite the minidump header: {e}"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "mh_crash=1\n\
         code=0xc0000005\n\
         flags=0x00000000\n\
         pid=4242\n\
         tid=1337\n\
         address=0x100175b0\n\
         pointers=0x0019f8b4\n\
         module=mh.dll\n\
         module_base=0x10000000\n\
         offset=0x000175b0\n\
         match_id=0199a3c1b2d04f1e8a7c6b5d4e3f2a10\n\
         build=0.1.0-rc1+abc12345\n\
         when=20260917T164346Z\n";

    /// The exact text `mh_crash_marker_text()` composes, parsed back field by field. If this ever
    /// fails, the two halves of the contract have drifted -- change both or neither.
    #[test]
    fn the_marker_the_dll_writes_parses_back_whole() {
        let m = Marker::parse(SAMPLE).unwrap();
        assert_eq!(m.version, 1);
        assert_eq!(m.code, 0xC000_0005);
        assert_eq!(m.pid, 4242);
        assert_eq!(m.tid, 1337);
        assert_eq!(m.address, 0x1001_75b0);
        assert_eq!(m.pointers, 0x0019_f8b4);
        assert_eq!(m.module, "mh.dll");
        assert_eq!(m.module_base, 0x1000_0000);
        assert_eq!(m.offset, 0x0001_75b0);
        assert_eq!(m.match_id, "0199a3c1b2d04f1e8a7c6b5d4e3f2a10");
        assert_eq!(m.build, "0.1.0-rc1+abc12345");
        assert_eq!(m.when, "20260917T164346Z");
        assert_eq!(m.where_text(), "mh.dll+0x175b0");
    }

    /// The offset the DLL wrote and the one implied by `address - module_base` must agree; a
    /// marker where they did not would send `crash_report.py` to the wrong function with no sign
    /// that anything was wrong.
    #[test]
    fn the_offset_is_the_address_minus_the_base() {
        let m = Marker::parse(SAMPLE).unwrap();
        assert_eq!(m.offset, m.address - m.module_base);
    }

    /// A marker truncated mid-write -- the process died between `WriteFile` and the flush -- must
    /// still yield what reached the disk.
    #[test]
    fn a_truncated_marker_still_gives_up_what_it_has() {
        let cut = &SAMPLE[..SAMPLE.find("module=").unwrap()];
        let m = Marker::parse(cut).unwrap();
        assert_eq!(m.code, 0xC000_0005);
        assert_eq!(m.pid, 4242);
        assert!(m.module.is_empty());
        assert!(m.where_text().contains("module unknown"));
    }

    #[test]
    fn something_that_is_not_a_marker_is_refused() {
        assert!(Marker::parse("hello=world\n").is_err());
        assert!(Marker::parse("").is_err());
        assert!(Marker::parse("mh_crash=99\n").is_err(), "a future version");
    }

    /// dist LA5's `ctx=` key. `SAMPLE` (above) has no such line -- adding a field must not have
    /// bumped `MH_CRASH_MARKER_VERSION`, so an OLD marker with no `ctx=` line still parses, just
    /// with `has_context` at its default (false).
    #[test]
    fn a_marker_with_no_ctx_line_defaults_to_no_context() {
        let m = Marker::parse(SAMPLE).unwrap();
        assert!(
            !m.has_context,
            "an old marker must not claim a sidecar it never wrote"
        );
    }

    #[test]
    fn ctx_1_and_ctx_0_parse_to_the_right_bool() {
        assert!(
            Marker::parse(&format!("{SAMPLE}ctx=1\n"))
                .unwrap()
                .has_context
        );
        assert!(
            !Marker::parse(&format!("{SAMPLE}ctx=0\n"))
                .unwrap()
                .has_context
        );
    }

    #[test]
    fn ctx32_path_is_the_marker_path_plus_the_suffix() {
        let p = Marker::ctx32_path(Path::new(r"C:\games\mh\logs\mh_crash_1234.marker"));
        assert_eq!(p, Path::new(r"C:\games\mh\logs\mh_crash_1234.marker.ctx32"));
    }

    /// The flag word, spelled out. Written as a test rather than a comment because the one thing
    /// that must never quietly change is the absence of `WithFullMemory` (1 << 1).
    #[test]
    fn the_dump_is_not_a_full_memory_dump() {
        assert_eq!(FLAGS, 0x0000_0121);
        assert_eq!(FLAGS & (1 << 1), 0, "WithFullMemory must never be set");
        assert_ne!(FLAGS & 1, 0, "WithDataSegs");
        assert_ne!(FLAGS & (1 << 5), 0, "WithUnloadedModules");
        assert_ne!(FLAGS & (1 << 8), 0, "WithProcessThreadData");
    }

    // ---- the dump itself, written and then READ BACK ------------------------------------------
    //
    // LA4's first done_when clause asks for a `.dmp` that loads in WinDbg with the faulting frame
    // inside mh.dll. `cdb` is not installed on this machine, and the clause's own fallback is to
    // "verify the minidump stream table with the `minidump` crate in a test". These two do that,
    // and between them they cover both halves of the real path:
    //
    //   * an EXTERNAL process, dumped by pid -- which is the whole point of D12 -- parsed, with
    //     its own module list read back to prove the dump is of the process we meant.
    //   * a dump carrying an EXCEPTION RECORD, proving the stream a debugger uses to pick the
    //     faulting thread is really written when one is supplied.
    //
    // WHAT THEY DO NOT COVER, stated here rather than left to be discovered: neither dumps a
    // 32-BIT target. Both processes involved are x86_64, so dbghelp's WOW64 behaviour -- the one
    // uncertainty in the module header -- is untested by anything offline. Only the game itself
    // can close that, which is why `write_dump` retries without the exception record.

    /// **DBGHELP IS NOT REENTRANT AND `cargo test` IS.** `MiniDumpWriteDump` serialises on a
    /// process-wide dbghelp lock. Only one test calls it today (see the note further down for why
    /// the second one is gone), and the mutex stays because the next one to be added would
    /// otherwise reintroduce the hang that removed it -- a guard that costs nothing and would have
    /// saved two debugging rounds. The launcher itself never meets this: it dumps one process,
    /// from the UI thread, which is why the serialisation lives here and not in `write_dump`.
    static DUMP: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// Spawn something external and long-lived enough to dump. `ping` is on every Windows install
    /// and needs no fixture of our own; the point is only that it is a process we did not compile.
    ///
    /// SPAWNED DIRECTLY, NOT THROUGH `cmd /c`, and that is a bug this test already had. Through a
    /// shell the victim is `cmd.exe` and `ping.exe` is its CHILD -- so `Child::kill` reaps the
    /// shell and leaves the grandchild alive, holding the inherited stdout handle for its full
    /// thirty seconds. A test runner reading that pipe then waits for a process nobody is tracking.
    /// Null stdio on top, so nothing is inherited even for the moment it lives.
    fn spawn_victim() -> std::process::Child {
        std::process::Command::new("ping")
            .args(["-n", "30", "127.0.0.1"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .stdin(std::process::Stdio::null())
            .spawn()
            .expect("ping.exe must be startable")
    }

    #[test]
    fn a_dump_of_an_external_process_parses_and_names_that_process() {
        use minidump::{Minidump, MinidumpModuleList, MinidumpSystemInfo, MinidumpThreadList};

        let _serialised = DUMP.lock().unwrap_or_else(|e| e.into_inner());
        let mut victim = spawn_victim();
        let pid = victim.id();
        let dest = std::env::temp_dir().join(format!("mh_launcher_test_external_{pid}.dmp"));
        let marker = Marker {
            version: 1,
            code: 0xC000_0005,
            pid,
            tid: 0,
            // No exception pointers: we have not made this process fault, and inventing an address
            // in ITS memory is precisely the mistake the ClientPointers contract exists to avoid.
            pointers: 0,
            ..Marker::default()
        };
        // No marker path either: `marker.has_context` is false (the default), so `write_dump`
        // never looks for a `.ctx32` sidecar and this path is unused -- exercised in its own right
        // by `append_exception_stream_adds_a_real_exception_stream_a_debugger_can_read` below.
        let marker_path =
            std::env::temp_dir().join(format!("mh_launcher_test_unused_{pid}.marker"));

        // A PROCESS THAT HAS ONLY JUST BEEN SPAWNED CANNOT BE DUMPED, and the error says so
        // obliquely: `MiniDumpWriteDump` answers ERROR_PARTIAL_COPY (0x8007012B, "only part of a
        // ReadProcessMemory request was completed") while the loader is still mapping the image
        // and the module list it walks does not exist yet. Measured on the first run of this test.
        // The real path never meets it -- the game has been running for minutes and is blocked
        // inside its own handler -- so the retry belongs here, in the fixture, and not in
        // `write_dump`, where it would paper over a genuine failure to read a crashed process.
        let mut res = write_dump(&marker, &marker_path, &dest);
        for _ in 0..40 {
            if res.is_ok() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
            res = write_dump(&marker, &marker_path, &dest);
        }
        let _ = victim.kill();
        let _ = victim.wait();
        let bytes = res.expect("a minidump of an external process must be writable");
        assert!(bytes > 4096, "a {bytes}-byte dump is not a dump");

        let dump = Minidump::read_path(&dest).expect("the dump must parse as a minidump");
        // The three streams any debugger needs before it can say anything at all.
        dump.get_stream::<MinidumpSystemInfo>()
            .expect("SystemInfo stream");
        let threads = dump
            .get_stream::<MinidumpThreadList>()
            .expect("ThreadList stream");
        assert!(!threads.threads.is_empty(), "a dump with no threads");
        let modules = dump
            .get_stream::<MinidumpModuleList>()
            .expect("ModuleList stream");
        let names: Vec<String> = modules
            .iter()
            .map(|m| {
                let n: &str = &m.name;
                n.rsplit(['\\', '/'])
                    .next()
                    .unwrap_or(n)
                    .to_ascii_lowercase()
            })
            .collect();
        // THE DUMP IS OF THE PROCESS WE MEANT. Without this the test would pass on a dump of
        // anything, which is the failure a "did it write a file" assertion cannot see.
        assert!(
            names.iter().any(|n| n == "ping.exe"),
            "the module list does not name the process we dumped: {names:?}"
        );
        assert!(
            names.iter().any(|n| n == "ntdll.dll"),
            "no ntdll in the module list: {names:?}"
        );
        std::fs::remove_file(&dest).ok();
    }

    // dist LA4 found no way to make `MiniDumpWriteDump` itself produce an Exception stream in a
    // test (three attempts, all abandoned -- see the LA4 session notes for why:
    // `dump_local_context` on the test process hangs `cargo test`'s ~57 other tests, the
    // `mh_exit_probe` fixture is not built by `cargo test`, and a synthetic `EXCEPTION_POINTERS`
    // written into an external victim would cost two more `windows-sys` features used nowhere in
    // production) -- and the acceptance run settled why none of them could ever have worked: on the
    // real path, a 64-bit launcher dumping a 32-bit game, `MiniDumpWriteDump` REFUSES the exception
    // record outright (ERROR_NOACCESS; it reads the WOW64 target's pointers as 64-bit). That
    // measurement is dist LA5's whole premise -- see docs/launcher.md section 8 and the module docs
    // above `append_exception_stream`. The test below proves the REPLACEMENT mechanism instead:
    // not "did `MiniDumpWriteDump` write an Exception stream" (it provably cannot, on this
    // configuration), but "does `append_exception_stream` turn a `.ctx32` sidecar into a real one
    // that the `minidump` crate reads back as a faulting thread + context".

    /// dist LA5's acceptance mechanism, offline: no real WOW64 crash is needed to prove the APPEND
    /// is correct, only a dump to append to (the same external-`ping.exe` dump the test above
    /// already proves is real) and a `.ctx32` blob shaped exactly the way
    /// `mh/seams/crash_marker.cpp::write_context` shapes one.
    #[test]
    fn append_exception_stream_adds_a_real_exception_stream_a_debugger_can_read() {
        use minidump::{Minidump, MinidumpException};

        // The two structs' sizes are the wire contract with the DLL -- see the `static_assert`s in
        // crash_marker.cpp. If either ever drifted, this is the assertion that would catch it before
        // a real dump silently misparsed.
        assert_eq!(std::mem::size_of::<EXCEPTION_RECORD32>(), 80);
        assert_eq!(std::mem::size_of::<WOW64_CONTEXT>(), 716);
        assert_eq!(std::mem::size_of::<MINIDUMP_HEADER>(), 32);
        assert_eq!(std::mem::size_of::<MINIDUMP_DIRECTORY>(), 12);
        assert_eq!(std::mem::size_of::<MINIDUMP_EXCEPTION_STREAM>(), 168);

        let _serialised = DUMP.lock().unwrap_or_else(|e| e.into_inner());
        let mut victim = spawn_victim();
        let pid = victim.id();
        let dest = std::env::temp_dir().join(format!("mh_launcher_test_exc_{pid}.dmp"));
        let marker_path = std::env::temp_dir().join(format!("mh_launcher_test_exc_{pid}.marker"));
        let marker = Marker {
            version: 1,
            code: 0xC000_0005,
            pid,
            tid: 4242,
            pointers: 0,
            ..Marker::default()
        };
        let mut res = write_dump(&marker, &marker_path, &dest);
        for _ in 0..40 {
            if res.is_ok() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
            res = write_dump(&marker, &marker_path, &dest);
        }
        let _ = victim.kill();
        let _ = victim.wait();
        res.expect("a base dump of an external process must be writable");
        // No Exception stream yet: `marker.has_context` was false, so `write_dump` had nothing to
        // append. This is the state a real WOW64 crash dump is in before LA5.
        {
            let dump = Minidump::read_path(&dest).expect("the base dump must parse");
            assert!(
                dump.get_stream::<MinidumpException>().is_err(),
                "the base dump should carry no Exception stream yet"
            );
        }

        // The synthetic `.ctx32` blob: an EXCEPTION_RECORD32 immediately followed by a
        // WOW64_CONTEXT, byte for byte what `write_context()` writes from inside the game.
        let rec = EXCEPTION_RECORD32 {
            ExceptionCode: 0xC000_0005u32 as i32,
            ExceptionFlags: 0,
            ExceptionRecord: 0,
            ExceptionAddress: 0x1001_75b0,
            NumberParameters: 2,
            ExceptionInformation: {
                let mut a = [0u32; 15];
                a[0] = 1; // a write access
                a[1] = 0x0000_0010;
                a
            },
        };
        let ctx = WOW64_CONTEXT {
            Eip: 0x1001_75b0,
            Eax: 0xdead_beef,
            Esp: 0x0019_f000,
            ..Default::default()
        };
        let mut ctx32 = Vec::new();
        // SAFETY: both are fully-initialised, padding-free plain-data structs (the same
        // justification as `as_bytes`'s doc comment; this is the test's own copy so the function
        // under test cannot mark its own homework).
        unsafe {
            ctx32.extend_from_slice(std::slice::from_raw_parts(
                (&rec as *const EXCEPTION_RECORD32).cast::<u8>(),
                std::mem::size_of::<EXCEPTION_RECORD32>(),
            ));
            ctx32.extend_from_slice(std::slice::from_raw_parts(
                (&ctx as *const WOW64_CONTEXT).cast::<u8>(),
                std::mem::size_of::<WOW64_CONTEXT>(),
            ));
        }

        append_exception_stream(&dest, &ctx32, &marker).expect("the append must succeed");

        let dump = Minidump::read_path(&dest).expect("the appended dump must still parse whole");
        let exc = dump
            .get_stream::<MinidumpException>()
            .expect("Exception stream (dist LA5's whole point)");
        assert_eq!(
            exc.get_crashing_thread_id(),
            4242,
            "the Exception stream must name the thread the MARKER said faulted"
        );
        assert_eq!(exc.raw.exception_record.exception_code, 0xC000_0005);
        assert_eq!(exc.raw.exception_record.exception_address, 0x1001_75b0);
        assert_eq!(exc.raw.exception_record.number_parameters, 2);
        assert_eq!(exc.raw.exception_record.exception_information[1], 0x10);

        // The CONTEXT bytes are reachable at the RVA the Exception stream names, and they are the
        // exact bytes this test wrote in -- Eip included, the one value a triager wants first.
        let ctx_loc = &exc.raw.thread_context;
        assert_eq!(
            ctx_loc.data_size as usize,
            std::mem::size_of::<WOW64_CONTEXT>()
        );
        let all = std::fs::read(&dest).expect("re-reading the dump file");
        let ctx_bytes =
            &all[ctx_loc.rva as usize..ctx_loc.rva as usize + ctx_loc.data_size as usize];
        let read_back: WOW64_CONTEXT =
            unsafe { std::ptr::read_unaligned(ctx_bytes.as_ptr().cast()) };
        assert_eq!(read_back.Eip, 0x1001_75b0);
        assert_eq!(read_back.Eax, 0xdead_beef);
        assert_eq!(read_back.Esp, 0x0019_f000);

        // The rest of the dump -- the module list `write_dump` wrote before this ever ran -- must
        // still be intact: the append must ADD a stream, never corrupt the ones already there.
        let modules = dump
            .get_stream::<minidump::MinidumpModuleList>()
            .expect("ModuleList stream must survive the append");
        assert!(
            modules
                .iter()
                .any(|m| m.name.to_ascii_lowercase().ends_with("ping.exe")),
            "the original dump's own module list must be unchanged"
        );

        std::fs::remove_file(&dest).ok();
    }

    /// A truncated `.ctx32` (the same "the process died mid-write" case the marker's own parser
    /// already has to tolerate) must be a clean refusal, not a panic or a corrupted dump.
    #[test]
    fn append_exception_stream_refuses_a_truncated_ctx32() {
        let dest = std::env::temp_dir().join("mh_launcher_test_exc_truncated.dmp");
        std::fs::write(&dest, b"not really a minidump, but long enough to open").unwrap();
        let marker = Marker {
            tid: 1,
            ..Marker::default()
        };
        let err = append_exception_stream(&dest, &[0u8; 10], &marker).unwrap_err();
        assert!(err.contains("need at least"), "{err}");
        std::fs::remove_file(&dest).ok();
    }

    /// The dump file itself must be a real minidump -- the signature check is what stops this
    /// function from writing a plausible-looking directory entry into a file that was never one.
    #[test]
    fn append_exception_stream_refuses_a_file_with_no_minidump_signature() {
        let dest = std::env::temp_dir().join("mh_launcher_test_exc_not_a_dump.dmp");
        std::fs::write(&dest, [0u8; 64]).unwrap();
        let marker = Marker {
            tid: 1,
            ..Marker::default()
        };
        let ctx32 = vec![
            0u8;
            std::mem::size_of::<EXCEPTION_RECORD32>()
                + std::mem::size_of::<WOW64_CONTEXT>()
        ];
        let err = append_exception_stream(&dest, &ctx32, &marker).unwrap_err();
        assert!(err.contains("minidump signature"), "{err}");
        std::fs::remove_file(&dest).ok();
    }

    /// The channel is a real pair of kernel objects, and a launcher that could not make them would
    /// silently lose every crash -- so the creation is asserted, not assumed.
    #[test]
    fn a_channel_creates_its_events_and_names_a_marker() {
        let dir = std::env::temp_dir().join("mh_launcher_test_crash_channel");
        let ch = Channel::create(&dir).expect("the events must be creatable");
        let env = ch.env();
        assert_eq!(env[0].0, ENV_CHANNEL);
        assert_eq!(env[1].0, ENV_MARKER);
        assert!(!env[0].1.is_empty());
        assert!(env[1].1.ends_with(".marker"), "{}", env[1].1);
        assert!(!ch.crashed(), "nothing has signalled it");
        std::fs::remove_dir_all(&dir).ok();
    }
}
