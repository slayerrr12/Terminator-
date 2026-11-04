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

The Makefile uses the project's C/POSIX compiler flags and links Xlib with
`-lX11`; no additional third-party library is required to build the app. To run
GUI tests without an active X display, install the optional X virtual display:

```sh
sudo apt install xvfb
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

## Automated tests

Run all automated checks with one command:

```sh
make test
```

The suite also has focused targets: `make test-unit`, `make test-integration`,
`make test-regression`, and `make test-gui`. Each case reports its expected and
observed result, and any failure makes the target return nonzero. Use
`make test-clean` to remove the generated test runner. See
[tests/README.md](tests/README.md) for the coverage map and known test limits.

## Using the window

- Press **Ctrl+T** or click **[+]** to open a tab.
- Each tab shows its `user@host:~/path` location. Click a tab to switch to it;
  hover over it to reveal the small **×** close button. A tab's worker continues
  while another tab is selected.
- Press **Ctrl+D** on an empty command line to close the current tab.
- Press **Ctrl+C** to interrupt the selected tab's foreground job and **Ctrl+Z**
  to stop it.
- Use **Up** and **Down** to browse the shared command history. Press **Ctrl+R**
  to search it. Type a term and press Enter: all commands containing that
  case-sensitive text are shown first. If none contain it, commands tied for
  the longest shared substring are shown when that match is longer than two
  characters.
- Press **Tab** to complete the filename before the cursor using the active
  tab's working directory. One match completes directly. Multiple matches
  complete the shared prefix, list the choices, and wait for a number and Enter.
- Use **Page Up/Page Down** or the mouse wheel to scroll output vertically. Use
  **Shift + mouse wheel** to scroll horizontally across long lines. Each tab
  keeps up to 10,000 output lines within a 1 MiB scrollback budget.
- Use **Ctrl+Shift+C/V** to copy selected visible text and paste into the input.
  Selection includes displayed output, the prompt, and editable command text.
- Use **Ctrl+A/E**, **Ctrl+U/K/W**, **Left/Right**, and **Backspace** to edit
  the current command line. **Ctrl+L** clears the active tab's output.

## Implemented features

- X11 window with per-tab input, output, prompt, scrollback, and tab selection.
- Thread mode and process mode, with shared command history protected by a
  semaphore.
- Built-in commands: `cd`, `clear`, `exit`, `history`, `source`, `multiWatch`,
  `jobs`, `fg`, `bg`, and `less`.
- External commands launched with `fork()` and `execvp()`.
- Multiline input, single and double quotes, environment variables, `$?`,
  nested and multiline `$()` and backtick substitutions, and wildcard expansion.
- Pipelines of multiple commands and input/output redirection (`<`, `>`, `>>`).
- Background jobs with basic `jobs`, `fg`, `bg`, Ctrl+C, and Ctrl+Z support.
- `multiWatch` runs commands concurrently and collects their output without
  busy-waiting.
- Persistent shared command history keeps the latest 10,000 commands on disk;
  `history` prints at most the latest 1,000. The default file is
  `~/.terminator_history`; set `TERMINATOR_HISTORY_FILE` to choose another path.
- `less FILE` copies file contents into Terminator's scrollback. It can also
  read standard input through a pipe or redirection. Because the app has no
  PTY, this is a simple scrollback viewer rather than a full-screen pager.
- Tab filename completion searches the current tab's directory, respects
  quotes and escaped characters, preserves input after the cursor, and offers
  numbered choices when multiple names remain ambiguous.
- Persistent configurable editing hotkeys read from `~/.terminatorrc`.

## Change hotkeys

Copy [docs/terminatorrc.example](docs/terminatorrc.example) to
`~/.terminatorrc`, edit the key names, and restart Terminator. For example,
`move_left = Alt+Left` changes the cursor-left shortcut. Supported modifiers
are `Ctrl`, `Shift`, and `Alt`; key names use X11 names such as `Left`, `F12`,
or a letter. If a binding is invalid or conflicts with another binding,
Terminator reports it in the first tab's scrollback and uses the default for
that action.

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
terminal control sequences are not supported. History is stored in a
Terminator-specific file so the application does not overwrite Bash's history.

The custom Bash-subset script interpreter remains unimplemented. `source`
executes complete input lines using Terminator's existing shell parser; it does
not provide that custom interpreter. See [docs/roadmap.md](docs/roadmap.md) for
task status and remaining work.
