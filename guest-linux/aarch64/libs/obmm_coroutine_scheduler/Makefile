ROOT_DIR := $(abspath ../..)
OUT_DIR ?= $(ROOT_DIR)/out/obmm_coroutine_scheduler
PREFIX ?= aarch64-linux-gnu-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -fno-stack-protector
CPPFLAGS += -I. -idirafter $(ROOT_DIR)/../kernel_ub/include/uapi

LIB := $(OUT_DIR)/lib/libobmm_coroutine_scheduler.a
OBJ := $(OUT_DIR)/obj/obmm_coroutine_scheduler.o \
	$(OUT_DIR)/obj/obmm_coroutine_scheduler_aarch64.o

.PHONY: all clean

all: $(LIB)

$(OUT_DIR)/obj/obmm_coroutine_scheduler.o: obmm_coroutine_scheduler.c obmm_coroutine_scheduler.h README.md
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/obj/obmm_coroutine_scheduler_aarch64.o: obmm_coroutine_scheduler_aarch64.S obmm_coroutine_scheduler.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

clean:
	rm -rf $(OUT_DIR)
