#include "shell.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Built-ins and the worker loop that connects one tab to the shell engine. */
static int builtin_cd(ShellContext *shell, char **arguments, int output_fd) {
  const char *dest = arguments[1] ? arguments[1] : getenv("HOME");
  if (!dest) {
    dprintf(output_fd, "terminator: cd: HOME not set\n");
    return 1;
  }
  char *expanded = NULL;
  if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
    const char *home = getenv("HOME");
    if (home && asprintf(&expanded, "%s%s", home, dest + 1) >= 0)
      dest = expanded;
  }
  char *candidate = NULL;
  if (dest[0] == '/')
    candidate = strdup(dest);
  else if (asprintf(&candidate, "%s/%s", shell->cwd, dest) < 0)
    candidate = NULL;
  char resolved[PATH_MAX];
  struct stat st;
  if (!candidate || stat(candidate, &st) < 0 || !S_ISDIR(st.st_mode) ||
      !realpath(candidate, resolved)) {
    int err = errno ? errno : ENOTDIR;
    dprintf(output_fd, "terminator: cd: %s: %s\n", dest, strerror(err));
    free(expanded);
    free(candidate);
    return 1;
  }
  if (strlen(resolved) >= sizeof(shell->cwd)) {
    dprintf(output_fd, "terminator: cd: path is too long\n");
    free(expanded);
    free(candidate);
    return 1;
  }
  strcpy(shell->cwd, resolved);
  free(expanded);
  free(candidate);
  return 0;
}

static int builtin_exit(ShellContext *shell, char **arguments, int output_fd) {
  (void)output_fd;
  shell->should_exit = 1;
  return arguments[1] ? atoi(arguments[1]) : shell->last_status;
}

/* Ask the GUI to clear this tab's scrollback without sending ANSI escapes. */
static int builtin_clear(ShellContext *shell, char **arguments, int output_fd) {
  (void)arguments;
  (void)output_fd;
  shell->clear_requested = 1;
  return 0;
}

static int builtin_history(ShellContext *shell, char **arguments,
                           int output_fd) {
  (void)arguments;
  char (*items)[TERM_LINE_MAX] = calloc(TERM_HISTORY_CAP, sizeof(*items));
  if (!items)
    return 1;
  size_t history_count =
      history_snapshot(shell->history, items, TERM_HISTORY_CAP);
  for (size_t history_index = 0; history_index < history_count; ++history_index)
    dprintf(output_fd, "%zu  %s\n", history_index + 1, items[history_index]);
  free(items);
  return 0;
}

static int builtin_source(ShellContext *shell, char **arguments,
                          int output_fd) {
  /* Run complete commands from a file in this tab's shell context. */
  if (!arguments[1]) {
    dprintf(output_fd, "terminator: source: filename required\n");
    return 2;
  }
  char *path = NULL;
  if (arguments[1][0] == '/')
    path = strdup(arguments[1]);
  else if (asprintf(&path, "%s/%s", shell->cwd, arguments[1]) < 0)
    path = NULL;
  FILE *file = path ? fopen(path, "r") : NULL;
  if (!file) {
    dprintf(output_fd, "terminator: source: %s: %s\n", arguments[1],
            strerror(errno));
    free(path);
    return 1;
  }
  free(path);
  char *line = NULL;
  size_t line_capacity = 0;
  ssize_t line_length;
  char command[TERM_LINE_MAX];
  size_t command_length = 0;
  int command_status = 0;
  while ((line_length = getline(&line, &line_capacity, file)) >= 0) {
    if ((size_t)line_length + command_length >= sizeof(command)) {
      dprintf(output_fd, "terminator: source: line too long\n");
      command_status = 2;
      break;
    }
    memcpy(command + command_length, line, (size_t)line_length);
    command_length += (size_t)line_length;
    command[command_length] = 0;
    if (!shell_is_complete(command))
      continue;
    command_status = shell_run_line(shell, command, output_fd);
    command_length = 0;
    command[0] = 0;
    if (shell->should_exit)
      break;
  }
  if (command_length) {
    dprintf(output_fd, "terminator: source: incomplete command\n");
    command_status = 2;
  }
  free(line);
  fclose(file);
  return command_status;
}

static int builtin_jobs(ShellContext *shell, char **arguments, int output_fd) {
  (void)arguments;
  return shell_jobs_list(shell, output_fd);
}

static int builtin_fg(ShellContext *shell, char **arguments, int output_fd) {
  return shell_job_foreground(shell, arguments, output_fd);
}

static int builtin_bg(ShellContext *shell, char **arguments, int output_fd) {
  return shell_job_background(shell, arguments, output_fd);
}

typedef int (*BuiltinHandler)(ShellContext *, char **, int);
typedef struct {
  const char *name;
  BuiltinHandler handler;
} BuiltinEntry;
/* Keep built-in lookup table-driven so adding a command stays local. */
static const BuiltinEntry builtins[] = {
    {"cd", builtin_cd},        {"clear", builtin_clear},
    {"exit", builtin_exit},    {"history", builtin_history},
    {"jobs", builtin_jobs},    {"fg", builtin_fg},
    {"bg", builtin_bg},        {"multiWatch", shell_builtin_multiwatch},
    {"source", builtin_source}};

int shell_builtin(ShellContext *shell, char **arguments, int output_fd) {
  if (!arguments || !arguments[0])
    return -1;
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    if (!strcmp(arguments[0], builtins[i].name))
      return builtins[i].handler(shell, arguments, output_fd);
  return -1;
}
#include "shell.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void worker_run(int input_fd, int output_fd, int status_fd,
                SharedHistory *history, SharedForeground *foreground,
                int tab_id, const char *working_directory) {
  /* Read newline-framed commands and report status/cwd back to the GUI. */
  ShellContext shell = {0};
  shell.history = history;
  shell.foreground = foreground;
  shell.last_status = 0;
  strncpy(shell.cwd, working_directory, sizeof(shell.cwd) - 1);
  char line[TERM_LINE_MAX];
  size_t n = 0;
  (void)tab_id;
  for (;;) {
    char ch;
    ssize_t r = read(input_fd, &ch, 1);
    if (r == 0)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ch == '\n') {
      line[n] = 0;
      if (!shell_is_complete(line)) {
        if (n && line[n - 1] == '\\')
          n--;
        else if (n + 1 < sizeof(line))
          line[n++] = '\n';
        continue;
      }
      int status = shell.last_status;
      if (n) {
        history_add(history, line);
        status = shell_run_line(&shell, line, output_fd);
      }
      n = 0;
      WorkerStatus message = {.status = status,
                              .clear_screen = shell.clear_requested};
      size_t cwd_len = strnlen(shell.cwd, sizeof(message.cwd) - 1);
      memcpy(message.cwd, shell.cwd, cwd_len);
      message.cwd[cwd_len] = '\0';
      ssize_t ignored = write(status_fd, &message, sizeof(message));
      (void)ignored;
      shell.clear_requested = 0;
      if (shell.should_exit)
        break;
    } else if (n + 1 < sizeof(line))
      line[n++] = ch;
  }
  shell_jobs_cleanup(&shell);
  close(input_fd);
  close(output_fd);
  close(status_fd);
}
