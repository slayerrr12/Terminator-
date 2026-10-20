#include "shell.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int builtin_cd(ShellContext *ctx, char **argv, int out) {
  const char *dest = argv[1] ? argv[1] : getenv("HOME");
  if (!dest) {
    dprintf(out, "terminator: cd: HOME not set\n");
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
  else if (asprintf(&candidate, "%s/%s", ctx->cwd, dest) < 0)
    candidate = NULL;
  char resolved[PATH_MAX];
  struct stat st;
  if (!candidate || stat(candidate, &st) < 0 || !S_ISDIR(st.st_mode) ||
      !realpath(candidate, resolved)) {
    int err = errno ? errno : ENOTDIR;
    dprintf(out, "terminator: cd: %s: %s\n", dest, strerror(err));
    free(expanded);
    free(candidate);
    return 1;
  }
  if (strlen(resolved) >= sizeof(ctx->cwd)) {
    dprintf(out, "terminator: cd: path is too long\n");
    free(expanded);
    free(candidate);
    return 1;
  }
  strcpy(ctx->cwd, resolved);
  free(expanded);
  free(candidate);
  return 0;
}

static int builtin_exit(ShellContext *ctx, char **argv, int out) {
  (void)out;
  ctx->should_exit = 1;
  return argv[1] ? atoi(argv[1]) : ctx->last_status;
}

static int builtin_history(ShellContext *ctx, char **argv, int out) {
  (void)argv;
  char (*items)[TERM_LINE_MAX] = calloc(TERM_HISTORY_CAP, sizeof(*items));
  if (!items)
    return 1;
  size_t n = history_snapshot(ctx->history, items, TERM_HISTORY_CAP);
  for (size_t i = 0; i < n; ++i)
    dprintf(out, "%zu  %s\n", i + 1, items[i]);
  free(items);
  return 0;
}

static int builtin_source(ShellContext *ctx, char **argv, int out) {
  if (!argv[1]) {
    dprintf(out, "terminator: source: filename required\n");
    return 2;
  }
  char *path = NULL;
  if (argv[1][0] == '/')
    path = strdup(argv[1]);
  else if (asprintf(&path, "%s/%s", ctx->cwd, argv[1]) < 0)
    path = NULL;
  FILE *file = path ? fopen(path, "r") : NULL;
  if (!file) {
    dprintf(out, "terminator: source: %s: %s\n", argv[1], strerror(errno));
    free(path);
    return 1;
  }
  free(path);
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  char command[TERM_LINE_MAX];
  size_t used = 0;
  int rc = 0;
  while ((n = getline(&line, &cap, file)) >= 0) {
    if ((size_t)n + used >= sizeof(command)) {
      dprintf(out, "terminator: source: line too long\n");
      rc = 2;
      break;
    }
    memcpy(command + used, line, (size_t)n);
    used += (size_t)n;
    command[used] = 0;
    if (!shell_is_complete(command))
      continue;
    rc = shell_run_line(ctx, command, out);
    used = 0;
    command[0] = 0;
    if (ctx->should_exit)
      break;
  }
  if (used) {
    dprintf(out, "terminator: source: incomplete command\n");
    rc = 2;
  }
  free(line);
  fclose(file);
  return rc;
}

typedef int (*BuiltinHandler)(ShellContext *, char **, int);
typedef struct {
  const char *name;
  BuiltinHandler handler;
} BuiltinEntry;
static const BuiltinEntry builtins[] = {{"cd", builtin_cd},
                                        {"exit", builtin_exit},
                                        {"history", builtin_history},
                                        {"source", builtin_source}};

int shell_builtin(ShellContext *ctx, char **argv, int out) {
  if (!argv || !argv[0])
    return -1;
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    if (!strcmp(argv[0], builtins[i].name))
      return builtins[i].handler(ctx, argv, out);
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
                int tab_id, const char *cwd) {
  ShellContext ctx = {0};
  ctx.history = history;
  ctx.foreground = foreground;
  ctx.last_status = 0;
  strncpy(ctx.cwd, cwd, sizeof(ctx.cwd) - 1);
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
      int status = ctx.last_status;
      if (n) {
        history_add(history, line);
        status = shell_run_line(&ctx, line, output_fd);
      }
      n = 0;
      WorkerStatus message = {.status = status};
      size_t cwd_len = strnlen(ctx.cwd, sizeof(message.cwd) - 1);
      memcpy(message.cwd, ctx.cwd, cwd_len);
      message.cwd[cwd_len] = '\0';
      ssize_t ignored = write(status_fd, &message, sizeof(message));
      (void)ignored;
      if (ctx.should_exit)
        break;
    } else if (n + 1 < sizeof(line))
      line[n++] = ch;
  }
  close(input_fd);
  close(output_fd);
  close(status_fd);
}
