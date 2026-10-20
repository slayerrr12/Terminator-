# Architecture

## Modules

- `src/main.c` parses the launch mode, initializes shared history, creates the first tab, and coordinates shutdown.
- `src/gui.c` owns Xlib, text input, drawing, tab selection, and the `poll()` event loop.
- `src/tab.c` owns per-tab pipes and starts, closes, and joins each worker thread or process.
- `src/shell.c` reads framed commands, updates history, dispatches built-ins, and returns completion status to the GUI.
- `src/parser.c` handles incomplete input, tokenization, quote-aware parsing, and word expansion.
- `src/executor.c` handles command expansion, filename globs, redirection, pipeline setup, process groups, and background jobs.
- `src/multiwatch.c` supervises concurrent command runs and forwards their output with `poll()`.
- `src/history.c` stores bounded shared history and protects it with an unnamed POSIX semaphore.
- `include/terminator.h` and `include/shell.h` define the interfaces shared by these modules.

## Command flow

The GUI sends a complete command to the selected worker over a pipe. A worker owns a `ShellContext` with its logical working directory, last exit status, and job table. The parser converts input into a `Pipeline` containing `SimpleCommand` values and an optional trailing background marker. The executor expands words, prepares redirections, and launches one child per pipeline stage. Every stage in a pipeline shares a process group. Foreground waits observe stopped children; background groups stay in the tab's job table for `jobs`, `fg`, and `bg`. The last stage's status becomes `$?`. The GUI collects output and completion status from every live tab without blocking X events.

State-changing and job-control built-ins run in the worker context for a single command. Pipeline stages and external commands run in children. Child processes change to the tab's logical cwd after `fork()`, so a `cd` in one tab cannot change another tab's directory.

## Execution modes and shared history

Both modes use the same worker, parser, executor, and history code. Thread mode starts a worker thread per tab and stores history in shared process memory. Process mode forks one worker per tab and maps history with `MAP_SHARED`. In both modes the shared history is guarded by a process-capable unnamed semaphore. Each tab's job table is private to its worker. Only the GUI thread calls Xlib.

`multiWatch` starts a supervisor in its own process group. The supervisor launches the requested commands concurrently, polls their separate output pipes, and restarts each command two seconds after it exits. Ctrl+C targets that group, leaving the tab's shell worker alive.

## Extending the shell

The parser, shell context, and GUI input cursor are the extension points for searchable history, completion, and script interpretation. The current history ring is fixed-size; a completion implementation can scan the tab's logical working directory and use the input word before the cursor as its prefix.
