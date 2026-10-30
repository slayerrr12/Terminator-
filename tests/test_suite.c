#include "hotkeys.h"
#include "scrollback.h"
#include "shell.h"
#include "terminator.h"
#include <X11/keysym.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int total_tests;
static int passed_tests;
static int failed_tests;
static int skipped_tests;
static char failed_test_ids[128][32];
static size_t failed_test_count;
static char temporary_root[] = "build/tests/tmp-XXXXXX";
static char *test_root;
static char shell_directory[TERM_PATH_BUFFER];

static void print_field(const char *text) {
  if (!text)
    return;
  for (; *text; text++) {
    if (*text == '\n')
      fputs("\\n", stdout);
    else if (*text == '\r')
      fputs("\\r", stdout);
    else if (*text == '\t')
      fputs("\\t", stdout);
    else if (*text == '|')
      fputs("\\|", stdout);
    else
      putchar(*text);
  }
}

static void report_test(const char *id, const char *feature,
                        const char *scenario, const char *input,
                        const char *expected, const char *actual, int passed,
                        const char *notes) {
  total_tests++;
  if (passed)
    passed_tests++;
  else {
    failed_tests++;
    if (failed_test_count < sizeof(failed_test_ids) / sizeof(failed_test_ids[0]))
      snprintf(failed_test_ids[failed_test_count++],
               sizeof(failed_test_ids[0]), "%s", id);
  }
  print_field(id);
  fputs(" | Feature: ", stdout);
  print_field(feature);
  fputs(" | Scenario: ", stdout);
  print_field(scenario);
  fputs(" | Input: ", stdout);
  print_field(input);
  fputs(" | Expected: ", stdout);
  print_field(expected);
  fputs(" | Actual: ", stdout);
  print_field(actual);
  printf(" | Status: %s | Notes: ", passed ? "PASS" : "FAIL");
  print_field(notes);
  putchar('\n');
  fflush(stdout);
}

static void report_skip(const char *id, const char *feature,
                        const char *scenario, const char *reason) {
  total_tests++;
  skipped_tests++;
  print_field(id);
  fputs(" | Feature: ", stdout);
  print_field(feature);
  fputs(" | Scenario: ", stdout);
  print_field(scenario);
  fputs(" | Input: n/a | Expected: run | Actual: skipped | Status: SKIPPED | Notes: ",
        stdout);
  print_field(reason);
  putchar('\n');
  fflush(stdout);
}

static void report_summary(void) {
  printf("\n========================================\n");
  printf("          TEST EXECUTION REPORT\n");
  printf("========================================\n");
  printf("Total Tests : %d\nPassed      : %d\nFailed      : %d\nSkipped     : %d\n",
         total_tests, passed_tests, failed_tests, skipped_tests);
  printf("Result: %s\n", failed_tests ? "FAILED" : "PASSED");
  if (failed_test_count) {
    printf("Failed Tests:\n");
    for (size_t i = 0; i < failed_test_count; i++)
      printf("%zu. %s\n", i + 1, failed_test_ids[i]);
  }
  printf("========================================\n");
}

static int join_test_path(char *destination, size_t capacity,
                          const char *directory, const char *name) {
  size_t directory_length = strlen(directory);
  size_t name_length = strlen(name);
  if (directory_length + name_length + 2 > capacity)
    return -1;
  memcpy(destination, directory, directory_length);
  destination[directory_length] = '/';
  memcpy(destination + directory_length + 1, name, name_length + 1);
  return 0;
}

static int write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (!file)
    return -1;
  size_t length = strlen(text);
  int result = fwrite(text, 1, length, file) == length ? 0 : -1;
  if (fclose(file) != 0)
    result = -1;
  return result;
}

static char *read_text_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *text = malloc((size_t)length + 1);
  if (!text) {
    fclose(file);
    return NULL;
  }
  size_t count = fread(text, 1, (size_t)length, file);
  text[count] = '\0';
  fclose(file);
  return text;
}

static int remove_tree(const char *path) {
  struct stat info;
  if (lstat(path, &info) < 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(info.st_mode))
    return unlink(path);

  DIR *directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent *entry;
  int result = 0;
  while ((entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char child[TERM_PATH_BUFFER];
    int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(child) || remove_tree(child) < 0)
      result = -1;
  }
  closedir(directory);
  if (rmdir(path) < 0)
    result = -1;
  return result;
}

static int set_shell_context(ShellContext *shell, SharedHistory *history,
                             const char *working_directory) {
  memset(shell, 0, sizeof(*shell));
  shell->history = history;
  if (snprintf(shell->working_directory, sizeof(shell->working_directory),
               "%s", working_directory) >=
      (int)sizeof(shell->working_directory))
    return -1;
  return 0;
}

static char *run_line(ShellContext *shell, const char *command, int *status) {
  FILE *capture = tmpfile();
  if (!capture)
    return NULL;
  *status = shell_run_line(shell, command, fileno(capture));
  fflush(capture);
  if (fseek(capture, 0, SEEK_END) != 0) {
    fclose(capture);
    return NULL;
  }
  long length = ftell(capture);
  if (length < 0 || fseek(capture, 0, SEEK_SET) != 0) {
    fclose(capture);
    return NULL;
  }
  char *output = malloc((size_t)length + 1);
  if (!output) {
    fclose(capture);
    return NULL;
  }
  size_t count = fread(output, 1, (size_t)length, capture);
  output[count] = '\0';
  fclose(capture);
  return output;
}

static int contains(const char *text, const char *part) {
  return text && strstr(text, part) != NULL;
}

static size_t count_occurrences(const char *text, const char *part) {
  size_t count = 0;
  size_t part_length = strlen(part);
  if (!part_length)
    return 0;
  while ((text = strstr(text, part))) {
    count++;
    text += part_length;
  }
  return count;
}

