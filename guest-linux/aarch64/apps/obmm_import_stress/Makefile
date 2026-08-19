CC = aarch64-linux-gnu-gcc
CFLAGS = -static -O2 -Wall -Wextra \
         -I../../kernel_ub/include/uapi \
         -I../../common \
         -I../obmm_queue

include ../../common/libobmm.mk

TARGET = obmm_import_stress

all: $(TARGET)

$(TARGET): obmm_import_stress.c
	$(CC) $(CFLAGS) $(OBMM_CFLAGS) $< $(OBMM_SRCS) -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
