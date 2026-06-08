CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: gsva_lifecycle_test

gsva_lifecycle_test: gsva_lifecycle_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f gsva_lifecycle_test

.PHONY: all clean
