# knotwatch build.
#
#   make              host build: portable core + `knotwatch analyze`
#   make check        run the core test suite (any host)
#   make QNX=1 ...    QNX OS 8.0 build: adds scan/capture/watch and the demo
#
# QNX builds:
#   on the target (self-hosted QNX Developer Desktop):   make
#   cross, QNX SDP 8.0 environment sourced:
#       make QNX=1 CC=ntox86_64-gcc      BUILD=build/x86_64
#       make QNX=1 CC=ntoaarch64-gcc     BUILD=build/aarch64le

CC      ?= cc
BUILD   ?= build
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
ALL_CFLAGS := -std=gnu11 $(WARN) -Iinclude $(CFLAGS)

ifeq ($(shell uname -s),QNX)
QNX ?= 1
endif

CORE_SRC := $(wildcard src/core/*.c)
CLI_SRC  := src/cli/main.c
BINS     := $(BUILD)/knotwatch

ifeq ($(QNX),1)
CLI_SRC    += $(wildcard src/qnx/*.c)
ALL_CFLAGS += -DKW_QNX
LDLIBS     += -lslog2
BINS       += $(patsubst demo/%.c,$(BUILD)/%,$(wildcard demo/*.c))
endif

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
CLI_OBJ  := $(CLI_SRC:%.c=$(BUILD)/%.o)

.PHONY: all check clean
all: $(BINS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/knotwatch: $(CORE_OBJ) $(CLI_OBJ)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Each demo/<name>.c is one demo program.
$(BUILD)/%: $(BUILD)/demo/%.o
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/test_core: $(CORE_OBJ) $(BUILD)/tests/test_core.o
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $^ -o $@

check: $(BUILD)/test_core $(BUILD)/knotwatch
	$(BUILD)/test_core tests/fixtures
	sh tests/test_cli.sh $(BUILD)/knotwatch

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
