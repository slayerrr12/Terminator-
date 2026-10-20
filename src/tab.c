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
static int make_pipe(int p[2]) {
  if (pipe(p) < 0)
    return -1;
  return 0;
}
static void close_on_exec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
int app_add_tab(App *app) {
  if (app->count >= TERM_MAX_TABS)
    return -1;
  size_t ix = app->count;
  char start_cwd[4096];
  if (app->count)
    memcpy(start_cwd, app->tabs[app->active].cwd, sizeof(start_cwd));
  else if (!getcwd(start_cwd, sizeof(start_cwd)))
    strcpy(start_cwd, "/");
  start_cwd[sizeof(start_cwd) - 1] = 0;
  Tab *t = &app->tabs[ix];
  memset(t, 0, sizeof(*t));
  t->id = (int)ix;
  t->input_fd = t->output_fd = t->status_fd = -1;
  memcpy(t->cwd, start_cwd, sizeof(t->cwd));
  t->foreground = mmap(NULL, sizeof(*t->foreground), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (t->foreground == MAP_FAILED) {
    t->foreground = NULL;
    return -1;
  }
  atomic_init(&t->foreground->pgid, 0);
  int in[2], out[2], st[2];
  if (make_pipe(in) < 0) {
    munmap(t->foreground, sizeof(*t->foreground));
    return -1;
  }
  if (make_pipe(out) < 0) {
    close(in[0]);
    close(in[1]);
    munmap(t->foreground, sizeof(*t->foreground));
    return -1;
  }
  if (make_pipe(st) < 0) {
    close(in[0]);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    munmap(t->foreground, sizeof(*t->foreground));
    return -1;
  }
  for (int i = 0; i < 2; i++) {
    close_on_exec(in[i]);
    close_on_exec(out[i]);
    close_on_exec(st[i]);
  }
  t->history = app->history;
  if (app->mode == MODE_THREADS) {
    t->input_fd = in[1];
    t->output_fd = out[0];
    t->status_fd = st[0];
    t->worker_input_fd = in[0];
    t->worker_output_fd = out[1];
    t->worker_status_fd = st[1];
    t->alive = t->is_thread = 1;
    extern void *terminator_worker_thread(void *);
    if (pthread_create(&t->thread, NULL, terminator_worker_thread, t) != 0) {
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      close(st[0]);
      close(st[1]);
      munmap(t->foreground, sizeof(*t->foreground));
      t->foreground = NULL;
      return -1;
    }
  } else {
    pid_t pid = fork();
    if (pid == 0) {
      /* The worker must not keep GUI connections or other tabs' pipe ends. */
      if (app->x_fd >= 0)
        close(app->x_fd);
      for (size_t k = 0; k < app->count; k++) {
        if (app->tabs[k].input_fd >= 0)
          close(app->tabs[k].input_fd);
        if (app->tabs[k].output_fd >= 0)
          close(app->tabs[k].output_fd);
        if (app->tabs[k].status_fd >= 0)
          close(app->tabs[k].status_fd);
      }
      close(in[1]);
      close(out[0]);
      close(st[0]);
      (void)setpgid(0, 0);
      worker_run(in[0], out[1], st[1], app->history, t->foreground, (int)ix,
                 t->cwd);
      _exit(0);
    }
    if (pid < 0) {
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      close(st[0]);
      close(st[1]);
      munmap(t->foreground, sizeof(*t->foreground));
      t->foreground = NULL;
      return -1;
    }
    (void)setpgid(pid, pid);
    close(in[0]);
    close(out[1]);
    close(st[1]);
    t->input_fd = in[1];
    t->output_fd = out[0];
    t->status_fd = st[0];
    t->pid = pid;
    t->alive = 1;
  }
  (void)fcntl(t->output_fd, F_SETFL, O_NONBLOCK);
  (void)fcntl(t->status_fd, F_SETFL, O_NONBLOCK);
  app->count++;
  app->active = ix;
  return (int)ix;
}
void *terminator_worker_thread(void *arg) {
  Tab *t = arg;
  worker_run(t->worker_input_fd, t->worker_output_fd, t->worker_status_fd,
             t->history, t->foreground, t->id, t->cwd);
  return NULL;
}
void app_close_tab(App *app, size_t ix) {
  if (ix >= app->count)
    return;
  Tab *t = &app->tabs[ix];
  /* Stop the active foreground group before joining or reaping its worker. */
  int pgid = atomic_load(&t->foreground->pgid);
  if (pgid > 0)
    (void)kill(-pgid, SIGTERM);
  if (t->input_fd >= 0) {
    close(t->input_fd);
    t->input_fd = -1;
  }
  if (t->is_thread) {
    pthread_join(t->thread, NULL);
  } else if (t->pid > 0) {
    (void)kill(-t->pid, SIGTERM);
    int st;
    while (waitpid(t->pid, &st, 0) < 0 && errno == EINTR) {
    }
  }
  if (t->output_fd >= 0)
    close(t->output_fd);
  if (t->status_fd >= 0)
    close(t->status_fd);
  for (size_t i = 0; i < t->line_count; i++)
    free(t->lines[i]);
  free(t->lines);
  if (t->foreground)
    munmap(t->foreground, sizeof(*t->foreground));
  if (ix + 1 < app->count)
    memmove(&app->tabs[ix], &app->tabs[ix + 1],
            (app->count - ix - 1) * sizeof(Tab));
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
