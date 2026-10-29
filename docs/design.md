# Design notes

This document explains the choices made in the current implementation. For a
beginner-oriented walkthrough of each task, see
[TASK_IMPLEMENTATION_README.md](TASK_IMPLEMENTATION_README.md).

## Window and tabs

`gui.c` owns the X11 display, window, keyboard input, drawing, and event loop.
It keeps an editable input buffer and a scrollback buffer for each tab.
`scrollback.c` stores up to 10,000 lines and 1 MiB of allocated line text per
tab in a ring, so old output is evicted without moving every remaining line.
For a line larger than that byte budget, it keeps the tail. Each tab tracks
whether it follows new output or is showing an earlier line. In the latter
case, output arriving later does not move the viewed line. The mouse wheel and
Page Up/Page Down control the view; scrolling back to the bottom resumes
following output. When
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

`SharedHistory` is a fixed-size ring with 1,024 entries. The same unnamed POSIX
semaphore protects history reads and writes in both modes. In process mode,
the semaphore is initialized with process sharing enabled.

## Parsing and expansion

`parser.c` first checks whether the input is complete. It tracks open quotes,
backticks, escapes, and nested `$()` substitutions so the GUI can keep
collecting multiline input. The lexer keeps quoted text together and separates
operators such as `|`, `<`, `>`, `>>`, and `&`. The parser groups tokens into
commands and a pipeline.

Before execution, the shell expands environment variables, `$?`, command
substitutions, and eligible wildcard patterns. This is a deliberate shell
subset. It does not implement every Bash expansion or grammar rule.

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
GUI sends Ctrl+C and Ctrl+Z to the selected tab's foreground process group.

## `multiWatch`

`multiwatch.c` starts one child process for each command string. Each child has
its own output pipe. A supervisor polls those pipes and forwards ready output
to the tab. When a command exits, the supervisor starts it again after two
seconds. Ctrl+C targets the supervisor's process group; the tab worker remains
available for the next command.

## Current limits

- History is shared between tabs but is only stored in memory. It keeps at
  most 1,024 commands and is cleared when the application exits.
- `source` executes complete input lines with the current shell parser. It is
  not the custom Bash-subset script interpreter in the assignment.
- Hotkeys cannot be configured or saved. History search and filename
  completion are not implemented.
- Background jobs use process groups and signals, but there is no PTY to
  provide full interactive terminal job control.

These limits are summarized by assignment task in [roadmap.md](roadmap.md).
