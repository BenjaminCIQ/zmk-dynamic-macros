# Standalone host build for the pure-core unit tests (redesign §4.1).
#
# Sub-second local red-green loop with plain cc and NO Zephyr. The SAME test
# .c files are also compiled as Ztest under `west test` (see testcase.yaml);
# this target is the fast loop and doubles as a decoupling proof — if a pure
# module stops compiling without Zephyr, `make` breaks here.
#
# Usage (from tests/unit/):
#   make          # build + run
#   make run      # alias
#   make clean

CC      ?= cc
REPO    := ../..
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 \
           -DZTEST_SHIM_HOST \
           -I. -I$(REPO)/include \
           -I$(REPO)/tests/parity -I$(REPO)/tests/parity/render

# Every test_*.c is a unit-test translation unit. host_runner.c provides main()
# and the registry storage. As pure-core modules are extracted (steps 1/3/4),
# add their Zephyr-free .c here alongside the test that drives them.
TEST_SRCS := $(wildcard test_*.c)
SRCS      := host_runner.c $(TEST_SRCS)

BIN := dm_unit_host

.PHONY: all run clean
all: run

$(BIN): $(SRCS) ztest_shim.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
