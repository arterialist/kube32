# Host checks of the models' C code against their test vectors, with the
# node's own linear algebra kernels (src/runtime/pod_linalg.cpp).
#
#   make -C models check     build and run every check
REPO = ..
B = build
WASM3 = $(REPO)/third_party/wasm3/source
CC ?= cc
CXX ?= c++
CFLAGS = -O2 -ffunction-sections -fdata-sections -ffp-contract=off -Wall -Wextra
CXXFLAGS = -std=c++17 -O2 -ffunction-sections -fdata-sections -ffp-contract=off -I$(REPO)/include -I$(WASM3)
comma = ,
GC = $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)
LINALG = $(B)/pod_linalg.o $(B)/k32_host.o

all: $(B)/kws_test $(B)/frontend_test $(B)/doodle_test $(B)/mdlm_test

$(B)/pod_linalg.o: $(REPO)/src/runtime/pod_linalg.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/k32_host.o: common/k32_host.cpp common/k32_host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/%.o: kws/%.c kws/*.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/doodle_%.o: doodle/doodle_%.c doodle/*.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/mdlm_%.o: mdlm/mdlm_%.c mdlm/*.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/mdlm_test: $(B)/mdlm_test.o $(B)/mdlm_infer.o $(LINALG)
	$(CXX) $(GC) -o $@ $^

$(B)/frontend_test: $(B)/frontend_test.o $(B)/frontend.o $(LINALG)
	$(CXX) $(GC) -o $@ $^

$(B)/kws_test: $(B)/kws_test.o $(B)/kws_infer.o $(B)/frontend.o $(LINALG)
	$(CXX) $(GC) -o $@ $^

$(B)/doodle_test: $(B)/doodle_test.o $(B)/doodle_infer.o $(LINALG)
	$(CXX) $(GC) -o $@ $^

check: all
	$(B)/frontend_test kws/kws.bin kws/vectors/kws-audio.bin kws/vectors/kws-features.bin
	$(B)/kws_test kws/kws.bin kws/vectors/kws-audio.bin kws/vectors/kws-features.bin kws/vectors/kws-logits.bin
	$(B)/doodle_test doodle/doodle-s.bin doodle/vectors/doodle-s-samples.bin
	$(B)/doodle_test doodle/doodle-l.bin doodle/vectors/doodle-l-samples.bin
	$(B)/mdlm_test mdlm/mdlm.bin mdlm/vectors/mdlm-samples.bin

clean:
	rm -rf $(B)

.PHONY: all check clean
