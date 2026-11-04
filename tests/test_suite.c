#include "hotkeys.h"
#include "completion.h"
#include "scrollback.h"
#include "shell.h"
#include "terminator.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
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
static char failed_test_ids[512][32];
static size_t failed_test_count;
typedef struct {
  char *id;
  char *task;
  char *feature;
  char *scenario;
  char *input;
  char *expected;
  char *actual;
  char *notes;
  int passed;
  int skipped;
} TestRecord;
static TestRecord *test_records;
static size_t test_record_count;
static size_t test_record_capacity;
static int test_record_error;
static char temporary_root[] = "build/tests/tmp-XXXXXX";
static char *test_root;
static char shell_directory[TERM_PATH_BUFFER];
static unsigned int history_test_number;
static void wait_milliseconds(long milliseconds);
static int gui_find_text_line(Display *display, Window window,
                              const char *expected_text, int after_baseline);
static int join_test_path(char *destination, size_t capacity,
                          const char *directory, const char *name);
static int write_all(int fd, const char *text, size_t length);
static SharedHistory *new_worker_history(int use_thread);
static SharedForeground *new_foreground(void);
typedef struct TestWorker TestWorker;
static void worker_stop(TestWorker *worker);

static const char *task_for_feature(const char *feature) {
  if (strstr(feature, "Tasks 4 and 5"))
    return "Tasks 4 and 5";
  if (strstr(feature, "Task 11")) return "Task 11";
  if (strstr(feature, "Task 10")) return "Task 10";
  if (strstr(feature, "Task 9")) return "Task 9";
  if (strstr(feature, "Task 8")) return "Task 8";
  if (strstr(feature, "Task 7")) return "Task 7";
  if (strstr(feature, "Task 6")) return "Task 6";
  if (strstr(feature, "Task 5")) return "Task 5";
  if (strstr(feature, "Task 4")) return "Task 4";
  if (strstr(feature, "Task 3")) return "Task 3";
  if (strstr(feature, "Task 2")) return "Task 2";
  if (strstr(feature, "Task 1")) return "Task 1";
  if (strstr(feature, "history"))
    return "Task 9";
  if (strstr(feature, "completion"))
    return "Task 10";
  if (strstr(feature, "multiWatch"))
    return "Task 6";
  if (strstr(feature, "hotkey") || strstr(feature, "clipboard"))
    return "Task 7";
  if (strstr(feature, "signal") || strstr(feature, "job control") ||
      strstr(feature, "fg/bg") || strstr(feature, "jobs") ||
      strstr(feature, "worker recovery"))
    return "Task 8";
  if (strstr(feature, "GUI") || strstr(feature, "worker") ||
      strstr(feature, "execution modes") || strstr(feature, "scrollback"))
    return "Task 1";
  if (strstr(feature, "parser") || strstr(feature, "multiline") ||
      strstr(feature, "substitution") || strstr(feature, "quoting"))
    return "Task 3";
  if (strstr(feature, "redirection"))
    return strstr(feature, "pipeline") ? "Tasks 4 and 5" : "Task 4";
  if (strstr(feature, "pipeline"))
    return "Task 5";
  return "Task 2";
}

/* Worker and GUI case IDs distinguish their execution mode. */
static const char *mode_for_test_id(const char *id) {
  if (!id || !id[0] || !id[1])
    return "shared";
  int number = atoi(id + 1);
  if (id[0] == 'W' || id[0] == 'J')
    return number >= 100 ? "process" : "thread";
  if (id[0] == 'M')
    return number >= 100 ? "thread" : "process";
  if (id[0] == 'G') {
    if (number == 2 || number >= 100)
      return "process";
    return "thread";
  }
  return "shared";
}

static char *copy_or_empty(const char *text) {
  return strdup(text ? text : "");
}

static void save_test_record(const char *id, const char *task,
                             const char *feature, const char *scenario,
                             const char *input, const char *expected,
                             const char *actual, const char *notes,
                             int passed, int skipped) {
  if (test_record_count == test_record_capacity) {
    size_t capacity = test_record_capacity ? test_record_capacity * 2 : 128;
    TestRecord *records = realloc(test_records, capacity * sizeof(*records));
    if (!records) {
      test_record_error = 1;
      return;
    }
    test_records = records;
    test_record_capacity = capacity;
  }
  TestRecord record = {0};
  record.id = copy_or_empty(id);
  record.task = copy_or_empty(task);
  record.feature = copy_or_empty(feature);
  record.scenario = copy_or_empty(scenario);
  record.input = copy_or_empty(input);
  record.expected = copy_or_empty(expected);
  record.actual = copy_or_empty(actual);
  record.notes = copy_or_empty(notes);
  record.passed = passed;
  record.skipped = skipped;
  if (!record.id || !record.task || !record.feature || !record.scenario ||
      !record.input || !record.expected || !record.actual || !record.notes) {
    free(record.id);
    free(record.task);
    free(record.feature);
    free(record.scenario);
    free(record.input);
    free(record.expected);
    free(record.actual);
    free(record.notes);
    test_record_error = 1;
    return;
  }
  test_records[test_record_count++] = record;
}

static void write_xml_text(FILE *file, const char *text) {
  for (; text && *text; text++) {
    switch (*text) {
    case '&': fputs("&amp;", file); break;
    case '<': fputs("&lt;", file); break;
    case '>': fputs("&gt;", file); break;
    case '\"': fputs("&quot;", file); break;
    case '\'': fputs("&apos;", file); break;
    default: fputc(*text, file); break;
    }
  }
}

static int write_junit_report(const char *path) {
  if (!path || !*path)
    return 0;
  FILE *file = fopen(path, "w");
  if (!file)
    return -1;
  fprintf(file,
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<testsuite name=\"Terminator\" tests=\"%d\" failures=\"%d\" "
          "skipped=\"%d\">\n",
          total_tests, failed_tests, skipped_tests);
  for (size_t i = 0; i < test_record_count; i++) {
    TestRecord *record = &test_records[i];
    fputs("  <testcase classname=\"Terminator.Tests\" name=\"", file);
    write_xml_text(file, record->id);
    fputs(" - ", file);
    write_xml_text(file, record->scenario);
    fputs("\">\n", file);
    fputs("    <properties><property name=\"task\" value=\"", file);
    write_xml_text(file, record->task);
    fputs("\"/><property name=\"mode\" value=\"", file);
    write_xml_text(file, mode_for_test_id(record->id));
    fputs("\"/><property name=\"feature\" value=\"", file);
    write_xml_text(file, record->feature);
    fputs("\"/></properties>\n", file);
    if (record->skipped) {
      fputs("    <skipped message=\"", file);
      write_xml_text(file, record->notes);
      fputs("\"/>\n", file);
    } else if (!record->passed) {
      fputs("    <failure message=\"", file);
      write_xml_text(file, record->actual);
      fputs("\">", file);
      write_xml_text(file, record->notes);
      fputs("</failure>\n", file);
    }
    fputs("    <system-out>Input: ", file);
    write_xml_text(file, record->input);
    fputs("; Expected: ", file);
    write_xml_text(file, record->expected);
    fputs("; Actual: ", file);
    write_xml_text(file, record->actual);
    fputs("; Notes: ", file);
    write_xml_text(file, record->notes);
    fputs("</system-out>\n  </testcase>\n", file);
  }
  int failed = fputs("</testsuite>\n", file) == EOF;
  if (fclose(file) != 0)
    failed = 1;
  return failed ? -1 : test_record_error ? -1 : 0;
}

static int set_test_history_path(const char *name) {
  char path[TERM_PATH_BUFFER];
  if (join_test_path(path, sizeof(path), test_root, name) < 0)
    return -1;
  return setenv("TERMINATOR_HISTORY_FILE", path, 1);
}

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
  save_test_record(id, task_for_feature(feature), feature, scenario, input,
                   expected, actual, notes, passed, 0);
  if (passed)
    passed_tests++;
  else {
    failed_tests++;
    if (failed_test_count < sizeof(failed_test_ids) / sizeof(failed_test_ids[0]))
      snprintf(failed_test_ids[failed_test_count++],
               sizeof(failed_test_ids[0]), "%s", id);
  }
  print_field(id);
  fputs(" | Task: ", stdout);
  print_field(task_for_feature(feature));
  fputs(" | Mode: ", stdout);
  print_field(mode_for_test_id(id));
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
  save_test_record(id, task_for_feature(feature), feature, scenario, "n/a",
                   "run", "skipped", reason, 0, 1);
  print_field(id);
  fputs(" | Task: ", stdout);
  print_field(task_for_feature(feature));
  fputs(" | Mode: ", stdout);
  print_field(mode_for_test_id(id));
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

typedef struct {
  char text[4096];
  size_t count;
} HistorySearchCapture;

static void capture_history_match(const char *command, void *context) {
  HistorySearchCapture *capture = context;
  size_t used = strlen(capture->text);
  size_t remaining = sizeof(capture->text) - used;
  if (remaining > 1)
    snprintf(capture->text + used, remaining, "%s\n", command);
  capture->count++;
}

static size_t brute_longest_common_substring(const char *first,
                                             const char *second) {
  size_t best = 0;
  for (size_t start = 0; first[start]; start++) {
    for (size_t length = 1; first[start + length - 1]; length++) {
      char part[TERM_LINE_MAX];
      memcpy(part, first + start, length);
      part[length] = '\0';
      if (strstr(second, part) && length > best)
        best = length;
    }
  }
  return best;
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

  pid_t stress_child = fork();
  if (stress_child == 0) {
    static const char alphabet[] = "abc '\"\\|&<>$()";
    alarm(4);
    for (int sample = 0; sample < 2000; sample++) {
      char input[160];
      size_t length = (size_t)(sample % (int)(sizeof(input) - 1));
      for (size_t i = 0; i < length; i++)
        input[i] = alphabet[(sample * 17 + (int)i * 7) %
                            (int)(sizeof(alphabet) - 1)];
      input[length] = '\0';
      memset(&tokens, 0, sizeof(tokens));
      memset(&pipeline, 0, sizeof(pipeline));
      error = NULL;
      (void)shell_parse(input, &tokens, &pipeline, &error);
      free(error);
      shell_tokens_free(&tokens);
      shell_pipeline_free(&pipeline);
    }
    _exit(0);
  }
  finished = 0;
  child_status = 0;
  for (int i = 0; stress_child > 0 && i < 450; i++) {
    pid_t waited = waitpid(stress_child, &child_status, WNOHANG);
    if (waited == stress_child) {
      finished = 1;
      break;
    }
    wait_milliseconds(10);
  }
  if (stress_child > 0 && !finished) {
    kill(stress_child, SIGKILL);
    waitpid(stress_child, &child_status, 0);
  }
  int parser_stress_ok = finished && WIFEXITED(child_status) &&
                         WEXITSTATUS(child_status) == 0;
  report_test("P014", "parser", "malformed-input crash and hang resistance",
              "2,000 deterministic combinations of quotes and operators",
              "child exits normally without hanging or crashing",
              parser_stress_ok ? "all parser samples returned"
                               : "parser child crashed or timed out",
              parser_stress_ok,
              "Runs in a disposable child so a parser fault cannot kill the suite.");
}

static void test_history(void) {
  set_test_history_path("history-unit");
  SharedHistory *history = calloc(1, sizeof(*history));
  if (!history || history_init(history, 0) < 0) {
    report_test("H001", "history", "initialize persistent history", "history_init",
                "initialized", "allocation or semaphore initialization failed",
                0, "");
    free(history);
    return;
  }
  char (*snapshot)[TERM_LINE_MAX] = calloc(12, sizeof(*snapshot));
  char value[64];
  for (int i = 0; i < TERM_HISTORY_CAP + 6; i++) {
    snprintf(value, sizeof(value), "cmd%05d", i);
    if (history_add(history, value) < 0)
      break;
  }
  size_t count = history_snapshot(history, snapshot, 10);
  int cap_ok = count == 10 && !strcmp(snapshot[0], "cmd09996") &&
               !strcmp(snapshot[9], "cmd10005");
  report_test("H002", "history", "persistent file keeps newest 10,000 commands",
              "add 10,006 commands", "history ends at cmd10005 and starts at cmd00006",
              cap_ok ? "newest entries are retained" : "history range differs",
              cap_ok, "The test reads only ten rows to keep memory use small.");

  char recent[64] = {0};
  int recent_ok = history_get_recent(history, 0, recent, sizeof(recent)) &&
                  !strcmp(recent, "cmd10005");
  report_test("H003", "history", "retrieve most recent entry", "offset 0",
              "cmd10005", recent, recent_ok, "");

  count = history_snapshot(history, snapshot, 5);
  int limited_ok = count == 5 && !strcmp(snapshot[0], "cmd10001") &&
                   !strcmp(snapshot[4], "cmd10005");
  report_test("H004", "history", "snapshot returns requested newest rows",
              "snapshot max_entries=5", "latest five entries",
              limited_ok ? "cmd10001 through cmd10005" : "different snapshot",
              limited_ok, "");

  int empty_add_ok = history_add(history, "") == 0;
  count = history_snapshot(history, snapshot, 1);
  int empty_ok = empty_add_ok && count == 1 && !strcmp(snapshot[0], "cmd10005");
  report_test("H005", "history", "empty command is not recorded", "add empty line",
              "history unchanged", empty_ok ? "unchanged" : "changed", empty_ok,
              "");
  int missing_ok = !history_get_recent(history, TERM_HISTORY_CAP, recent,
                                       sizeof(recent));
  report_test("H006", "history", "out-of-range history offset is rejected",
              "request offset 10,000", "returns no entry",
              missing_ok ? "no entry" : recent, missing_ok, "");

  int multiline_ok = history_add(history, "first line\nsecond\\line") == 0;
  int reinitialize_ok = 0;
  sem_destroy(&history->semaphore);
  if (multiline_ok && history_init(history, 0) == 0) {
    reinitialize_ok = history_get_recent(history, 0, recent, sizeof(recent)) &&
                      !strcmp(recent, "first line\nsecond\\line");
  }
  report_test("H007", "history persistence", "restart reloads escaped multiline entry",
              "add command with newline and backslash; initialize again",
              "same command bytes are recovered",
              reinitialize_ok ? "multiline command restored" : "command differed",
              reinitialize_ok, "The file uses escaped physical lines.");

  history_add(history, "run myprog -v");
  history_add(history, "run myprog --help");
  HistorySearchCapture matches = {0};
  size_t best = 0;
  int found = history_search(history, "myprog", capture_history_match,
                             &matches, &best);
  int exact_ok = found == 2 && matches.count == 2 &&
                 contains(matches.text, "run myprog -v") &&
                 contains(matches.text, "run myprog --help");
  report_test("H008", "history search", "exact term returns all matching commands",
              "search myprog", "two matching history commands",
              matches.text, exact_ok, "");

  history_add(history, "/tmp/alpha-one");
  history_add(history, "/tmp/alpha-two");
  memset(&matches, 0, sizeof(matches));
  found = history_search(history, "zzalphaqq", capture_history_match,
                         &matches, &best);
  int similar_ok = found == 2 && best == 5 && matches.count == 2 &&
                   contains(matches.text, "/tmp/alpha-one") &&
                   contains(matches.text, "/tmp/alpha-two");
  report_test("H009", "history search", "fallback returns longest shared substring ties",
              "search zzalphaqq", "two commands with shared substring alpha",
              matches.text, similar_ok, "Best substring must be longer than two bytes.");

  memset(&matches, 0, sizeof(matches));
  found = history_search(history, "qx", capture_history_match, &matches, &best);
  int too_short_ok = found == 0 && matches.count == 0 && best <= 2;
  report_test("H010", "history search", "two-byte fallback is rejected", "search qx",
              "no callback results", matches.text, too_short_ok, "");
  found = history_search(history, "no-such-history-command", NULL, NULL, &best);
  int not_found_ok = found == 0;
  report_test("H011", "history search", "missing term returns zero", "unique absent term",
              "zero matches", found == 0 ? "zero matches" : "unexpected result",
              not_found_ok, "");
  found = history_search(history, "", NULL, NULL, &best);
  int empty_search_ok = found == 0;
  report_test("H012", "history search", "empty search is safe", "empty term",
              "zero matches", found == 0 ? "zero matches" : "unexpected result",
              empty_search_ok, "");

  set_test_history_path("history-search-oracle");
  SharedHistory oracle_history = {0};
  const char *commands[] = {"abca", "aacb", "cabca", "bdabc", "ddcaa",
                            "ababa", "bcacb", "zzabx", "xxxy", "xabcx"};
  const char *terms[] = {"ab", "abc", "abca", "bcac", "zyx",
                         "zzac", "cbb", "aacaa", "bca", "xyq"};
  int oracle_initialized = history_init(&oracle_history, 0) == 0;
  int oracle_ok = oracle_initialized;
  for (size_t i = 0; oracle_ok && i < sizeof(commands) / sizeof(commands[0]); i++)
    oracle_ok = history_add(&oracle_history, commands[i]) == 0;
  for (size_t term_index = 0;
       oracle_ok && term_index < sizeof(terms) / sizeof(terms[0]); term_index++) {
    const char *term = terms[term_index];
    size_t expected_best = 0;
    int has_exact = 0;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
      if (strstr(commands[i], term))
        has_exact = 1;

    char expected[4096] = "";
    size_t expected_count = 0;
    if (has_exact) {
      for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strstr(commands[i], term)) {
          snprintf(expected + strlen(expected), sizeof(expected) - strlen(expected),
                   "%s\n", commands[i]);
          expected_count++;
        }
      }
    } else {
      for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        size_t shared = brute_longest_common_substring(term, commands[i]);
        if (shared > expected_best)
          expected_best = shared;
      }
      if (expected_best > 2) {
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
          if (brute_longest_common_substring(term, commands[i]) == expected_best) {
            snprintf(expected + strlen(expected), sizeof(expected) - strlen(expected),
                     "%s\n", commands[i]);
            expected_count++;
          }
        }
      }
    }
    HistorySearchCapture actual = {0};
    size_t actual_best = 0;
    int actual_count = history_search(&oracle_history, term,
                                      capture_history_match, &actual,
                                      &actual_best);
    if (actual_count != (int)expected_count || actual.count != expected_count ||
        strcmp(actual.text, expected) || actual_best != expected_best)
      oracle_ok = 0;
  }
  report_test("H013", "history search", "suffix search agrees with brute-force oracle",
              "10 overlapping and repeated-character queries",
              "same exact matches or best-substring ties as reference logic",
              oracle_ok ? "all ten query results match" : "a result differed",
              oracle_ok, "Exercises substring automaton fallback and exact matching.");
  if (oracle_initialized)
    sem_destroy(&oracle_history.semaphore);

  set_test_history_path("history-concurrent");
  SharedHistory concurrent_history = {0};
  int concurrent_initialized = history_init(&concurrent_history, 0) == 0;
  int concurrent_ok = concurrent_initialized;
  pid_t writer = concurrent_ok ? fork() : -1;
  if (writer == 0) {
    SharedHistory child_history = {0};
    int child_ok = history_init(&child_history, 0) == 0;
    for (int i = 0; child_ok && i < 100; i++) {
      char entry[32];
      snprintf(entry, sizeof(entry), "child-%03d", i);
      child_ok = history_add(&child_history, entry) == 0;
    }
    if (child_ok)
      sem_destroy(&child_history.semaphore);
    _exit(child_ok ? 0 : 1);
  }
  if (writer < 0)
    concurrent_ok = 0;
  for (int i = 0; concurrent_ok && i < 100; i++) {
    char entry[32];
    snprintf(entry, sizeof(entry), "parent-%03d", i);
    concurrent_ok = history_add(&concurrent_history, entry) == 0;
  }
  if (writer > 0) {
    int writer_status = 0;
    concurrent_ok = waitpid(writer, &writer_status, 0) == writer &&
                    WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0 &&
                    concurrent_ok;
  }
  char (*concurrent_rows)[TERM_LINE_MAX] = calloc(200, sizeof(*concurrent_rows));
  size_t concurrent_count = concurrent_rows
                                ? history_snapshot(&concurrent_history,
                                                   concurrent_rows, 200)
                                : 0;
  int all_writers_ok = concurrent_ok && concurrent_count == 200;
  int parent_rows = 0;
  int child_rows = 0;
  for (size_t i = 0; i < concurrent_count; i++) {
    parent_rows += !strncmp(concurrent_rows[i], "parent-", 7);
    child_rows += !strncmp(concurrent_rows[i], "child-", 6);
  }
  all_writers_ok = all_writers_ok && parent_rows == 100 && child_rows == 100;
  report_test("H014", "history concurrency", "independent processes append without lost rows",
              "two writers append 100 entries each", "200 entries remain in the file",
              all_writers_ok ? "all parent and child rows found" : "rows were lost",
              all_writers_ok, "Uses the sidecar flock while each process has its own semaphore.");
  free(concurrent_rows);
  if (concurrent_initialized)
    sem_destroy(&concurrent_history.semaphore);

  sem_destroy(&history->semaphore);
  free(snapshot);
  free(history);
}

