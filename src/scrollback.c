#include "scrollback.h"
#include <stdlib.h>
#include <string.h>

static size_t line_slot(const Scrollback *scrollback, size_t line) {
  return (scrollback->oldest_line + line) % scrollback->line_capacity;
}

static size_t maximum_first_line(const Scrollback *scrollback,
                                 size_t visible_rows) {
  if (!visible_rows)
    visible_rows = 1;
  if (scrollback->line_count <= visible_rows)
    return 0;
  return scrollback->line_count - visible_rows;
}

size_t scrollback_first_visible(const Scrollback *scrollback,
                                size_t visible_rows) {
  size_t maximum = maximum_first_line(scrollback, visible_rows);
  if (scrollback->follows_output)
    return maximum;
  return scrollback->first_visible_line < maximum
             ? scrollback->first_visible_line
             : maximum;
}

const char *scrollback_get_line(const Scrollback *scrollback, size_t line) {
  if (!scrollback || line >= scrollback->line_count)
    return "";

  const ScrollbackLine *entry = &scrollback->lines[line_slot(scrollback, line)];
  return entry->text ? entry->text : "";
}

static int grow_line_slots(Scrollback *scrollback, size_t required) {
  if (required <= scrollback->line_capacity)
    return 0;

  size_t new_capacity = scrollback->line_capacity
                            ? scrollback->line_capacity * 2
                            : 64;
  while (new_capacity < required) {
    if (new_capacity > TERM_SCROLLBACK_MAX_LINES / 2) {
      new_capacity = TERM_SCROLLBACK_MAX_LINES;
      break;
    }
    new_capacity *= 2;
  }

  ScrollbackLine *new_lines = calloc(new_capacity, sizeof(*new_lines));
  if (!new_lines)
    return -1;

  for (size_t line = 0; line < scrollback->line_count; line++)
    new_lines[line] = scrollback->lines[line_slot(scrollback, line)];

  free(scrollback->lines);
  scrollback->lines = new_lines;
  scrollback->line_capacity = new_capacity;
  scrollback->oldest_line = 0;
  return 0;
}

static void remove_oldest_line(Scrollback *scrollback) {
  ScrollbackLine *oldest = &scrollback->lines[scrollback->oldest_line];
  scrollback->stored_bytes -= oldest->capacity;
  free(oldest->text);
  memset(oldest, 0, sizeof(*oldest));
  scrollback->oldest_line = (scrollback->oldest_line + 1) %
                            scrollback->line_capacity;
  scrollback->line_count--;

  /* Keep the same content at the top of the viewport as the ring advances. */
  if (!scrollback->follows_output && scrollback->first_visible_line)
    scrollback->first_visible_line--;
}

static int start_new_line(Scrollback *scrollback) {
  if (scrollback->line_count == TERM_SCROLLBACK_MAX_LINES)
    remove_oldest_line(scrollback);

  if (grow_line_slots(scrollback, scrollback->line_count + 1) < 0)
    return -1;

  size_t new_slot = line_slot(scrollback, scrollback->line_count);
  memset(&scrollback->lines[new_slot], 0, sizeof(scrollback->lines[new_slot]));
  scrollback->line_count++;
  return 0;
}

static int append_to_current_line(Scrollback *scrollback, const char *text,
                                  size_t length) {
  if (!length)
    return 0;
  if (!scrollback->line_count && start_new_line(scrollback) < 0)
    return -1;

  ScrollbackLine *line =
      &scrollback->lines[line_slot(scrollback, scrollback->line_count - 1)];
  if (length > (size_t)-1 - line->length - 1)
    return -1;
  size_t required = line->length + length + 1;

  /* Keep a very long output line from exceeding this tab's byte budget. */
  if (required > TERM_SCROLLBACK_MAX_BYTES) {
    while (scrollback->line_count > 1)
      remove_oldest_line(scrollback);
    line = &scrollback->lines[line_slot(scrollback, 0)];

    size_t keep_input = length < TERM_SCROLLBACK_MAX_BYTES - 1
                            ? length
                            : TERM_SCROLLBACK_MAX_BYTES - 1;
    size_t keep_existing = TERM_SCROLLBACK_MAX_BYTES - 1 - keep_input;
    if (keep_existing > line->length)
      keep_existing = line->length;
    if (keep_input < length)
      text += length - keep_input;
    if (keep_existing)
      memmove(line->text, line->text + line->length - keep_existing,
              keep_existing);

    size_t new_capacity = TERM_SCROLLBACK_MAX_BYTES;
    char *new_text = realloc(line->text, new_capacity);
    if (!new_text)
      return -1;
    scrollback->stored_bytes -= line->capacity;
    scrollback->stored_bytes += new_capacity;
    line->text = new_text;
    line->capacity = new_capacity;
    memcpy(line->text + keep_existing, text, keep_input);
    line->length = keep_existing + keep_input;
    line->text[line->length] = '\0';
    return 0;
  }

  if (required > line->capacity) {
    size_t new_capacity = line->capacity ? line->capacity : 64;
    while (new_capacity < required) {
      if (new_capacity > (size_t)-1 / 2) {
        new_capacity = required;
        break;
      }
      new_capacity *= 2;
    }
    if (new_capacity > TERM_SCROLLBACK_MAX_BYTES)
      new_capacity = TERM_SCROLLBACK_MAX_BYTES;

    while (scrollback->stored_bytes - line->capacity + new_capacity >
               TERM_SCROLLBACK_MAX_BYTES &&
           scrollback->line_count > 1) {
      remove_oldest_line(scrollback);
      line = &scrollback->lines[line_slot(scrollback,
                                          scrollback->line_count - 1)];
    }
    if (scrollback->stored_bytes - line->capacity + new_capacity >
        TERM_SCROLLBACK_MAX_BYTES)
      new_capacity = required;

    char *new_text = realloc(line->text, new_capacity);
    if (!new_text)
      return -1;
    scrollback->stored_bytes -= line->capacity;
    scrollback->stored_bytes += new_capacity;
    line->text = new_text;
    line->capacity = new_capacity;
  }

  memcpy(line->text + line->length, text, length);
  line->length += length;
  line->text[line->length] = '\0';
  return 0;
}

