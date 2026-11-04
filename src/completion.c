#include "completion.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int is_word_boundary(char character) {
  return isspace((unsigned char)character) || strchr("|&;<>", character);
}

static char *join_path(const char *directory, const char *name) {
  size_t size = strlen(directory) + strlen(name) + 2;
  char *path = malloc(size);
  if (path)
    snprintf(path, size, "%s/%s", directory, name);
  return path;
}

static char *decode_prefix(const char *raw, size_t length, char *active_quote) {
  char *decoded = malloc(length + 1);
  if (!decoded)
    return NULL;
  size_t output = 0;
  char quote = 0;
  for (size_t i = 0; i < length; i++) {
    char current = raw[i];
    if (quote == '\'') {
      if (current == '\'')
        quote = 0;
      else
        decoded[output++] = current;
    } else if (quote == '"') {
      if (current == '"') {
        quote = 0;
      } else if (current == '\\' && i + 1 < length &&
                 (raw[i + 1] == '"' || raw[i + 1] == '\\' ||
                  raw[i + 1] == '$' || raw[i + 1] == '`')) {
        decoded[output++] = raw[++i];
      } else {
        decoded[output++] = current;
      }
    } else if (current == '\'' || current == '"') {
      quote = current;
    } else if (current == '\\' && i + 1 < length) {
      decoded[output++] = raw[++i];
    } else {
      decoded[output++] = current;
    }
  }
  decoded[output] = '\0';
  *active_quote = quote;
  return decoded;
}

static int append_candidate(CompletionResult *result, const char *name) {
  char **items = realloc(result->items, (result->count + 1) * sizeof(*items));
  if (!items)
    return -1;
  result->items = items;
  result->items[result->count] = strdup(name);
  if (!result->items[result->count])
    return -1;
  result->count++;
  return 0;
}

static int compare_names(const void *left, const void *right) {
  const char *const *first = left;
  const char *const *second = right;
  return strcmp(*first, *second);
}

