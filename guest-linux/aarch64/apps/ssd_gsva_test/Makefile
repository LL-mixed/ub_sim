CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: ssd_gsva_test

ssd_gsva_test: ssd_gsva_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) \
		$(OBMM_LDLIBS) -o $@

clean:
	rm -f ssd_gsva_test

.PHONY: all clean
