/* driver-bme688: Bosch BME688 (and BME680) temperature, pressure, humidity
 * and gas sensor on I2C, such as the M5Stack ENV Pro unit (0x77).
 *
 * Each tick starts a forced-mode measurement: temperature x2, pressure x16
 * and humidity x1 oversampling, IIR filter off, then one heater step
 * (CONFIG_HEATER_C for CONFIG_HEATER_MS) and the gas resistance at its end.
 * The driver is ticked every 10 ms until the result is in (about 43 ms plus
 * the heater time) and never blocks the interpreter while it waits. The
 * heater target is computed from the last temperature measured.
 *
 * Readings: temperature_c, pressure_hpa, humidity_pct, gas_ohms (null when
 * the heater did not reach its temperature or the gas reading is not valid;
 * left out with CONFIG_GAS=off). Chip id 0x61 at 0xD0; the variant at 0xF0
 * selects the gas formula: 0x01 BME688, 0x00 BME680.
 *
 * Environment (besides driver.h's):
 *   CONFIG_HEATER_C   heater temperature, 200 to 400 (default 300)
 *   CONFIG_HEATER_MS  heater time, 1 to 4032 (default 100)
 *   CONFIG_GAS        on or off (default on)
 *
 * The compensation (calc_* below) is ported from the Bosch Sensortec BME68x
 * Sensor API v4.4.8 (https://github.com/boschsensortec/BME68x_SensorAPI):
 *
 * Copyright (c) 2023 Bosch Sensortec GmbH. All rights reserved.
 *
 * BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE. */
#define DRIVER "bme688"
#define INTERVAL_MS 5000
#define I2C_DEFAULT_ADDR 0x77
#define DESCRIBE "{\"driver\":\"bme688\",\"readings\":{\"temperature_c\":\"C\",\"pressure_hpa\":\"hPa\",\"humidity_pct\":\"%RH\",\"gas_ohms\":\"ohm\"},\"actions\":[]}"
#include "../common/driver.h"

#define REG_COEFF3     0x00
#define REG_FIELD0     0x1D
#define REG_RES_HEAT0  0x5A
#define REG_GAS_WAIT0  0x64
#define REG_CTRL_GAS_0 0x70
#define REG_CTRL_GAS_1 0x71
#define REG_CTRL_HUM   0x72
#define REG_CTRL_MEAS  0x74
#define REG_COEFF1     0x8A
#define REG_CHIP_ID    0xD0
#define REG_RESET      0xE0
#define REG_COEFF2     0xE1
#define REG_VARIANT    0xF0
#define CHIP_ID        0x61
#define CTRL_HUM       0x01                          /* humidity x1 */
#define CTRL_MEAS      (2 << 5 | 5 << 2)            /* temperature x2, pressure x16 */
#define TPH_MS         43                            /* their conversion and wake-up (bme68x_get_meas_dur) */

/* Calibration (bme68x_calib_data) */
/* wasm3 on the board makes no tail calls: every instruction keeps a native
 * frame until its function returns, so the compensation steps stay functions
 * of their own instead of one long inlined tick */
#define NOINLINE __attribute__((noinline))

static struct {
    int32_t t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, h1, h2, h3, h4, h5, h6, h7;
    int32_t gh1, gh2, gh3, res_heat_range, res_heat_val, range_sw_err;
} c;
/* Where each of them is in the 42 bytes read from 0x8A, 0xE1 and 0x00, and
 * its type: index << 2 | 0 int8, 1 uint8, 2 int16, 3 uint16 (LSB first).
 * h1, h2, res_heat_range and range_sw_err (0 here) are bit fields. */
