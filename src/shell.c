#include "shell.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Built-ins and the worker loop that connects one tab to the shell engine. */
static char *join_path(const char *directory, const char *name) {
  size_t path_size = strlen(directory) + strlen(name) + 2;
  char *path = malloc(path_size);
  if (!path)
    return NULL;

  snprintf(path, path_size, "%s/%s", directory, name);
  return path;
}

static char *make_path_from_working_directory(const char *working_directory,
                                              const char *path) {
  if (path[0] == '/')
    return strdup(path);
  return join_path(working_directory, path);
}

static int resolve_directory(const char *path, char *resolved_path) {
  struct stat path_info;
  if (stat(path, &path_info) < 0)
    return -1;
  if (!S_ISDIR(path_info.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  return realpath(path, resolved_path) ? 0 : -1;
}

static int builtin_cd(ShellContext *shell, char **arguments, int output_fd) {
  const char *destination = arguments[1] ? arguments[1] : getenv("HOME");
  if (!destination) {
    dprintf(output_fd, "terminator: cd: HOME not set\n");
    return 1;
  }

  char *expanded_destination = NULL;
  if (destination[0] == '~' &&
      (destination[1] == '/' || destination[1] == '\0')) {
    const char *home = getenv("HOME");
    if (home) {
      size_t path_size = strlen(home) + strlen(destination + 1) + 1;
      expanded_destination = malloc(path_size);
      if (!expanded_destination) {
        dprintf(output_fd, "terminator: cd: out of memory\n");
        return 1;
      }
      snprintf(expanded_destination, path_size, "%s%s", home, destination + 1);
      destination = expanded_destination;
    }
  }

  char *candidate =
      make_path_from_working_directory(shell->working_directory, destination);
  char resolved[PATH_MAX];
  if (!candidate || resolve_directory(candidate, resolved) < 0) {
    int error_number = errno ? errno : ENOMEM;
    dprintf(output_fd, "terminator: cd: %s: %s\n", destination,
            strerror(error_number));
    free(expanded_destination);
    free(candidate);
    return 1;
  }
  if (strlen(resolved) >= sizeof(shell->working_directory)) {
    dprintf(output_fd, "terminator: cd: path is too long\n");
    free(expanded_destination);
    free(candidate);
    return 1;
  }
  strcpy(shell->working_directory, resolved);
  free(expanded_destination);
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
  char *path =
      make_path_from_working_directory(shell->working_directory, arguments[1]);
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
    {"cd", builtin_cd},         {"clear", builtin_clear},
    {"exit", builtin_exit},     {"history", builtin_history},
    {"jobs", builtin_jobs},     {"fg", builtin_fg},
    {"bg", builtin_bg},         {"multiWatch", shell_builtin_multiwatch},
    {"source", builtin_source},
};

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
                const char *working_directory) {
  /* Read newline-framed commands and report status/cwd back to the GUI. */
  ShellContext shell = {0};
  shell.history = history;
  shell.foreground = foreground;
  shell.last_status = 0;
  snprintf(shell.working_directory, sizeof(shell.working_directory), "%s",
           working_directory);
  char command_buffer[TERM_LINE_MAX];
  size_t command_length = 0;
  for (;;) {
    char character;
    ssize_t bytes_read = read(input_fd, &character, 1);
    if (bytes_read == 0)
      break;
    if (bytes_read < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (character == '\n') {
      command_buffer[command_length] = '\0';
      if (!shell_is_complete(command_buffer)) {
        if (command_length && command_buffer[command_length - 1] == '\\')
          command_length--;
        else if (command_length + 1 < sizeof(command_buffer))
          command_buffer[command_length++] = '\n';
        continue;
      }
      int status = shell.last_status;
      if (command_length) {
        history_add(history, command_buffer);
        status = shell_run_line(&shell, command_buffer, output_fd);
      }
      command_length = 0;
      WorkerStatus message = {.status = status,
                              .clear_screen = shell.clear_requested};
      size_t working_directory_length = strlen(shell.working_directory);
      if (working_directory_length >= sizeof(message.working_directory))
        working_directory_length = sizeof(message.working_directory) - 1;
      memcpy(message.working_directory, shell.working_directory,
             working_directory_length);
      message.working_directory[working_directory_length] = '\0';
      ssize_t ignored = write(status_fd, &message, sizeof(message));
      (void)ignored;
      shell.clear_requested = 0;
      if (shell.should_exit)
        break;
    } else if (command_length + 1 < sizeof(command_buffer)) {
      command_buffer[command_length++] = character;
    }
  }
  shell_jobs_cleanup(&shell);
  close(input_fd);
  close(output_fd);
  close(status_fd);
}
