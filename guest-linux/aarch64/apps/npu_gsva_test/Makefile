CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: npu_gsva_test

npu_gsva_test: npu_gsva_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f npu_gsva_test

.PHONY: all clean
