//! The resolution list the Display tab offers for `[video] resolution` (RES3).
//!
//! A line-for-line Rust copy of the game's generator (`src/mh_dll/mh/gfx/res_list.cpp`): the launcher
//! writes a size, the DLL looks it up in the list IT generates for the same desktop, so the two must agree
//! entry for entry. The test vectors below are the ones `net_selftest.exe reslisttest` asserts; change both
//! together. The rules (docs/view-scaling.md):
//!
//! * the three stock sizes lead; then the fixed `COMMON` sizes (RES24: 800x600 .. 2048x1536 in 4:3, 1280x720 ..
//!   3840x2160 in 16:9) that fit the desktop and `valid_size`; also the desktop divided by k in {1, 1.25, 1.5, 2, 2.5, 3, 4}, EXACT (RES18:
//!   the game renders the size rounded up to the 32 px tile grid and the presenter crops it, so a 1080p desktop
//!   gets 1920x1080, not 1920x1056), then a 4:3 family from the same heights (a size two sources both give keeps the earlier source's label); every size is at least 640x480, never
//!   larger than the desktop, at most 2048x1600 (the view must fit the sprite window of the 64-tile
//!   smallest map, see `CAP_W`), never wider than 16:9 (a wider desktop gets 16:9 sizes of its own
//!   height, with side bars) and, rounded up to the grid, within the cap and 4096 tiles; after the stock three: every 4:3 size, then every 16:9
//!   size, then the rest (`aspect_group`), each group ascending by area;
//! * Auto is the desktop divided by the Windows display scale, exact, never below 1024x768 and never
//!   above the desktop or the cap.
//!
//! The row's stored value is `stock` (the default), `auto` or `WxH`; never a list index.

use super::model::ProviderOption;

pub const MIN_W: i32 = 640;
pub const MIN_H: i32 = 480;
/// The smallest map the game loads is 64x64 tiles (all 31 shipped planet maps are 64, 128 or 256 square), so
/// `big` is 2048 px there. Two limits, the tighter one wins:
/// * tile window: the strategic view repeats the terrain once the viewport (W - 160 by H - 32) plus two
///   tiles covers the map: W <= 2112 and H <= 1984;
/// * sprite window: sprite placement uses the biased window `[-K, big-K)` (`gfx/wrap_window.h`), correct while
///   viewport + K <= big: with K = 160 / 256, W <= 2048 and H <= 1824.
pub const MIN_MAP_TILES: i32 = 64;
/// The biased sprite window's bias (px); see `src/mh_dll/mh/gfx/wrap_window.h` for the measurement.
pub const K_X: i32 = 160;
pub const K_Y: i32 = 256;
const TILE_WIN_W: i32 = 160 + (MIN_MAP_TILES - 3) * 32;
const TILE_WIN_H: i32 = 32 + (MIN_MAP_TILES - 3) * 32;
const SPRITE_WIN_W: i32 = 160 + MIN_MAP_TILES * 32 - K_X;
const SPRITE_WIN_H: i32 = 32 + MIN_MAP_TILES * 32 - K_Y;
pub const NOWRAP_W: i32 = if TILE_WIN_W < SPRITE_WIN_W {
    TILE_WIN_W
} else {
    SPRITE_WIN_W
};
pub const NOWRAP_H: i32 = if TILE_WIN_H < SPRITE_WIN_H {
    TILE_WIN_H
} else {
    SPRITE_WIN_H
};
pub const CAP_W: i32 = NOWRAP_W;
/// 1600 is the older CPU-cost bound and is the tighter of the two heights.
pub const CAP_H: i32 = if 1600 < NOWRAP_H { 1600 } else { NOWRAP_H };
pub const AUTO_MIN_W: i32 = 1024;
pub const AUTO_MIN_H: i32 = 768;
pub const MAX_TILES: i32 = 4096;

const K100: [i32; 7] = [100, 125, 150, 200, 250, 300, 400];

