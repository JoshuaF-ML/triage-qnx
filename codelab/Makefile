# Codelab -- standalone. It shares nothing with the main project's build,
# because the point is that you can follow the codelab without reading
# Triage first.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make                        # x86_64, for the QEMU target
#   make ARCH=aarch64le         # for a Raspberry Pi 4/5 target
#   make clean

ARCH    ?= x86_64
VARIANT := gcc_nto$(ARCH)

CC      := qcc
CFLAGS  := -V$(VARIANT) -Wall -Wextra -Werror -O2 -g -std=gnu11 -D_QNX_SOURCE
LDFLAGS := -V$(VARIANT)

BUILD   := build/$(ARCH)
BINS    := $(BUILD)/step1_hello $(BUILD)/step2_prio $(BUILD)/step3_pool \
           $(BUILD)/ask

.PHONY: all clean
all: $(BINS)
	@echo
	@echo "built in $(BUILD):"
	@ls -1 $(BUILD) | grep -v '\.o$$'

$(BUILD)/%: %.c clab.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -rf build
