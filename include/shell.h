#ifndef TERM_SHELL_H
#define TERM_SHELL_H
#include "terminator.h"
#include <stddef.h>

typedef struct {
  char **argv;
  size_t argc;
  char *input_path;
  char *output_path;
  int append;
} SimpleCommand;
typedef struct {
  SimpleCommand *commands;
  size_t count;
} Pipeline;
typedef struct {
  char cwd[4096];
  int last_status;
  int should_exit;
  SharedHistory *history;
  SharedForeground *foreground;
} ShellContext;
typedef struct {
  char **items;
  size_t count;
} Tokens;
int shell_is_complete(const char *s);
int shell_lex(const char *s, Tokens *tokens, char **error);
int shell_parse(const char *s, Tokens *tokens, Pipeline *out, char **error);
void shell_tokens_free(Tokens *tokens);
void shell_pipeline_free(Pipeline *p);
int shell_execute(ShellContext *ctx, Pipeline *p, int output_fd);
int shell_run_line(ShellContext *ctx, const char *line, int output_fd);
int shell_expand_word(ShellContext *ctx, const char *input, char **out,
                      int output_fd, int *glob_eligible);
int shell_builtin(ShellContext *ctx, char **argv, int output_fd);
#endif
