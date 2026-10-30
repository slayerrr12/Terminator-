#include "shell.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Converts shell text into tokens and pipeline structures. This module parses
 * syntax and expands words; process creation is handled by executor.c.
 */
static char *copy_text(const char *text, size_t length)
{
  char *copy = malloc(length + 1);
  if (!copy)
    return NULL;

  memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

static int append_token(Tokens *tokens, const char *text, size_t length)
{
  char *copy = copy_text(text, length);
  if (!copy)
    return -1;
  char **items = realloc(tokens->items, (tokens->count + 1) * sizeof(*items));
  if (!items)
  {
    free(copy);
    return -1;
  }
  tokens->items = items;
  tokens->items[tokens->count++] = copy;
  return 0;
}

/* Add one argument and keep the argument list NULL-terminated. */
static int append_argument(SimpleCommand *command, const char *text)
{
  char *copy = strdup(text);
  if (!copy)
    return -1;

  size_t new_count = command->argument_count + 1;
  char **arguments = realloc(command->arguments,
                             (new_count + 1) * sizeof(*arguments));
  if (!arguments)
  {
    free(copy);
    return -1;
  }

  command->arguments = arguments;
  command->arguments[command->argument_count] = copy;
  command->argument_count = new_count;
  command->arguments[new_count] = NULL;
  return 0;
}

/* These are the three redirection tokens supported by this shell. */
static int is_redirection(const char *token)
{
  return !strcmp(token, "<") || !strcmp(token, ">") ||
         !strcmp(token, ">>");
}

static int scan_word(const char *start, const char **end);
static int is_operator(char character);

/* Return false while the GUI should keep collecting a multiline command. */
int shell_is_complete(const char *text)
{
  if (!text)
    return 0;

  const char *position = text;
  while (*position)
  {
    /* Ignore whitespace between words and operators. */
    while (*position == ' ' || *position == '\t' || *position == '\n' ||
           *position == '\r')
      position++;

    if (!*position)
      break;

    /* Operators are complete tokens on their own. */
    if (is_operator(*position))
    {
      if (*position == '>' && position[1] == '>')
        position += 2;
      else
        position++;
      continue;
    }

    const char *word_end = NULL;
    if (scan_word(position, &word_end) < 0 || word_end == position)
      return 0;

    position = word_end;
  }

  return 1;
}

/* Split operators while keeping quoted operators inside words. */
int shell_lex(const char *text, Tokens *tokens, char **error)
{
  memset(tokens, 0, sizeof(*tokens));
  if (error)
    *error = NULL;

  const char *position = text;
  while (*position)
  {
    /* Match the whitespace rules used by the original lexer. */
    while (*position == ' ' || *position == '\t' || *position == '\n')
      position++;

    if (!*position)
      break;

    /* Operators split words only when they are outside quotes/substitutions. */
    if (is_operator(*position))
    {
      size_t operator_length =
          (*position == '>' && position[1] == '>') ? 2 : 1;
      if (append_token(tokens, position, operator_length) < 0)
        goto out_of_memory;

      position += operator_length;
      continue;
    }

    const char *word_start = position;
    if (scan_word(word_start, &position) < 0)
    {
      if (error)
        *error = strdup("incomplete quote or substitution");
      shell_tokens_free(tokens);
      return -1;
    }

    size_t word_length = (size_t)(position - word_start);
    if (append_token(tokens, word_start, word_length) < 0)
      goto out_of_memory;
  }

  return 0;

out_of_memory:
  if (error)
    *error = strdup("out of memory");
  shell_tokens_free(tokens);
  return -1;
}

/* Build commands, redirections, pipes, and trailing '&' from tokens. */
int shell_parse(const char *text, Tokens *tokens, Pipeline *out, char **error)
{
  memset(out, 0, sizeof(*out));
  out->source = strdup(text);
  if (!out->source)
    return -1;

  if (shell_lex(text, tokens, error) < 0)
    goto malformed;

  size_t command_capacity = 0;
  size_t token_count = tokens->count;
  size_t token_index = 0;

  while (token_index < token_count)
  {
    /* Make room for another command when the pipeline grows. */
    if (out->count == command_capacity)
    {
      size_t new_capacity = command_capacity ? command_capacity * 2 : 4;
      SimpleCommand *commands =
          realloc(out->commands, new_capacity * sizeof(*commands));
      if (!commands)
        goto out_of_memory;

      out->commands = commands;
      command_capacity = new_capacity;
    }

    SimpleCommand *command = &out->commands[out->count++];
    memset(command, 0, sizeof(*command));

    /* Read arguments and redirections until the next pipe or '&'. */
    while (token_index < token_count &&
           strcmp(tokens->items[token_index], "|") != 0 &&
           strcmp(tokens->items[token_index], "&") != 0)
    {
      char *token = tokens->items[token_index++];

      if (is_redirection(token))
      {
        int is_input = strcmp(token, "<") == 0;
        int append_to_file = strcmp(token, ">>") == 0;

        /* A redirection must be followed by a filename. */
        if (token_index >= token_count ||
            strcmp(tokens->items[token_index], "|") == 0 ||
            is_redirection(tokens->items[token_index]))
        {
          if (error)
            *error = strdup("redirection requires a filename");
          goto malformed;
        }

        char **path = is_input ? &command->input_path : &command->output_path;
        free(*path);
        *path = strdup(tokens->items[token_index++]);
        if (!*path)
          goto out_of_memory;

        if (append_to_file)
          command->append = 1;
        continue;
      }

      if (append_argument(command, token) < 0)
        goto out_of_memory;
    }

    /* Handle a pipe separator or the optional background marker. */
    if (token_index < token_count)
    {
      int is_background = strcmp(tokens->items[token_index], "&") == 0;
      token_index++;

      if (is_background)
      {
        /* This shell supports one trailing '&' for a whole pipeline. */
        if (token_index != token_count)
        {
          if (error)
            *error = strdup("'&' is only supported at the end of a command");
          goto malformed;
        }
        out->background = 1;
      }

      if (token_index == token_count && !is_background)
      {
        if (error)
          *error = strdup("pipeline requires a command after '|'");
        goto malformed;
      }
    }

    if (command->argument_count == 0)
    {
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

out_of_memory:
  if (error)
    *error = strdup("out of memory");
  shell_pipeline_free(out);
  shell_tokens_free(tokens);
  return -1;
}

void shell_tokens_free(Tokens *tokens)
{
  if (!tokens)
    return;
  for (size_t i = 0; i < tokens->count; ++i)
    free(tokens->items[i]);
  free(tokens->items);
  memset(tokens, 0, sizeof(*tokens));
}

void shell_pipeline_free(Pipeline *pipeline)
{
  if (!pipeline)
    return;
  for (size_t i = 0; i < pipeline->count; ++i)
  {
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
static int is_space(char character)
{
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r';
}

static int is_operator(char character)
{
  return character == '|' || character == '<' || character == '>' ||
         character == '&';
}

static const char *after_substitution(const char *start);
static const char *after_backticks(const char *start);

/* Find the closing backtick, skipping escaped ones and nested $(...) text. */
static const char *after_backticks(const char *start)
{
  const char *position = start + 1;
  char quote = 0;
  while (*position)
  {
    if (quote == '\'')
    {
      if (*position == '\'')
        quote = 0;
      position++;
      continue;
    }
    if (quote == '"')
    {
      if (*position == '\\' && position[1])
      {
        position += 2;
      }
      else if (*position == '"')
      {
        quote = 0;
        position++;
      }
      else if (*position == '$' && position[1] == '(')
      {
        const char *after = after_substitution(position);
        if (!after)
          return NULL;
        position = after;
      }
      else
      {
        position++;
      }
      continue;
    }
    if (*position == '\\' && position[1])
    {
      position += 2;
    }
    else if (*position == '\'' || *position == '"')
    {
      quote = *position++;
    }
    else if (*position == '$' && position[1] == '(')
    {
      const char *after = after_substitution(position);
      if (!after)
        return NULL;
      position = after;
    }
    else if (*position == '`')
    {
      return position + 1;
    }
    else
    {
      position++;
    }
  }
  return NULL;
}

/*
 * Find the matching ')' for $(...). The depth counter handles ordinary
 * parentheses, while the quote and substitution checks skip parentheses that
 * belong to quoted text or to a nested command substitution.
 */
static const char *after_substitution(const char *start)
{
  const char *position = start + 2;
  int depth = 1;
  char quote = 0;
  while (*position)
  {
    if (quote == '\'')
    {
      if (*position == '\'')
        quote = 0;
      position++;
      continue;
    }
    if (quote == '"')
    {
      if (*position == '\\' && position[1])
      {
        position += 2;
      }
      else if (*position == '"')
      {
        quote = 0;
        position++;
      }
      else if (*position == '$' && position[1] == '(')
      {
        const char *after = after_substitution(position);
        if (!after)
          return NULL;
        position = after;
      }
      else if (*position == '`')
      {
        const char *after = after_backticks(position);
        if (!after)
          return NULL;
        position = after;
      }
      else
      {
        position++;
      }
      continue;
    }
    if (*position == '\\')
    {
      if (!position[1])
        return NULL;
      position += 2;
    }
    else if (*position == '\'' || *position == '"')
    {
      quote = *position++;
    }
    else if (*position == '$' && position[1] == '(')
    {
      const char *after = after_substitution(position);
      if (!after)
        return NULL;
      position = after;
    }
    else if (*position == '`')
    {
      const char *after = after_backticks(position);
      if (!after)
        return NULL;
      position = after;
    }
    else if (*position == '(')
    {
      depth++;
      position++;
    }
    else if (*position == ')' && --depth == 0)
    {
      return position + 1;
    }
    else
    {
      position++;
    }
  }
  return NULL;
}

/* Scan one token while keeping quoted and substituted text together. */
static int scan_word(const char *start, const char **end)
{
  const char *position = start;
  char quote = 0;
  while (*position)
  {
    if (quote == '\'')
    {
      if (*position == '\'')
        quote = 0;
      position++;
      continue;
    }
    if (quote == '"')
    {
      if (*position == '\\')
      {
        if (!position[1])
          return -1;
        position += 2;
      }
      else if (*position == '"')
      {
        quote = 0;
        position++;
      }
      else if (*position == '$' && position[1] == '(')
      {
        const char *after = after_substitution(position);
        if (!after)
          return -1;
        position = after;
      }
      else if (*position == '`')
      {
        const char *after = after_backticks(position);
        if (!after)
          return -1;
        position = after;
      }
      else
      {
        position++;
      }
      continue;
    }
    if (is_space(*position) || is_operator(*position))
      break;
    if (*position == '\\')
    {
      if (!position[1])
        return -1;
      position += 2;
    }
    else if (*position == '\'' || *position == '"')
    {
      quote = *position++;
    }
    else if (*position == '$' && position[1] == '(')
    {
      const char *after = after_substitution(position);
      if (!after)
        return -1;
      position = after;
    }
    else if (*position == '`')
    {
      const char *after = after_backticks(position);
      if (!after)
        return -1;
      position = after;
    }
    else
    {
      position++;
    }
  }
  if (quote)
    return -1;
  *end = position;
  return 0;
}

/* Return true when the final backslash joins the next physical input line. */
int shell_trailing_backslash_continues(const char *text)
{
  typedef struct
  {
    char end_character;
    char quote;
    size_t parenthesis_depth;
  } QuoteFrame;

  if (!text)
    return 0;
  size_t length = strlen(text);
  if (!length || text[length - 1] != '\\')
    return 0;

  size_t slash_count = 0;
  while (slash_count < length && text[length - slash_count - 1] == '\\')
    slash_count++;
  if (slash_count % 2 == 0)
    return 0;

  /* Each frame tracks one active $(...) or backtick substitution. */
  QuoteFrame frames[256] = {{0}};
  size_t frame_count = 1;
  size_t index = 0;
  size_t scan_limit = length - 1;
  while (index < scan_limit)
  {
    QuoteFrame *frame = &frames[frame_count - 1];
    char current = text[index];

    if (frame->quote == '\'')
    {
      if (current == '\'')
        frame->quote = 0;
      index++;
      continue;
    }
    if (frame->quote == '"')
    {
      if (current == '\\' && index + 1 < scan_limit &&
          strchr("\"\\$`\n", text[index + 1]))
      {
        index += 2;
        continue;
      }
      if (current == '"')
      {
        frame->quote = 0;
        index++;
        continue;
      }
    }
    else
    {
      if (current == '\\')
      {
        index += index + 1 < scan_limit ? 2 : 1;
        continue;
      }
      if (current == '\'' || current == '"')
      {
        frame->quote = current;
        index++;
        continue;
      }
    }

    if (current == '$' && index + 1 < scan_limit && text[index + 1] == '(')
    {
      if (frame_count == sizeof(frames) / sizeof(frames[0]))
        return 0;
      frames[frame_count++] = (QuoteFrame){')', 0, 1};
      index += 2;
      continue;
    }
    if (current == '`' && frame->end_character == '`')
    {
      frame_count--;
      index++;
      continue;
    }
    if (current == '`')
    {
      if (frame_count == sizeof(frames) / sizeof(frames[0]))
        return 0;
      frames[frame_count++] = (QuoteFrame){'`', 0, 0};
      index++;
      continue;
    }
    if (frame->end_character == ')' && current == '(')
    {
      frame->parenthesis_depth++;
    }
    else if (frame->end_character == ')' && current == ')')
    {
      if (--frame->parenthesis_depth == 0)
        frame_count--;
    }
    index++;
  }

  return frames[frame_count - 1].quote != '\'';
}

/* Grow a null-terminated text buffer geometrically, then append one chunk. */
static int append_to_buffer(char **buffer, size_t *length, size_t *capacity,
                            const char *text, size_t text_length)
{
  if (*length + text_length + 1 > *capacity)
  {
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

/* Remove the escapes used to nest old-style backtick substitutions. */
static char *decode_backtick_command(const char *text, size_t length,
                                     size_t *decoded_length)
{
  char *decoded = malloc(length + 1);
  if (!decoded)
    return NULL;

  size_t source = 0;
  size_t target = 0;
  while (source < length)
  {
    if (text[source] == '\\' && source + 1 < length)
    {
      char next = text[source + 1];
      if (next == '\n')
      {
        source += 2;
        continue;
      }
      if (next == '$' || next == '`' || next == '\\')
      {
        decoded[target++] = next;
        source += 2;
        continue;
      }
    }
    decoded[target++] = text[source++];
  }
  decoded[target] = '\0';
  *decoded_length = target;
  return decoded;
}
static int contains_wildcard(const char *text)
{
  for (; *text; text++)
    if (*text == '*' || *text == '?' || *text == '[')
      return 1;
  return 0;
}

/* Append an expanded value and remember whether it enables filename matching. */
static int append_expansion(char **buffer, size_t *length, size_t *capacity,
                            const char *value, int check_wildcards,
                            int *glob_active)
{
  if (check_wildcards && contains_wildcard(value))
    *glob_active = 1;

  return append_to_buffer(buffer, length, capacity, value, strlen(value));
}
static int capture_command_output(ShellContext *shell, const char *command_text,
                                  size_t command_length,
                                  char **captured_output)
{
  /* A temporary file captures output without risking a full pipe buffer. */
  FILE *capture_file = tmpfile();
  if (!capture_file)
    return -1;

  char *nested_command = copy_text(command_text, command_length);
  if (!nested_command)
  {
    fclose(capture_file);
    return -1;
  }
  ShellContext nested_shell = *shell;
  int capture_fd = fileno(capture_file);
  shell_run_line(&nested_shell, nested_command, capture_fd);
  free(nested_command);
  if (fflush(capture_file) != 0 || lseek(capture_fd, 0, SEEK_SET) < 0)
  {
    fclose(capture_file);
    return -1;
  }

  char *capture_buffer = NULL;
  size_t captured_length = 0, capture_capacity = 0;
  char read_buffer[512];
  int capture_failed = 0;
  for (;;)
  {
    ssize_t bytes_read = read(capture_fd, read_buffer, sizeof(read_buffer));
    if (bytes_read > 0)
    {
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
  fclose(capture_file);
  if (capture_failed)
  {
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
                      char **expanded_word, int *glob_eligible)
{
  /* Remove quote syntax and expand variables, substitutions, and escapes. */
  char *expanded_buffer = NULL;
  size_t buffer_length = 0, buffer_capacity = 0;
  char active_quote = 0;
  int glob_active = 0;
  for (size_t input_index = 0; input_text[input_index];)
  {
    char current_char = input_text[input_index];
    if (!active_quote && (current_char == '\'' || current_char == '"'))
    {
      active_quote = current_char;
      ++input_index;
      continue;
    }
    if (active_quote == current_char)
    {
      active_quote = 0;
      ++input_index;
      continue;
    }
    if (current_char == '\\' && active_quote != '\'' &&
        input_text[input_index + 1])
    {
      char next_char = input_text[input_index + 1];
      if (active_quote == '"' && next_char != '"' && next_char != '\\' &&
          next_char != '$' && next_char != '`' && next_char != '\n')
      {
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
    if (active_quote != '\'' && current_char == '$')
    {
      if (input_text[input_index + 1] == '(')
      {
        const char *after = after_substitution(input_text + input_index);
        if (!after)
        {
          errno = EINVAL;
          goto fail;
        }
        size_t command_length =
            (size_t)(after - (input_text + input_index)) - 3;
        char *expansion_value = NULL;
        if (capture_command_output(shell, input_text + input_index + 2,
                                   command_length, &expansion_value) < 0)
          goto fail;
        int append_status = append_expansion(
            &expanded_buffer, &buffer_length, &buffer_capacity, expansion_value,
            !active_quote, &glob_active);
        free(expansion_value);
        if (append_status < 0)
          goto fail;
        input_index = (size_t)(after - input_text);
        continue;
      }
      size_t scan_index = input_index + 1;
      char variable_name[256];
      size_t name_length = 0;
      if (input_text[scan_index] == '?')
      {
        snprintf(variable_name, sizeof(variable_name), "%d",
                 shell->last_status);
        scan_index++;
      }
      else if (input_text[scan_index] == '{')
      {
        scan_index++;
        while (input_text[scan_index] && input_text[scan_index] != '}' &&
               name_length < sizeof(variable_name) - 1)
          variable_name[name_length++] = input_text[scan_index++];
        if (input_text[scan_index] == '}')
          scan_index++;
        variable_name[name_length] = 0;
      }
      else if (isalpha((unsigned char)input_text[scan_index]) ||
               input_text[scan_index] == '_')
      {
        while ((isalnum((unsigned char)input_text[scan_index]) ||
                input_text[scan_index] == '_') &&
               name_length < sizeof(variable_name) - 1)
          variable_name[name_length++] = input_text[scan_index++];
        variable_name[name_length] = 0;
      }
      else
      {
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
      if (expansion_value &&
          append_expansion(&expanded_buffer, &buffer_length, &buffer_capacity,
                           expansion_value, !active_quote, &glob_active) < 0)
        goto fail;
      input_index = scan_index;
      continue;
    }
    if (current_char == '`' && active_quote != '\'')
    {
      const char *after = after_backticks(input_text + input_index);
      if (!after)
        goto fail;
      size_t command_length =
          (size_t)(after - (input_text + input_index)) - 2;
      size_t decoded_length = 0;
      char *nested_command = decode_backtick_command(
          input_text + input_index + 1, command_length, &decoded_length);
      if (!nested_command)
        goto fail;
      char *expansion_value = NULL;
      int capture_status = capture_command_output(
          shell, nested_command, decoded_length, &expansion_value);
      free(nested_command);
      if (capture_status < 0)
        goto fail;
      int append_status = append_expansion(
          &expanded_buffer, &buffer_length, &buffer_capacity, expansion_value,
          !active_quote, &glob_active);
      free(expansion_value);
      if (append_status < 0)
        goto fail;
      input_index = (size_t)(after - input_text);
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
