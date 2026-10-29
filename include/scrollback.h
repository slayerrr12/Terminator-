#ifndef TERM_SCROLLBACK_H
#define TERM_SCROLLBACK_H

#include <stddef.h>

#define TERM_SCROLLBACK_MAX_LINES 10000
#define TERM_SCROLLBACK_MAX_BYTES (1024 * 1024)

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
} ScrollbackLine;

typedef struct {
  ScrollbackLine *lines;
  size_t line_count;
  size_t line_capacity;
  size_t oldest_line;
  size_t first_visible_line;
  size_t stored_bytes;
  int follows_output;
} Scrollback;

void scrollback_init(Scrollback *scrollback);
void scrollback_destroy(Scrollback *scrollback);
void scrollback_clear(Scrollback *scrollback);
int scrollback_append_text(Scrollback *scrollback, const char *text,
                           size_t length);
void scrollback_scroll_up(Scrollback *scrollback, size_t visible_rows,
                          size_t amount);
void scrollback_scroll_down(Scrollback *scrollback, size_t visible_rows,
                            size_t amount);
void scrollback_scroll_to(Scrollback *scrollback, size_t line,
                          size_t visible_rows);
size_t scrollback_first_visible(const Scrollback *scrollback,
                                size_t visible_rows);
const char *scrollback_get_line(const Scrollback *scrollback, size_t line);

#endif
