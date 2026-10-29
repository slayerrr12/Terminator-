#include "shell.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Applies redirections and launches parsed commands. Each pipeline gets one
 * process group so foreground waits, Ctrl+C, job control, and cleanup agree.
 */
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
static int append_argument(char ***args, size_t *count, char *value) {
  char **next = realloc(*args, (*count + 2) * sizeof(*next));
  if (!next)
    return -1;
  *args = next;
  (*args)[(*count)++] = value;
  (*args)[*count] = NULL;
  return 0;
}

static int expand_command_arguments(ShellContext *shell, SimpleCommand *command,
                                    int fd) {
  /* Expand each argument and glob eligible words before execvp(). */
  char **arguments = NULL;
  size_t argument_count = 0;
  for (size_t i = 0; i < command->argument_count; ++i) {
    char *word = NULL;
    int glob_eligible = 0;
    if (shell_expand_word(shell, command->arguments[i], &word, &glob_eligible) <
        0)
      goto fail;
    glob_t matches = {0};
    char *pattern = NULL;
    char *working_directory_prefix = NULL;
    int result = GLOB_NOMATCH;
    if (glob_eligible) {
      if (word[0] == '/')
        pattern = strdup(word);
      else {
        working_directory_prefix = shell_path_from_working_directory(
            shell->working_directory, "");
        pattern = shell_path_from_working_directory(shell->working_directory,
                                                    word);
      }
      if (!pattern || (word[0] != '/' && !working_directory_prefix)) {
        free(pattern);
        free(working_directory_prefix);
        free(word);
        goto fail;
      }
      result = glob(pattern, 0, NULL, &matches);
    }
    if (result == 0) {
      for (size_t j = 0; j < matches.gl_pathc; ++j) {
        const char *match = matches.gl_pathv[j];
        if (working_directory_prefix &&
            !strncmp(match, working_directory_prefix,
                     strlen(working_directory_prefix)))
          match += strlen(working_directory_prefix);
        char *copy = strdup(match);
        if (!copy || append_argument(&arguments, &argument_count, copy) < 0) {
          free(copy);
          globfree(&matches);
          free(pattern);
          free(working_directory_prefix);
          free(word);
          goto fail;
        }
      }
    } else {
      if (append_argument(&arguments, &argument_count, word) < 0) {
        free(word);
        globfree(&matches);
        free(pattern);
        free(working_directory_prefix);
        goto fail;
      }
      word = NULL;
    }
    free(word);
    globfree(&matches);
    free(pattern);
    free(working_directory_prefix);
  }
  if (!arguments)
    arguments = calloc(1, sizeof(*arguments));
  if (!arguments)
    goto fail;
  for (size_t i = 0; i < command->argument_count; ++i)
    free(command->arguments[i]);
  free(command->arguments);
  command->arguments = arguments;
  command->argument_count = argument_count;
  if (command->input_path) {
    char *expanded = NULL;
    if (shell_expand_word(shell, command->input_path, &expanded, NULL) < 0)
      return -1;
    free(command->input_path);
    command->input_path = expanded;
  }
  if (command->output_path) {
    char *expanded = NULL;
    if (shell_expand_word(shell, command->output_path, &expanded, NULL) < 0)
      return -1;
    free(command->output_path);
    command->output_path = expanded;
  }
  return 0;
fail:
  if (arguments) {
    for (size_t i = 0; i < argument_count; ++i)
      free(arguments[i]);
    free(arguments);
  }
  dprintf(fd, "terminator: expansion failed\n");
  return -1;
}

static int run_command_in_child(ShellContext *shell, SimpleCommand *command,
                                int output_fd) {
  if (chdir(shell->working_directory) < 0) {
    dprintf(output_fd, "terminator: %s: %s\n", shell->working_directory,
            strerror(errno));
    return 1;
  }
  if (redirection_apply(command, output_fd) < 0)
    return 1;
  int builtin = shell_builtin(shell, command->arguments, STDOUT_FILENO);
  if (builtin >= 0)
    return builtin;
  if (output_fd > STDERR_FILENO)
    close(output_fd);
  execvp(command->arguments[0], command->arguments);
  int err = errno;
  if (err == ENOENT)
    dprintf(STDERR_FILENO, "terminator: %s: command not found\n",
            command->arguments[0]);
  else
    dprintf(STDERR_FILENO, "terminator: %s: %s\n", command->arguments[0],
            strerror(err));
  return 127;
}