static void test_parser(void) {
  report_test("P001", "parser", "empty input is complete", "empty string",
              "complete", shell_is_complete("") ? "complete" : "incomplete",
              shell_is_complete("") == 1, "");
  report_test("P002", "parser", "open quote requests another line",
              "echo \"unfinished", "incomplete",
              shell_is_complete("echo \"unfinished") ? "complete"
                                                       : "incomplete",
              shell_is_complete("echo \"unfinished") == 0, "");
  const char *nested = "echo \"$(printf '%s' $(printf nested))\"";
  report_test("P003", "parser", "nested substitution is complete", nested,
              "complete", shell_is_complete(nested) ? "complete" : "incomplete",
              shell_is_complete(nested) == 1, "");
  const char *multiline = "echo $(\n  printf value\n)";
  report_test("P004", "parser", "multiline substitution is complete",
              "echo $(<newline> printf value<newline>)", "complete",
              shell_is_complete(multiline) ? "complete" : "incomplete",
              shell_is_complete(multiline) == 1, "");
  report_test("P005", "parser", "single-quoted trailing slash is literal",
              "'literal\\", "does not join next line",
              shell_trailing_backslash_continues("'literal\\") ? "joins"
                                                                  : "literal",
              shell_trailing_backslash_continues("'literal\\") == 0, "");
  report_test("P006", "parser", "odd trailing slash joins next line",
              "echo value\\", "joins next line",
              shell_trailing_backslash_continues("echo value\\") ? "joins"
                                                                   : "does not join",
              shell_trailing_backslash_continues("echo value\\") == 1, "");

  Tokens tokens = {0};
  char *error = NULL;
  int result = shell_lex("echo 'two words' | sort", &tokens, &error);
  int lex_ok = result == 0 && tokens.count == 4 &&
               !strcmp(tokens.items[1], "'two words'") &&
               !strcmp(tokens.items[2], "|");
  report_test("P007", "parser", "quoted word and operator tokenization",
              "echo 'two words' | sort", "4 tokens; quote stays within word",
              lex_ok ? "4 expected tokens" : "different token sequence", lex_ok,
              error ? error : "");
  free(error);
  shell_tokens_free(&tokens);

  Pipeline pipeline = {0};
  error = NULL;
  result = shell_parse("cat < input | sort >> output &", &tokens, &pipeline,
                       &error);
  int parse_ok = result == 0 && pipeline.count == 2 && pipeline.background &&
                 pipeline.commands[0].input_path &&
                 !strcmp(pipeline.commands[0].input_path, "input") &&
                 pipeline.commands[1].output_path &&
                 !strcmp(pipeline.commands[1].output_path, "output") &&
                 pipeline.commands[1].append;
  report_test("P008", "parser", "pipeline, redirection, append, background",
              "cat < input | sort >> output &", "2 stages; input/output; append; background",
              parse_ok ? "parsed as expected" : "unexpected parse result", parse_ok,
              error ? error : "");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);

  const char *invalid[] = {"| cat", "cat |", "cat || sort", "cat >", "cat & sort"};
  int invalid_ok = 1;
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    memset(&tokens, 0, sizeof(tokens));
    memset(&pipeline, 0, sizeof(pipeline));
    error = NULL;
    if (shell_parse(invalid[i], &tokens, &pipeline, &error) == 0)
      invalid_ok = 0;
    free(error);
    shell_tokens_free(&tokens);
    shell_pipeline_free(&pipeline);
  }
  report_test("P009", "parser", "rejects malformed pipeline and redirection",
              "| cat; cat |; cat || sort; cat >; cat & sort",
              "all five rejected", invalid_ok ? "all rejected" : "one accepted",
              invalid_ok, "");

  char many_arguments[TERM_LINE_MAX];
  strcpy(many_arguments, "echo");
  for (int i = 0; i < 400; i++)
    strcat(many_arguments, " x");
  memset(&tokens, 0, sizeof(tokens));
  memset(&pipeline, 0, sizeof(pipeline));
  error = NULL;
  result = shell_parse(many_arguments, &tokens, &pipeline, &error);
  int many_ok = result == 0 && pipeline.count == 1 &&
                pipeline.commands[0].argument_count == 401;
  report_test("P010", "parser", "many arguments within line limit",
              "echo followed by 400 arguments", "401 parsed arguments",
              many_ok ? "401 parsed arguments" : "parse failed or count differed",
              many_ok, error ? error : "");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);

  memset(&tokens, 0, sizeof(tokens));
  memset(&pipeline, 0, sizeof(pipeline));
  error = NULL;
  result = shell_parse("printf text > first.txt > second.txt", &tokens,
                       &pipeline, &error);
  int repeated_redirection_ok =
      result == 0 && pipeline.count == 1 &&
      pipeline.commands[0].output_path &&
      !strcmp(pipeline.commands[0].output_path, "second.txt");
  report_test("P012", "parser", "last repeated output redirection is retained",
              "printf text > first.txt > second.txt", "output path is second.txt",
              repeated_redirection_ok ? "second.txt" : "different parse result",
              repeated_redirection_ok, error ? error : "");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);

  char long_line[TERM_LINE_MAX];
  strcpy(long_line, "echo ");
  memset(long_line + 5, 'x', sizeof(long_line) - 7);
  long_line[sizeof(long_line) - 2] = '\0';
  memset(&tokens, 0, sizeof(tokens));
  memset(&pipeline, 0, sizeof(pipeline));
  error = NULL;
  result = shell_parse(long_line, &tokens, &pipeline, &error);
  int long_line_ok = result == 0 && pipeline.count == 1 &&
                     pipeline.commands[0].argument_count == 2 &&
                     strlen(pipeline.commands[0].arguments[1]) ==
                         sizeof(long_line) - 7;
  report_test("P013", "parser", "near-maximum command line is accepted",
              "echo plus a 8 KiB argument", "one command with two arguments",
              long_line_ok ? "long argument parsed" : "parse failed or truncated",
              long_line_ok, error ? error : "");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);

  pid_t parser_child = fork();
  if (parser_child == 0) {
    alarm(1);
    memset(&tokens, 0, sizeof(tokens));
    _exit(shell_lex("echo\rvalue", &tokens, NULL) == 0 ? 0 : 1);
  }
  int child_status = 0;
  int finished = 0;
  for (int i = 0; i < 20; i++) {
    pid_t waited = waitpid(parser_child, &child_status, WNOHANG);
    if (waited == parser_child) {
      finished = 1;
      break;
    }
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
    nanosleep(&pause, NULL);
  }
  if (!finished) {
    kill(parser_child, SIGKILL);
    waitpid(parser_child, &child_status, 0);
  }
  int carriage_return_ok = finished && WIFEXITED(child_status) &&
                           WEXITSTATUS(child_status) == 0;
  report_test("P011", "parser", "carriage return whitespace returns",
              "echo<CR>value", "lexer returns without hanging",
              finished ? (carriage_return_ok ? "returned" : "returned error")
                       : "hung until test timeout",
              carriage_return_ok,
              "Runs in a disposable child so a parser loop cannot hang the suite.");
}

static void test_history(void) {
  SharedHistory *history = calloc(1, sizeof(*history));
  if (!history || history_init(history, 0) < 0) {
    report_test("H001", "history", "initialize shared ring", "history_init",
                "initialized", "allocation or semaphore initialization failed",
                0, "");
    free(history);
    return;
  }
  char (*snapshot)[TERM_LINE_MAX] = calloc(TERM_HISTORY_CAP, sizeof(*snapshot));
  char value[64];
  for (int i = 0; i < TERM_HISTORY_CAP + 6; i++) {
    snprintf(value, sizeof(value), "cmd%d", i);
    history_add(history, value);
  }
  size_t count = history_snapshot(history, snapshot, TERM_HISTORY_CAP);
  int ring_ok = count == TERM_HISTORY_CAP && !strcmp(snapshot[0], "cmd6") &&
                !strcmp(snapshot[count - 1], "cmd1029");
  report_test("H002", "history", "ring wraps at configured capacity",
              "add 1,030 commands", "keeps cmd6 through cmd1029",
              ring_ok ? "kept newest 1,024 in order" : "ring contents differ",
              ring_ok, "");

  char recent[64] = {0};
  int recent_ok = history_get_recent(history, 0, recent, sizeof(recent)) &&
                  !strcmp(recent, "cmd1029");
  report_test("H003", "history", "retrieve most recent entry", "offset 0",
              "cmd1029", recent, recent_ok, "");

  count = history_snapshot(history, snapshot, 5);
  int limited_ok = count == 5 && !strcmp(snapshot[0], "cmd1025") &&
                   !strcmp(snapshot[4], "cmd1029");
  report_test("H004", "history", "snapshot respects requested maximum",
              "snapshot max_entries=5", "latest five entries",
              limited_ok ? "cmd1025 through cmd1029" : "different snapshot",
              limited_ok, "");

  history_add(history, "");
  count = history_snapshot(history, snapshot, TERM_HISTORY_CAP);
  int empty_ok = count == TERM_HISTORY_CAP && !strcmp(snapshot[count - 1], "cmd1029");
  report_test("H005", "history", "empty command is not recorded", "add empty line",
              "history unchanged", empty_ok ? "unchanged" : "changed", empty_ok,
              "");
  int missing_ok = !history_get_recent(history, TERM_HISTORY_CAP, recent,
                                       sizeof(recent));
  report_test("H006", "history", "out-of-range history offset is rejected",
              "request offset 1,024", "returns no entry",
              missing_ok ? "no entry" : recent, missing_ok, "");
  sem_destroy(&history->semaphore);
  free(snapshot);
  free(history);
}

