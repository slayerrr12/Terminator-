# Design decisions

## GUI and tabs

The application uses one Xlib window with one bounded text buffer per tab. The GUI polls the X connection and each tab's output and status pipes, so an idle application sleeps and worker output does not block input handling. The GUI owns all drawing and Xlib calls. A fixed X font and simple text rendering keep the implementation small; this is not a PTY-backed terminal emulator.

Tabs have independent input pipes, output pipes, shell contexts, logical working directories, and foreground process groups. Thread mode runs a worker thread per tab. Process mode runs a worker process per tab. The GUI relays Ctrl+C to the active tab's foreground process group and closes that tab's worker when requested.

## Shell and execution

The parser tracks quotes, escapes, and nested command substitutions before splitting words, pipes, and redirection operators. It retains raw words until expansion so quoted wildcards stay literal. Expansion supports environment variables, `$?`, nested command substitution, and globbing relative to the tab's logical directory.

The executor creates `N-1` pipes for a pipeline of `N` commands, forks each stage, wires descriptors with `dup2()`, applies per-stage input/output redirection, and waits for the foreground process group. A failed input open reports an error and does not launch the command. The `source` built-in reads and executes complete commands from a file in the current worker context.

## History and shared state

History is an in-memory ring with 1,024 entries. One unnamed semaphore protects updates and snapshots. Thread mode initializes it for threads in the process; process mode places the ring in shared anonymous memory and initializes a process-shared semaphore.

## Limits

The parser implements the project's current shell subset, not full Bash grammar. It does not yet parse conditional command lists, shell assignments, background jobs, or compound script syntax. There is no PTY, terminal mode negotiation, full screen-buffer model, or complete keyboard protocol. The history ring is not persisted to disk. Later assignment features are listed in [roadmap.md](roadmap.md).