void scrollback_init(Scrollback *scrollback) {
  memset(scrollback, 0, sizeof(*scrollback));
  scrollback->follows_output = 1;
}

void scrollback_clear(Scrollback *scrollback) {
  for (size_t line = 0; line < scrollback->line_count; line++) {
    size_t slot = line_slot(scrollback, line);
    scrollback->stored_bytes -= scrollback->lines[slot].capacity;
    free(scrollback->lines[slot].text);
    memset(&scrollback->lines[slot], 0, sizeof(scrollback->lines[slot]));
  }
  scrollback->line_count = 0;
  scrollback->oldest_line = 0;
  scrollback->first_visible_line = 0;
  scrollback->stored_bytes = 0;
  scrollback->follows_output = 1;
}

void scrollback_destroy(Scrollback *scrollback) {
  if (!scrollback)
    return;
  scrollback_clear(scrollback);
  free(scrollback->lines);
  memset(scrollback, 0, sizeof(*scrollback));
}

int scrollback_append_text(Scrollback *scrollback, const char *text,
                           size_t length) {
  if (!scrollback || (!text && length))
    return -1;
  if (!length)
    return 0;

  size_t start = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i < length && text[i] != '\n' && text[i] != '\r' && text[i] != '\b' &&
        text[i] != '\t')
      continue;
    if (append_to_current_line(scrollback, text + start, i - start) < 0)
      return -1;
    if (i == length)
      break;

    if (text[i] == '\n') {
      if (start_new_line(scrollback) < 0)
        return -1;
    } else if (text[i] == '\r') {
      if (!scrollback->line_count && start_new_line(scrollback) < 0)
        return -1;
      ScrollbackLine *line = &scrollback->lines[
          line_slot(scrollback, scrollback->line_count - 1)];
      line->length = 0;
      if (line->text)
        line->text[0] = '\0';
    } else if (text[i] == '\b') {
      if (scrollback->line_count) {
        ScrollbackLine *line = &scrollback->lines[
            line_slot(scrollback, scrollback->line_count - 1)];
        if (line->length) {
          line->length--;
          line->text[line->length] = '\0';
        }
      }
    } else {
      size_t column = scrollback->line_count
                          ? scrollback->lines[line_slot(
                                scrollback, scrollback->line_count - 1)]
                                .length
                          : 0;
      size_t spaces = 8 - column % 8;
      static const char blanks[] = "        ";
      if (append_to_current_line(scrollback, blanks, spaces) < 0)
        return -1;
    }
    start = i + 1;
  }
  return 0;
}

void scrollback_scroll_to(Scrollback *scrollback, size_t line,
                          size_t visible_rows) {
  size_t maximum = maximum_first_line(scrollback, visible_rows);
  if (line > maximum)
    line = maximum;
  scrollback->first_visible_line = line;
  scrollback->follows_output = line == maximum;
}

void scrollback_scroll_up(Scrollback *scrollback, size_t visible_rows,
                          size_t amount) {
  size_t current = scrollback_first_visible(scrollback, visible_rows);
  if (amount > current)
    current = 0;
  else
    current -= amount;
  scrollback_scroll_to(scrollback, current, visible_rows);
}

void scrollback_scroll_down(Scrollback *scrollback, size_t visible_rows,
                            size_t amount) {
  size_t current = scrollback_first_visible(scrollback, visible_rows);
  size_t maximum = maximum_first_line(scrollback, visible_rows);
  if (amount > maximum - current)
    current = maximum;
  else
    current += amount;
  scrollback_scroll_to(scrollback, current, visible_rows);
}
