CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11 -static
AARCH64_ROOT := ../..
UB_SIM_ROOT := $(abspath $(AARCH64_ROOT)/../..)
LIB_DIR := $(AARCH64_ROOT)/libs/lingqu_shmem_pto
ASYNC_DIR := $(AARCH64_ROOT)/libs/obmm_async
TARGET ?= lingqu_shmem_pto_direct
MANAGED ?= 0
MEM_SERVICE_ROOT ?= $(abspath $(AARCH64_ROOT)/../../mem_service)

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

ifeq ($(MANAGED),1)
MEM_SERVICE_COMPONENT := $(MEM_SERVICE_ROOT)/components/mem_service
SOURCES += \
	lingqu_shmem_pto_managed.c \
	$(LIB_DIR)/lingqu_shmem_mem_service.c \
	$(LIB_DIR)/lingqu_shmem_mem_service_obmm.c \
	$(MEM_SERVICE_COMPONENT)/mem_service_client.c \
	$(MEM_SERVICE_COMPONENT)/mem_service_wire_client.c \
	$(MEM_SERVICE_COMPONENT)/mem_service_provider.c \
	$(MEM_SERVICE_COMPONENT)/mem_service_mapping_owner.c \
	$(MEM_SERVICE_COMPONENT)/providers/mem_service_provider_obmm.c
CPPFLAGS += -DLINGQU_SHMEM_MANAGED_MAPPING -I$(AARCH64_ROOT) -I$(MEM_SERVICE_ROOT)
# Drop the unselected legacy arena adapter and its model-runtime dependencies.
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections -pthread
endif

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBMM_CFLAGS) \
		$(SOURCES) $(OBMM_SRCS) $(LDFLAGS) $(OBMM_LDLIBS) -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
