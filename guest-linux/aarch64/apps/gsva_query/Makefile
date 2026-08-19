CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../../common/libobmm.mk

all: gsva_query

gsva_query: gsva_query.c
	$(CC) $(CFLAGS) -I$(ROOT)/kernel_ub/include -I$(ROOT)/common $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f gsva_query

.PHONY: all clean
