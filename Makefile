# Compiler and options used to build the project.
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -pthread -Iinclude \
	-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700

# Libraries used by the GUI, X11 hotkey handling, and worker threads.
LIBRARIES = -lX11 -pthread
XVFB_RUN ?= $(shell command -v xvfb-run 2>/dev/null)
TIMEOUT ?= timeout
TEST_TIMEOUT ?= 180

# All source files that make up the terminal application.
SOURCES = src/main.c src/gui.c src/tab.c src/parser.c \
	src/executor.c src/shell.c src/multiwatch.c src/history.c \
	src/scrollback.c src/selection.c src/hotkeys.c src/completion.c

# The test binary links the shell and utility modules without starting X11.
TEST_SOURCES = tests/test_suite.c src/parser.c src/executor.c src/shell.c \
	src/multiwatch.c src/history.c src/scrollback.c src/hotkeys.c \
	src/completion.c
TEST_BINARY = build/tests/test_suite

# Running "make" builds the application.
.PHONY: all run clean test test-unit test-integration test-regression \
	test-gui test-clean test-verbose
all: terminator

# Compile and link every source file into one executable.
terminator: $(SOURCES) include/terminator.h include/shell.h \
	include/scrollback.h include/selection.h include/hotkeys.h \
	include/completion.h
	$(CC) $(CFLAGS) $(SOURCES) -o terminator $(LIBRARIES)

# Build the application if needed, then start it.
run: terminator
	./terminator

# Compile the automated suite and its production modules.
$(TEST_BINARY): $(TEST_SOURCES) include/terminator.h include/shell.h \
	include/scrollback.h include/hotkeys.h include/completion.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TEST_SOURCES) -o $@ $(LIBRARIES)

# Run each suite category independently.
test-unit: $(TEST_BINARY)
	$(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) unit

test-integration: $(TEST_BINARY)
	$(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) integration

test-regression: $(TEST_BINARY)
	$(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) regression

# Attempt the app build and report GUI checks even when compilation fails.
test-gui: $(TEST_BINARY)
	@set +e; \
	$(MAKE) --no-print-directory terminator; \
	build_status=$$?; \
	if [ -n "$(XVFB_RUN)" ]; then \
		if [ $$build_status -eq 0 ]; then \
			$(XVFB_RUN) -a env TERMINATOR_TEST_BINARY="$(CURDIR)/terminator" $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) gui; \
		else \
			$(XVFB_RUN) -a env TERMINATOR_TEST_BUILD_FAILED=1 $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) gui; \
		fi; \
	else \
		if [ $$build_status -eq 0 ]; then \
			TERMINATOR_TEST_BINARY="$(CURDIR)/terminator" $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) gui; \
		else \
			TERMINATOR_TEST_BUILD_FAILED=1 $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) gui; \
		fi; \
	fi; \
	gui_status=$$?; \
	if [ $$build_status -ne 0 ] || [ $$gui_status -ne 0 ]; then exit 1; fi

# Run every category and attempt the application build even if it fails.
# This lets the shell tests finish and records GUI build failures in the report.
test: $(TEST_BINARY)
	@set +e; \
	$(MAKE) --no-print-directory terminator; \
	build_status=$$?; \
	if [ -n "$(XVFB_RUN)" ]; then \
		if [ $$build_status -eq 0 ]; then \
			$(XVFB_RUN) -a env TERMINATOR_TEST_BINARY="$(CURDIR)/terminator" $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) all; \
		else \
			$(XVFB_RUN) -a env TERMINATOR_TEST_BUILD_FAILED=1 $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) all; \
		fi; \
	else \
		if [ $$build_status -eq 0 ]; then \
			TERMINATOR_TEST_BINARY="$(CURDIR)/terminator" $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) all; \
		else \
			TERMINATOR_TEST_BUILD_FAILED=1 $(TIMEOUT) --kill-after=5s $(TEST_TIMEOUT)s ./$(TEST_BINARY) all; \
		fi; \
	fi; \
	test_status=$$?; \
	if [ $$build_status -ne 0 ] || [ $$test_status -ne 0 ]; then exit 1; fi

# Detailed output is the default, so this target is an explicit alias.
test-verbose: test

# Remove generated test executable and artifacts.
test-clean:
	rm -rf build/tests

# Remove the application executable and generated build files.
clean:
	rm -rf build terminator
