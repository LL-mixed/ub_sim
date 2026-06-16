CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: obmm_gsva

obmm_gsva: obmm_gsva.c
	$(CC) $(CFLAGS) -I$(ROOT)/common $< -o $@

clean:
	rm -f obmm_gsva

.PHONY: all clean
