CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static

all: ub_tcp_each_server

ub_tcp_each_server: ub_tcp_each_server.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ub_tcp_each_server

.PHONY: all clean
