CC ?= cc
CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -pthread -MMD -MP
LDLIBS += -lX11 -pthread

SOURCES := src/main.c src/gui.c src/tab.c src/parser.c src/executor.c src/shell.c src/history.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

.PHONY: all clean test
all: terminator
terminator: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf build terminator

test: terminator
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/worker_test.c src/parser.c src/executor.c src/shell.c src/history.c -o build/worker_test
	timeout 20s ./build/worker_test
	./tests/integration.sh
