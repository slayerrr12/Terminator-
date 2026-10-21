#ifndef TERMINATOR_H
#define TERMINATOR_H
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>

#define TERM_MAX_TABS 16
#define TERM_HISTORY_CAP 1024
#define TERM_LINE_MAX 8192
#define TERM_CWD_MAX 1024
typedef struct {
  int status;       /* Exit status of the command just handled. */
  int clear_screen; /* GUI should clear this tab's scrollback when nonzero. */
  char cwd[TERM_CWD_MAX]; /* Updated logical working directory. */
} WorkerStatus;
typedef struct {
  atomic_int pgid;
} SharedForeground;
extern volatile sig_atomic_t terminator_stopping;
typedef struct {
  sem_t lock;
  size_t count;
  size_t next;
  char entries[TERM_HISTORY_CAP][TERM_LINE_MAX];
} SharedHistory;
typedef enum { MODE_THREADS, MODE_PROCESSES } ExecutionMode;
typedef struct {
  int id, input_fd, output_fd, status_fd;
  int worker_input_fd, worker_output_fd, worker_status_fd;
  pid_t pid;
  pthread_t thread;
  int is_thread, alive, busy;
  SharedHistory *history;
  SharedForeground *foreground;
  char cwd[4096];
  char input[TERM_LINE_MAX];
  size_t input_len, cursor;
  char multiline[TERM_LINE_MAX];
  size_t multiline_len;
  char **lines;
  size_t line_count, line_cap, scroll;
} Tab;
typedef struct {
  ExecutionMode mode;
  SharedHistory *history;
  Tab tabs[TERM_MAX_TABS];
  size_t count, active;
  int x_fd;
} App;

void history_init(SharedHistory *h, int process_shared);
void history_add(SharedHistory *h, const char *line);
size_t history_snapshot(SharedHistory *h, char (*out)[TERM_LINE_MAX],
                        size_t cap);
int app_add_tab(App *app);
void app_close_tab(App *app, size_t index);
void app_shutdown(App *app);
void worker_run(int input_fd, int output_fd, int status_fd,
                SharedHistory *history, SharedForeground *foreground,
                int tab_id, const char *cwd);
int gui_run(App *app);
#endif
