# The dataset tool: Tessera's sample data in tessera-db's on-card format.
CLANG ?= clang
SRC := main.c dataset.c seed.c ../../api/src/k32lib.c ../../db/src/tdb.c ../../common/tlog.c ../../common/host/shim.c
HDR := dataset.h seed.h ../../api/src/k32lib.h ../../db/src/tdb.h ../../common/tlog.h ../../common/host/shim.h ../../common/k32_host.h

build/dataset: $(SRC) $(HDR)
	@mkdir -p build
	$(CLANG) -O2 -std=c11 -Wall -Wextra -DK32_NATIVE -fno-builtin -DTLOG_CRC_TABLE256 -o $@ $(SRC)

clean:
	rm -rf build
.PHONY: clean
