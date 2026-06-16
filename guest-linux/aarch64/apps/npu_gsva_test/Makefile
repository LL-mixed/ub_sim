CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: npu_gsva_test

npu_gsva_test: npu_gsva_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f npu_gsva_test

.PHONY: all clean
