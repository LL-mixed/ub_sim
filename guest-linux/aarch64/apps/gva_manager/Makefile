CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

all: gva_manager

gva_manager: gva_manager.c
	$(CC) $(CFLAGS) -I$(ROOT)/common -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue_demo $< -o $@

clean:
	rm -f gva_manager

.PHONY: all clean
