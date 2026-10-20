//! The window-procedure hook (dist LA22): WM_INPUTLANGCHANGEREQUEST without the blocking default,
//! and the "which message is the UI thread inside" breadcrumb for the freeze watchdog.
//!
//! THE BUG. A player's launcher froze for 3 s (and once for 3723 s) right after the game exited.
//! The dump put the UI thread in `win32u!NtUserMessageCall` under `call [DefWindowProcW]` in winit's
//! window procedure, handling `WM_INPUTLANGCHANGEREQUEST` (0x50) with lParam 0x409. The player's
//! AutoHotkey script runs `PostMessage(0x50, 0, 0x409,, "A")` on every single left-Shift press, i.e.
//! it posts a layout-switch request to the active window -- here the launcher -- with a bare language
//! id and no care whether that layout is already the active one. `DefWindowProcW` answers that with
//! `ActivateKeyboardLayout(hkl, 0)`, which also tells the shell (`HSHELL_LANGUAGE` to every shell-hook
//! window, a cross-process wait) and the input-method framework. With a hook-injecting overlay
//! (RivaTuner) in the process and an AutoHotkey process pumping at its own pace, that wait can run
//! to seconds or longer, on the UI thread, with nothing of ours in the stack.
//!
//! THE FIX, in terms of what we control:
//! * a request for the layout that is ALREADY active is a no-op -- we answer it without calling
//!   into the kernel at all (the repeated-Shift case, which is most of them);
//! * a request for a different layout is applied by us with `ActivateKeyboardLayout(hkl,
//!   KLF_NOTELLSHELL)`: same layout switch (so `WM_CHAR` text follows it and `WM_INPUTLANGCHANGE`
//!   still arrives), minus the shell-hook broadcast;
//! * a bare language id (0x0409) is resolved against `GetKeyboardLayoutList` first, as
//!   `DefWindowProcW` itself does; an id with no installed layout is ignored rather than passed on;
//! * `HKL_NEXT`/`HKL_PREV` (lParam 1/0) and everything else go to the default untouched.
//!
//! The hook is a comctl32 window SUBCLASS rather than winit's `with_msg_hook`: that hook runs in the
//! message loop only, so a SENT message (SendMessage, which AutoHotkey also uses) would bypass it.

use std::sync::atomic::{AtomicBool, Ordering};

/// What to do with one WM_INPUTLANGCHANGEREQUEST.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    /// Not ours to interpret (HKL_NEXT / HKL_PREV): hand it to the default procedure.
    Default,
    /// Nothing to do: already active, or no such layout installed.
    Ignore,
    /// Activate this layout (the low 32 bits of the HKL).
    Activate(u32),
}

/// Decide the action for lParam `lparam` given the installed layouts (HKL low 32 bits) and the
/// UI thread's current one. Pure; unit-tested.
pub fn decide(lparam: isize, installed: &[u32], current: u32) -> Action {
    let lp = lparam as usize as u32;
    if lp <= 1 {
        return Action::Default; // HKL_PREV / HKL_NEXT
    }
    let target = if lp >> 16 == 0 {
        // A bare language id: the first installed layout of that language (the HKL's low word).
        match installed.iter().find(|h| **h & 0xFFFF == lp) {
            Some(h) => *h,
            None => return Action::Ignore,
        }
    } else if installed.contains(&lp) {
        lp
    } else {
        return Action::Ignore;
    };
    if target == current {
        Action::Ignore
    } else {
        Action::Activate(target)
    }
}

static INSTALLED: AtomicBool = AtomicBool::new(false);

/// Subclass this thread's top-level launcher window. Idempotent; true once installed.
/// Call on the UI thread (a subclass can only be set from the window's own thread).
pub fn install() -> bool {
    if INSTALLED.load(Ordering::Acquire) {
        return true;
    }
    match imp::install() {
        true => {
            INSTALLED.store(true, Ordering::Release);
            crate::log::line(
                "ui: window hook installed (WM_INPUTLANGCHANGEREQUEST, message watch)",
            );
            true
        }
        false => false,
    }
}

