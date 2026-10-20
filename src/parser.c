#include "shell.h"
#include <stdlib.h>
#include <string.h>

static int add_token(Tokens *tokens, const char *text, size_t length) {
  char *copy = malloc(length + 1);
  if (!copy)
    return -1;
  memcpy(copy, text, length);
  copy[length] = '\0';
  char **items = realloc(tokens->items, (tokens->count + 1) * sizeof(*items));
  if (!items) {
    free(copy);
    return -1;
  }
  tokens->items = items;
  tokens->items[tokens->count++] = copy;
  return 0;
}

int shell_is_complete(const char *text) {
  /* Keep collecting input while quotes, escapes, or $(...) remain open. */
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
      if (add_token(tokens, s, n))
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
    if (add_token(tokens, start, (size_t)(s - start)))
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
        int append = !strcmp(token, ">>");
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
        if (append)
          command->append = 1;
        continue;
      }
      char **argv = realloc(command->argv, (command->argc + 2) * sizeof(*argv));
      if (!argv)
        goto oom;
      command->argv = argv;
      command->argv[command->argc++] = strdup(token);
      if (!command->argv[command->argc - 1])
        goto oom;
      command->argv[command->argc] = NULL;
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
    if (!command->argc) {
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
    for (size_t j = 0; j < command->argc; ++j)
      free(command->argv[j]);
    free(command->argv);
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

static int append(char **b, size_t *n, size_t *cap, const char *s, size_t z) {
  if (*n + z + 1 > *cap) {
    size_t c = *cap ? *cap : 64;
    while (c < *n + z + 1)
      c *= 2;
    char *p = realloc(*b, c);
    if (!p)
      return -1;
    *b = p;
    *cap = c;
  }
  memcpy(*b + *n, s, z);
  *n += z;
  (*b)[*n] = 0;
  return 0;
}
static int has_glob(const char *s) {
  for (; *s; s++)
    if (*s == '*' || *s == '?' || *s == '[')
      return 1;
  return 0;
}
static int substitute(ShellContext *ctx, const char *cmd, size_t len,
                      char **value, int fd) {
  int p[2];
  if (pipe(p) < 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    ShellContext sub = *ctx;
    char *s = strndup(cmd, len);
    int rc = s ? shell_run_line(&sub, s, p[1]) : 1;
    free(s);
    close(p[1]);
    _exit(rc ? 1 : 0);
  }
  close(p[1]);
  char *buf = NULL;
  size_t n = 0, cap = 0;
  char tmp[512];
  ssize_t r;
  while ((r = read(p[0], tmp, sizeof(tmp))) > 0)
    if (append(&buf, &n, &cap, tmp, (size_t)r) < 0)
      break;
  close(p[0]);
  int st;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (!buf)
    buf = strdup("");
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;
  *value = buf;
  (void)fd;
  return 0;
}
int shell_expand_word(ShellContext *ctx, const char *in, char **out, int fd,
                      int *glob_eligible) {
  char *b = NULL;
  size_t n = 0, cap = 0;
  char q = 0;
  int glob_active = 0;
  for (size_t i = 0; in[i];) {
    char c = in[i];
    if (!q && (c == '\'' || c == '"')) {
      q = c;
      ++i;
      continue;
    }
    if (q == c) {
      q = 0;
      ++i;
      continue;
    }
    if (c == '\\' && q != '\'' && in[i + 1]) {
      char next = in[i + 1];
      if (q == '"' && next != '"' && next != '\\' && next != '$' &&
          next != '`' && next != '\n') {
        if (append(&b, &n, &cap, "\\", 1))
          goto fail;
        i++;
        continue;
      }
      if (next != '\n' && append(&b, &n, &cap, &next, 1))
        goto fail;
      i += 2;
      continue;
    }
    if (q != '\'' && c == '$') {
      if (in[i + 1] == '(') {
        size_t j = i + 2;
        int d = 1;
        char innerq = 0;
        while (in[j] && d) {
          if (innerq) {
            if (in[j] == innerq)
              innerq = 0;
          } else if (in[j] == '\'' || in[j] == '"')
            innerq = in[j];
          else if (in[j] == '(')
            d++;
          else if (in[j] == ')')
            d--;
          j++;
        }
        if (d) {
          errno = EINVAL;
          goto fail;
        }
        char *v = NULL;
        if (substitute(ctx, in + i + 2, j - i - 3, &v, fd) < 0)
          goto fail;
        if (!q && has_glob(v))
          glob_active = 1;
        if (append(&b, &n, &cap, v, strlen(v))) {
          free(v);
          goto fail;
        }
        free(v);
        i = j;
        continue;
      }
      size_t j = i + 1;
      char name[256];
      size_t k = 0;
      if (in[j] == '?') {
        snprintf(name, sizeof(name), "%d", ctx->last_status);
        j++;
      } else if (in[j] == '{') {
        j++;
        while (in[j] && in[j] != '}' && k < sizeof(name) - 1)
          name[k++] = in[j++];
        if (in[j] == '}')
          j++;
        name[k] = 0;
      } else if (isalpha((unsigned char)in[j]) || in[j] == '_') {
        while ((isalnum((unsigned char)in[j]) || in[j] == '_') &&
               k < sizeof(name) - 1)
          name[k++] = in[j++];
        name[k] = 0;
      } else {
        if (append(&b, &n, &cap, "$", 1))
          goto fail;
        i++;
        continue;
      }
      const char *v;
      if (name[0] && (!strcmp(name, "?") || isdigit((unsigned char)name[0])))
        v = name;
      else
        v = getenv(name);
      if (!q && v && has_glob(v))
        glob_active = 1;
      if (v && append(&b, &n, &cap, v, strlen(v)))
        goto fail;
      i = j;
      continue;
    }
    if (c == '`' && q != '\'') {
      size_t j = i + 1;
      while (in[j] && in[j] != '`')
        j++;
      if (!in[j])
        goto fail;
      char *v = NULL;
      if (substitute(ctx, in + i + 1, j - i - 1, &v, fd) < 0)
        goto fail;
      if (!q && has_glob(v))
        glob_active = 1;
      if (append(&b, &n, &cap, v, strlen(v))) {
        free(v);
        goto fail;
      }
      free(v);
      i = j + 1;
      continue;
    }
    if (!q && (c == '*' || c == '?' || c == '['))
      glob_active = 1;
    if (append(&b, &n, &cap, &c, 1))
      goto fail;
    i++;
  }
  if (!b)
    b = strdup("");
  *out = b;
  if (glob_eligible)
    *glob_eligible = glob_active;
  return 0;
fail:
  free(b);
  return -1;
}
