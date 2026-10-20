# Architecture

## Modules

- `src/main.c` parses the launch mode, initializes shared history, creates the first tab, and coordinates shutdown.
- `src/gui.c` owns Xlib, text input, drawing, tab selection, and the `poll()` event loop.
- `src/tab.c` owns per-tab pipes and starts, closes, and joins each worker thread or process.
- `src/shell.c` reads framed commands, updates history, dispatches built-ins, and returns completion status to the GUI.
- `src/parser.c` handles incomplete input, tokenization, quote-aware parsing, and word expansion.
- `src/executor.c` handles command expansion, filename globs, redirection, pipeline setup, child execution, and status collection.
- `src/history.c` stores bounded shared history and protects it with an unnamed POSIX semaphore.
- `include/terminator.h` and `include/shell.h` define the interfaces shared by these modules.

## Command flow

The GUI sends a complete command to the selected worker over a pipe. A worker owns a `ShellContext` with its logical working directory and last exit status. The parser converts input into a `Pipeline` containing `SimpleCommand` values. The executor expands words, prepares redirections, and launches one child per pipeline stage. The last stage writes to the tab output pipe; the GUI collects output and completion status from every live tab without blocking X events.

State-changing built-ins (`cd`, `exit`, `history`, and `source`) run in the worker context for a single command. Pipeline stages and external commands run in children. Child processes change to the tab's logical cwd after `fork()`, so a `cd` in one tab cannot change another tab's directory.

## Execution modes and shared history

Both modes use the same worker, parser, executor, and history code. Thread mode starts a worker thread per tab and stores history in shared process memory. Process mode forks one worker per tab and maps history with `MAP_SHARED`. In both modes the shared history is guarded by a process-capable unnamed semaphore. Only the GUI thread calls Xlib.

## Extending the shell

The shell context, pipeline representation, parser entry points, executor, and tab control pipes provide simple extension points for later job control, `multiWatch`, searchable history, completion, and script interpretation. Future work can extend these types and modules as the feature requires it.
