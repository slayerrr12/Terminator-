# Design notes

This document explains the choices made in the current implementation. For a
beginner-oriented walkthrough of each task, see
[TASK_IMPLEMENTATION_README.md](TASK_IMPLEMENTATION_README.md).

## Window and tabs

`gui.c` owns the X11 display, window, keyboard input, drawing, and event loop.
It keeps an editable input buffer and a scrollback buffer for each tab.
`selection.c` handles mouse text selection and X11 clipboard copy and paste so
those details stay out of the window event loop.
`scrollback.c` stores up to 10,000 lines and 1 MiB of allocated line text per
tab in a ring, so old output is evicted without moving every remaining line.
For a line larger than that byte budget, it keeps the tail. Each tab tracks
whether it follows new output or is showing an earlier line. In the latter
case, output arriving later does not move the viewed line. The mouse wheel and
Page Up/Page Down control vertical scrolling; Shift + mouse wheel scrolls
horizontally across long lines. Scrolling back to the bottom resumes following
output. When
a command is complete, the GUI writes it to that tab's command pipe. The worker
returns output and a small status message through two other pipes.

The event loop uses `poll()` for both the X11 connection and worker pipes. This
lets the window receive keyboard events while commands produce output, without
checking the pipes in a tight loop. Tabs stay active when hidden because their
workers are independent of which tab is currently drawn.

The window draws text directly with Xlib. It has no PTY, terminal mode, or
ANSI control-sequence interpreter, so full-screen interactive programs are
outside the current design.

## Worker modes and shared state

`./terminator` starts one worker thread for each tab. The threads share the
application's history object. `./terminator useProcess=True` starts each tab
worker with `fork()`; the history object then lives in `MAP_SHARED` memory.

Each tab has a `ShellContext` containing its working directory, last exit
status, and job table. The GUI's process directory is not changed by `cd`.
External commands run in child processes and call `chdir()` to the tab's
working directory before execution.

`SharedHistory` contains a semaphore, the selected history-file path, and
cached file metadata. Commands are written to `~/.terminator_history` by
default; setting `TERMINATOR_HISTORY_FILE` selects another file. The newest
10,000 commands are retained, and `history` displays at most the newest 1,000.
The semaphore coordinates tabs within one application. A sidecar lock file
uses `flock()` to coordinate separate application processes. In process mode,
the small coordination structure is in shared memory and the history records
remain in the file.

The history-related fields are intentionally split by lifetime. `App` holds a
pointer to the shared `SharedHistory` object. Each `Tab` has its own
`history_position` and draft buffer for Up/Down browsing, plus a separate
search buffer for Ctrl+R. Filename completion state is also per tab: the
candidate names, their count, the cursor to resume at, the common-prefix
length, and the active quote style live in `Tab`. `CompletionResult` in
`completion.h` temporarily holds the matches and token information while a
completion scan runs. The command records themselves stay on disk rather than
being kept in a permanent in-memory array.

Ctrl+R switches the active tab to a separate search prompt. On Enter,
`history_search()` first scans saved commands for the case-sensitive search
text as a substring and prints every command containing it, in history-file
order. If there are no such commands, it finds each command's longest shared
continuous substring with the search term and prints all commands tied for the
longest match, provided the match is longer than two bytes. Otherwise the GUI
prints the no-match message. The search buffer is separate from the normal
command line, so the search term is not executed or added to history.

The assignment wording says “the most recent command that exactly matches the
term,” while its example displays more than one matching command. The current
implementation follows the example by displaying all substring matches; it
does not return only the newest whole-command match.

The ordinary `history` built-in uses a different path: `shell.c` asks
`history_snapshot()` for up to the newest 1,000 entries and prints them. Up and
Down use `history_get_recent()` to fetch one command at a time from the shared
file, while Ctrl+R calls `history_search()`.

## Filename completion and `less`

`completion.c` scans the active tab's logical working directory (or the
directory portion typed in the path) for names matching the word before the
cursor. It decodes quotes and backslash escapes while matching, then encodes
only the missing suffix for the current quote style. Directories receive a
trailing slash. Multiple candidates are sorted; the GUI inserts their common
prefix and presents a numbered chooser if they remain ambiguous. The insertion
routine leaves text after the cursor in place. Names containing newline or
carriage return are omitted because the shell treats those bytes as input
separators. Completion fills the input line; it does not execute the command.

The `less` built-in streams file bytes or standard input to its output file
descriptor. In the GUI, that output flows into the tab's scrollback buffer, so
Page Up, Page Down, and the mouse wheel provide navigation. It does not enter a
full-screen mode because the application has no PTY.

## Parsing and expansion

`parser.c` uses the same quote-aware word scanner to decide whether input is
complete and to split tokens. The scanner keeps multiline quotes and nested
`$()` or backtick substitutions together, including quoted parentheses inside
substitutions. The GUI asks for another line until those constructs close. The
lexer separates operators such as `|`, `<`, `>`, `>>`, and `&`; the parser
groups tokens into commands and a pipeline.

Before execution, the shell expands environment variables, `$?`, command
substitutions, and eligible wildcard patterns. A command substitution runs in
a copy of the current tab's shell state and writes output to a temporary file.
The parser removes trailing newlines from captured output. Single quotes keep
their contents literal; double quotes still allow variable and command
substitution. This is a deliberate shell subset, not every Bash expansion or
grammar rule.

## Processes, pipelines, and redirection

`executor.c` launches external commands with `fork()` and `execvp()`. A
pipeline with N commands uses N-1 pipes and starts one child process for each
command. The child connects its standard input and output with `dup2()`, then
applies any file redirections.

Input redirection opens the requested file for reading. Output redirection
creates or truncates a file; `>>` appends. A failed input open is reported and
the command is not started. The output pipe connects command output back to the
GUI.

Each pipeline shares a process group. The worker waits for foreground jobs and
records background jobs in its own fixed-size table. `jobs` checks job states,
`fg` resumes and waits for one job, and `bg` resumes it without waiting. The
GUI sends Ctrl+C and Ctrl+Z to the selected tab's foreground process group, so
the signals reach the command rather than the GUI or shell worker. During
shutdown, stopped jobs are resumed before SIGTERM is sent so the worker can
reap them and finish closing the tab.

## `multiWatch`

`multiwatch.c` starts one child process for each command string. Each child has
its own output pipe. A supervisor polls those pipes and forwards ready output
to the tab. When a command exits, the supervisor starts it again after two
seconds. Ctrl+C targets the supervisor's process group; the tab worker remains
available for the next command.

## Hotkey configuration

`hotkeys.c` starts with the assignment defaults, then reads
`~/.terminatorrc`. Each line maps an action such as `move_left` to a key such as
`Alt+Left`. The loader checks key names and modifier combinations, detects
collisions, and keeps the default for invalid or conflicting entries. The GUI
reports those problems in the first tab's scrollback. A sample configuration
is in `docs/terminatorrc.example`.

## Current limits

- `source` executes complete input lines with the current shell parser. It is
  not the custom Bash-subset script interpreter in Task 11.
- Background jobs use process groups and signals, but there is no PTY to
  provide full interactive terminal job control.

These limits are summarized by assignment task in [roadmap.md](roadmap.md).
