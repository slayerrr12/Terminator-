# Terminator project notes

- Keep Xlib calls in `src/gui.c`; worker code must not touch X11.
- Keep tab lifecycle and worker pipes in `src/tab.c`.
- Shared shell behavior belongs in `src/shell.c`, `src/parser.c`, and `src/executor.c`.
- Each tab has its own `ShellContext`; use its logical cwd instead of changing the GUI or worker process cwd.
- Use `make` to build and `make test` to run worker and X11 interaction checks.
- Later assignment work is tracked in `docs/roadmap.md`.
