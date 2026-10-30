# Automated test suite

Run the complete suite with:

```sh
make test
```

The Makefile builds the test runner, prepares temporary test data, attempts the
application build, runs the unit, integration, regression, and GUI checks, and
returns a nonzero status if a test or the application build fails. Each test
prints its ID, feature, scenario, input, expected result, observed result, and
status. Temporary files and worker processes are removed by the runner.

## Make targets

| Target | What it runs |
| --- | --- |
| `make test` | Full suite and application build attempt |
| `make test-unit` | Parser, history, scrollback, and hotkey tests |
| `make test-integration` | Shell commands, worker modes, job control, signals, and multiWatch |
| `make test-regression` | Compact multiWatch command-list regression from the reported screenshot |
| `make test-gui` | Build and start both GUI execution modes, using Xvfb when available |
| `make test-verbose` | Alias for `make test`; individual test results are always printed |
| `make test-clean` | Remove the generated test executable |
| `make clean` | Remove the application and all generated build files |

## Coverage map

| IDs | Feature | Coverage |
| --- | --- | --- |
| `P001`–`P013` | Parser | Empty input, unmatched quotes, nested and multiline substitutions, backslash continuation, quoting around operators, pipeline and redirection syntax, malformed forms, many arguments, repeated redirections, and a near-limit command line |
| `P011` | Parser regression | Checks that carriage-return whitespace does not hang the lexer. It runs in a disposable child with a timeout so a hang cannot stall the suite. |
| `H002`–`H006` | Shared history | Ring wraparound and ordering, recent-entry lookup, bounded snapshots, ignoring empty commands, and an out-of-range lookup |
| `S001`–`S006` | Scrollback | Joining output chunks, carriage return/backspace/tab handling, line and byte limits, clear, and scrolling up and back to live output |
| `K001`–`K002` | Hotkeys | All default key mappings, config loading, custom bindings, conflicts, unknown actions, and invalid key names |
| `E001`–`E026` | Shell and executor | External commands, quotes, variable and nested/multiline command substitution, exit status, wildcard expansion, input/output/append and repeated redirection, redirection errors, five-stage pipelines, an invalid pipeline stage, `cd`, `history`, `clear`, multi-command `source`, backticks, background jobs, and `exit` |
| `W002`–`W105` | Tab workers | Thread and process modes, command delivery and output, independent per-tab working directories, shared history, and `clear` status messages |
| `J002`–`J106` | Signals and job control | Ctrl+C recovery, Ctrl+Z, `jobs`, `bg`, `fg`, and recovery in both worker modes |
| `M002`–`M104` | `multiWatch` | Concurrent command output, formatted separators, Ctrl+C cleanup, and worker recovery in thread and process modes |
| `R001` | `multiWatch` regression | Reproduces the compact comma-separated list from the screenshot without spaces after commas. It checks that three commands remain separate. |
| `G001`–`G002` | GUI startup | Starts the X11 application in both execution modes and checks that each remains alive until the test shuts it down. Uses Xvfb when available. |

## Source module inventory

| Module | Responsibility | How it is exercised |
| --- | --- | --- |
| `src/main.c` | Initialize shared history, choose thread or process mode, create the first tab, and install shutdown handlers | GUI startup test when the app builds |
| `src/gui.c` | X11 window, prompt, input editing, tabs, hotkeys, output polling, and drawing | GUI startup smoke test only; event-by-event GUI interaction is not automated yet |
| `src/tab.c` | Per-tab pipes, worker creation, close, and shutdown | Worker pipe behavior is tested using the same `worker_run` protocol; app build covers this file |
| `src/parser.c` | Completeness checks, tokenization, pipeline parsing, quote/variable/substitution expansion, and path expansion | Parser unit tests and shell execution tests |
| `src/executor.c` | Redirection, `fork`/`execvp`, pipelines, process groups, background jobs, and status handling | Shell integration and job-control tests |
| `src/shell.c` | Built-ins, per-worker shell state, source files, and newline-framed worker input | Direct shell tests and thread/process worker tests |
| `src/multiwatch.c` | Parallel watched commands, output forwarding, restart timing, and interruption | Thread/process integration and compact-syntax regression tests |
| `src/history.c` | Semaphore-protected shared history ring | Capacity, ordering, snapshot, lookup, and worker-sharing tests |
| `src/scrollback.c` | Bounded output lines, control characters, clear, and viewport movement | Scrollback unit tests |
| `src/hotkeys.c` | Default bindings, configuration parsing, conflict checks, and action lookup | Hotkey unit tests |
| `src/selection.c` | Mouse selection and X11 clipboard transfer | Not directly tested; requires GUI event automation |

## Limits

The GUI checks currently cover startup and shutdown, while command editing,
click-based tab switching, clipboard selection and paste, rendering, and mouse
selection are not driven through synthetic X11 events. Worker and shell
behavior is exercised directly through the same pipe protocol used by tabs.
The GUI and tab files are not linked into the independent shell test runner.
The GUI startup check exercises them only after a successful application build.
The selection and clipboard module is not directly exercised.

Permission-denied cases are not asserted because the suite may run as root,
which can bypass ordinary file permission bits. Missing-file and invalid-path
errors are checked instead. Future assignment items that are absent from the
source, including searchable persistent history, filename completion, and the
custom script interpreter, are outside this suite.

Known issues are reported as failing tests rather than hidden or worked around.
In particular, `P011` checks the lexer’s carriage-return behavior and `R001`
checks the compact `multiWatch` syntax shown in the issue report. The complete
test target also attempts the application build, so application compile errors
are visible in the same workflow.
