# libobmm wiring shared by apps that include common/obmm_common.h.
# Include from an app Makefile as:  include ../../common/libobmm.mk
OBMM_MK_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
AARCH64_ROOT := $(OBMM_MK_DIR)/..
UB_SIM_ROOT := $(AARCH64_ROOT)/../..
OBMM_SUBMODULE := $(UB_SIM_ROOT)/vendor/obmm
KERNEL_UB_UAPI := $(UB_SIM_ROOT)/guest-linux/kernel_ub/include/uapi
KERNEL_UB_INC := $(UB_SIM_ROOT)/guest-linux/kernel_ub/include
# Raw kernel uapi headers need __EXPORTED_HEADERS__ (suppresses the user-space
# #warning) and the non-uapi include dir for linux/compiler_types.h.
OBMM_CFLAGS := -D__EXPORTED_HEADERS__ -I$(OBMM_SUBMODULE)/src/libobmm \
               -I$(KERNEL_UB_UAPI) -I$(KERNEL_UB_INC)
OBMM_SRCS := $(UB_SIM_ROOT)/vendor/obmm/src/libobmm/libobmm.c \
             $(OBMM_MK_DIR)/obmm_vendor_adaptor_sim.c
OBMM_LDLIBS := -pthread
