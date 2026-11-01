# Automated test suite

Run the complete suite with one command:

```sh
make test
```

The Makefile builds the test runner and application, runs unit, integration,
regression, and GUI cases, and returns a nonzero status if a build or test
fails. Each reported case includes its input, expected result, actual result,
and status. The suite creates all fixture files below `build/tests/` and removes
them when the runner exits.

## Make targets

| Target | What it runs |
| --- | --- |
| `make test` | Full suite and application build |
| `make test-unit` | Parser, history, completion, scrollback, and hotkey tests |
| `make test-integration` | Shell commands, `less`, pipelines, workers, signals, jobs, and multiWatch |
| `make test-regression` | Compact multiWatch syntax regression from the issue report |
| `make test-gui` | X11 startup and input interaction in both execution modes; uses Xvfb when available |
| `make test-verbose` | Alias for `make test`; per-case output is always printed |
| `make test-clean` | Remove the generated test runner and temporary artifacts |
| `make clean` | Remove the executable and all generated build files |

Every test-runner invocation is bounded by 180 seconds by default. Override
the limit with `make test TEST_TIMEOUT=240`. Worker and GUI event checks also
use shorter polling deadlines for their expected waits.

## Coverage map

| IDs | Feature | Coverage |
| --- | --- | --- |
| `P001`–`P013` | Parser | Empty input, unfinished quotes, nested and multiline substitutions, backslash continuation, quoted operators, pipelines, redirections, malformed syntax, many arguments, repeated output redirection, near-limit input, and carriage-return handling |
| `H002`–`H028`, `H112`–`H113` | Persistent history | 10,000-entry retention, ordering, snapshots, restart persistence, escaped newline/CR/backslash, exact search, longest-substring fallback, tie handling, concurrent writers, private file permissions, null/empty/oversized inputs, default path, missing `HOME`, and overlong paths |
| `H031`–`H111` | History-search reference matrix | 81 generated four-character queries compared with a brute-force longest-common-substring implementation |
| `C001`–`C039` | Filename completion | Common prefix, unique match, directories, relative/absolute/nested paths, hidden files, quotes, escapes, operators, symlinks, newline/CR filenames, cursor placement, suffix preservation, invalid inputs, selection bounds, and ordering |
| `C040`–`C130` | Completion prefix matrix | Every prefix of regular, hidden, spaced, quoted, operator, and expansion-character filenames is checked for matching and decoding correctness |
| `S001`–`S006` | Scrollback | Joining output chunks, carriage-return/backspace/tab behavior, line and byte limits, clear, and scrolling back to live output |
| `K001`–`K002` | Hotkeys | Required defaults, custom bindings, conflicts, unknown actions, and invalid key names |
| `E001`–`E045` | Shell and executor | External commands, quoting and expansion, substitutions, wildcards, `less`, file and pipe input, binary and large output, redirection, five-stage and four-stage pipelines, invalid commands, background jobs, `source`, and exit behavior |
| `W002`–`W105` | Tab workers | Thread/process modes, command delivery, separate per-tab directories, shared history, and clear status |
| `J002`–`J106` | Signals and job control | Ctrl+C recovery, Ctrl+Z, `jobs`, `bg`, `fg`, and worker recovery in both modes |
| `M002`–`M104` | `multiWatch` | Concurrent output, separators, Ctrl+C cleanup, and worker recovery in both modes |
| `R001` | `multiWatch` regression | Compact comma-separated syntax from the issue report |
| `G001`–`G004`, `G103`–`G104` | GUI interaction | Application startup and shutdown, typing, unique completion, invalid choice and recovery, no-match completion, Ctrl+R exact/fallback/no-match searches, and Escape cancellation in thread and process modes |

The generated tests report each query or filename prefix as its own test row,
so an individual mismatch identifies the exact input that failed.

## Source module inventory

| Module | Responsibility | How it is exercised |
| --- | --- | --- |
| `src/main.c` | Initialize history, choose execution mode, start the GUI, and install shutdown handlers | GUI startup tests |
| `src/gui.c` | X11 window, prompt, input editing, tabs, hotkeys, output polling, and drawing | Synthetic X11 input tests under Xvfb |
| `src/tab.c` | Per-tab pipes and worker thread/process lifecycle | Application build and worker protocol integration cases |
| `src/parser.c` | Input completeness, tokenization, pipelines, quoting, and expansion | Parser unit and shell execution cases |
| `src/executor.c` | Redirection, process creation, pipelines, process groups, and jobs | Shell, worker, and job-control integration cases |
| `src/shell.c` | Worker loop, shell state, built-ins, `source`, and `less` | Shell integration cases, including multi-megabyte streaming |
| `src/multiwatch.c` | Concurrent watched commands, output forwarding, and restart timing | Thread/process integration and syntax regression |
| `src/history.c` | Persistent shared history, retention, locking, snapshots, and search | Unit, generated-oracle, concurrency, and worker-sharing cases |
| `src/completion.c` | Filename matching, prefix handling, quoting, insertion, and selection | Unit edge cases and generated prefix matrix |
| `src/scrollback.c` | Bounded output lines, control characters, clearing, and viewport movement | Scrollback unit cases |
| `src/hotkeys.c` | Default bindings, config parsing, conflict checks, and action lookup | Hotkey unit cases |
| `src/selection.c` | Mouse selection and X11 clipboard transfer | Not directly exercised by these automated tests |

## Limits and notes

The GUI tests verify that commands enter the history file as expected. They do
not inspect rendered pixels or verify the text visually displayed in the
window. Clipboard selection, mouse-based tab switching, and manual visual
layout are not automated.

Permission-denied cases are not asserted because the suite may run as root,
which can bypass ordinary file permissions. The history-file mode is checked
directly; missing-file, directory-read, invalid-home, and invalid-path errors
are also covered. Task 11's custom script interpreter is not implemented and
is therefore outside this suite.

Worker and GUI interactions use bounded polling loops. Direct shell
integration calls are covered by the Makefile's overall test-runner deadline;
if one hangs, the test target terminates it and returns failure rather than
waiting forever.
