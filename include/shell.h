#ifndef TERM_SHELL_H
#define TERM_SHELL_H
#include "terminator.h"
#include <stddef.h>

typedef struct {
  char **arguments;
  size_t argument_count;
  char *input_path;
  char *output_path;
  int append;
} SimpleCommand;
/* A parsed command line: one or more stages connected by pipes. */
typedef struct {
  SimpleCommand *commands;
  size_t count;
  int background;
  char *source;
} Pipeline;

#define TERM_MAX_JOBS 64

/* A job belongs to one tab and contains one or more child processes. */
typedef enum { JOB_UNUSED, JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct {
  int id;
  pid_t pgid;
  pid_t last_pid;
  JobState state;
  char command[TERM_LINE_MAX];
} ShellJob;

/* State that belongs to one tab's shell worker. */
typedef struct {
  char working_directory[TERM_PATH_BUFFER];
  int last_status;
  int should_exit;
  int clear_requested;
  int next_job_id;
  pid_t process_group;
  SharedHistory *history;
  SharedForeground *foreground;
  ShellJob jobs[TERM_MAX_JOBS];
} ShellContext;

typedef struct {
  char **items;
  size_t count;
} Tokens;

int shell_is_complete(const char *text);
int shell_lex(const char *text, Tokens *tokens, char **error);
int shell_parse(const char *text, Tokens *tokens, Pipeline *pipeline,
                char **error);
void shell_tokens_free(Tokens *tokens);
void shell_pipeline_free(Pipeline *pipeline);
/* Build an absolute path from this tab's logical current directory. */
char *shell_path_from_working_directory(const char *working_directory,
                                        const char *path);
int shell_execute(ShellContext *shell, Pipeline *pipeline, int output_fd);
int shell_run_line(ShellContext *shell, const char *line, int output_fd);
int shell_expand_word(ShellContext *shell, const char *input_text,
                      char **expanded_word, int *glob_eligible);
int shell_builtin(ShellContext *shell, char **arguments, int output_fd);
int shell_builtin_multiwatch(ShellContext *shell, char **arguments,
                             int output_fd);
void shell_jobs_cleanup(ShellContext *shell);
int shell_jobs_list(ShellContext *shell, int output_fd);
int shell_job_foreground(ShellContext *shell, char **arguments, int output_fd);
int shell_job_background(ShellContext *shell, char **arguments, int output_fd);
#endif
