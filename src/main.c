#include "terminator.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* SIGTERM and SIGHUP request cleanup; the GUI loop performs it safely. */
volatile sig_atomic_t terminator_stopping = 0;
static void stop_handler(int signal_number) {
  (void)signal_number;
  terminator_stopping = 1;
}

/* Initialize shared state, create the first tab, and hand control to X11. */
int main(int argument_count, char **arguments) {
  struct sigaction sa = {0};
  sa.sa_handler = stop_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  /* Threads are the default; the assignment's flag selects worker processes. */
  ExecutionMode mode = MODE_THREADS;
  if (argument_count == 2 && !strcmp(arguments[1], "useProcess=True"))
    mode = MODE_PROCESSES;
  else if (argument_count > 1) {
    fprintf(stderr, "usage: %s [useProcess=True]\n", arguments[0]);
    return 2;
  }
  App app = {0};
  app.x11_connection_fd = -1;
  app.mode = mode;
  if (mode == MODE_PROCESSES) {
    app.history = mmap(NULL, sizeof(*app.history), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (app.history == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
    if (history_init(app.history, 1) < 0) {
      perror("sem_init");
      munmap(app.history, sizeof(*app.history));
      return 1;
    }
  } else {
    app.history = calloc(1, sizeof(*app.history));
    if (!app.history) {
      perror("calloc");
      return 1;
    }
    if (history_init(app.history, 0) < 0) {
      perror("sem_init");
      free(app.history);
      return 1;
    }
  }
  if (app_add_tab(&app) < 0) {
    fprintf(stderr, "terminator: could not create initial tab\n");
    app_shutdown(&app);
    return 1;
  }
  int gui_status = gui_run(&app);
  app_shutdown(&app);
  return gui_status;
}
