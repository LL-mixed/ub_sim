CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: obmm_gsva

obmm_gsva: obmm_gsva.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f obmm_gsva

.PHONY: all clean
