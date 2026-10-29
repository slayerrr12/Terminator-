# Tasks 1 and 2: GUI, tabs, and commands

This guide explains how the current code handles the X11 window, multiple
tabs, thread mode, process mode, and commands. The assignment PDF remains the
complete requirements document. This explanation describes the code; it does
not change the assignment requirements.

## The main idea

Terminator has one GUI window and one shell worker for each tab. The GUI owns
the X11 window and keyboard events. A worker owns the shell state for its tab
and handles commands sent to that tab. Pipes connect the GUI and each worker.

```text
                         one Terminator window
                                  |
                     one X11 GUI event loop
                       /                    \
               Tab 1 buffers              Tab 2 buffers
                    |                          |
                three pipes                three pipes
                    |                          |
              Tab 1 worker                Tab 2 worker
             (thread or process)          (thread or process)
                    |                          |
             command child processes, created with fork()
             and replaced with external programs using execvp()
```

The diagram shows the roles. In thread mode, both workers are threads inside
the same application process. In process mode, each worker is a separate
process. In either mode, external commands run in child processes.

## Where the main pieces live

| File | Responsibility |
| --- | --- |
| [`src/main.c`](../src/main.c) | Chooses thread or process mode, initializes shared history, creates the first tab, and starts the GUI. |
| [`src/gui.c`](../src/gui.c) | Creates the X11 window, edits the active tab's input, sends completed commands, draws output, and watches all tabs for output. |
| [`src/selection.c`](../src/selection.c) | Tracks mouse text selection and exchanges selected or pasted text through X11 clipboard selections. |
| [`src/tab.c`](../src/tab.c) | Creates each tab's pipes and starts, closes, or joins/reaps its worker. |
| [`src/shell.c`](../src/shell.c) | Runs the worker's input loop and implements built-in commands. |
| [`src/parser.c`](../src/parser.c) | Turns a command line into tokens and parsed command/pipeline structures. |
| [`src/executor.c`](../src/executor.c) | Runs built-ins or forks child processes, connects pipelines and redirections, calls `execvp()`, and waits for jobs. |
| [`include/terminator.h`](../include/terminator.h) | Defines `App`, `Tab`, and the pipe and history state shared by the GUI and workers. |
| [`include/selection.h`](../include/selection.h) | Declares the small interface between the GUI and text-selection code. |
| [`include/shell.h`](../include/shell.h) | Defines `ShellContext`, parsed commands, pipelines, and per-tab jobs. |

## Thread mode and process mode

Run the application without an option for thread mode, or pass the assignment's
option for process mode:

```sh
./terminator
./terminator useProcess=True
```

`main()` in `src/main.c` selects `MODE_THREADS` by default. It selects
`MODE_PROCESSES` only when it sees `useProcess=True`. Both modes call the same
`worker_run()` shell code; only the way a tab's worker starts is different.

### Thread mode

In `src/tab.c`, `app_add_tab()` calls `pthread_create()` and passes that tab's
`Tab` structure to `run_shell_worker_thread()`. That function calls
`worker_run()` using the pipe ends assigned to the tab.

All tab workers share one process and its address space. Each worker still gets
its own `ShellContext`: `worker_run()` creates it as a local variable when that
worker starts. That context holds the tab's working directory, last command
status, and job table. So tab 1's `cd` changes tab 1's context; it does not
change tab 2's context or the GUI's working directory. The shared command
history is an intentional exception: all workers use the same history object,
protected by a semaphore.

### Process mode

In `src/tab.c`, `app_add_tab()` calls `fork()` to create a worker process for
the tab. The child closes pipe ends it does not use and calls the same
`worker_run()` function. The GUI keeps the other pipe ends and remembers the
worker's PID so it can close the tab later.

The history object is placed in shared memory by `main.c`, and its semaphore
is initialized for process sharing. Each process has its own `ShellContext`
and job table. A process-mode worker also closes its inherited X11 connection
and the GUI-side pipe ends so that it cannot accidentally keep another tab's
communication channel open.