static void test_scrollback(void) {
  Scrollback scrollback;
  scrollback_init(&scrollback);
  scrollback_append_text(&scrollback, "hel", 3);
  scrollback_append_text(&scrollback, "lo\nwor", 6);
  scrollback_append_text(&scrollback, "ld", 2);
  int fragmented_ok = scrollback.line_count == 2 &&
                      !strcmp(scrollback_get_line(&scrollback, 0), "hello") &&
                      !strcmp(scrollback_get_line(&scrollback, 1), "world");
  report_test("S001", "scrollback", "output chunks join across reads",
              "hel + lo<newline>wor + ld", "hello and world on separate lines",
              fragmented_ok ? "hello / world" : "different lines", fragmented_ok,
              "");
  scrollback_clear(&scrollback);

  scrollback_append_text(&scrollback, "abc\rX\b!\tZ", 9);
  int controls_ok = scrollback.line_count == 1 &&
                    !strcmp(scrollback_get_line(&scrollback, 0), "!       Z");
  report_test("S002", "scrollback", "carriage return, backspace, and tab",
              "abc<CR>X<BS>!<TAB>Z", "! plus tab spaces plus Z",
              controls_ok ? "control characters applied" : "unexpected line text",
              controls_ok, "");
  scrollback_destroy(&scrollback);

  scrollback_init(&scrollback);
  for (int i = 0; i < TERM_SCROLLBACK_MAX_LINES + 1; i++) {
    char line[32];
    int length = snprintf(line, sizeof(line), "line%d\n", i);
    if (scrollback_append_text(&scrollback, line, (size_t)length) < 0)
      break;
  }
  int line_limit_ok = scrollback.line_count == TERM_SCROLLBACK_MAX_LINES &&
                      !strcmp(scrollback_get_line(&scrollback, 0), "line2");
  char actual[128];
  snprintf(actual, sizeof(actual), "count=%zu, first=%s",
           scrollback.line_count, scrollback_get_line(&scrollback, 0));
  report_test("S003", "scrollback", "oldest lines trimmed at line limit",
              "append 10,001 lines", "10,000 rows starting at line2",
              actual,
              line_limit_ok, "");
  scrollback_destroy(&scrollback);

  scrollback_init(&scrollback);
  char *large_line = malloc(TERM_SCROLLBACK_MAX_BYTES + 100);
  memset(large_line, 'x', TERM_SCROLLBACK_MAX_BYTES + 99);
  large_line[TERM_SCROLLBACK_MAX_BYTES + 99] = '\0';
  int append_status = scrollback_append_text(
      &scrollback, large_line, TERM_SCROLLBACK_MAX_BYTES + 99);
  int byte_limit_ok = append_status == 0 && scrollback.line_count == 1 &&
                      scrollback.stored_bytes <= TERM_SCROLLBACK_MAX_BYTES &&
                      scrollback.lines[0].length == TERM_SCROLLBACK_MAX_BYTES - 1;
  report_test("S004", "scrollback", "single output line respects byte budget",
              "append 1 MiB plus 99 bytes", "stored line capped at 1 MiB budget",
              byte_limit_ok ? "line truncated within byte budget"
                            : "byte budget not respected",
              byte_limit_ok, "");
  free(large_line);
  scrollback_clear(&scrollback);
  int clear_ok = scrollback.line_count == 0 && scrollback.stored_bytes == 0;
  report_test("S005", "scrollback", "clear frees buffered output", "clear buffer",
              "zero lines and zero stored bytes",
              clear_ok ? "empty" : "buffer still contains data", clear_ok, "");
  scrollback_destroy(&scrollback);

  scrollback_init(&scrollback);
  for (int i = 0; i < 10; i++) {
    char line[16];
    int length = snprintf(line, sizeof(line), "%d\n", i);
    scrollback_append_text(&scrollback, line, (size_t)length);
  }
  scrollback_scroll_up(&scrollback, 3, 2);
  int up_ok = scrollback_first_visible(&scrollback, 3) == 6 &&
              !scrollback.follows_output;
  scrollback_scroll_down(&scrollback, 3, 20);
  int down_ok = scrollback_first_visible(&scrollback, 3) == 8 &&
                scrollback.follows_output;
  report_test("S006", "scrollback", "scroll up and return to live output",
              "10 lines; viewport 3; up 2; down 20",
              "first line 6, then follow output at line 8",
              up_ok && down_ok ? "scroll positions match" : "position differed",
              up_ok && down_ok, "");
  scrollback_destroy(&scrollback);
}

static void test_hotkeys(void) {
  HotkeyConfig config;
  hotkeys_set_defaults(&config);
  int defaults_ok =
      hotkeys_find_action(&config, XK_Left, 0) == HOTKEY_MOVE_LEFT &&
      hotkeys_find_action(&config, XK_Right, 0) == HOTKEY_MOVE_RIGHT &&
      hotkeys_find_action(&config, XK_a, ControlMask) == HOTKEY_LINE_START &&
      hotkeys_find_action(&config, XK_e, ControlMask) == HOTKEY_LINE_END &&
      hotkeys_find_action(&config, XK_Up, 0) == HOTKEY_HISTORY_UP &&
      hotkeys_find_action(&config, XK_Down, 0) == HOTKEY_HISTORY_DOWN &&
      hotkeys_find_action(&config, XK_u, ControlMask) == HOTKEY_DELETE_TO_START &&
      hotkeys_find_action(&config, XK_k, ControlMask) == HOTKEY_DELETE_TO_END &&
      hotkeys_find_action(&config, XK_w, ControlMask) ==
          HOTKEY_DELETE_PREVIOUS_WORD &&
      hotkeys_find_action(&config, XK_l, ControlMask) == HOTKEY_CLEAR_SCREEN &&
      hotkeys_find_action(&config, XK_d, ControlMask) == HOTKEY_CLOSE_TAB &&
      hotkeys_find_action(&config, XK_c, ControlMask | ShiftMask) == HOTKEY_COPY &&
      hotkeys_find_action(&config, XK_v, ControlMask | ShiftMask) == HOTKEY_PASTE &&
      hotkeys_find_action(&config, XK_c, ControlMask) == HOTKEY_INTERRUPT &&
      hotkeys_find_action(&config, XK_z, ControlMask) == HOTKEY_SUSPEND &&
      hotkeys_find_action(&config, XK_t, ControlMask) == HOTKEY_NEW_TAB;
  report_test("K001", "hotkeys", "assignment defaults are present",
              "all 16 default actions", "every key maps to its required action",
              defaults_ok ? "defaults match" : "one or more defaults differ",
              defaults_ok, "");

  char config_path[TERM_PATH_BUFFER];
  char config_home[TERM_PATH_BUFFER];
  join_test_path(config_home, sizeof(config_home), test_root, "hotkeys");
  mkdir(config_home, 0700);
  join_test_path(config_path, sizeof(config_path), config_home,
                 ".terminatorrc");
  const char *old_home = getenv("HOME");
  char *saved_home = old_home ? strdup(old_home) : NULL;
  setenv("HOME", config_home, 1);
  write_text_file(config_path,
                  "# custom key\nmove_left = Alt+Left\nline_start = Ctrl+e\n"
                  "unknown = F12\nline_end = NotARealKey\n");
  char warnings[1024];
  int load_result = hotkeys_load_user_config(&config, warnings, sizeof(warnings));
  int config_ok = load_result > 0 &&
                  hotkeys_find_action(&config, XK_Left, Mod1Mask) ==
                      HOTKEY_MOVE_LEFT &&
                  hotkeys_find_action(&config, XK_Left, 0) == HOTKEY_NONE &&
                  hotkeys_find_action(&config, XK_e, ControlMask) ==
                      HOTKEY_LINE_END && contains(warnings, "unknown action") &&
                  contains(warnings, "invalid key binding");
  report_test("K002", "hotkeys", "custom key plus invalid entries and collision",
              "~/.terminatorrc with Alt+Left and conflicting Ctrl+E",
              "custom applies; conflict/invalid entries warn and default",
              config_ok ? "custom and fallback behavior observed" : warnings,
              config_ok, warnings);
  if (saved_home) {
    setenv("HOME", saved_home, 1);
    free(saved_home);
  } else {
    unsetenv("HOME");
  }
}

