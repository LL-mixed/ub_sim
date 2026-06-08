CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: gsva_query

gsva_query: gsva_query.c
	$(CC) $(CFLAGS) -I$(ROOT)/kernel_ub/include $< -o $@

clean:
	rm -f gsva_query

.PHONY: all clean
