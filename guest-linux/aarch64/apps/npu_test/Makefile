CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: npu_test

npu_test: npu_test.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f npu_test

.PHONY: all clean
