CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: obmm_coh_test

obmm_coh_test: obmm_coh_test.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f obmm_coh_test

.PHONY: all clean
