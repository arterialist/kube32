//! The request block the host hands to `k32_handle`.
//!
//! Encoding (from `k32_app_abi.h`):
//!
//! ```text
//! METHOD SP PATH[?QUERY] LF
//! (Header-Name ":" SP value LF)*
//! LF
//! body bytes
//! ```
//!
//! Only four headers are forwarded: `Cookie`, `Content-Type`, `X-Request-Id`
//! and `X-Forwarded-For`. Everything borrows from the block; nothing is
//! copied or allocated.
//!
//! The request line and the header lines are parsed as bytes and must be
//! ASCII: a request line with other bytes is rejected (the generated
//! `k32_handle` answers 400), a header line with other bytes is ignored.
//! Clients percent-encode paths and queries, so this only refuses requests
//! that were malformed already. Checking for ASCII instead of validating
//! UTF-8 keeps the parser to a few hundred bytes of wasm.

/// A parsed request. All string fields are ASCII and empty when absent.
#[derive(Clone, Copy, Debug)]
pub struct Request<'a> {
    pub method: &'a str,
    /// Path without the query string, e.g. `/api/items`. Still percent-encoded.
    pub path: &'a str,
    /// Raw query string after `?`, without the `?`. Percent-encoded.
    pub query: &'a str,
    /// The `Cookie` header, e.g. `sid=abc; theme=dark`.
    pub cookie: &'a str,
    pub content_type: &'a str,
    pub request_id: &'a str,
    /// The client address the node saw.
    pub forwarded_for: &'a str,
    pub body: &'a [u8],
}

#[inline]
fn find(hay: &[u8], needle: u8) -> Option<usize> {
    hay.iter().position(|&b| b == needle)
}

/// `b` must be ASCII (checked by every caller): ASCII is valid UTF-8 and
/// every byte offset into it is a character boundary.
#[inline]
fn ascii(b: &[u8]) -> &str {
    debug_assert!(b.is_ascii());
    unsafe { core::str::from_utf8_unchecked(b) }
}

/// Splits `s` at the first `sep`: `(before, after)`, or `(s, "")` without one.
#[inline]
fn split_once(s: &[u8], sep: u8) -> (&[u8], &[u8]) {
    match find(s, sep) {
        Some(i) => (&s[..i], &s[i + 1..]),
        None => (s, &[]),
    }
}

fn trim_spaces(mut s: &[u8]) -> &[u8] {
    while let [b' ', rest @ ..] = s {
        s = rest;
    }
    while let [rest @ .., b' '] = s {
        s = rest;
    }
    s
}

/// Looks `name` up in `list`, a sequence of `key=value` items separated by
/// `sep` (query strings, cookie headers). `list` is ASCII.
fn lookup<'a>(list: &'a str, sep: u8, name: &str) -> Option<&'a str> {
    let mut rest = list.as_bytes();
    while !rest.is_empty() {
        let (item, tail) = split_once(rest, sep);
        rest = tail;
        let (k, v) = split_once(trim_spaces(item), b'=');
        if !k.is_empty() && k == name.as_bytes() {
            return Some(ascii(v));
        }
    }
    None
}

impl<'a> Request<'a> {
    /// Parses a request block. Returns `None` when the request line is missing,
    /// has no method, or is not ASCII.
    pub fn parse(block: &'a [u8]) -> Option<Request<'a>> {
        let nl = find(block, b'\n')?;
        let line = &block[..nl];
        if !line.is_ascii() {
            return None;
        }
        let (method, target) = split_once(line, b' ');
        if method.is_empty() {
            return None;
        }
        let (path, query) = split_once(target, b'?');
        let mut req = Request {
            method: ascii(method),
            path: ascii(path),
            query: ascii(query),
            cookie: "",
            content_type: "",
            request_id: "",
            forwarded_for: "",
            body: &[],
        };
        let mut rest = &block[nl + 1..];
        loop {
            let Some(end) = find(rest, b'\n') else {
                // No terminating blank line: no body.
                return Some(req);
            };
            let hdr = &rest[..end];
            rest = &rest[end + 1..];
            if hdr.is_empty() {
                req.body = rest;
                return Some(req);
            }
            if !hdr.is_ascii() {
                continue;
            }
            let Some(colon) = find(hdr, b':') else { continue };
            let name = &hdr[..colon];
            let value = ascii(trim_spaces(&hdr[colon + 1..]));
            if name.eq_ignore_ascii_case(b"Cookie") {
                req.cookie = value;
            } else if name.eq_ignore_ascii_case(b"Content-Type") {
                req.content_type = value;
            } else if name.eq_ignore_ascii_case(b"X-Request-Id") {
                req.request_id = value;
            } else if name.eq_ignore_ascii_case(b"X-Forwarded-For") {
                req.forwarded_for = value;
            }
        }
    }

    /// `true` when the method and the path (without query) match exactly.
    pub fn is(&self, method: &str, path: &str) -> bool {
        self.method == method && self.path == path
    }

    /// The raw (still percent-encoded) value of a query parameter. An empty
    /// string for `?flag` or `?flag=`.
    pub fn query_param(&self, name: &str) -> Option<&'a str> {
        lookup(self.query, b'&', name)
    }

