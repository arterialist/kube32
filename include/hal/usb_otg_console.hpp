#pragma once

// A node's console over USB OTG: a TinyUSB CDC-ACM port, for boards whose USB
// port is wired to OTG instead of USB-Serial/JTAG. CONFIG_KUBE32_CONSOLE_USB_OTG
// selects it. UART0 keeps the boot and panic output.

#include <cstddef>
#include <cstdint>

namespace kube32::hal::usb_otg_console {

// stdout and stderr keep going to UART0 and also into the output ring that
// the first host reads. Call first thing in app_main.
void early_init();

// Starts TinyUSB and the CDC port. False leaves the console on USB-Serial/JTAG
// for this boot: after a crash, after the guard, or when TinyUSB fails.
bool start();

// Up to `cap` bytes the host sent, waiting up to timeout_ms; 0 when none came.
int read(char* buf, size_t cap, uint32_t timeout_ms);

// Whole `len` bytes queued for the host, or dropped when no host reads: this
// never waits for a host.
void write(const char* data, size_t len);

// One whole frame queued for the host, waiting up to timeout_ms for room. False
// when no host has the port open or room did not appear.
bool write_frame(const char* data, size_t len, uint32_t timeout_ms);

// Sets the chip up for a reset into its ROM download mode over USB.
void prepare_download_reset();

}  // namespace kube32::hal::usb_otg_console
