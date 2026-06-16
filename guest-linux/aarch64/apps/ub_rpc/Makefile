CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: ub_rpc

ub_rpc: ub_rpc.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ub_rpc

.PHONY: all clean
