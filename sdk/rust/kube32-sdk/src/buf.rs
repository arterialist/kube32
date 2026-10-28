//! A fixed-capacity text buffer for event payloads and log lines, without
//! `core::fmt` (which costs several KB of wasm) and without an allocator.

use crate::response::fmt_u64;

/// Up to `N` bytes of UTF-8 text. A push that does not fit appends nothing
/// and marks the buffer as overflowed, so the contents stay valid text.
pub struct Buf<const N: usize> {
    data: [u8; N],
    len: usize,
    overflowed: bool,
}

impl<const N: usize> Buf<N> {
    pub const fn new() -> Self {
        Buf { data: [0; N], len: 0, overflowed: false }
    }

    fn push_bytes(&mut self, b: &[u8]) -> &mut Self {
        if b.len() > N - self.len {
            self.overflowed = true;
        } else {
            self.data[self.len..self.len + b.len()].copy_from_slice(b);
            self.len += b.len();
        }
        self
    }

    pub fn push_str(&mut self, s: &str) -> &mut Self {
        self.push_bytes(s.as_bytes())
    }

    pub fn push_u64(&mut self, v: u64) -> &mut Self {
        let mut digits = [0u8; 20];
        let d = fmt_u64(v, &mut digits);
        self.push_bytes(d)
    }

    pub fn push_i64(&mut self, v: i64) -> &mut Self {
        if v < 0 {
            self.push_bytes(b"-");
        }
        self.push_u64(v.unsigned_abs())
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.len]
    }

    pub fn as_str(&self) -> &str {
        // Only whole &str values and ASCII digits are ever appended.
        unsafe { core::str::from_utf8_unchecked(self.as_bytes()) }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// `true` when a push did not fit and was dropped.
    pub fn overflowed(&self) -> bool {
        self.overflowed
    }

    pub fn clear(&mut self) {
        self.len = 0;
        self.overflowed = false;
    }
}

impl<const N: usize> Default for Buf<N> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::Buf;

    #[test]
    fn pushes_and_overflow() {
        let mut b = Buf::<12>::new();
        b.push_str("v=").push_i64(-42).push_str(";");
        assert_eq!(b.as_str(), "v=-42;");
        assert!(!b.overflowed());
        b.push_str("toolong!");
        assert_eq!(b.as_str(), "v=-42;");
        assert!(b.overflowed());
        b.push_u64(123456);
        assert_eq!(b.as_str(), "v=-42;123456");
        b.clear();
        assert!(b.is_empty() && !b.overflowed());
    }
}
