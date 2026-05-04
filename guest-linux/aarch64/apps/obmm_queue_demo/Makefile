TARGET_CC ?= $(CC)
HOST_CC   ?= cc
CFLAGS    := -static -O2 -Wall -Wextra -I.

DEMO_SRC := obmm_queue_demo.c
DEMO_BIN := obmm_queue_demo

TEST_SRC := obmm_queue_test.c
TEST_BIN := obmm_queue_test

HEADERS := obmm_pool_types.h obmm_queue.h obmm_pool_helpers.h

.PHONY: all demo test clean

all: demo

demo: $(DEMO_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(HEADERS)
	$(TARGET_CC) $(CFLAGS) -o $@ $(DEMO_SRC)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) obmm_pool_types.h obmm_queue.h
	$(HOST_CC) -O2 -Wall -Wextra -I. -o $@ $(TEST_SRC) -lpthread

clean:
	rm -f $(DEMO_BIN) $(TEST_BIN)