/// RES24: common display sizes, 4:3 then 16:9, each offered when it fits the desktop and `valid_size` holds. 800x600 is
/// a real 800x600 (renders 800x608, the presenter crops); the stock 800x576 entry stays separate.
const COMMON: [(i32, i32); 15] = [
    (800, 600),
    (1024, 768),
    (1152, 864),
    (1280, 960),
    (1400, 1050),
    (1600, 1200),
    (1920, 1440),
    (2048, 1536),
    (1280, 720),
    (1366, 768),
    (1600, 900),
    (1920, 1080),
    (2560, 1440),
    (3200, 1800),
    (3840, 2160),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Stock,
    Native,
    FourThree,
    Requested,
    /// RES24: one of the fixed common 4:3 / 16:9 sizes (`COMMON`).
    Common,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    pub w: i32,
    pub h: i32,
    pub kind: Kind,
}

/// What the machine reports: the desktop's pixel size and the Windows display scale in percent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Desktop {
    pub w: i32,
    pub h: i32,
    pub scale_pct: i32,
}

/// The frame the game renders for a `v`-pixel session size (RES18).
pub fn ceil32(v: i32) -> i32 {
    (v + 31) & !31
}

/// The widest 16:9 width for a height, rounded UP: a 1366x768 panel is "16:9" (768 * 16 / 9 = 1365.3).
fn wide169(h: i32) -> i32 {
    (h * 16 + 8) / 9
}

/// Is `w` x `h` no wider than 16:9, with the width allowed to round up (1366x768 qualifies)?
pub fn within_16x9(w: i32, h: i32) -> bool {
    w <= wide169(h)
}

/// A size the game can show (RES18): at least 640x480, and its `ceil32` render within the cap and 4096 tiles.
pub fn valid_size(w: i32, h: i32) -> bool {
    if w < MIN_W || h < MIN_H {
        return false;
    }
    let (rw, rh) = (ceil32(w), ceil32(h));
    if rw > CAP_W || rh > CAP_H {
        return false;
    }
    (rw / 32) * (rh / 32) <= MAX_TILES
}

fn area(e: &Entry) -> i64 {
    i64::from(e.w) * i64::from(e.h)
}

/// The tail's grouping after the stock three: 0 = 4:3, 1 = 16:9, 2 = everything else. Sizes are exact since
/// RES18; the tests keep the old slack (a hand-typed or legacy 32-snapped size): 4:3 when |3w - 4h| <= 96;
/// 16:9 when -288 <= 9w - 16h <= 496.
pub fn aspect_group(w: i32, h: i32) -> i32 {
    let d43 = 3 * w - 4 * h;
    if (-96..=96).contains(&d43) {
        return 0;
    }
    let d169 = 9 * w - 16 * h;
    if (-288..=496).contains(&d169) {
        return 1;
    }
    2
}

/// The tail's sort key: group, then area, then width.
fn tail_key(e: &Entry) -> (i32, i64, i32) {
    (aspect_group(e.w, e.h), area(e), e.w)
}

fn add(list: &mut Vec<Entry>, w: i32, h: i32, kind: Kind) {
    if !list.iter().any(|e| e.w == w && e.h == h) {
        list.push(Entry { w, h, kind });
    }
}

/// The list for a desktop of `dw` x `dh`: the stock three first, then every 4:3 size, every 16:9 size and the
/// rest, each group ascending by area.
pub fn generate(dw: i32, dh: i32) -> Vec<Entry> {
    let mut out = Vec::new();
    add(&mut out, 640, 480, Kind::Stock);
    add(&mut out, 800, 576, Kind::Stock); // the engine's "800x600" renders 576 rows
    add(&mut out, 1024, 768, Kind::Stock);
    if dw <= 0 || dh <= 0 {
        return out;
    }
    for k in K100 {
        let h = dh * 100 / k; // RES18: exact, not snapped to the tile grid
        let mut w = dw * 100 / k;
        if w > wide169(h) {
            w = h * 16 / 9; // a wider desktop: 16:9 of its height, side bars
        }
        if valid_size(w, h) && w <= dw && h <= dh {
            add(&mut out, w, h, Kind::Native);
        }
    }
    for k in K100 {
        let h = dh * 100 / k;
        let w = (h * 4 + 1) / 3; // nearest whole pixel
        if valid_size(w, h) && w <= dw && h <= dh {
            add(&mut out, w, h, Kind::FourThree);
        }
    }
    for (w, h) in COMMON {
        if valid_size(w, h) && w <= dw && h <= dh {
            add(&mut out, w, h, Kind::Common);
        }
    }
    out[3..].sort_by_key(tail_key);
    out
}

