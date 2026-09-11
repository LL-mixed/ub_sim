CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
MANAGER_LIB := $(ROOT)/libs/gva_manager
MANAGER_UAPI := $(ROOT)/out/gva_manager_uapi/include
MANAGER_UAPI_SRC := $(ROOT)/../kernel_ub/include/uapi/ub

include ../../common/libobmm.mk

all: gva_manager

gva_manager: gva_manager.c $(MANAGER_LIB)/gva_manager.c $(MANAGER_LIB)/gva_manager.h $(MANAGER_UAPI_SRC)/obmm.h $(MANAGER_UAPI_SRC)/gsva.h | prepare-gva-uapi
	$(CC) $(CFLAGS) -I$(MANAGER_UAPI) -I$(ROOT)/common -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue $(OBMM_CFLAGS) $< $(MANAGER_LIB)/gva_manager.c $(OBMM_SRCS) -o $@

prepare-gva-uapi:
	mkdir -p $(MANAGER_UAPI)/ub
	cp $(MANAGER_UAPI_SRC)/obmm.h $(MANAGER_UAPI_SRC)/gsva.h $(MANAGER_UAPI)/ub/

clean:
	rm -f gva_manager

.PHONY: all clean prepare-gva-uapi