    /// The value of a cookie from the `Cookie` header (`a=1; b=2`).
    pub fn cookie(&self, name: &str) -> Option<&'a str> {
        lookup(self.cookie, b';', name)
    }

    /// The body as text, when it is UTF-8. Links the UTF-8 validator
    /// (about 0.7 KB of wasm) into apps that call it.
    pub fn body_str(&self) -> Option<&'a str> {
        core::str::from_utf8(self.body).ok()
    }
}

/// Parses a decimal number without sign, as found in query strings and
/// environment values. `None` when empty, not all digits, or above `u32::MAX`.
pub fn parse_u32(s: &str) -> Option<u32> {
    if s.is_empty() {
        return None;
    }
    let mut v: u32 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u32)?;
    }
    Some(v)
}

/// Decodes `%XX` escapes and `+` (as space) from `src` into `out`. Returns the
/// decoded text, or `None` when `out` is too small, an escape is malformed, or
/// the result is not UTF-8.
pub fn percent_decode<'b>(src: &str, out: &'b mut [u8]) -> Option<&'b str> {
    let bytes = src.as_bytes();
    let mut i = 0;
    let mut o = 0;
    while i < bytes.len() {
        if o >= out.len() {
            return None;
        }
        match bytes[i] {
            b'%' => {
                if i + 2 >= bytes.len() {
                    return None;
                }
                let hi = hex(bytes[i + 1])?;
                let lo = hex(bytes[i + 2])?;
                out[o] = hi << 4 | lo;
                i += 3;
            }
            b'+' => {
                out[o] = b' ';
                i += 1;
            }
            b => {
                out[o] = b;
                i += 1;
            }
        }
        o += 1;
    }
    core::str::from_utf8(&out[..o]).ok()
}

fn hex(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_full_block() {
        let block = b"POST /api/items?limit=5&q=a%20b\nCookie: sid=abc; theme=dark\nContent-Type: application/json\nX-Request-Id: r1\nX-Forwarded-For: 10.0.0.2\n\n{\"a\":1}";
        let r = Request::parse(block).unwrap();
        assert_eq!(r.method, "POST");
        assert_eq!(r.path, "/api/items");
        assert_eq!(r.query, "limit=5&q=a%20b");
        assert_eq!(r.cookie("sid"), Some("abc"));
        assert_eq!(r.cookie("theme"), Some("dark"));
        assert_eq!(r.cookie("nope"), None);
        assert_eq!(r.content_type, "application/json");
        assert_eq!(r.request_id, "r1");
        assert_eq!(r.forwarded_for, "10.0.0.2");
        assert_eq!(r.body, b"{\"a\":1}");
        assert_eq!(r.query_param("limit"), Some("5"));
        let mut buf = [0u8; 16];
        assert_eq!(percent_decode(r.query_param("q").unwrap(), &mut buf), Some("a b"));
        assert!(r.is("POST", "/api/items"));
        assert!(!r.is("GET", "/api/items"));
    }

    #[test]
    fn minimal_block() {
        let r = Request::parse(b"GET /\n\n").unwrap();
        assert_eq!(r.method, "GET");
        assert_eq!(r.path, "/");
        assert_eq!(r.query, "");
        assert_eq!(r.body, b"");
        assert_eq!(r.query_param("x"), None);
        assert!(Request::parse(b"").is_none());
        assert!(Request::parse(b"\n\n").is_none());
        assert!(Request::parse(b"GET /\xff\n\n").is_none());
        assert!(Request::parse(b" /x\n\n").is_none());
    }

    #[test]
    fn no_blank_line_means_no_body() {
        let r = Request::parse(b"GET /x?flag\nCookie: a=1\n").unwrap();
        assert_eq!(r.body, b"");
        assert_eq!(r.query_param("flag"), Some(""));
        assert_eq!(r.cookie("a"), Some("1"));
    }

    #[test]
    fn non_ascii_header_is_ignored_and_body_is_kept() {
        let r = Request::parse(b"PUT /f\nCookie: n=\xc3\xa9\nContent-Type: text/plain\n\n\xc3\xa9").unwrap();
        assert_eq!(r.cookie, "");
        assert_eq!(r.content_type, "text/plain");
        assert_eq!(r.body_str(), Some("\u{e9}"));
    }

    #[test]
    fn query_edge_cases() {
        let r = Request::parse(b"GET /?&a=1&&b&=x&a=2\n\n").unwrap();
        assert_eq!(r.query_param("a"), Some("1"));
        assert_eq!(r.query_param("b"), Some(""));
        assert_eq!(r.query_param(""), None);
    }

    #[test]
    fn numbers() {
        assert_eq!(parse_u32("0"), Some(0));
        assert_eq!(parse_u32("4294967295"), Some(u32::MAX));
        assert_eq!(parse_u32("4294967296"), None);
        assert_eq!(parse_u32(""), None);
        assert_eq!(parse_u32("1a"), None);
    }

    #[test]
    fn percent_decoding_errors() {
        let mut buf = [0u8; 2];
        assert_eq!(percent_decode("abc", &mut buf), None);
        assert_eq!(percent_decode("%zz", &mut buf), None);
        assert_eq!(percent_decode("%4", &mut buf), None);
        assert_eq!(percent_decode("a+b", &mut [0u8; 3]), Some("a b"));
    }
}