static ShellJob *find_job_by_id(ShellContext *shell,
                                const char *job_identifier) {
  /* Accept either a numeric job id or %id; with no id, choose the newest. */
  int id = 0;
  if (job_identifier && job_identifier[0] == '%')
    job_identifier++;
  if (job_identifier && *job_identifier) {
    char *end = NULL;
    long value = strtol(job_identifier, &end, 10);
    if (!end || *end || value <= 0 || value > INT_MAX)
      return NULL;
    id = (int)value;
  }
  ShellJob *latest = NULL;
  for (size_t i = 0; i < TERM_MAX_JOBS; ++i) {
    ShellJob *job = &shell->jobs[i];
    if (job->state == JOB_UNUSED || job->state == JOB_DONE)
      continue;
    if (id && job->id == id)
      return job;
    if (!id && (!latest || job->id > latest->id))
      latest = job;
  }
  return latest;
}

static ShellJob *record_job(ShellContext *shell, pid_t pgid, JobState state,
                            pid_t last_pid, const char *command) {
  /* Reuse completed slots so each tab's fixed-size job table stays bounded. */
  for (size_t i = 0; i < TERM_MAX_JOBS; ++i) {
    if (shell->jobs[i].state == JOB_UNUSED ||
        shell->jobs[i].state == JOB_DONE) {
      ShellJob *job = &shell->jobs[i];
      memset(job, 0, sizeof(*job));
      job->id = ++shell->next_job_id;
      job->pgid = pgid;
      job->last_pid = last_pid;
      job->state = state;
      snprintf(job->command, sizeof(job->command), "%s", command);
      return job;
    }
  }
  return NULL;
}

static int wait_for_process_group(ShellContext *shell, pid_t pgid,
                                  pid_t last_pid, int output_fd, int job_id,
                                  const char *command) {
  /* Wait for any child in the pipeline's process group, including stops. */
  int result = 0;
  int last_status = 0;
  int last_seen = 0;
  for (;;) {
    int status = 0;
    pid_t pid;
    do {
      pid = waitpid(-pgid, &status, WUNTRACED);
    } while (pid < 0 && errno == EINTR);
    if (pid > 0) {
      if (pid == last_pid) {
        last_status = status;
        last_seen = 1;
      }
      if (WIFSTOPPED(status)) {
        ShellJob *job = NULL;
        for (size_t i = 0; i < TERM_MAX_JOBS; ++i)
          if (shell->jobs[i].id == job_id && shell->jobs[i].state != JOB_UNUSED)
            job = &shell->jobs[i];
        if (!job)
          job = record_job(shell, pgid, JOB_STOPPED, last_pid, command);
        if (job)
          job->state = JOB_STOPPED;
        dprintf(output_fd, "[%d]+ Stopped\t%s\n", job ? job->id : job_id,
                job ? job->command : command);
        result = 128 + WSTOPSIG(status);
        break;
      }
      continue;
    }
    if (pid < 0 && errno == ECHILD) {
      if (last_seen && WIFEXITED(last_status))
        result = WEXITSTATUS(last_status);
      else if (last_seen && WIFSIGNALED(last_status))
        result = 128 + WTERMSIG(last_status);
      break;
    }
    if (pid < 0) {
      result = 1;
      break;
    }
  }
  if (shell->foreground)
    atomic_store(&shell->foreground->pgid, 0);
  return result;
}

