# alfs: `make test` runs the host suite under the address and undefined
# behaviour sanitizers; `make target` checks the Cortex-M0+ budgets of
# SIMP-02 to SIMP-04. T=name runs the tests whose names contain it.

CC ?= cc
ARMCC ?= arm-none-eabi-gcc
ARMSIZE ?= arm-none-eabi-size
WARN = -std=c99 -Wall -Wextra -Werror -pedantic
CFLAGS = $(WARN) -O1 -g -I. -Itests
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

all: test

build:
	mkdir -p build

build/test_alfs: alfs.c alfs.h tests/test_alfs.c tests/sim.c tests/sim.h tests/tf.c tests/tf.h | build
	$(CC) $(CFLAGS) $(SAN) -o $@ alfs.c tests/test_alfs.c tests/sim.c tests/tf.c

test: build/test_alfs
	./build/test_alfs $(T)

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

clean:
	rm -rf build

.PHONY: all test tests-compile target clean
