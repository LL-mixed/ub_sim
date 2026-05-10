TARGET_CC ?= $(CC)
LIB_DIR   := ../libs/obmm_queue

CFLAGS    := -static -O2 -Wall -Wextra -I$(LIB_DIR) -I.

DEMO_SRC := obmm_queue_demo.c
DEMO_BIN := obmm_queue_demo

HEADERS := obmm_pool_helpers.h

.PHONY: all demo clean

all: demo

demo: $(DEMO_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(HEADERS)
	$(TARGET_CC) $(CFLAGS) -o $@ $(DEMO_SRC)

clean:
	rm -f $(DEMO_BIN)