static int execute_pipeline(ShellContext *shell, Pipeline *pipeline,
                            int output_fd) {
  size_t count = pipeline->count;
  int (*pipes)[2] = count > 1 ? calloc(count - 1, sizeof(*pipes)) : NULL;
  pid_t *pids = calloc(count, sizeof(*pids));
  if (!pids || (count > 1 && !pipes)) {
    free(pipes);
    free(pids);
    return 1;
  }
  /* A pipeline with N stages uses exactly N-1 inter-stage pipes. */
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
  pid_t pgid = shell->process_group, last_pid = 0;
  for (size_t i = 0; i < count; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      (void)setpgid(0, pgid ? pgid : 0);
      ShellContext child_shell = *shell;
      child_shell.process_group = pgid ? pgid : getpid();
      child_shell.foreground = NULL;
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
      _exit(run_command_in_child(&child_shell, &pipeline->commands[i],
                                 output_fd) &
            255);
    }
    if (pid < 0) {
      dprintf(output_fd, "terminator: fork: %s\n", strerror(errno));
      break;
    }
    if (i == 0) {
      if (!pgid)
        pgid = pid;
      if (shell->foreground)
        atomic_store(&shell->foreground->pgid, (int)pgid);
    }
    (void)setpgid(pid, pgid);
    pids[started++] = pid;
    last_pid = pid;
  }
  for (size_t i = 0; i + 1 < count; ++i) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  if (started != count) {
    if (pgid > 0)
      (void)kill(-pgid, SIGTERM);
    for (size_t i = 0; i < started; ++i)
      (void)waitpid(pids[i], NULL, 0);
    if (shell->foreground)
      atomic_store(&shell->foreground->pgid, 0);
    free(pipes);
    free(pids);
    return 1;
  }
  int result;
  if (pipeline->background) {
    ShellJob *job =
        record_job(shell, pgid, JOB_RUNNING, last_pid, pipeline->source);
    if (job)
      dprintf(output_fd, "[%d] %d\n", job->id, (int)pgid);
    else {
      dprintf(output_fd, "terminator: job table is full\n");
      (void)kill(-pgid, SIGTERM);
      for (size_t i = 0; i < started; ++i)
        (void)waitpid(pids[i], NULL, 0);
      result = 1;
    }
    if (shell->foreground)
      atomic_store(&shell->foreground->pgid, 0);
    result = job ? 0 : 1;
  } else {
    result = wait_for_process_group(shell, pgid, last_pid, output_fd, 0,
                                    pipeline->source);
  }
  free(pipes);
  free(pids);
  return result;
}

int shell_jobs_list(ShellContext *shell, int output_fd) {
  int result = 0;
  for (size_t i = 0; i < TERM_MAX_JOBS; ++i) {
    ShellJob *job = &shell->jobs[i];
    if (job->state == JOB_UNUSED)
      continue;
    if (job->state == JOB_RUNNING) {
      int status;
      pid_t pid;
      do {
        pid = waitpid(-job->pgid, &status, WNOHANG | WUNTRACED);
      } while (pid > 0 && !WIFSTOPPED(status));
      if (pid > 0 && WIFSTOPPED(status))
        job->state = JOB_STOPPED;
      else if (pid < 0 && errno == ECHILD)
        job->state = JOB_DONE;
      else if (pid < 0 && errno != EINTR)
        result = 1;
    }
    const char *state = job->state == JOB_RUNNING   ? "Running"
                        : job->state == JOB_STOPPED ? "Stopped"
                                                    : "Done";
    dprintf(output_fd, "[%d] %-7s %s\n", job->id, state, job->command);
    if (job->state == JOB_DONE)
      memset(job, 0, sizeof(*job));
  }
  return result;
}

int shell_job_background(ShellContext *shell, char **arguments, int output_fd) {
  ShellJob *job = find_job_by_id(shell, arguments[1]);
  if (!job) {
    dprintf(output_fd, "terminator: bg: no such job\n");
    return 1;
  }
  if (job->state == JOB_DONE) {
    dprintf(output_fd, "terminator: bg: job has already completed\n");
    return 1;
  }
  if (kill(-job->pgid, SIGCONT) < 0) {
    dprintf(output_fd, "terminator: bg: %s\n", strerror(errno));
    return 1;
  }
  job->state = JOB_RUNNING;
  dprintf(output_fd, "[%d]+ %s &\n", job->id, job->command);
  return 0;
}