/// Auto: desktop / display scale, exact, then clamped (see the module comment).
pub fn auto_size(dw: i32, dh: i32, scale_pct: i32) -> Entry {
    let pct = if scale_pct <= 0 { 100 } else { scale_pct };
    let mut w = (i64::from(dw) * 100 / i64::from(pct)) as i32;
    let mut h = (i64::from(dh) * 100 / i64::from(pct)) as i32;
    if w > wide169(h) {
        w = h * 16 / 9; // never wider than 16:9
    }
    w = w.max(AUTO_MIN_W); // never below the largest stock UI ...
    h = h.max(AUTO_MIN_H);
    if dw > 0 && w > dw {
        w = dw; // ... and never above the desktop (this one wins)
    }
    if dh > 0 && h > dh {
        h = dh;
    }
    if ceil32(w) > CAP_W || ceil32(h) > CAP_H {
        // over the cap: the largest generated "native" entry (it keeps the desktop's aspect)
        let best = generate(dw, dh)
            .into_iter()
            .skip(3)
            .filter(|e| e.kind == Kind::Native)
            .max_by_key(area);
        if let Some(b) = best {
            return b;
        }
        w = CAP_W;
        h = CAP_H;
    }
    Entry {
        w: w.max(MIN_W),
        h: h.max(MIN_H),
        kind: Kind::Native,
    }
}

/// "1920x1056" -> Some((1920, 1056)); `x` or `X`, digits only.
pub fn parse_size(s: &str) -> Option<(i32, i32)> {
    let (a, b) = s.split_once(['x', 'X'])?;
    let num = |t: &str| -> Option<i32> {
        if t.is_empty() || t.len() > 5 || !t.bytes().all(|c| c.is_ascii_digit()) {
            return None;
        }
        t.parse().ok()
    };
    Some((num(a)?, num(b)?))
}

fn scale_text(s100: i32) -> String {
    let n = (s100 + 50) / 100;
    let dev = (s100 - n * 100).abs();
    if n >= 1 && dev <= 3 * n {
        return if n == 1 {
            "1:1".to_string()
        } else {
            format!("{n}x")
        };
    }
    let frac = s100 % 100;
    let mut t = format!("{}.{}", s100 / 100, frac / 10);
    if frac % 10 != 0 {
        t.push_str(&(frac % 10).to_string());
    }
    t.push('x');
    t
}

/// "1920x1056 (1:1, sharpest)", "1280x704 (1.5x)", "1408x1056 (4:3, 1:1)", "640x480 (classic)".
pub fn describe(e: &Entry, dw: i32, dh: i32) -> String {
    let mut t = format!("{}x{} (", e.w, e.h);
    if e.kind == Kind::Stock {
        t.push_str("classic");
    } else {
        let sx = if dw > 0 {
            f64::from(dw) / f64::from(e.w)
        } else {
            1.0
        };
        let sy = if dh > 0 {
            f64::from(dh) / f64::from(e.h)
        } else {
            1.0
        };
        let s100 = (sx.min(sy) * 100.0 + 0.5) as i32;
        if e.kind == Kind::Common {
            // RES24: a fixed common size is labelled by its aspect, not by a scale
            t.push_str(match aspect_group(e.w, e.h) {
                0 => "4:3",
                1 => "16:9",
                _ => "other",
            });
            t.push(')');
            return t;
        }
        if e.kind == Kind::FourThree {
            t.push_str("4:3, ");
        }
        t.push_str(&scale_text(s100));
        if e.kind == Kind::Native && (97..=103).contains(&s100) {
            t.push_str(", sharpest");
        }
    }
    t.push(')');
    t
}

