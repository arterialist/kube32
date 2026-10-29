# Tessera: four pods and a dataset.
#
#   make            pod images: images/tessera-{api,db,web,hud}.wasm and the
#                   web bundle images/web/tessera-web/
#   make test       native host tests of tessera-db, tessera-api and tessera-web
#   make dataset    sample data for tessera-db in build/card/ (copy build/card/kube32
#                   to the root of the Cardputer's microSD card)
#   make size       memory layout and load cost of tessera-api, layout checks of the small pods
ISSUES   ?= 2400
COMMENTS ?= 4800
EPOCH_MS ?= 1793059200000
CARD     ?= build/card

.PHONY: all images test dataset size clean
all: images

images:
	$(MAKE) -C api wasm
	$(MAKE) -C db wasm
	$(MAKE) -C web all
	$(MAKE) -C hud

test:
	$(MAKE) -C db test
	$(MAKE) -C api test
	$(MAKE) -C web test

dataset:
	$(MAKE) -C tools/dataset build/dataset
	tools/dataset/build/dataset --issues $(ISSUES) --comments $(COMMENTS) --epoch $(EPOCH_MS) --out $(CARD)/kube32/volumes/tessera-db

size:
	$(MAKE) -C api size
	$(MAKE) -C db wasm
	$(MAKE) -C web wasm
	$(MAKE) -C hud

clean:
	$(MAKE) -C api clean
	$(MAKE) -C db clean
	$(MAKE) -C web clean
	$(MAKE) -C hud clean
	$(MAKE) -C tools/dataset clean
	rm -rf build