static void test_history_edges(void) {
  set_test_history_path("history-edges");
  SharedHistory history = {0};
  int initialized = history_init(&history, 0) == 0;
  report_test("H015", "history file", "new history file uses private permissions",
              "initialize a fresh history file", "mode is 0600",
              initialized ? "initialized" : "initialization failed", initialized,
              "Checks the file mode rather than relying on the process umask.");
  if (!initialized)
    return;
  struct stat file_info;
  int private_file = stat(history.file_path, &file_info) == 0 &&
                     (file_info.st_mode & 0777) == 0600;
  report_test("H016", "history file", "history data is not world-readable",
              history.file_path, "permissions are exactly 0600",
              private_file ? "mode 0600" : "unexpected mode or stat failure",
              private_file, "The lock file is private as well.");

  size_t empty_start_count = history.entry_count;
  int null_add_ok = history_add(NULL, "ignored") == 0 &&
                    history_add(&history, NULL) == 0 &&
                    history_add(&history, "") == 0 &&
                    history.entry_count == empty_start_count;
  report_test("H017", "history boundaries", "null and empty entries are ignored",
              "history_add(NULL), history_add(history,NULL), history_add(empty)",
              "no crash and no entry", null_add_ok ? "all inputs ignored" : "unexpected result",
              null_add_ok, "The worker records only non-empty submitted commands.");

  char too_long[TERM_LINE_MAX + 1];
  memset(too_long, 'x', sizeof(too_long) - 1);
  too_long[sizeof(too_long) - 1] = '\0';
  errno = 0;
  int too_long_result = history_add(&history, too_long);
  int long_rejected = too_long_result < 0 && errno == E2BIG;
  report_test("H018", "history boundaries", "oversized command is rejected safely",
              "add TERM_LINE_MAX bytes", "returns error E2BIG",
              long_rejected ? "rejected with E2BIG" : "accepted or wrong error",
              long_rejected, "Does not append a truncated command.");

  history_add(&history, "alpha-beta exact");
  history_add(&history, "alpha-xxxxx only-shares-alpha");
  history_add(&history, "duplicate-term");
  history_add(&history, "duplicate-term");
  history_add(&history, "boundary-abc-match");
  HistorySearchCapture matches = {0};
  size_t best = 0;
  int found = history_search(&history, "alpha-beta", capture_history_match,
                             &matches, &best);
  int exact_precedence = found == 1 && matches.count == 1 &&
                         !strcmp(matches.text, "alpha-beta exact\n");
  report_test("H019", "history search", "exact matches suppress approximate matches",
              "search alpha-beta", "only the command containing the full term",
              matches.text, exact_precedence,
              "A longer common fragment in another command must not replace an exact hit.");

  memset(&matches, 0, sizeof(matches));
  found = history_search(&history, "duplicate-term", capture_history_match,
                         &matches, &best);
  int duplicate_ok = found == 2 && matches.count == 2 &&
                     count_occurrences(matches.text, "duplicate-term\n") == 2;
  report_test("H020", "history search", "repeated commands stay as separate history rows",
              "search duplicate-term", "both submitted copies are returned",
              matches.text, duplicate_ok, "History does not deduplicate commands.");

  memset(&matches, 0, sizeof(matches));
  found = history_search(&history, "xxabczz", capture_history_match,
                         &matches, &best);
  int three_byte_match = found == 1 && best == 3 &&
                         contains(matches.text, "boundary-abc-match");
  report_test("H021", "history search", "fallback accepts a three-byte shared substring",
              "search xxabczz", "command sharing exactly abc is returned",
              matches.text, three_byte_match, "Two-byte matches remain below the threshold.");

  found = history_search(&history, "duplicate-term", NULL, NULL, &best);
  int no_callback_ok = found == 2;
  report_test("H022", "history search", "counting works without a callback",
              "search duplicate-term with callback=NULL", "returns two matches",
              no_callback_ok ? "two matches" : "wrong match count", no_callback_ok, "");

  char short_buffer[5] = {0};
  int small_buffer_ok = history_get_recent(&history, 0, short_buffer,
                                           sizeof(short_buffer)) &&
                        !strcmp(short_buffer, "boun");
  report_test("H023", "history snapshot", "small output buffer is safely terminated",
              "read newest row into 5 bytes", "returns the first four bytes and NUL",
              short_buffer, small_buffer_ok, "Caller-owned buffer remains terminated.");

  char zero_capacity_row[TERM_LINE_MAX] = {0};
  int zero_snapshot_ok = history_snapshot(&history, NULL, 1) == 0 &&
                         history_snapshot(&history, &zero_capacity_row, 0) == 0;
  report_test("H024", "history snapshot", "null snapshot and zero capacity are safe",
              "snapshot(NULL,1); snapshot(buffer,0)", "both return zero",
              zero_snapshot_ok ? "zero rows" : "unexpected result", zero_snapshot_ok, "");

  size_t before = history.entry_count;
  int cr_ok = history_add(&history, "line\rreturn\\tail") == 0;
  sem_destroy(&history.semaphore);
  int reopened = cr_ok && history_init(&history, 0) == 0;
  char recent[TERM_LINE_MAX] = {0};
  int cr_round_trip = reopened && history_get_recent(&history, 0, recent,
                                                     sizeof(recent)) &&
                      !strcmp(recent, "line\rreturn\\tail");
  report_test("H025", "history persistence", "carriage return and backslash survive restart",
              "append CR and backslash, reopen file", "all original bytes are restored",
              cr_round_trip ? "record restored exactly" : "record changed", cr_round_trip,
              "The file escapes CR and backslash so one command stays one physical row.");

  int too_long_search = 0;
  char huge_term[TERM_LINE_MAX + 1];
  memset(huge_term, 'q', sizeof(huge_term) - 1);
  huge_term[sizeof(huge_term) - 1] = '\0';
  too_long_search = history_search(&history, huge_term, NULL, NULL, &best) == 0;
  report_test("H026", "history search", "overlong search term returns no result",
              "search TERM_LINE_MAX bytes", "returns zero without allocating an oversized automaton",
              too_long_search ? "zero matches" : "unexpected result", too_long_search, "");

  int unchanged_count = reopened && history.entry_count >= before + 1;
  report_test("H027", "history cache", "reopen refreshes persistent entry count",
              "reinitialize after CR entry", "new row is included in the count",
              unchanged_count ? "entry count refreshed" : "entry count differed",
              unchanged_count, "Count describes physical records in the history file.");
  sem_destroy(&history.semaphore);

  char default_home[TERM_PATH_BUFFER];
  join_test_path(default_home, sizeof(default_home), test_root, "history-home");
  mkdir(default_home, 0700);
  const char *old_home_value = getenv("HOME");
  char *saved_home = old_home_value ? strdup(old_home_value) : NULL;
  char *saved_history = getenv("TERMINATOR_HISTORY_FILE")
                            ? strdup(getenv("TERMINATOR_HISTORY_FILE"))
                            : NULL;
  setenv("HOME", default_home, 1);
  unsetenv("TERMINATOR_HISTORY_FILE");
  SharedHistory default_history = {0};
  int default_ok = history_init(&default_history, 0) == 0;
  char expected_path[TERM_PATH_BUFFER];
  join_test_path(expected_path, sizeof(expected_path), default_home,
                 ".terminator_history");
  default_ok = default_ok && !strcmp(default_history.file_path, expected_path);
  report_test("H028", "history configuration", "default history is under HOME",
              "unset override; HOME=<temporary directory>",
              "uses ~/.terminator_history", default_ok ? default_history.file_path
                                                        : "wrong path or initialization failed",
              default_ok, "TERMINATOR_HISTORY_FILE remains available for test isolation.");
  if (default_ok)
    sem_destroy(&default_history.semaphore);
  if (saved_home) {
    setenv("HOME", saved_home, 1);
    free(saved_home);
  } else {
    unsetenv("HOME");
  }
  if (saved_history) {
    setenv("TERMINATOR_HISTORY_FILE", saved_history, 1);
    free(saved_history);
  } else {
    unsetenv("TERMINATOR_HISTORY_FILE");
  }

  const char *restored_home_value = getenv("HOME");
  char *restored_home = restored_home_value ? strdup(restored_home_value) : NULL;
  const char *restored_history_value = getenv("TERMINATOR_HISTORY_FILE");
  char *restored_history = restored_history_value
                               ? strdup(restored_history_value)
                               : NULL;
  unsetenv("HOME");
  unsetenv("TERMINATOR_HISTORY_FILE");
  SharedHistory missing_home_history = {0};
  errno = 0;
  int missing_home_result = history_init(&missing_home_history, 0);
  int missing_home_ok = missing_home_result < 0 && errno == ENOENT;
  report_test("H112", "history configuration", "missing HOME reports the right error",
              "unset HOME and history path override", "fails with ENOENT",
              missing_home_ok ? "ENOENT" : "wrong return or errno",
              missing_home_ok, "Keeps the path-construction error for the caller.");

  char long_path[TERM_PATH_BUFFER];
  memset(long_path, 'p', sizeof(long_path) - 1);
  long_path[sizeof(long_path) - 1] = '\0';
  setenv("HOME", default_home, 1);
  setenv("TERMINATOR_HISTORY_FILE", long_path, 1);
  SharedHistory long_path_history = {0};
  errno = 0;
  int long_path_result = history_init(&long_path_history, 0);
  int long_path_ok = long_path_result < 0 && errno == ENAMETOOLONG;
  report_test("H113", "history configuration", "overlong history path is rejected",
              "set override to 4,095 bytes", "fails with ENAMETOOLONG",
              long_path_ok ? "ENAMETOOLONG" : "wrong return or errno",
              long_path_ok, "No file is opened for a path too large for SharedHistory.");
  if (restored_home) {
    setenv("HOME", restored_home, 1);
    free(restored_home);
  } else {
    unsetenv("HOME");
  }
  if (restored_history) {
    setenv("TERMINATOR_HISTORY_FILE", restored_history, 1);
    free(restored_history);
  } else {
    unsetenv("TERMINATOR_HISTORY_FILE");
  }
}

static void test_history_search_matrix(void) {
  set_test_history_path("history-search-matrix");
  SharedHistory history = {0};
  if (history_init(&history, 0) < 0) {
    report_test("H029", "history search", "initialize generated search matrix",
                "history_init", "ready", "initialization failed", 0, "");
    return;
  }
  const char *commands[] = {"abcabc", "abccba", "cababa", "ccabca",
                            "baacbb", "bcbcaa", "cabbcc", "abcxyz"};
  int entries_added = 1;
  for (size_t i = 0; entries_added &&
                     i < sizeof(commands) / sizeof(commands[0]); i++)
    entries_added = history_add(&history, commands[i]) == 0;
  if (!entries_added) {
    report_test("H030", "history search", "seed generated search matrix",
                "append eight test commands", "all commands recorded",
                "one or more appends failed", 0, "");
    sem_destroy(&history.semaphore);
    return;
  }
  int case_number = 31;
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      for (int c = 0; c < 3; c++)
        for (int d = 0; d < 3; d++) {
          char term[] = {(char)('a' + a), (char)('a' + b),
                         (char)('a' + c), (char)('a' + d), '\0'};
          int has_exact = 0;
          for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
            if (strstr(commands[i], term))
              has_exact = 1;
          size_t expected_best = 0;
          char expected[4096] = "";
          size_t expected_count = 0;
          if (has_exact) {
            for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
              if (strstr(commands[i], term)) {
                size_t used = strlen(expected);
                snprintf(expected + used, sizeof(expected) - used,
                         "%s\n", commands[i]);
                expected_count++;
              }
            }
          } else {
            for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
              size_t shared = brute_longest_common_substring(term, commands[i]);
              if (shared > expected_best)
                expected_best = shared;
            }
            if (expected_best > 2) {
              for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                if (brute_longest_common_substring(term, commands[i]) ==
                    expected_best) {
                  size_t used = strlen(expected);
                  snprintf(expected + used, sizeof(expected) - used,
                           "%s\n", commands[i]);
                  expected_count++;
                }
              }
            }
          }

          HistorySearchCapture actual = {0};
          size_t actual_best = 0;
          int actual_count = history_search(&history, term,
                                            capture_history_match, &actual,
                                            &actual_best);
          int case_ok = actual_count == (int)expected_count &&
                        actual.count == expected_count &&
                        !strcmp(actual.text, expected) &&
                        actual_best == expected_best;
          char id[16];
          snprintf(id, sizeof(id), "H%03d", case_number);
          report_test(id, "history search", "generated exact/fallback query",
                      term, "matches the brute-force substring oracle",
                      case_ok ? "oracle agrees" : "result differs", case_ok,
                      "The query is one of 81 strings over the alphabet abc.");
          case_number++;
        }
  sem_destroy(&history.semaphore);
}

