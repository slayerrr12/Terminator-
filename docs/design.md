# Design decisions

## GUI and tabs

The application uses one Xlib window with one bounded text buffer per tab. The GUI polls the X connection and each tab's output and status pipes, so an idle application sleeps and worker output does not block input handling. The GUI owns all drawing and Xlib calls. A fixed X font and simple text rendering keep the implementation small; this is not a PTY-backed terminal emulator.

Tabs have independent input pipes, output pipes, shell contexts, logical working directories, and job tables. Thread mode runs a worker thread per tab. Process mode runs a worker process per tab. The GUI relays Ctrl+C and Ctrl+Z to the active tab's foreground process group and closes that tab's worker when requested.

## Shell and execution

The parser tracks quotes, escapes, and nested command substitutions before splitting words, pipes, and redirection operators. It retains raw words until expansion so quoted wildcards stay literal. Expansion supports environment variables, `$?`, nested command substitution, and globbing relative to the tab's logical directory.

The executor creates `N-1` pipes for a pipeline of `N` commands, forks each stage, wires descriptors with `dup2()`, applies per-stage input/output redirection, and waits for the foreground process group. A failed input open reports an error and does not launch the command. A trailing `&` leaves a process group running in the background and records its ID, process group, state, last-stage PID, and command text in that tab's fixed-size job table. `jobs` reaps completed children, `fg` resumes and waits for a job, and `bg` resumes it without waiting. `source` reads and executes complete commands in the current worker context.

`multiWatch` accepts a bracketed list of command strings. A separate supervisor launches one process per command, uses a `poll()` loop over individual output pipes, and restarts completed commands after two seconds. The GUI sends Ctrl+C to the supervisor's process group; the worker remains available for another command.

## History and shared state

History is an in-memory ring with 1,024 entries. One unnamed semaphore protects updates and snapshots. Thread mode initializes it for threads in the process; process mode places the ring in shared anonymous memory and initializes a process-shared semaphore. Filename completion is not implemented in this milestone; it can use the current tab's input prefix and logical cwd to scan directory entries without adding persistent global state.

## Limits

The parser implements the project's current shell subset, not full Bash grammar. It does not yet parse conditional command lists, shell assignments, or compound script syntax. Job control supports background pipelines and stop/continue signals but does not provide terminal foreground ownership through a PTY. There is no PTY, terminal mode negotiation, full screen-buffer model, or complete keyboard protocol. The history ring is not persisted to disk. Later assignment features are listed in [roadmap.md](roadmap.md).
