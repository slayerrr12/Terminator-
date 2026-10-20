# Roadmap

## Current milestone

The current implementation covers the X11 tabbed GUI, thread and process worker modes, built-ins and external commands, multiline quoting and substitutions, redirection, pipelines, `multiWatch`, and basic background job control. `make test` runs worker tests in both modes and drives the X11 interface under Xvfb.

## Later assignment work

| Feature | Main integration points |
|---|---|
| Configurable hotkeys | Input handling in `gui.c` and a small persisted key-binding configuration |
| PTY-backed terminal job control | `ShellContext`, process groups in `executor.c`, PTY management, and control keys in `gui.c` |
| Persistent searchable history | `history.c`, the shared history layout, and search input in `gui.c` |
| Filename completion | Current input cursor in `gui.c` and a completion request using the tab's cwd |
| Bash-subset script interpreter | `parser.c`, `ShellContext`, and a script execution path in `shell.c` |
