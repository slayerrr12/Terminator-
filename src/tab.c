#include "shell.h"
#include "terminator.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Each tab has GUI-owned ends for input, output, and status pipes. In thread
 * mode, the worker uses the other ends in the same process; in process mode,
 * the worker child owns them. Keep these descriptors local to the tab.
 */
static void set_close_on_exec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void *run_shell_worker_thread(void *argument);

/* Create a tab's three communication pipes and start its shell worker. */
int app_add_tab(App *app) {
  if (app->count >= TERM_MAX_TABS)
    return -1;
  size_t tab_index = app->count;
  char start_cwd[4096];
  if (app->count)
    memcpy(start_cwd, app->tabs[app->active].cwd, sizeof(start_cwd));
  else if (!getcwd(start_cwd, sizeof(start_cwd)))
    strcpy(start_cwd, "/");
  start_cwd[sizeof(start_cwd) - 1] = 0;
  Tab *tab = &app->tabs[tab_index];
  memset(tab, 0, sizeof(*tab));
  tab->id = (int)tab_index;
  tab->input_fd = tab->output_fd = tab->status_fd = -1;
  memcpy(tab->cwd, start_cwd, sizeof(tab->cwd));
  tab->foreground = mmap(NULL, sizeof(*tab->foreground), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tab->foreground == MAP_FAILED) {
    tab->foreground = NULL;
    return -1;
  }
  atomic_init(&tab->foreground->pgid, 0);
  /*
   * For each pipe, index 0 reads and index 1 writes:
   * GUI -> worker input, worker -> GUI output, worker -> GUI status.
   */
  int input_pipe[2], output_pipe[2], status_pipe[2];
  if (pipe(input_pipe) < 0) {
    munmap(tab->foreground, sizeof(*tab->foreground));
    return -1;
  }
  if (pipe(output_pipe) < 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    munmap(tab->foreground, sizeof(*tab->foreground));
    return -1;
  }
  if (pipe(status_pipe) < 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    munmap(tab->foreground, sizeof(*tab->foreground));
    return -1;
  }
  for (int pipe_end = 0; pipe_end < 2; pipe_end++) {
    set_close_on_exec(input_pipe[pipe_end]);
    set_close_on_exec(output_pipe[pipe_end]);
    set_close_on_exec(status_pipe[pipe_end]);
  }
  tab->history = app->history;
  if (app->mode == MODE_THREADS) {
    tab->input_fd = input_pipe[1];
    tab->output_fd = output_pipe[0];
    tab->status_fd = status_pipe[0];
    tab->worker_input_fd = input_pipe[0];
    tab->worker_output_fd = output_pipe[1];
    tab->worker_status_fd = status_pipe[1];
    tab->alive = tab->is_thread = 1;
    if (pthread_create(&tab->thread, NULL, run_shell_worker_thread, tab) != 0) {
      close(input_pipe[0]);
      close(input_pipe[1]);
      close(output_pipe[0]);
      close(output_pipe[1]);
      close(status_pipe[0]);
      close(status_pipe[1]);
      munmap(tab->foreground, sizeof(*tab->foreground));
      tab->foreground = NULL;
      return -1;
    }
  } else {
    pid_t pid = fork();
    if (pid == 0) {
      /* The worker must not keep GUI connections or other tabs' pipe ends. */
      if (app->x_fd >= 0)
        close(app->x_fd);
      for (size_t existing_tab_index = 0; existing_tab_index < app->count;
           existing_tab_index++) {
        if (app->tabs[existing_tab_index].input_fd >= 0)
          close(app->tabs[existing_tab_index].input_fd);
        if (app->tabs[existing_tab_index].output_fd >= 0)
          close(app->tabs[existing_tab_index].output_fd);
        if (app->tabs[existing_tab_index].status_fd >= 0)
          close(app->tabs[existing_tab_index].status_fd);
      }
      close(input_pipe[1]);
      close(output_pipe[0]);
      close(status_pipe[0]);
      (void)setpgid(0, 0);
      worker_run(input_pipe[0], output_pipe[1], status_pipe[1], app->history,
                 tab->foreground, (int)tab_index, tab->cwd);
      _exit(0);
    }
    if (pid < 0) {
      close(input_pipe[0]);
      close(input_pipe[1]);
      close(output_pipe[0]);
      close(output_pipe[1]);
      close(status_pipe[0]);
      close(status_pipe[1]);
      munmap(tab->foreground, sizeof(*tab->foreground));
      tab->foreground = NULL;
      return -1;
    }
    (void)setpgid(pid, pid);
    close(input_pipe[0]);
    close(output_pipe[1]);
    close(status_pipe[1]);
    tab->input_fd = input_pipe[1];
    tab->output_fd = output_pipe[0];
    tab->status_fd = status_pipe[0];
    tab->pid = pid;
    tab->alive = 1;
  }
  (void)fcntl(tab->output_fd, F_SETFL, O_NONBLOCK);
  (void)fcntl(tab->status_fd, F_SETFL, O_NONBLOCK);
  app->count++;
  app->active = tab_index;
  return (int)tab_index;
}
static void *run_shell_worker_thread(void *argument) {
  Tab *tab = argument;
  worker_run(tab->worker_input_fd, tab->worker_output_fd, tab->worker_status_fd,
             tab->history, tab->foreground, tab->id, tab->cwd);
  return NULL;
}
/* Stop the tab's active job, join/reap its worker, and release its buffers. */
void app_close_tab(App *app, size_t tab_index) {
  if (tab_index >= app->count)
    return;
  Tab *tab = &app->tabs[tab_index];
  /* Stop the active foreground group before joining or reaping its worker. */
  int pgid = atomic_load(&tab->foreground->pgid);
  if (pgid > 0)
    (void)kill(-pgid, SIGTERM);
  if (tab->input_fd >= 0) {
    close(tab->input_fd);
    tab->input_fd = -1;
  }
  if (tab->is_thread) {
    pthread_join(tab->thread, NULL);
  } else if (tab->pid > 0) {
    (void)kill(-tab->pid, SIGTERM);
    int worker_status;
    while (waitpid(tab->pid, &worker_status, 0) < 0 && errno == EINTR) {
    }
  }
  if (tab->output_fd >= 0)
    close(tab->output_fd);
  if (tab->status_fd >= 0)
    close(tab->status_fd);
  for (size_t i = 0; i < tab->line_count; i++)
    free(tab->lines[i]);
  free(tab->lines);
  if (tab->foreground)
    munmap(tab->foreground, sizeof(*tab->foreground));
  if (tab_index + 1 < app->count)
    memmove(&app->tabs[tab_index], &app->tabs[tab_index + 1],
            (app->count - tab_index - 1) * sizeof(Tab));
  app->count--;
  if (app->count) {
    if (app->active >= app->count)
      app->active = app->count - 1;
  }
}
void app_shutdown(App *app) {
  while (app->count)
    app_close_tab(app, app->count - 1);
  if (app->mode == MODE_PROCESSES && app->history) {
    sem_destroy(&app->history->lock);
    munmap(app->history, sizeof(*app->history));
  } else
    free(app->history);
}
