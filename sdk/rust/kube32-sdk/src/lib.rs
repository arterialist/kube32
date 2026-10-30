//! Rust bindings for the Kube32 WebAssembly pod ABI.
//!
//! A request-handling pod is a few functions and one macro invocation:
//!
//! ```ignore
//! #![no_std]
//! use kube32_sdk::{kube32_app, Request, Response};
//!
//! fn handle(req: &Request) -> i32 {
//!     let mut resp = Response::json(200);
//!     resp.write_str("{\"path\":").write_json_str(req.path).write_str("}");
//!     resp.end()
//! }
//!
//! kube32_app! { handle: handle, }
//! ```
//!
//! The macro exports `k32_alloc` and `k32_handle` (plus `k32_init`,
//! `k32_tick`, `k32_event` when given); the host looks them up by name
//! (`include/runtime/k32_app_abi.h`). Build with
//! `cargo build --release --target wasm32-unknown-unknown` and the link flags
//! in `sdk/rust/.cargo/config.toml`, or with `tools/kube32-cli build`.
//!
//! The crate is `no_std`; the only allocation is the static request buffer
//! the macro reserves for `k32_alloc`.
//!
//! Features:
//!
//! * `panic-handler` (default): a `#[panic_handler]` that logs through
//!   `k32_log` and traps.
//! * `hardware`: [`hardware`] wraps `k32_display_*`, `k32_key_read`,
//!   `k32_tone` and the infrared imports `esp32_rmt_send` and
//!   `esp32_rmt_receive`.
//! * `fs`: [`fs`] wraps the `k32_fs_*` imports (pod storage on the microSD card).
//!
//! An import is only linked into the module when the app calls it, so the
//! module's import list tells which manifest resources it needs;
//! `tools/kube32-cli check` prints that list.
#![no_std]
#![allow(clippy::needless_doctest_main)]

pub mod abi;
pub mod buf;
pub mod channel;
#[cfg(feature = "fs")]
pub mod fs;
#[cfg(feature = "hardware")]
pub mod hardware;
pub mod host;
pub mod request;
pub mod response;

pub use buf::Buf;
pub use channel::Channel;
pub use host::{delay_ms, emit, env, env_len, env_u32, log, mem_stats, random, time_ms};
pub use request::{parse_u32, percent_decode, Request};
pub use response::Response;

/// `k32_handle` return value when the request block could not be parsed.
pub const HANDLE_ERR_NO_RESPONSE: i32 = -1;
pub const HANDLE_ERR_BAD_REQUEST: i32 = -2;

/// Limits enforced by the host (`k32_app_abi.h`).
pub const MAX_REQUEST_BYTES: usize = 16 * 1024;
pub const MAX_EVENT_BYTES: usize = 200;
pub const MAX_HEADER_BYTES: usize = 256;

/// Host error codes: negative errno values (`k32_app_abi.h`, `hal_types.hpp`).
pub const ERR_PERM: i32 = -1;
pub const ERR_NOENT: i32 = -2;
pub const ERR_IO: i32 = -5;
pub const ERR_TOOBIG: i32 = -7;
pub const ERR_BADF: i32 = -9;
pub const ERR_AGAIN: i32 = -11;
pub const ERR_NOMEM: i32 = -12;
pub const ERR_FAULT: i32 = -14;
pub const ERR_EXIST: i32 = -17;
pub const ERR_NODEV: i32 = -19;
pub const ERR_ISDIR: i32 = -21;
pub const ERR_INVAL: i32 = -22;
pub const ERR_MFILE: i32 = -24;
pub const ERR_NOSPC: i32 = -28;
pub const ERR_NAMETOOLONG: i32 = -36;
/// `kube32_channel_recv` when nothing arrived in time.
pub const ERR_TIMEDOUT: i32 = -110;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The manifest does not grant the capability (`K32_ERR_PERM`); the host logs a violation.
    Permission,
    /// No such file or directory (`K32_ERR_NOENT`).
    NotFound,
    /// I/O error on the card or the radio (`K32_ERR_IO`).
    Io,
    /// The data does not fit the buffer (`K32_ERR_TOOBIG`): an IR frame
    /// longer than the buffer, whose first part was copied.
    TooBig,
    /// File descriptor not open, or opened for the other direction (`K32_ERR_BADF`).
    BadFd,
    /// Pointer or length outside linear memory (`K32_ERR_FAULT`).
    Fault,
    /// `O_EXCL` and the file exists (`K32_ERR_EXIST`).
    Exists,
    /// No such device on this node (`K32_ERR_NODEV`): no card, or no display on the simulator.
    NoDevice,
    /// The path names a directory (`K32_ERR_ISDIR`).
    IsDir,
    /// Bad argument: bad path or flags, oversized payload (`K32_ERR_INVAL`).
    Invalid,
    /// Four files already open, per pod or node-wide (`K32_ERR_MFILE`).
    TooManyFiles,
    /// Card full (`K32_ERR_NOSPC`).
    NoSpace,
    /// Path longer than 96 bytes (`K32_ERR_NAMETOOLONG`).
    NameTooLong,
    /// Any other negative code from the host.
    Host(i32),
}

