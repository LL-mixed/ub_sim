CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: ub_udma

ub_udma: ub_udma.c
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. $< -o $@

clean:
	rm -f ub_udma

.PHONY: all clean
