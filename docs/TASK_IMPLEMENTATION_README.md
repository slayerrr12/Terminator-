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
  hotkeys.h          Configurable key-binding interface
  completion.h       Filename completion interface
src/
  main.c             Startup, execution-mode selection, and shutdown
  gui.c              X11 window, keyboard input, drawing, and event loop
  tab.c              Per-tab pipes and worker thread/process lifecycle
  shell.c            Shell worker and built-in commands
  parser.c           Input completeness, tokenizing, parsing, and expansion
  executor.c         Redirection, pipelines, processes, and job control
  multiwatch.c       Concurrent command supervisor for multiWatch
  history.c          Persistent shared history and search
  scrollback.c       Bounded per-tab output history and scrolling behavior
  selection.c        Text selection and X11 clipboard handling
  hotkeys.c          Default keys and ~/.terminatorrc configuration
  completion.c       Filename matching and input-line insertion
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
  The threads share a small history coordination structure and one history
  file.
- In **process mode**, the GUI forks a worker process for each tab. Workers
  share the coordination structure in shared memory and use a lock file to
  coordinate updates to the same persistent history file.
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
The mouse wheel and Page Up/Page Down scroll the active tab vertically. Shift
+ mouse wheel scrolls horizontally across long lines. If output arrives while
the user is looking at earlier lines, the viewport stays on the same content.
Returning to the bottom resumes following output.

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
- `src/selection.c`: mouse selection, selection rendering, and X11 clipboard
  requests and replies.
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
`multiWatch`, `jobs`, `fg`, `bg`, and `less`. The shell saves the last
command's exit status in `ShellContext.last_status`; `$?` expands to that
value. `less` copies files or piped input into scrollback; it does not open a
full-screen pager because the application has no PTY.

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

The GUI collects more input when the command is incomplete. `shell_is_complete`
and `shell_lex` use the same scanner so quotes, escapes, backticks, and nested
`$()` substitutions are treated consistently. If a quote or substitution is
still open, Enter adds a continuation line instead of sending the command to
the worker. Both substitution forms can contain multiple lines, and nested
substitutions run from the inside out.

The lexer keeps quoted text together while separating shell operators. The
word expander removes quote characters and handles environment variables,
`$?`, nested command substitutions, and eligible wildcard patterns.
Single-quoted text stays literal. Double-quoted text still expands variables
and substitutions. Captured output is stored in a temporary file, then trailing
newlines are removed before it is added to the surrounding word. This covers
the quoting and substitution rules requested by this task; it is not every Bash
expansion.

### Where is the code for this?

- `src/gui.c`: `submit_input` accumulates the lines.
- `src/parser.c`: `shell_is_complete`, `shell_lex`, `shell_parse`, and
  `shell_expand_word`.
- `include/shell.h`: `Tokens`, `Pipeline`, and `SimpleCommand`.

### Explanation of incomplete input

`submit_input()` asks `shell_is_complete()` before sending anything. For
example, an open double quote or command substitution means the user is still
entering one command:

```sh
echo "first line
second line"
```

The first Enter does not run `echo`. The GUI displays the continuation prompt
and keeps collecting text. Once the closing quote or substitution arrives,
the complete command is sent through the tab's input pipe. A trailing
backslash joins the next line according to the implementation's continuation
handling.

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

`src/hotkeys.c` defines the assignment defaults and loads optional overrides
from `~/.terminatorrc`. The sample file is
`docs/terminatorrc.example`. For example, `move_left = Alt+Left` changes the
cursor-left key without recompiling. The config loads at startup, so custom
bindings remain active after restart.

`src/gui.c` calls `hotkeys_find_action()` from `handle_key_press()` and applies
the action to the selected tab. Left/Right move the cursor, Ctrl+A/E move to
the start or end, Ctrl+U/K delete to either end, and Ctrl+W deletes the previous
word. Ctrl+L clears the active tab's output, while Ctrl+D closes an empty tab.
Up/Down browse shared history. Ctrl+Shift+C/V use the X11 selection code to
copy and paste. Ctrl+C and Ctrl+Z signal the active job; Ctrl+T opens a tab.

If a configured key is unknown or conflicts with another binding, the loader
reports the problem in the first tab's scrollback and keeps the default for
that action. Dragging over visible text selects it, including output, the live
prompt, and editable input. `src/selection.c` owns selection drawing, mouse
tracking, and X11 clipboard exchange.

