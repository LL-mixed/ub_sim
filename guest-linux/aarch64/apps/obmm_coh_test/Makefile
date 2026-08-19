CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: obmm_coh_test

obmm_coh_test: obmm_coh_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f obmm_coh_test

.PHONY: all clean
