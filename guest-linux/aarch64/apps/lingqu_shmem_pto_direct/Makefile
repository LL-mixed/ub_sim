CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11 -static
AARCH64_ROOT := ../..
UB_SIM_ROOT := $(abspath $(AARCH64_ROOT)/../..)
LIB_DIR := $(AARCH64_ROOT)/libs/lingqu_shmem_pto
ASYNC_DIR := $(AARCH64_ROOT)/libs/obmm_async
TARGET ?= lingqu_shmem_pto_direct

include ../../common/libobmm.mk

SOURCES := \
	lingqu_shmem_pto_direct.c \
	$(LIB_DIR)/lingqu_shmem.c \
	$(LIB_DIR)/lingqu_shmem_pto_guest.c \
	$(LIB_DIR)/lingqu_shmem_pto_endpoint.c \
	$(ASYNC_DIR)/obmm_async.c \
	$(ASYNC_DIR)/obmm_async_aarch64.S

CPPFLAGS += \
	-I$(AARCH64_ROOT)/common \
	-I$(LIB_DIR) \
	-I$(ASYNC_DIR) \
	-I$(UB_SIM_ROOT)/crates/sim-qemu/include

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBMM_CFLAGS) \
		$(SOURCES) $(OBMM_SRCS) $(OBMM_LDLIBS) -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
