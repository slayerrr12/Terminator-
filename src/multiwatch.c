#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Runs a set of shell commands concurrently, captures each output stream, and
 * restarts finished commands on the watch interval.
 */
typedef struct {
  const char *command;
  pid_t pid;
  int output_fd;
  time_t restart_at;
} WatchCommand;

/* Start one command and give the supervisor a nonblocking output pipe. */
static int start_watched_command(WatchCommand *watch, ShellContext *shell,
                                 int process_group, int output_fd) {
  int output[2];
  if (pipe(output) < 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(output[0]);
    (void)setpgid(0, process_group);
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDIN_FILENO);
      close(null_fd);
    }
    (void)dup2(output[1], STDOUT_FILENO);
    (void)dup2(output[1], STDERR_FILENO);
    close(output[1]);
    ShellContext child_context = *shell;
    child_context.foreground = NULL;
    child_context.process_group = process_group;
    _exit(shell_run_line(&child_context, watch->command, STDOUT_FILENO) & 255);
  }
  close(output[1]);
  if (pid < 0) {
    close(output[0]);
    return -1;
  }
  (void)setpgid(pid, process_group);
  (void)fcntl(output[0], F_SETFL, O_NONBLOCK);
  watch->pid = pid;
  watch->output_fd = output[0];
  time_t now = time(NULL);
  dprintf(output_fd, "\"%s\", %lld:\n", watch->command, (long long)now);
  dprintf(output_fd, "----------------------------------------------------\n");
  return 0;
}

/* Run every command concurrently and forward ready pipe data as it arrives. */
static int run_watch_supervisor(WatchCommand *commands, size_t count,
                                ShellContext *shell, int output_fd) {
  pid_t group = shell->process_group;
  if (group <= 0) {
    (void)setpgid(0, 0);
    group = getpgrp();
  }
  for (size_t i = 0; i < count; ++i) {
    commands[i].pid = 0;
    commands[i].output_fd = -1;
    commands[i].restart_at = 0;
    if (start_watched_command(&commands[i], shell, (int)group, output_fd) < 0) {
      dprintf(output_fd, "terminator: multiWatch: %s\n", strerror(errno));
      commands[i].restart_at = time(NULL) + 2;
    }
  }

  for (;;) {
    struct pollfd poll_fds[TERM_MAX_JOBS];
    size_t indexes[TERM_MAX_JOBS];
    nfds_t poll_count = 0;
    time_t now = time(NULL);
    int timeout_ms = 2000;
    for (size_t i = 0; i < count; ++i) {
      if (commands[i].output_fd >= 0) {
        poll_fds[poll_count] =
            (struct pollfd){commands[i].output_fd, POLLIN | POLLHUP, 0};
        indexes[poll_count++] = i;
      } else if (commands[i].restart_at <= now) {
        if (start_watched_command(&commands[i], shell, (int)group, output_fd) <
            0) {
          dprintf(output_fd, "terminator: multiWatch: %s\n", strerror(errno));
          commands[i].restart_at = now + 2;
        }
      } else {
        time_t remaining = commands[i].restart_at - now;
        if (remaining * 1000 < timeout_ms)
          timeout_ms = (int)(remaining * 1000);
      }
    }
    int ready = poll(poll_fds, poll_count, timeout_ms);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      break;
    for (nfds_t p = 0; p < poll_count; ++p) {
      if (!poll_fds[p].revents)
        continue;
      WatchCommand *watch = &commands[indexes[p]];
      char buffer[2048];
      ssize_t n;
      while ((n = read(watch->output_fd, buffer, sizeof(buffer))) > 0) {
        size_t sent = 0;
        while (sent < (size_t)n) {
          ssize_t written = write(output_fd, buffer + sent, (size_t)n - sent);
          if (written < 0 && errno == EINTR)
            continue;
          if (written <= 0)
            break;
          sent += (size_t)written;
        }
      }
      if (n == 0 || (poll_fds[p].revents & (POLLHUP | POLLERR))) {
        (void)waitpid(watch->pid, NULL, 0);
        close(watch->output_fd);
        watch->output_fd = -1;
        watch->pid = 0;
        dprintf(output_fd,
                "----------------------------------------------------\n");
        watch->restart_at = time(NULL) + 2;
      }
    }
  }
  /* On a supervisor error, stop and reap its direct command workers. */
  for (size_t i = 0; i < count; ++i) {
    if (commands[i].output_fd >= 0)
      close(commands[i].output_fd);
    if (commands[i].pid > 0) {
      (void)kill(commands[i].pid, SIGTERM);
      (void)waitpid(commands[i].pid, NULL, 0);
    }
  }
  return 1;
}

int shell_builtin_multiwatch(ShellContext *shell, char **arguments,
                             int output_fd) {
  size_t count = 0;
  for (size_t i = 1; arguments[i]; ++i) {
    char *argument = arguments[i];
    if (*argument == '[')
      ++argument;
    size_t length = strlen(argument);
    while (length &&
           (argument[length - 1] == ',' || argument[length - 1] == ']'))
      argument[--length] = '\0';
    if (length)
      ++count;
  }
  if (!count || count > TERM_MAX_JOBS) {
    dprintf(output_fd, "terminator: multiWatch: expected 1-%d commands\n",
            TERM_MAX_JOBS);
    return 2;
  }
  WatchCommand *commands = calloc(count, sizeof(*commands));
  if (!commands)
    return 1;
  size_t index = 0;
  for (size_t i = 1; arguments[i]; ++i) {
    char *argument = arguments[i];
    if (*argument == '[')
      ++argument;
    size_t length = strlen(argument);
    while (length &&
           (argument[length - 1] == ',' || argument[length - 1] == ']'))
      argument[--length] = '\0';
    if (length)
      commands[index++].command = argument;
  }
  /* A background invocation already runs in its tracked job process group. */
  if (shell->process_group > 0) {
    int result = run_watch_supervisor(commands, count, shell, output_fd);
    free(commands);
    return result;
  }
  pid_t supervisor = fork();
  if (supervisor == 0) {
    int result = run_watch_supervisor(commands, count, shell, output_fd);
    free(commands);
    _exit(result);
  }
  free(commands);
  if (supervisor < 0) {
    dprintf(output_fd, "terminator: multiWatch: %s\n", strerror(errno));
    return 1;
  }
  (void)setpgid(supervisor, supervisor);
  if (shell->foreground)
    atomic_store(&shell->foreground->pgid, (int)supervisor);
  int status = 0;
  while (waitpid(supervisor, &status, WUNTRACED) < 0 && errno == EINTR) {
  }
  if (shell->foreground)
    atomic_store(&shell->foreground->pgid, 0);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
