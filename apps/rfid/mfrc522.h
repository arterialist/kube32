/* MFRC522-compatible 13.56 MHz front ends on I2C (the M5Stack RFID 2 unit's
 * WS1850S answers at 0x28): reader setup and the ISO 14443-A exchange that
 * reads the UID of one tag (4, 7 or 10 bytes) and halts it. Shared by
 * apps/rfid and apps/drivers/rfid.
 *
 * The including file defines the reader's bus and address before including
 * this one:
 *   static uint32_t bus, addr; */

/* MFRC522 registers and commands */
#define REG_COMMAND    0x01
#define REG_COM_IRQ    0x04
#define REG_ERROR      0x06
#define REG_FIFO_DATA  0x09
#define REG_FIFO_LEVEL 0x0A
#define REG_CONTROL    0x0C
#define REG_BIT_FRAMING 0x0D
#define REG_COLL       0x0E
#define REG_MODE       0x11
#define REG_TX_MODE    0x12
#define REG_RX_MODE    0x13
#define REG_TX_CONTROL 0x14
#define REG_TX_ASK     0x15
#define REG_MOD_WIDTH  0x24
#define REG_T_MODE     0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_H 0x2C
#define REG_T_RELOAD_L 0x2D
#define REG_VERSION    0x37
#define CMD_IDLE       0x00
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

/* Results of a frame exchange */
#define ST_OK        0
#define ST_TIMEOUT  -1
#define ST_ERROR    -2
#define ST_COLLISION -3
#define ST_BUS      -4

struct tag { uint8_t uid[10]; uint8_t len; uint8_t sak; uint32_t seen_ms; };

static uint8_t version;
static uint32_t bus_errors;
static int32_t last_bus_error;

/* ---- Register access ---- */
static int bus_fail(int32_t r) {
    bus_errors++;
    last_bus_error = r;
    return ST_BUS;
}

static int wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    int32_t r = esp32_i2c_write(bus, addr, b, 2);
    return r == 2 ? 0 : bus_fail(r);
}

static int rd(uint8_t reg, uint8_t* val) {
    int32_t r = esp32_i2c_write(bus, addr, &reg, 1);
    if (r != 1) return bus_fail(r);
    r = esp32_i2c_read(bus, addr, val, 1);
    return r == 1 ? 0 : bus_fail(r);
}

static int set_bits(uint8_t reg, uint8_t mask, int on) {
    uint8_t v;
    if (rd(reg, &v) != 0) return ST_BUS;
    return wr(reg, on ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask));
}

/* ---- ISO 14443-A ---- */
static uint16_t crc_a(const uint8_t* p, uint32_t n) {
    uint16_t crc = 0x6363;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)(p[i] ^ (uint8_t)crc);
        ch = (uint8_t)(ch ^ (uint8_t)(ch << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)ch << 8) ^ ((uint16_t)ch << 3) ^ (ch >> 4));
    }
    return crc;
}

/* Sends `n` bytes (the last one `last_bits` long, 0 = 8) and reads the answer
 * into `out` (at most *out_len bytes); *out_len receives its length. The
 * reader's timer ends the wait after 25 ms. */
static int transceive(const uint8_t* data, uint32_t n, uint8_t last_bits, uint8_t* out, uint32_t* out_len) {
    uint8_t fifo[1 + 16];
    if (n > 16) return ST_ERROR;
    if (wr(REG_COMMAND, CMD_IDLE) || wr(REG_COM_IRQ, 0x7F) || wr(REG_FIFO_LEVEL, 0x80)) return ST_BUS;
    fifo[0] = REG_FIFO_DATA;
    for (uint32_t i = 0; i < n; i++) fifo[1 + i] = data[i];
    if (esp32_i2c_write(bus, addr, fifo, n + 1) != (int32_t)(n + 1)) return ST_BUS;
    if (wr(REG_BIT_FRAMING, last_bits) || wr(REG_COMMAND, CMD_TRANSCEIVE) || wr(REG_BIT_FRAMING, (uint8_t)(0x80 | last_bits))) return ST_BUS;
    uint8_t irq = 0;
    int done = 0;
    for (int i = 0; i < 200 && !done; i++) {
        if (rd(REG_COM_IRQ, &irq)) return ST_BUS;
        if (irq & 0x30) done = 1;            /* RxIRq or IdleIRq: answered */
        else if (irq & 0x01) return ST_TIMEOUT; /* TimerIRq: nobody answered */
    }
    wr(REG_BIT_FRAMING, last_bits);         /* StartSend off */
    if (!done) return ST_TIMEOUT;
    uint8_t err = 0, level = 0;
    if (rd(REG_ERROR, &err)) return ST_BUS;
    if (err & 0x13) return ST_ERROR;        /* buffer overflow, parity or protocol error */
    if (err & 0x08) return ST_COLLISION;
    if (rd(REG_FIFO_LEVEL, &level)) return ST_BUS;
    if (level > *out_len) return ST_ERROR;
    if (level) {
        uint8_t reg = REG_FIFO_DATA;
        if (esp32_i2c_write(bus, addr, &reg, 1) != 1 || esp32_i2c_read(bus, addr, out, level) != level) return ST_BUS;
    }
    *out_len = level;
    return ST_OK;
}

