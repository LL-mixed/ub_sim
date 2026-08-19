CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: obmm_dataplane_microbench

obmm_dataplane_microbench: obmm_dataplane_microbench.c
	$(CC) $(CFLAGS) -I$(ROOT)/kernel_ub/include/uapi -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f obmm_dataplane_microbench

.PHONY: all clean