typedef struct {
  int command_write;
  int output_read;
  int status_read;
  int worker_input;
  int worker_output;
  int worker_status;
  int use_thread;
  int started;
  pid_t pid;
  pthread_t thread;
  SharedHistory *history;
  SharedForeground *foreground;
  char working_directory[TERM_PATH_BUFFER];
} TestWorker;

static void *test_worker_thread(void *argument) {
  TestWorker *worker = argument;
  worker_run(worker->worker_input, worker->worker_output, worker->worker_status,
             worker->history, worker->foreground, worker->working_directory);
  return NULL;
}

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void worker_init(TestWorker *worker) {
  memset(worker, 0, sizeof(*worker));
  worker->command_write = -1;
  worker->output_read = -1;
  worker->status_read = -1;
  worker->worker_input = -1;
  worker->worker_output = -1;
  worker->worker_status = -1;
}

static void close_pipe_if_open(int pipe_ends[2]) {
  for (int end = 0; end < 2; end++) {
    if (pipe_ends[end] >= 0) {
      close(pipe_ends[end]);
      pipe_ends[end] = -1;
    }
  }
}

static int worker_start(TestWorker *worker, int use_thread,
                        SharedHistory *history, SharedForeground *foreground,
                        const char *working_directory, TestWorker *existing,
                        size_t existing_count) {
  worker_init(worker);
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  int status_pipe[2] = {-1, -1};
  if (pipe(input_pipe) < 0 || pipe(output_pipe) < 0 || pipe(status_pipe) < 0) {
    close_pipe_if_open(input_pipe);
    close_pipe_if_open(output_pipe);
    close_pipe_if_open(status_pipe);
    return -1;
  }
  worker->command_write = input_pipe[1];
  worker->output_read = output_pipe[0];
  worker->status_read = status_pipe[0];
  worker->worker_input = input_pipe[0];
  worker->worker_output = output_pipe[1];
  worker->worker_status = status_pipe[1];
  worker->history = history;
  worker->foreground = foreground;
  worker->use_thread = use_thread;
  snprintf(worker->working_directory, sizeof(worker->working_directory), "%s",
           working_directory);
  if (use_thread) {
    if (pthread_create(&worker->thread, NULL, test_worker_thread, worker) != 0) {
      close_pipe_if_open(input_pipe);
      close_pipe_if_open(output_pipe);
      close_pipe_if_open(status_pipe);
      worker->command_write = worker->output_read = worker->status_read = -1;
      return -1;
    }
    worker->started = 1;
    /* The worker thread owns these descriptors until worker_run returns. */
  } else {
    pid_t pid = fork();
    if (pid == 0) {
      close(input_pipe[1]);
      close(output_pipe[0]);
      close(status_pipe[0]);
      for (size_t i = 0; i < existing_count; i++) {
        if (existing[i].command_write >= 0)
          close(existing[i].command_write);
        if (existing[i].output_read >= 0)
          close(existing[i].output_read);
        if (existing[i].status_read >= 0)
          close(existing[i].status_read);
      }
      worker_run(input_pipe[0], output_pipe[1], status_pipe[1], history,
                 foreground, working_directory);
      _exit(0);
    }
    if (pid < 0) {
      close_pipe_if_open(input_pipe);
      close_pipe_if_open(output_pipe);
      close_pipe_if_open(status_pipe);
      worker->command_write = worker->output_read = worker->status_read = -1;
      return -1;
    }
    worker->pid = pid;
    worker->started = 1;
    close(input_pipe[0]);
    close(output_pipe[1]);
    close(status_pipe[1]);
    worker->worker_input = worker->worker_output = worker->worker_status = -1;
  }
  if (set_nonblocking(worker->output_read) < 0 ||
      set_nonblocking(worker->status_read) < 0)
    return -1;
  return 0;
}

static int write_all(int fd, const char *text, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t result = write(fd, text + sent, length - sent);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    sent += (size_t)result;
  }
  return 0;
}

static void append_capture(char *capture, size_t capacity, size_t *length,
                           const char *text, size_t count) {
  if (*length >= capacity - 1)
    return;
  if (count > capacity - 1 - *length)
    count = capacity - 1 - *length;
  memcpy(capture + *length, text, count);
  *length += count;
  capture[*length] = '\0';
}