/* WUPA, then one cascade level after another until the UID is complete,
 * then HLTA. Halted tags answer WUPA again, so a tag that stays in the field
 * is read on every poll. */
static int read_tag(struct tag* t) {
    uint8_t cmd[9], rx[8];
    uint32_t len = sizeof rx;
    set_bits(REG_COLL, 0x80, 0);            /* ValuesAfterColl off */
    cmd[0] = 0x52;
    int st = transceive(cmd, 1, 7, rx, &len);
    if (st != ST_OK) return st;
    if (len != 2) return ST_ERROR;
    t->len = 0;
    for (uint8_t level = 0; level < 3; level++) {
        const uint8_t sel = (uint8_t)(0x93 + 2 * level);
        cmd[0] = sel; cmd[1] = 0x20;
        len = sizeof rx;
        st = transceive(cmd, 2, 0, rx, &len);
        if (st != ST_OK) return st;
        if (len != 5 || (uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) return ST_ERROR;
        cmd[0] = sel; cmd[1] = 0x70;
        for (int i = 0; i < 5; i++) cmd[2 + i] = rx[i];
        uint16_t crc = crc_a(cmd, 7);
        cmd[7] = (uint8_t)crc; cmd[8] = (uint8_t)(crc >> 8);
        uint8_t sak[3];
        uint32_t sl = sizeof sak;
        st = transceive(cmd, 9, 0, sak, &sl);
        if (st != ST_OK) return st;
        if (sl != 3 || crc_a(sak, 1) != (uint16_t)(sak[1] | (sak[2] << 8))) return ST_ERROR;
        const int cascade = (rx[0] == 0x88);   /* cascade tag: three UID bytes follow */
        for (int i = cascade ? 1 : 0; i < 4; i++) t->uid[t->len++] = rx[i];
        t->sak = sak[0];
        if (!(sak[0] & 0x04)) break;           /* UID complete */
    }
    cmd[0] = 0x50; cmd[1] = 0x00;
    uint16_t crc = crc_a(cmd, 2);
    cmd[2] = (uint8_t)crc; cmd[3] = (uint8_t)(crc >> 8);
    len = sizeof rx;
    transceive(cmd, 4, 0, rx, &len);           /* a halted tag does not answer */
    return ST_OK;
}

static int init_reader(void) {
    if (wr(REG_COMMAND, CMD_SOFT_RESET)) return ST_BUS;
    esp32_delay_ms(50);
    if (rd(REG_VERSION, &version)) return ST_BUS;
    if (wr(REG_TX_MODE, 0x00) || wr(REG_RX_MODE, 0x00) || wr(REG_MOD_WIDTH, 0x26)) return ST_BUS;
    /* Timer: 25 ms per frame exchange */
    if (wr(REG_T_MODE, 0x80) || wr(REG_T_PRESCALER, 0xA9) || wr(REG_T_RELOAD_H, 0x03) || wr(REG_T_RELOAD_L, 0xE8)) return ST_BUS;
    if (wr(REG_TX_ASK, 0x40) || wr(REG_MODE, 0x3D)) return ST_BUS;
    return set_bits(REG_TX_CONTROL, 0x03, 1);   /* antenna on */
}
