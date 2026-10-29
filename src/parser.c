#include "shell.h"
#include <stdlib.h>
#include <string.h>

/*
 * Converts shell text into tokens and pipeline structures. This module parses
 * syntax and expands words; process creation is handled by executor.c.
 */
static char *copy_text(const char *text, size_t length) {
  char *copy = malloc(length + 1);
  if (!copy)
    return NULL;

  memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

static int append_token(Tokens *tokens, const char *text, size_t length) {
  char *copy = copy_text(text, length);
  if (!copy)
    return -1;
  char **items = realloc(tokens->items, (tokens->count + 1) * sizeof(*items));
  if (!items) {
    free(copy);
    return -1;
  }
  tokens->items = items;
  tokens->items[tokens->count++] = copy;
  return 0;
}

/* Return false while the GUI should keep collecting a multiline command. */
int shell_is_complete(const char *text) {
  /* Track open quotes, escapes, backticks, and nested command substitutions. */
  char quote = 0;
  int escaped = 0, depth = 0, backtick = 0;
  for (const char *s = text; *s; ++s) {
    if (escaped) {
      escaped = 0;
      continue;
    }
    if (*s == '\\' && quote != '\'') {
      escaped = 1;
      continue;
    }
    if (quote) {
      if (*s == quote)
        quote = 0;
      continue;
    }
    if (backtick) {
      if (*s == '`')
        backtick = 0;
      continue;
    }
    if (*s == '\'' || *s == '"')
      quote = *s;
    else if (*s == '`')
      backtick = 1;
    else if (*s == '$' && s[1] == '(') {
      depth++;
      ++s;
    } else if (*s == ')' && depth)
      depth--;
  }
  return !quote && !escaped && !backtick && depth == 0;
}

/* Split operators while keeping quoted operators inside words. */
int shell_lex(const char *text, Tokens *tokens, char **error) {
  memset(tokens, 0, sizeof(*tokens));
  if (error)
    *error = NULL;
  const char *s = text;
  while (*s) {
    while (*s == ' ' || *s == '\t' || *s == '\n')
      ++s;
    if (!*s)
      break;
    /* Operators split words only when they are outside quotes/substitutions. */
    if (*s == '|' || *s == '<' || *s == '>' || *s == '&') {
      size_t n = (*s == '>' && s[1] == '>') ? 2 : 1;
      if (append_token(tokens, s, n))
        goto oom;
      s += n;
      continue;
    }
    const char *start = s;
    char quote = 0;
    int escaped = 0, depth = 0, backtick = 0;
    while (*s) {
      if (escaped) {
        escaped = 0;
        ++s;
        continue;
      }
      if (*s == '\\' && quote != '\'') {
        escaped = 1;
        ++s;
        continue;
      }
      if (quote) {
        if (*s == quote)
          quote = 0;
        ++s;
        continue;
      }
      if (backtick) {
        if (*s == '`')
          backtick = 0;
        ++s;
        continue;
      }
      if (*s == '\'' || *s == '"') {
        quote = *s;
        ++s;
        continue;
      }
      if (*s == '`') {
        backtick = 1;
        ++s;
        continue;
      }
      if (*s == '$' && s[1] == '(') {
        depth++;
        s += 2;
        continue;
      }
      if (*s == ')' && depth) {
        depth--;
        ++s;
        continue;
      }
      if (!depth && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '|' ||
                     *s == '<' || *s == '>' || *s == '&'))
        break;
      ++s;
    }
    if (quote || backtick || depth || escaped) {
      if (error)
        *error = strdup("incomplete quote or substitution");
      shell_tokens_free(tokens);
      return -1;
    }
    if (append_token(tokens, start, (size_t)(s - start)))
      goto oom;
  }
  return 0;
oom:
  if (error)
    *error = strdup("out of memory");
  shell_tokens_free(tokens);
  return -1;
}
#include "shell.h"
#include <stdlib.h>
#include <string.h>

