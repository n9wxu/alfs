# alfs: `make test` runs the host suite on 8 simulator seeds under the
# address and undefined behaviour sanitizers; `make soak` on 100 seeds
# without them; `make target` checks the Cortex-M0+ budgets of SIMP-02 to
# SIMP-04. `make test-rt` and `make target-rt` do the same for the flash
# task (alfs_rtos.c). T=name runs the tests whose names contain it;
# SEEDS="..." the seeds given. `make probe` builds the flash probe
# (docs/TEST_PLAN.md 7).

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

# The flash task (alfs_rtos.c) against FreeRTOS-Kernel, fetched once into
# build/_deps. ALFS_RT_KERNEL names another copy; FREERTOS_KERNEL_PATH is
# deliberately not read, since a shell may export a stale one.
ALFS_RT_KERNEL ?= build/_deps/FreeRTOS-Kernel
RT_TAG = V11.3.1
RT_POSIX = $(ALFS_RT_KERNEL)/portable/ThirdParty/GCC/Posix
RT_KSRC = $(ALFS_RT_KERNEL)/tasks.c $(ALFS_RT_KERNEL)/queue.c $(ALFS_RT_KERNEL)/list.c \
	$(RT_POSIX)/port.c $(RT_POSIX)/utils/wait_for_event.c
RT_INC = -I. -Itests -Itests/rtos -I$(ALFS_RT_KERNEL)/include -I$(RT_POSIX) -I$(RT_POSIX)/utils

$(ALFS_RT_KERNEL):
	git clone -q --depth 1 --branch $(RT_TAG) https://github.com/FreeRTOS/FreeRTOS-Kernel.git $@

build/rt_kernel.a: | build $(ALFS_RT_KERNEL)
	cd build && rm -f rt_*.o && for f in $(abspath $(RT_KSRC)); do \
		$(CC) -std=gnu99 -O1 -g -w $(addprefix -I$(CURDIR)/,$(patsubst -I%,%,$(RT_INC))) -c $$f -o rt_$$(basename $$f .c).o || exit 1; done
	ar rcs $@ build/rt_*.o

build/test_rtos: alfs.c alfs.h alfs_rtos.c alfs_rtos.h tests/test_rtos.c tests/sim.c tests/sim.h tests/tf.c tests/rtos/FreeRTOSConfig.h build/rt_kernel.a
	$(CC) -std=gnu99 -Wall -Wextra -Werror -O1 -g $(SAN) $(RT_INC) -o $@ alfs.c alfs_rtos.c tests/test_rtos.c tests/sim.c tests/tf.c build/rt_kernel.a -lpthread

test-rt: build/test_rtos
	$(call run_seeds,./build/test_rtos,$(SEEDS))

# SIMP-02 and SIMP-04 for the task layer, on the kernel's generic Cortex-M0 port.
RT_M0_INC = -I. -Itests/rtos_m0 -I$(ALFS_RT_KERNEL)/include -I$(ALFS_RT_KERNEL)/portable/GCC/ARM_CM0

target-rt: target | $(ALFS_RT_KERNEL)
	$(ARMCC) -std=c99 -Wall -Wextra -Werror -mcpu=cortex-m0plus -mthumb -Os -fcallgraph-info=su $(RT_M0_INC) -c alfs_rtos.c -o build/alfs_rtos_m0.o
	$(ARMSIZE) build/alfs_rtos_m0.o
	sh tests/check_target_rt.sh build/alfs_rtos_m0.o build/alfs_rtos_m0.ci build/alfs_m0.ci

.PHONY: all test soak tests-compile target test-rt target-rt probe clean
