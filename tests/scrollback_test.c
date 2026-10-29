#include "scrollback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void append(Scrollback *scrollback, const char *text) {
  assert(scrollback_append_text(scrollback, text, strlen(text)) == 0);
}

static void test_appends_and_independent_buffers(void) {
  Scrollback first;
  Scrollback second;
  scrollback_init(&first);
  scrollback_init(&second);

  append(&first, "alpha\nbeta\n");
  append(&second, "other tab\n");
  assert(first.line_count == 3);
  assert(strcmp(scrollback_get_line(&first, 0), "alpha") == 0);
  assert(strcmp(scrollback_get_line(&first, 1), "beta") == 0);
  assert(strcmp(scrollback_get_line(&second, 0), "other tab") == 0);
  assert(strcmp(scrollback_get_line(&second, 1), "") == 0);

  scrollback_destroy(&first);
  scrollback_destroy(&second);
}

static void test_text_split_across_pipe_reads_and_terminal_controls(void) {
  Scrollback scrollback;
  scrollback_init(&scrollback);

  assert(scrollback_append_text(&scrollback, "hel", 3) == 0);
  assert(scrollback_append_text(&scrollback, "lo\nwo", 5) == 0);
  assert(scrollback_append_text(&scrollback, "rld\n", 4) == 0);
  assert(strcmp(scrollback_get_line(&scrollback, 0), "hello") == 0);
  assert(strcmp(scrollback_get_line(&scrollback, 1), "world") == 0);

  scrollback_clear(&scrollback);
  append(&scrollback, "abc\rxy\b\na\tb\n");
  assert(strcmp(scrollback_get_line(&scrollback, 0), "x") == 0);
  assert(strcmp(scrollback_get_line(&scrollback, 1), "a       b") == 0);
  scrollback_destroy(&scrollback);
}

static void test_view_stays_on_same_line_when_output_arrives(void) {
  Scrollback scrollback;
  scrollback_init(&scrollback);
  append(&scrollback, "one\ntwo\nthree\nfour\nfive\n");

  scrollback_scroll_up(&scrollback, 2, 3);
  size_t first_before_output =
      scrollback_first_visible(&scrollback, 2);
  assert(strcmp(scrollback_get_line(&scrollback, first_before_output),
                "two") == 0);

  append(&scrollback, "six\n");
  size_t first_after_output =
      scrollback_first_visible(&scrollback, 2);
  assert(first_after_output == first_before_output);
  assert(strcmp(scrollback_get_line(&scrollback, first_after_output),
                "two") == 0);

  scrollback_scroll_down(&scrollback, 2, 100);
  assert(scrollback.follows_output);
  assert(scrollback_first_visible(&scrollback, 2) ==
         scrollback.line_count - 2);
  append(&scrollback, "seven\n");
  assert(scrollback.follows_output);
  assert(scrollback_first_visible(&scrollback, 2) ==
         scrollback.line_count - 2);
  scrollback_destroy(&scrollback);
}

static void test_page_controls_and_clear(void) {
  Scrollback scrollback;
  scrollback_init(&scrollback);
  append(&scrollback, "0\n1\n2\n3\n4\n5\n6\n7\n8\n");

  scrollback_scroll_to(&scrollback, 0, 3);
  scrollback_scroll_down(&scrollback, 3, 2);
  assert(scrollback_first_visible(&scrollback, 3) == 2);
  scrollback_scroll_up(&scrollback, 3, 1);
  assert(scrollback_first_visible(&scrollback, 3) == 1);
  scrollback_scroll_down(&scrollback, 3, 100);
  assert(scrollback_first_visible(&scrollback, 3) ==
         scrollback.line_count - 3);

  scrollback_clear(&scrollback);
  assert(scrollback.line_count == 0);
  assert(scrollback.follows_output);
  assert(strcmp(scrollback_get_line(&scrollback, 0), "") == 0);
  scrollback_destroy(&scrollback);
}

static void test_keeps_newest_ten_thousand_lines(void) {
  Scrollback scrollback;
  scrollback_init(&scrollback);
  char line[32];
  for (size_t number = 0; number <= TERM_SCROLLBACK_MAX_LINES; number++) {
    snprintf(line, sizeof(line), "line-%05zu\n", number);
    append(&scrollback, line);
  }

  assert(scrollback.line_count == TERM_SCROLLBACK_MAX_LINES);
  assert(strcmp(scrollback_get_line(&scrollback, 0), "line-00002") == 0);
  assert(strcmp(scrollback_get_line(&scrollback,
                                    TERM_SCROLLBACK_MAX_LINES - 2),
                "line-10000") == 0);
  assert(strcmp(scrollback_get_line(&scrollback,
                                    TERM_SCROLLBACK_MAX_LINES - 1),
                "") == 0);

  scrollback_scroll_to(&scrollback, 100, 4);
  assert(strcmp(scrollback_get_line(
                    &scrollback,
                    scrollback_first_visible(&scrollback, 4)),
                "line-00102") == 0);
  append(&scrollback, "line-10001\n");
  assert(strcmp(scrollback_get_line(
                    &scrollback,
                    scrollback_first_visible(&scrollback, 4)),
                "line-00102") == 0);
  scrollback_destroy(&scrollback);
}

int main(void) {
  test_appends_and_independent_buffers();
  test_text_split_across_pipe_reads_and_terminal_controls();
  test_view_stays_on_same_line_when_output_arrives();
  test_page_controls_and_clear();
  test_keeps_newest_ten_thousand_lines();
  puts("scrollback tests passed");
  return 0;
}
