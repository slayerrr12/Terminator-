# Terminator: task-by-task implementation guide

This guide follows the project assignment and points to the code that
implements each area. It is written for someone learning C and POSIX process
programming. The assignment PDF is the authoritative requirements document;
this guide records implementation details and gaps without changing those
requirements.

## Project structure

```text
include/
  terminator.h       App, Tab, worker, and shared-history data structures
  shell.h            Shell state, parsed commands, and job structures
  scrollback.h       Per-tab scrollback and viewport interface
src/
  main.c             Startup, execution-mode selection, and shutdown
  gui.c              X11 window, keyboard input, drawing, and event loop
  tab.c              Per-tab pipes and worker thread/process lifecycle
  shell.c            Shell worker and built-in commands
  parser.c           Input completeness, tokenizing, parsing, and expansion
  executor.c         Redirection, pipelines, processes, and job control
  multiwatch.c       Concurrent command supervisor for multiWatch
  history.c          Shared in-memory command history
  scrollback.c       Bounded per-tab output history and scrolling behavior
docs/                This guide, design notes, architecture, and roadmap
Makefile             Build, run, and clean commands
README.md            Dependencies, build instructions, features, and limits
```

The project keeps a small number of modules with related work together. The
GUI handles the window and keyboard. The shell handles the state of one tab.
The parser describes a command. The executor runs it. This separation lets a
reader follow one command through the program without navigating many tiny
files.

## Build and run

The Makefile compiles and links all source files into `./terminator`:

```sh
make
./terminator
```

Thread mode is the default. To start in process mode, run:

```sh
./terminator useProcess=True
```

The Makefile links X11 and POSIX thread support. For Debian or Ubuntu, install
the compiler, Make, and X11 development headers with:

```sh
sudo apt install build-essential libx11-dev
```

## Execution modes and data flow

Every tab has a shell worker. The worker mode changes how the worker is
started, but both modes use the same shell code.

- In **thread mode**, the GUI and each tab worker are threads in one process.
  The threads share the in-memory history ring.
- In **process mode**, the GUI forks a worker process for each tab. History is
  placed in shared memory so the separate processes can use the same ring.
- Each tab has a command pipe from the GUI to its worker, an output pipe from
  the worker to the GUI, and a status pipe from the worker to the GUI.
- The GUI polls its X11 connection and all worker output/status pipes. It sleeps
  while they are idle, so command output does not prevent keyboard input.

```mermaid
flowchart LR
    A[KeyPress] --> B[GUI input buffer]
    B -->|command pipe| C[Tab worker]
    C --> D[Parser]
    D --> E[Executor]
    E -->|output pipe| F[GUI scrollback]
    C -->|status pipe| B
```

### Where is the code for this?

- `src/main.c`: selects the mode and creates shared history.
- `src/tab.c`: `app_add_tab` creates the pipes and starts a thread or process.
- `src/shell.c`: `worker_run` reads commands for one tab.
- `src/history.c`: initializes and protects the shared command history.
- `include/terminator.h`: defines `App`, `Tab`, `WorkerStatus`, and
  `SharedHistory`.

### Explanation of the pipe setup

For a pipe, element `0` is the read end and element `1` is the write end. The
GUI writes commands to `gui_to_worker_fd`. The worker writes output to its
output pipe, which the GUI reads through `worker_to_gui_fd`. A separate status
pipe carries the exit status and updated working directory.

The two-way flow uses separate one-way pipes because that keeps each direction
easy to follow. A tab's pipes also keep its command output separate from the
other tabs' output.

## Task 1: X11 GUI and tabs

### Implementation details

The GUI creates one X11 window and draws a tab bar, text output, the prompt,
the editable input, and the input cursor. Each `Tab` stores its own input
buffer and up to 10,000 lines of output within a 1 MiB allocated-text budget.
If one output line exceeds that budget, the buffer keeps its tail. Clicking a
tab changes which tab is drawn and receives keyboard input. The other tabs'
workers continue running.
The mouse wheel and Page Up/Page Down scroll the active tab. If output arrives
while the user is looking at earlier lines, the viewport stays on the same
content. Returning to the bottom resumes following output.

`XLookupString()` translates a key event into text and a `KeySym`. The GUI
handles the editing keys and sends the full command to the selected worker
after Enter. Worker output returns through the output pipe and is added to that
tab's scrollback.

The event loop uses `poll()` on the X11 connection and worker pipes. A timeout
of `-1` means it waits until something is ready rather than repeatedly checking
the same descriptors.

### Where is the code for this?

- `src/gui.c`: `gui_run`, `handle_key_press`, `draw_terminal_window`,
  `submit_input`, and `append_output_text`.
- `src/scrollback.c`: line storage, the 10,000-line and 1 MiB per-tab limits,
  viewport position, and follow-output behavior.
