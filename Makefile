# C compiler and options used to build the project.
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -pthread -Iinclude \
	-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700

# Libraries used by the GUI and thread support.
LIBRARIES = -lX11 -pthread

# All source files that make up the terminal application.
SOURCES = src/main.c src/gui.c src/tab.c src/parser.c \
	src/executor.c src/shell.c src/multiwatch.c src/history.c \
	src/scrollback.c

# Source files needed by the shell worker test.
WORKER_SOURCES = src/parser.c src/executor.c src/shell.c \
	src/multiwatch.c src/history.c

# Running "make" builds the application.
.PHONY: all run clean test
all: terminator

# Compile and link every source file into one executable.
terminator: $(SOURCES) include/terminator.h include/shell.h \
	include/scrollback.h
	$(CC) $(CFLAGS) $(SOURCES) -o terminator $(LIBRARIES)

# Build the application if needed, then start it.
run: terminator
	./terminator

# Remove the executable and generated test files.
clean:
	rm -rf build terminator

# Build and run the project's existing checks.
test: terminator
	mkdir -p build
	$(CC) $(CFLAGS) tests/worker_test.c $(WORKER_SOURCES) \
		-o build/worker_test -pthread
	timeout 20s ./build/worker_test
	$(CC) $(CFLAGS) tests/scrollback_test.c src/scrollback.c \
		-o build/scrollback_test
	./build/scrollback_test
	./tests/integration.sh
