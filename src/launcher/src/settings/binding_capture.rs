//! Passive, capture-scoped observer of the launcher's keyboard message queue.
//! WH_KEYBOARD retains scan/E0 identity without a desktop-global low-level hook; every message chains unchanged.
use super::bindings::{self, Chord};
use egui::{Event, PointerButton};
use std::cell::RefCell;
use windows_sys::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
use windows_sys::Win32::System::Threading::{GetCurrentProcessId, GetCurrentThreadId};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, GetForegroundWindow, GetWindowThreadProcessId, SetWindowsHookExW,
    UnhookWindowsHookEx, HC_ACTION, HHOOK, WH_KEYBOARD,
};

#[derive(Clone, Copy, Debug)]
struct Stroke {
    scan: u8,
    e0: bool,
    modifiers: u8,
    role: Option<&'static str>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct KeyTransition {
    vk: usize,
    scan: u8,
    e0: bool,
    up: bool,
    repeat: bool,
}
/// WH_KEYBOARD receives a virtual key and the original WM_KEY* LPARAM, not a KBDLLHOOKSTRUCT pointer.
fn transition(wparam: WPARAM, lparam: LPARAM) -> Option<KeyTransition> {
    if wparam > 255 {
        return None;
    }
    let bits = lparam as u32;
    let scan = ((bits >> 16) & 0xff) as u8;
    let e0 = bits & (1 << 24) != 0;
    // WM_KEY* uses generic modifier VKs; retain which physical side changed so snapshots/releases agree.
    let vk = match wparam {
        0x10 => {
            if scan == 0x36 {
                0xa1
            } else {
                0xa0
            }
        }
        0x11 => {
            if e0 {
                0xa3
            } else {
                0xa2
            }
        }
        0x12 => {
            if e0 {
                0xa5
            } else {
                0xa4
            }
        }
        vk => vk,
    };
    Some(KeyTransition {
        vk,
        scan,
        e0,
        up: bits & (1 << 31) != 0,
        repeat: bits & (1 << 30) != 0,
    })
}
struct Observer {
    down: [bool; 256],
    blocked: [bool; 256],
    strokes: [Option<Stroke>; 32],
    count: usize,
}
impl Observer {
    fn observe(&mut self, key: KeyTransition) {
        let vk = key.vk;
        let up = key.up;
        let repeat = key.repeat || self.down[vk];
        self.down[vk] = !up;
        if up {
            self.blocked[vk] = false;
            return;
        }
        if repeat || self.blocked.iter().any(|blocked| *blocked) {
            return;
        }
        let held = |keys: &[usize]| keys.iter().any(|vk| self.down[*vk]);
        let modifiers = u8::from(held(&[0x11, 0xa2, 0xa3]))
            | (u8::from(held(&[0x12, 0xa4, 0xa5])) << 1)
            | (u8::from(held(&[0x10, 0xa0, 0xa1])) << 2);
        let role = match vk {
            0x11 | 0xa2 | 0xa3 => Some("Ctrl"),
            0x12 | 0xa4 | 0xa5 => Some("Alt"),
            0x10 | 0xa0 | 0xa1 => Some("Shift"),
            _ => None,
        };
        if self.count < self.strokes.len() {
            let supported = key.scan > 0 && key.scan <= 0x7f && vk != 0x13 && !held(&[0x5b, 0x5c]);
            self.strokes[self.count] = Some(Stroke {
                scan: if supported { key.scan } else { 0 },
                e0: key.e0,
                modifiers,
                role,
            });
            self.count += 1;
        }
    }
}
thread_local! { static OBSERVER: RefCell<Option<Observer>> = const { RefCell::new(None) }; }
fn own_foreground() -> bool {
    let mut pid = 0;
    unsafe {
        GetWindowThreadProcessId(GetForegroundWindow(), &mut pid);
    }
    pid == unsafe { GetCurrentProcessId() }
}
unsafe extern "system" fn keyboard(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    // HC_NOREMOVE is deliberately ignored: PeekMessage(PM_NOREMOVE) must not enqueue the same press twice.
    if code == HC_ACTION as i32 && own_foreground() {
        if let Some(key) = transition(wparam, lparam) {
            OBSERVER.with(|slot| {
                if let Some(observer) = slot.borrow_mut().as_mut() {
                    observer.observe(key);
                }
            });
        }
    }
    unsafe { CallNextHookEx(std::ptr::null_mut(), code, wparam, lparam) }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Key,
    Modifier,
    Button,
}
#[derive(Debug, PartialEq, Eq)]
pub enum Result {
    Value(String),
    Cancel,
    Unsupported,
}
pub struct Capture {
    pub id: String,
    pub kind: Kind,
    hook: HHOOK,
    pub focus_id: egui::Id,
    mouse_ready: bool,
    pub mouse_rect: egui::Rect,
    pub owner_rect: egui::Rect,
}
impl Capture {
    pub fn begin(
        id: String,
        kind: Kind,
        focus_id: egui::Id,
        ctx: &egui::Context,
    ) -> std::result::Result<Self, String> {
        let mut pid = 0;
        let window_thread = unsafe { GetWindowThreadProcessId(GetForegroundWindow(), &mut pid) };
        if window_thread == 0 || pid != unsafe { GetCurrentProcessId() } {
            return Err("physical capture needs the launcher's foreground window".into());
        }
        if window_thread != unsafe { GetCurrentThreadId() } {
            return Err("physical capture must run on the launcher's window message thread".into());
        }
        let mut down = [false; 256];
        for (vk, state) in down.iter_mut().enumerate().skip(8) {
            *state = unsafe { GetAsyncKeyState(vk as i32) } < 0;
        }
        // Generic modifier VKs do not get independent release events; use left/right states.
        for vk in [0x10, 0x11, 0x12] {
            down[vk] = false;
        }
        OBSERVER.with(|slot| {
            *slot.borrow_mut() = Some(Observer {
                blocked: down,
                down,
                strokes: [None; 32],
                count: 0,
            })
        });
        let hook = unsafe {
            SetWindowsHookExW(
                WH_KEYBOARD,
                Some(keyboard),
                std::ptr::null_mut(),
                window_thread,
            )
        };
        if hook.is_null() {
            OBSERVER.with(|slot| *slot.borrow_mut() = None);
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(Self {
            id,
            kind,
            hook,
            focus_id,
            mouse_ready: !ctx.input(|i| i.pointer.any_down()),
            mouse_rect: egui::Rect::NOTHING,
            owner_rect: egui::Rect::NOTHING,
        })
    }
    /// Run BEFORE any launcher widgets. Logical navigation focus may already have moved at
    /// begin_pass, but a queued physical stroke still belongs to the active capture.
    pub fn poll(&mut self, ctx: &egui::Context) -> Option<Result> {
        if ctx.input(|i| {
            !i.focused
                || i.events
                    .iter()
                    .any(|event| matches!(event, Event::WindowFocused(false)))
        }) {
            return Some(Result::Cancel);
        }
        let strokes = OBSERVER.with(|slot| {
            let mut slot = slot.borrow_mut();
            let Some(observer) = slot.as_mut() else {
                return [None; 32];
            };
            observer.count = 0;
            std::mem::take(&mut observer.strokes)
        });
        let has_strokes = strokes.iter().any(Option::is_some);
        let events = ctx.input(|i| i.events.clone());
        if events.iter().any(|event| {
            matches!(event,
            Event::PointerButton { pos, pressed: true, .. } if !self.owner_rect.contains(*pos))
        }) {
            return Some(Result::Cancel);
        }
        if !ctx.memory(|memory| memory.has_focus(self.focus_id)) && !has_strokes {
            return Some(Result::Cancel);
        }
        ctx.memory_mut(|memory| {
            if has_strokes {
                memory.request_focus(self.focus_id);
            }
            // begin_pass may already have set Next/Previous before the new owner's filter exists.
            // Clear that navigation intent before widgets see it, not merely its source event.
            memory.move_focus(egui::FocusDirection::None);
            memory.set_focus_lock_filter(
                self.focus_id,
                egui::EventFilter {
                    tab: true,
                    horizontal_arrows: true,
                    vertical_arrows: true,
                    escape: true,
                },
            );
        });
        let mut result = None;
        for stroke in strokes.into_iter().flatten() {
            if stroke.scan == 1 && !stroke.e0 {
                result = Some(Result::Cancel);
                break;
            }
            match self.kind {
                Kind::Modifier => {
                    result = Some(
                        stroke
                            .role
                            .map_or(Result::Unsupported, |role| Result::Value(role.into())),
                    );
                }
                Kind::Key if stroke.role.is_none() => {
                    let value = bindings::format(Chord {
                        scan: stroke.scan,
                        e0: stroke.e0,
                        modifiers: stroke.modifiers,
                    });
                    result = Some(if bindings::parse(&value).is_ok() {
                        Result::Value(value)
                    } else {
                        Result::Unsupported
                    });
                }
                Kind::Button => result = Some(Result::Unsupported),
                _ => (),
            }
            if result.is_some() {
                break;
            }
        }
        if self.kind == Kind::Button && self.mouse_ready && result.is_none() {
            for event in events {
                if let Event::PointerButton {
                    pos,
                    button,
                    pressed: true,
                    ..
                } = event
                {
                    if !self.mouse_rect.contains(pos) {
                        continue;
                    }
                    result = Some(match button {
                        PointerButton::Primary => Result::Value("MouseLeft".into()),
                        PointerButton::Secondary => Result::Value("MouseRight".into()),
                        _ => Result::Unsupported,
                    });
                    break;
                }
            }
        }
        if !ctx.input(|i| i.pointer.any_down()) {
            self.mouse_ready = true;
        }
        // Do not let a captured key activate menu/Start or enter search later in this frame.
        // Window focus and outside-panel clicks cancel above without consuming text-entry input.
        ctx.input_mut(|i| {
            i.events
                .retain(|event| !matches!(event, Event::Key { .. } | Event::Text(_)))
        });
        result
    }
}
impl Drop for Capture {
    fn drop(&mut self) {
        if !self.hook.is_null() {
            unsafe {
                UnhookWindowsHookEx(self.hook);
            }
        }
        OBSERVER.with(|slot| *slot.borrow_mut() = None);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn finish_headless_frame(mut output: egui::FullOutput) {
        // Input/focus checks have no renderer to upload the font atlas to.
        output.textures_delta.clear();
    }
    fn fixture(kind: Kind) -> (Capture, egui::Context) {
        OBSERVER.with(|slot| {
            *slot.borrow_mut() = Some(Observer {
                down: [false; 256],
                blocked: [false; 256],
                strokes: [None; 32],
                count: 0,
            })
        });
        let capture = Capture {
            id: "keys.camera_up_alt".into(),
            kind,
            hook: std::ptr::null_mut(),
            focus_id: egui::Id::new("capture_owner"),
            mouse_ready: true,
            mouse_rect: egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(100.0, 100.0)),
            owner_rect: egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(100.0, 150.0)),
        };
        let ctx = egui::Context::default();
        ctx.begin_pass(egui::RawInput {
            focused: true,
            ..Default::default()
        });
        ctx.memory_mut(|memory| memory.request_focus(capture.focus_id));
        finish_headless_frame(ctx.end_pass());
        (capture, ctx)
    }
    fn stroke(vk: u32, scan: u32, e0: bool, up: bool) {
        let bits = (scan << 16) | if e0 { 1 << 24 } else { 0 } | if up { 1 << 31 } else { 0 };
        OBSERVER.with(|slot| {
            slot.borrow_mut()
                .as_mut()
                .unwrap()
                .observe(transition(vk as WPARAM, bits as LPARAM).unwrap())
        });
    }
    #[test]
    fn keyboard_message_decoder_preserves_generic_modifier_sides_and_physical_twins() {
        for (vk, scan, e0, expected_vk) in [
            (0x10, 0x2a, false, 0xa0),
            (0x10, 0x36, false, 0xa1),
            (0x11, 0x1d, false, 0xa2),
            (0x11, 0x1d, true, 0xa3),
            (0x12, 0x38, false, 0xa4),
            (0x12, 0x38, true, 0xa5),
            (0x26, 0x48, true, 0x26),
            (0x26, 0x48, false, 0x26),
            (0x0d, 0x1c, false, 0x0d),
            (0x0d, 0x1c, true, 0x0d),
        ] {
            let bits = (scan << 16) | if e0 { 1 << 24 } else { 0 };
            let key = transition(vk, bits).unwrap();
            assert_eq!(
                (key.vk, key.scan, key.e0, key.up, key.repeat),
                (expected_vk, scan as u8, e0, false, false)
            );
            let release = transition(vk, (bits as u32 | 1 << 30 | 1 << 31) as LPARAM).unwrap();
            assert!(release.up && release.repeat);
        }
        let mut observer = Observer {
            down: [false; 256],
            blocked: [false; 256],
            strokes: [None; 32],
            count: 0,
        };
        observer.observe(transition(0x41, (0x1e << 16) | (1 << 30)).unwrap());
        assert_eq!(
            observer.count, 0,
            "Windows' previous-state bit rejects repeats even after a missed initial down"
        );
    }
    #[test]
    fn capture_matrix_retains_fast_navigation_e0_and_either_side_roles_before_widgets() {
        // Observer-stage physical events, not an assertion about OS acquisition. Live smoke covers the hook.
        for (kind, vk, scan, e0, expected) in [
            (Kind::Key, 0x09, 0x0f, false, "Tab"),
            (Kind::Key, 0x26, 0x48, true, "Up"),
            (Kind::Key, 0x68, 0x48, false, "Numpad8"),
            (Kind::Key, 0x0d, 0x1c, true, "NumpadEnter"),
            (Kind::Modifier, 0xa2, 0x1d, false, "Ctrl"),
            (Kind::Modifier, 0xa3, 0x1d, true, "Ctrl"),
            (Kind::Modifier, 0xa0, 0x2a, false, "Shift"),
            (Kind::Modifier, 0xa1, 0x36, false, "Shift"),
            (Kind::Modifier, 0xa4, 0x38, false, "Alt"),
            (Kind::Modifier, 0xa5, 0x38, true, "Alt"),
        ] {
            let (mut capture, ctx) = fixture(kind);
            stroke(vk, scan, e0, false);
            finish_headless_frame(ctx.run_ui(
                egui::RawInput {
                    focused: true,
                    events: vec![Event::Key {
                        key: egui::Key::Tab,
                        physical_key: Some(egui::Key::Tab),
                        pressed: true,
                        repeat: false,
                        modifiers: egui::Modifiers::NONE,
                    }],
                    ..Default::default()
                },
                |root| {
                    // Reproduce navigation moving focus before the listening owner's filter can arm.
                    ctx.memory_mut(|memory| memory.request_focus(egui::Id::new("search")));
                    assert_eq!(capture.poll(&ctx), Some(Result::Value(expected.into())));
                    assert!(ctx.input(|input| !input
                        .events
                        .iter()
                        .any(|event| matches!(event, Event::Key { .. } | Event::Text(_)))));
                    egui::CentralPanel::default().show(root, |ui| {
                        let _ = ui.interact(
                            egui::Rect::from_min_size(egui::pos2(0.0, 0.0), egui::vec2(20.0, 20.0)),
                            capture.focus_id,
                            egui::Sense::click(),
                        );
                        let _ = ui.button("Next control");
                    });
                    assert!(
                        ctx.memory(|memory| memory.has_focus(capture.focus_id)),
                        "navigation intent was cleared"
                    );
                },
            ));
        }
        let (mut capture, ctx) = fixture(Kind::Modifier);
        stroke(0x09, 0x0f, false, false);
        finish_headless_frame(ctx.run_ui(
            egui::RawInput {
                focused: true,
                events: vec![Event::Key {
                    key: egui::Key::Tab,
                    physical_key: Some(egui::Key::Tab),
                    pressed: true,
                    repeat: false,
                    modifiers: egui::Modifiers::NONE,
                }],
                ..Default::default()
            },
            |root| {
                assert_eq!(capture.poll(&ctx), Some(Result::Unsupported));
                egui::CentralPanel::default().show(root, |ui| {
                    let _ = ui.interact(
                        egui::Rect::from_min_size(egui::pos2(0.0, 0.0), egui::vec2(20.0, 20.0)),
                        capture.focus_id,
                        egui::Sense::click(),
                    );
                    let _ = ui.button("Next control");
                });
                assert!(
                    ctx.memory(|memory| memory.has_focus(capture.focus_id)),
                    "unsupported Tab keeps capture ownership"
                );
            },
        ));
    }
    #[test]
    fn capture_matrix_ignores_opening_hold_repeats_and_cancels_without_eating_search_text() {
        let (mut capture, ctx) = fixture(Kind::Key);
        OBSERVER.with(|slot| {
            let mut slot = slot.borrow_mut();
            let observer = slot.as_mut().unwrap();
            observer.down[0x0d] = true;
            observer.blocked[0x0d] = true;
        });
        stroke(0x0d, 0x1c, false, false);
        ctx.begin_pass(egui::RawInput {
            focused: true,
            ..Default::default()
        });
        ctx.memory_mut(|memory| memory.request_focus(capture.focus_id));
        assert_eq!(
            capture.poll(&ctx),
            None,
            "opening hold/repeat is not a new assignment"
        );
        finish_headless_frame(ctx.end_pass());
        stroke(0x0d, 0x1c, false, true);
        stroke(0x0d, 0x1c, false, false);
        stroke(0x0d, 0x1c, false, false);
        ctx.begin_pass(egui::RawInput {
            focused: true,
            ..Default::default()
        });
        assert_eq!(capture.poll(&ctx), Some(Result::Value("Enter".into())));
        assert_eq!(
            capture.poll(&ctx),
            None,
            "the repeat and stroke cannot dispatch twice"
        );
        finish_headless_frame(ctx.end_pass());
        stroke(0x1b, 1, false, false);
        ctx.begin_pass(egui::RawInput {
            focused: true,
            ..Default::default()
        });
        assert_eq!(capture.poll(&ctx), Some(Result::Cancel));
        finish_headless_frame(ctx.end_pass());
        ctx.begin_pass(egui::RawInput {
            focused: true,
            events: vec![
                Event::PointerButton {
                    pos: egui::pos2(200.0, 20.0),
                    button: PointerButton::Primary,
                    pressed: true,
                    modifiers: egui::Modifiers::NONE,
                },
                Event::Text("search".into()),
            ],
            ..Default::default()
        });
        assert_eq!(capture.poll(&ctx), Some(Result::Cancel));
        assert!(ctx.input(|input| input
            .events
            .iter()
            .any(|event| matches!(event, Event::Text(text) if text == "search"))));
        finish_headless_frame(ctx.end_pass());
        ctx.begin_pass(egui::RawInput {
            focused: false,
            ..Default::default()
        });
        assert_eq!(capture.poll(&ctx), Some(Result::Cancel));
        finish_headless_frame(ctx.end_pass());
    }
    #[test]
    fn capture_matrix_does_not_assign_opening_mouse_press_or_controls_click() {
        let (mut capture, ctx) = fixture(Kind::Button);
        capture.mouse_ready = false;
        let event = |pressed| Event::PointerButton {
            pos: egui::pos2(20.0, 20.0),
            button: PointerButton::Secondary,
            pressed,
            modifiers: egui::Modifiers::NONE,
        };
        for (events, expected) in [
            (vec![event(true)], None),
            (vec![event(false)], None),
            (vec![event(true)], Some(Result::Value("MouseRight".into()))),
        ] {
            ctx.begin_pass(egui::RawInput {
                focused: true,
                events,
                ..Default::default()
            });
            ctx.memory_mut(|memory| memory.request_focus(capture.focus_id));
            assert_eq!(capture.poll(&ctx), expected);
            finish_headless_frame(ctx.end_pass());
        }
        ctx.begin_pass(egui::RawInput {
            focused: true,
            events: vec![Event::PointerButton {
                pos: egui::pos2(20.0, 130.0),
                button: PointerButton::Primary,
                pressed: true,
                modifiers: egui::Modifiers::NONE,
            }],
            ..Default::default()
        });
        ctx.memory_mut(|memory| memory.request_focus(capture.focus_id));
        assert_eq!(
            capture.poll(&ctx),
            None,
            "controls are outside the mouse listening region"
        );
        finish_headless_frame(ctx.end_pass());
    }
}
