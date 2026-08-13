ROOT_DIR := $(abspath ../..)
OUT_DIR ?= $(ROOT_DIR)/out/obmm_scc
PREFIX ?= aarch64-linux-gnu-
CC = $(PREFIX)gcc
AR = $(PREFIX)ar
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -fno-stack-protector
CPPFLAGS += -I. -idirafter $(ROOT_DIR)/../kernel_ub/include/uapi

LIB := $(OUT_DIR)/lib/libobmm_scc.a
OBJ := $(OUT_DIR)/obj/obmm_scc.o \
	$(OUT_DIR)/obj/obmm_scc_aarch64.o

.PHONY: all clean

all: $(LIB)

$(OUT_DIR)/obj/obmm_scc.o: obmm_scc.c obmm_scc.h README.md
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/obj/obmm_scc_aarch64.o: obmm_scc_aarch64.S obmm_scc.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

clean:
	rm -rf $(OUT_DIR)
