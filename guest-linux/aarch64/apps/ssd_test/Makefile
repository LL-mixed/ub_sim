CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: ssd_test

ssd_test: ssd_test.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ssd_test

.PHONY: all clean
