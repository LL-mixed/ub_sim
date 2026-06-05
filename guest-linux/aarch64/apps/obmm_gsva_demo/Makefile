CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: obmm_gsva_demo

obmm_gsva_demo: obmm_gsva_demo.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f obmm_gsva_demo

.PHONY: all clean
