#include "terminator.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

volatile sig_atomic_t terminator_stopping = 0;
static void stop_handler(int sig) {
  (void)sig;
  terminator_stopping = 1;
}

int main(int argc, char **argv) {
  struct sigaction sa = {0};
  sa.sa_handler = stop_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  ExecutionMode mode = MODE_THREADS;
  if (argc == 2 && !strcmp(argv[1], "useProcess=True"))
    mode = MODE_PROCESSES;
  else if (argc > 1) {
    fprintf(stderr, "usage: %s [useProcess=True]\n", argv[0]);
    return 2;
  }
  App app = {0};
  app.x_fd = -1;
  app.mode = mode;
  if (mode == MODE_PROCESSES) {
    app.history = mmap(NULL, sizeof(*app.history), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (app.history == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
    history_init(app.history, 1);
  } else {
    app.history = calloc(1, sizeof(*app.history));
    if (!app.history)
      return 1;
    history_init(app.history, 0);
  }
  if (app_add_tab(&app) < 0) {
    fprintf(stderr, "terminator: could not create initial tab\n");
    app_shutdown(&app);
    return 1;
  }
  int rc = gui_run(&app);
  app_shutdown(&app);
  return rc;
}
