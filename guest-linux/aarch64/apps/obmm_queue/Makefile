TARGET_CC ?= $(CC)
LIB_DIR   := ../../libs/obmm_queue

CFLAGS    := -static -O2 -Wall -Wextra -I$(LIB_DIR) -I.

APP_SRC := obmm_queue.c
APP_BIN := obmm_queue

HEADERS := obmm_pool_helpers.h

.PHONY: all clean

all: $(APP_BIN)

$(APP_BIN): $(APP_SRC) $(HEADERS)
	$(TARGET_CC) $(CFLAGS) -o $@ $(APP_SRC)

clean:
	rm -f $(APP_BIN)
