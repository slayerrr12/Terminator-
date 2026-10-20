# Terminator

Terminator is a small Linux terminal and shell project written in C. It uses Xlib for its window and runs one independent shell worker per tab. Workers run as threads by default or as processes with `useProcess=True`.

## Project layout

```text
include/   Public application and shell types
src/       GUI, tab lifecycle, shell, parser, executor, and history modules
tests/     Worker regression tests and X11 interaction driver
docs/      Architecture, design notes, and future work
examples/  Sample commands
```

## Dependencies

On Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential libx11-dev xvfb
```

A running X server is required to use the application. `Xvfb` is used by the headless GUI checks.

## Build and run

```sh
make
./terminator
./terminator useProcess=True
```

The default mode starts one worker thread per tab. Process mode starts one worker process per tab. Press **Ctrl+T** or click **[+]** to create a tab, click a tab label to switch, and press **Ctrl+D** on an empty prompt to close the active tab.

## Current features

- Xlib window with per-tab input/output buffers, command prompt, scrolling, tab switching, and non-blocking event polling.
- External commands launched with `fork()` and `execvp()`.
- Built-ins: `cd`, `exit`, `history`, and `source` for sequential command files.
- Shared, synchronized in-memory command history; each tab keeps its own working directory and `$?` status.
- Pipelines of arbitrary length, input/output redirection (`<`, `>`, `>>`), and quote-aware filename globbing.
- Environment and exit-status expansion, nested `$()` and backtick command substitution, single and double quotes, multiline quotes, and trailing-backslash continuation.
- Ctrl+C signals the active tab's foreground process group. Closing a tab stops its worker and active foreground group.

## Tests

```sh
make test
```

Worker tests cover both execution modes, shell parsing and execution, history sharing, per-tab working directories, source files, redirection, pipelines, and Ctrl+C isolation. The Xvfb driver types commands through the GUI, creates and closes tabs, and verifies the resulting files in both modes.

## Current limits and later work

Terminator implements a shell subset rather than full Bash. It has no PTY or complete terminal emulation, so interactive full-screen programs and terminal control sequences are not supported. Compound command lists, assignments, complete job control, configurable hotkeys, persistent searchable history, filename completion, and the custom script interpreter remain future work. See [docs/roadmap.md](docs/roadmap.md).

## Design notes

- [Architecture](docs/architecture.md)
- [Design decisions](docs/design.md)
- [Roadmap](docs/roadmap.md)
- [Sample commands](examples/demo-commands.txt)