static void test_completion(void) {
  char directory[TERM_PATH_BUFFER];
  char subdirectory[TERM_PATH_BUFFER];
  char path[TERM_PATH_BUFFER];
  join_test_path(directory, sizeof(directory), test_root, "completion");
  mkdir(directory, 0700);
  join_test_path(path, sizeof(path), directory, "alpha.txt");
  write_text_file(path, "a");
  join_test_path(path, sizeof(path), directory, "alpine.txt");
  write_text_file(path, "b");
  join_test_path(path, sizeof(path), directory, "document.txt");
  write_text_file(path, "doc");
  join_test_path(path, sizeof(path), directory, "space file.txt");
  write_text_file(path, "space");
  join_test_path(path, sizeof(path), directory, ".secret");
  write_text_file(path, "hidden");
  join_test_path(subdirectory, sizeof(subdirectory), directory, "subdir");
  mkdir(subdirectory, 0700);
  join_test_path(path, sizeof(path), subdirectory, "inside.txt");
  write_text_file(path, "inside");

  CompletionResult result;
  char *prefix = NULL;
  const char *line = "cat al";
  int scan = completion_find(line, strlen(line), directory, &result, &prefix);
  char *common = completion_common_prefix(result.items, result.count);
  int multiple_ok = scan == 0 && result.count == 2 && !strcmp(prefix, "al") &&
                    common && !strcmp(common, "alp");
  report_test("C001", "filename completion", "multiple matches find longest common prefix",
              line, "alpha.txt and alpine.txt; common prefix alp",
              multiple_ok ? common : "candidate list or prefix differed",
              multiple_ok, "");
  free(common);
  free(prefix);
  completion_free(&result);

  line = "cat docu";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int one_ok = scan == 0 && result.count == 1 &&
               !strcmp(result.items[0], "document.txt");
  report_test("C002", "filename completion", "one matching file is returned",
              line, "document.txt", one_ok ? result.items[0] : "no unique match",
              one_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat sub";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int directory_ok = scan == 0 && result.count == 1 &&
                     !strcmp(result.items[0], "subdir/");
  report_test("C003", "filename completion", "directories receive a slash",
              line, "subdir/", directory_ok ? result.items[0] : "no directory candidate",
              directory_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat subdir/in";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int nested_ok = scan == 0 && result.count == 1 &&
                  !strcmp(result.items[0], "subdir/inside.txt");
  report_test("C004", "filename completion", "nested path uses the tab directory",
              line, "subdir/inside.txt", nested_ok ? result.items[0] : "no nested match",
              nested_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat ";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int hidden_ok = scan == 0 && result.count >= 4;
  for (size_t i = 0; i < result.count; i++)
    if (result.items[i][0] == '.')
      hidden_ok = 0;
  report_test("C005", "filename completion", "hidden files are omitted for an empty prefix",
              line, "ordinary names only", hidden_ok ? "hidden entry omitted"
                                                       : "hidden entry was listed",
              hidden_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat .";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int dotfile_ok = scan == 0 && result.count == 1 &&
                   !strcmp(result.items[0], ".secret");
  report_test("C006", "filename completion", "dotfiles appear when the prefix starts with a dot",
              line, ".secret", dotfile_ok ? result.items[0] : "dotfile missing",
              dotfile_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat 'space f";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  char *encoded = completion_encode_suffix("ile.txt", result.quote);
  int quote_ok = scan == 0 && result.count == 1 &&
                 !strcmp(result.items[0], "space file.txt") &&
                 result.quote == '\'' && encoded && !strcmp(encoded, "ile.txt");
  report_test("C007", "filename completion", "quoted path prefix is decoded and kept quoted",
              line, "space file.txt with single-quote context",
              quote_ok ? "quoted completion matches" : "quoted completion differed",
              quote_ok, "");
  free(encoded);
  free(prefix);
  completion_free(&result);

  line = "cat space\\ f";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int escaped_ok = scan == 0 && result.count == 1 &&
                   !strcmp(prefix, "space f") &&
                   !strcmp(result.items[0], "space file.txt");
  report_test("C008", "filename completion", "escaped whitespace is matched",
              line, "space file.txt", escaped_ok ? result.items[0] : "no escaped match",
              escaped_ok, "");
  free(prefix);
  completion_free(&result);

  line = "cat missing-file-prefix";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int no_match_ok = scan == 0 && result.count == 0;
  report_test("C009", "filename completion", "no matching file leaves no candidates",
              line, "zero candidates", no_match_ok ? "zero candidates" : "unexpected match",
              no_match_ok, "");
  free(prefix);
  completion_free(&result);

  char editable[64] = "cat al|grep";
  size_t editable_length = strlen(editable);
  size_t cursor = 6;
  int insert_ok = completion_insert_text(editable, sizeof(editable),
                                         &editable_length, &cursor, "pha.txt") == 0 &&
                  !strcmp(editable, "cat alpha.txt|grep") && cursor == 13;
  report_test("C010", "filename completion", "insertion preserves text after cursor",
              "cat al|grep; insert pha.txt at cursor", "cat alpha.txt|grep",
              editable, insert_ok, "");

  char quoted[64] = "cat ";
  size_t quoted_length = strlen(quoted);
  cursor = quoted_length;
  char *escaped_suffix = completion_encode_suffix("space\\ file.txt", 0);
  int escaping_ok = escaped_suffix &&
                    completion_insert_text(quoted, sizeof(quoted), &quoted_length,
                                           &cursor, escaped_suffix) == 0 &&
                    !strcmp(quoted, "cat space\\\\\\ file.txt");
  report_test("C011", "filename completion", "unquoted inserted text is escaped for the parser",
              "insert a space and backslash", "shell metacharacters are quoted",
              quoted, escaping_ok, "");
  free(escaped_suffix);

  char absolute_line[TERM_PATH_BUFFER + 32];
  snprintf(absolute_line, sizeof(absolute_line), "cat %s/al", directory);
  scan = completion_find(absolute_line, strlen(absolute_line), directory,
                         &result, &prefix);
  int absolute_ok = scan == 0 && result.count == 2 &&
                    !strncmp(result.items[0], directory, strlen(directory));
  report_test("C012", "filename completion", "absolute directory prefix is scanned",
              absolute_line, "absolute alpha and alpine paths",
              absolute_ok ? result.items[0] : "absolute path not found",
              absolute_ok, "");
  free(prefix);
  completion_free(&result);

  char *choices[] = {"abc.txt", "abcd.txt"};
  char choice_line[64] = "run abc|sort";
  size_t choice_length = strlen(choice_line);
  size_t choice_cursor = strlen("run abc");
  int choose_ok = completion_select(choice_line, sizeof(choice_line),
                                    &choice_length, &choice_cursor, choices, 2,
                                    1, 3, 0) == 0 &&
                  !strcmp(choice_line, "run abc.txt|sort");
  report_test("C013", "filename completion", "numbered choice inserts selected suffix",
              "choose 1 from abc.txt and abcd.txt", "run abc.txt|sort",
              choice_line, choose_ok, "The untouched text after the cursor stays in place.");

  char invalid_choice_line[32] = "abc";
  size_t invalid_length = 3;
  size_t invalid_cursor = 3;
  int invalid_choice_ok = completion_select(
                              invalid_choice_line, sizeof(invalid_choice_line),
                              &invalid_length, &invalid_cursor, choices, 2, 3,
                              3, 0) < 0 && !strcmp(invalid_choice_line, "abc");
  report_test("C014", "filename completion", "out-of-range selection changes nothing",
              "choose 3 from two candidates", "input remains abc",
              invalid_choice_line, invalid_choice_ok, "");

  char apostrophe_name[] = "can't.txt";
  char *apostrophe_suffix = completion_encode_suffix(apostrophe_name + 3, '\'');
  const char apostrophe_expected[] = {'\'', '\\', '\'', '\'', 't', '.',
                                      't', 'x', 't', '\0'};
  int apostrophe_ok = apostrophe_suffix &&
                      !strcmp(apostrophe_suffix, apostrophe_expected);
  report_test("C015", "filename completion", "apostrophe is escaped inside single quotes",
              "complete can't.txt inside an open single quote",
              "close quote, escape apostrophe, reopen quote",
              apostrophe_suffix ? apostrophe_suffix : "allocation failed",
              apostrophe_ok, "");
  free(apostrophe_suffix);

  char *double_suffix = completion_encode_suffix("$name`", '"');
  int double_quote_ok = double_suffix && !strcmp(double_suffix, "\\$name\\`");
  report_test("C016", "filename completion", "expansion characters are escaped in double quotes",
              "$name backtick", "both shell expansion characters escaped",
              double_suffix ? double_suffix : "allocation failed",
              double_quote_ok, "");
  free(double_suffix);

  char short_line[5] = "abc";
  size_t short_length = 3;
  size_t short_cursor = 3;
  int boundary_ok = completion_insert_text(short_line, sizeof(short_line),
                                           &short_length, &short_cursor,
                                           "long") == 0 &&
                    !strcmp(short_line, "abcl") && short_length == 4;
  report_test("C017", "filename completion", "insertion respects input buffer capacity",
              "insert longer text than the one-byte space left", "safe truncation to abcl",
              short_line, boundary_ok, "");

  char other_directory[TERM_PATH_BUFFER];
  join_test_path(other_directory, sizeof(other_directory), test_root,
                 "completion-other");
  mkdir(other_directory, 0700);
  join_test_path(path, sizeof(path), other_directory, "only-here.txt");
  write_text_file(path, "other");
  line = "cat only";
  scan = completion_find(line, strlen(line), other_directory, &result, &prefix);
  int tab_directory_ok = scan == 0 && result.count == 1 &&
                         !strcmp(result.items[0], "only-here.txt");
  report_test("C018", "filename completion", "candidate source is the active tab directory",
              line, "only-here.txt from supplied directory",
              tab_directory_ok ? result.items[0] : "wrong directory searched",
              tab_directory_ok, "Each tab passes its own working directory.");
  free(prefix);
  completion_free(&result);
}

static void test_completion_edges(void) {
  char directory[TERM_PATH_BUFFER];
  char nested[TERM_PATH_BUFFER];
  char deeper[TERM_PATH_BUFFER];
  char path[TERM_PATH_BUFFER];
  join_test_path(directory, sizeof(directory), test_root, "completion-edge");
  mkdir(directory, 0700);
  join_test_path(nested, sizeof(nested), directory, "nested");
  mkdir(nested, 0700);
  join_test_path(deeper, sizeof(deeper), nested, "deeper");
  mkdir(deeper, 0700);
  const char *names[] = {"alpha.txt", "albatross", "beta.txt", "space file.txt",
                         "quote'file.txt", "semi;colon.txt", "$cash.txt",
                         ".hidden", "line\nbreak.txt",
                         "carriage\rreturn.txt"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    join_test_path(path, sizeof(path), directory, names[i]);
    write_text_file(path, "fixture\n");
  }
  join_test_path(path, sizeof(path), nested, "inside.txt");
  write_text_file(path, "nested\n");
  join_test_path(path, sizeof(path), deeper, "deep.txt");
  write_text_file(path, "deep\n");
  char link_path[TERM_PATH_BUFFER];
  join_test_path(link_path, sizeof(link_path), directory, "linked-dir");
  symlink("nested", link_path);
  join_test_path(link_path, sizeof(link_path), directory, "linked-file.txt");
  join_test_path(path, sizeof(path), directory, "alpha.txt");
  symlink(path, link_path);

  CompletionResult result = {0};
  char *prefix = NULL;
  const char *line = "cat bet";
  int scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int exact_prefix = scan == 0 && result.count == 1 && prefix &&
                     !strcmp(prefix, "bet") &&
                     !strcmp(result.items[0], "beta.txt");
  report_test("C019", "filename completion", "ordinary prefix selects one filename",
              line, "beta.txt with decoded prefix bet",
              exact_prefix ? result.items[0] : "candidate or prefix differed",
              exact_prefix, "");
  free(prefix);
  completion_free(&result);

  line = "cat betX";
  scan = completion_find(line, 7, directory, &result, &prefix);
  char editable[64] = "cat betX";
  size_t editable_length = strlen(editable);
  size_t cursor = 7;
  int middle_cursor = scan == 0 && result.count == 1 && prefix &&
                      !strcmp(prefix, "bet") &&
                      completion_insert_text(editable, sizeof(editable),
                                             &editable_length, &cursor,
                                             "a.txt") == 0 &&
                      !strcmp(editable, "cat beta.txtX");
  report_test("C020", "filename completion", "cursor in the middle preserves following characters",
              "cat bet|X; insert .txt", "cat beta.txtX",
              editable, middle_cursor, "Completion scans only up to the cursor.");
  free(prefix);
  completion_free(&result);

  line = "cat|bet";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int pipe_boundary = scan == 0 && result.count == 1 && result.token_start == 4 &&
                      !strcmp(prefix, "bet");
  report_test("C021", "filename completion", "pipe operator starts a new word",
              line, "token begins after | and prefix is bet",
              pipe_boundary ? "operator boundary recognized" : "wrong token boundary",
              pipe_boundary, "");
  free(prefix);
  completion_free(&result);

  line = "echo done;bet";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int semicolon_boundary = scan == 0 && result.count == 1 &&
                           result.token_start == strlen("echo done;") &&
                           !strcmp(prefix, "bet");
  report_test("C022", "filename completion", "semicolon operator starts a new word",
              line, "prefix bet after semicolon", semicolon_boundary
                       ? "operator boundary recognized" : "wrong token boundary",
              semicolon_boundary, "");
  free(prefix);
  completion_free(&result);

  line = "cat nested/in";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int nested_path = scan == 0 && result.count == 1 &&
                    !strcmp(result.items[0], "nested/inside.txt");
  report_test("C023", "filename completion", "nested directory prefix is retained",
              line, "nested/inside.txt", nested_path ? result.items[0]
                                                        : "nested entry missing",
              nested_path, "");
  free(prefix);
  completion_free(&result);

  line = "cat ./nested/in";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int dot_relative = scan == 0 && result.count == 1 &&
                     !strcmp(result.items[0], "./nested/inside.txt");
  report_test("C024", "filename completion", "./ relative path is completed",
              line, "./nested/inside.txt", dot_relative ? result.items[0]
                                                          : "relative entry missing",
              dot_relative, "");
  free(prefix);
  completion_free(&result);

  line = "cat ../alpha";
  scan = completion_find(line, strlen(line), nested, &result, &prefix);
  int parent_relative = scan == 0 && result.count == 1 &&
                        !strcmp(result.items[0], "../alpha.txt");
  report_test("C025", "filename completion", "../ path uses the active directory",
              line, "../alpha.txt", parent_relative ? result.items[0]
                                                     : "parent entry missing",
              parent_relative, "");
  free(prefix);
  completion_free(&result);

  line = "cat linked-dir/in";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int linked_directory = scan == 0 && result.count == 1 &&
                         !strcmp(result.items[0], "linked-dir/inside.txt");
  report_test("C026", "filename completion", "symlinked directory is traversed",
              line, "linked-dir/inside.txt", linked_directory ? result.items[0]
                                                               : "symlink directory missing",
              linked_directory, "stat follows the directory symlink.");
  free(prefix);
  completion_free(&result);

  line = "cat linked-file";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int linked_file = scan == 0 && result.count == 1 &&
                    !strcmp(result.items[0], "linked-file.txt");
  report_test("C027", "filename completion", "symlinked file is offered",
              line, "linked-file.txt", linked_file ? result.items[0]
                                                    : "symlink file missing",
              linked_file, "");
  free(prefix);
  completion_free(&result);

  line = "cat semi\\;c";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int escaped_operator = scan == 0 && result.count == 1 &&
                         !strcmp(prefix, "semi;c") &&
                         !strcmp(result.items[0], "semi;colon.txt");
  report_test("C028", "filename completion", "escaped operator stays within the filename",
              line, "semi;colon.txt", escaped_operator ? result.items[0]
                                                        : "escaped name was split",
              escaped_operator, "");
  free(prefix);
  completion_free(&result);

  line = "cat 'quote";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  char *single_quoted_suffix = result.count
                                   ? completion_encode_suffix(
                                         result.items[0] + strlen(prefix),
                                         result.quote)
                                   : NULL;
  int quote_name = scan == 0 && result.count == 1 &&
                   !strcmp(result.items[0], "quote'file.txt") &&
                   single_quoted_suffix &&
                   !strcmp(single_quoted_suffix, "'\\''file.txt");
  report_test("C029", "filename completion", "apostrophe filename completes inside an open quote",
              line, "quote break and reopen the single-quoted string",
              single_quoted_suffix ? single_quoted_suffix : "no candidate",
              quote_name, "");
  free(single_quoted_suffix);
  free(prefix);
  completion_free(&result);

  line = "cat semi;colon";
  scan = completion_find(line, strlen("cat semi"), directory, &result, &prefix);
  int suffix_cursor = scan == 0 && result.count == 1 &&
                      !strcmp(result.items[0], "semi;colon.txt");
  report_test("C030", "filename completion", "cursor before operator suffix sees the partial filename",
              "cat semi|;colon", "matches semi;colon.txt using prefix semi",
              suffix_cursor ? "candidate found" : "candidate missing",
              suffix_cursor, "Text after the cursor is preserved by the GUI insert helper.");
  free(prefix);
  completion_free(&result);

  line = "cat line";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int newline_skipped = scan == 0 && result.count == 0;
  report_test("C031", "filename completion", "filename containing a newline is omitted",
              line, "no unsafe multi-line candidate", newline_skipped
                       ? "candidate omitted" : "newline filename was listed",
              newline_skipped, "The shell editor accepts one command line at a time.");
  free(prefix);
  completion_free(&result);

  line = "cat nested/";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int trailing_slash = scan == 0 && result.count == 2 &&
                       !strcmp(result.items[0], "nested/deeper/") &&
                       !strcmp(result.items[1], "nested/inside.txt");
  report_test("C032", "filename completion", "trailing slash scans the selected directory",
              line, "deep directory and file listed in sorted order",
              trailing_slash ? "both children listed" : "children differed",
              trailing_slash, "Directories retain a slash for continued completion.");
  free(prefix);
  completion_free(&result);

  line = "cat beta";
  scan = completion_find(line, strlen(line) + 20, directory, &result, &prefix);
  int cursor_clamped = scan == 0 && result.count == 1 &&
                       !strcmp(result.items[0], "beta.txt");
  report_test("C033", "filename completion", "cursor beyond line end is clamped",
              "cat beta; cursor=length+20", "beta.txt candidate", cursor_clamped
                       ? "cursor clamped safely" : "unexpected result",
              cursor_clamped, "");
  free(prefix);
  completion_free(&result);

  line = "cat " ;
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int sorted = scan == 0 && result.count >= 8;
  for (size_t i = 1; sorted && i < result.count; i++)
    if (strcmp(result.items[i - 1], result.items[i]) > 0)
      sorted = 0;
  for (size_t i = 0; i < result.count; i++)
    if (strchr(result.items[i], '\n') || result.items[i][0] == '.')
      sorted = 0;
  report_test("C034", "filename completion", "empty prefix lists visible candidates in order",
              line, "non-hidden, single-line names sorted lexicographically",
              sorted ? "candidates sorted and safe" : "candidate order or visibility differed",
              sorted, "This is the list used by the multiple-match chooser.");
  free(prefix);
  completion_free(&result);

  char *common_inputs[] = {"cat", "dog", "dot"};
  char *common_empty = completion_common_prefix(common_inputs, 3);
  char *common_single = completion_common_prefix(common_inputs, 1);
  char *common_zero = completion_common_prefix(common_inputs, 0);
  int common_edges = common_empty && !*common_empty && common_single &&
                     !strcmp(common_single, "cat") && common_zero && !*common_zero;
  report_test("C035", "filename completion", "common-prefix helper handles zero and one item",
              "common prefix of {cat,dog,dot}, {cat}, and empty list",
              "empty, cat, and empty strings", common_edges ? "all edge results match"
                                                             : "a result differed",
              common_edges, "");
  free(common_empty);
  free(common_single);
  free(common_zero);

  CompletionResult invalid_result = {(char **)1, 99, 99, 99, 'x'};
  char *invalid_prefix = (char *)1;
  errno = 0;
  int invalid_arguments = completion_find(NULL, 0, directory, &invalid_result,
                                          &invalid_prefix) < 0 && errno == EINVAL &&
                          invalid_result.items == NULL &&
                          invalid_result.count == 0 &&
                          invalid_result.token_start == 0 &&
                          invalid_prefix == NULL;
  report_test("C036", "filename completion", "null command line is rejected",
              "completion_find(NULL,...)", "returns -1 and sets EINVAL",
              invalid_arguments ? "EINVAL" : "wrong return or errno",
              invalid_arguments, "");
  free(invalid_prefix);
  completion_free(&invalid_result);

  char invalid_line[16] = "abc";
  size_t invalid_length = 3;
  size_t invalid_cursor = 4;
  errno = 0;
  int invalid_insert = completion_insert_text(invalid_line, sizeof(invalid_line),
                                             &invalid_length, &invalid_cursor,
                                             "x") < 0 && errno == EINVAL &&
                       !strcmp(invalid_line, "abc");
  report_test("C037", "filename completion", "cursor past input length is rejected",
              "insert at cursor 4 into a 3-byte line", "error with line unchanged",
              invalid_insert ? "rejected without changing input" : "input changed",
              invalid_insert, "");

  char selected_line[32] = "ab";
  size_t selected_length = 2;
  size_t selected_cursor = 2;
  char *one_choice[] = {"a"};
  errno = 0;
  int invalid_prefix_length = completion_select(
                                  selected_line, sizeof(selected_line),
                                  &selected_length, &selected_cursor, one_choice,
                                  1, 1, 2, 0) < 0 && errno == EINVAL &&
                              !strcmp(selected_line, "ab");
  report_test("C038", "filename completion", "selection rejects longer prefix than candidate",
              "choose a with two-byte prefix", "error without altering command",
              invalid_prefix_length ? "selection rejected" : "selection changed input",
              invalid_prefix_length, "");

  line = "cat carriage";
  scan = completion_find(line, strlen(line), directory, &result, &prefix);
  int carriage_return_skipped = scan == 0 && result.count == 0;
  report_test("C039", "filename completion", "filename containing CR is omitted",
              line, "no candidate that shell tokenization would split",
              carriage_return_skipped ? "candidate omitted" : "CR filename was listed",
              carriage_return_skipped, "Carriage return is shell whitespace here.");
  free(prefix);
  completion_free(&result);
}

static void test_completion_prefix_matrix(void) {
  char directory[TERM_PATH_BUFFER];
  join_test_path(directory, sizeof(directory), test_root, "completion-edge");
  const char *names[] = {"alpha.txt", "albatross", "beta.txt", "space file.txt",
                         "semi;colon.txt", "quote'file.txt", "$cash.txt",
                         ".hidden"};
  int test_number = 40;
  for (size_t name_index = 0; name_index < sizeof(names) / sizeof(names[0]);
       name_index++) {
    size_t name_length = strlen(names[name_index]);
    size_t first_prefix = names[name_index][0] == '.' ? 1 : 0;
    for (size_t prefix_length = first_prefix; prefix_length <= name_length;
         prefix_length++) {
      char decoded[TERM_PATH_BUFFER];
      memcpy(decoded, names[name_index], prefix_length);
      decoded[prefix_length] = '\0';
      char *encoded = completion_encode_suffix(decoded, 0);
      char command[TERM_PATH_BUFFER * 2];
      snprintf(command, sizeof(command), "cat %s", encoded ? encoded : "");
      CompletionResult result = {0};
      char *actual_prefix = NULL;
      int scan = encoded ? completion_find(command, strlen(command), directory,
                                           &result, &actual_prefix)
                         : -1;
      int found_candidate = 0;
      for (size_t i = 0; i < result.count; i++)
        if (!strcmp(result.items[i], names[name_index]))
          found_candidate = 1;
      int prefix_ok = scan == 0 && found_candidate && actual_prefix &&
                      !strcmp(actual_prefix, decoded);
      char id[16];
      snprintf(id, sizeof(id), "C%03d", test_number++);
      char scenario[96];
      snprintf(scenario, sizeof(scenario), "prefix length %zu of %s",
               prefix_length, names[name_index]);
      report_test(id, "filename completion", scenario,
                  command, "candidate remains available and decoded prefix matches",
                  prefix_ok ? "candidate found" : "candidate or prefix differed",
                  prefix_ok,
                  "Generated prefixes include spaces, quotes, operators, and expansion characters.");
      free(encoded);
      free(actual_prefix);
      completion_free(&result);
    }
  }
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
      hotkeys_find_action(&config, XK_C, ControlMask | ShiftMask) == HOTKEY_COPY &&
      hotkeys_find_action(&config, XK_v, ControlMask | ShiftMask) == HOTKEY_PASTE &&
      hotkeys_find_action(&config, XK_c, ControlMask) == HOTKEY_INTERRUPT &&
      hotkeys_find_action(&config, XK_z, ControlMask) == HOTKEY_SUSPEND &&
      hotkeys_find_action(&config, XK_t, ControlMask) == HOTKEY_NEW_TAB &&
      hotkeys_find_action(&config, XK_r, ControlMask) == HOTKEY_SEARCH_HISTORY;
  report_test("K001", "hotkeys", "assignment defaults are present",
              "all configured default actions", "every key maps to its required action",
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
                  "search_history = Alt+R\n"
                  "unknown = F12\nline_end = NotARealKey\n");
  char warnings[1024];
  int load_result = hotkeys_load_user_config(&config, warnings, sizeof(warnings));
  int config_ok = load_result > 0 &&
                  hotkeys_find_action(&config, XK_Left, Mod1Mask) ==
                      HOTKEY_MOVE_LEFT &&
                  hotkeys_find_action(&config, XK_Left, 0) == HOTKEY_NONE &&
                  hotkeys_find_action(&config, XK_e, ControlMask) ==
                      HOTKEY_LINE_END &&
                  hotkeys_find_action(&config, XK_r, Mod1Mask) ==
                      HOTKEY_SEARCH_HISTORY &&
                  hotkeys_find_action(&config, XK_r, ControlMask) == HOTKEY_NONE &&
                  contains(warnings, "unknown action") &&
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

struct TestWorker {
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
};

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

static int test_early_pipe_reader(int use_thread) {
  SharedHistory *history = new_worker_history(use_thread);
  SharedForeground *foreground = new_foreground();
  if (!history || !foreground) {
    if (history)
      munmap(history, sizeof(*history));
    if (foreground)
      munmap(foreground, sizeof(*foreground));
    return 0;
  }

  pid_t child = fork();
  if (child == 0) {
    signal(SIGPIPE, SIG_DFL);
    alarm(12);
    TestWorker worker;
    worker_init(&worker);
    int started = worker_start(&worker, use_thread, history, foreground,
                               shell_directory, NULL, 0) == 0;
    char output[256];
    WorkerStatus status;
    int command_ok = started &&
                     worker_command(&worker,
                                    "/usr/bin/yes | /usr/bin/head -n 1",
                                    output, sizeof(output), &status) == 0 &&
                     status.status == 0 && !strcmp(output, "y\n");
    if (!command_ok) {
      int process_group = atomic_load(&foreground->pgid);
      if (process_group > 0)
        kill(-process_group, SIGKILL);
    }
    if (started)
      worker_stop(&worker);
    _exit(command_ok ? 0 : 1);
  }

  int finished = 0;
  int status = 0;
  for (int elapsed = 0; child > 0 && elapsed < 140; elapsed++) {
    pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      finished = 1;
      break;
    }
    wait_milliseconds(100);
  }
  if (child > 0 && !finished) {
    int process_group = atomic_load(&foreground->pgid);
    if (process_group > 0)
      kill(-process_group, SIGKILL);
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
  }
  sem_destroy(&history->semaphore);
  munmap(history, sizeof(*history));
  munmap(foreground, sizeof(*foreground));
  return finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int worker_wait_output(TestWorker *worker, const char *first,
                              const char *second, const char *third,
                              char *capture, size_t capacity,
                              size_t minimum_occurrences, int timeout_ms) {
  size_t length = 0;
  capture[0] = '\0';
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    if (count_occurrences(capture, first) >= minimum_occurrences &&
        (!second || count_occurrences(capture, second) >= minimum_occurrences) &&
        (!third || count_occurrences(capture, third) >= minimum_occurrences))
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
                  count_occurrences(capture, second) >= minimum_occurrences) &&
                 (!third ||
                  count_occurrences(capture, third) >= minimum_occurrences)
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
  char history_name[64];
  snprintf(history_name, sizeof(history_name), "worker-history-%u",
           history_test_number++);
  if (set_test_history_path(history_name) < 0)
    return NULL;
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

  result = worker_command(&workers[0], "/usr/bin/printf '%s\\n' 'mode works'",
                          output, sizeof(output), &status);
  int mode_command_ok = result == 0 && status.status == 0 &&
                        !strcmp(output, "mode works\n");
  report_test(use_thread ? "W006" : "W106",
              use_thread ? "Task 2: command execution (thread mode)"
                         : "Task 2: command execution (process mode)",
              "quoted external command runs through this mode",
              "/usr/bin/printf '%s\\n' 'mode works'",
              "prints one quoted argument and status 0",
              mode_command_ok ? output : "command failed or output differed",
              mode_command_ok, "");

  worker_command(&workers[0], "/usr/bin/false", output, sizeof(output),
                 &status);
  int false_status = status.status == 1;
  result = worker_command(&workers[0], "/usr/bin/printf '%s\\n' \"$?\"",
                          output, sizeof(output), &status);
  int last_status_ok = false_status && result == 0 && status.status == 0 &&
                       !strcmp(output, "1\n");
  report_test(use_thread ? "W016" : "W116",
              use_thread ? "Task 2: last exit status (thread mode)"
                         : "Task 2: last exit status (process mode)",
              "$? retains the previous external command status",
              "false; printf '%s\\n' \"$?\"", "prints 1",
              last_status_ok ? output : "status differed", last_status_ok, "");

  char source_path[TERM_PATH_BUFFER];
  char source_command[TERM_LINE_MAX];
  snprintf(source_path, sizeof(source_path), "%s/source-%s.commands",
           test_root, mode);
  write_text_file(source_path, "/usr/bin/printf sourced-mode\n");
  snprintf(source_command, sizeof(source_command), "source %s", source_path);
  result = worker_command(&workers[0], source_command, output, sizeof(output),
                          &status);
  int source_mode_ok = result == 0 && status.status == 0 &&
                       contains(output, "sourced-mode");
  report_test(use_thread ? "W017" : "W117",
              use_thread ? "Task 2: source builtin (thread mode)"
                         : "Task 2: source builtin (process mode)",
              "source runs a command file in this tab's shell context",
              source_command, "sourced-mode output and status 0",
              source_mode_ok ? output : "source command failed",
              source_mode_ok, "");

  result = worker_command(&workers[0],
                          "/usr/bin/printf '%s\\n' \"first\nsecond\"",
                          output, sizeof(output), &status);
  int multiline_mode_ok = result == 0 && status.status == 0 &&
                          !strcmp(output, "first\nsecond\n");
  report_test(use_thread ? "W007" : "W107",
              use_thread ? "Task 3: multiline quoting (thread mode)"
                         : "Task 3: multiline quoting (process mode)",
              "worker keeps collecting input until a quoted command is complete",
              "printf with a quoted argument spanning two lines",
              "both lines print and worker returns status 0",
              multiline_mode_ok ? output : "multiline command failed",
              multiline_mode_ok, "");

  char mode_redirect_path[TERM_PATH_BUFFER];
  char mode_redirect_command[TERM_LINE_MAX];
  snprintf(mode_redirect_path, sizeof(mode_redirect_path), "%s/redirect-%s.txt",
           test_root, mode);
  snprintf(mode_redirect_command, sizeof(mode_redirect_command),
           "/usr/bin/printf redirected > %s", mode_redirect_path);
  result = worker_command(&workers[0], mode_redirect_command, output,
                          sizeof(output), &status);
  char *mode_redirect_contents = read_text_file(mode_redirect_path);
  int mode_redirect_ok = result == 0 && status.status == 0 &&
                         mode_redirect_contents &&
                         !strcmp(mode_redirect_contents, "redirected");
  report_test(use_thread ? "W008" : "W108",
              use_thread ? "Task 4: redirection (thread mode)"
                         : "Task 4: redirection (process mode)",
              "worker command writes output to a file",
              mode_redirect_command, "file contains redirected",
              mode_redirect_contents ? mode_redirect_contents : "no file",
              mode_redirect_ok, "");
  free(mode_redirect_contents);

  char mode_input_path[512];
  char mode_copy_path[512];
  char mode_copy_command[TERM_LINE_MAX];
  snprintf(mode_input_path, sizeof(mode_input_path), "%s/input-%s.txt",
           test_root, mode);
  snprintf(mode_copy_path, sizeof(mode_copy_path), "%s/copy-%s.txt",
           test_root, mode);
  write_text_file(mode_input_path, "input-data\n");
  snprintf(mode_copy_command, sizeof(mode_copy_command),
           "/bin/cat < %s > %s", mode_input_path, mode_copy_path);
  result = worker_command(&workers[0], mode_copy_command, output,
                          sizeof(output), &status);
  char *mode_copy_contents = read_text_file(mode_copy_path);
  int input_output_mode_ok = result == 0 && status.status == 0 &&
                             mode_copy_contents &&
                             !strcmp(mode_copy_contents, "input-data\n");
  report_test(use_thread ? "W018" : "W118",
              use_thread ? "Task 4: input/output redirection (thread mode)"
                         : "Task 4: input/output redirection (process mode)",
              "command reads one file and writes another",
              mode_copy_command, "output file matches input file",
              mode_copy_contents ? mode_copy_contents : "no output file",
              input_output_mode_ok, "");
  free(mode_copy_contents);
  snprintf(mode_copy_command, sizeof(mode_copy_command),
           "/usr/bin/printf appended >> %s", mode_copy_path);
  result = worker_command(&workers[0], mode_copy_command, output,
                          sizeof(output), &status);
  mode_copy_contents = read_text_file(mode_copy_path);
  int append_mode_ok = result == 0 && status.status == 0 &&
                       mode_copy_contents &&
                       !strcmp(mode_copy_contents, "input-data\nappended");
  report_test(use_thread ? "W019" : "W119",
              use_thread ? "Task 4: append redirection (thread mode)"
                         : "Task 4: append redirection (process mode)",
              "append preserves existing file bytes",
              mode_copy_command, "file has the original and appended bytes",
              mode_copy_contents ? mode_copy_contents : "no output file",
              append_mode_ok, "");
  free(mode_copy_contents);

  result = worker_command(&workers[0],
                         "/usr/bin/printf fail > /tmp", output,
                         sizeof(output), &status);
  int bad_redirection_ok = result == 0 && status.status != 0 &&
                           contains(output, "/tmp");
  report_test(use_thread ? "W013" : "W113",
              use_thread ? "Task 4: invalid output path (thread mode)"
                         : "Task 4: invalid output path (process mode)",
              "redirection to a directory reports failure without crashing",
              "/usr/bin/printf fail > /tmp",
              "nonzero status and readable diagnostic",
              output, bad_redirection_ok,
              "Uses a directory path to exercise the open failure path.");

  result = worker_command(
      &workers[0],
      "/usr/bin/printf alpha | /usr/bin/tr a-z A-Z | /usr/bin/cat",
      output, sizeof(output), &status);
  int mode_pipeline_ok = result == 0 && status.status == 0 &&
                         !strcmp(output, "ALPHA");
  report_test(use_thread ? "W009" : "W109",
              use_thread ? "Task 5: pipeline (thread mode)"
                         : "Task 5: pipeline (process mode)",
              "three concurrently connected pipeline stages",
              "printf alpha | tr a-z A-Z | cat", "prints ALPHA",
              mode_pipeline_ok ? output : "pipeline failed or output differed",
              mode_pipeline_ok, "");

  int closed_reader_ok = test_early_pipe_reader(use_thread);
  report_test(use_thread ? "W012" : "W112",
              use_thread ? "Task 5: early pipe reader exit (thread mode)"
                         : "Task 5: early pipe reader exit (process mode)",
              "writer receives SIGPIPE after head closes its input",
              "yes | head -n 1", "pipeline finishes with one line",
              closed_reader_ok ? "one line; pipeline completed"
                               : "pipeline hung, crashed, or failed",
              closed_reader_ok,
              "Checks that unused pipe ends close and the worker survives SIGPIPE.");

  result = worker_command(&workers[0], "/bin/sh -c 'kill -SEGV $$'", output,
                          sizeof(output), &status);
  int command_crashed = result == 0 && status.status != 0;
  int recovery_result =
      worker_command(&workers[0], "/usr/bin/printf recovered", output,
                     sizeof(output), &status);
  int crash_recovery_ok = command_crashed && recovery_result == 0 &&
                          status.status == 0 && contains(output, "recovered");
  report_test(use_thread ? "W010" : "W110",
              use_thread ? "Task 2: crash recovery (thread mode)"
                         : "Task 2: crash recovery (process mode)",
              "a command killed by SIGSEGV does not kill its tab worker",
              "/bin/sh -c 'kill -SEGV $$'; then printf recovered",
              "failed command returns; following command succeeds",
              crash_recovery_ok ? "worker recovered after SIGSEGV"
                                : "worker did not recover",
              crash_recovery_ok,
              "The disposable command child receives SIGSEGV; the worker must remain alive.");

  result = worker_command(&workers[0], "/terminator-command-does-not-exist",
                          output, sizeof(output), &status);
  int missing_command_ok = result == 0 && status.status == 127 &&
                           contains(output, "command not found");
  report_test(use_thread ? "W014" : "W114",
              use_thread ? "Task 2: unknown command (thread mode)"
                         : "Task 2: unknown command (process mode)",
              "exec failure is reported and the worker stays alive",
              "/terminator-command-does-not-exist",
              "status 127 and command-not-found diagnostic",
              missing_command_ok ? "status 127 reported"
                                 : "missing-command handling differed",
              missing_command_ok, "");
  int missing_recovery = worker_command(
                             &workers[0], "/usr/bin/printf after-missing",
                             output, sizeof(output), &status) == 0 &&
                         status.status == 0 &&
                         contains(output, "after-missing");
  report_test(use_thread ? "W015" : "W115",
              use_thread ? "Task 2: recovery after exec failure (thread mode)"
                         : "Task 2: recovery after exec failure (process mode)",
              "worker accepts a valid command after an exec failure",
              "/usr/bin/printf after-missing",
              "status 0 and output is present",
              missing_recovery ? output : "worker did not recover",
              missing_recovery, "");

  if (!use_thread) {
    int killed = workers[0].pid > 0 &&
                 kill(workers[0].pid, SIGKILL) == 0;
    int isolated_result =
        worker_command(&workers[1], "/usr/bin/printf other-tab-alive", output,
                       sizeof(output), &status);
    int isolated_ok = killed && isolated_result == 0 && status.status == 0 &&
                      contains(output, "other-tab-alive");
    report_test("W111", "Task 1: process tab crash isolation",
                "one tab worker is killed while a second tab remains usable",
                "SIGKILL tab 1 worker; run printf in tab 2",
                "tab 2 returns output and status 0",
                isolated_ok ? "second process worker remains usable"
                            : "another tab was affected",
                isolated_ok,
                "Task 1 explicitly requires a process-mode tab crash not to affect other tabs.");
    worker_stop(&workers[0]);
  }

  worker_stop(&workers[0]);
  worker_stop(&workers[1]);
  sem_destroy(&history->semaphore);
  munmap(history, sizeof(*history));
  munmap(foreground0, sizeof(*foreground0));
  munmap(foreground1, sizeof(*foreground1));
}

static void test_shell_features(void) {
  set_test_history_path("history-shell");
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

  snprintf(redir, sizeof(redir), "less %s", input_file);
  output = run_line(&shell, redir, &status);
  int less_file_ok = status == 0 && output && !strcmp(output, "zeta\nalpha\n");
  report_test("E030", "built-in less", "display a relative or absolute file",
              redir, "file bytes appear in output", output ? output : "no output",
              less_file_ok, "The output is kept in Terminator scrollback.");
  free(output);

  output = run_line(&shell,
                    "/usr/bin/printf 'pipe-data\\n' | less", &status);
  int less_pipe_ok = status == 0 && output && !strcmp(output, "pipe-data\n");
  report_test("E031", "built-in less", "read piped standard input",
              "printf pipe-data | less", "pipe-data reaches the viewer",
              output ? output : "no output", less_pipe_ok,
              "This works without a PTY because less copies stdin to stdout.");
  free(output);

  char second_input[256];
  char empty_input[256];
  join_test_path(second_input, sizeof(second_input), shell_directory,
                 "second-less.txt");
  join_test_path(empty_input, sizeof(empty_input), shell_directory,
                 "empty-less.txt");
  write_text_file(second_input, "second\n");
  write_text_file(empty_input, "");
  snprintf(redir, sizeof(redir), "less %s %s", input_file, second_input);
  output = run_line(&shell, redir, &status);
  int less_multiple_ok = status == 0 && output && contains(output, "zeta\n") &&
                         contains(output, "second\n") &&
                         contains(output, "==>");
  report_test("E032", "built-in less", "display more than one file with headers",
              "less file-one file-two", "both files and headers appear",
              output ? output : "no output", less_multiple_ok, "");
  free(output);

  snprintf(redir, sizeof(redir), "less %s", empty_input);
  output = run_line(&shell, redir, &status);
  int less_empty_ok = status == 0 && output && !*output;
  report_test("E033", "built-in less", "empty file is a successful empty display",
              redir, "zero output and status 0", output ? output : "no output",
              less_empty_ok, "");
  free(output);

  snprintf(redir, sizeof(redir), "less %s/missing-less.txt", shell_directory);
  output = run_line(&shell, redir, &status);
  int less_missing_ok = status != 0 && output && contains(output, "less:") &&
                        contains(output, "missing-less.txt");
  report_test("E034", "built-in less", "missing file reports an error",
              redir, "nonzero status and file name in diagnostic",
              output ? output : "no output", less_missing_ok, "");
  free(output);

  snprintf(redir, sizeof(redir), "less %s > %s", input_file, output_file);
  output = run_line(&shell, redir, &status);
  free(output);
  char *less_redirect_contents = read_text_file(output_file);
  int less_redirect_ok = status == 0 && less_redirect_contents &&
                         !strcmp(less_redirect_contents, "zeta\nalpha\n");
  report_test("E035", "built-in less", "file output redirection still works",
              "less input.txt > output.txt", "viewer bytes are redirected to file",
              less_redirect_contents ? less_redirect_contents : "file missing",
              less_redirect_ok, "");
  free(less_redirect_contents);

  char large_input[TERM_PATH_BUFFER];
  join_test_path(large_input, sizeof(large_input), shell_directory,
                 "large-less.txt");
  FILE *large_file = fopen(large_input, "w");
  if (large_file) {
    for (int i = 0; i < 5; i++)
      for (int byte = 0; byte < 8192; byte++)
        fputc('x', large_file);
    fclose(large_file);
  }
  snprintf(redir, sizeof(redir), "less %s", large_input);
  output = run_line(&shell, redir, &status);
  int less_large_ok = status == 0 && output && strlen(output) == 40960;
  report_test("E036", "built-in less", "stream output larger than one read buffer",
              "display a 40 KiB file", "all bytes reach output",
              output ? "captured output length checked" : "no output",
              less_large_ok, "Exercises multiple read/write iterations.");
  free(output);

  output = run_line(&shell, "less --help", &status);
  int less_help_ok = status == 0 && output && contains(output, "Usage: less") &&
                     contains(output, "PageUp");
  report_test("E037", "built-in less", "help documents scrollback controls",
              "less --help", "usage and scrolling guidance are printed",
              output ? output : "no output", less_help_ok, "");
  free(output);

  output = run_line(&shell, "/usr/bin/printf 'dash-input\\n' | less -", &status);
  int less_dash_ok = status == 0 && output && !strcmp(output, "dash-input\n");
  report_test("E038", "built-in less", "dash reads standard input",
              "printf dash-input | less -", "stdin bytes are copied to stdout",
              output ? output : "no output", less_dash_ok, "");
  free(output);

  snprintf(redir, sizeof(redir), "less < %s", input_file);
  output = run_line(&shell, redir, &status);
  int less_input_redirect_ok = status == 0 && output &&
                               !strcmp(output, "zeta\nalpha\n");
  report_test("E039", "built-in less", "input redirection supplies file contents",
              redir, "input bytes appear in scrollback output",
              output ? output : "no output", less_input_redirect_ok, "");
  free(output);

  char append_less[TERM_PATH_BUFFER];
  join_test_path(append_less, sizeof(append_less), shell_directory,
                 "append-less.txt");
  write_text_file(append_less, "existing\n");
  snprintf(redir, sizeof(redir), "less %s >> %s", input_file, append_less);
  output = run_line(&shell, redir, &status);
  free(output);
  char *append_contents = read_text_file(append_less);
  int less_append_ok = status == 0 && append_contents &&
                       !strcmp(append_contents, "existing\nzeta\nalpha\n");
  report_test("E040", "built-in less", "append redirection preserves earlier file contents",
              "less input.txt >> append-less.txt",
              "old bytes followed by viewer bytes",
              append_contents ? append_contents : "output file missing",
              less_append_ok, "");
  free(append_contents);

  snprintf(redir, sizeof(redir), "less %s %s/missing-middle %s",
           input_file, shell_directory, second_input);
  output = run_line(&shell, redir, &status);
  int less_continue_after_error = status != 0 && output &&
                                  contains(output, "zeta\n") &&
                                  contains(output, "second\n") &&
                                  contains(output, "missing-middle");
  report_test("E041", "built-in less", "later files are still read after one file fails",
              "less valid missing valid", "both valid contents plus an error",
              output ? output : "no output", less_continue_after_error,
              "A missing file reports failure but does not stop the remaining file list.");
  free(output);

  snprintf(redir, sizeof(redir), "less %s", shell_directory);
  output = run_line(&shell, redir, &status);
  int less_directory_error = status != 0 && output && contains(output, "less:");
  report_test("E042", "built-in less", "directory read failure is reported",
              redir, "nonzero status and less diagnostic",
              output ? output : "no diagnostic", less_directory_error,
              "Opening a directory may succeed; reading it fails on Linux.");
  free(output);

  char binary_input[TERM_PATH_BUFFER];
  join_test_path(binary_input, sizeof(binary_input), shell_directory,
                 "binary-less.bin");
  int binary_fd = open(binary_input, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  const unsigned char binary_bytes[] = {0, 1, 0xff, '\n', 'z'};
  int binary_written = binary_fd >= 0 &&
                       write(binary_fd, binary_bytes, sizeof(binary_bytes)) ==
                           (ssize_t)sizeof(binary_bytes);
  if (binary_fd >= 0)
    close(binary_fd);
  snprintf(redir, sizeof(redir), "less %s | /usr/bin/wc -c", binary_input);
  output = run_line(&shell, redir, &status);
  int binary_less_ok = binary_written && status == 0 && output &&
                       atoi(output) == (int)sizeof(binary_bytes);
  report_test("E043", "built-in less", "binary bytes are copied without string processing",
              redir, "wc reports five bytes including NUL and 0xff",
              output ? output : "no output", binary_less_ok,
              "The test checks byte count because captured C strings cannot display NUL.");
  free(output);

  char megabyte_input[TERM_PATH_BUFFER];
  join_test_path(megabyte_input, sizeof(megabyte_input), shell_directory,
                 "megabyte-less.bin");
  int megabyte_fd = open(megabyte_input, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  char block[8192];
  memset(block, 'm', sizeof(block));
  int megabyte_written = megabyte_fd >= 0;
  for (int block_index = 0; megabyte_written && block_index < 128;
       block_index++)
    megabyte_written = write_all(megabyte_fd, block, sizeof(block)) == 0;
  if (megabyte_fd >= 0)
    close(megabyte_fd);
  snprintf(redir, sizeof(redir), "less %s | /usr/bin/wc -c", megabyte_input);
  output = run_line(&shell, redir, &status);
  int large_pipeline_ok = megabyte_written && status == 0 && output &&
                          atoi(output) == 1024 * 1024;
  report_test("E044", "built-in less / pipeline", "megabyte stream drains through a bounded pipe",
              "less 1 MiB file | wc -c", "exactly 1,048,576 bytes reach wc",
              output ? output : "no output", large_pipeline_ok,
              "Reader and writer must make progress concurrently beyond pipe capacity.");
  free(output);

  snprintf(redir, sizeof(redir),
           "/usr/bin/printf 'one\\ntwo\\n' | less | /usr/bin/tr a-z A-Z | /usr/bin/wc -l");
  output = run_line(&shell, redir, &status);
  int less_long_pipeline = status == 0 && output && atoi(output) == 2;
  report_test("E045", "built-in less / pipeline", "less works in a four-stage pipeline",
              "printf | less | tr | wc -l", "two lines reach the final command",
              output ? output : "no output", less_long_pipeline,
              "Checks that the viewer reads its pipe and forwards every line.");
  free(output);

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
  char history_value[64];
  for (int i = 0; i < TERM_HISTORY_DISPLAY + 5; i++) {
    snprintf(history_value, sizeof(history_value), "display-%04d", i);
    history_add(history, history_value);
  }
  output = run_line(&shell, "history", &status);
  int history_builtin_ok = status == 0 && output &&
                           count_occurrences(output, "\n") == TERM_HISTORY_DISPLAY &&
                           contains(output, "display-1004") &&
                           !contains(output, "history seed entry");
  report_test("E013", "built-in history", "history displays only the newest 1,000 commands",
              "history", "1,000 rows, including newest, without oldest seed",
              output ? "history row count and newest command checked" : "no output",
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
    int invalid_job_result =
        worker_command(&worker, "fg %999", output, sizeof(output), &status);
    int invalid_job_ok =
        invalid_job_result == 0 && status.status != 0 && output[0] != '\0';
    report_test(use_thread ? "J007" : "J107",
                use_thread ? "Task 8: invalid job ID (thread mode)"
                           : "Task 8: invalid job ID (process mode)",
                "fg with an unknown job returns an error but keeps worker alive",
                "fg %999", "nonzero status and diagnostic",
                invalid_job_ok ? output : "invalid job was not rejected",
                invalid_job_ok, "");
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
                "multiWatch [\"/usr/bin/printf left\",\"/usr/bin/printf right\",\"/usr/bin/printf third\"]");
    int outputs_ok = worker_wait_output(&worker, "left", "right", "third",
                                        first_run, sizeof(first_run), 2,
                                        4000) == 0;
    int group = wait_for_foreground(foreground, 3000);
    int signalled = group > 0 && kill(-group, SIGINT) == 0;
    int completed = signalled && worker_collect(&worker, output, sizeof(output),
                                                 &status, 4000) == 0;
    int interrupt_ok = completed && status.status == 128 + SIGINT;
    int format_ok = outputs_ok && count_occurrences(first_run, "left") >= 2 &&
                    count_occurrences(first_run, "right") >= 2 &&
                    count_occurrences(first_run, "third") >= 2;
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

    worker_send(
        &worker,
        "multiWatch [\"/bin/sh -c 'kill -SEGV $$'\",\"/usr/bin/printf '\\115\\127\\137\\117\\113\\n'\"]");
    int survivor_ran =
        worker_wait_output(&worker, "MW_OK", NULL, NULL, first_run,
                           sizeof(first_run), 1, 4000) == 0;
    group = wait_for_foreground(foreground, 3000);
    signalled = group > 0 && kill(-group, SIGINT) == 0;
    completed = signalled && worker_collect(&worker, output, sizeof(output),
                                             &status, 4000) == 0;
    int watcher_recovered = survivor_ran && completed &&
                            status.status == 128 + SIGINT;
    report_test(use_thread ? "M105" : "M005",
                use_thread ? "Task 6: multiWatch child crash (thread mode)"
                           : "Task 6: multiWatch child crash (process mode)",
                "a crashing watched child does not stop its sibling or supervisor",
                "watch a SIGSEGV command alongside printf survivor",
                "MW_OK output appears and Ctrl+C returns status 130",
                watcher_recovered ? "watcher stayed active until interrupted"
                                  : "watcher did not recover from child crash",
                watcher_recovered,
                "The test interrupts the watcher as soon as the sibling runs.");
    int after_crash_ok =
        worker_command(&worker, "/usr/bin/printf watch-recovered", output,
                       sizeof(output), &status) == 0 &&
        status.status == 0 && contains(output, "watch-recovered");
    report_test(use_thread ? "M106" : "M006",
                use_thread ? "Task 6: multiWatch worker recovery (thread mode)"
                           : "Task 6: multiWatch worker recovery (process mode)",
                "tab worker accepts a command after the crashing watcher stops",
                "printf watch-recovered", "status 0 and output is present",
                after_crash_ok ? output : "worker did not recover",
                after_crash_ok, "");
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
  report_test("R001", "multiWatch syntax", "compact comma-separated list",
              command, "the bracketed list reaches the builtin as one argument",
              one_argument ? "bracketed list preserved"
                           : "list was split unexpectedly",
              one_argument,
              "The builtin splits its contents into three watched commands.");
  free(error);
  shell_tokens_free(&tokens);
  shell_pipeline_free(&pipeline);
}

static Window find_terminator_window(Display *display) {
  Window root = DefaultRootWindow(display);
  Window parent;
  Window *children = NULL;
  unsigned int child_count = 0;
  if (!XQueryTree(display, root, &root, &parent, &children, &child_count))
    return None;
  Window found = None;
  for (unsigned int i = 0; i < child_count; i++) {
    char *title = NULL;
    if (XFetchName(display, children[i], &title) && title) {
      if (!strcmp(title, "Terminator"))
        found = children[i];
      XFree(title);
    }
    if (found != None)
      break;
  }
  if (children)
    XFree(children);
  return found;
}

static int send_gui_key(Display *display, Window window, KeySym key,
                        unsigned int modifiers) {
  KeyCode code = XKeysymToKeycode(display, key);
  if (!code)
    return -1;
  XEvent event = {0};
  event.xkey.type = KeyPress;
  event.xkey.display = display;
  event.xkey.window = window;
  event.xkey.root = DefaultRootWindow(display);
  event.xkey.time = CurrentTime;
  event.xkey.state = modifiers;
  event.xkey.keycode = code;
  event.xkey.same_screen = True;
  int sent = XSendEvent(display, window, True, KeyPressMask, &event);
  XFlush(display);
  return sent ? 0 : -1;
}

static int send_gui_text(Display *display, Window window, const char *text) {
  for (const unsigned char *character = (const unsigned char *)text; *character;
       character++) {
    KeySym key = *character;
    unsigned int modifiers = 0;
    if (*character >= 'A' && *character <= 'Z') {
      key = *character - 'A' + XK_a;
      modifiers = ShiftMask;
    } else if (*character == '_') {
      key = XK_minus;
      modifiers = ShiftMask;
    } else if (*character == ':') {
      key = XK_semicolon;
      modifiers = ShiftMask;
    } else if (*character == '+') {
      key = XK_equal;
      modifiers = ShiftMask;
    }
    if (*character == ' ')
      key = XK_space;
    if (send_gui_key(display, window, key, modifiers) < 0)
      return -1;
  }
  return 0;
}

static int send_gui_mouse_selection(Display *display, Window window,
                                    int first_x, int first_y, int last_x,
                                    int last_y) {
  XEvent event = {0};
  event.xbutton.type = ButtonPress;
  event.xbutton.display = display;
  event.xbutton.window = window;
  event.xbutton.root = DefaultRootWindow(display);
  event.xbutton.time = CurrentTime;
  event.xbutton.x = first_x;
  event.xbutton.y = first_y;
  event.xbutton.button = Button1;
  event.xbutton.same_screen = True;
  if (!XSendEvent(display, window, True, ButtonPressMask, &event))
    return -1;

  event = (XEvent){0};
  event.xmotion.type = MotionNotify;
  event.xmotion.display = display;
  event.xmotion.window = window;
  event.xmotion.root = DefaultRootWindow(display);
  event.xmotion.time = CurrentTime;
  event.xmotion.x = last_x;
  event.xmotion.y = last_y;
  event.xmotion.state = Button1Mask;
  event.xmotion.same_screen = True;
  if (!XSendEvent(display, window, True, ButtonMotionMask, &event))
    return -1;

  event = (XEvent){0};
  event.xbutton.type = ButtonRelease;
  event.xbutton.display = display;
  event.xbutton.window = window;
  event.xbutton.root = DefaultRootWindow(display);
  event.xbutton.time = CurrentTime;
  event.xbutton.x = last_x;
  event.xbutton.y = last_y;
  event.xbutton.button = Button1;
  event.xbutton.same_screen = True;
  if (!XSendEvent(display, window, True, ButtonReleaseMask, &event))
    return -1;

  XFlush(display);
  return 0;
}

static int read_gui_clipboard(Display *display, Window requestor,
                              char *text, size_t text_size) {
  Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
  Atom property = XInternAtom(display, "TERMINATOR_TEST_CLIPBOARD", False);
  XDeleteProperty(display, requestor, property);
  XConvertSelection(display, clipboard, utf8, property, requestor, CurrentTime);
  XFlush(display);

  XEvent event;
  int notified = 0;
  for (int tries = 0; tries < 300; tries++) {
    XPending(display);
    if (XCheckTypedWindowEvent(display, requestor, SelectionNotify, &event)) {
      notified = 1;
      break;
    }
    wait_milliseconds(10);
  }
  if (!notified || event.xselection.property == None)
    return -1;

  Atom actual_type;
  int format;
  unsigned long item_count, bytes_after;
  unsigned char *data = NULL;
  int status = XGetWindowProperty(display, requestor, property, 0, 4096, True,
                                  AnyPropertyType, &actual_type, &format,
                                  &item_count, &bytes_after, &data);
  if (status != Success || format != 8 || !data) {
    if (data)
      XFree(data);
    return -1;
  }
  size_t length = item_count < text_size - 1 ? (size_t)item_count : text_size - 1;
  memcpy(text, data, length);
  text[length] = '\0';
  XFree(data);
  return 0;
}

/* Wait for the GUI to process Ctrl+Shift+C before checking its clipboard. */
static int wait_for_gui_clipboard(Display *display, Window owner,
                                  Window requestor, const char *expected,
                                  char *text, size_t text_size) {
  Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
  for (int tries = 0; tries < 50; tries++) {
    if (XGetSelectionOwner(display, clipboard) == owner &&
        read_gui_clipboard(display, requestor, text, text_size) == 0 &&
        !strcmp(text, expected))
      return 0;
    wait_milliseconds(10);
  }
  return -1;
}

static int answer_clipboard_request(Display *display, Window owner,
                                    const char *clipboard_text,
                                    int reject_utf8) {
  XEvent event;
  XPending(display);
  if (!XCheckTypedEvent(display, SelectionRequest, &event)) {
    return 0;
  }

  XSelectionRequestEvent *request = &event.xselectionrequest;
  if (request->owner != owner) {
    fprintf(stderr, "clipboard test: request arrived for unexpected owner\n");
    return -1;
  }
  Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
  Atom property = request->property == None ? request->target : request->property;
  XSelectionEvent reply = {0};
  reply.type = SelectionNotify;
  reply.display = display;
  reply.requestor = request->requestor;
  reply.selection = request->selection;
  reply.target = request->target;
  reply.time = request->time;
  reply.property = None;
  if (request->target == utf8 && !reject_utf8) {
    XChangeProperty(display, request->requestor, property, utf8, 8,
                    PropModeReplace,
                    (const unsigned char *)clipboard_text,
                    (int)strlen(clipboard_text));
    reply.property = property;
  } else if (request->target == XA_STRING) {
    Atom type = request->target == XA_STRING ? XA_STRING : utf8;
    XChangeProperty(display, request->requestor, property, type, 8,
                    PropModeReplace,
                    (const unsigned char *)clipboard_text,
                    (int)strlen(clipboard_text));
    reply.property = property;
  } else if (request->target != utf8) {
    return -1;
  }
  XSendEvent(display, request->requestor, False, 0, (XEvent *)&reply);
  XFlush(display);
  return request->target == utf8 && reject_utf8 ? 2 : 1;
}

static int paste_external_command(Display *display, Window window,
                                  Display *clipboard_display,
                                  Window clipboard_window,
                                  const char *history_path, const char *command,
                                  const char *output, int reject_utf8) {
  Atom clipboard = XInternAtom(clipboard_display, "CLIPBOARD", False);
  XSetSelectionOwner(clipboard_display, clipboard, clipboard_window,
                     CurrentTime);
  XFlush(clipboard_display);
  if (XGetSelectionOwner(clipboard_display, clipboard) != clipboard_window) {
    fprintf(stderr, "clipboard paste test: could not own CLIPBOARD\n");
    return -1;
  }
  if (send_gui_key(display, window, XK_v, ControlMask | ShiftMask) < 0) {
    fprintf(stderr, "clipboard paste test: Ctrl+Shift+V send failed\n");
    return -1;
  }
  /* Finish the server-side key delivery before waiting for selection data. */
  XSync(display, False);

  int request_answered = 0;
  int request_seen = 0;
  for (int tries = 0; tries < 300 && !request_answered; tries++) {
    int answer = answer_clipboard_request(clipboard_display, clipboard_window,
                                          command, reject_utf8);
    if (answer < 0) {
      fprintf(stderr, "clipboard paste test: unexpected request target/owner\n");
      return -1;
    }
    if (answer != 0)
      request_seen = 1;
    if (answer == 1)
      request_answered = 1;
    /* Retry once if XSendEvent has not reached the GUI yet. */
    if (!request_seen && (tries == 49 || tries == 149) &&
        send_gui_key(display, window, XK_v, ControlMask | ShiftMask) < 0) {
      fprintf(stderr, "clipboard paste test: retry key send failed\n");
      return -1;
    }
    if (!request_seen && (tries == 49 || tries == 149))
      XSync(display, False);
    wait_milliseconds(10);
  }
  if (!request_answered) {
    fprintf(stderr, "clipboard paste test: selection request timed out\n");
    return -1;
  }

  /* Answer any duplicate requests caused by the bounded key retry. */
  for (int tries = 0; tries < 5; tries++) {
    int answer = answer_clipboard_request(clipboard_display, clipboard_window,
                                          command, reject_utf8);
    if (answer < 0) {
      fprintf(stderr, "clipboard paste test: unexpected retry request\n");
      return -1;
    }
    wait_milliseconds(10);
  }

  wait_milliseconds(100);
  if (send_gui_key(display, window, XK_Return, 0) < 0) {
    fprintf(stderr, "clipboard paste test: Enter send failed\n");
    return -1;
  }
  wait_milliseconds(300);
  char *history = read_text_file(history_path);
  int command_saved = history && contains(history, command);
  free(history);
  int output_visible = gui_find_text_line(display, window, output, -1) >= 0;
  if (!command_saved || !output_visible)
    fprintf(stderr,
            "clipboard paste test: saved=%d output_visible=%d expected=%s\n",
            command_saved, output_visible, command);
  return command_saved && output_visible ? 0 : -1;
}

/* Match the application's font choice so pixel-based GUI checks stay aligned. */
static XFontStruct *load_gui_test_font(Display *display) {
  XFontStruct *font = XLoadQueryFont(display, "10x20");
  if (!font)
    font = XLoadQueryFont(display, "9x15");
  if (!font)
    font = XLoadQueryFont(display, "fixed");
  return font;
}

static int test_gui_clipboard_copy(Display *display, Window window,
                                   Display *clipboard_display,
                                   Window clipboard_window) {
  static const char copied_text[] = "TERMINATOR_COPY_MARKER";
  static const char first_line[] = "COPY one: spaces&and$symbol#!";
  static const char second_line[] = "COPY two: x[y]{z}+";
  static const char multiline_text[] =
      "COPY one: spaces&and$symbol#!\nCOPY two: x[y]{z}+";
  int marker_y = gui_find_text_line(display, window, copied_text, -1);
  int first_y = gui_find_text_line(display, window, first_line, -1);
  int second_y = gui_find_text_line(display, window, second_line, -1);
  XFontStruct *font = load_gui_test_font(display);
  int result = -1;
  if (!font) {
    fprintf(stderr, "clipboard test: test font could not be loaded\n");
    return -1;
  }
  if (marker_y < 0 || first_y < 0 || second_y < 0) {
    fprintf(stderr, "clipboard test: expected copy text was not rendered\n");
    XFreeFont(display, font);
    return -1;
  }

  char clipboard_text[256];
  int marker_right = 12 + XTextWidth(font, copied_text,
                                     (int)strlen(copied_text));
  int marker_text_y = marker_y - font->ascent / 2;
  int marker_selected =
      send_gui_mouse_selection(display, window, 12, marker_text_y, marker_right,
                               marker_text_y) == 0;
  int copied_marker = marker_selected &&
      send_gui_key(display, window, XK_c, ControlMask | ShiftMask) == 0 &&
      wait_for_gui_clipboard(clipboard_display, window, clipboard_window,
                             copied_text, clipboard_text,
                             sizeof(clipboard_text)) == 0;
  XSync(display, False);
  XImage *selection_image =
      copied_marker
          ? XGetImage(display, window, 12, marker_y - font->ascent,
                      (unsigned)(marker_right - 12),
                      (unsigned)(font->ascent + font->descent), AllPlanes,
                      ZPixmap)
          : NULL;
  int visibly_selected = 0;
  if (selection_image) {
    for (int y = 0; y < selection_image->height && !visibly_selected; y++)
      for (int x = 0; x < selection_image->width; x++)
        if (XGetPixel(selection_image, x, y) == 0x85a8d0) {
          visibly_selected = 1;
          break;
        }
    XDestroyImage(selection_image);
  }

  int second_right = 12 + XTextWidth(font, second_line,
                                     (int)strlen(second_line));
  int first_text_y = first_y - font->ascent / 2;
  int second_text_y = second_y - font->ascent / 2;
  int copied_multiline =
      send_gui_mouse_selection(display, window, 12, first_text_y, second_right,
                               second_text_y) == 0 &&
      send_gui_key(display, window, XK_c, ControlMask | ShiftMask) == 0 &&
      wait_for_gui_clipboard(clipboard_display, window, clipboard_window,
                             multiline_text, clipboard_text,
                             sizeof(clipboard_text)) == 0;

  /* Copy remains available while Ctrl+R or the completion picker is active. */
  int copied_during_search =
      copied_multiline && send_gui_key(display, window, XK_r, ControlMask) == 0 &&
      send_gui_key(display, window, XK_c, ControlMask | ShiftMask) == 0 &&
      wait_for_gui_clipboard(clipboard_display, window, clipboard_window,
                             multiline_text, clipboard_text,
                             sizeof(clipboard_text)) == 0;
  send_gui_key(display, window, XK_Escape, 0);

  /* Without an active selection the clipboard should retain its old value. */
  int empty_selection_is_noop =
      send_gui_mouse_selection(display, window, 12, first_text_y, 12,
                               first_text_y) == 0 &&
      send_gui_key(display, window, XK_c, ControlMask | ShiftMask) == 0 &&
      read_gui_clipboard(clipboard_display, clipboard_window, clipboard_text,
                         sizeof(clipboard_text)) == 0 &&
      !strcmp(clipboard_text, multiline_text);

  int copied_in_second_tab =
      send_gui_key(display, window, XK_t, ControlMask) == 0 &&
      send_gui_text(display, window, "/usr/bin/echo TAB_COPY_MARKER") == 0 &&
      send_gui_key(display, window, XK_Return, 0) == 0;
  wait_milliseconds(250);
  int tab_marker_y = gui_find_text_line(display, window, "TAB_COPY_MARKER", -1);
  int tab_marker_right = 12 + XTextWidth(font, "TAB_COPY_MARKER", 15);
  copied_in_second_tab =
      copied_in_second_tab && tab_marker_y >= 0 &&
      send_gui_mouse_selection(display, window, 12,
                               tab_marker_y - font->ascent / 2,
                               tab_marker_right,
                               tab_marker_y - font->ascent / 2) == 0 &&
      send_gui_key(display, window, XK_c, ControlMask | ShiftMask) == 0 &&
      wait_for_gui_clipboard(clipboard_display, window, clipboard_window,
                             "TAB_COPY_MARKER", clipboard_text,
                             sizeof(clipboard_text)) == 0;
  send_gui_key(display, window, XK_d, ControlMask);

  if (copied_marker && copied_multiline && copied_during_search &&
      empty_selection_is_noop && copied_in_second_tab)
    result = 0;
  else
    fprintf(stderr,
            "clipboard test: marker=%d highlight=%d multiline=%d modal=%d "
            "empty-noop=%d second-tab=%d\n",
            copied_marker, visibly_selected, copied_multiline,
            copied_during_search, empty_selection_is_noop,
            copied_in_second_tab);

  XFreeFont(display, font);

  return result;
}

static void wait_milliseconds(long milliseconds) {
  struct timespec pause = {.tv_sec = milliseconds / 1000,
                           .tv_nsec = (milliseconds % 1000) * 1000000};
  nanosleep(&pause, NULL);
}

/* Find a line by its rendered pixels so GUI tests check what users can see. */
static int gui_find_text_line(Display *display, Window window,
                              const char *expected_text,
                              int after_baseline) {
  XFontStruct *font = load_gui_test_font(display);
  if (!font)
    return -1;

  XWindowAttributes attributes;
  if (!XGetWindowAttributes(display, window, &attributes)) {
    XFreeFont(display, font);
    return -1;
  }

  int text_width = XTextWidth(font, expected_text, (int)strlen(expected_text));
  int text_height = font->ascent + font->descent;
  if (text_width <= 0 || text_height <= 0 ||
      attributes.width < text_width + 12) {
    XFreeFont(display, font);
    return -1;
  }
  XImage *background_image =
      XGetImage(display, window, 0, 54, 1, 1, AllPlanes, ZPixmap);
  if (!background_image) {
    XFreeFont(display, font);
    return -1;
  }
  unsigned long background_pixel = XGetPixel(background_image, 0, 0);
  XDestroyImage(background_image);

  Pixmap expected = XCreatePixmap(display, window, (unsigned)text_width,
                                  (unsigned)text_height,
                                  (unsigned)attributes.depth);
  GC graphics = XCreateGC(display, expected, 0, NULL);
  XSetForeground(display, graphics, 0);
  XFillRectangle(display, expected, graphics, 0, 0, (unsigned)text_width,
                 (unsigned)text_height);
  XSetForeground(display, graphics, 1);
  XSetFont(display, graphics, font->fid);
  XDrawString(display, expected, graphics, 0, font->ascent, expected_text,
              (int)strlen(expected_text));
  XImage *expected_image = XGetImage(display, expected, 0, 0,
                                     (unsigned)text_width,
                                     (unsigned)text_height, AllPlanes, ZPixmap);
  int result = -1;
  int line_height = text_height + 3;
  for (int baseline = 54;
       expected_image &&
       baseline + font->descent < attributes.height - 5;
       baseline += line_height) {
    if (baseline <= after_baseline)
      continue;
    XImage *actual_image = XGetImage(
        display, window, 12, baseline - font->ascent, (unsigned)text_width,
        (unsigned)text_height, AllPlanes, ZPixmap);
    if (!actual_image)
      continue;

    int matches = 1;
    for (int row = 0; row < text_height && matches; row++)
      for (int column = 0; column < text_width; column++)
        if ((XGetPixel(actual_image, column, row) !=
             background_pixel) !=
            (XGetPixel(expected_image, column, row) != 0)) {
          matches = 0;
          break;
        }
    XDestroyImage(actual_image);
    if (matches) {
      result = baseline;
      break;
    }
  }

  if (expected_image)
    XDestroyImage(expected_image);
  XFreeGC(display, graphics);
  XFreePixmap(display, expected);
  XFreeFont(display, font);
  return result;
}

static int gui_lines_are_ordered(Display *display, Window window,
                                 const char *first_text,
                                 const char *second_text) {
  int first_line = gui_find_text_line(display, window, first_text, -1);
  int second_line =
      gui_find_text_line(display, window, second_text, first_line);
  return first_line >= 0 && second_line > first_line;
}

static int send_gui_button(Display *display, Window window, int x, int y,
                           unsigned int button) {
  XEvent event = {0};
  event.xbutton.type = ButtonPress;
  event.xbutton.display = display;
  event.xbutton.window = window;
  event.xbutton.root = DefaultRootWindow(display);
  event.xbutton.time = CurrentTime;
  event.xbutton.x = x;
  event.xbutton.y = y;
  event.xbutton.button = button;
  event.xbutton.same_screen = True;
  int sent = XSendEvent(display, window, True, ButtonPressMask, &event);
  XFlush(display);
  return sent ? 0 : -1;
}

static int send_gui_motion(Display *display, Window window, int x, int y) {
  XEvent event = {0};
  event.xmotion.type = MotionNotify;
  event.xmotion.display = display;
  event.xmotion.window = window;
  event.xmotion.root = DefaultRootWindow(display);
  event.xmotion.time = CurrentTime;
  event.xmotion.x = x;
  event.xmotion.y = y;
  event.xmotion.same_screen = True;
  int sent = XSendEvent(display, window, True, PointerMotionMask, &event);
  XFlush(display);
  return sent ? 0 : -1;
}

static unsigned long gui_header_hash(Display *display, Window window) {
  XWindowAttributes attributes;
  if (!XGetWindowAttributes(display, window, &attributes) ||
      attributes.width <= 0)
    return 0;
  XImage *image = XGetImage(display, window, 0, 0,
                            (unsigned)attributes.width, 34, AllPlanes, ZPixmap);
  if (!image)
    return 0;
  unsigned long hash = 5381;
  for (int y = 0; y < 34; y++)
    for (int x = 0; x < attributes.width; x++)
      hash = hash * 33 + XGetPixel(image, x, y);
  XDestroyImage(image);
  return hash;
}

static int gui_command_was_saved(const char *history_path,
                                 const char *command) {
  char *history = read_text_file(history_path);
  int saved = history && contains(history, command);
  free(history);
  return saved;
}

static int test_gui_tab_switching(Display *display, Window window,
                                  const char *working_directory) {
  if (send_gui_key(display, window, XK_t, ControlMask) < 0)
    return -1;
  wait_milliseconds(150);
  unsigned long before_cd = gui_header_hash(display, window);
  if (send_gui_text(display, window, "cd /tmp") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(150);
  unsigned long after_cd = gui_header_hash(display, window);
  if (send_gui_text(display, window, "/usr/bin/pwd") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(250);
  int second_tab_ok = gui_find_text_line(display, window, "/tmp", -1) >= 0 &&
                      before_cd && after_cd && before_cd != after_cd;

  if (send_gui_button(display, window, 50, 20, Button1) < 0 ||
      send_gui_text(display, window, "/usr/bin/pwd") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(250);
  int first_tab_ok =
      gui_find_text_line(display, window, working_directory, -1) >= 0;

  XWindowAttributes attributes;
  if (!XGetWindowAttributes(display, window, &attributes))
    return -1;
  int tab_width = (attributes.width - 56) / 2;
  if (tab_width > 180)
    tab_width = 180;
  if (tab_width < 1)
    return -1;
  int close_x = 8 + tab_width + tab_width - 13;
  unsigned long before_hover = gui_header_hash(display, window);
  if (send_gui_motion(display, window, close_x, 20) < 0)
    return -1;
  wait_milliseconds(50);
  unsigned long after_hover = gui_header_hash(display, window);
  if (!before_hover || !after_hover || before_hover == after_hover)
    return -1;
  if (send_gui_button(display, window, close_x, 20, Button1) < 0)
    return -1;
  wait_milliseconds(200);
  return first_tab_ok && second_tab_ok ? 0 : -1;
}

static int test_gui_editor(Display *display, Window window,
                           const char *history_path) {
  if (send_gui_text(display, window, "/usr/bin/echo EDITOR_REMOVE") < 0 ||
      send_gui_key(display, window, XK_w, ControlMask) < 0 ||
      send_gui_text(display, window, "EDITOR_OK") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(500);
  int delete_word_ok = gui_command_was_saved(
      history_path, "/usr/bin/echo EDITOR_OK\n");

  if (send_gui_text(display, window, "/usr/bin/echo KEEP TRIM") < 0)
    return -1;
  for (int i = 0; i < 5; i++)
    if (send_gui_key(display, window, XK_Left, 0) < 0)
      return -1;
  if (send_gui_key(display, window, XK_k, ControlMask) < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(500);
  int delete_to_end_ok =
      gui_command_was_saved(history_path, "/usr/bin/echo KEEP\n");

  if (send_gui_text(display, window, "/usr/bin/echo MOVE") < 0 ||
      send_gui_key(display, window, XK_a, ControlMask) < 0 ||
      send_gui_key(display, window, XK_e, ControlMask) < 0 ||
      send_gui_text(display, window, "_END") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(500);
  int cursor_keys_ok =
      gui_command_was_saved(history_path, "/usr/bin/echo MOVE_END\n");

  if (send_gui_text(display, window, "discard this line") < 0 ||
      send_gui_key(display, window, XK_u, ControlMask) < 0 ||
      send_gui_text(display, window, "/usr/bin/echo CLEAR_OK") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(500);
  char *history = read_text_file(history_path);
  int delete_to_start_ok = history && contains(history, "/usr/bin/echo CLEAR_OK\n") &&
                           !contains(history, "discard this line");
  free(history);
  if (!delete_word_ok || !delete_to_end_ok || !cursor_keys_ok ||
      !delete_to_start_ok)
    fprintf(stderr,
            "GUI editor mismatch: word=%d end=%d cursor=%d start=%d\n",
            delete_word_ok, delete_to_end_ok, cursor_keys_ok,
            delete_to_start_ok);
  return delete_word_ok && delete_to_end_ok && cursor_keys_ok &&
                 delete_to_start_ok
             ? 0
             : -1;
}

static int test_gui_scrollback(Display *display, Window window) {
  if (send_gui_text(display, window, "/usr/bin/seq 1 50") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(450);
  int latest_visible = gui_find_text_line(display, window, "50", -1) >= 0;

  if (send_gui_key(display, window, XK_Page_Up, 0) < 0)
    return -1;
  wait_milliseconds(100);
  int page_up_visible = gui_find_text_line(display, window, "1", -1) >= 0;

  if (send_gui_key(display, window, XK_Page_Down, 0) < 0)
    return -1;
  wait_milliseconds(100);
  int page_down_visible = gui_find_text_line(display, window, "50", -1) >= 0;

  if (send_gui_button(display, window, 30, 100, Button4) < 0)
    return -1;
  wait_milliseconds(100);
  int wheel_up_visible = gui_find_text_line(display, window, "47", -1) >= 0;
  if (send_gui_button(display, window, 30, 100, Button5) < 0)
    return -1;
  wait_milliseconds(100);
  int wheel_down_visible = gui_find_text_line(display, window, "50", -1) >= 0;
  return latest_visible && page_up_visible && page_down_visible &&
                 wheel_up_visible && wheel_down_visible
             ? 0
             : -1;
}

static int test_gui_interrupt(Display *display, Window window) {
  if (send_gui_text(display, window, "/bin/sleep 10") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(150);
  if (send_gui_key(display, window, XK_c, ControlMask) < 0)
    return -1;
  wait_milliseconds(200);
  if (send_gui_text(display, window, "/usr/bin/printf GUI_SIGNAL_OK") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(350);
  return gui_find_text_line(display, window, "GUI_SIGNAL_OK", -1) >= 0 ? 0
                                                                       : -1;
}

static size_t read_app_children(pid_t app_pid, pid_t *children,
                                size_t child_capacity) {
  char path[128];
  snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
           (long)app_pid, (long)app_pid);
  FILE *file = fopen(path, "r");
  if (!file)
    return 0;
  size_t count = 0;
  long child_pid;
  while (count < child_capacity && fscanf(file, "%ld", &child_pid) == 1)
    children[count++] = (pid_t)child_pid;
  fclose(file);
  return count;
}

static int test_gui_worker_crash(Display *display, Window window,
                                 pid_t app_pid, int process_mode) {
  if (!process_mode) {
    if (send_gui_text(display, window, "/bin/sh -c 'kill -SEGV $$'") < 0 ||
        send_gui_key(display, window, XK_Return, 0) < 0)
      return -1;
    wait_milliseconds(250);
    if (send_gui_text(display, window,
                      "/usr/bin/printf GUI_AFTER_COMMAND_CRASH") < 0 ||
        send_gui_key(display, window, XK_Return, 0) < 0)
      return -1;
    wait_milliseconds(300);
    return gui_find_text_line(display, window, "GUI_AFTER_COMMAND_CRASH", -1) >= 0
               ? 0
               : -1;
  }

  pid_t previous[TERM_MAX_TABS];
  size_t previous_count =
      read_app_children(app_pid, previous, TERM_MAX_TABS);
  if (!previous_count || send_gui_key(display, window, XK_t, ControlMask) < 0)
    return -1;

  pid_t new_worker = 0;
  for (int attempt = 0; attempt < 50 && !new_worker; attempt++) {
    pid_t current[TERM_MAX_TABS];
    size_t current_count =
        read_app_children(app_pid, current, TERM_MAX_TABS);
    for (size_t i = 0; i < current_count; i++) {
      int was_present = 0;
      for (size_t j = 0; j < previous_count; j++)
        if (current[i] == previous[j])
          was_present = 1;
      if (!was_present) {
        new_worker = current[i];
        break;
      }
    }
    if (!new_worker)
      wait_milliseconds(20);
  }
  if (new_worker <= 0 || kill(new_worker, SIGKILL) < 0)
    return -1;

  wait_milliseconds(250);
  if (send_gui_button(display, window, 50, 20, Button1) < 0 ||
      send_gui_text(display, window,
                    "/usr/bin/printf GUI_AFTER_TAB_CRASH") < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0)
    return -1;
  wait_milliseconds(350);
  return kill(app_pid, 0) == 0 &&
                 gui_find_text_line(display, window, "GUI_AFTER_TAB_CRASH", -1) >= 0
             ? 0
             : -1;
}

static int run_gui_feature_test(const char *mode_argument,
                                const char *mode_name, int *clipboard_ok_out,
                                int *clipboard_fallback_ok_out,
                                int *tab_switch_ok_out, int *scroll_ok_out,
                                int *editor_ok_out, int *clear_ok_out,
                                int *signal_ok_out,
                                int *completion_ok_out, int *search_ok_out,
                                int *crash_ok_out) {
  *clipboard_ok_out = 0;
  *clipboard_fallback_ok_out = 0;
  *tab_switch_ok_out = 0;
  *scroll_ok_out = 0;
  *editor_ok_out = 0;
  *clear_ok_out = 0;
  *signal_ok_out = 0;
  *completion_ok_out = 0;
  *search_ok_out = 0;
  *crash_ok_out = 0;
  char gui_directory[TERM_PATH_BUFFER];
  char gui_home[TERM_PATH_BUFFER];
  char unique_file[TERM_PATH_BUFFER];
  char first_choice[TERM_PATH_BUFFER];
  char second_choice[TERM_PATH_BUFFER];
  char third_choice[TERM_PATH_BUFFER];
  char history_path[TERM_PATH_BUFFER];
  char search_marker[128];
  const char *user = getenv("USER");
  if (!user || !*user)
    user = "user";
  snprintf(search_marker, sizeof(search_marker),
           "%s@terminator> (Ctrl+R)", user);
  join_test_path(gui_directory, sizeof(gui_directory), test_root,
                 "gui-working-directory");
  join_test_path(gui_home, sizeof(gui_home), test_root, "gui-home");
  mkdir(gui_directory, 0700);
  mkdir(gui_home, 0700);
  join_test_path(unique_file, sizeof(unique_file), gui_directory,
                 "terminator-completion-unique.txt");
  join_test_path(first_choice, sizeof(first_choice), gui_directory,
                 "pick-alpha.txt");
  join_test_path(second_choice, sizeof(second_choice), gui_directory,
                 "pick-beta.txt");
  join_test_path(third_choice, sizeof(third_choice), gui_directory,
                 "pick-gamma.txt");
  write_text_file(unique_file, "unique\n");
  write_text_file(first_choice, "first\n");
  write_text_file(second_choice, "second\n");
  write_text_file(third_choice, "third\n");
  if (set_test_history_path(mode_name) < 0)
    return -1;
  join_test_path(history_path, sizeof(history_path), test_root, mode_name);

  const char *old_home = getenv("HOME");
  char *saved_home = old_home ? strdup(old_home) : NULL;
  setenv("HOME", gui_home, 1);
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
  if (saved_home) {
    setenv("HOME", saved_home, 1);
    free(saved_home);
  } else {
    unsetenv("HOME");
  }
  if (child < 0)
    return -1;

  Display *display = XOpenDisplay(NULL);
  if (!display) {
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    return -1;
  }
  Display *clipboard_display = XOpenDisplay(NULL);
  if (!clipboard_display) {
    XCloseDisplay(display);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    return -1;
  }
  Window window = None;
  for (int tries = 0; tries < 50 && window == None; tries++) {
    window = find_terminator_window(display);
    if (window == None)
      wait_milliseconds(50);
  }
  Window clipboard_window = XCreateSimpleWindow(
      clipboard_display, DefaultRootWindow(clipboard_display), 0, 0, 1, 1, 0,
      0, 0);
  if (clipboard_window != None) {
    XMapWindow(clipboard_display, clipboard_window);
    XSync(clipboard_display, False);
  }

  int result = 0;
  int search_order_ok = 1;
  if (window == None || clipboard_window == None ||
      send_gui_text(display, window, "cd ") < 0 ||
      send_gui_text(display, window, gui_directory) < 0 ||
      send_gui_key(display, window, XK_Return, 0) < 0) {
    result = -1;
  } else {
    wait_milliseconds(250);
    if (send_gui_text(display, window,
                      "/usr/bin/echo TERMINATOR_COPY_MARKER") < 0 ||
        send_gui_key(display, window, XK_Return, 0) < 0) {
      result = -1;
    } else {
      wait_milliseconds(250);
      if (send_gui_text(display, window,
                        "/usr/bin/printf 'COPY one: spaces\\046and\\044"
                        "symbol\\043\\041\\nCOPY two: x\\133y\\135"
                        "\\173z\\175+'") < 0 ||
          send_gui_key(display, window, XK_Return, 0) < 0) {
        result = -1;
      } else {
        wait_milliseconds(300);
        int copied = test_gui_clipboard_copy(display, window, clipboard_display,
                                             clipboard_window) == 0;
        *clipboard_ok_out =
            copied && paste_external_command(
                          display, window, clipboard_display, clipboard_window,
                          history_path, "/usr/bin/echo TERMINATOR_PASTE_MARKER",
                          "TERMINATOR_PASTE_MARKER", 0) == 0;
        *clipboard_fallback_ok_out =
            *clipboard_ok_out &&
            paste_external_command(
                display, window, clipboard_display, clipboard_window,
                history_path, "/usr/bin/echo TERMINATOR_FALLBACK_MARKER",
                "TERMINATOR_FALLBACK_MARKER", 1) == 0;
        *tab_switch_ok_out =
            test_gui_tab_switching(display, window, gui_directory) == 0;
        *crash_ok_out = test_gui_worker_crash(
                            display, window, child, mode_argument != NULL) == 0;
        *editor_ok_out = test_gui_editor(display, window, history_path) == 0;
        *scroll_ok_out = test_gui_scrollback(display, window) == 0;
      }
      if (result == 0 &&
          (send_gui_text(display, window, "cat terminator-completion-uniq") < 0 ||
           send_gui_key(display, window, XK_Tab, 0) < 0 ||
           send_gui_key(display, window, XK_Return, 0) < 0)) {
        result = -1;
      } else if (result == 0) {
        wait_milliseconds(250);
        if (send_gui_text(display, window, "cat pick-") < 0 ||
            send_gui_key(display, window, XK_Tab, 0) < 0 ||
            send_gui_text(display, window, "9") < 0 ||
            send_gui_key(display, window, XK_Return, 0) < 0 ||
            send_gui_key(display, window, XK_BackSpace, 0) < 0) {
          result = -1;
        } else {
          if (send_gui_text(display, window, "3") < 0 ||
              send_gui_key(display, window, XK_Return, 0) < 0 ||
              send_gui_key(display, window, XK_Return, 0) < 0)
            result = -1;
          wait_milliseconds(300);
          if (result == 0 &&
              (send_gui_text(display, window, "cat no-such-prefix") < 0 ||
               send_gui_key(display, window, XK_Tab, 0) < 0 ||
               send_gui_key(display, window, XK_Return, 0) < 0))
            result = -1;
          wait_milliseconds(300);
        }
      }
    }
  }

  char *history_text = read_text_file(history_path);
  char expected_cd[TERM_PATH_BUFFER + 8];
  snprintf(expected_cd, sizeof(expected_cd), "cd %s\n", gui_directory);
  int history_ok = history_text && contains(history_text, expected_cd) &&
                   contains(history_text, "cat terminator-completion-unique.txt\n") &&
                   contains(history_text, "cat pick-gamma.txt\n") &&
                   contains(history_text, "cat no-such-prefix\n");
  int no_search_command = history_text &&
                          !contains(history_text, "\npick-gamma.txt\n");
  if (!history_ok || !no_search_command)
    fprintf(stderr, "GUI completion history mismatch (%s):\n%s\n",
            mode_name, history_text ? history_text : "<history unavailable>");
  if (result == 0 && (!history_ok || !no_search_command))
    result = -1;
  *completion_ok_out = result == 0;

  if (result == 0) {
    if (send_gui_key(display, window, XK_r, ControlMask) < 0 ||
        send_gui_text(display, window, "pick-gamma.txt") < 0 ||
        send_gui_key(display, window, XK_Return, 0) < 0)
      result = -1;
    else {
      wait_milliseconds(200);
      search_order_ok =
          gui_lines_are_ordered(display, window, search_marker,
                                "Enter search term: pick-gamma.txt") &&
          gui_lines_are_ordered(display, window,
                                "Enter search term: pick-gamma.txt",
                                "cat pick-gamma.txt");
    }
  }
  if (result == 0 &&
      (send_gui_key(display, window, XK_r, ControlMask) < 0 ||
       send_gui_text(display, window, "pick-gama") < 0 ||
       send_gui_key(display, window, XK_Return, 0) < 0))
    result = -1;
  wait_milliseconds(150);
  if (result == 0)
    search_order_ok =
        search_order_ok &&
        gui_lines_are_ordered(display, window, "Enter search term: pick-gama",
                              "cat pick-gamma.txt");
  if (result == 0 &&
      (send_gui_key(display, window, XK_r, ControlMask) < 0 ||
       send_gui_text(display, window, "xyz") < 0 ||
       send_gui_key(display, window, XK_Return, 0) < 0))
    result = -1;
  wait_milliseconds(150);
  if (result == 0)
    search_order_ok =
        search_order_ok &&
        gui_lines_are_ordered(
            display, window, "Enter search term: xyz",
            "No match for search term in history");
  if (result == 0 &&
      (send_gui_key(display, window, XK_r, ControlMask) < 0 ||
       send_gui_key(display, window, XK_Return, 0) < 0))
    result = -1;
  wait_milliseconds(150);
  if (result == 0)
    search_order_ok =
        search_order_ok &&
        gui_lines_are_ordered(display, window, "Enter search term: ",
                              "No match for search term in history");
  if (result == 0 &&
      (send_gui_key(display, window, XK_r, ControlMask) < 0 ||
       send_gui_text(display, window, "cancel-this-search") < 0 ||
       send_gui_key(display, window, XK_Escape, 0) < 0))
    result = -1;
  wait_milliseconds(100);
  if (result == 0)
    search_order_ok =
        search_order_ok &&
        gui_find_text_line(display, window,
                           "Enter search term: cancel-this-search", -1) < 0;
  if (result == 0 &&
      (send_gui_text(display, window, "/usr/bin/printf gui-after-escape") < 0 ||
       send_gui_key(display, window, XK_Return, 0) < 0))
    result = -1;
  wait_milliseconds(300);
  free(history_text);
  history_text = read_text_file(history_path);
  int search_not_submitted = history_text &&
                             count_occurrences(history_text,
                                               "pick-gamma.txt\n") == 1 &&
                             !contains(history_text, "\npick-gamma.txt\n") &&
                             contains(history_text,
                                      "/usr/bin/printf gui-after-escape\n") &&
                             !contains(history_text, "pick-gama\n") &&
                             !contains(history_text, "xyz\n") &&
                             !contains(history_text, "cancel-this-search\n");
  if (!search_not_submitted)
    fprintf(stderr, "GUI search history mismatch (%s):\n%s\n",
            mode_name, history_text ? history_text : "<history unavailable>");
  if (!search_order_ok)
    fprintf(stderr, "GUI history transcript lines are missing or out of order (%s)\n",
            mode_name);
  if (result == 0 && !search_not_submitted)
    result = -1;
  *search_ok_out = result == 0 && search_not_submitted && search_order_ok;
  free(history_text);

  if (send_gui_key(display, window, XK_l, ControlMask) == 0) {
    wait_milliseconds(100);
    *clear_ok_out =
        gui_find_text_line(display, window, "TERMINATOR_COPY_MARKER", -1) < 0 &&
        gui_find_text_line(display, window, "50", -1) < 0;
  }
  *signal_ok_out = test_gui_interrupt(display, window) == 0;

  XDestroyWindow(clipboard_display, clipboard_window);
  XCloseDisplay(clipboard_display);
  XCloseDisplay(display);
  kill(child, SIGTERM);
  int finished = 0;
  int status = 0;
  for (int tries = 0; tries < 20; tries++) {
    if (waitpid(child, &status, WNOHANG) == child) {
      finished = 1;
      break;
    }
    wait_milliseconds(50);
  }
  if (!finished) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    finished = 1;
  }
  if (result == 0 && (!finished || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
    result = -1;
  return result;
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
    test_history_edges();
    test_history_search_matrix();
    test_completion();
    test_completion_edges();
    test_completion_prefix_matrix();
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
      report_test("G003", "filename completion",
                  "thread-mode typing and numbered choice", "make terminator",
                  "GUI completion interaction runs", "application build failed",
                  0, "The core build failed before GUI input could be tested.");
      report_test("G004", "history search", "thread-mode Ctrl+R search",
                  "make terminator", "GUI history search runs",
                  "application build failed", 0,
                  "The core build failed before GUI input could be tested.");
      report_test("G005", "X11 clipboard", "thread-mode mouse selection and Ctrl+Shift+C/V",
                  "make terminator", "GUI copy and paste work",
                  "application build failed", 0,
                  "The core build failed before clipboard exchange could be tested.");
      report_test("G006", "X11 clipboard", "thread-mode STRING fallback",
                  "clipboard rejects UTF8_STRING", "paste retries with XA_STRING",
                  "application build failed", 0, "GUI build failed.");
      report_test("G007", "GUI tabs", "thread-mode tab switching and close",
                  "Ctrl+T; cd /tmp; switch tabs; hover and click ×", "tab state stays independent",
                  "application build failed", 0, "GUI build failed.");
      report_test("G008", "GUI scrollback", "thread-mode keyboard and mouse scrolling",
                  "seq 1 50; Page Up/Down; wheel up/down", "older and latest lines are visible",
                  "application build failed", 0, "GUI build failed.");
      report_test("G009", "GUI hotkeys", "thread-mode clear screen",
                  "Ctrl+L", "old output is removed", "application build failed", 0,
                  "GUI build failed.");
      report_test("G010", "GUI hotkeys", "thread-mode editable cursor operations",
                  "Ctrl+A/E, Ctrl+U/K/W, arrows", "edited commands execute as expected",
                  "application build failed", 0, "GUI build failed.");
      report_test("G011", "GUI signals", "thread-mode Ctrl+C recovery",
                  "sleep 10; Ctrl+C; printf", "worker accepts a later command",
                  "application build failed", 0, "GUI build failed.");
      report_test("G012", "Task 1: thread-mode command crash recovery",
                  "a crashing command does not terminate the GUI or tab worker",
                  "run a SIGSEGV child, then print a recovery marker",
                  "GUI remains responsive and displays the marker",
                  "application build failed", 0, "GUI build failed.");
      report_test("G103", "filename completion",
                  "process-mode typing and numbered choice", "make terminator",
                  "GUI completion interaction runs", "application build failed",
                  0, "The core build failed before GUI input could be tested.");
      report_test("G104", "history search", "process-mode Ctrl+R search",
                  "make terminator", "GUI history search runs",
                  "application build failed", 0,
                  "The core build failed before GUI input could be tested.");
      report_test("G105", "X11 clipboard", "process-mode mouse selection and Ctrl+Shift+C/V",
                  "make terminator", "GUI copy and paste work",
                  "application build failed", 0,
                  "The core build failed before clipboard exchange could be tested.");
      report_test("G106", "X11 clipboard", "process-mode STRING fallback",
                  "clipboard rejects UTF8_STRING", "paste retries with XA_STRING",
                  "application build failed", 0, "GUI build failed.");
      report_test("G107", "GUI tabs", "process-mode tab switching and close",
                  "Ctrl+T; cd /tmp; switch tabs; hover and click ×", "tab state stays independent",
                  "application build failed", 0, "GUI build failed.");
      report_test("G108", "GUI scrollback", "process-mode keyboard and mouse scrolling",
                  "seq 1 50; Page Up/Down; wheel up/down", "older and latest lines are visible",
                  "application build failed", 0, "GUI build failed.");
      report_test("G109", "GUI hotkeys", "process-mode clear screen",
                  "Ctrl+L", "old output is removed", "application build failed", 0,
                  "GUI build failed.");
      report_test("G110", "GUI hotkeys", "process-mode editable cursor operations",
                  "Ctrl+A/E, Ctrl+U/K/W, arrows", "edited commands execute as expected",
                  "application build failed", 0, "GUI build failed.");
      report_test("G111", "GUI signals", "process-mode Ctrl+C recovery",
                  "sleep 10; Ctrl+C; printf", "worker accepts a later command",
                  "application build failed", 0, "GUI build failed.");
      report_test("G112", "Task 1: process-mode tab crash isolation",
                  "killing one tab worker leaves the GUI and another tab usable",
                  "kill new tab worker; run a command in the original tab",
                  "GUI remains responsive and displays the marker",
                  "application build failed", 0, "GUI build failed.");
    } else if (!getenv("DISPLAY")) {
      report_skip("G001", "X11 GUI", "thread-mode startup", "DISPLAY is not set");
      report_skip("G002", "X11 GUI", "process-mode startup", "DISPLAY is not set");
      report_skip("G003", "filename completion", "thread-mode typing and numbered choice",
                  "DISPLAY is not set");
      report_skip("G004", "history search", "thread-mode Ctrl+R search",
                  "DISPLAY is not set");
      report_skip("G005", "X11 clipboard", "thread-mode mouse selection and Ctrl+Shift+C/V",
                  "DISPLAY is not set");
      report_skip("G006", "X11 clipboard", "thread-mode STRING fallback",
                  "DISPLAY is not set");
      report_skip("G007", "GUI tabs", "thread-mode tab switching and close",
                  "DISPLAY is not set");
      report_skip("G008", "GUI scrollback", "thread-mode keyboard and mouse scrolling",
                  "DISPLAY is not set");
      report_skip("G009", "GUI hotkeys", "thread-mode clear screen",
                  "DISPLAY is not set");
      report_skip("G010", "GUI hotkeys", "thread-mode editable cursor operations",
                  "DISPLAY is not set");
      report_skip("G011", "GUI signals", "thread-mode Ctrl+C recovery",
                  "DISPLAY is not set");
      report_skip("G012", "Task 1: thread-mode command crash recovery",
                  "command crash and worker recovery", "DISPLAY is not set");
      report_skip("G103", "filename completion", "process-mode typing and numbered choice",
                  "DISPLAY is not set");
      report_skip("G104", "history search", "process-mode Ctrl+R search",
                  "DISPLAY is not set");
      report_skip("G105", "X11 clipboard", "process-mode mouse selection and Ctrl+Shift+C/V",
                  "DISPLAY is not set");
      report_skip("G106", "X11 clipboard", "process-mode STRING fallback",
                  "DISPLAY is not set");
      report_skip("G107", "GUI tabs", "process-mode tab switching and close",
                  "DISPLAY is not set");
      report_skip("G108", "GUI scrollback", "process-mode keyboard and mouse scrolling",
                  "DISPLAY is not set");
      report_skip("G109", "GUI hotkeys", "process-mode clear screen",
                  "DISPLAY is not set");
      report_skip("G110", "GUI hotkeys", "process-mode editable cursor operations",
                  "DISPLAY is not set");
      report_skip("G111", "GUI signals", "process-mode Ctrl+C recovery",
                  "DISPLAY is not set");
      report_skip("G112", "Task 1: process-mode tab crash isolation",
                  "kill one process tab worker while retaining another",
                  "DISPLAY is not set");
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
      int completion_ok = 0;
      int search_ok = 0;
      int clipboard_ok = 0;
      int fallback_ok = 0;
      int tab_switch_ok = 0;
      int scroll_ok = 0;
      int editor_ok = 0;
      int clear_ok = 0;
      int signal_ok = 0;
      int crash_ok = 0;
      result = run_gui_feature_test(NULL, "gui-thread-history",
                                    &clipboard_ok, &fallback_ok, &tab_switch_ok,
                                    &scroll_ok, &editor_ok, &clear_ok,
                                    &signal_ok, &completion_ok, &search_ok,
                                    &crash_ok);
      report_test("G003", "filename completion",
                  "thread-mode typing, unique completion, and numbered choice",
                  "type with Tab; reject 9; erase it; choose 3; try a no-match Tab",
                  "completed commands are submitted exactly once",
                  completion_ok ? "unique and selected candidates were submitted"
                                : "GUI completion interaction failed",
                  completion_ok,
                  "Uses XSendEvent through Xvfb; checks persisted commands.");
      report_test("G004", "history search",
                  "thread-mode search transcript and search results",
                  "exact; fuzzy; xyz no-match; empty term; Escape",
                  "prompt, search term, and result appear in order; query is not saved",
                  search_ok ? "transcript order and history checks passed"
                            : "transcript order or history check failed",
                  search_ok,
                  "Checks rendered X11 text and persistent history.");
      report_test("G005", "X11 clipboard",
                  "thread-mode mouse selection and Ctrl+Shift+C/V",
                  "drag over output; copy to a second X client; paste a command",
                  "copied bytes match and pasted command runs",
                  clipboard_ok ? "copy and paste worked"
                               : "selection, copy, or paste failed",
                  clipboard_ok,
                  "Exercises X11 selection requests and the real GUI key handler.");
      report_test("G006", "X11 clipboard", "thread-mode STRING fallback",
                  "clipboard rejects UTF8_STRING", "paste retries with XA_STRING",
                  fallback_ok ? "fallback text executed"
                              : "STRING fallback failed",
                  fallback_ok, "Exercises the selection conversion retry path.");
      report_test("G007", "GUI tabs", "thread-mode tab switching and close",
                  "Ctrl+T; cd /tmp; switch tabs; hover and click ×",
                  "each tab keeps its directory and can close independently",
                  tab_switch_ok ? "tab path title updated and hover-close worked"
                                : "tab state, switching, or close failed",
                  tab_switch_ok,
                  "Checks per-tab path title updates, hover close icon, click-to-close, and directory isolation.");
      report_test("G008", "GUI scrollback", "thread-mode keyboard and mouse scrolling",
                  "seq 1 50; Page Up/Down; wheel up/down",
                  "older and latest output lines are visible",
                  scroll_ok ? "keyboard and mouse scrolling passed"
                            : "scroll position or visible line was wrong",
                  scroll_ok, "Checks rendered output in the X11 viewport.");
      report_test("G009", "GUI hotkeys", "thread-mode clear screen", "Ctrl+L",
                  "old output is removed", clear_ok ? "scrollback cleared"
                                                    : "old output remained visible",
                  clear_ok, "Checks the keyboard handler and rendered view.");
      report_test("G010", "GUI hotkeys", "thread-mode editable cursor operations",
                  "Ctrl+A/E, Ctrl+U/K/W, arrows",
                  "edited commands execute as expected",
                  editor_ok ? "edited commands were saved correctly"
                            : "input editing did not match expected commands",
                  editor_ok, "Checks the command buffer through submitted commands.");
      report_test("G011", "GUI signals", "thread-mode Ctrl+C recovery",
                  "sleep 10; Ctrl+C; printf", "worker accepts a later command",
                  signal_ok ? "command interrupted and worker recovered"
                            : "Ctrl+C did not return a working prompt",
                  signal_ok, "The GUI sends SIGINT to the active job group.");
      report_test("G012", "Task 1: thread-mode command crash recovery",
                  "a crashing command does not terminate the GUI or tab worker",
                  "run a SIGSEGV child, then print a recovery marker",
                  "GUI remains responsive and displays the marker",
                  crash_ok ? "GUI accepted a command after SIGSEGV"
                           : "GUI or worker failed to recover",
                  crash_ok, "The fatal signal is delivered only to the command child.");
      result = run_gui_feature_test("useProcess=True", "gui-process-history",
                                    &clipboard_ok, &fallback_ok, &tab_switch_ok,
                                    &scroll_ok, &editor_ok, &clear_ok,
                                    &signal_ok, &completion_ok, &search_ok,
                                    &crash_ok);
      report_test("G103", "filename completion",
                  "process-mode typing, unique completion, and numbered choice",
                  "type with Tab; reject 9; erase it; choose 3; try a no-match Tab",
                  "completed commands are submitted exactly once",
                  completion_ok ? "unique and selected candidates were submitted"
                                : "GUI completion interaction failed",
                  completion_ok,
                  "Uses XSendEvent through Xvfb; checks persisted commands.");
      report_test("G104", "history search",
                  "process-mode search transcript and search results",
                  "exact; fuzzy; xyz no-match; empty term; Escape",
                  "prompt, search term, and result appear in order; query is not saved",
                  search_ok ? "transcript order and history checks passed"
                            : "transcript order or history check failed",
                  search_ok,
                  "Checks rendered X11 text and persistent history.");
      report_test("G105", "X11 clipboard",
                  "process-mode mouse selection and Ctrl+Shift+C/V",
                  "drag over output; copy to a second X client; paste a command",
                  "copied bytes match and pasted command runs",
                  clipboard_ok ? "copy and paste worked"
                               : "selection, copy, or paste failed",
                  clipboard_ok,
                  "Exercises X11 selection requests and the real GUI key handler.");
      report_test("G106", "X11 clipboard", "process-mode STRING fallback",
                  "clipboard rejects UTF8_STRING", "paste retries with XA_STRING",
                  fallback_ok ? "fallback text executed"
                              : "STRING fallback failed",
                  fallback_ok, "Exercises the selection conversion retry path.");
      report_test("G107", "GUI tabs", "process-mode tab switching and close",
                  "Ctrl+T; cd /tmp; switch tabs; hover and click ×",
                  "each tab keeps its directory and can close independently",
                  tab_switch_ok ? "tab path title updated and hover-close worked"
                                : "tab state, switching, or close failed",
                  tab_switch_ok,
                  "Checks per-tab path title updates, hover close icon, click-to-close, and directory isolation.");
      report_test("G108", "GUI scrollback", "process-mode keyboard and mouse scrolling",
                  "seq 1 50; Page Up/Down; wheel up/down",
                  "older and latest output lines are visible",
                  scroll_ok ? "keyboard and mouse scrolling passed"
                            : "scroll position or visible line was wrong",
                  scroll_ok, "Checks rendered output in the X11 viewport.");
      report_test("G109", "GUI hotkeys", "process-mode clear screen", "Ctrl+L",
                  "old output is removed", clear_ok ? "scrollback cleared"
                                                    : "old output remained visible",
                  clear_ok, "Checks the keyboard handler and rendered view.");
      report_test("G110", "GUI hotkeys", "process-mode editable cursor operations",
                  "Ctrl+A/E, Ctrl+U/K/W, arrows",
                  "edited commands execute as expected",
                  editor_ok ? "edited commands were saved correctly"
                            : "input editing did not match expected commands",
                  editor_ok, "Checks the command buffer through submitted commands.");
      report_test("G111", "GUI signals", "process-mode Ctrl+C recovery",
                  "sleep 10; Ctrl+C; printf", "worker accepts a later command",
                  signal_ok ? "command interrupted and worker recovered"
                            : "Ctrl+C did not return a working prompt",
                  signal_ok, "The GUI sends SIGINT to the active job group.");
      report_test("G112", "Task 1: process-mode tab crash isolation",
                  "killing one tab worker leaves the GUI and another tab usable",
                  "kill new tab worker; run a command in the original tab",
                  "GUI remains responsive and displays the marker",
                  crash_ok ? "original tab and GUI stayed usable"
                           : "GUI or remaining tab stopped responding",
                  crash_ok,
                  "Kills the actual second worker child and checks the original tab.");
    }
  }
  if (all)
    report_skip("T110", "Task 11: custom Bash-subset interpreter",
                "script-interpreter requirement audit",
                "Task 11 is not implemented; source runs lines through the existing shell.");
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
  const char *report_path = getenv("TERMINATOR_JUNIT_REPORT");
  if (report_path && write_junit_report(report_path) < 0) {
    fprintf(stderr, "could not write JUnit report to %s\n", report_path);
    failed = 1;
  }
  if (test_record_error) {
    fprintf(stderr, "could not store all test results for the report\n");
    failed = 1;
  }
  if (remove_tree(test_root) < 0) {
    fprintf(stderr, "test cleanup failed for %s\n", test_root);
    failed = 1;
  }
  free(test_root);
  for (size_t i = 0; i < test_record_count; i++) {
    free(test_records[i].id);
    free(test_records[i].task);
    free(test_records[i].feature);
    free(test_records[i].scenario);
    free(test_records[i].input);
    free(test_records[i].expected);
    free(test_records[i].actual);
    free(test_records[i].notes);
  }
  free(test_records);
  return failed ? 1 : 0;
}
