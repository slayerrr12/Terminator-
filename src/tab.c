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

static void close_pipe_pair(int pipe_ends[2]) {
  for (int end = 0; end < 2; end++) {
    if (pipe_ends[end] >= 0) {
      close(pipe_ends[end]);
      pipe_ends[end] = -1;
    }
  }
}

static void close_tab_pipes(int input_pipe[2], int output_pipe[2],
                            int status_pipe[2]) {
  close_pipe_pair(input_pipe);
  close_pipe_pair(output_pipe);
  close_pipe_pair(status_pipe);
}

static int create_tab_pipes(int input_pipe[2], int output_pipe[2],
                            int status_pipe[2]) {
  input_pipe[0] = input_pipe[1] = -1;
  output_pipe[0] = output_pipe[1] = -1;
  status_pipe[0] = status_pipe[1] = -1;

  if (pipe(input_pipe) < 0 || pipe(output_pipe) < 0 || pipe(status_pipe) < 0) {
    close_tab_pipes(input_pipe, output_pipe, status_pipe);
    return -1;
  }
  return 0;
}

static void *run_shell_worker_thread(void *argument);

/* Create a tab's three communication pipes and start its shell worker. */
int app_add_tab(App *app) {
  if (app->tab_count >= TERM_MAX_TABS)
    return -1;
  size_t tab_index = app->tab_count;
  char initial_working_directory[TERM_PATH_BUFFER];
  if (app->tab_count)
    memcpy(initial_working_directory,
           app->tabs[app->active_tab].working_directory,
           sizeof(initial_working_directory));
  else if (!getcwd(initial_working_directory,
                   sizeof(initial_working_directory)))
    strcpy(initial_working_directory, "/");
  initial_working_directory[sizeof(initial_working_directory) - 1] = 0;
  Tab *tab = &app->tabs[tab_index];
  memset(tab, 0, sizeof(*tab));
  tab->gui_to_worker_fd = -1;
  tab->worker_to_gui_fd = -1;
  tab->status_from_worker_fd = -1;
  scrollback_init(&tab->scrollback);
  memcpy(tab->working_directory, initial_working_directory,
         sizeof(tab->working_directory));
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
  if (create_tab_pipes(input_pipe, output_pipe, status_pipe) < 0) {
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
    tab->gui_to_worker_fd = input_pipe[1];
    tab->worker_to_gui_fd = output_pipe[0];
    tab->status_from_worker_fd = status_pipe[0];
    tab->worker_input_fd = input_pipe[0];
    tab->worker_output_fd = output_pipe[1];
    tab->worker_status_fd = status_pipe[1];
    tab->worker_is_thread = 1;
    if (pthread_create(&tab->worker_thread, NULL, run_shell_worker_thread,
                       tab) != 0) {
      close_tab_pipes(input_pipe, output_pipe, status_pipe);
      munmap(tab->foreground, sizeof(*tab->foreground));
      tab->foreground = NULL;
      return -1;
    }
  } else {
    pid_t worker_pid = fork();
    if (worker_pid == 0) {
      /* The worker must not keep GUI connections or other tabs' pipe ends. */
      if (app->x11_connection_fd >= 0)
        close(app->x11_connection_fd);
      for (size_t existing_tab_index = 0; existing_tab_index < app->tab_count;
           existing_tab_index++) {
        if (app->tabs[existing_tab_index].gui_to_worker_fd >= 0)
          close(app->tabs[existing_tab_index].gui_to_worker_fd);
        if (app->tabs[existing_tab_index].worker_to_gui_fd >= 0)
          close(app->tabs[existing_tab_index].worker_to_gui_fd);
        if (app->tabs[existing_tab_index].status_from_worker_fd >= 0)
          close(app->tabs[existing_tab_index].status_from_worker_fd);
      }
      close(input_pipe[1]);
      close(output_pipe[0]);
      close(status_pipe[0]);
      (void)setpgid(0, 0);
      worker_run(input_pipe[0], output_pipe[1], status_pipe[1], app->history,
                 tab->foreground, tab->working_directory);
      _exit(0);
    }
    if (worker_pid < 0) {
      close_tab_pipes(input_pipe, output_pipe, status_pipe);
      munmap(tab->foreground, sizeof(*tab->foreground));
      tab->foreground = NULL;
      return -1;
    }
    (void)setpgid(worker_pid, worker_pid);
    close(input_pipe[0]);
    close(output_pipe[1]);
    close(status_pipe[1]);
    tab->gui_to_worker_fd = input_pipe[1];
    tab->worker_to_gui_fd = output_pipe[0];
    tab->status_from_worker_fd = status_pipe[0];
    tab->worker_pid = worker_pid;
  }
  (void)fcntl(tab->worker_to_gui_fd, F_SETFL, O_NONBLOCK);
  (void)fcntl(tab->status_from_worker_fd, F_SETFL, O_NONBLOCK);
  app->tab_count++;
  app->active_tab = tab_index;
  return (int)tab_index;
}
static void *run_shell_worker_thread(void *argument) {
  Tab *tab = argument;
  worker_run(tab->worker_input_fd, tab->worker_output_fd, tab->worker_status_fd,
             tab->history, tab->foreground, tab->working_directory);
  return NULL;
}
/* Stop the tab's active job, join/reap its worker, and release its buffers. */
void app_close_tab(App *app, size_t tab_index) {
  if (tab_index >= app->tab_count)
    return;
  Tab *tab = &app->tabs[tab_index];
  /* Stop the active foreground group before joining or reaping its worker. */
  int pgid = atomic_load(&tab->foreground->pgid);
  if (pgid > 0)
    (void)kill(-pgid, SIGTERM);
  if (tab->gui_to_worker_fd >= 0) {
    close(tab->gui_to_worker_fd);
    tab->gui_to_worker_fd = -1;
  }
  if (tab->worker_is_thread) {
    pthread_join(tab->worker_thread, NULL);
  } else if (tab->worker_pid > 0) {
    (void)kill(-tab->worker_pid, SIGTERM);
    int worker_status;
    while (waitpid(tab->worker_pid, &worker_status, 0) < 0 && errno == EINTR) {
    }
  }
  if (tab->worker_to_gui_fd >= 0)
    close(tab->worker_to_gui_fd);
  if (tab->status_from_worker_fd >= 0)
    close(tab->status_from_worker_fd);
  scrollback_destroy(&tab->scrollback);
  if (tab->foreground)
    munmap(tab->foreground, sizeof(*tab->foreground));
  if (tab_index + 1 < app->tab_count)
    memmove(&app->tabs[tab_index], &app->tabs[tab_index + 1],
            (app->tab_count - tab_index - 1) * sizeof(Tab));
  app->tab_count--;
  if (app->tab_count) {
    if (app->active_tab >= app->tab_count)
      app->active_tab = app->tab_count - 1;
  }
}
void app_shutdown(App *app) {
  while (app->tab_count)
    app_close_tab(app, app->tab_count - 1);
  if (!app->history)
    return;

  sem_destroy(&app->history->semaphore);
  if (app->mode == MODE_PROCESSES)
    munmap(app->history, sizeof(*app->history));
  else
    free(app->history);
}