static const uint8_t calib_at[26] = {
    31 << 2 | 3, 0 << 2 | 2, 2 << 2 | 0,                                       /* t1 t2 t3 */
    4 << 2 | 3, 6 << 2 | 2, 8 << 2 | 0, 10 << 2 | 2, 12 << 2 | 2,              /* p1 .. p5 */
    15 << 2 | 0, 14 << 2 | 0, 18 << 2 | 2, 20 << 2 | 2, 22 << 2 | 1,           /* p6 .. p10 */
    0, 0, 26 << 2 | 0, 27 << 2 | 0, 28 << 2 | 0, 29 << 2 | 1, 30 << 2 | 0,     /* h1 .. h7 */
    35 << 2 | 0, 33 << 2 | 2, 36 << 2 | 0, 0, 37 << 2 | 0, 0,                 /* gh1 gh2 gh3 res_heat_range res_heat_val range_sw_err */
};
static int found, measuring, variant, gas, gas_ok;
static uint32_t heater_c, heater_ms, started;
static int32_t t_fine, temp_x100, amb_c = 25;
static uint32_t press_pa, hum_milli, gas_ohms;

/* ---- compensation (Bosch BME68x Sensor API, integer version) --------------- */
static NOINLINE int32_t calc_temperature(uint32_t temp_adc) {
    int64_t var1 = ((int32_t)temp_adc >> 3) - (c.t1 << 1);
    int64_t var2 = (var1 * c.t2) >> 11;
    int64_t var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
    var3 = (var3 * (c.t3 << 4)) >> 14;
    t_fine = (int32_t)(var2 + var3);
    return (int16_t)(((t_fine * 5) + 128) >> 8);
}

static NOINLINE uint32_t calc_pressure(uint32_t pres_adc) {
    int32_t var1 = (t_fine >> 1) - 64000;
    int32_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * c.p6) >> 2;
    var2 = var2 + ((var1 * c.p5) << 1);
    var2 = (var2 >> 2) + (c.p4 << 16);
    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * (c.p3 << 5)) >> 3) + ((c.p2 * var1) >> 1);
    var1 = var1 >> 18;
    var1 = ((32768 + var1) * c.p1) >> 15;
    int32_t p = 1048576 - (int32_t)pres_adc;
    p = (int32_t)((p - (var2 >> 12)) * ((uint32_t)3125));
    if (p >= 0x40000000) p = ((p / var1) << 1);
    else p = ((p << 1) / var1);
    var1 = (c.p9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
    var2 = ((int32_t)(p >> 2) * c.p8) >> 13;
    int32_t var3 = ((int32_t)(p >> 8) * (int32_t)(p >> 8) * (int32_t)(p >> 8) * c.p10) >> 17;
    return (uint32_t)(p + ((var1 + var2 + var3 + (c.p7 << 7)) >> 4));
}

static NOINLINE uint32_t calc_humidity(uint16_t hum_adc) {
    int32_t temp_scaled = ((t_fine * 5) + 128) >> 8;
    int32_t var1 = (int32_t)(hum_adc - (c.h1 * 16)) - (((temp_scaled * c.h3) / 100) >> 1);
    int32_t var2 = (c.h2 * (((temp_scaled * c.h4) / 100) + (((temp_scaled * ((temp_scaled * c.h5) / 100)) >> 6) / 100) + (1 << 14))) >> 10;
    int32_t var3 = var1 * var2;
    int32_t var4 = ((c.h6 << 7) + ((temp_scaled * c.h7) / 100)) >> 4;
    int32_t var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
    int32_t var6 = (var4 * var5) >> 1;
    int32_t hum = (((var3 + var6) >> 10) * 1000) >> 12;
    return hum > 100000 ? 100000 : (hum < 0 ? 0 : (uint32_t)hum);
}

