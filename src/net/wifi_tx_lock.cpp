// The Wi-Fi driver links the frames it is given into one software TX queue, and
// its TX task drains that queue. Each sender holds the driver's global lock
// while it links a frame in, except on the path ESP-NOW frames take. The TX task
// removes frames under that lock. An insert that interleaves with a removal can
// leave the queue's head empty and its tail inside a frame the head no longer
// reaches. Every later frame joins that unreachable chain, the driver's TX
// buffers run out, and the station stops sending while it keeps receiving.
//
// The linker sends the ESP-NOW path through this wrapper (--wrap in
// src/CMakeLists.txt), which holds the lock for the insert as the other senders
// do. The lock is a recursive mutex and this path runs in task context.
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_WIFI_SUPPORTED && !CONFIG_ETH_USE_OPENETH

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {

extern void* g_wifi_global_lock __attribute__((weak));
int __real_ieee80211_post_hmac_tx(void* eb) __attribute__((weak));
int __wrap_ieee80211_post_hmac_tx(void* eb);

int __wrap_ieee80211_post_hmac_tx(void* eb) {
    SemaphoreHandle_t lock = &g_wifi_global_lock ? static_cast<SemaphoreHandle_t>(g_wifi_global_lock) : nullptr;
    if (lock) xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int result = __real_ieee80211_post_hmac_tx(eb);
    if (lock) xSemaphoreGiveRecursive(lock);
    return result;
}

}  // extern "C"

#endif  // SOC_WIFI_SUPPORTED && !CONFIG_ETH_USE_OPENETH
