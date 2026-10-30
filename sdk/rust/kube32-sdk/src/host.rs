//! Node services: log, time, random, environment, memory statistics, events.

use crate::abi;
use crate::Error;

/// Writes a line to the pod log (`kubectl logs` / serial console).
pub fn log(msg: &str) {
    unsafe { abi::k32_log(msg.as_ptr(), msg.len() as u32) }
}

/// Milliseconds since the node booted.
pub fn time_ms() -> i64 {
    unsafe { abi::k32_time_ms() }
}

/// Wall clock in Unix milliseconds; `None` while the node's clock is not set.
pub fn unix_time_ms() -> Option<i64> {
    match unsafe { abi::k32_unix_time_ms() } {
        0 => None,
        ms => Some(ms),
    }
}

/// Hardware random number on ESP32, `rand()` on the host simulator.
pub fn random() -> u32 {
    unsafe { abi::k32_random() }
}

/// Copies the value of a container `env` entry into `buf` and returns it.
/// `None` when the variable is unset, not UTF-8, or longer than `buf`
/// (`env_len` tells the real length).
pub fn env<'b>(key: &str, buf: &'b mut [u8]) -> Option<&'b str> {
    let n = unsafe { abi::k32_env_get(key.as_ptr(), key.len() as u32, buf.as_mut_ptr(), buf.len() as u32) };
    if n < 0 {
        return None;
    }
    let n = n as usize;
    if n > buf.len() {
        return None;
    }
    core::str::from_utf8(&buf[..n]).ok()
}

/// Length of an environment value, or `None` when unset.
pub fn env_len(key: &str) -> Option<usize> {
    let n = unsafe { abi::k32_env_get(key.as_ptr(), key.len() as u32, core::ptr::null_mut(), 0) };
    if n < 0 {
        None
    } else {
        Some(n as usize)
    }
}

/// An environment value parsed as a decimal number.
pub fn env_u32(key: &str) -> Option<u32> {
    let mut buf = [0u8; 16];
    crate::request::parse_u32(env(key, &mut buf)?)
}

/// `(used, limit)` bytes of linear memory as the host sees them.
pub fn mem_stats() -> Option<(u32, u32)> {
    let mut used: u32 = 0;
    let mut limit: u32 = 0;
    let rc = unsafe { abi::k32_mem_stats(&mut used, &mut limit) };
    if rc == 0 {
        Some((used, limit))
    } else {
        None
    }
}

/// Publishes an event on a cluster channel (`<app>.events` reaches the
/// ingress SSE stream and every pod exporting `k32_event`). Needs the
/// `CHANNEL_IPC` capability (`hardware.kube32.io/ipc: "1"`). Payloads are at
/// most `MAX_EVENT_BYTES` (200); topics at most 64 bytes.
pub fn emit(topic: &str, data: &[u8]) -> Result<u32, Error> {
    if data.len() > crate::MAX_EVENT_BYTES || topic.is_empty() || topic.len() > 64 {
        return Err(Error::Invalid);
    }
    let rc = unsafe { abi::k32_event_emit(topic.as_ptr(), topic.len() as u32, data.as_ptr(), data.len() as u32) };
    Error::from_rc(rc)
}

/// Blocks the pod for `ms` milliseconds. Meant for `app_main` loops; inside
/// `k32_handle` it stalls every request to this pod.
pub fn delay_ms(ms: u32) {
    unsafe { abi::esp32_delay_ms(ms) }
}