/// The desktop the game would see at start: the primary display's current pixel size and the system DPI as a
/// percentage of 96. `None` where it cannot be read (no display, a service session).
#[cfg(windows)]
pub fn read_desktop() -> Option<Desktop> {
    use windows_sys::Win32::Graphics::Gdi::{
        EnumDisplaySettingsW, DEVMODEW, ENUM_CURRENT_SETTINGS,
    };
    use windows_sys::Win32::UI::HiDpi::{
        GetDpiForSystem, SetThreadDpiAwarenessContext, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE,
    };
    // SAFETY: plain Win32 queries on a zeroed, size-stamped struct; the awareness context is restored.
    unsafe {
        let mut dm: DEVMODEW = std::mem::zeroed();
        dm.dmSize = std::mem::size_of::<DEVMODEW>() as u16;
        if EnumDisplaySettingsW(std::ptr::null(), ENUM_CURRENT_SETTINGS, &mut dm) == 0 {
            return None;
        }
        // An unaware thread is told 96 DPI; ask as a system-aware one, which is what the game process is.
        let old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        let dpi = GetDpiForSystem();
        if !old.is_null() {
            SetThreadDpiAwarenessContext(old);
        }
        let (w, h) = (dm.dmPelsWidth as i32, dm.dmPelsHeight as i32);
        if w <= 0 || h <= 0 {
            return None;
        }
        let pct = if dpi >= 96 {
            (dpi * 100 / 96) as i32
        } else {
            100
        };
        Some(Desktop {
            w,
            h,
            scale_pct: pct,
        })
    }
}

#[cfg(not(windows))]
pub fn read_desktop() -> Option<Desktop> {
    None
}