- `src/tab.c`: `app_add_tab` and `app_close_tab`.
- `include/terminator.h`: the `App` and `Tab` data structures.

### Explanation of the event loop

The loop builds a list of descriptors to watch:

```c
int pr = poll(pf, nf, -1);
if (pr < 0) {
  if (errno == EINTR && terminator_stopping)
    break;
  if (errno == EINTR)
    continue;
  break;
}
```

The list contains the X11 connection plus each tab's output and status pipes.
When `poll()` returns, the GUI handles ready X11 events, reads worker output,
or reads a worker's status message. It then redraws the window from the saved
tab buffers. Storing output in the tab means the UI can redraw without asking
the worker to repeat it.

The current interface uses Xlib to draw text; it is not a full terminal
emulator. It does not allocate a PTY or interpret all terminal control
sequences.

## Task 2: built-in and external commands

### Implementation details

Commands that need to change a tab's shell state are built-ins. The built-in
table in `shell.c` maps a command name to its function. `cd` changes the
working directory in that tab's `ShellContext`, so it does not change the
directory of another tab or of the GUI process.

External commands run in child processes. The child changes to the tab's
working directory and calls `execvp()`. If the command cannot be found, the
child prints an error to the tab output. The worker remains available for the
next command.

The current built-ins are `cd`, `clear`, `exit`, `history`, `source`,
`multiWatch`, `jobs`, `fg`, and `bg`. The shell saves the last command's exit
status in `ShellContext.last_status`; `$?` expands to that value.

### Where is the code for this?

- `src/shell.c`: built-in functions, the built-in lookup table,
  `shell_builtin`, and `worker_run`.
- `src/executor.c`: `run_command_in_child`, `shell_execute`, and
  `shell_run_line`.
- `include/shell.h`: `ShellContext` and `ShellJob`.

### Explanation of built-in versus external commands

`cd` must run in the tab's worker because changing directory in a child would
not affect the shell that reads the next command. External programs do not
need to change the shell's state, so the executor starts them in child
processes. The child calls `execvp()` to replace itself with the requested
program. The worker waits for foreground commands and keeps the prompt alive.

The executor checks for a built-in before trying an external program:

```c
int builtin = shell_builtin(shell, command->arguments, STDOUT_FILENO);
if (builtin >= 0)
  return builtin;

execvp(command->arguments[0], command->arguments);
```

`execvp()` replaces the child with the requested program. If it returns, the
program could not be started, so the executor prints an error and returns a
failure status.

`source` executes complete lines from a file through `shell_run_line()`. It is
a simple command-file feature; it is not the custom script language required
by the later script-interpreter task.

## Task 3: multiline input and quoting

### Implementation details

The GUI collects more input when the command is incomplete. The parser checks
whether quotes, escapes, backticks, or a `$()` command substitution are still
open. If so, Enter adds a continuation line instead of sending the command to
the worker.

The lexer keeps quoted text together while separating shell operators. The
word expander removes quote characters and handles the implemented variables,
`$?`, command substitutions, and eligible wildcard patterns. Single-quoted
text stays literal. Double-quoted text permits variable and command
substitution. This is a useful shell subset, not every Bash quoting rule.

### Where is the code for this?

- `src/gui.c`: `submit_input` accumulates the lines.
- `src/parser.c`: `shell_is_complete`, `shell_lex`, `shell_parse`, and
  `shell_expand_word`.
- `include/shell.h`: `Tokens`, `Pipeline`, and `SimpleCommand`.

### Explanation of incomplete input

`submit_input()` asks `shell_is_complete()` before sending anything. For
example, an open double quote means the user is still entering one command:

```sh
echo "first line
second line"
```

The first Enter does not run `echo`. The GUI displays the continuation prompt
and keeps collecting text. Once the closing quote arrives, the complete
command is sent through the tab's input pipe. A trailing backslash joins the
next line according to the implementation's continuation handling.

## Task 4: I/O redirection

### Implementation details

The parser recognizes `<`, `>`, and `>>` outside quotes. It stores the input
and output file names on the relevant `SimpleCommand`. The executor opens the
files and connects them to standard input or standard output with `dup2()`.
The same setup is used for individual commands in a pipeline.

If an input file cannot be opened, the executor reports the error and does not
run that command. `>` creates or truncates a file; `>>` appends to it.

### Where is the code for this?

- `src/parser.c`: `shell_parse` stores redirection paths.
- `src/executor.c`: `redirection_open_input`, `redirection_open_output`,
  `redirection_apply`, and the built-in redirection path in `shell_execute`.

### Explanation of `dup2()`

`open()` returns a file descriptor for the requested file. `dup2(file_fd,
STDOUT_FILENO)` makes standard output refer to that file. The command can then
write normally to standard output without knowing that it was redirected.
Input redirection uses the same idea with `STDIN_FILENO`.

For example:

