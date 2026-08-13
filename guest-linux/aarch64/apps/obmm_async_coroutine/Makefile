CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -static
ROOT := ../..
KERNEL_UAPI := ../../../kernel_ub/include/uapi
ASYNC_LIB_DIR := $(ROOT)/libs/obmm_async
SCC_LIB_DIR := $(ROOT)/libs/obmm_scc
BUILD_DIR ?= $(ROOT)/out/obmm_async_coroutine
TARGET := $(BUILD_DIR)/obmm_async_coroutine
SOURCES := obmm_async_coroutine.c uffd_mode.c uffd_state.c
SOURCES += $(ROOT)/common/obmm_uffd.c
ASYNC_LIB := $(ROOT)/out/obmm_async/lib/libobmm_async.a
SCC_LIB := $(ROOT)/out/obmm_scc/lib/libobmm_scc.a

CPPFLAGS += -I$(ROOT)/common -I$(ASYNC_LIB_DIR) -I$(SCC_LIB_DIR)
CPPFLAGS += -idirafter $(KERNEL_UAPI)

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(ASYNC_LIB):
	$(MAKE) -C $(ASYNC_LIB_DIR) all

$(SCC_LIB):
	$(MAKE) -C $(SCC_LIB_DIR) all

$(TARGET): $(SOURCES) $(ASYNC_LIB) $(SCC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(ASYNC_LIB) $(SCC_LIB) \
		-pthread -o $@

clean:
	rm -f $(TARGET)