/// The Resolution row's options: the classic-only choice (the default), Auto (with the size it resolves
/// to), then every generated size. Values are what the ini stores: `stock`, `auto`, `WxH`.
pub fn options(
    desk: Option<Desktop>,
    auto_label: &str,
    classic_label: &str,
) -> Vec<ProviderOption> {
    let mut out = vec![ProviderOption {
        value: "stock".to_string(),
        label: classic_label.to_string(),
    }];
    match desk {
        Some(d) => {
            let a = auto_size(d.w, d.h, d.scale_pct);
            out.push(ProviderOption {
                value: "auto".to_string(),
                label: format!("{auto_label} ({}x{})", a.w, a.h),
            });
            let mut list = generate(d.w, d.h);
            // A size Auto picks is always selectable by value too; keep the list in step with the game's.
            if !list.iter().any(|e| e.w == a.w && e.h == a.h) {
                list.push(a);
                list[3..].sort_by_key(tail_key);
            }
            for e in list {
                out.push(ProviderOption {
                    value: format!("{}x{}", e.w, e.h),
                    label: describe(&e, d.w, d.h),
                });
            }
        }
        None => out.push(ProviderOption {
            value: "auto".to_string(),
            label: auto_label.to_string(),
        }),
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sizes(dw: i32, dh: i32) -> Vec<(i32, i32)> {
        generate(dw, dh).iter().map(|e| (e.w, e.h)).collect()
    }

    // The vectors are the ones res_list_selftest.cpp asserts.
    #[test]
    fn lists_match_the_game_generator() {
        assert_eq!(
            sizes(1920, 1080),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (720, 540),
                (800, 600),
                (960, 720),
                (1152, 864),
                (1280, 960),
                (1400, 1050),
                (1440, 1080),
                (960, 540),
                (1280, 720),
                (1366, 768),
                (1536, 864),
                (1600, 900),
                (1920, 1080)
            ]
        );
        assert_eq!(
            sizes(2560, 1440),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (768, 576),
                (800, 600),
                (960, 720),
                (1152, 864),
                (1280, 960),
                (1400, 1050),
                (1536, 1152),
                (1600, 1200),
                (1920, 1440),
                (853, 480),
                (1024, 576),
                (1280, 720),
                (1366, 768),
                (1600, 900),
                (1706, 960),
                (1920, 1080),
                (2048, 1152)
            ]
        );
        assert_eq!(
            sizes(3840, 2160),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (720, 540),
                (800, 600),
                (960, 720),
                (1152, 864),
                (1280, 960),
                (1400, 1050),
                (1440, 1080),
                (1600, 1200),
                (1920, 1440),
                (2048, 1536),
                (960, 540),
                (1280, 720),
                (1366, 768),
                (1536, 864),
                (1600, 900),
                (1920, 1080)
            ]
        );
        assert_eq!(
            sizes(1920, 1200),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (800, 600),
                (1067, 800),
                (1152, 864),
                (1280, 960),
                (1400, 1050),
                (1600, 1200),
                (1280, 720),
                (1366, 768),
                (1600, 900),
                (1920, 1080),
                (768, 480),
                (960, 600),
                (1280, 800),
                (1536, 960),
                (1920, 1200)
            ]
        );
        assert_eq!(
            sizes(2560, 1600),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (711, 533),
                (800, 600),
                (853, 640),
                (1067, 800),
                (1152, 864),
                (1280, 960),
                (1400, 1050),
                (1421, 1066),
                (1600, 1200),
                (1707, 1280),
                (1920, 1440),
                (2048, 1536),
                (1280, 720),
                (1366, 768),
                (1600, 900),
                (1920, 1080),
                (853, 533),
                (1024, 640),
                (1280, 800),
                (1706, 1066),
                (2048, 1280)
            ]
        );
        // ultrawide: 16:9 sizes of the desktop's own height (side bars) -- the same list as 2560x1440
        for (dw, dh) in [(3440, 1440), (5120, 1440)] {
            assert_eq!(sizes(dw, dh), sizes(2560, 1440), "{dw}x{dh}");
        }
        assert_eq!(
            sizes(1366, 768),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (683, 512),
                (800, 600),
                (819, 614),
                (910, 512),
                (1092, 614),
                (1280, 720),
                (1366, 768)
            ]
        );
        assert_eq!(
            sizes(1280, 1024),
            [
                (640, 480),
                (800, 576),
                (1024, 768),
                (683, 512),
                (800, 600),
                (909, 682),
                (1092, 819),
                (1152, 864),
                (1280, 960),
                (1280, 720),
                (640, 512),
                (853, 682),
                (1024, 819),
                (1280, 1024)
            ]
        );
    }

    #[test]
    fn auto_picks_match_the_game_generator() {
        let a = |dw, dh, p| {
            let e = auto_size(dw, dh, p);
            (e.w, e.h)
        };
        assert_eq!(a(1920, 1080, 100), (1920, 1080));
        assert_eq!(a(1920, 1080, 150), (1280, 768)); // 1280x720 would be below 1024x768 in height
        assert_eq!(a(2560, 1440, 100), (2048, 1152)); // 2560 is over the cap: the largest generated entry
        assert_eq!(a(2560, 1440, 125), (2048, 1152)); // 2560 / 1.25 = 2048: exactly the cap
        assert_eq!(a(2560, 1440, 200), (1280, 768));
        assert_eq!(a(2560, 1440, 150), (1706, 960));
        assert_eq!(a(3840, 2160, 100), (1920, 1080)); // over the cap: the largest generated entry
        assert_eq!(a(3840, 2160, 150), (1920, 1080));
        assert_eq!(a(2560, 1600, 100), (2048, 1280));
        assert_eq!(a(3440, 1440, 100), (2048, 1152)); // ultrawide: 16:9 of its own height
        assert_eq!(a(5120, 1440, 100), (2048, 1152));
        assert_eq!(a(3840, 2160, 200), (1920, 1080));
        assert_eq!(a(1920, 1200, 100), (1920, 1200));
        assert_eq!(a(1366, 768, 100), (1366, 768));
        assert_eq!(a(1280, 1024, 100), (1280, 1024));
        assert_eq!(a(1280, 720, 100), (1280, 720)); // never above the desktop beats never below 1024x768
        assert_eq!(a(5120, 2880, 100), (2048, 1152));
        assert_eq!(a(1920, 1080, 0), (1920, 1080)); // a bad scale reads as 100%
    }

    #[test]
    fn labels_match_the_game_generator() {
        let d = |e: Entry, dw, dh| describe(&e, dw, dh);
        let e = |w, h, kind| Entry { w, h, kind };
        assert_eq!(
            d(e(1920, 1080, Kind::Native), 1920, 1080),
            "1920x1080 (1:1, sharpest)"
        );
        assert_eq!(
            d(e(1920, 1056, Kind::Native), 1920, 1080),
            "1920x1056 (1:1, sharpest)" // a legacy size
        );
        assert_eq!(d(e(1280, 720, Kind::Native), 1920, 1080), "1280x720 (1.5x)");
        assert_eq!(d(e(960, 540, Kind::Native), 1920, 1080), "960x540 (2x)");
        assert_eq!(d(e(1536, 864, Kind::Native), 3840, 2160), "1536x864 (2.5x)");
        assert_eq!(
            d(e(2048, 1152, Kind::Native), 2560, 1440),
            "2048x1152 (1.25x)" // no longer offered (over the cap) but the label rule is unchanged
        );
        assert_eq!(
            d(e(1408, 1056, Kind::FourThree), 1920, 1080),
            "1408x1056 (4:3, 1:1)"
        );
        assert_eq!(d(e(640, 480, Kind::Stock), 1920, 1080), "640x480 (classic)");
        assert_eq!(d(e(1600, 900, Kind::Common), 2560, 1440), "1600x900 (16:9)");
        assert_eq!(
            d(e(1400, 1050, Kind::Common), 2560, 1440),
            "1400x1050 (4:3)"
        );
    }

    #[test]
    fn sizes_parse_and_validate_like_the_game() {
        assert_eq!(parse_size("1920x1056"), Some((1920, 1056)));
        assert_eq!(parse_size("1280X704"), Some((1280, 704)));
        for bad in ["auto", "", "1920x1056 ", "1920x", "x1056", "stock"] {
            assert_eq!(parse_size(bad), None, "{bad:?}");
        }
        assert!(valid_size(640, 480) && valid_size(2048, 1600));
        // RES18: any size whose ceil32 render fits (1920x1080 renders 1920x1088)
        assert!(valid_size(1920, 1080) && valid_size(1366, 768) && valid_size(2041, 1081));
        assert!(!valid_size(2144, 1440) && !valid_size(608, 480) && !valid_size(640, 479));
        assert!(!valid_size(2049, 1152) && !valid_size(2048, 1601));
        assert_eq!((ceil32(1080), ceil32(1088), ceil32(1)), (1088, 1088, 32));
        assert!(
            !valid_size(2560, 1440) && !valid_size(2112, 1184) && !valid_size(2080, 1152),
            "past the sprite window of a 64-tile map"
        );
        assert!(!valid_size(3840, 2144));
        assert_eq!((CAP_W, CAP_H), (2048, 1600));
        assert_eq!((NOWRAP_W, NOWRAP_H), (2048, 1824));
        assert_eq!((CAP_W - 160) + K_X, MIN_MAP_TILES * 32);
        assert!(within_16x9(1920, 1080) && within_16x9(1366, 768) && within_16x9(2048, 1152));
        assert!(within_16x9(1440, 1080));
        assert!(!within_16x9(1696, 704) && !within_16x9(2560, 1080) && !within_16x9(1920, 1056));
    }

    #[test]
    fn the_row_offers_classic_only_first_then_auto_then_the_list() {
        let o = options(
            Some(Desktop {
                w: 1920,
                h: 1080,
                scale_pct: 100,
            }),
            "Auto",
            "Classic sizes only",
        );
        assert_eq!(o[0].value, "stock");
        assert_eq!(o[0].label, "Classic sizes only");
        assert_eq!(o[1].value, "auto");
        assert_eq!(o[1].label, "Auto (1920x1080)");
        assert_eq!(o[2].value, "640x480");
        assert_eq!(o[5].value, "720x540"); // the generated tail starts with the 4:3 sizes
        assert_eq!(o.last().unwrap().value, "1920x1080"); // and ends with the 16:9 ones here
        assert!(o
            .iter()
            .any(|x| x.value == "1280x720" && x.label == "1280x720 (1.5x)"));
        // Without a readable desktop: classic and Auto only -- never an empty row.
        let n = options(None, "Auto", "Classic sizes only");
        assert_eq!(n.len(), 2);
        assert_eq!(
            (n[0].value.as_str(), n[1].value.as_str()),
            ("stock", "auto")
        );
    }

    #[test]
    fn auto_at_a_scale_the_list_lacks_is_still_selectable() {
        // Auto at 175% on 1080p is 1024x768 (the stock floor wins); whatever Auto picks must be selectable by value.
        let o = options(
            Some(Desktop {
                w: 1920,
                h: 1080,
                scale_pct: 175,
            }),
            "Auto",
            "Classic",
        );
        let auto = auto_size(1920, 1080, 175);
        assert!(o
            .iter()
            .any(|x| x.value == format!("{}x{}", auto.w, auto.h)));
    }
}