mod imp {
    use super::{decide, Action};
    use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        ActivateKeyboardLayout, GetKeyboardLayout, GetKeyboardLayoutList, HKL, KLF_NOTELLSHELL,
    };
    use windows_sys::Win32::UI::Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass};
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        EnumThreadWindows, GetWindowTextW, IsWindowVisible, WM_INPUTLANGCHANGEREQUEST, WM_NCDESTROY,
    };

    const TITLE: &str = "Mission Humanity";
    const SUBCLASS_ID: usize = 0x4C41_3232; // "LA22"

    unsafe extern "system" fn enum_cb(h: HWND, out: LPARAM) -> i32 {
        let mut buf = [0u16; 64];
        // SAFETY: `buf` is valid for 64 u16s; `h` is a window handle handed in by the enumeration.
        let n = unsafe { GetWindowTextW(h, buf.as_mut_ptr(), 64) };
        let title = String::from_utf16_lossy(&buf[..n.max(0) as usize]);
        // SAFETY: plain query on a live handle.
        if title == TITLE && unsafe { IsWindowVisible(h) } != 0 {
            // SAFETY: `out` is the address of the `usize` slot `install` passed.
            unsafe { *(out as *mut usize) = h as usize };
            return 0; // stop
        }
        1
    }

    fn installed_layouts() -> Vec<u32> {
        // SAFETY: the two-call size-then-fill protocol of GetKeyboardLayoutList.
        unsafe {
            let n = GetKeyboardLayoutList(0, std::ptr::null_mut());
            if n <= 0 {
                return Vec::new();
            }
            let mut v: Vec<HKL> = vec![std::ptr::null_mut(); n as usize];
            let got = GetKeyboardLayoutList(n, v.as_mut_ptr());
            v.truncate(got.max(0) as usize);
            v.into_iter().map(|h| h as usize as u32).collect()
        }
    }

    pub fn install() -> bool {
        let mut found: usize = 0;
        // SAFETY: enumerates this thread's windows; `found` outlives the call.
        unsafe {
            EnumThreadWindows(
                windows_sys::Win32::System::Threading::GetCurrentThreadId(),
                Some(enum_cb),
                &mut found as *mut usize as LPARAM,
            );
        }
        if found == 0 {
            return false;
        }
        // SAFETY: `found` is a live window of this thread; `proc_` has the SUBCLASSPROC signature.
        unsafe { SetWindowSubclass(found as HWND, Some(proc_), SUBCLASS_ID, 0) != 0 }
    }

    unsafe extern "system" fn proc_(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
        _id: usize,
        _data: usize,
    ) -> LRESULT {
        let prev = crate::watchdog::msg_enter(hwnd as usize, msg);
        let r = if msg == WM_INPUTLANGCHANGEREQUEST {
            // SAFETY: plain query of this thread's layout.
            let current = unsafe { GetKeyboardLayout(0) } as usize as u32;
            match decide(lparam, &installed_layouts(), current) {
                Action::Default => unsafe { DefSubclassProc(hwnd, msg, wparam, lparam) },
                Action::Ignore => 0,
                Action::Activate(low) => {
                    // The HKL is sign-extended on 64-bit; rebuild it from the installed list so the
                    // exact handle value the OS gave out is the one passed back.
                    let hkl = installed_layouts_raw()
                        .into_iter()
                        .find(|h| *h as usize as u32 == low)
                        .unwrap_or(low as usize as HKL);
                    // SAFETY: `hkl` is an installed layout handle.
                    unsafe { ActivateKeyboardLayout(hkl, KLF_NOTELLSHELL) };
                    0
                }
            }
        } else {
            if msg == WM_NCDESTROY {
                // SAFETY: removing our own subclass from the window being destroyed.
                unsafe { RemoveWindowSubclass(hwnd, Some(proc_), SUBCLASS_ID) };
                super::INSTALLED.store(false, std::sync::atomic::Ordering::Release);
            }
            // SAFETY: forwarding the message unchanged to the next procedure in the chain.
            unsafe { DefSubclassProc(hwnd, msg, wparam, lparam) }
        };
        crate::watchdog::msg_leave(prev);
        r
    }

    fn installed_layouts_raw() -> Vec<HKL> {
        // SAFETY: as `installed_layouts`.
        unsafe {
            let n = GetKeyboardLayoutList(0, std::ptr::null_mut());
            if n <= 0 {
                return Vec::new();
            }
            let mut v: Vec<HKL> = vec![std::ptr::null_mut(); n as usize];
            let got = GetKeyboardLayoutList(n, v.as_mut_ptr());
            v.truncate(got.max(0) as usize);
            v
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const US: u32 = 0x0409_0409;
    const RU: u32 = 0x0419_0419;
    const UK: u32 = 0x0415_0415;

    #[test]
    fn bare_language_id_resolves_to_the_installed_layout() {
        // The AutoHotkey trigger: PostMessage(0x50, 0, 0x409).
        assert_eq!(decide(0x409, &[US, RU, UK], RU), Action::Activate(US));
    }

    #[test]
    fn repeated_request_for_the_active_layout_is_a_no_op() {
        assert_eq!(decide(0x409, &[US, RU], US), Action::Ignore);
        assert_eq!(decide(US as isize, &[US, RU], US), Action::Ignore);
    }

    #[test]
    fn full_hkl_must_be_installed() {
        assert_eq!(decide(RU as isize, &[US, RU], US), Action::Activate(RU));
        assert_eq!(decide(UK as isize, &[US, RU], US), Action::Ignore);
    }

    #[test]
    fn unknown_language_is_ignored_not_passed_on() {
        assert_eq!(decide(0x40C, &[US, RU], US), Action::Ignore);
    }

    #[test]
    fn next_and_prev_go_to_the_default() {
        assert_eq!(decide(0, &[US], US), Action::Default);
        assert_eq!(decide(1, &[US], US), Action::Default);
    }

    #[test]
    fn sign_extended_hkl_in_lparam_matches() {
        // A 64-bit HKL such as 0xFFFFFFFFF0010409 truncates to the installed low half.
        let h = 0xF001_0409u32;
        assert_eq!(decide(h as i32 as isize, &[US, h], US), Action::Activate(h));
    }
}