static void drain_output(TestWorker *worker, char *capture, size_t capacity,
                         size_t *length) {
  char buffer[2048];
  for (;;) {
    ssize_t count = read(worker->output_read, buffer, sizeof(buffer));
    if (count > 0) {
      append_capture(capture, capacity, length, buffer, (size_t)count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
}

static int worker_send(TestWorker *worker, const char *command) {
  size_t length = strlen(command);
  int has_newline = length && command[length - 1] == '\n';
  if (write_all(worker->command_write, command, length) < 0)
    return -1;
  return has_newline ? 0 : write_all(worker->command_write, "\n", 1);
}

static int worker_collect(TestWorker *worker, char *capture, size_t capacity,
                          WorkerStatus *message, int timeout_ms) {
  size_t capture_length = 0;
  size_t status_length = 0;
  char status_bytes[sizeof(*message)];
  capture[0] = '\0';
  int elapsed = 0;
  while (status_length < sizeof(status_bytes) && elapsed < timeout_ms) {
    struct pollfd descriptors[2] = {
        {worker->output_read, POLLIN | POLLHUP, 0},
        {worker->status_read, POLLIN | POLLHUP, 0},
    };
    int ready = poll(descriptors, 2, 100);
    elapsed += 100;
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return -1;
    if (descriptors[0].revents & (POLLIN | POLLHUP))
      drain_output(worker, capture, capacity, &capture_length);
    if (descriptors[1].revents & (POLLIN | POLLHUP)) {
      ssize_t count = read(worker->status_read, status_bytes + status_length,
                           sizeof(status_bytes) - status_length);
      if (count > 0)
        status_length += (size_t)count;
      else if (count < 0 && errno != EAGAIN && errno != EINTR)
        return -1;
    }
  }
  if (status_length != sizeof(status_bytes))
    return -1;
  memcpy(message, status_bytes, sizeof(*message));
  drain_output(worker, capture, capacity, &capture_length);
  return 0;
}

static int worker_command(TestWorker *worker, const char *command,
                          char *capture, size_t capacity,
                          WorkerStatus *message) {
  if (worker_send(worker, command) < 0)
    return -1;
  return worker_collect(worker, capture, capacity, message, 5000);
}

static int worker_wait_output(TestWorker *worker, const char *first,
                              const char *second, char *capture,
                              size_t capacity, size_t minimum_occurrences,
                              int timeout_ms) {
  size_t length = 0;
  capture[0] = '\0';
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    if (count_occurrences(capture, first) >= minimum_occurrences &&
        (!second || count_occurrences(capture, second) >= minimum_occurrences))
      return 0;
    struct pollfd descriptor = {worker->output_read, POLLIN | POLLHUP, 0};
    int ready = poll(&descriptor, 1, 100);
    elapsed += 100;
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return -1;
    if (ready && descriptor.revents)
      drain_output(worker, capture, capacity, &length);
  }
  return count_occurrences(capture, first) >= minimum_occurrences &&
                 (!second ||
                  count_occurrences(capture, second) >= minimum_occurrences)
             ? 0
             : -1;
}

static void worker_stop(TestWorker *worker) {
  if (worker->command_write >= 0) {
    close(worker->command_write);
    worker->command_write = -1;
  }
  if (!worker->started) {
    if (worker->output_read >= 0)
      close(worker->output_read);
    if (worker->status_read >= 0)
      close(worker->status_read);
    worker->output_read = worker->status_read = -1;
    return;
  }
  if (worker->use_thread) {
    pthread_join(worker->thread, NULL);
  } else if (worker->pid > 0) {
    int status;
    for (int i = 0; i < 50; i++) {
      pid_t result = waitpid(worker->pid, &status, WNOHANG);
      if (result == worker->pid) {
        worker->pid = 0;
        break;
      }
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
      nanosleep(&pause, NULL);
    }
    if (worker->pid > 0) {
      kill(worker->pid, SIGKILL);
      waitpid(worker->pid, NULL, 0);
      worker->pid = 0;
    }
  }
  if (worker->output_read >= 0)
    close(worker->output_read);
  if (worker->status_read >= 0)
    close(worker->status_read);
  worker->output_read = worker->status_read = -1;
}

static SharedHistory *new_worker_history(int use_thread) {
  int flags = MAP_SHARED | MAP_ANONYMOUS;
  SharedHistory *history = mmap(NULL, sizeof(*history), PROT_READ | PROT_WRITE,
                                flags, -1, 0);
  if (history == MAP_FAILED)
    return NULL;
  if (history_init(history, use_thread ? 0 : 1) < 0) {
    munmap(history, sizeof(*history));
    return NULL;
  }
  return history;
}

static SharedForeground *new_foreground(void) {
  SharedForeground *foreground = mmap(NULL, sizeof(*foreground),
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (foreground == MAP_FAILED)
    return NULL;
  atomic_init(&foreground->pgid, 0);
  return foreground;
}

static int wait_for_foreground(SharedForeground *foreground, int timeout_ms) {
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
    int pgid = atomic_load(&foreground->pgid);
    if (pgid > 0)
      return pgid;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
    nanosleep(&pause, NULL);
  }
  return 0;
}

static void test_worker_execution_mode(int use_thread) {
  const char *mode = use_thread ? "thread" : "process";
  SharedHistory *history = new_worker_history(use_thread);
  SharedForeground *foreground0 = new_foreground();
  SharedForeground *foreground1 = new_foreground();
  char subdirectory[TERM_PATH_BUFFER];
  snprintf(subdirectory, sizeof(subdirectory), "%s/worker-dir", test_root);
  mkdir(subdirectory, 0700);
  TestWorker workers[2];
  worker_init(&workers[0]);
  worker_init(&workers[1]);
  char output[8192];
  WorkerStatus status;
  int started = history && foreground0 && foreground1 &&
                worker_start(&workers[0], use_thread, history, foreground0,
                             shell_directory, workers, 0) == 0 &&
                worker_start(&workers[1], use_thread, history, foreground1,
                             shell_directory, workers, 1) == 0;
  if (!started) {
    report_test(use_thread ? "W001" : "W101", "worker", "start two tab workers",
                mode, "both workers start", "worker setup failed", 0, "");
    worker_stop(&workers[0]);
    worker_stop(&workers[1]);
    if (history)
      munmap(history, sizeof(*history));
    if (foreground0)
      munmap(foreground0, sizeof(*foreground0));
    if (foreground1)
      munmap(foreground1, sizeof(*foreground1));
    return;
  }

  int result = worker_command(&workers[0], "/usr/bin/printf worker-ok",
                              output, sizeof(output), &status);
  int command_ok = result == 0 && status.status == 0 && contains(output, "worker-ok");
  report_test(use_thread ? "W002" : "W102", "worker",
              "command pipe reaches the matching worker", mode,
              "worker output contains worker-ok and status is 0",
              command_ok ? "worker-ok; status 0" : output, command_ok, "");

  char change_directory[TERM_LINE_MAX];
  snprintf(change_directory, sizeof(change_directory), "cd %s", subdirectory);
  result = worker_command(&workers[0], change_directory, output, sizeof(output),
                          &status);
  char pwd[TERM_LINE_MAX];
  int pwd_result = worker_command(&workers[0], "/bin/pwd", output, sizeof(output),
                                  &status);
  int tab0_directory_ok = pwd_result == 0 && contains(output, subdirectory);
  result |= pwd_result;
  int tab1_pwd_result = worker_command(&workers[1], "/bin/pwd", pwd, sizeof(pwd),
                                       &status);
  int isolation_ok = tab1_pwd_result == 0 && contains(pwd, shell_directory) &&
                     !contains(pwd, subdirectory);
  report_test(use_thread ? "W003" : "W103", "execution modes",
              "working directory remains separate per tab", mode,
              "tab 0 changes directory; tab 1 stays in starting directory",
              tab0_directory_ok && isolation_ok ? "directories are isolated"
                                                : "directory state leaked or failed",
              tab0_directory_ok && isolation_ok, "");

  worker_command(&workers[0], "/usr/bin/printf shared-history-entry", output,
                 sizeof(output), &status);
  int history_result = worker_command(&workers[1], "history", output,
                                      sizeof(output), &status);
  int history_ok = history_result == 0 && contains(output, "shared-history-entry");
  report_test(use_thread ? "W004" : "W104", "shared history",
              "command history is visible across tabs", mode,
              "tab 1 history includes command from tab 0",
              history_ok ? "shared-history-entry found" : output, history_ok,
              "");

  result = worker_command(&workers[0], "clear", output, sizeof(output), &status);
  int clear_ok = result == 0 && status.clear_screen;
  report_test(use_thread ? "W005" : "W105", "built-ins",
              "clear signals GUI to clear this tab", mode,
              "status message has clear_screen set",
              clear_ok ? "clear_screen=1" : "clear_screen=0", clear_ok, "");

  worker_stop(&workers[0]);
  worker_stop(&workers[1]);
  sem_destroy(&history->semaphore);
  munmap(history, sizeof(*history));
  munmap(foreground0, sizeof(*foreground0));
  munmap(foreground1, sizeof(*foreground1));
}

static void test_shell_features(void) {
  SharedHistory *history = calloc(1, sizeof(*history));
  if (!history || history_init(history, 0) < 0) {
    report_test("E001", "shell", "initialize execution context", "history",
                "ready", "initialization failed", 0, "");
    free(history);
    return;
  }
  ShellContext shell;
  set_shell_context(&shell, history, shell_directory);
  int status = 0;
  char *output = run_line(&shell, "/usr/bin/printf '%s\\n' 'hello world'", &status);
  int external_ok = status == 0 && output && !strcmp(output, "hello world\n");
  report_test("E001", "external commands", "exec external program with quoted arg",
              "/usr/bin/printf '%s\\n' 'hello world'", "prints one argument",
              output ? output : "no output", external_ok, "");
  free(output);

  setenv("TERMINATOR_TEST_VALUE", "value with spaces", 1);
  output = run_line(&shell,
                    "/usr/bin/printf '%s\\n' \"$TERMINATOR_TEST_VALUE\"", &status);
  int variable_ok = status == 0 && output && !strcmp(output, "value with spaces\n");
  report_test("E002", "expansion", "double quotes expand environment variable",
              "$TERMINATOR_TEST_VALUE", "value with spaces stays one argument",
              output ? output : "no output", variable_ok, "");
  free(output);
  output = run_line(&shell,
                    "/usr/bin/printf '%s\\n' '$TERMINATOR_TEST_VALUE'", &status);
  int literal_ok = status == 0 && output &&
                   !strcmp(output, "$TERMINATOR_TEST_VALUE\n");
  report_test("E003", "expansion", "single quotes keep variable literal",
              "'$TERMINATOR_TEST_VALUE'", "prints literal variable name",
              output ? output : "no output", literal_ok, "");
  free(output);

  output = run_line(&shell,
                    "/usr/bin/printf '%s\\n' \"$(/usr/bin/printf nested)\"", &status);
  int substitution_ok = status == 0 && output && !strcmp(output, "nested\n");
  report_test("E004", "command substitution", "capture nested command output",
              "$(/usr/bin/printf nested)", "prints nested", output ? output : "no output",
              substitution_ok, "");
  free(output);

  output = run_line(
      &shell,
      "/usr/bin/printf '%s\\n' \"$(/usr/bin/printf outer$(/usr/bin/printf inner))\"",
      &status);
  int nested_substitution_ok = status == 0 && output &&
                               !strcmp(output, "outerinner\n");
  report_test("E025", "nested command substitution",
              "evaluate the inner substitution before the outer one",
              "$(printf outer$(printf inner))", "prints outerinner",
              output ? output : "no output", nested_substitution_ok, "");
  free(output);

  output = run_line(&shell,
                    "/usr/bin/printf '%s\\n' \"$(/usr/bin/printf\n nested)\"",
                    &status);
  int multiline_substitution_ok = status == 0 && output &&
                                  !strcmp(output, "nested\n");
  report_test("E026", "multiline command substitution",
              "allow a line break inside $(...) input",
              "$(printf<newline> nested)", "prints nested",
              output ? output : "no output", multiline_substitution_ok, "");
  free(output);

  output = run_line(&shell, "/usr/bin/false", &status);
  free(output);
  output = run_line(&shell, "/usr/bin/printf '%s\\n' \"$?\"", &status);
  int status_expansion_ok = output && !strcmp(output, "1\n");
  report_test("E005", "exit status", "dollar-question keeps previous status",
              "false then printf '$?'", "prints 1", output ? output : "no output",
              status_expansion_ok, "");
  free(output);

  char first[256], second[256], redir[TERM_LINE_MAX];
  join_test_path(first, sizeof(first), shell_directory, "match-a.txt");
  join_test_path(second, sizeof(second), shell_directory, "match-b.txt");
  write_text_file(first, "a\n");
  write_text_file(second, "b\n");
  output = run_line(&shell, "/usr/bin/printf '%s\\n' match-*.txt", &status);
  int glob_ok = status == 0 && output && contains(output, "match-a.txt") &&
                contains(output, "match-b.txt");
  report_test("E006", "wildcard expansion", "glob expands in tab directory",
              "match-*.txt", "both matching file names", output ? output : "no output",
              glob_ok, "");
  free(output);

  char input_file[256], output_file[256];
  join_test_path(input_file, sizeof(input_file), shell_directory, "input.txt");
  join_test_path(output_file, sizeof(output_file), shell_directory,
                 "output.txt");
  write_text_file(input_file, "zeta\nalpha\n");
  snprintf(redir, sizeof(redir), "/bin/cat < %s | /usr/bin/sort > %s", input_file,
           output_file);
  output = run_line(&shell, redir, &status);
  free(output);
  char *contents = read_text_file(output_file);
  int redirect_pipe_ok = status == 0 && contents &&
                         !strcmp(contents, "alpha\nzeta\n");
  report_test("E007", "redirection and pipeline",
              "input redirection feeds pipe and final output is redirected",
              "cat < input.txt | sort > output.txt", "sorted file contents",
              contents ? contents : "output file missing", redirect_pipe_ok, "");
  free(contents);

  snprintf(redir, sizeof(redir), "/usr/bin/printf first > %s", output_file);
  output = run_line(&shell, redir, &status);
  free(output);
  snprintf(redir, sizeof(redir), "/usr/bin/printf second >> %s", output_file);
  output = run_line(&shell, redir, &status);
  free(output);
  contents = read_text_file(output_file);
  int append_ok = status == 0 && contents && !strcmp(contents, "firstsecond");
  report_test("E008", "append redirection", "append preserves existing contents",
              "> then >>", "firstsecond", contents ? contents : "file missing",
              append_ok, "");
  free(contents);

  char missing[TERM_PATH_BUFFER];
  join_test_path(missing, sizeof(missing), shell_directory, "no-such-input");
  snprintf(redir, sizeof(redir), "/bin/cat < %s", missing);
  output = run_line(&shell, redir, &status);
  int missing_ok = status != 0 && output && contains(output, missing);
  report_test("E009", "redirection error", "missing input file reports error",
              "cat < nonexistent", "nonzero status and path in diagnostic",
              output ? output : "no diagnostic", missing_ok, "");
  free(output);

  output = run_line(&shell, "/terminator-test-command-does-not-exist", &status);
  int unknown_ok = status == 127 && output && contains(output, "command not found");
  report_test("E010", "external command error", "unknown command leaves worker usable",
              "nonexistent absolute command", "status 127 and diagnostic",
              output ? output : "no diagnostic", unknown_ok, "");
  free(output);

  output = run_line(&shell,
                    "/usr/bin/printf A | /usr/bin/tr A B | /usr/bin/tr B C | "
                    "/usr/bin/tr C D | /usr/bin/tr D E",
                    &status);
  int long_pipeline_ok = status == 0 && output && !strcmp(output, "E");
  report_test("E011", "pipeline", "five commands execute in a pipeline",
              "printf A | tr A B | tr B C | tr C D | tr D E", "prints E",
              output ? output : "no output", long_pipeline_ok, "");
  free(output);

  char subdirectory[TERM_PATH_BUFFER];
  join_test_path(subdirectory, sizeof(subdirectory), shell_directory, "subdir");
  mkdir(subdirectory, 0700);
  snprintf(redir, sizeof(redir), "cd %s", subdirectory);
  output = run_line(&shell, redir, &status);
  int cd_ok = status == 0 && !strcmp(shell.working_directory, subdirectory);
  report_test("E012", "built-in cd", "cd changes this shell context", redir,
              "context working directory changes", cd_ok ? "directory changed"
                                                         : "directory unchanged",
              cd_ok, "");
  free(output);

  history_add(history, "history seed entry");
  output = run_line(&shell, "history", &status);
  int history_builtin_ok = status == 0 && output && contains(output, "history");
  report_test("E013", "built-in history", "history lists entered commands", "history",
              "history output includes command", output ? output : "no output",
              history_builtin_ok, "");
  free(output);

  output = run_line(&shell, "clear", &status);
  int clear_builtin_ok = status == 0 && shell.clear_requested;
  report_test("E014", "built-in clear", "clear requests GUI scrollback reset", "clear",
              "clear_requested is set", clear_builtin_ok ? "flag set" : "flag not set",
              clear_builtin_ok, "");
  free(output);

  char source_file[TERM_PATH_BUFFER];
  join_test_path(source_file, sizeof(source_file), shell_directory,
                 "source.commands");
  write_text_file(source_file,
                  "/usr/bin/printf 'sourced-one\\n'\n"
                  "/usr/bin/printf 'sourced-two\\n'\n");
  snprintf(redir, sizeof(redir), "source %s", source_file);
  output = run_line(&shell, redir, &status);
  int source_ok = status == 0 && output && contains(output, "sourced-one") &&
                  contains(output, "sourced-two");
  report_test("E015", "built-in source", "source runs multiple file commands",
              redir, "both command outputs are produced",
              output ? output : "no output", source_ok, "");
  free(output);

  const char *multiline =
      "/usr/bin/printf '%s\\n' \"first\nsecond\"";
  output = run_line(&shell, multiline, &status);
  int multiline_ok = status == 0 && output &&
                     !strcmp(output, "first\nsecond\n");
  report_test("E016", "multiline quoting", "newline inside a quoted argument",
              "printf with a two-line double-quoted argument",
              "prints both lines", output ? output : "no output", multiline_ok,
              "");
  free(output);

  output = run_line(&shell,
                    "/usr/bin/printf '%s\\n' `/usr/bin/printf backtick`",
                    &status);
  int backtick_ok = status == 0 && output && !strcmp(output, "backtick\n");
  report_test("E017", "command substitution", "expand backtick substitution",
              "`/usr/bin/printf backtick`", "prints backtick",
              output ? output : "no output", backtick_ok, "");
  free(output);

  char invalid_output[TERM_PATH_BUFFER];
  join_test_path(invalid_output, sizeof(invalid_output), test_root,
                 "missing-directory/output.txt");
  snprintf(redir, sizeof(redir), "/usr/bin/printf fail > %s", invalid_output);
  output = run_line(&shell, redir, &status);
  int bad_output_path_ok = status != 0 && output &&
                           contains(output, "No such file or directory");
  report_test("E018", "output redirection error",
              "nonexistent parent directory reports a failure", redir,
              "nonzero status and an error", output ? output : "no diagnostic",
              bad_output_path_ok, "Permission bits are not tested because the suite may run as root.");
  free(output);

  output = run_line(&shell, "/bin/sleep 0.2 &", &status);
  int background_started = status == 0 && output && contains(output, "[1]");
  free(output);
  output = run_line(&shell, "jobs", &status);
  int running_job = output && contains(output, "Running");
  free(output);
  struct timespec background_wait = {.tv_sec = 0, .tv_nsec = 400000000};
  nanosleep(&background_wait, NULL);
  output = run_line(&shell, "jobs", &status);
  int completed_job = output && contains(output, "Done");
  free(output);
  output = run_line(&shell, "jobs", &status);
  int completed_job_removed = status == 0 && (!output || !*output);
  report_test("E019", "background jobs", "start, list, and reap a command",
              "/bin/sleep 0.2 &; jobs; wait; jobs", "Running then Done, then removed",
              background_started && running_job && completed_job &&
                      completed_job_removed
                  ? "job lifecycle observed"
                  : "background job lifecycle differed",
              background_started && running_job && completed_job &&
                  completed_job_removed,
              "Uses a short bounded sleep so a failed child cannot hang the suite.");
  free(output);

  char first_redirect[256], second_redirect[256];
  join_test_path(first_redirect, sizeof(first_redirect), shell_directory,
                 "first-redirect.txt");
  join_test_path(second_redirect, sizeof(second_redirect), shell_directory,
                 "second-redirect.txt");
  snprintf(redir, sizeof(redir), "/usr/bin/printf data > %s > %s",
           first_redirect, second_redirect);
  output = run_line(&shell, redir, &status);
  free(output);
  char *first_contents = read_text_file(first_redirect);
  char *second_contents = read_text_file(second_redirect);
  int last_redirect_ok = status == 0 && !first_contents && second_contents &&
                         !strcmp(second_contents, "data");
  report_test("E021", "multiple redirections",
              "last output redirection receives command output", redir,
              "first file is untouched and second contains data",
              last_redirect_ok ? "only second file was written"
                               : "redirection result differed",
              last_redirect_ok, "");
  free(first_contents);
  free(second_contents);

  output = run_line(&shell,
                    "/usr/bin/printf input | /terminator-missing-stage | /usr/bin/tr a-z A-Z",
                    &status);
  int bad_pipeline_stage_ok = output &&
                              contains(output, "terminator-missing-stage") &&
                              contains(output, "command not found");
  report_test("E022", "pipeline error",
              "unknown command in middle pipeline stage reports an error",
              "printf input | missing-command | tr a-z A-Z",
              "diagnostic names the failed stage",
              output ? output : "no diagnostic", bad_pipeline_stage_ok, "");
  free(output);

  char copied_input[256], copied_output[256];
  join_test_path(copied_input, sizeof(copied_input), shell_directory,
                 "input.txt");
  join_test_path(copied_output, sizeof(copied_output), shell_directory,
                 "copy.txt");
  snprintf(redir, sizeof(redir), "/bin/cat < %s > %s", copied_input,
           copied_output);
  output = run_line(&shell, redir, &status);
  free(output);
  contents = read_text_file(copied_output);
  int input_output_ok = status == 0 && contents &&
                        !strcmp(contents, "zeta\nalpha\n");
  report_test("E023", "input and output redirection",
              "one command reads and writes different files", redir,
              "output file matches input", contents ? contents : "no file",
              input_output_ok, "");
  free(contents);

  output = run_line(&shell, "source no-such-script", &status);
  int source_error_ok = status != 0 && output &&
                        contains(output, "no-such-script");
  report_test("E024", "source error", "missing source file is reported",
              "source no-such-script", "nonzero status and filename in error",
              output ? output : "no diagnostic", source_error_ok, "");
  free(output);

  output = run_line(&shell, "exit 7", &status);
  int exit_ok = status == 7 && shell.should_exit;
  report_test("E020", "built-in exit", "exit records requested shell status",
              "exit 7", "returns 7 and marks this context to exit",
              exit_ok ? "status 7; exit requested" : "unexpected exit result",
              exit_ok, "");
  free(output);
  shell_jobs_cleanup(&shell);

  sem_destroy(&history->semaphore);
  free(history);
}

static void test_job_control(void) {
  for (int use_thread = 0; use_thread <= 1; use_thread++) {
    const char *mode = use_thread ? "thread" : "process";
    SharedHistory *history = new_worker_history(use_thread);
    SharedForeground *foreground = new_foreground();
    TestWorker worker;
    worker_init(&worker);
    int started = history && foreground &&
                  worker_start(&worker, use_thread, history, foreground,
                               shell_directory, NULL, 0) == 0;
    if (!started) {
      report_test(use_thread ? "J001" : "J101", "signals", "start worker for signal test",
                  mode, "worker starts", "worker setup failed", 0, "");
      if (worker.command_write >= 0)
        worker_stop(&worker);
      if (history)
        munmap(history, sizeof(*history));
      if (foreground)
        munmap(foreground, sizeof(*foreground));
      continue;
    }
    char output[8192];
    WorkerStatus status;
    worker_send(&worker, "/bin/sleep 10");
    int group = wait_for_foreground(foreground, 3000);
    int signal_sent = group > 0 && kill(-group, SIGINT) == 0;
    int collected = signal_sent && worker_collect(&worker, output, sizeof(output),
                                                  &status, 3000) == 0;
    int interrupt_ok = collected && status.status == 128 + SIGINT;
    report_test(use_thread ? "J002" : "J102", "Ctrl+C signal",
                "interrupt foreground command without stopping worker", mode,
                "command status 130 and worker remains available",
                interrupt_ok ? "foreground command interrupted" : "signal/status failed",
                interrupt_ok, "");
    int alive_result = worker_command(&worker, "/usr/bin/printf alive", output,
                                      sizeof(output), &status);
    int alive_ok = alive_result == 0 && status.status == 0 && contains(output, "alive");
    report_test(use_thread ? "J003" : "J103", "worker recovery",
                "worker accepts command after Ctrl+C", mode,
                "prints alive after interrupted command",
                alive_ok ? "alive" : output, alive_ok, "");

    worker_send(&worker, "/bin/sleep 10");
    group = wait_for_foreground(foreground, 3000);
    signal_sent = group > 0 && kill(-group, SIGTSTP) == 0;
    collected = signal_sent && worker_collect(&worker, output, sizeof(output),
                                               &status, 3000) == 0;
    int stopped_ok = collected && status.status == 128 + SIGTSTP &&
                     contains(output, "Stopped");
    report_test(use_thread ? "J004" : "J104", "Ctrl+Z/job control",
                "suspend foreground process group", mode,
                "worker returns and reports stopped job",
                stopped_ok ? "stopped job reported" : output, stopped_ok, "");

    int jobs_result = worker_command(&worker, "jobs", output, sizeof(output), &status);
    int jobs_ok = jobs_result == 0 && contains(output, "Stopped");
    report_test(use_thread ? "J005" : "J105", "jobs", "list stopped job", "jobs",
                "job shown as Stopped", jobs_ok ? "Stopped job listed" : output,
                jobs_ok, "");

    worker_command(&worker, "bg %1", output, sizeof(output), &status);
    worker_send(&worker, "fg %1");
    group = wait_for_foreground(foreground, 3000);
    signal_sent = group > 0 && kill(-group, SIGINT) == 0;
    collected = signal_sent && worker_collect(&worker, output, sizeof(output),
                                               &status, 3000) == 0;
    int fg_ok = collected && status.status == 128 + SIGINT;
    report_test(use_thread ? "J006" : "J106", "fg/bg", "resume background job in foreground",
                "bg %1 then fg %1; Ctrl+C", "foreground job can be interrupted",
                fg_ok ? "job resumed then interrupted" : "resume/wait failed", fg_ok,
                "");
    worker_stop(&worker);
    sem_destroy(&history->semaphore);
    munmap(history, sizeof(*history));
    munmap(foreground, sizeof(*foreground));
  }
}

static void test_multiwatch(void) {
  for (int use_thread = 0; use_thread <= 1; use_thread++) {
    const char *mode = use_thread ? "thread" : "process";
    const char *start_id = use_thread ? "M101" : "M001";
    SharedHistory *history = new_worker_history(use_thread);
    SharedForeground *foreground = new_foreground();
    TestWorker worker;
    worker_init(&worker);
    int started = history && foreground &&
                  worker_start(&worker, use_thread, history, foreground,
                               shell_directory, NULL, 0) == 0;
    if (!started) {
      report_test(start_id, "multiWatch", "start worker", mode,
                  "worker starts", "worker setup failed", 0, "");
      worker_stop(&worker);
      if (history)
        munmap(history, sizeof(*history));
      if (foreground)
        munmap(foreground, sizeof(*foreground));
      continue;
    }

    char output[8192];
    char first_run[8192];
    WorkerStatus status;
    worker_send(&worker,
                "multiWatch [\"/usr/bin/printf left\", \"/usr/bin/printf right\"]");
    int outputs_ok = worker_wait_output(&worker, "left", "right", first_run,
                                        sizeof(first_run), 2, 4000) == 0;
    int group = wait_for_foreground(foreground, 3000);
    int signalled = group > 0 && kill(-group, SIGINT) == 0;
    int completed = signalled && worker_collect(&worker, output, sizeof(output),
                                                 &status, 4000) == 0;
    int interrupt_ok = completed && status.status == 128 + SIGINT;
    int format_ok = outputs_ok && count_occurrences(first_run, "left") >= 2 &&
                    count_occurrences(first_run, "right") >= 2;
    report_test(use_thread ? "M102" : "M002", "multiWatch",
                "parallel commands print separated output", mode,
                "each command header is followed by its output", first_run,
                format_ok,
                "The supervisor is interrupted after both outputs arrive.");
    report_test(use_thread ? "M103" : "M003", "multiWatch/Ctrl+C",
                "interrupt watcher but keep worker alive", mode,
                "worker reports status 130", completed ? "status received"
                                                        : "no status returned",
                interrupt_ok, "");
    int usable = worker_command(&worker, "/usr/bin/printf after-watch", output,
                                sizeof(output), &status) == 0 &&
                 status.status == 0 && contains(output, "after-watch");
    report_test(use_thread ? "M104" : "M004", "multiWatch",
                "worker accepts a command after watcher stops", mode,
                "prints after-watch", usable ? "after-watch" : output, usable,
                "");
    worker_stop(&worker);
    sem_destroy(&history->semaphore);
    munmap(history, sizeof(*history));
    munmap(foreground, sizeof(*foreground));
  }
}

static void test_multiwatch_spacing_regression(void) {
  Tokens tokens = {0};
  Pipeline pipeline = {0};
  char *error = NULL;
  const char *command = "multiWatch [\"pwd\",\"whoami\",\"ls\"]";
  int result = shell_parse(command, &tokens, &pipeline, &error);
  int one_argument = result == 0 && pipeline.count == 1 &&
                     pipeline.commands[0].argument_count == 2;
  report_test("R001", "multiWatch syntax", "comma-separated items without spaces",
              command, "three commands remain separate watch items",
              one_argument ? "list is one shell argument after command"
                           : "different tokenization",
              !one_argument,
              "This reproduces the compact list format shown in the user screenshot.");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);
}

static int run_gui_smoke(const char *mode_argument) {
  const char *binary = getenv("TERMINATOR_TEST_BINARY");
  if (!binary || !*binary)
    binary = "./terminator";
  pid_t child = fork();
  if (child == 0) {
    if (mode_argument)
      execl(binary, binary, mode_argument, (char *)NULL);
    else
      execl(binary, binary, (char *)NULL);
    _exit(127);
  }
  if (child < 0)
    return -1;
  int finished = 0;
  int status = 0;
  for (int waited = 0; waited < 30; waited++) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      finished = 1;
      break;
    }
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
    nanosleep(&pause, NULL);
  }
  if (!finished) {
    kill(child, SIGTERM);
    for (int waited = 0; waited < 20; waited++) {
      if (waitpid(child, &status, WNOHANG) == child) {
        finished = 1;
        break;
      }
      struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
      nanosleep(&pause, NULL);
    }
  }
  if (!finished) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return -1;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void run_selected(const char *selection) {
  int all = !strcmp(selection, "all");
  if (all || !strcmp(selection, "unit")) {
    test_parser();
    test_history();
    test_scrollback();
    test_hotkeys();
  }
  if (all || !strcmp(selection, "integration")) {
    test_shell_features();
    test_worker_execution_mode(1);
    test_worker_execution_mode(0);
    test_job_control();
    test_multiwatch();
  }
  if (all || !strcmp(selection, "regression"))
    test_multiwatch_spacing_regression();
  if (all || !strcmp(selection, "gui")) {
    if (getenv("TERMINATOR_TEST_BUILD_FAILED")) {
      report_test("G001", "X11 GUI", "thread-mode application startup",
                  "make terminator", "application builds and opens",
                  "application build failed", 0,
                  "The core build failed before the GUI smoke test could run.");
      report_test("G002", "X11 GUI", "process-mode application startup",
                  "make terminator", "application builds and opens",
                  "application build failed", 0,
                  "The core build failed before the GUI smoke test could run.");
    } else if (!getenv("DISPLAY")) {
      report_skip("G001", "X11 GUI", "thread-mode startup", "DISPLAY is not set");
      report_skip("G002", "X11 GUI", "process-mode startup", "DISPLAY is not set");
    } else {
      int result = run_gui_smoke(NULL);
      report_test("G001", "X11 GUI", "thread-mode application startup", "./terminator",
                  "window stays alive until SIGTERM", result == 0 ? "started and stopped cleanly"
                                                                   : "startup failed or timed out",
                  result == 0, "Headless X11 smoke test; it does not inspect pixels.");
      result = run_gui_smoke("useProcess=True");
      report_test("G002", "X11 GUI", "process-mode application startup",
                  "./terminator useProcess=True", "window stays alive until SIGTERM",
                  result == 0 ? "started and stopped cleanly" : "startup failed or timed out",
                  result == 0, "Headless X11 smoke test; it does not inspect pixels.");
    }
  }
}

int main(int argument_count, char **arguments) {
  if (argument_count != 2) {
    fprintf(stderr, "usage: %s unit|integration|regression|all|gui\n",
            arguments[0]);
    return 2;
  }
  /* A broken worker pipe should fail a test, not kill the test runner. */
  signal(SIGPIPE, SIG_IGN);
  char *created_root = mkdtemp(temporary_root);
  if (!created_root) {
    perror("mkdtemp");
    return 2;
  }
  test_root = realpath(created_root, NULL);
  if (!test_root) {
    perror("realpath test directory");
    remove_tree(created_root);
    return 2;
  }
  snprintf(shell_directory, sizeof(shell_directory), "%s/shell", test_root);
  if (mkdir(shell_directory, 0700) < 0) {
    perror("mkdir test shell directory");
    remove_tree(test_root);
    return 2;
  }

  run_selected(arguments[1]);
  report_summary();
  int failed = failed_tests != 0;
  if (remove_tree(test_root) < 0) {
    fprintf(stderr, "test cleanup failed for %s\n", test_root);
    failed = 1;
  }
  free(test_root);
  return failed ? 1 : 0;
}
