# alfs: `make test` runs the host suite on 8 simulator seeds under the
# address and undefined behaviour sanitizers; `make soak` on 100 seeds
# without them; `make target` checks the Cortex-M0+ budgets of SIMP-02 to
# SIMP-04. T=name runs the tests whose names contain it; SEEDS="..." the
# seeds given. `make probe` builds the flash probe (docs/TEST_PLAN.md 7).

CC ?= cc
ARMCC ?= arm-none-eabi-gcc
ARMSIZE ?= arm-none-eabi-size
WARN = -std=c99 -Wall -Wextra -Werror -pedantic
CFLAGS = $(WARN) -O1 -g -I. -Itests
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
PICO_SDK_PATH ?= $(HOME)/.pico-sdk/sdk/2.2.0
PICO_TOOLCHAIN_PATH ?= $(HOME)/.pico-sdk/toolchain/14_2_Rel1

all: test

build:
	mkdir -p build

build/test_alfs: alfs.c alfs.h tests/test_alfs.c tests/sim.c tests/sim.h tests/tf.c tests/tf.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ alfs.c tests/test_alfs.c tests/sim.c tests/tf.c

SEEDS ?= 1 2 3 4 5 6 7 8
JOBS ?= 8

# Each seed in its own process, JOBS at a time; a failing seed's log is shown.
define run_seeds
	@rm -f build/seed_*.log; \
	echo $(2) | tr ' ' '\n' | xargs -P $(JOBS) -I{} sh -c 'ALFS_SEED={} $(1) $(T) > build/seed_{}.log 2>&1 || mv build/seed_{}.log build/seed_{}.fail'; \
	fails=$$(ls build/seed_*.fail 2>/dev/null); \
	for f in $$fails; do echo "== $$f"; grep -v '^ok' $$f; done; \
	n=$$(echo $(2) | wc -w); echo "$$n seeds, $$(echo $$fails | wc -w) failing"; \
	rm -f build/seed_*.fail; [ -z "$$fails" ]
endef

test: build/test_alfs
	$(call run_seeds,./build/test_alfs,$(SEEDS))

build/test_alfs_fast: alfs.c alfs.h tests/test_alfs.c tests/sim.c tests/sim.h tests/tf.c tests/tf.h | build
	$(CC) $(WARN) -O2 -I. -Itests -o $@ alfs.c tests/test_alfs.c tests/sim.c tests/tf.c

soak: build/test_alfs_fast
	$(call run_seeds,./build/test_alfs_fast,$(shell seq 1 100))

# The tests alone, compiled: what exists before the code does.
tests-compile: | build
	$(CC) $(CFLAGS) -c tests/test_alfs.c -o build/test_alfs.o
	$(CC) $(CFLAGS) -c tests/sim.c -o build/sim.o
	$(CC) $(CFLAGS) -c tests/tf.c -o build/tf.o

target: | build
	$(ARMCC) $(WARN) -mcpu=cortex-m0plus -mthumb -Os -fcallgraph-info=su -I. -c alfs.c -o build/alfs_m0.o
	$(ARMCC) $(WARN) -mcpu=cortex-m0plus -mthumb -Os -I. -c tests/target_sizes.c -o build/target_sizes.o
	$(ARMSIZE) build/alfs_m0.o
	sh tests/check_target.sh build/alfs_m0.o build/alfs_m0.ci

probe:
	cmake -S probe -B build/probe -G Ninja -DPICO_SDK_PATH=$(PICO_SDK_PATH) -DPICO_TOOLCHAIN_PATH=$(PICO_TOOLCHAIN_PATH)
	cmake --build build/probe

clean:
	rm -rf build

.PHONY: all test soak tests-compile target probe clean
