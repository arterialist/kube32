/* driver-switch: a GPIO output driving a relay, a MOSFET or an LED.
 *
 * POST / {"action":"set","on":true} or {"action":"toggle"} switches it; each
 * change is published, and the state again every tick (DEVICE_INTERVAL_MS).
 * Each tick also drives the pin again, so a pin reset by something else
 * returns to its state. Reading: on.
 *
 * Environment (besides driver.h's):
 *   PIN_PIN         the GPIO (required)
 *   CONFIG_INITIAL  off or on: the state at start (default off)
 *   CONFIG_ACTIVE   the level that means on: high or low (default high) */
#define DRIVER "switch"
#define INTERVAL_MS 30000
#define GPIO_DRIVER
#define ACTIONS
#define DESCRIBE "{\"driver\":\"switch\",\"readings\":{\"on\":\"bool\"},\"actions\":[{\"action\":\"set\",\"on\":\"bool\"},{\"action\":\"toggle\"}]}"
#include "../common/driver.h"

static int on, active_low, configured;

/* Drives the pin to the state: the level first, so the pin starts at it
 * when it becomes an output */
static int32_t drive(void) {
    if (pin == NO_PIN) return K32_ERR_INVAL;
    int32_t r = esp32_gpio_write(pin, (uint32_t)(on ^ active_low));
    if (r >= 0 && !configured) {
        r = esp32_gpio_set_mode(pin, K32_GPIO_OUTPUT);
        if (r >= 0) r = esp32_gpio_write(pin, (uint32_t)(on ^ active_low));
    }
    configured = r >= 0;
    return r;
}

static void start(void) {
    on = env_word("CONFIG_INITIAL", "off\0on\0", 0);
    active_low = env_word("CONFIG_ACTIVE", "high\0low\0", 0);
    out_len = 0;
    if (pin == NO_PIN) {
        put(DRIVER ": PIN_PIN is not set");
    } else {
        put(DRIVER ":"); put_where(); put(active_low ? ", active low" : ", active high"); put(on ? ", on" : ", off");
    }
    log_out();
}

/* Drives the pin and publishes the state when `changed` or the pin had failed */
static int update(uint32_t now, int changed) {
    int32_t r = drive();
    if (r < 0) {
        dev_fail(now, pin == NO_PIN ? "PIN_PIN is not set" : "cannot drive the pin", pin == NO_PIN ? 0 : r);
        return ACT_FAILED;
    }
    if (dev_ok(now) || changed) publish();
    return 0;
}

static int poll(uint32_t now) {
    update(now, 1);
    return 0;
}

static void put_readings(void) {
    key("on"); put_bool(on);
}

static int act(const char* name, uint32_t n, const char* body, const char* end) {
    const int was = on;
    if (is(name, n, "toggle")) {
        on = !on;
    } else if (is(name, n, "set")) {
        const int v = json_bool(body, end, "on");
        if (v < 0) {
            act_error = "set takes on: true or false";
            return ACT_BAD;
        }
        on = v;
    } else {
        return ACT_BAD;
    }
    const int rc = update((uint32_t)k32_time_ms(), on != was);
    if (rc) on = was;
    return rc;
}