int shell_job_foreground(ShellContext *shell, char **arguments, int output_fd) {
  ShellJob *job = find_job_by_id(shell, arguments[1]);
  if (!job) {
    dprintf(output_fd, "terminator: fg: no such job\n");
    return 1;
  }
  pid_t pgid = job->pgid;
  int id = job->id;
  if (job->state == JOB_STOPPED && kill(-pgid, SIGCONT) < 0) {
    dprintf(output_fd, "terminator: fg: %s\n", strerror(errno));
    return 1;
  }
  job->state = JOB_RUNNING;
  dprintf(output_fd, "%s\n", job->command);
  if (shell->foreground)
    atomic_store(&shell->foreground->pgid, (int)pgid);
  int status = wait_for_process_group(shell, pgid, job->last_pid, output_fd, id,
                                      job->command);
  for (size_t i = 0; i < TERM_MAX_JOBS; ++i)
    if (shell->jobs[i].id == id && shell->jobs[i].state != JOB_STOPPED)
      memset(&shell->jobs[i], 0, sizeof(shell->jobs[i]));
  return status;
}

void shell_jobs_cleanup(ShellContext *shell) {
  if (!shell)
    return;
  for (size_t i = 0; i < TERM_MAX_JOBS; ++i) {
    ShellJob *job = &shell->jobs[i];
    if (job->state == JOB_UNUSED || job->state == JOB_DONE)
      continue;
    /* Resume stopped jobs so they can receive SIGTERM and be reaped. */
    (void)kill(-job->pgid, SIGCONT);
    (void)kill(-job->pgid, SIGTERM);
    while (waitpid(-job->pgid, NULL, 0) > 0 || errno == EINTR) {
    }
    memset(job, 0, sizeof(*job));
  }
}

int shell_execute(ShellContext *shell, Pipeline *pipeline, int output_fd) {
  if (!pipeline->count)
    return 0;
  if (pipeline->count == 1) {
    SimpleCommand *command = &pipeline->commands[0];
    const char *name = command->arguments[0];
    /* Stateful built-ins must run here so cd and job changes persist. */
    int parent_builtin = !pipeline->background &&
                         (!strcmp(name, "cd") || !strcmp(name, "exit") ||
                          !strcmp(name, "clear") || !strcmp(name, "history") ||
                          !strcmp(name, "source") || !strcmp(name, "jobs") ||
                          !strcmp(name, "fg") || !strcmp(name, "bg") ||
                          !strcmp(name, "multiWatch"));
    if (parent_builtin) {
      int target = output_fd, opened = -1;
      if (command->input_path) {
        char *path = shell_path_from_working_directory(
            shell->working_directory, command->input_path);
        int fd = path ? redirection_open_input(path, output_fd) : -1;
        free(path);
        if (fd < 0) {
          shell->last_status = 1;
          return 1;
        }
        close(fd);
      }
      if (command->output_path) {
        char *path = shell_path_from_working_directory(
            shell->working_directory, command->output_path);
        opened = path
                     ? redirection_open_output(path, command->append, output_fd)
                     : -1;
        free(path);
        if (opened < 0) {
          shell->last_status = 1;
          return 1;
        }
        target = opened;
      }
      int result = shell_builtin(shell, command->arguments, target);
      if (opened >= 0)
        close(opened);
      shell->last_status = result;
      return result;
    }
  }
  int result = execute_pipeline(shell, pipeline, output_fd);
  shell->last_status = result;
  return result;
}

int shell_run_line(ShellContext *shell, const char *line, int output_fd) {
  /* Parse, expand, execute, and release one complete input line. */
  Tokens tokens = {0};
  Pipeline pipeline = {0};
  char *error = NULL;
  if (shell_parse(line, &tokens, &pipeline, &error) < 0) {
    dprintf(output_fd, "terminator: %s\n", error ? error : "parse error");
    free(error);
    shell->last_status = 2;
    return 2;
  }
  shell_tokens_free(&tokens);
  for (size_t i = 0; i < pipeline.count; ++i) {
    if (expand_command_arguments(shell, &pipeline.commands[i], output_fd) < 0) {
      shell_pipeline_free(&pipeline);
      shell->last_status = 1;
      return 1;
    }
  }
  int result = shell_execute(shell, &pipeline, output_fd);
  shell_pipeline_free(&pipeline);
  return result;
}
