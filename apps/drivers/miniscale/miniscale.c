/* driver-miniscale: the M5Stack Unit Mini Scales, a 5 kg load cell on an
 * HX711 behind an STM32F030 that answers on I2C at 0x26 (protocol v3; the
 * v4 firmware reports negative weights as well).
 *
 * Registers: 0x00 raw ADC (int32, little-endian), 0x60 weight x 100 (int32,
 * grams), 0x30 LED (R, G, B), 0x50 offset (write 1: tare), 0xFE firmware
 * version, 0xFF the unit's I2C address. The unit keeps its calibration (the
 * gap at 0x40) itself.
 *
 * Each tick reads the weight and the raw value. Actions:
 *   {"action":"tare"}                       the current load becomes zero
 *   {"action":"led","r":0,"g":32,"b":0}     the RGB LED, 0-255 each (absent: 0) */
#define DRIVER "miniscale"
#define INTERVAL_MS 1000
#define I2C_DEFAULT_ADDR 0x26
#define ACTIONS
#define DESCRIBE "{\"driver\":\"miniscale\",\"readings\":{\"weight_g\":\"g\",\"raw\":\"adc\"},\"actions\":[{\"action\":\"tare\"},{\"action\":\"led\",\"r\":\"0-255\",\"g\":\"0-255\",\"b\":\"0-255\"}]}"
#include "../common/driver.h"

#define REG_RAW         0x00
#define REG_LED         0x30
#define REG_OFFSET      0x50
#define REG_WEIGHT_X100 0x60
#define REG_VERSION     0xFE
#define REG_ADDRESS     0xFF

static int32_t weight_x100, raw;

static int32_t le32(const uint8_t* b) {
    return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}

static void start(void) {
    uint8_t id[2] = {0, 0};   /* address register, firmware version */
    int32_t r = reg_read(REG_ADDRESS, &id[0], 1);
    if (!r) r = reg_read(REG_VERSION, &id[1], 1);
    out_len = 0;
    if (r) {
        put(DRIVER ": no Unit Mini Scales"); put_where(); put(" yet (error "); put_int(r); put(")");
    } else {
        put(DRIVER ": Unit Mini Scales"); put_where(); put(", firmware "); put_uint(id[1]);
        if (id[0] != addr) { put(", but its address register holds "); put_hex(id[0]); }
    }
    log_out();
}

static int poll(uint32_t now) {
    uint8_t b[4];
    int32_t r = reg_read(REG_WEIGHT_X100, b, 4);
    if (!r) {
        weight_x100 = le32(b);
        r = reg_read(REG_RAW, b, 4);
    }
    if (r) {
        dev_fail(now, "the scale does not answer", r);
        return 0;
    }
    raw = le32(b);
    dev_ok(now);
    publish();
    return 0;
}

static void put_readings(void) {
    key("weight_g"); put_fixed(weight_x100, 2);
    key("raw"); put_int(raw);
}

static int act(const char* name, uint32_t n, const char* body, const char* end) {
    uint8_t w[4];
    uint32_t len = 4;
    if (is(name, n, "tare")) {
        w[0] = REG_OFFSET;
        w[1] = 1;
        len = 2;
    } else if (is(name, n, "led")) {
        const char* k = "r\0g\0b";
        w[0] = REG_LED;
        for (int i = 0; i < 3; i++) {
            const int32_t v = json_uint(body, end, k + 2 * i, 255, 0);
            if (v < 0) {
                act_error = "led takes r, g and b from 0 to 255";
                return ACT_BAD;
            }
            w[1 + i] = (uint8_t)v;
        }
    } else {
        return ACT_BAD;
    }
    const int32_t r = reg_write(w, len);
    if (r) {
        dev_fail((uint32_t)k32_time_ms(), "the scale does not answer", r);
        return ACT_FAILED;
    }
    return 0;
}
