/* driver-button: a push button or any contact on a GPIO input.
 *
 * Each tick reads the pin. A new level counts once it has held for
 * CONFIG_DEBOUNCE_MS: the driver is ticked every 10 ms until then, so the
 * tick interval is the time to notice a press and the debounce time the
 * time to trust it. Each change is published, and the state again every
 * CONFIG_REPORT_MS without one. Readings: pressed, and presses since the
 * driver started.
 *
 * Environment (besides driver.h's):
 *   PIN_PIN             the GPIO (required)
 *   CONFIG_PULL         up, down or none (default up)
 *   CONFIG_ACTIVE       the level of a pressed button: low or high (default low)
 *   CONFIG_DEBOUNCE_MS  default 30
 *   CONFIG_REPORT_MS    default 60000, 0 publishes changes only */
#define DRIVER "button"
#define INTERVAL_MS 50
#define GPIO_DRIVER
#define DESCRIBE "{\"driver\":\"button\",\"readings\":{\"pressed\":\"bool\",\"presses\":\"count\"},\"actions\":[]}"
#include "../common/driver.h"

static uint32_t mode, active, debounce_ms, report_ms;
static int configured, pressed, candidate = -1;
static uint32_t since, reported, presses;

static void start(void) {
    static const uint32_t modes[] = {K32_GPIO_INPUT_PULLUP, K32_GPIO_INPUT_PULLDOWN, K32_GPIO_INPUT};
    mode = modes[env_word("CONFIG_PULL", "up\0down\0none\0", 0)];
    active = (uint32_t)env_word("CONFIG_ACTIVE", "low\0high\0", 0);
    debounce_ms = env_num("CONFIG_DEBOUNCE_MS", 30, 0);
    report_ms = env_num("CONFIG_REPORT_MS", 60000, 0);
    out_len = 0;
    if (pin == NO_PIN) {
        put(DRIVER ": PIN_PIN is not set");
    } else {
        put(DRIVER ":"); put_where(); put(active ? ", active high" : ", active low");
        put(mode == K32_GPIO_INPUT_PULLUP ? ", pull-up" : mode == K32_GPIO_INPUT_PULLDOWN ? ", pull-down" : ", no pull");
    }
    log_out();
}

static int poll(uint32_t now) {
    if (pin == NO_PIN) {
        dev_fail(now, "PIN_PIN is not set", 0);
        return 0;
    }
    int32_t r = configured ? 0 : esp32_gpio_set_mode(pin, mode);
    if (r >= 0) r = esp32_gpio_read(pin);
    if (r < 0) {
        configured = 0;
        dev_fail(now, "cannot read the pin", r);
        return 0;
    }
    configured = 1;
    const int level_pressed = (uint32_t)r == active;
    int changed = 0;
    if (state != 1) {                       /* the first reading */
        pressed = level_pressed;
        candidate = -1;
    } else if (level_pressed == pressed) {
        candidate = -1;
    } else if (candidate != level_pressed) {
        candidate = level_pressed;
        since = now;
        return 1;
    } else if (now - since < debounce_ms) {
        return 1;
    } else {
        pressed = level_pressed;
        candidate = -1;
        presses += (uint32_t)pressed;
        changed = 1;
    }
    if (dev_ok(now) || changed || (report_ms && now - reported >= report_ms)) {
        reported = now;
        publish();
    }
    return 0;
}

static void put_readings(void) {
    key("pressed"); put_bool(pressed);
    key("presses"); put_uint(presses);
}