### Quick comparison

| | Thread mode | Process mode |
| --- | --- | --- |
| Tab worker created by | `pthread_create()` | `fork()` |
| Shell state | A separate `ShellContext` in each worker thread | A separate `ShellContext` in each worker process |
| History | Shared memory within the application process | Explicit shared memory with a process-shared semaphore |
| GUI | One X11 GUI in the application process | One X11 GUI in the parent process |
| External commands | A worker thread calls `fork()`; the child calls `execvp()` | A worker process calls `fork()`; the child calls `execvp()` |

The process relationship for two tabs looks like this:

```text
Thread mode
Terminator process
  main GUI thread
  worker thread for Tab 1 --calls fork()--> command child --execvp()--> program
  worker thread for Tab 2 --calls fork()--> command child --execvp()--> program

Process mode
Terminator GUI process --fork()--> worker process for Tab 1
                                   `--fork()--> command child --execvp()--> program
                         `--fork()--> worker process for Tab 2
                                      `--fork()--> command child --execvp()--> program
```

In thread mode, the worker thread calls `fork()` in the Terminator process;
the new command child is a process, not another thread. In process mode, the
tab worker is already its own process and forks the command child. A pipeline
creates one command child per stage.

## How one GUI manages all the tabs

There is no separate X11 window or GUI event loop for every tab. `gui_run()` in
`src/gui.c` opens one display and one window. The `App` structure stores all
the `Tab` structures and an `active_tab` index.

The event loop watches the X11 connection and both GUI-facing pipes for every
tab with `poll()`:

1. A key press is handled for `app->tabs[app->active_tab]`, the selected tab.
   Clicking a tab changes `active_tab`; Ctrl+T or **[+]** calls `app_add_tab()`.
2. Enter sends the completed input line through that tab's
   `gui_to_worker_fd` pipe.
3. The same `poll()` loop continues watching every tab, including tabs that are
   not selected. When a worker's output pipe is ready, the GUI adds those bytes
   to that tab's own scrollback buffer.
4. When a worker's status pipe is ready, the GUI handles its clear-screen
   request, refreshes the tab's working directory, and marks the command as
   finished. The worker keeps the exit status in its own `ShellContext` for the
   next `$?` expansion.
5. The window is redrawn from the selected tab's saved input and output
   buffers. Switching tabs therefore shows that tab's existing display state.

`poll()` waits for an X11 or pipe event instead of repeatedly checking in a
tight loop. A tab's worker keeps running while another tab is displayed; its
output remains associated with its own pipe and buffers.

Each tab has three one-way pipes. For a pipe returned by `pipe(fds)`, `fds[0]`
is the read end and `fds[1]` is the write end:

| Pipe | GUI end | Worker end | What it carries |
| --- | --- | --- | --- |
| Command input | `gui_to_worker_fd` writes | `worker_input_fd` reads | Completed command lines from the selected tab. |
| Command output | `worker_to_gui_fd` reads | `worker_output_fd` writes | Text produced by that tab's commands. |
| Status | `status_from_worker_fd` reads | `worker_status_fd` writes | Exit status, clear-screen request, and updated working directory. |

The fields are in `Tab` in `include/terminator.h`; the pipe creation and
assignment are in `app_add_tab()` in `src/tab.c`.

## Task 1: X11 GUI and tabs

### What happens at startup

`main()` initializes the application and starts its first tab. Then
`gui_run()` in `src/gui.c` calls `XOpenDisplay()`, creates the window with
Xlib, selects keyboard, button, and window events, and maps the window. The GUI
draws the tab bar, prompt, input line, and scrollback from the `App` and `Tab`
data.

### How input reaches a tab

`handle_key_press()` translates X11 key events with `XLookupString()` and edits
the active tab's `input_buffer` and `input_cursor`. On Enter,
`submit_input()` appends the visible prompt and typed text to that tab's
scrollback. It checks whether multiline input is complete, then writes the
completed command to that tab's command-input pipe.

### How output returns to the window

The worker writes command output to its output pipe. The GUI reads that pipe
when `poll()` reports it ready and appends the text to that tab's saved output
lines. A status message travels separately, so the GUI can update the tab's
prompt directory and know when its command finished. Drawing from saved
buffers lets the GUI repaint the window or switch tabs without rerunning a
command.

This project draws command text in an X11 window but does not use a PTY. As a
result, it does not provide every behavior of a full Linux terminal emulator,
such as full-screen terminal applications and all terminal control sequences.
That implementation limit does not remove or alter any requirement in the
assignment PDF.

## Task 2: built-in and external commands

### One command's path through the code

After a worker reads a complete line, `worker_run()` in `src/shell.c` calls
`shell_run_line()` in `src/executor.c`. The parser turns the text into command
arguments and pipeline stages. The executor expands arguments, then chooses
between a built-in and an external command.

```text
GUI input
  -> that tab's command pipe
  -> that tab's worker_run()
  -> shell_run_line() parses and expands the line
  -> shell_execute()
       -> built-in handler, or
       -> execute_pipeline() forks a child
            -> run_command_in_child() applies cwd/redirection
            -> execvp() replaces the child with the external program
  -> worker output pipe
  -> that tab's GUI scrollback
