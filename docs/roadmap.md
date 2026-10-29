# Assignment status

This table describes the code currently in the repository. The assignment PDF
is the complete specification; an implemented subset should not be mistaken
for a finished later task.

| Assignment area | Status | Current behavior |
| --- | --- | --- |
| Execution modes | Implemented | Thread mode is the default; `useProcess=True` starts worker processes. |
| Task 1: X11 GUI and tabs | Implemented for the project UI | Per-tab input and output, tab switching, scrollback, and a `poll()` event loop are present. It is not a PTY terminal emulator. |
| Task 2: built-in and external commands | Implemented for the listed commands | External commands use `fork()` and `execvp()`. Built-ins include `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg`, `source`, and `clear`; `$?` is available. |
| Task 3: multiline input and quoting | Implemented for the specified requirements | Continuation lines, multiline single/double quotes, nested and multiline `$()` and backtick substitutions, and the requested single/double quote expansion rules are supported. This is not a full Bash implementation. |
| Task 4: I/O redirection | Implemented | Input `<`, output `>`, append `>>`, and redirection on pipeline stages are supported. |
| Task 5: pipes | Implemented | Pipelines can have multiple stages and can be combined with redirection. |
| Task 6: `multiWatch` | Implemented | Commands run concurrently; `poll()` collects their output and completed commands restart after about two seconds. |
| Task 7: configurable hotkeys | Implemented | Editing, history, clear, close, copy/paste, interrupt, suspend, and new-tab bindings load from `~/.terminatorrc`; invalid or conflicting bindings are reported and use their defaults. |
| Task 8: signals and job control | Implemented for the assignment requirements | Ctrl+C and Ctrl+Z signal the active job process group; `cmd &`, `jobs`, `fg`, and `bg` are supported. PTY-based terminal control is not implemented. |
| Task 9: searchable persistent history | Partial | Tabs share a synchronized in-memory ring of up to 1,024 commands; Up/Down browse it. The history file, 10,000-entry limit, and Ctrl+R search are not implemented. |
| Task 10: file-name completion | Not implemented | Tab does not scan the current directory to complete file names. |
| Task 11: Bash-subset script interpreter | Not implemented | `source` executes complete lines through the current shell parser; it does not interpret assignments, positional parameters, or conditional blocks itself. |

## Main extension points

- Add key editing and configurable bindings in `src/gui.c`.
- Add history persistence and search around `src/history.c` and the GUI input
  handling in `src/gui.c`.
- Add filename completion using the current tab's input cursor and
  `working_directory` in `src/gui.c`.
- Add the custom script grammar and execution path in `src/parser.c` and
  `src/shell.c`.
- Add PTY-based terminal control alongside worker and process-group handling in
  `src/tab.c`, `src/executor.c`, and `src/gui.c`.

The current project structure leaves these as future additions; it does not add
empty modules or placeholder code for them.
