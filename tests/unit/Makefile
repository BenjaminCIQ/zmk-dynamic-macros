# Standalone host build for the pure-core unit tests.
#
# Plain `cc`, NO Zephyr — this IS the CI rail for the pure core (a gcc job runs
# `make` here; see .github/workflows/host-tests.yml) as well as the sub-second
# local loop. It doubles as a decoupling proof: if a pure module stops compiling
# without Zephyr, `make` breaks here. (run-host.ps1 is the equivalent MSVC loop.)
#
# Usage (from tests/unit/):
#   make          # build + run
#   make run      # alias
#   make clean

CC      ?= cc
REPO    := ../..
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 \
           -DZTEST_SHIM_HOST \
           -I. -I$(REPO)/include

# Every test_*.c is a unit-test translation unit. host_runner.c provides main()
# and the registry storage.
TEST_SRCS := $(wildcard test_*.c)

# Pure-core module sources under test, compiled directly (Zephyr-free). Add each
# pure module's .c here as it is added (mirrors run-host.ps1).
MODULE_SRCS := $(REPO)/src/dm_render.c $(REPO)/src/slot_store.c $(REPO)/src/dm_machine.c \
               $(REPO)/src/dm_feedback_build.c $(REPO)/src/dm_query.c

SRCS      := host_runner.c $(TEST_SRCS) $(MODULE_SRCS)

BIN := dm_unit_host

.PHONY: all run clean
all: run

$(BIN): $(SRCS) ztest_shim.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
