CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: ssd_gsva_test

ssd_gsva_test: ssd_gsva_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f ssd_gsva_test

.PHONY: all clean