/* BME680 */
static NOINLINE uint32_t calc_gas_resistance_low(uint16_t gas_res_adc, uint8_t gas_range) {
    static const uint32_t lookup_table1[16] = {
        2147483647u, 2147483647u, 2147483647u, 2147483647u, 2147483647u, 2126008810u, 2147483647u, 2130303777u,
        2147483647u, 2147483647u, 2143188679u, 2136746228u, 2147483647u, 2126008810u, 2147483647u, 2147483647u};
    static const uint32_t lookup_table2[16] = {
        4096000000u, 2048000000u, 1024000000u, 512000000u, 255744255u, 127110228u, 64000000u, 32258064u,
        16016016u, 8000000u, 4000000u, 2000000u, 1000000u, 500000u, 250000u, 125000u};
    int64_t var1 = (int64_t)((1340 + (5 * (int64_t)c.range_sw_err)) * ((int64_t)lookup_table1[gas_range])) >> 16;
    uint64_t var2 = (((int64_t)((int64_t)gas_res_adc << 15) - (int64_t)(16777216)) + var1);
    int64_t var3 = (((int64_t)lookup_table2[gas_range] * (int64_t)var1) >> 9);
    return (uint32_t)((var3 + ((int64_t)var2 >> 1)) / (int64_t)var2);
}

/* BME688 */
static NOINLINE uint32_t calc_gas_resistance_high(uint16_t gas_res_adc, uint8_t gas_range) {
    uint32_t var1 = 262144u >> gas_range;
    int32_t var2 = (int32_t)gas_res_adc - 512;
    var2 = 4096 + var2 * 3;
    return (10000u * var1) / (uint32_t)var2 * 100;
}

static NOINLINE uint8_t calc_res_heat(uint32_t temp) {
    if (temp > 400) temp = 400;
    int32_t var1 = ((amb_c * c.gh3) / 1000) * 256;
    int32_t var2 = (c.gh1 + 784) * (((((c.gh2 + 154009) * (int32_t)temp * 5) / 100) + 3276800) / 10);
    int32_t var3 = var1 + (var2 / 2);
    int32_t var4 = (var3 / (c.res_heat_range + 4));
    int32_t var5 = (131 * c.res_heat_val) + 65536;
    int32_t heatr_res_x100 = (int32_t)(((var4 / var5) - 250) * 34);
    return (uint8_t)((heatr_res_x100 + 50) / 100);
}

static NOINLINE uint8_t calc_gas_wait(uint32_t dur) {
    uint8_t factor = 0;
    if (dur >= 0xfc0) return 0xff;
    while (dur > 0x3f) {
        dur /= 4;
        factor++;
    }
    return (uint8_t)(dur + factor * 64);
}

/* ---- the part -------------------------------------------------------------- */
/* Identifies the chip, resets it and reads its calibration */
static NOINLINE int32_t identify(uint32_t now) {
    uint8_t b[42];
    int32_t r = reg_read(REG_CHIP_ID, b, 1);
    if (r) return r;
    if (b[0] != CHIP_ID) {
        dev_fail(now, "another chip (its id is not 0x61) answers", 0);
        return 1;
    }
    b[0] = REG_RESET;
    b[1] = 0xB6;
    if ((r = reg_write(b, 2))) return r;
    esp32_delay_ms(10);
    if ((r = reg_read(REG_VARIANT, b, 1))) return r;
    variant = b[0];
    if ((r = reg_read(REG_COEFF1, b, 23)) || (r = reg_read(REG_COEFF2, b + 23, 14)) || (r = reg_read(REG_COEFF3, b + 37, 5))) return r;
    int32_t v[26];
    _Static_assert(sizeof v == sizeof c, "calib_at lists every calibration value");
    for (int i = 0; i < 26; i++) {
        const uint8_t* at = b + (calib_at[i] >> 2);
        const int type = calib_at[i] & 3;
        v[i] = type == 0 ? (int8_t)at[0] : type == 1 ? at[0] : type == 2 ? (int16_t)(at[0] | at[1] << 8) : (at[0] | at[1] << 8);
    }
    __builtin_memcpy(&c, v, sizeof c);
    c.h1 = b[25] << 4 | (b[24] & 0x0F);
    c.h2 = b[23] << 4 | b[24] >> 4;
    c.res_heat_range = b[39] >> 4 & 3;
    c.range_sw_err = (int8_t)b[41] >> 4;
    found = 1;
    if (state == 0) return 0;                          /* back: dev_ok says so */
    out_len = 0;
    put(DRIVER ": "); put(variant == 1 ? "BME688" : "BME680"); put_where();
    if (gas) { put(", heater "); put_uint(heater_c); put(" C for "); put_uint(heater_ms); put(" ms"); }
    log_out();
    return 0;
}

