/* driver-rfid: ISO 14443-A tags through an MFRC522-compatible reader on I2C,
 * such as the M5Stack RFID 2 unit (WS1850S, 0x28). The reader code is
 * apps/rfid's (../../rfid/mfrc522.h).
 *
 * Each tick wakes the tags in the field, reads the UID of one and halts it.
 * A tag that comes into the field is published, and so is the field going
 * empty (no tag for three polls). Readings: tag (the last UID in lower-case
 * hex, null before the first) and present. A reader that stops answering is
 * set up again on the next tick. */
#define DRIVER "rfid"
#define INTERVAL_MS 200
#define I2C_DEFAULT_ADDR 0x28
#define DESCRIBE "{\"driver\":\"rfid\",\"readings\":{\"tag\":\"uid\",\"present\":\"bool\"},\"actions\":[]}"
#include "../common/driver.h"
#include "../../rfid/mfrc522.h"

#define MISSES_UNTIL_GONE 3

static int reader_ok, present, misses;
static struct tag last;   /* len 0: no tag read yet */

static void put_uid(void) {
    const char* hex = "0123456789abcdef";
    for (uint32_t i = 0; i < last.len; i++) {
        put_n(&hex[last.uid[i] >> 4], 1);
        put_n(&hex[last.uid[i] & 15], 1);
    }
}

static void start(void) {}

static int poll(uint32_t now) {
    if (!reader_ok) {
        if (init_reader() != ST_OK) {
            dev_fail(now, "no reader answers", last_bus_error);
            return 0;
        }
        reader_ok = 1;
        if (state != 0) {                              /* back: dev_ok says so */
            out_len = 0;
            put(DRIVER ": reader"); put_where(); put(", version "); put_hex(version);
            log_out();
        }
    }
    struct tag t;
    const int st = read_tag(&t);
    if (st == ST_BUS) {
        reader_ok = present = 0;
        dev_fail(now, "the reader stopped answering", last_bus_error);
        return 0;
    }
    int changed = 0;
    if (st == ST_OK) {
        int same = present && t.len == last.len;
        for (uint8_t i = 0; same && i < t.len; i++) same = t.uid[i] == last.uid[i];
        misses = 0;
        if (!same) {
            last = t;
            present = changed = 1;
            out_len = 0;
            put(DRIVER ": tag "); put_uid();
            log_out();
        }
    } else if (present && ++misses >= MISSES_UNTIL_GONE) {
        present = 0;
        changed = 1;
    }
    if (dev_ok(now) || changed) publish();
    return 0;
}

static void put_readings(void) {
    key("tag");
    if (last.len) { put("\""); put_uid(); put("\""); }
    else put("null");
    key("present"); put_bool(present);
}