static int find_matching_files(const char *working_directory,
                               const char *prefix, CompletionResult *result) {
  const char *last_slash = strrchr(prefix, '/');
  size_t directory_length = last_slash ? (size_t)(last_slash - prefix + 1) : 0;
  const char *base_name = prefix + directory_length;
  char *directory_part = strndup(prefix, directory_length);
  if (!directory_part)
    return -1;

  char *scan_path = directory_length && directory_part[0] == '/'
                        ? strdup(directory_part)
                        : join_path(working_directory, directory_part);
  free(directory_part);
  if (!scan_path)
    return -1;
  DIR *directory = opendir(scan_path);
  if (!directory) {
    free(scan_path);
    return 0;
  }

  struct dirent *entry;
  int result_code = 0;
  while ((entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (entry->d_name[0] == '.' && base_name[0] != '.')
      continue;
    if (strncmp(entry->d_name, base_name, strlen(base_name)))
      continue;

    char *full_path = join_path(scan_path, entry->d_name);
    struct stat info;
    if (!full_path || stat(full_path, &info) < 0) {
      free(full_path);
      continue;
    }
    free(full_path);

    size_t name_length = directory_length + strlen(entry->d_name) + 2;
    char *candidate = malloc(name_length);
    if (!candidate) {
      result_code = -1;
      break;
    }
    snprintf(candidate, name_length, "%.*s%s%s", (int)directory_length,
             prefix, entry->d_name, S_ISDIR(info.st_mode) ? "/" : "");
    /* Newline and carriage return split input into separate shell words. */
    if (!(strchr(candidate, '\n') || strchr(candidate, '\r')) &&
        append_candidate(result, candidate) < 0) {
      result_code = -1;
    }
    free(candidate);
    if (result_code < 0)
      break;
  }
  closedir(directory);
  free(scan_path);
  if (!result_code && result->count > 1)
    qsort(result->items, result->count, sizeof(*result->items), compare_names);
  return result_code;
}

int completion_find(const char *line, size_t cursor, const char *working_directory,
                   CompletionResult *result, char **decoded_prefix) {
  if (result)
    memset(result, 0, sizeof(*result));
  if (decoded_prefix)
    *decoded_prefix = NULL;
  if (!line || !working_directory || !result || !decoded_prefix) {
    errno = EINVAL;
    return -1;
  }
  size_t line_length = strlen(line);
  if (cursor > line_length)
    cursor = line_length;

  size_t token_start = 0;
  char quote = 0;
  int escaped = 0;
  for (size_t i = 0; i < cursor; i++) {
    char current = line[i];
    if (escaped) {
      escaped = 0;
      continue;
    }
    if (quote != '\'' && current == '\\') {
      escaped = 1;
      continue;
    }
    if (quote == '\'') {
      if (current == '\'')
        quote = 0;
      continue;
    }
    if (quote == '"') {
      if (current == '"')
        quote = 0;
      continue;
    }
    if (current == '\'' || current == '"') {
      quote = current;
    } else if (is_word_boundary(current)) {
      token_start = i + 1;
      quote = 0;
    }
  }

  char final_quote = 0;
  char *prefix = decode_prefix(line + token_start, cursor - token_start,
                               &final_quote);
  if (!prefix)
    return -1;
  result->token_start = token_start;
  result->prefix_length = strlen(prefix);
  result->quote = final_quote;
  *decoded_prefix = prefix;
  return find_matching_files(working_directory, prefix, result);
}

char *completion_common_prefix(char *const *items, size_t count) {
  if (!items || !count)
    return strdup("");
  size_t length = strlen(items[0]);
  for (size_t item = 1; item < count; item++) {
    size_t index = 0;
    while (index < length && items[item][index] &&
           items[item][index] == items[0][index])
      index++;
    length = index;
  }
  return strndup(items[0], length);
}

char *completion_encode_suffix(const char *suffix, char quote) {
  if (!suffix)
    return NULL;
  size_t length = strlen(suffix);
  char *encoded = malloc(length * 4 + 1);
  if (!encoded)
    return NULL;
  size_t output = 0;
  for (size_t i = 0; i < length; i++) {
    char character = suffix[i];
    if (quote == '\'') {
      if (character == '\'') {
        memcpy(encoded + output, "'\\''", 4);
        output += 4;
      } else {
        encoded[output++] = character;
      }
    } else if (quote == '"') {
      if (character == '"' || character == '\\' || character == '$' ||
          character == '`')
        encoded[output++] = '\\';
      encoded[output++] = character;
    } else {
      if (isspace((unsigned char)character) ||
          strchr("\\|&;<>$'\"`()!*?[]{}", character))
        encoded[output++] = '\\';
      encoded[output++] = character;
    }
  }
  encoded[output] = '\0';
  return encoded;
}

int completion_insert_text(char *line, size_t capacity, size_t *length,
                           size_t *cursor, const char *text) {
  if (!line || !length || !cursor || !text || !capacity || *cursor > *length ||
      *length >= capacity) {
    errno = EINVAL;
    return -1;
  }
  size_t available = capacity - 1 - *length;
  size_t inserted = strlen(text);
  if (inserted > available)
    inserted = available;
  memmove(line + *cursor + inserted, line + *cursor, *length - *cursor);
  memcpy(line + *cursor, text, inserted);
  *length += inserted;
  *cursor += inserted;
  line[*length] = '\0';
  return 0;
}

int completion_select(char *line, size_t capacity, size_t *length,
                      size_t *cursor, char *const *items, size_t count,
                      size_t one_based_choice, size_t prefix_length,
                      char quote) {
  if (!items || !one_based_choice || one_based_choice > count) {
    errno = EINVAL;
    return -1;
  }
  const char *candidate = items[one_based_choice - 1];
  size_t candidate_length = strlen(candidate);
  if (prefix_length > candidate_length) {
    errno = EINVAL;
    return -1;
  }
  char *suffix = completion_encode_suffix(candidate + prefix_length, quote);
  if (!suffix)
    return -1;
  int result = completion_insert_text(line, capacity, length, cursor, suffix);
  free(suffix);
  return result;
}

void completion_free(CompletionResult *result) {
  if (!result)
    return;
  for (size_t i = 0; i < result->count; i++)
    free(result->items[i]);
  free(result->items);
  memset(result, 0, sizeof(*result));
}
