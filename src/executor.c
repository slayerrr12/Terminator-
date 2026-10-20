#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int redirection_open_input(const char *path, int error_fd) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    dprintf(error_fd, "terminator: %s: %s\n", path, strerror(errno));
  return fd;
}

int redirection_open_output(const char *path, int append, int error_fd) {
  int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  int fd = open(path, flags, 0666);
  if (fd < 0)
    dprintf(error_fd, "terminator: %s: %s\n", path, strerror(errno));
  return fd;
}

int redirection_apply(const SimpleCommand *command, int error_fd) {
  if (command->input_path) {
    int fd = redirection_open_input(command->input_path, error_fd);
    if (fd < 0)
      return -1;
    if (dup2(fd, STDIN_FILENO) < 0) {
      int err = errno;
      close(fd);
      errno = err;
      return -1;
    }
    close(fd);
  }
  if (command->output_path) {
    int fd = redirection_open_output(command->output_path, command->append,
                                     error_fd);
    if (fd < 0)
      return -1;
    if (dup2(fd, STDOUT_FILENO) < 0) {
      int err = errno;
      close(fd);
      errno = err;
      return -1;
    }
    close(fd);
  }
  return 0;
}
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int push_arg(char ***args, size_t *count, char *value) {
  char **next = realloc(*args, (*count + 2) * sizeof(*next));
  if (!next)
    return -1;
  *args = next;
  (*args)[(*count)++] = value;
  (*args)[*count] = NULL;
  return 0;
}

static int expand_command(ShellContext *ctx, SimpleCommand *command, int fd) {
  char **argv = NULL;
  size_t argc = 0;
  for (size_t i = 0; i < command->argc; ++i) {
    char *word = NULL;
    int glob_eligible = 0;
    if (shell_expand_word(ctx, command->argv[i], &word, fd, &glob_eligible) < 0)
      goto fail;
    glob_t matches = {0};
    char *pattern = NULL, *prefix = NULL;
    int result = GLOB_NOMATCH;
    if (glob_eligible) {
      if (word[0] == '/')
        pattern = strdup(word);
      else if (asprintf(&prefix, "%s/", ctx->cwd) >= 0 &&
               asprintf(&pattern, "%s%s", prefix, word) < 0) {
        free(prefix);
        prefix = NULL;
      }
      if (pattern)
        result = glob(pattern, 0, NULL, &matches);
    }
    if (result == 0) {
      for (size_t j = 0; j < matches.gl_pathc; ++j) {
        const char *match = matches.gl_pathv[j];
        if (prefix && !strncmp(match, prefix, strlen(prefix)))
          match += strlen(prefix);
        char *copy = strdup(match);
        if (!copy || push_arg(&argv, &argc, copy) < 0) {
          free(copy);
          globfree(&matches);
          free(pattern);
          free(prefix);
          free(word);
          goto fail;
        }
      }
    } else {
      if (push_arg(&argv, &argc, word) < 0) {
        free(word);
        globfree(&matches);
        free(pattern);
        free(prefix);
        goto fail;
      }
      word = NULL;
    }
    free(word);
    globfree(&matches);
    free(pattern);
    free(prefix);
  }
  if (!argv)
    argv = calloc(1, sizeof(*argv));
  if (!argv)
    goto fail;
  for (size_t i = 0; i < command->argc; ++i)
    free(command->argv[i]);
  free(command->argv);
  command->argv = argv;
  command->argc = argc;
  if (command->input_path) {
    char *expanded = NULL;
    if (shell_expand_word(ctx, command->input_path, &expanded, fd, NULL) < 0)
      return -1;
    free(command->input_path);
    command->input_path = expanded;
  }
  if (command->output_path) {
    char *expanded = NULL;
    if (shell_expand_word(ctx, command->output_path, &expanded, fd, NULL) < 0)
      return -1;
    free(command->output_path);
    command->output_path = expanded;
  }
  return 0;
fail:
  if (argv) {
    for (size_t i = 0; i < argc; ++i)
      free(argv[i]);
    free(argv);
  }
  dprintf(fd, "terminator: expansion failed\n");
  return -1;
}

static int child_command(ShellContext *ctx, SimpleCommand *command, int out) {
  if (chdir(ctx->cwd) < 0) {
    dprintf(out, "terminator: %s: %s\n", ctx->cwd, strerror(errno));
    return 1;
  }
  if (redirection_apply(command, out) < 0)
    return 1;
  int builtin = shell_builtin(ctx, command->argv, STDOUT_FILENO);
  if (builtin >= 0)
    return builtin;
  if (out > STDERR_FILENO)
    close(out);
  execvp(command->argv[0], command->argv);
  int err = errno;
  if (err == ENOENT)
    dprintf(STDERR_FILENO, "terminator: %s: command not found\n",
            command->argv[0]);
  else
    dprintf(STDERR_FILENO, "terminator: %s: %s\n", command->argv[0],
            strerror(err));
  return 127;
}

