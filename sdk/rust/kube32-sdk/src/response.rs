//! Streaming responses through `k32_resp_begin` / `k32_resp_header` /
//! `k32_resp_write` / `k32_resp_end`.
//!
//! The host streams the body with chunked transfer encoding and buffers the
//! writes itself, so a response never needs a buffer on the guest side. The
//! order the host enforces: `begin` once, headers before the first write,
//! writes, `end`.

use crate::abi;

/// An open response. Dropping it without `end()` still ends it.
pub struct Response {
    status: i32,
    ended: bool,
    write_failed: bool,
}

impl Response {
    /// Sends the status line and content type. Only the first call per
    /// request has an effect on the host.
    pub fn begin(status: i32, content_type: &str) -> Response {
        unsafe { abi::k32_resp_begin(status, content_type.as_ptr(), content_type.len() as u32) };
        Response { status, ended: false, write_failed: false }
    }

    pub fn json(status: i32) -> Response {
        Self::begin(status, "application/json")
    }

    pub fn text(status: i32) -> Response {
        Self::begin(status, "text/plain; charset=utf-8")
    }

    pub fn html(status: i32) -> Response {
        Self::begin(status, "text/html; charset=utf-8")
    }

    /// Adds a header. Must come before the first `write`; the host ignores
    /// names longer than 64 bytes and values longer than 256.
    pub fn header(&mut self, name: &str, value: &str) -> &mut Self {
        unsafe { abi::k32_resp_header(name.as_ptr(), name.len() as u32, value.as_ptr(), value.len() as u32) };
        self
    }

    /// Appends body bytes. A failed write (client gone) is remembered in
    /// `write_failed()`; later writes are still attempted.
    pub fn write(&mut self, data: &[u8]) -> &mut Self {
        if !data.is_empty() && !self.ended {
            let rc = unsafe { abi::k32_resp_write(data.as_ptr(), data.len() as u32) };
            if rc < 0 {
                self.write_failed = true;
            }
        }
        self
    }

    pub fn write_str(&mut self, s: &str) -> &mut Self {
        self.write(s.as_bytes())
    }

    /// Writes a decimal number without pulling in `core::fmt`.
    pub fn write_u64(&mut self, v: u64) -> &mut Self {
        let mut buf = [0u8; 20];
        let s = fmt_u64(v, &mut buf);
        self.write(s)
    }

    pub fn write_i64(&mut self, v: i64) -> &mut Self {
        if v < 0 {
            self.write(b"-");
            self.write_u64(v.unsigned_abs())
        } else {
            self.write_u64(v as u64)
        }
    }

    pub fn write_u32(&mut self, v: u32) -> &mut Self {
        self.write_u64(v as u64)
    }

    /// Writes `s` as a quoted JSON string, escaping `"`, `\` and control characters.
    pub fn write_json_str(&mut self, s: &str) -> &mut Self {
        self.write(b"\"");
        let bytes = s.as_bytes();
        let mut start = 0;
        for (i, &b) in bytes.iter().enumerate() {
            let esc: Option<&[u8]> = match b {
                b'"' => Some(b"\\\""),
                b'\\' => Some(b"\\\\"),
                b'\n' => Some(b"\\n"),
                b'\r' => Some(b"\\r"),
                b'\t' => Some(b"\\t"),
                0..=0x1f => Some(b"\\u0000"), // other control characters: lossy but valid JSON
                _ => None,
            };
            if let Some(e) = esc {
                self.write(&bytes[start..i]);
                self.write(e);
                start = i + 1;
            }
        }
        self.write(&bytes[start..]);
        self.write(b"\"")
    }

    pub fn write_failed(&self) -> bool {
        self.write_failed
    }

    pub fn status(&self) -> i32 {
        self.status
    }

    /// Finishes the response and returns the status code for `k32_handle`.
    pub fn end(mut self) -> i32 {
        self.finish();
        self.status
    }

    fn finish(&mut self) {
        if !self.ended {
            self.ended = true;
            unsafe { abi::k32_resp_end() };
        }
    }
}

impl Drop for Response {
    fn drop(&mut self) {
        self.finish();
    }
}

/// `write!(resp, "{}", n)` support. Formatting machinery costs a few KB of
/// code; the `write_*` helpers above are cheaper.
impl core::fmt::Write for Response {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.write(s.as_bytes());
        Ok(())
    }
}

/// Formats `v` into `buf` (at least 20 bytes) and returns the digits.
pub fn fmt_u64(mut v: u64, buf: &mut [u8; 20]) -> &[u8] {
    let mut i = buf.len();
    loop {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    &buf[i..]
}

#[cfg(test)]
mod tests {
    use super::fmt_u64;

    #[test]
    fn formats_numbers() {
        let mut b = [0u8; 20];
        assert_eq!(fmt_u64(0, &mut b), b"0");
        assert_eq!(fmt_u64(42, &mut b), b"42");
        assert_eq!(fmt_u64(u64::MAX, &mut b), b"18446744073709551615");
    }
}