/* Build commands, redirections, pipes, and trailing '&' from tokens. */
int shell_parse(const char *text, Tokens *tokens, Pipeline *out, char **error) {
  memset(out, 0, sizeof(*out));
  out->source = strdup(text);
  if (!out->source)
    return -1;
  if (shell_lex(text, tokens, error) < 0)
    goto malformed;
  size_t capacity = 0;
  for (size_t i = 0; i < tokens->count;) {
    if (out->count == capacity) {
      capacity = capacity ? capacity * 2 : 4;
      SimpleCommand *commands =
          realloc(out->commands, capacity * sizeof(*commands));
      if (!commands)
        goto oom;
      out->commands = commands;
    }
    SimpleCommand *command = &out->commands[out->count++];
    memset(command, 0, sizeof(*command));
    while (i < tokens->count && strcmp(tokens->items[i], "|") &&
           strcmp(tokens->items[i], "&")) {
      char *token = tokens->items[i++];
      if (!strcmp(token, "<") || !strcmp(token, ">") || !strcmp(token, ">>")) {
        int input = !strcmp(token, "<");
        int append_to_file = !strcmp(token, ">>");
        if (i >= tokens->count || !strcmp(tokens->items[i], "|") ||
            !strcmp(tokens->items[i], "<") || !strcmp(tokens->items[i], ">") ||
            !strcmp(tokens->items[i], ">>")) {
          if (error)
            *error = strdup("redirection requires a filename");
          goto malformed;
        }
        char **path = input ? &command->input_path : &command->output_path;
        free(*path);
        *path = strdup(tokens->items[i++]);
        if (!*path)
          goto oom;
        if (append_to_file)
          command->append = 1;
        continue;
      }
      char **arguments =
          realloc(command->arguments,
                  (command->argument_count + 2) * sizeof(*arguments));
      if (!arguments)
        goto oom;
      command->arguments = arguments;
      command->arguments[command->argument_count++] = strdup(token);
      if (!command->arguments[command->argument_count - 1])
        goto oom;
      command->arguments[command->argument_count] = NULL;
    }
    if (i < tokens->count) {
      int background = !strcmp(tokens->items[i], "&");
      ++i;
      if (background) {
        /* This shell supports one trailing '&' for a whole pipeline. */
        if (i != tokens->count) {
          if (error)
            *error = strdup("'&' is only supported at the end of a command");
          goto malformed;
        }
        out->background = 1;
      }
      if (i == tokens->count) {
        if (!background && error)
          *error = strdup("pipeline requires a command after '|'");
        if (!background)
          goto malformed;
      }
    }
    if (!command->argument_count) {
      if (error)
        *error = strdup("empty command");
      goto malformed;
    }
  }
  return 0;
malformed:
  shell_pipeline_free(out);
  shell_tokens_free(tokens);
  return -1;
oom:
  if (error)
    *error = strdup("out of memory");
  shell_pipeline_free(out);
  shell_tokens_free(tokens);
  return -1;
}

void shell_tokens_free(Tokens *tokens) {
  if (!tokens)
    return;
  for (size_t i = 0; i < tokens->count; ++i)
    free(tokens->items[i]);
  free(tokens->items);
  memset(tokens, 0, sizeof(*tokens));
}

