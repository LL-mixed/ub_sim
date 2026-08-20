CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -static
ROOT := ../..
KERNEL_UAPI := ../../../kernel_ub/include/uapi
OBMM_SUBMODULE_DIR := ../../../../vendor/obmm
ASYNC_LIB_DIR := $(ROOT)/libs/obmm_async
COROUTINE_SCHEDULER_LIB_DIR := $(ROOT)/libs/obmm_coroutine_scheduler
BUILD_DIR ?= $(ROOT)/out/obmm_async_coroutine
TARGET := $(BUILD_DIR)/obmm_async_coroutine
SOURCES := obmm_async_coroutine.c uffd_mode.c uffd_state.c
SOURCES += $(ROOT)/common/obmm_uffd.c
SOURCES += $(OBMM_SUBMODULE_DIR)/src/libobmm/libobmm.c
SOURCES += $(ROOT)/common/obmm_vendor_adaptor_sim.c
ASYNC_LIB := $(ROOT)/out/obmm_async/lib/libobmm_async.a
COROUTINE_SCHEDULER_LIB := $(ROOT)/out/obmm_coroutine_scheduler/lib/libobmm_coroutine_scheduler.a

CPPFLAGS += -I$(ROOT)/common -I$(ASYNC_LIB_DIR) -I$(COROUTINE_SCHEDULER_LIB_DIR)
CPPFLAGS += -D__EXPORTED_HEADERS__ -I$(OBMM_SUBMODULE_DIR)/src/libobmm
CPPFLAGS += -idirafter $(KERNEL_UAPI)

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(ASYNC_LIB):
	$(MAKE) -C $(ASYNC_LIB_DIR) all

$(COROUTINE_SCHEDULER_LIB):
	$(MAKE) -C $(COROUTINE_SCHEDULER_LIB_DIR) all

$(TARGET): $(SOURCES) $(ASYNC_LIB) $(COROUTINE_SCHEDULER_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(ASYNC_LIB) $(COROUTINE_SCHEDULER_LIB) \
		-pthread -o $@

clean:
	rm -f $(TARGET)