/* Starts a forced measurement: register and value pairs in one write, the
 * measurement mode last */
static int32_t trigger(void) {
    uint8_t w[12] = {
        REG_CTRL_HUM, CTRL_HUM,
        REG_RES_HEAT0, calc_res_heat(heater_c),
        REG_GAS_WAIT0, calc_gas_wait(heater_ms),
        REG_CTRL_GAS_0, gas ? 0x00 : 0x08,                               /* heat_off */
        REG_CTRL_GAS_1, gas ? (variant == 1 ? 0x20 : 0x10) : 0x00,        /* run_gas, heater step 0 */
        REG_CTRL_MEAS, CTRL_MEAS | 0x01,                                  /* forced mode */
    };
    return reg_write(w, sizeof w);
}

static void start(void) {
    heater_c = env_num("CONFIG_HEATER_C", 300, 0);
    if (heater_c < 200 || heater_c > 400) heater_c = 300;
    heater_ms = env_num("CONFIG_HEATER_MS", 100, 0);
    if (heater_ms < 1 || heater_ms > 4032) heater_ms = 100;
    gas = env_word("CONFIG_GAS", "off\0on\0", 1);
}

static int poll(uint32_t now) {
    int32_t r = 0;
    if (!found && (r = identify(now))) {
        if (r < 0) dev_fail(now, "no answer", r);
        return 0;
    }
    if (!measuring) {
        if ((r = trigger())) {
            found = 0;
            dev_fail(now, "no answer", r);
            return 0;
        }
        measuring = 1;
        started = now;
        return 1;
    }
    const uint32_t due = TPH_MS + (gas ? heater_ms : 0);
    if (now - started < due) return 1;
    uint8_t f[17];
    if ((r = reg_read(REG_FIELD0, f, sizeof f))) {
        measuring = found = 0;
        dev_fail(now, "no answer", r);
        return 0;
    }
    if (!(f[0] & 0x80)) {                              /* new_data */
        if (now - started < due + 500) return 1;
        measuring = found = 0;
        dev_fail(now, "no measurement came in", 0);
        return 0;
    }
    measuring = 0;
    temp_x100 = calc_temperature((uint32_t)f[5] << 12 | (uint32_t)f[6] << 4 | f[7] >> 4);
    press_pa = calc_pressure((uint32_t)f[2] << 12 | (uint32_t)f[3] << 4 | f[4] >> 4);
    hum_milli = calc_humidity((uint16_t)(f[8] << 8 | f[9]));
    const uint8_t* g = variant == 1 ? f + 15 : f + 13;  /* gas_r: 0x2C/0x2D, on the BME680 0x2A/0x2B */
    const uint16_t gas_adc = (uint16_t)(g[0] << 2 | g[1] >> 6);
    gas_ok = gas && (g[1] & 0x30) == 0x30;           /* gas_valid and heat_stab */
    gas_ohms = variant == 1 ? calc_gas_resistance_high(gas_adc, g[1] & 0x0F) : calc_gas_resistance_low(gas_adc, g[1] & 0x0F);
    amb_c = temp_x100 / 100;
    dev_ok(now);
    publish();
    return 0;
}

static void put_readings(void) {
    key("temperature_c"); put_fixed(temp_x100, 2);
    key("pressure_hpa"); put_fixed(press_pa, 2);
    key("humidity_pct"); put_fixed((hum_milli + 5) / 10, 2);
    if (gas) {
        key("gas_ohms");
        if (gas_ok) put_uint(gas_ohms);
        else put("null");
    }
}
