"""The drivers of apps/drivers as an agent needs to know them.

`readings` and `actions` are each driver's GET /describe body (DESCRIBE in
apps/drivers/<driver>/<driver>.c). A running driver answers /describe itself,
and its answer takes precedence: a Device may run another image (spec.image).
`behaviour` holds what apps/drivers/README.md says about using the part.
"""

DRIVERS = {
    "bme688": {
        "part": "Bosch BME688 or BME680 environment sensor (M5Stack ENV Pro)",
        "bus": "I2C, address 0x77 (0x76 with SDO low)",
        "period_ms": 5000,
        "readings": {"temperature_c": "C", "pressure_hpa": "hPa", "humidity_pct": "%RH", "gas_ohms": "ohm"},
        "actions": [],
        "behaviour": [
            "Each reading is one forced-mode measurement (2x oversampling for temperature, 16x for pressure, 1x for "
            "humidity, no IIR filter) followed by one heater step for the gas resistance.",
            "gas_ohms is null when the heater did not reach its temperature or the gas reading is not valid. It falls "
            "when volatile compounds rise. A new sensor needs some hours of operation before it settles.",
            "Device config: heater-c (200 to 400, default 300), heater-ms (1 to 4032, default 100), gas (on or off).",
        ],
    },
    "miniscale": {
        "part": "M5Stack Unit Mini Scales, a 5 kg load cell",
        "bus": "I2C, address 0x26",
        "period_ms": 1000,
        "readings": {"weight_g": "g", "raw": "adc"},
        "actions": [{"action": "tare"}, {"action": "led", "r": "0-255", "g": "0-255", "b": "0-255"}],
        "behaviour": [
            "tare makes the current load zero; later readings are relative to it.",
            "led sets the unit's RGB LED; a color left out is 0.",
            "The unit keeps its own calibration. Firmware v4 also reports negative weights.",
        ],
    },
    "rfid": {
        "part": "MFRC522-compatible ISO 14443-A reader (M5Stack RFID 2)",
        "bus": "I2C, address 0x28",
        "period_ms": 200,
        "readings": {"tag": "uid", "present": "bool"},
        "actions": [],
        "behaviour": [
            "tag is the last UID read, in lowercase hex, or null before the first tag.",
            "present tells whether that tag is still in the field. The field counts as empty after three polls "
            "without a tag.",
            "The driver publishes an event when a tag enters the field and when the field empties.",
        ],
    },
    "chip-temp": {
        "part": "the ESP32-S3's internal temperature sensor",
        "bus": "internal",
        "period_ms": 10000,
        "readings": {"temperature_c": "C"},
        "actions": [],
        "behaviour": [
            "It measures the die, which is warmer than the room while the CPU and radio work. Resolution 0.1 C.",
            "Discovery does not create these Devices: write one for each node to read.",
        ],
    },
    "button": {
        "part": "a push button or contact on a GPIO input",
        "bus": "GPIO input",
        "period_ms": 50,
        "readings": {"pressed": "bool", "presses": "count"},
        "actions": [],
        "behaviour": [
            "A new level counts once it stays for debounce-ms (default 30). presses counts the presses since the "
            "driver started.",
            "The driver publishes each change, and the state every report-ms (default 60000) without one.",
            "Device config: pull (up, down or none), active (low or high), debounce-ms, report-ms.",
        ],
    },
    "switch": {
        "part": "a relay, MOSFET or LED on a GPIO output",
        "bus": "GPIO output",
        "period_ms": 30000,
        "readings": {"on": "bool"},
        "actions": [{"action": "set", "on": "bool"}, {"action": "toggle"}],
        "behaviour": [
            "The driver drives the pin again on each tick, and publishes each change and the state on each tick.",
            "The output holds only while the driver runs. An OnDemand switch stops idleSeconds after its last use "
            "and starts again in config initial (default off): set activation Always for an output that must stay set.",
            "Device config: initial (off or on), active (high or low).",
        ],
    },
}


def action_arguments(entry):
    """{argument: spec} of one /describe action entry, such as {"r": "0-255"} or {"on": "bool"}."""
    return {k: v for k, v in entry.items() if k != "action"}


def check_argument(spec, value):
    """Why `value` does not fit a /describe argument spec ("bool", "<min>-<max>", or text), or None."""
    if spec == "bool":
        return None if isinstance(value, bool) else "takes true or false"
    lo, sep, hi = str(spec).partition("-")
    if sep and lo.isdigit() and hi.isdigit():
        if isinstance(value, bool) or not isinstance(value, int):
            return "takes a whole number from %s to %s" % (lo, hi)
        if not int(lo) <= value <= int(hi):
            return "takes %s to %s, not %s" % (lo, hi, value)
    return None
