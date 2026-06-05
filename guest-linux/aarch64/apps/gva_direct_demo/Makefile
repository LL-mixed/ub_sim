CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: gva_direct_demo

gva_direct_demo: gva_direct_demo.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f gva_direct_demo

.PHONY: all clean