void shell_pipeline_free(Pipeline *pipeline) {
  if (!pipeline)
    return;
  for (size_t i = 0; i < pipeline->count; ++i) {
    SimpleCommand *command = &pipeline->commands[i];
    for (size_t j = 0; j < command->argument_count; ++j)
      free(command->arguments[j]);
    free(command->arguments);
    free(command->input_path);
    free(command->output_path);
  }
  free(pipeline->commands);
  free(pipeline->source);
  memset(pipeline, 0, sizeof(*pipeline));
}
#include "shell.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Grow a null-terminated text buffer geometrically, then append one chunk. */
static int append_to_buffer(char **buffer, size_t *length, size_t *capacity,
                            const char *text, size_t text_length) {
  if (*length + text_length + 1 > *capacity) {
    size_t new_capacity = *capacity ? *capacity : 64;
    while (new_capacity < *length + text_length + 1)
      new_capacity *= 2;
    char *grown_buffer = realloc(*buffer, new_capacity);
    if (!grown_buffer)
      return -1;
    *buffer = grown_buffer;
    *capacity = new_capacity;
  }
  memcpy(*buffer + *length, text, text_length);
  *length += text_length;
  (*buffer)[*length] = 0;
  return 0;
}
static int contains_wildcard(const char *text) {
  for (; *text; text++)
    if (*text == '*' || *text == '?' || *text == '[')
      return 1;
  return 0;
}
static int capture_command_output(ShellContext *shell, const char *command_text,
                                  size_t command_length,
                                  char **captured_output) {
  /* Run $(...) in a child and capture its output for the containing word. */
  int capture_pipe[2];
  if (pipe(capture_pipe) < 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    int saved_errno = errno;
    close(capture_pipe[0]);
    close(capture_pipe[1]);
    errno = saved_errno;
    return -1;
  }
  if (pid == 0) {
    close(capture_pipe[0]);
    ShellContext child_shell = *shell;
    char *nested_command = copy_text(command_text, command_length);
    int command_status =
        nested_command
            ? shell_run_line(&child_shell, nested_command, capture_pipe[1])
            : 1;
    free(nested_command);
    close(capture_pipe[1]);
    _exit(command_status ? 1 : 0);
  }
  close(capture_pipe[1]);
  char *capture_buffer = NULL;
  size_t captured_length = 0, capture_capacity = 0;
  char read_buffer[512];
  int capture_failed = 0;
  for (;;) {
    ssize_t bytes_read =
        read(capture_pipe[0], read_buffer, sizeof(read_buffer));
    if (bytes_read > 0) {
      if (!capture_failed &&
          append_to_buffer(&capture_buffer, &captured_length, &capture_capacity,
                           read_buffer, (size_t)bytes_read) < 0)
        capture_failed = 1;
      continue;
    }
    if (bytes_read < 0 && errno == EINTR)
      continue;
    if (bytes_read < 0)
      capture_failed = 1;
    break;
  }
  close(capture_pipe[0]);
  while (waitpid(pid, NULL, 0) < 0) {
    if (errno == EINTR)
      continue;
    capture_failed = 1;
    break;
  }
  if (capture_failed) {
    free(capture_buffer);
    return -1;
  }
  if (!capture_buffer)
    capture_buffer = strdup("");
  if (!capture_buffer)
    return -1;
  while (captured_length && (capture_buffer[captured_length - 1] == '\n' ||
                             capture_buffer[captured_length - 1] == '\r'))
    capture_buffer[--captured_length] = 0;
  *captured_output = capture_buffer;
  return 0;
}
int shell_expand_word(ShellContext *shell, const char *input_text,
                      char **expanded_word, int *glob_eligible) {
  /* Remove quote syntax and expand variables, substitutions, and escapes. */
  char *expanded_buffer = NULL;
  size_t buffer_length = 0, buffer_capacity = 0;
  char active_quote = 0;
  int glob_active = 0;
  for (size_t input_index = 0; input_text[input_index];) {
    char current_char = input_text[input_index];
    if (!active_quote && (current_char == '\'' || current_char == '"')) {
      active_quote = current_char;
      ++input_index;
      continue;
    }
    if (active_quote == current_char) {
      active_quote = 0;
      ++input_index;
      continue;
    }
    if (current_char == '\\' && active_quote != '\'' &&
        input_text[input_index + 1]) {
      char next_char = input_text[input_index + 1];
      if (active_quote == '"' && next_char != '"' && next_char != '\\' &&
          next_char != '$' && next_char != '`' && next_char != '\n') {
        if (append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                             "\\", 1))
          goto fail;
        input_index++;
        continue;
      }
      if (next_char != '\n' &&
          append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                           &next_char, 1))
        goto fail;
      input_index += 2;
      continue;
    }
    if (active_quote != '\'' && current_char == '$') {
      if (input_text[input_index + 1] == '(') {
        size_t scan_index = input_index + 2;
        int nesting_depth = 1;
        char inner_quote = 0;
        while (input_text[scan_index] && nesting_depth) {
          if (inner_quote) {
            if (input_text[scan_index] == inner_quote)
              inner_quote = 0;
          } else if (input_text[scan_index] == '\'' ||
                     input_text[scan_index] == '"')
            inner_quote = input_text[scan_index];
          else if (input_text[scan_index] == '(')
            nesting_depth++;
          else if (input_text[scan_index] == ')')
            nesting_depth--;
          scan_index++;
        }
        if (nesting_depth) {
          errno = EINVAL;
          goto fail;
        }
        char *expansion_value = NULL;
        if (capture_command_output(shell, input_text + input_index + 2,
                                   scan_index - input_index - 3,
                                   &expansion_value) < 0)
          goto fail;
        if (!active_quote && contains_wildcard(expansion_value))
          glob_active = 1;
        if (append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                             expansion_value, strlen(expansion_value))) {
          free(expansion_value);
          goto fail;
        }
        free(expansion_value);
        input_index = scan_index;
        continue;
      }
      size_t scan_index = input_index + 1;
      char variable_name[256];
      size_t name_length = 0;
      if (input_text[scan_index] == '?') {
        snprintf(variable_name, sizeof(variable_name), "%d",
                 shell->last_status);
        scan_index++;
      } else if (input_text[scan_index] == '{') {
        scan_index++;
        while (input_text[scan_index] && input_text[scan_index] != '}' &&
               name_length < sizeof(variable_name) - 1)
          variable_name[name_length++] = input_text[scan_index++];
        if (input_text[scan_index] == '}')
          scan_index++;
        variable_name[name_length] = 0;
      } else if (isalpha((unsigned char)input_text[scan_index]) ||
                 input_text[scan_index] == '_') {
        while ((isalnum((unsigned char)input_text[scan_index]) ||
                input_text[scan_index] == '_') &&
               name_length < sizeof(variable_name) - 1)
          variable_name[name_length++] = input_text[scan_index++];
        variable_name[name_length] = 0;
      } else {
        if (append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                             "$", 1))
          goto fail;
        input_index++;
        continue;
      }
      const char *expansion_value;
      if (variable_name[0] && (!strcmp(variable_name, "?") ||
                               isdigit((unsigned char)variable_name[0])))
        expansion_value = variable_name;
      else
        expansion_value = getenv(variable_name);
      if (!active_quote && expansion_value &&
          contains_wildcard(expansion_value))
        glob_active = 1;
      if (expansion_value &&
          append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                           expansion_value, strlen(expansion_value)))
        goto fail;
      input_index = scan_index;
      continue;
    }
    if (current_char == '`' && active_quote != '\'') {
      size_t scan_index = input_index + 1;
      while (input_text[scan_index] && input_text[scan_index] != '`')
        scan_index++;
      if (!input_text[scan_index])
        goto fail;
      char *expansion_value = NULL;
      if (capture_command_output(shell, input_text + input_index + 1,
                                 scan_index - input_index - 1,
                                 &expansion_value) < 0)
        goto fail;
      if (!active_quote && contains_wildcard(expansion_value))
        glob_active = 1;
      if (append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                           expansion_value, strlen(expansion_value))) {
        free(expansion_value);
        goto fail;
      }
      free(expansion_value);
      input_index = scan_index + 1;
      continue;
    }
    if (!active_quote &&
        (current_char == '*' || current_char == '?' || current_char == '['))
      glob_active = 1;
    if (append_to_buffer(&expanded_buffer, &buffer_length, &buffer_capacity,
                         &current_char, 1))
      goto fail;
    input_index++;
  }
  if (!expanded_buffer)
    expanded_buffer = strdup("");
  *expanded_word = expanded_buffer;
  if (glob_eligible)
    *glob_eligible = glob_active;
  return 0;
fail:
  free(expanded_buffer);
  return -1;
}
