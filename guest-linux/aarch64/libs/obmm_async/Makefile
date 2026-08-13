CC = aarch64-linux-gnu-gcc
AR = aarch64-linux-gnu-ar
CFLAGS ?= -O2 -Wall -Wextra -Werror
ROOT := ../..
KERNEL_UAPI := ../../../kernel_ub/include/uapi
CPPFLAGS += -I. -idirafter $(KERNEL_UAPI)
BUILD_DIR ?= $(ROOT)/out/obmm_async/lib

LIB := $(BUILD_DIR)/libobmm_async.a
OBJECTS := $(BUILD_DIR)/obmm_async.o $(BUILD_DIR)/obmm_async_aarch64.o

.PHONY: all clean

all: $(LIB)

$(BUILD_DIR):
	mkdir -p $@

$(LIB): $(OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $^

$(BUILD_DIR)/obmm_async.o: obmm_async.c obmm_async.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

$(BUILD_DIR)/obmm_async_aarch64.o: obmm_async_aarch64.S | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(LIB) $(OBJECTS)