static int run_pipeline(ShellContext *ctx, Pipeline *pipeline, int output_fd) {
  size_t count = pipeline->count;
  int (*pipes)[2] = count > 1 ? calloc(count - 1, sizeof(*pipes)) : NULL;
  pid_t *pids = calloc(count, sizeof(*pids));
  if (!pids || (count > 1 && !pipes)) {
    free(pipes);
    free(pids);
    return 1;
  }
  for (size_t i = 0; i + 1 < count; ++i) {
    if (pipe(pipes[i]) < 0) {
      dprintf(output_fd, "terminator: pipe: %s\n", strerror(errno));
      for (size_t j = 0; j < i; ++j) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }
      free(pipes);
      free(pids);
      return 1;
    }
  }
  size_t started = 0;
  pid_t pgid = 0;
  for (size_t i = 0; i < count; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      (void)setpgid(0, i ? pids[0] : 0);
      if (i)
        dup2(pipes[i - 1][0], STDIN_FILENO);
      else {
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
          dup2(nullfd, STDIN_FILENO);
          close(nullfd);
        }
      }
      if (i + 1 < count)
        dup2(pipes[i][1], STDOUT_FILENO);
      else
        dup2(output_fd, STDOUT_FILENO);
      dup2(output_fd, STDERR_FILENO);
      for (size_t j = 0; j + 1 < count; ++j) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }
      _exit(child_command(ctx, &pipeline->commands[i], output_fd) & 255);
    }
    if (pid < 0) {
      dprintf(output_fd, "terminator: fork: %s\n", strerror(errno));
      break;
    }
    if (i == 0) {
      pgid = pid;
      if (ctx->foreground)
        atomic_store(&ctx->foreground->pgid, (int)pgid);
    }
    (void)setpgid(pid, pgid);
    pids[started++] = pid;
  }
  for (size_t i = 0; i + 1 < count; ++i) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  int result = 1;
  for (size_t i = 0; i < started; ++i) {
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(pids[i], &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (i + 1 == count && waited > 0 && WIFEXITED(status))
      result = WEXITSTATUS(status);
    else if (i + 1 == count && waited > 0 && WIFSIGNALED(status))
      result = 128 + WTERMSIG(status);
  }
  if (ctx->foreground)
    atomic_store(&ctx->foreground->pgid, 0);
  free(pipes);
  free(pids);
  return result;
}

static char *cwd_path(const char *cwd, const char *path) {
  if (path[0] == '/')
    return strdup(path);
  char *joined = NULL;
  if (asprintf(&joined, "%s/%s", cwd, path) < 0)
    return NULL;
  return joined;
}

int shell_execute(ShellContext *ctx, Pipeline *pipeline, int output_fd) {
  if (!pipeline->count)
    return 0;
  if (pipeline->count == 1) {
    SimpleCommand *command = &pipeline->commands[0];
    const char *name = command->argv[0];
    int parent_builtin = !strcmp(name, "cd") || !strcmp(name, "exit") ||
                         !strcmp(name, "history") || !strcmp(name, "source");
    if (parent_builtin) {
      int target = output_fd, opened = -1;
      if (command->input_path) {
        char *path = cwd_path(ctx->cwd, command->input_path);
        int fd = path ? redirection_open_input(path, output_fd) : -1;
        free(path);
        if (fd < 0)
          return 1;
        close(fd);
      }
      if (command->output_path) {
        char *path = cwd_path(ctx->cwd, command->output_path);
        opened = path
                     ? redirection_open_output(path, command->append, output_fd)
                     : -1;
        free(path);
        if (opened < 0)
          return 1;
        target = opened;
      }
      int result = shell_builtin(ctx, command->argv, target);
      if (opened >= 0)
        close(opened);
      ctx->last_status = result;
      return result;
    }
  }
  int result = run_pipeline(ctx, pipeline, output_fd);
  ctx->last_status = result;
  return result;
}

int shell_run_line(ShellContext *ctx, const char *line, int output_fd) {
  Tokens tokens = {0};
  Pipeline pipeline = {0};
  char *error = NULL;
  if (shell_parse(line, &tokens, &pipeline, &error) < 0) {
    dprintf(output_fd, "terminator: %s\n", error ? error : "parse error");
    free(error);
    return 2;
  }
  shell_tokens_free(&tokens);
  for (size_t i = 0; i < pipeline.count; ++i) {
    if (expand_command(ctx, &pipeline.commands[i], output_fd) < 0) {
      shell_pipeline_free(&pipeline);
      return 1;
    }
  }
  int result = shell_execute(ctx, &pipeline, output_fd);
  shell_pipeline_free(&pipeline);
  return result;
}
