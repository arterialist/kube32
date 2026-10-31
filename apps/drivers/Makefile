# Builds every device driver into images/driver-<name>.wasm (see README.md).
DRIVERS = bme688 miniscale rfid chip-temp button switch

all clean:
	@for d in $(DRIVERS); do $(MAKE) -C $$d $@ || exit 1; done

.PHONY: all clean
