CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
MEM_SERVICE := $(ROOT)/components/mem_service/w4_kvcache_db_service.c
LLM_INFER := $(ROOT)/components/llm_infer/llm_infer.c

all: linqu_w4_guest

linqu_w4_guest: w4_guest.c $(MEM_SERVICE) $(LLM_INFER)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue $^ -lm -o $@

clean:
	rm -f linqu_w4_guest

.PHONY: all clean