```

### Built-in commands

The built-in lookup table in `src/shell.c` maps names to functions. It
currently includes `cd`, `clear`, `exit`, `history`, `source`, `multiWatch`,
`jobs`, `fg`, and `bg`.

Commands that change persistent shell state need to run in that tab's worker.
For example, `cd` updates `ShellContext.working_directory`. If `cd` ran only in
a temporary child process, that child would change directory and then exit;
the worker waiting for the next command would still have the old directory.
For this reason, `shell_execute()` runs a single foreground stateful built-in
in the worker's own `ShellContext`. A built-in used as a pipeline stage or in a
background command runs in a child, where its state change is not expected to
persist after that command.

The `exit` built-in sets `should_exit`, which makes that worker leave its read
loop. In thread mode this ends the tab's worker thread; in process mode it ends
the tab's worker process. It does not directly exit the GUI or another tab.

### External commands and `execvp()`

An external command such as `ls` is not run inside the GUI and does not replace
the worker. `execute_pipeline()` in `src/executor.c` calls `fork()` to create a
child process. In that child, `run_command_in_child()` changes to the tab's
working directory, connects any required pipes and redirections, and calls
`execvp()` with the argument array. `execvp()` searches `PATH` for the program
when its name does not contain a directory path.

The call in `src/executor.c` is:

```c
execvp(command->arguments[0], command->arguments);
```

For `ls -l`, the parser prepares an argument array like `{"ls", "-l", NULL}`.
The first item names the program, the remaining items are its arguments, and
the final `NULL` marks the end of the array.

The important separation is:

```text
worker: reads the next command and keeps the tab's shell state
  `-- forked child: execvp() turns this child into ls, sort, gcc, etc.
```

When `execvp()` succeeds, it replaces the child process with the requested
program. It does not return. The worker remains alive and waits for a
foreground command or pipeline to finish. If the command cannot be found,
`execvp()` returns an error, the child reports it through the tab's output
pipe, and the worker can accept the next command.

### How commands stay separate between tabs

Every tab gets its own command-input pipe and its own worker. Each worker reads
only its own pipe and has its own `ShellContext`, including its working
directory, `$?` status, and job table. Thus, while Tab 1 runs `cd /tmp`, Tab 2
can keep using its previous directory and can run a different command. Their
outputs return through different output pipes, so the GUI places each result
in the correct tab.

Thread mode does not mean the shell calls `execvp()` directly from the GUI
thread. The per-tab worker thread receives and interprets the command; the
executor forks a command child; only that child is replaced by the external
program. Process mode follows the same command path, with the worker itself
being a separate process instead of a thread.

The shell's supported syntax is a project subset, not all of Bash. Parsing,
quoting, multiline input, redirection, and pipelines are described in the
assignment task guide and the source comments. The full requirements remain
in the assignment PDF.
