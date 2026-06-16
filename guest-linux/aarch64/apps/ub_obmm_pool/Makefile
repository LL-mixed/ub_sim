CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: ub_obmm_pool

ub_obmm_pool: ub_obmm_pool.c
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. $< -o $@

clean:
	rm -f ub_obmm_pool

.PHONY: all clean
