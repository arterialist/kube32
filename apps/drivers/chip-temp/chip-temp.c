/* driver-chip-temp: the ESP32-S3's internal temperature sensor. It measures
 * the die, which runs above the room's temperature while the CPU and the
 * radio work; it tells how warm the board runs, not how warm the room is.
 *
 * Each tick reads the sensor (esp32_chip_temp_read, granted by
 * hardware.kube32.io/chip-temp) and publishes {"temperature_c":41.5}. */
#define DRIVER "chip-temp"
#define INTERVAL_MS 10000
#define DESCRIBE "{\"driver\":\"chip-temp\",\"readings\":{\"temperature_c\":\"C\"},\"actions\":[]}"
#include "../common/driver.h"

static int32_t milli_c;

static void start(void) {
    out_len = 0;
    put(DRIVER ": the chip's temperature every "); put_uint(interval_ms); put(" ms");
    log_out();
}

static int poll(uint32_t now) {
    int32_t r = esp32_chip_temp_read(&milli_c);
    if (r < 0) {
        dev_fail(now, "the temperature sensor did not answer", r);
        return 0;
    }
    dev_ok(now);
    publish();
    return 0;
}

/* Tenths of a degree, the sensor's resolution */
static void put_readings(void) {
    key("temperature_c");
    put_fixed((milli_c + (milli_c < 0 ? -50 : 50)) / 100, 1);
}
