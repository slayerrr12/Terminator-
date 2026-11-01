#ifndef TERMINATOR_H
#define TERMINATOR_H
#include "scrollback.h"
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>

#define TERM_MAX_TABS 16
#define TERM_HISTORY_CAP 10000
#define TERM_HISTORY_DISPLAY 1000
#define TERM_LINE_MAX 8192
#define TERM_CWD_MAX 1024
#define TERM_PATH_BUFFER 4096

typedef struct {
  int status;
  int clear_screen;
  char working_directory[TERM_CWD_MAX];
} WorkerStatus;

typedef struct {
  atomic_int pgid;
} SharedForeground;

extern volatile sig_atomic_t terminator_stopping;

typedef struct {
  sem_t semaphore;
  char file_path[TERM_PATH_BUFFER];
  size_t entry_count;
  off_t file_size;
} SharedHistory;

typedef enum { MODE_THREADS, MODE_PROCESSES } ExecutionMode;

typedef struct {
  /* Pipe ends used by the GUI. */
  int gui_to_worker_fd;
  int worker_to_gui_fd;
  int status_from_worker_fd;

  /* Pipe ends used by a worker thread in thread mode. */
  int worker_input_fd;
  int worker_output_fd;
  int worker_status_fd;

  pid_t worker_pid;
  pthread_t worker_thread;
  int worker_is_thread;
  int command_is_running;
  SharedHistory *history;
  SharedForeground *foreground;
  char working_directory[TERM_PATH_BUFFER];

  char input_buffer[TERM_LINE_MAX];
  size_t input_length;
  size_t input_cursor;
  size_t history_position;
  char history_draft[TERM_LINE_MAX];
  size_t history_draft_length;
  size_t history_draft_cursor;

  char multiline_buffer[TERM_LINE_MAX];
  size_t multiline_length;

  int history_search_active;
  char history_search_buffer[TERM_LINE_MAX];
  size_t history_search_length;

  int completion_waiting;
  char **completion_matches;
  size_t completion_match_count;
  size_t completion_insert_cursor;
  size_t completion_prefix_length;
  char completion_quote;
  char completion_choice[16];
  size_t completion_choice_length;

  Scrollback scrollback;
} Tab;

typedef struct {
  ExecutionMode mode;
  SharedHistory *history;
  Tab tabs[TERM_MAX_TABS];
  size_t tab_count;
  size_t active_tab;
  int x11_connection_fd;
} App;

int history_init(SharedHistory *history, int process_shared);
int history_add(SharedHistory *history, const char *line);
size_t history_snapshot(SharedHistory *history, char (*snapshot)[TERM_LINE_MAX],
                        size_t max_entries);
int history_get_recent(SharedHistory *history, size_t offset_from_newest,
                       char *buffer, size_t buffer_size);
typedef void (*HistorySearchCallback)(const char *command, void *context);
int history_search(SharedHistory *history, const char *term,
                   HistorySearchCallback callback, void *context,
                   size_t *best_match_length);
int app_add_tab(App *app);
void app_close_tab(App *app, size_t tab_index);
void app_shutdown(App *app);
void worker_run(int input_fd, int output_fd, int status_fd,
                SharedHistory *history, SharedForeground *foreground,
                const char *working_directory);
int gui_run(App *app);
#endif
