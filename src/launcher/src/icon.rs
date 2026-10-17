//! The window / taskbar icon (dist LA19), decoded from the SAME `assets/mh_launcher.ico` that
//! build.rs embeds as the exe's icon resource, so the two cannot drift apart.
//!
//! Deliberately no image crate: the ICO directory is 16 bytes per entry and the entries this file
//! ships are 32-bit BGRA DIBs (BITMAPINFOHEADER, bottom-up, alpha already in the pixels). An entry in
//! any other form (PNG-compressed, palettised) is skipped rather than guessed at, and the window
//! simply gets no custom icon.

const ICO: &[u8] = include_bytes!("../assets/mh_launcher.ico");

fn u16le(d: &[u8], at: usize) -> Option<usize> {
    Some(u16::from_le_bytes(d.get(at..at + 2)?.try_into().ok()?) as usize)
}

fn u32le(d: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(d.get(at..at + 4)?.try_into().ok()?) as usize)
}

/// Decode the largest 32-bit DIB entry of at most `max_px` pixels square, as RGBA.
fn decode(ico: &[u8], max_px: usize) -> Option<egui::IconData> {
    if u16le(ico, 0)? != 0 || u16le(ico, 2)? != 1 {
        return None;
    }
    let count = u16le(ico, 4)?;
    let mut best: Option<(usize, usize)> = None; // (width, data offset)
    for i in 0..count {
        let e = 6 + 16 * i;
        let w = match *ico.get(e)? {
            0 => 256,
            n => n as usize,
        };
        let off = u32le(ico, e + 12)?;
        let is_dib = u32le(ico, off)? == 40 && u16le(ico, off + 14)? == 32;
        if is_dib && w <= max_px && best.is_none_or(|(bw, _)| w > bw) {
            best = Some((w, off));
        }
    }
    let (w, off) = best?;
    // BITMAPINFOHEADER: height is doubled (XOR + AND mask); 32 bpp pixels follow the 40-byte header.
    let px = ico.get(off + 40..off + 40 + w * w * 4)?;
    let mut rgba = Vec::with_capacity(w * w * 4);
    for row in (0..w).rev() {
        for p in px[row * w * 4..(row + 1) * w * 4].as_chunks::<4>().0 {
            rgba.extend_from_slice(&[p[2], p[1], p[0], p[3]]);
        }
    }
    Some(egui::IconData {
        rgba,
        width: w as u32,
        height: w as u32,
    })
}

/// 64 px is what Windows scales down for the title bar and up for the taskbar; the 64 px entry is
/// the largest the file carries.
pub fn window_icon() -> Option<egui::IconData> {
    decode(ICO, 64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shipped_icon_decodes_at_64() {
        let i = window_icon().expect("the shipped .ico has a 32-bit DIB entry");
        assert_eq!((i.width, i.height), (64, 64));
        assert_eq!(i.rgba.len(), 64 * 64 * 4);
        assert!(
            i.rgba.as_chunks::<4>().0.iter().any(|p| p[3] > 0),
            "all-transparent icon"
        );
    }

    #[test]
    fn smaller_cap_picks_smaller_entry() {
        assert_eq!(decode(ICO, 32).map(|i| i.width), Some(32));
        assert!(decode(ICO, 8).is_none());
        assert!(decode(&[0, 0], 64).is_none());
    }
}
