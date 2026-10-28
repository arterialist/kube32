//! Raw host imports, one per declaration in `include/runtime/k32_app_abi.h`.
//!
//! Signatures mirror the `m3_LinkRawFunction` calls in
//! `src/runtime/app_pod.cpp` (`k32_*`) and `src/runtime/host_functions.cpp`
//! (`esp32_delay_ms`, `kube32_channel_*`): pointers and lengths are
//! `i32`/`u32` in wasm, `k32_time_ms` returns `i64`. The GPIO, ADC, I2C and
//! UART imports of host_functions.cpp are not wrapped by this crate.
//! Everything here is `unsafe`; the safe wrappers live in the other modules.
#![allow(clippy::missing_safety_doc)]

#[cfg(target_arch = "wasm32")]
#[link(wasm_import_module = "env")]
extern "C" {
    // Response streaming
    pub fn k32_resp_begin(status: i32, content_type: *const u8, content_type_len: u32);
    pub fn k32_resp_header(name: *const u8, name_len: u32, value: *const u8, value_len: u32);
    pub fn k32_resp_write(data: *const u8, len: u32) -> i32;
    pub fn k32_resp_end();

    // Cluster events (needs the CHANNEL_IPC capability)
    pub fn k32_event_emit(topic: *const u8, topic_len: u32, data: *const u8, len: u32) -> i32;

    // Node services
    pub fn k32_time_ms() -> i64;
    pub fn k32_log(msg: *const u8, len: u32);
    pub fn k32_random() -> u32;
    pub fn k32_env_get(key: *const u8, key_len: u32, out: *mut u8, out_cap: u32) -> i32;
    pub fn k32_mem_stats(out_used: *mut u32, out_limit: *mut u32) -> i32;

    // Hardware, gated by the capability firewall
    pub fn k32_display_text(x: i32, y: i32, text: *const u8, len: u32, fg565: u32, bg565: u32) -> i32;
    pub fn k32_display_rect(x: i32, y: i32, w: i32, h: i32, color565: u32) -> i32;
    pub fn k32_display_bar(x: i32, y: i32, w: i32, h: i32, pct: u32, fg565: u32, bg565: u32) -> i32;
    pub fn k32_key_read() -> i32;
    pub fn k32_tone(freq_hz: u32, duration_ms: u32, volume_pct: u32) -> i32;

    // Pod storage on the microSD card, gated by hardware.kube32.io/sd
    pub fn k32_fs_open(path: *const u8, path_len: u32, flags: u32) -> i32;
    pub fn k32_fs_read(fd: i32, buf: *mut u8, len: u32) -> i32;
    pub fn k32_fs_write(fd: i32, buf: *const u8, len: u32) -> i32;
    pub fn k32_fs_close(fd: i32) -> i32;
    pub fn k32_fs_size(fd: i32) -> i32;
    pub fn k32_fs_list(dir: *const u8, dir_len: u32, out: *mut u8, out_cap: u32) -> i32;
    pub fn k32_fs_remove(path: *const u8, path_len: u32) -> i32;

    // Low-level imports from host_functions.cpp that app pods use
    pub fn esp32_delay_ms(ms: u32);
    /// `name` must be NUL-terminated: the host reads it with strnlen.
    pub fn kube32_channel_open(name: *const u8, flags: u32) -> i32;
    pub fn kube32_channel_send(handle: i32, data: *const u8, len: u32) -> i32;
    pub fn kube32_channel_recv(handle: i32, buf: *mut u8, max_len: u32, timeout_ms: u32) -> i32;
    pub fn kube32_channel_close(handle: i32) -> i32;
}

// On any other target (host unit tests of the parsers) the imports do not
// exist. These stubs keep the crate compiling; calling one is a bug.
#[cfg(not(target_arch = "wasm32"))]
mod stubs {
    #![allow(unused_variables)]
    fn no_host() -> ! {
        panic!("kube32-sdk: host import called outside the Kube32 runtime")
    }
    pub unsafe fn k32_resp_begin(status: i32, content_type: *const u8, content_type_len: u32) { no_host() }
    pub unsafe fn k32_resp_header(name: *const u8, name_len: u32, value: *const u8, value_len: u32) { no_host() }
    pub unsafe fn k32_resp_write(data: *const u8, len: u32) -> i32 { no_host() }
    pub unsafe fn k32_resp_end() { no_host() }
    pub unsafe fn k32_event_emit(topic: *const u8, topic_len: u32, data: *const u8, len: u32) -> i32 { no_host() }
    pub unsafe fn k32_time_ms() -> i64 { no_host() }
    pub unsafe fn k32_log(msg: *const u8, len: u32) { no_host() }
    pub unsafe fn k32_random() -> u32 { no_host() }
    pub unsafe fn k32_env_get(key: *const u8, key_len: u32, out: *mut u8, out_cap: u32) -> i32 { no_host() }
    pub unsafe fn k32_mem_stats(out_used: *mut u32, out_limit: *mut u32) -> i32 { no_host() }
    pub unsafe fn k32_display_text(x: i32, y: i32, text: *const u8, len: u32, fg565: u32, bg565: u32) -> i32 { no_host() }
    pub unsafe fn k32_display_rect(x: i32, y: i32, w: i32, h: i32, color565: u32) -> i32 { no_host() }
    pub unsafe fn k32_display_bar(x: i32, y: i32, w: i32, h: i32, pct: u32, fg565: u32, bg565: u32) -> i32 { no_host() }
    pub unsafe fn k32_key_read() -> i32 { no_host() }
    pub unsafe fn k32_tone(freq_hz: u32, duration_ms: u32, volume_pct: u32) -> i32 { no_host() }
    pub unsafe fn k32_fs_open(path: *const u8, path_len: u32, flags: u32) -> i32 { no_host() }
    pub unsafe fn k32_fs_read(fd: i32, buf: *mut u8, len: u32) -> i32 { no_host() }
    pub unsafe fn k32_fs_write(fd: i32, buf: *const u8, len: u32) -> i32 { no_host() }
    pub unsafe fn k32_fs_close(fd: i32) -> i32 { no_host() }
    pub unsafe fn k32_fs_size(fd: i32) -> i32 { no_host() }
    pub unsafe fn k32_fs_list(dir: *const u8, dir_len: u32, out: *mut u8, out_cap: u32) -> i32 { no_host() }
    pub unsafe fn k32_fs_remove(path: *const u8, path_len: u32) -> i32 { no_host() }
    pub unsafe fn esp32_delay_ms(ms: u32) { no_host() }
    pub unsafe fn kube32_channel_open(name: *const u8, flags: u32) -> i32 { no_host() }
    pub unsafe fn kube32_channel_send(handle: i32, data: *const u8, len: u32) -> i32 { no_host() }
    pub unsafe fn kube32_channel_recv(handle: i32, buf: *mut u8, max_len: u32, timeout_ms: u32) -> i32 { no_host() }
    pub unsafe fn kube32_channel_close(handle: i32) -> i32 { no_host() }
}
#[cfg(not(target_arch = "wasm32"))]
pub use stubs::*;
