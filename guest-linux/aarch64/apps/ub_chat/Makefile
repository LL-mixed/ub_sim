CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: ub_chat

ub_chat: ub_chat.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ub_chat

.PHONY: all clean
