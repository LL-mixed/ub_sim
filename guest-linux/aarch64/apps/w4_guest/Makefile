CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
W5_MEM_SERVICE := $(ROOT)/components/w5_mem_service/w4_kvcache_db_service.c

all: linqu_w4_guest

linqu_w4_guest: w4_guest.c $(W5_MEM_SERVICE)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue $^ -lm -o $@

clean:
	rm -f linqu_w4_guest

.PHONY: all clean