## Task 8: signals and job control

`src/executor.c` creates a process group for each pipeline and records
background jobs in the tab's `ShellContext`. `jobs`, `fg`, and `bg` inspect,
resume, or wait for those jobs. `src/gui.c` sends Ctrl+C and Ctrl+Z to the
active tab's foreground process group.

This implements the signal and job actions listed in Task 8. Without a PTY,
Terminator cannot provide the full foreground-terminal behavior of a normal
Linux terminal.

## Task 9: searchable persistent history

### Implementation details

Commands are saved to `~/.terminator_history` by default. Set
`TERMINATOR_HISTORY_FILE` to use another path. This keeps the application from
overwriting the user's Bash history and gives tests a private history file.
The file keeps the latest 10,000 commands; the `history` built-in prints the
latest 1,000. Newline, carriage return, and backslash bytes are escaped so each
command occupies one physical line. A semaphore coordinates tabs in one
application, and a sidecar lock file coordinates separate Terminator
processes.

Press **Ctrl+R**, type a term, and press Enter. The GUI first displays every
command containing that case-sensitive text. If no command contains the term,
it finds the longest common substring between the term and each command. If the
best substring is longer than two characters, all commands tied for that
longest match are displayed. Otherwise it prints `No match for search term in
history`. The exact pass displays matches in history-file order. The
assignment's wording says “the most recent command,” while its example shows
multiple matches; the current code follows the example. The scrollback
transcript shows the `USER@terminator> (Ctrl+R)`
line, then `Enter search term: <term>`, then the matches or no-match message.
This preserves the order shown in the assignment example. Search terms are
displayed for context but are not saved as commands.

### Where is the code for this?

- `src/history.c`: file creation, escaping, 10,000-row retention, locking,
  snapshots, and search.
- `src/shell.c`: `builtin_history()` displays at most 1,000 newest rows.
- `src/gui.c`: Ctrl+R prompt mode, result display, and the no-match message.
- `src/hotkeys.c`, `include/hotkeys.h`, and `docs/terminatorrc.example`:
  configurable `search_history` action, with Ctrl+R as the default.
- `include/terminator.h`: `SharedHistory` stores the semaphore, file path,
  and cached metadata; command records live on disk.

### Explanation of the important code

`history_add()` encodes special line bytes, then takes the semaphore and the
lock file before appending. Once more than 10,000 records are present, it
copies the newest 10,000 rows to a temporary file and replaces the old file.
`history_search()` checks whether the term occurs inside each command first.
Only when there are no containing commands does it build a suffix automaton
for the term and use it to find the longest shared substring efficiently.

The GUI uses a separate search buffer, so Enter displays matches instead of
running the search term as a shell command.

## Task 10: file-name auto-completion

### Implementation details

Press **Tab** to complete the word before the input cursor. The search uses the
active tab's `working_directory`. One match inserts the rest of the name.
Several matches insert only the longest common prefix, print a sorted numbered
list, and wait for a number followed by Enter. No match leaves the command line
unchanged. Text after the cursor stays in place, quotes and escaped spaces are
preserved, and directories receive a trailing slash.

### Where is the code for this?

- `src/completion.c`: scans filenames, decodes the current token, finds a
  common prefix, escapes the missing suffix, inserts text, and applies a
  numbered choice.
- `include/completion.h`: interface between the completion helpers and GUI.
- `src/gui.c`: handles Tab, lists candidates, accepts a choice, and updates
  the input buffer.
- `include/terminator.h`: each `Tab` owns its cursor, candidate list, and
  temporary chooser state.

### Explanation of the important code

`completion_find()` scans input only up to the cursor, decodes the token's
quotes and backslash escapes, and searches the matching directory portion.
The GUI uses `completion_common_prefix()` to insert only the shared suffix. If
there are multiple choices, `completion_select()` inserts the selected name's
remaining suffix at the saved cursor and leaves following input in place.
Names containing newline or carriage return are skipped because the shell
uses those bytes as input separators.

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
- Task 11's custom script interpreter remains unfinished. `source` sends
  complete lines through the existing shell parser but does not implement the
  assignment's script language.

See [architecture.md](architecture.md) for module responsibilities,
[design.md](design.md) for implementation choices, and
[roadmap.md](roadmap.md) for the assignment status table.
