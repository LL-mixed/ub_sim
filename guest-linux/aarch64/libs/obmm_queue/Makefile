HOST_CC   ?= cc
CFLAGS    := -O2 -Wall -Wextra -I.

TEST_SRC := obmm_queue_test.c
TEST_BIN := obmm_queue_test
HEADERS  := obmm_queue_types.h obmm_spsc_queue.h \
            obmm_spmc_queue.h obmm_mpsc_queue.h obmm_mpmc_queue.h

.PHONY: test clean

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	$(HOST_CC) $(CFLAGS) -o $@ $(TEST_SRC) -lpthread

clean:
	rm -f $(TEST_BIN)
