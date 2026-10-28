//! Cluster virtual channels (`kube32_channel_*`): ESP-NOW frames on hardware,
//! UDP between simulator processes. Needs the `CHANNEL_IPC` capability.
//!
//! Request pods usually do not need this: the host delivers `<app>.events`
//! and `<app>.input` to their `k32_event` export. `app_main` pods poll a
//! channel from their own loop, as `apps/tessera/hud/hud.c` does.

use crate::abi;
use crate::Error;

pub struct Channel {
    handle: i32,
}

impl Channel {
    /// Opens (or subscribes to) a channel by name; at most 63 bytes.
    pub fn open(name: &str, flags: u32) -> Result<Channel, Error> {
        if name.is_empty() || name.len() > 63 {
            return Err(Error::Invalid);
        }
        let mut z = [0u8; 64];
        z[..name.len()].copy_from_slice(name.as_bytes());
        let rc = unsafe { abi::kube32_channel_open(z.as_ptr(), flags) };
        Error::from_rc(rc).map(|h| Channel { handle: h as i32 })
    }

    pub fn send(&self, data: &[u8]) -> Result<u32, Error> {
        let rc = unsafe { abi::kube32_channel_send(self.handle, data.as_ptr(), data.len() as u32) };
        Error::from_rc(rc)
    }

    /// Waits up to `timeout_ms` for one frame. `Ok(None)` on timeout.
    pub fn recv<'b>(&self, buf: &'b mut [u8], timeout_ms: u32) -> Result<Option<&'b [u8]>, Error> {
        let rc = unsafe { abi::kube32_channel_recv(self.handle, buf.as_mut_ptr(), buf.len() as u32, timeout_ms) };
        if rc == 0 || rc == crate::ERR_AGAIN || rc == crate::ERR_TIMEDOUT {
            return Ok(None);
        }
        let n = Error::from_rc(rc)? as usize;
        Ok(Some(&buf[..n.min(buf.len())]))
    }

    pub fn raw_handle(&self) -> i32 {
        self.handle
    }
}

impl Drop for Channel {
    fn drop(&mut self) {
        unsafe { abi::kube32_channel_close(self.handle) };
    }
}