```sh
sort < names.txt > sorted.txt
```

The executor opens `names.txt` for reading, opens or creates `sorted.txt` for
writing, and connects those descriptors before starting `sort`.

## Task 5: pipes

### Implementation details

The parser stores each pipeline stage as a separate `SimpleCommand`. For N
commands, `executor.c` creates N-1 pipes and starts one child for each stage.
Each child connects its standard input and output to the neighboring pipe with
`dup2()`. The parent closes its copies of the pipe ends and waits for the
foreground process group.

Redirection can be used on pipeline commands. For example,
`cat names.txt | sort > sorted.txt` sends the contents of `names.txt` through
`sort` and writes the sorted output to `sorted.txt`.

### Where is the code for this?

- `src/parser.c`: `shell_parse` builds the list of pipeline stages.
- `src/executor.c`: `execute_pipeline` creates pipes, forks children, and
  connects their standard input and output.
- `src/executor.c`: `redirection_apply` applies file redirection for a stage.

### Explanation of the pipe connections

The first command reads from the tab's default input and writes to pipe 1. A
middle command reads from the previous pipe and writes to the next one. The
last command reads from the previous pipe and writes to the tab's output. The
same rule works for two commands or a longer pipeline.

```text
command 1 --pipe--> command 2 --pipe--> command 3
```

Every process closes pipe ends it does not use. Closing those unused ends is
important: it lets a reader see end-of-file when the writer has finished.

## Task 6: `multiWatch`

### Implementation details

`multiWatch` starts one process for each command string. Each process writes to
its own pipe. A supervisor waits for output with `poll()`, forwards the output
as it arrives, and restarts commands after they finish. The default interval
is about two seconds. Ctrl+C targets the supervisor's process group, leaving
the tab's worker available for the next command.

Each run is preceded by the command and its Unix timestamp, and its output is
followed by a separator line, as required by the assignment:

```text
"date +%T", 1790860020:
----------------------------------------------------
18:37:00
----------------------------------------------------
```

### Where is the code for this?

- `src/shell.c`: registers `multiWatch` as a built-in.
- `src/multiwatch.c`: `shell_builtin_multiwatch`,
  `start_watched_command`, and `run_watch_supervisor`.

## Task 7: configurable hotkeys

`src/gui.c` handles the assigned editing keys directly in
`handle_key_press()`: Left/Right move the cursor, Ctrl+A/E move to the start or
end, Ctrl+U/K delete to either end, and Ctrl+W deletes the previous word.
Ctrl+L clears the active tab's output, while Ctrl+D closes an empty tab.
Up/Down browse the shared command history; Page Up/Page Down and the mouse
wheel scroll output.

Dragging over output selects text. Ctrl+Shift+C places the selection on the
X11 clipboard, and Ctrl+Shift+V inserts clipboard text into the input line.
`reply_to_selection_request()` and `receive_paste()` handle the X11 clipboard
exchange. The bindings are fixed in the code; user-configurable saved bindings
are not implemented.

## Task 8: signals and job control

`src/executor.c` creates a process group for each pipeline and records
background jobs in the tab's `ShellContext`. `jobs`, `fg`, and `bg` inspect,
resume, or wait for those jobs. `src/gui.c` sends Ctrl+C and Ctrl+Z to the
active tab's foreground process group.

This is basic process-group job control. Without a PTY, Terminator cannot
provide the full foreground-terminal behavior of a normal Linux terminal.

## Task 9: searchable persistent history

`src/history.c` stores up to 1,024 commands in a shared in-memory ring. A
semaphore protects reads and writes in both execution modes. The `history`
built-in prints the saved entries, and Up/Down retrieve recent entries for the
active input line. Page Up/Page Down and the mouse wheel control output
scrollback separately.

The assignment's persistent 10,000-command file and Ctrl+R search are not
implemented.

## Task 10: file-name auto-completion

File-name completion is not implemented. Pressing Tab does not scan the
working directory or change the input line. The relevant UI state is the
active tab's `input_buffer`, `input_cursor`, and `working_directory` in
`include/terminator.h`.

## Task 11: custom Bash-subset script interpreter

`source` reads complete lines and sends them through the existing parser and
executor. It supports the ordinary commands and syntax that those modules
already understand. It does not implement a separate language for variable
assignments, positional parameters, `if`/`elif`/`else`, or `exit [n]` in a
script. The custom interpreter task remains future work.

## Current limits at a glance

- The command language is a subset of Bash.
- The window is not backed by a PTY and does not support full-screen
  interactive applications.
- History is shared between tabs but exists only in memory and holds at most
  1,024 entries.
- Configurable hotkeys, history search, file-name completion, and the custom
  script interpreter remain unfinished.

See [architecture.md](architecture.md) for module responsibilities,
[design.md](design.md) for implementation choices, and
[roadmap.md](roadmap.md) for the assignment status table.