impl Error {
    /// Maps a host return code: negative codes become errors, others are returned.
    pub fn from_rc(rc: i32) -> Result<u32, Error> {
        match rc {
            n if n >= 0 => Ok(n as u32),
            ERR_PERM => Err(Error::Permission),
            ERR_NOENT => Err(Error::NotFound),
            ERR_IO => Err(Error::Io),
            ERR_TOOBIG => Err(Error::TooBig),
            ERR_BADF => Err(Error::BadFd),
            ERR_FAULT => Err(Error::Fault),
            ERR_EXIST => Err(Error::Exists),
            ERR_NODEV => Err(Error::NoDevice),
            ERR_ISDIR => Err(Error::IsDir),
            ERR_INVAL => Err(Error::Invalid),
            ERR_MFILE => Err(Error::TooManyFiles),
            ERR_NOSPC => Err(Error::NoSpace),
            ERR_NAMETOOLONG => Err(Error::NameTooLong),
            n => Err(Error::Host(n)),
        }
    }

    /// The host's code for this error.
    pub fn code(self) -> i32 {
        match self {
            Error::Permission => ERR_PERM,
            Error::NotFound => ERR_NOENT,
            Error::Io => ERR_IO,
            Error::TooBig => ERR_TOOBIG,
            Error::BadFd => ERR_BADF,
            Error::Fault => ERR_FAULT,
            Error::Exists => ERR_EXIST,
            Error::NoDevice => ERR_NODEV,
            Error::IsDir => ERR_ISDIR,
            Error::Invalid => ERR_INVAL,
            Error::TooManyFiles => ERR_MFILE,
            Error::NoSpace => ERR_NOSPC,
            Error::NameTooLong => ERR_NAMETOOLONG,
            Error::Host(n) => n,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_codes_map_to_errors_and_back() {
        let all = [
            Error::Permission,
            Error::NotFound,
            Error::Io,
            Error::TooBig,
            Error::BadFd,
            Error::Fault,
            Error::Exists,
            Error::NoDevice,
            Error::IsDir,
            Error::Invalid,
            Error::TooManyFiles,
            Error::NoSpace,
            Error::NameTooLong,
        ];
        for e in all {
            assert_eq!(Error::from_rc(e.code()), Err(e));
        }
        assert_eq!(Error::from_rc(ERR_TOOBIG), Err(Error::TooBig));
        assert_eq!(Error::from_rc(67), Ok(67));
        assert_eq!(Error::from_rc(ERR_AGAIN), Err(Error::Host(ERR_AGAIN)));
    }
}

/// Default size of the buffer `k32_alloc` hands to the host: one maximal
/// request block (16 KiB) plus room for an event topic and payload.
pub const DEFAULT_ALLOC_BYTES: usize = MAX_REQUEST_BYTES + 512;

#[doc(hidden)]
#[macro_export]
macro_rules! __k32_alloc_bytes {
    () => {
        $crate::DEFAULT_ALLOC_BYTES
    };
    ($e:expr) => {
        $e
    };
}

/// Exports the request-pod entry points. Keys are optional but must appear
/// in this order: `alloc` (buffer size in bytes), `init`, `handle`, `tick`,
/// `event`.
///
/// ```ignore
/// kube32_app! {
///     alloc: 4096,              // optional, default DEFAULT_ALLOC_BYTES
///     init: init,               // fn() -> i32
///     handle: handle,           // fn(&Request) -> i32
///     tick: tick,               // fn(uptime_ms: u32) -> i32   (1 when more work is pending)
///     event: on_event,          // fn(topic: &str, data: &[u8]) -> i32
/// }
/// ```
#[macro_export]
macro_rules! kube32_app {
    (
        $(alloc: $alloc:expr,)?
        $(init: $init:path,)?
        $(handle: $handle:path,)?
        $(tick: $tick:path,)?
        $(event: $event:path,)?
    ) => {
        const __K32_ALLOC_BYTES: usize = $crate::__k32_alloc_bytes!($($alloc)?);
        static mut __K32_ALLOC_BUF: [u8; __K32_ALLOC_BYTES] = [0; __K32_ALLOC_BYTES];

        /// The host copies each request or event here before calling the handler.
        #[no_mangle]
        pub extern "C" fn k32_alloc(size: u32) -> u32 {
            if size as usize > __K32_ALLOC_BYTES {
                return 0;
            }
            unsafe { ::core::ptr::addr_of_mut!(__K32_ALLOC_BUF) as *mut u8 as u32 }
        }

        $(
            #[no_mangle]
            pub extern "C" fn k32_init() -> i32 {
                let f: fn() -> i32 = $init;
                f()
            }
        )?

        $(
            #[no_mangle]
            pub extern "C" fn k32_handle(ptr: u32, len: u32) -> i32 {
                let block: &[u8] = unsafe { ::core::slice::from_raw_parts(ptr as *const u8, len as usize) };
                let f: fn(&$crate::Request) -> i32 = $handle;
                match $crate::Request::parse(block) {
                    Some(req) => f(&req),
                    // Not a request line we can read (no method, non-ASCII):
                    // answer instead of letting the host turn silence into a 500.
                    None => {
                        let mut resp = $crate::Response::text(400);
                        resp.write_str("bad request\n");
                        resp.end()
                    }
                }
            }
        )?

        $(
            #[no_mangle]
            pub extern "C" fn k32_tick(uptime_ms: u32) -> i32 {
                let f: fn(u32) -> i32 = $tick;
                f(uptime_ms)
            }
        )?

        $(
            #[no_mangle]
            pub extern "C" fn k32_event(topic_ptr: u32, topic_len: u32, ptr: u32, len: u32) -> i32 {
                let topic: &[u8] = unsafe { ::core::slice::from_raw_parts(topic_ptr as *const u8, topic_len as usize) };
                let data: &[u8] = unsafe { ::core::slice::from_raw_parts(ptr as *const u8, len as usize) };
                let f: fn(&str, &[u8]) -> i32 = $event;
                // Topics are channel names such as "counter.events": ASCII.
                let topic = if topic.is_ascii() { unsafe { ::core::str::from_utf8_unchecked(topic) } } else { "" };
                f(topic, data)
            }
        )?
    };
}

/// Exports `app_main` for a pod that runs its own loop (display, keyboard,
/// channels) instead of handling requests. The host runs it on the pod's own
/// thread; returning ends the container (the pod log says
/// `entrypoint returned, container completed`). The return value is not used.
///
/// ```ignore
/// fn main_loop() -> i32 { loop { /* ... */ } }
/// kube32_main!(main_loop);
/// ```
#[macro_export]
macro_rules! kube32_main {
    ($main:path) => {
        #[no_mangle]
        pub extern "C" fn app_main() -> i32 {
            let f: fn() -> i32 = $main;
            f()
        }
    };
}

#[cfg(all(target_arch = "wasm32", feature = "panic-handler"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    host::log("[rust] panic: guest trapped");
    core::arch::wasm32::unreachable()
}
