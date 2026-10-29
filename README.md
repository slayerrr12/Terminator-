# Terminator

Terminator is a small Linux terminal window and shell written in C. It uses
Xlib to draw the window and supports multiple tabs. Each tab has its own shell
state and runs as a worker thread by default. You can also start each tab's
worker as a separate process.

## Build and run

Install the compiler, Make, and X11 development files on Debian or Ubuntu:

```sh
sudo apt install build-essential libx11-dev
```

From the project directory, build the executable and start it:

```sh
make
./terminator
```

`make` compiles and links the source files into `./terminator`. To use process
mode, pass the assignment's launch option:

```sh
./terminator useProcess=True
```

The Makefile also provides `make run` to build the program if needed and start
it. `make clean` removes the executable and generated build files.

## Using the window

- Press **Ctrl+T** or click **[+]** to open a tab.
- Click a tab number to switch tabs. A tab's worker continues while another tab
  is selected.
- Press **Ctrl+D** on an empty command line to close the current tab.
- Press **Ctrl+C** to interrupt the selected tab's foreground job and **Ctrl+Z**
  to stop it.
- Use **Up** and **Down** to move 10 lines at a time, **Page Up/Page Down** to
  move by a screen, or the mouse wheel to move 3 lines per notch. Each tab
  keeps up to 10,000 lines of output within a 1 MiB scrollback text budget.
  If one line exceeds that budget, its oldest characters are discarded.

## Implemented features

- X11 window with per-tab input, output, prompt, scrollback, and tab selection.
- Thread mode and process mode, with shared command history protected by a
  semaphore.
- Built-in commands: `cd`, `clear`, `exit`, `history`, `source`, `multiWatch`,
  `jobs`, `fg`, and `bg`.
- External commands launched with `fork()` and `execvp()`.
- Multiline input, single and double quotes, environment variables, `$?`,
  command substitution, and wildcard expansion.
- Pipelines of multiple commands and input/output redirection (`<`, `>`, `>>`).
- Background jobs with basic `jobs`, `fg`, `bg`, Ctrl+C, and Ctrl+Z support.
- `multiWatch` runs commands concurrently and collects their output without
  busy-waiting.

For the implementation of each assignment task, see
[docs/TASK_IMPLEMENTATION_README.md](docs/TASK_IMPLEMENTATION_README.md).
For a beginner walkthrough of the GUI, tabs, execution modes, built-ins, and
`execvp()`, see [docs/tasks-1-and-2-explained.md](docs/tasks-1-and-2-explained.md).
Architecture and design details are in [docs/architecture.md](docs/architecture.md)
and [docs/design.md](docs/design.md).
The complete requirements remain in [Terminator - Google Docs.pdf](<Terminator - Google Docs.pdf>);
the implementation guide describes current coverage and does not replace that
specification.

## Current limitations

Terminator implements a shell subset. It is not a full Bash implementation and
does not use a PTY, so full-screen programs and programs that depend on normal
terminal control sequences are not supported. The current history is shared
and kept in memory, with a limit of 1,024 commands; it is not saved between
runs.

Configurable hotkeys, command-history search, filename completion, and the
custom Bash-subset script interpreter are not implemented. `source` executes
complete input lines using Terminator's existing shell parser; it does not
provide the custom interpreter described in the larger assignment. See
[docs/roadmap.md](docs/roadmap.md) for task status and remaining work.

## Checks

The repository includes worker and X11 interaction checks. Run them with:

```sh
make test
```

The GUI interaction check uses Xvfb. Install it on Debian or Ubuntu if it is
not already available:

```sh
sudo apt install xvfb
```
