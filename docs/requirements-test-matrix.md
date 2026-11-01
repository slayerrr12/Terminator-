# Assignment Task Test Matrix

This document maps the assignment PDF to executable tests. **make test** runs
the full matrix. Test output includes Task and Mode fields, and the same result
rows are written as JUnit XML to **build/tests/reports/full.xml**. A mode is
`thread` or `process` for mode-specific integration cases; `shared` marks
module tests that run the same implementation used by both modes.

## Execution-mode rule

The GUI integration tests launch the real application twice: default thread
mode and **useProcess=True**. Worker integration tests also run once with a
thread worker and once with a process worker. Parser, history, completion,
scrollback, and hotkey unit tests run against the shared module code; those
modules do not have separate implementations per mode. End-to-end tests then
exercise those same features through both modes where the feature is used by
the GUI or a worker.

| PDF task | Requirements covered | Thread-mode checks | Process-mode checks | Main implementation |
| --- | --- | --- | --- | --- |
| 1. X11 GUI and tabs | App starts; input/output; independent tab state; tab switching/closing; scrollback; event-driven GUI; a crashed process-mode tab leaves another tab available | G001, G003–G012; W002–W005, W006–W019 | G002, G103–G112; W102–W119, including W111 | src/gui.c, src/tab.c, src/scrollback.c, src/main.c |
| 2. Built-in and external commands | External execvp; shell built-ins; unknown-command errors; $?; source; less; recovery after a command child crashes | E001–E006, E010, E012–E015, E020, E030–E043; W006, W010, W014–W017 | Same mode-independent integration cases; worker parity W106, W110, W114–W117 | src/shell.c, src/executor.c |
| 3. Multiline input and quoting | Incomplete quotes, continuations, nested/multiline substitution, quoted operators, and parser stress against malformed input | P001–P014, E016–E017, E025–E026, W007 | P001–P014, E016–E017, E025–E026, W107 | src/parser.c, src/shell.c |
| 4. I/O redirection | <, >, >>, combined input/output, pipeline-stage redirection, missing and invalid paths | E007–E009, E018, E021, E023, E035, E039–E040; W008, W013, W018–W019 | Same mode-independent integration cases; worker parity W108, W113, W118–W119 | src/executor.c |
| 5. Pipes | Multi-stage pipelines; pipeline plus redirection; large streams; less as a stage; invalid stages; early reader exit and SIGPIPE | E011, E022, E044–E045; W009, W012 | Same mode-independent integration cases; worker parity W109, W112 | src/executor.c |
| 6. multiWatch | Parallel commands, separated output, restart, Ctrl+C cleanup, sibling/supervisor survival when a watched command crashes | M101–M106 | M001–M006 | src/multiwatch.c, src/shell.c, src/executor.c |
| 7. Hotkeys | Required default bindings, config parsing/conflicts, editing keys, clear, X11 copy/paste and string fallback | K001–K002, G005–G006, G009–G010 | Same configuration tests; GUI parity G105–G106, G109–G110 | src/hotkeys.c, src/selection.c, src/gui.c |
| 8. Signals and job control | Ctrl+C, Ctrl+Z, process groups, jobs, fg, bg, invalid job IDs, and recovery | J001–J007, G011 | J101–J107, G111 | src/executor.c, src/gui.c, src/shell.c |
| 9. Searchable history | Persistent 10,000-entry history; 1,000-line display; shared tabs; exact and fallback search; malformed/boundary inputs; concurrent writers | H002–H030, H031–H113, E013, W004, G004 | Shared module tests; worker parity W104, GUI parity G104 | src/history.c, src/gui.c, src/shell.c |
| 10. File-name completion | Matching, quoting, cursor placement, edge filenames, numbered choice, invalid selection, and GUI interaction | C001–C130, G003 | Shared module tests; GUI parity G103 | src/completion.c, src/gui.c |
| 11. Bash-subset script interpreter | Requirements are recorded as unsupported rather than being reported as passing | T110 is explicitly skipped | T110 is explicitly skipped | Not implemented. source runs lines through the current shell parser; it does not interpret variables, positional parameters, or conditional blocks. |

## Crash and hang tests

| Test ID | Mode | Failure exercised | Safety boundary |
| --- | --- | --- | --- |
| P011 | Shared parser code | Carriage-return input previously capable of trapping the lexer in a loop | Runs in a disposable child with a deadline |
| P014 | Shared parser code | 2,000 deterministic malformed quote/operator combinations | Runs in a disposable child; timeout or signal is a failure |
| W010 / W110 | Thread / process worker | A command child receives SIGSEGV; the worker must run a later command | Signal is sent only to a command child |
| W111 | Process workers | Kill one process-mode worker and use another tab worker | Uses a process worker pair; the second worker must print a marker |
| W012 / W112 | Thread / process worker | yes writes after head -n 1 exits, exercising pipe closure and SIGPIPE | Runs in a disposable process with normal SIGPIPE; a deadline and process-group cleanup prevent a runaway writer |
| W013 / W113 | Thread / process worker | Output redirection attempts to open a directory | Checks nonzero status and diagnostic, then continues the mode suite |
| M105 / M005 | Thread / process worker | A watched command crashes while a sibling continues | Interrupts the watcher after the sibling marker, then checks worker recovery |
| G012 | Thread GUI | A command child crashes and the tab accepts a follow-up command | Exercises the real GUI and thread worker |
| G112 | Process GUI | Kill the second tab's actual worker process; original tab remains usable | Uses Linux /proc child listing to identify the new worker; GUI is stopped during cleanup |
| J002 / J102, J004 / J104 | Thread / process worker | Interrupt or suspend a foreground process group | Uses a bounded sleep process and explicit group signaling |
| M103 / M003 | Thread / process worker | Interrupt the multiWatch supervisor and verify the shell worker survives | Uses the supervisor process group and a bounded status wait |

## Important interpretation notes

- Unit tests for parsing and data structures are mode-independent because both
  modes call the same C modules. The mode-specific worker cases verify that
  those modules remain usable through each worker transport.
- The task 1 process-mode isolation test kills a real application tab worker.
  The thread-mode crash test targets a command child. A fatal signal delivered
  directly to any thread is process-wide on Linux, so the assignment's
  process-mode tab-isolation requirement is tested by killing a worker process.
- Permission-denied checks are not reliable when tests run as root. The suite
  checks invalid paths, missing files, directories used as files, file
  permissions on history, and error propagation instead.
- GUI cases use Xvfb when xvfb-run is installed. If no X display is available,
  GUI test rows are marked SKIPPED, and the JUnit report preserves that state.
- Test commands run with deadlines. A timeout is reported as a test failure;
  the early-reader pipe case additionally kills its pipeline process group on
  timeout so a failed case cannot leave a writer running.
