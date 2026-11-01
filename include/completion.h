#ifndef TERM_COMPLETION_H
#define TERM_COMPLETION_H

#include <stddef.h>

typedef struct {
  char **items;
  size_t count;
  size_t token_start;
  size_t prefix_length;
  char quote;
} CompletionResult;

/* Find the current word, decode its prefix, and list matching file names. */
int completion_find(const char *line, size_t cursor, const char *working_directory,
                   CompletionResult *result, char **decoded_prefix);
char *completion_common_prefix(char *const *items, size_t count);
char *completion_encode_suffix(const char *suffix, char quote);
int completion_insert_text(char *line, size_t capacity, size_t *length,
                           size_t *cursor, const char *text);
int completion_select(char *line, size_t capacity, size_t *length,
                      size_t *cursor, char *const *items, size_t count,
                      size_t one_based_choice, size_t prefix_length,
                      char quote);
void completion_free(CompletionResult *result);

#endif
