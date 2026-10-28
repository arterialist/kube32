//! On-board peripherals behind the capability firewall. Every call needs the
//! matching resource in the pod manifest, otherwise the host returns
//! `Error::Permission` and logs a violation:
//!
//! | functions          | resource                           | capability |
//! |--------------------|------------------------------------|------------|
//! | `display_*`        | `hardware.kube32.io/tft-display: "1"` | `DISPLAY`  |
//! | `key_read`         | `hardware.kube32.io/keyboard: "1"`    | `KEYBOARD` |
//! | `tone`             | `hardware.kube32.io/speaker: "1"`     | `SPEAKER`  |
//!
//! Colors are RGB565. The display on the reference board is 240 x 135.

use crate::abi;
use crate::Error;

/// Packs 8-bit RGB into RGB565.
pub const fn rgb565(r: u8, g: u8, b: u8) -> u32 {
    (((r as u32) & 0xF8) << 8) | (((g as u32) & 0xFC) << 3) | ((b as u32) >> 3)
}

/// Draws `text` at (x, y). The host accepts at most 64 bytes per call.
pub fn display_text(x: i32, y: i32, text: &str, fg565: u32, bg565: u32) -> Result<(), Error> {
    let len = text.len().min(64);
    let rc = unsafe { abi::k32_display_text(x, y, text.as_ptr(), len as u32, fg565, bg565) };
    Error::from_rc(rc).map(|_| ())
}

pub fn display_rect(x: i32, y: i32, w: i32, h: i32, color565: u32) -> Result<(), Error> {
    let rc = unsafe { abi::k32_display_rect(x, y, w, h, color565) };
    Error::from_rc(rc).map(|_| ())
}

/// A horizontal progress bar filled to `pct` percent.
pub fn display_bar(x: i32, y: i32, w: i32, h: i32, pct: u32, fg565: u32, bg565: u32) -> Result<(), Error> {
    let rc = unsafe { abi::k32_display_bar(x, y, w, h, pct, fg565, bg565) };
    Error::from_rc(rc).map(|_| ())
}

/// The next key from the keyboard queue as an ASCII code (`\n` for Enter,
/// `\x08` for Backspace). `Ok(None)` when the queue is empty.
pub fn key_read() -> Result<Option<u8>, Error> {
    let rc = unsafe { abi::k32_key_read() };
    if rc == crate::ERR_AGAIN {
        return Ok(None);
    }
    Error::from_rc(rc).map(|k| Some(k as u8))
}

/// Plays a tone. The host clamps `duration_ms` to 2000 and `volume_pct` to 100.
pub fn tone(freq_hz: u32, duration_ms: u32, volume_pct: u32) -> Result<(), Error> {
    let rc = unsafe { abi::k32_tone(freq_hz, duration_ms, volume_pct) };
    Error::from_rc(rc).map(|_| ())
}
