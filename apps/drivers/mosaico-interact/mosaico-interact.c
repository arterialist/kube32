/* driver-mosaico-interact: ESP-Mosaico Interaction Module device driver.
 *
 * Reads PIR motion (AS312), ambient light (GL5506 LDR, filtered 0..100 + raw),
 * and 2 capacitive/GPIO buttons via k32_interact_read (granted by
 * hardware.kube32.io/interact). Actions control the 6x WS2812 RGB LEDs (safe
 * brightness cap applied by firmware) and 38 kHz NEC IR transmitter. */
#define DRIVER "mosaico-interact"
#define INTERVAL_MS 500
#define ACTIONS 1
#define DESCRIBE "{\"driver\":\"mosaico-interact\",\"readings\":{\"left_pressed\":\"bool\",\"right_pressed\":\"bool\",\"motion\":\"bool\",\"light_pct\":\"%\",\"light_raw\":\"adc\"},\"actions\":[{\"action\":\"led\",\"index\":\"0-5\",\"r\":\"0-255\",\"g\":\"0-255\",\"b\":\"0-255\"},{\"action\":\"led_clear\"},{\"action\":\"ir_nec\",\"address\":\"0-255\",\"command\":\"0-255\"}]}"
#include "../common/driver.h"

static k32_interact_inputs_t cur;
static int have_prev;
static uint8_t prev_l, prev_r, prev_m, prev_light;
static uint32_t last_pub_ms;

static void start(void) {
    out_len = 0;
    put(DRIVER ": polling Interaction Module every "); put_uint(interval_ms); put(" ms");
    log_out();
}

static int poll(uint32_t now) {
    k32_interact_inputs_t in;
    int32_t rc = k32_interact_read(&in);
    if (rc < 0) {
        dev_fail(now, "interaction module did not answer", rc);
        return 0;
    }
    cur = in;
    int first_ok = dev_ok(now);
    int changed = !have_prev || in.left_pressed != prev_l || in.right_pressed != prev_r ||
                  in.motion_detected != prev_m ||
                  (in.light_level > prev_light ? in.light_level - prev_light : prev_light - in.light_level) >= 3;
    if (first_ok || changed || (uint32_t)(now - last_pub_ms) >= 5000u) {
        have_prev = 1;
        prev_l = in.left_pressed;
        prev_r = in.right_pressed;
        prev_m = in.motion_detected;
        prev_light = in.light_level;
        last_pub_ms = now;
        publish();
    }
    return 0;
}

static void put_readings(void) {
    key("left_pressed"); put_bool(cur.left_pressed);
    key("right_pressed"); put_bool(cur.right_pressed);
    key("motion"); put_bool(cur.motion_detected);
    key("light_pct"); put_uint(cur.light_level);
    key("light_raw"); put_int(cur.light_raw);
}

static int act(const char* name, uint32_t n, const char* b, const char* end) {
    if (is(name, n, "led")) {
        int32_t idx = json_uint(b, end, "index", 5, -2);
        if (idx == -1) { act_error = "index must be 0 to 5 (or omitted for all LEDs)"; return ACT_BAD; }
        int32_t r = json_uint(b, end, "r", 255, 0);
        int32_t g = json_uint(b, end, "g", 255, 0);
        int32_t bl = json_uint(b, end, "b", 255, 0);
        if (r < 0 || g < 0 || bl < 0) { act_error = "r, g and b are 0 to 255"; return ACT_BAD; }
        uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        int32_t rc = k32_interact_led_set(idx < 0 ? K32_LED_ALL : (uint32_t)idx, rgb);
        if (rc < 0) { dev_fail(last_now, "LED update failed", rc); return ACT_FAILED; }
        return 0;
    }
    if (is(name, n, "led_clear")) {
        int32_t rc = k32_interact_led_set(K32_LED_ALL, 0);
        if (rc < 0) { dev_fail(last_now, "LED clear failed", rc); return ACT_FAILED; }
        return 0;
    }
    if (is(name, n, "ir_nec")) {
        int32_t addr = json_uint(b, end, "address", 255, -1);
        int32_t cmd = json_uint(b, end, "command", 255, -1);
        if (addr < 0 || cmd < 0) { act_error = "address and command are 0 to 255"; return ACT_BAD; }
        int32_t rc = k32_interact_ir_send_nec((uint32_t)addr, (uint32_t)cmd);
        if (rc < 0) { dev_fail(last_now, "IR NEC send failed", rc); return ACT_FAILED; }
        return 0;
    }
    return ACT_BAD;
}
