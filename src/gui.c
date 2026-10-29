#include "shell.h"
#include "terminator.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Owns the X11 window, keyboard input, tab bar, and visible scrollback. Shell
 * workers never call Xlib; they send output and completion messages over pipes.
 */
static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static int width = 900, height = 600;
static Atom wm_delete;
static Atom clipboard_atom, utf8_atom, targets_atom, text_atom;
static Atom clipboard_property;
static Atom paste_selection;
static int paste_pending, paste_tried_string;
static char *clipboard_text;

typedef struct {
  int active;
  int dragging;
  size_t tab_index;
  size_t start_line, start_column;
  size_t end_line, end_column;
} TextSelection;

static TextSelection text_selection;

static void clear_text_selection(void) {
  text_selection.active = 0;
  text_selection.dragging = 0;
}

static void clear_scrollback(Tab *tab) {
  scrollback_clear(&tab->scrollback);
  clear_text_selection();
}

/* Pipe reads can split text at any byte; the scrollback joins each chunk. */
static void append_output_text(Tab *tab, const char *text, size_t length) {
  (void)scrollback_append_text(&tab->scrollback, text, length);
}

static void build_shell_prompt(const Tab *tab, char *prompt, size_t size) {
  const char *user = getenv("USER");
  char host[128] = "localhost";
  if (!user || !*user)
    user = "user";
  if (gethostname(host, sizeof(host)) < 0)
    strcpy(host, "localhost");
  host[sizeof(host) - 1] = '\0';

  const char *cwd = tab->working_directory;
  const char *home = getenv("HOME");
  char display_cwd[sizeof(tab->working_directory) + 2];
  if (home && *home && !strncmp(cwd, home, strlen(home)) &&
      (cwd[strlen(home)] == '\0' || cwd[strlen(home)] == '/')) {
    snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    cwd = display_cwd;
  }
  snprintf(prompt, size, "%s@%s:%s$ ", user, host, cwd);
}

static int calculate_tab_width(size_t count) {
  int area = width - 56;
  if (area < 1)
    area = 1;
  int cell = count ? area / (int)count : area;
  if (cell > 180)
    cell = 180;
  return cell > 0 ? cell : 1;
}

static int calculate_add_tab_button_x(size_t count) {
  return 8 + (int)count * calculate_tab_width(count) + 4;
}

static int terminal_line_height(void) {
  return font ? font->ascent + font->descent + 3 : 18;
}

static size_t terminal_visible_rows(void) {
  int visible = (height - 54 - 10) / terminal_line_height();
  return visible > 0 ? (size_t)visible : 1;
}

static int compare_position(size_t line_a, size_t column_a, size_t line_b,
                            size_t column_b) {
  if (line_a != line_b)
    return line_a < line_b ? -1 : 1;
  if (column_a == column_b)
    return 0;
  return column_a < column_b ? -1 : 1;
}

static void ordered_selection(size_t *first_line, size_t *first_column,
                              size_t *last_line, size_t *last_column) {
  *first_line = text_selection.start_line;
  *first_column = text_selection.start_column;
  *last_line = text_selection.end_line;
  *last_column = text_selection.end_column;
  if (compare_position(*first_line, *first_column, *last_line,
                       *last_column) > 0) {
    size_t line = *first_line, column = *first_column;
    *first_line = *last_line;
    *first_column = *last_column;
    *last_line = line;
    *last_column = column;
  }
}

static int selected_columns(size_t line_number, size_t line_length,
                            size_t *first_column, size_t *last_column) {
  if (!text_selection.active)
    return 0;
  size_t first_line, first, last_line, last;
  ordered_selection(&first_line, &first, &last_line, &last);
  if (line_number < first_line || line_number > last_line)
    return 0;

  *first_column = line_number == first_line ? first : 0;
  *last_column = line_number == last_line ? last : line_length;
  if (*first_column > line_length)
    *first_column = line_length;
  if (*last_column > line_length)
    *last_column = line_length;
  return *last_column > *first_column;
}

static int text_width(const char *text, size_t length) {
  if (!length)
    return 0;
  return font ? XTextWidth(font, text, (int)length) : (int)length * 8;
}

static void draw_output_line(size_t line_number, const char *line, int x,
                             int baseline) {
  size_t length = strlen(line), first = 0, last = 0;
  if (!selected_columns(line_number, length, &first, &last)) {
    XDrawString(dpy, win, gc, x, baseline, line, (int)length);
    return;
  }

  XDrawString(dpy, win, gc, x, baseline, line, (int)first);
  int selection_x = x + text_width(line, first);
  int selection_width = text_width(line + first, last - first);
  int ascent = font ? font->ascent : 12;
  int line_height = font ? font->ascent + font->descent : 15;
  XSetForeground(dpy, gc, 0x85a8d0);
  XFillRectangle(dpy, win, gc, selection_x, baseline - ascent,
                 (unsigned)(selection_width > 1 ? selection_width : 2),
                 (unsigned)line_height);
  XSetForeground(dpy, gc, 0x171717);
  XDrawString(dpy, win, gc, selection_x, baseline, line + first,
              (int)(last - first));
  XSetForeground(dpy, gc, 0xf2f2f2);
  XDrawString(dpy, win, gc, selection_x + selection_width, baseline,
              line + last, (int)(length - last));
}

static size_t column_at_x(const char *line, int x) {
  if (x <= 0)
    return 0;
  int character_width = font ? font->max_bounds.width : 8;
  if (character_width < 1)
    character_width = 8;
  size_t column = (size_t)(x + character_width / 2) / (size_t)character_width;
  size_t length = strlen(line);
  if (column > length)
    column = length;
  return column;
}

static int mouse_text_position(App *app, int x, int y, size_t *line_number,
                               size_t *column) {
  Tab *tab = &app->tabs[app->active_tab];
  const int top = 54;
  if (y < top || !tab->scrollback.line_count)
    return 0;

  size_t row = (size_t)((y - top) / terminal_line_height());
  size_t line = scrollback_first_visible(&tab->scrollback,
                                         terminal_visible_rows()) + row;
  if (line >= tab->scrollback.line_count)
    return 0;
  const char *text = scrollback_get_line(&tab->scrollback, line);
  *line_number = line;
  *column = column_at_x(text, x - 12);
  return 1;
}

static char *copy_selected_text(const Scrollback *scrollback,
                                size_t *copied_length) {
  if (!text_selection.active)
    return NULL;

  size_t first_line, first_column, last_line, last_column;
  ordered_selection(&first_line, &first_column, &last_line, &last_column);
  size_t capacity = TERM_SCROLLBACK_MAX_BYTES + TERM_SCROLLBACK_MAX_LINES + 1;
  char *text = malloc(capacity);
  if (!text)
    return NULL;

  size_t used = 0;
  for (size_t line_number = first_line;
       line_number <= last_line && line_number < scrollback->line_count;
       ++line_number) {
    const char *line = scrollback_get_line(scrollback, line_number);
    size_t length = strlen(line);
    size_t start = line_number == first_line ? first_column : 0;
    size_t end = line_number == last_line ? last_column : length;
    if (start > length)
      start = length;
    if (end > length)
      end = length;
    if (end > start && used + end - start < capacity) {
      memcpy(text + used, line + start, end - start);
      used += end - start;
    }
    if (line_number < last_line && used + 1 < capacity)
      text[used++] = '\n';
  }
  text[used] = '\0';
  *copied_length = used;
  return text;
}

static void draw_terminal_window(App *app) {
  if (!dpy)
    return;
  XClearWindow(dpy, win);
  if (font)
    XSetFont(dpy, gc, font->fid);
  XSetForeground(dpy, gc, 0x3b1d34);
  XFillRectangle(dpy, win, gc, 0, 0, (unsigned)width, 34);
  const int left = 8;
  int tab_width = calculate_tab_width(app->tab_count);
  for (size_t i = 0; i < app->tab_count; i++) {
    int x = left + (int)i * tab_width;
    XSetForeground(dpy, gc, i == app->active_tab ? 0x69435f : 0x4a2944);
    int tab_fill_width = tab_width > 2 ? tab_width - 2 : 1;
    XFillRectangle(dpy, win, gc, x, 4, (unsigned)tab_fill_width, 26);
    XSetForeground(dpy, gc, 0xf2f2f2);
    char label[24];
    snprintf(label, sizeof(label), "%zu", i + 1);
    int label_width = font ? XTextWidth(font, label, (int)strlen(label))
                           : (int)strlen(label) * 8;
    if (label_width <= tab_width - 8)
      XDrawString(dpy, win, gc, x + (tab_width - label_width) / 2, 22, label,
                  (int)strlen(label));
  }
  int plus_x = calculate_add_tab_button_x(app->tab_count);
  XSetForeground(dpy, gc, 0x4a2944);
  XFillRectangle(dpy, win, gc, plus_x, 4, 32, 26);
  XSetForeground(dpy, gc, 0xf2f2f2);
  XDrawString(dpy, win, gc, plus_x + 12, 22, "+", 1);
  /* Keep terminal output and the input prompt readable on the dark background.
   */
  XSetForeground(dpy, gc, 0xf2f2f2);
  Tab *tab = &app->tabs[app->active_tab];
  int lineh = terminal_line_height();
  const int top = 54;
  size_t visible = terminal_visible_rows();
  int prompt_visible = !tab->command_is_running;
  size_t total = tab->scrollback.line_count;
  if (prompt_visible && total == 0)
    total = 1;
  size_t first =
      scrollback_first_visible(&tab->scrollback, visible);
  int y = top;
  for (size_t row = first; row < total && y < height - 5; row++, y += lineh) {
    const char *line = scrollback_get_line(&tab->scrollback, row);
    int line_x = 12;
    if (line[0]) {
      draw_output_line(row, line, line_x, y);
      line_x += font ? XTextWidth(font, line, (int)strlen(line))
                     : (int)strlen(line) * 8;
    }

    /* Append the live prompt to the current row, like a shell prompt. */
    if (prompt_visible && row + 1 == total) {
      char prompt[sizeof(tab->working_directory) + 256];
      if (tab->multiline_length)
        strcpy(prompt, "> ");
      else
        build_shell_prompt(tab, prompt, sizeof(prompt));
      XDrawString(dpy, win, gc, line_x, y, prompt, (int)strlen(prompt));
      int px = line_x + (font ? XTextWidth(font, prompt, (int)strlen(prompt))
                              : (int)strlen(prompt) * 8);
      XDrawString(dpy, win, gc, px, y, tab->input_buffer,
                  (int)tab->input_length);
      if (tab->scrollback.follows_output) {
        int cx = px + (font ? XTextWidth(font, tab->input_buffer,
                                         (int)tab->input_cursor)
                            : (int)tab->input_cursor * 8);
        XDrawLine(dpy, win, gc, cx, y - (font ? font->ascent : 10), cx,
                  y + (font ? font->descent : 3));
      }
    }
  }
  XFlush(dpy);
}
static void insert_input_text(Tab *tab, const char *text, size_t n) {
  if (n > TERM_LINE_MAX - 1 - tab->input_length)
    n = TERM_LINE_MAX - 1 - tab->input_length;
  memmove(tab->input_buffer + tab->input_cursor + n,
          tab->input_buffer + tab->input_cursor,
          tab->input_length - tab->input_cursor);
  memcpy(tab->input_buffer + tab->input_cursor, text, n);
  tab->input_length += n;
  tab->input_cursor += n;
  tab->input_buffer[tab->input_length] = 0;
}

static void set_input_text(Tab *tab, const char *text) {
  size_t length = strlen(text);
  if (length >= sizeof(tab->input_buffer))
    length = sizeof(tab->input_buffer) - 1;
  memcpy(tab->input_buffer, text, length);
  tab->input_buffer[length] = '\0';
  tab->input_length = length;
  tab->input_cursor = length;
}

static void browse_history(App *app, Tab *tab, int direction) {
  if (tab->multiline_length)
    return;

  char history_line[TERM_LINE_MAX];
  if (direction < 0) {
    size_t offset = tab->history_position;
    if (!history_get_recent(app->history, offset, history_line,
                            sizeof(history_line)))
      return;
    if (!tab->history_position) {
      memcpy(tab->history_draft, tab->input_buffer, tab->input_length + 1);
      tab->history_draft_length = tab->input_length;
      tab->history_draft_cursor = tab->input_cursor;
    }
    tab->history_position++;
    set_input_text(tab, history_line);
  } else if (tab->history_position) {
    tab->history_position--;
    if (!tab->history_position) {
      memcpy(tab->input_buffer, tab->history_draft,
             tab->history_draft_length + 1);
      tab->input_length = tab->history_draft_length;
      tab->input_cursor = tab->history_draft_cursor;
    } else if (history_get_recent(app->history, tab->history_position - 1,
                                  history_line, sizeof(history_line))) {
      set_input_text(tab, history_line);
    }
  }
}

static void delete_previous_word(Tab *tab) {
  size_t word_start = tab->input_cursor;
  while (word_start && isspace((unsigned char)tab->input_buffer[word_start - 1]))
    word_start--;
  while (word_start &&
         !isspace((unsigned char)tab->input_buffer[word_start - 1]))
    word_start--;

  size_t removed = tab->input_cursor - word_start;
  memmove(tab->input_buffer + word_start,
          tab->input_buffer + tab->input_cursor,
          tab->input_length - tab->input_cursor + 1);
  tab->input_length -= removed;
  tab->input_cursor = word_start;
}

static void copy_selection(App *app) {
  if (!text_selection.active || text_selection.tab_index >= app->tab_count ||
      text_selection.tab_index != app->active_tab)
    return;

  size_t length = 0;
  char *copy = copy_selected_text(
      &app->tabs[text_selection.tab_index].scrollback, &length);
  if (!copy || !length) {
    free(copy);
    return;
  }
  free(clipboard_text);
  clipboard_text = copy;
  XSetSelectionOwner(dpy, clipboard_atom, win, CurrentTime);
  XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
  XFlush(dpy);
}

static void request_paste(void) {
  paste_selection = clipboard_atom;
  if (XGetSelectionOwner(dpy, paste_selection) == None)
    paste_selection = XA_PRIMARY;
  if (XGetSelectionOwner(dpy, paste_selection) == None)
    return;

  paste_pending = 1;
  paste_tried_string = 0;
  XConvertSelection(dpy, paste_selection, utf8_atom, clipboard_property, win,
                    CurrentTime);
  XFlush(dpy);
}

static void reply_to_selection_request(XSelectionRequestEvent *request) {
  XSelectionEvent response = {0};
  response.type = SelectionNotify;
  response.display = request->display;
  response.requestor = request->requestor;
  response.selection = request->selection;
  response.target = request->target;
  response.time = request->time;
  response.property = None;

  Atom property = request->property == None ? request->target : request->property;
  if (request->target == targets_atom) {
    Atom supported[] = {targets_atom, utf8_atom, text_atom, XA_STRING};
    XChangeProperty(dpy, request->requestor, property, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported,
                    (int)(sizeof(supported) / sizeof(supported[0])));
    response.property = property;
  } else if (clipboard_text &&
             (request->target == utf8_atom || request->target == text_atom ||
              request->target == XA_STRING)) {
    Atom type = request->target == XA_STRING ? XA_STRING : utf8_atom;
    XChangeProperty(dpy, request->requestor, property, type, 8, PropModeReplace,
                    (unsigned char *)clipboard_text,
                    (int)strlen(clipboard_text));
    response.property = property;
  }

  XSendEvent(dpy, request->requestor, False, 0, (XEvent *)&response);
  XFlush(dpy);
}

static void receive_paste(App *app, XSelectionEvent *event) {
  if (!paste_pending || event->selection != paste_selection)
    return;
  if (event->property == None) {
    if (!paste_tried_string) {
      paste_tried_string = 1;
      XConvertSelection(dpy, paste_selection, XA_STRING, clipboard_property,
                        win, CurrentTime);
      XFlush(dpy);
    } else {
      paste_pending = 0;
    }
    return;
  }

  Atom actual_type;
  int format;
  unsigned long item_count, bytes_after;
  unsigned char *data = NULL;
  long max_items = (TERM_LINE_MAX + 3) / 4;
  int result = XGetWindowProperty(dpy, win, clipboard_property, 0, max_items,
                                  True, AnyPropertyType, &actual_type, &format,
                                  &item_count, &bytes_after, &data);
  if (result == Success && format == 8 && data) {
    Tab *tab = &app->tabs[app->active_tab];
    size_t available = TERM_LINE_MAX - 1 - tab->input_length;
    size_t count = item_count < available ? item_count : available;
    char paste[TERM_LINE_MAX];
    for (size_t i = 0; i < count; ++i) {
      char character = (char)data[i];
      paste[i] = character == '\n' || character == '\r' ? ' ' : character;
    }
    insert_input_text(tab, paste, count);
  }
  if (data)
    XFree(data);
  paste_pending = 0;
  draw_terminal_window(app);
}

static int send_command_to_worker(int file_descriptor, const char *text,
                                  size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = write(file_descriptor, text + sent, n - sent);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    sent += (size_t)w;
  }
  return 0;
}
static void submit_input(App *app, Tab *tab) {
  /* Save the command in scrollback, then send it to this tab's worker. */
  if (tab->multiline_length + tab->input_length + 2 >= TERM_LINE_MAX) {
    append_output_text(tab, "terminator: input too long\n", 27);
    tab->multiline_length = 0;
    return;
  }
  int continuation = tab->multiline_length > 0;
  char prompt[sizeof(tab->working_directory) + 256];
  if (continuation)
    strcpy(prompt, "> ");
  else
    build_shell_prompt(tab, prompt, sizeof(prompt));
  append_output_text(tab, prompt, strlen(prompt));
  append_output_text(tab, tab->input_buffer, tab->input_length);
  append_output_text(tab, "\n", 1);
  memcpy(tab->multiline_buffer + tab->multiline_length, tab->input_buffer,
         tab->input_length);
  tab->multiline_length += tab->input_length;
  tab->multiline_buffer[tab->multiline_length] = 0;
  if (!shell_is_complete(tab->multiline_buffer)) {
    size_t n = tab->input_length;
    if (n && tab->input_buffer[n - 1] == '\\') {
      tab->multiline_length--;
      tab->multiline_buffer[tab->multiline_length] = 0;
    } else
      tab->multiline_buffer[tab->multiline_length++] = '\n';
    tab->input_length = tab->input_cursor = 0;
    tab->input_buffer[0] = 0;
    tab->history_position = 0;
    draw_terminal_window(app);
    return;
  }
  tab->multiline_buffer[tab->multiline_length++] = '\n';
  if (send_command_to_worker(tab->gui_to_worker_fd, tab->multiline_buffer,
                             tab->multiline_length) < 0)
    append_output_text(tab, "terminator: tab worker is unavailable\n", 38);
  tab->command_is_running = 1;
  tab->multiline_length = 0;
  tab->multiline_buffer[0] = 0;
  tab->input_length = tab->input_cursor = 0;
  tab->input_buffer[0] = 0;
  tab->history_position = 0;
  (void)app;
}
static void handle_key_press(App *app, XKeyEvent *key_event) {
  /* Apply editing and control shortcuts to the currently selected tab. */
  Tab *tab = &app->tabs[app->active_tab];
  KeySym key_symbol;
  KeySym hotkey_symbol = XLookupKeysym(key_event, 0);
  char buf[64];
  int n = XLookupString(key_event, buf, sizeof(buf), &key_symbol, NULL);
  int ctrl = (key_event->state & ControlMask) != 0;
  int shift = (key_event->state & ShiftMask) != 0;
  if (ctrl && shift && hotkey_symbol == XK_c) {
    copy_selection(app);
    return;
  }
  if (ctrl && shift && hotkey_symbol == XK_v) {
    request_paste();
    return;
  }
  if (ctrl && !shift && hotkey_symbol == XK_c) {
    int pgid = atomic_load(&tab->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGINT);
      append_output_text(tab, "^C\n", 3);
    }
    return;
  }
  if (ctrl && !shift && hotkey_symbol == XK_z) {
    int pgid = atomic_load(&tab->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGTSTP);
      append_output_text(tab, "^Z\n", 3);
    }
    return;
  }
  if (ctrl && !shift && hotkey_symbol == XK_d) {
    if (!tab->input_length && !tab->multiline_length) {
      clear_text_selection();
      app_close_tab(app, app->active_tab);
      if (!app->tab_count)
        terminator_stopping = 1;
    }
    return;
  }
  if (ctrl && !shift && hotkey_symbol == XK_t) {
    clear_text_selection();
    if (app_add_tab(app) < 0)
      append_output_text(tab, "terminator: maximum tabs reached\n", 34);
    return;
  }
  if (key_symbol == XK_Return || key_symbol == XK_KP_Enter) {
    submit_input(app, tab);
    return;
  }
  if (key_symbol == XK_BackSpace && tab->input_cursor) {
    memmove(tab->input_buffer + tab->input_cursor - 1,
            tab->input_buffer + tab->input_cursor,
            tab->input_length - tab->input_cursor);
    tab->input_cursor--;
    tab->input_length--;
    tab->input_buffer[tab->input_length] = 0;
    return;
  }
  if (key_symbol == XK_Left && tab->input_cursor)
    tab->input_cursor--;
  if (key_symbol == XK_Right && tab->input_cursor < tab->input_length)
    tab->input_cursor++;
  if (key_symbol == XK_Home || (ctrl && hotkey_symbol == XK_a))
    tab->input_cursor = 0;
  if (key_symbol == XK_End || (ctrl && hotkey_symbol == XK_e))
    tab->input_cursor = tab->input_length;
  if (ctrl && !shift && hotkey_symbol == XK_u) {
    memmove(tab->input_buffer, tab->input_buffer + tab->input_cursor,
            tab->input_length - tab->input_cursor);
    tab->input_length -= tab->input_cursor;
    tab->input_cursor = 0;
    tab->input_buffer[tab->input_length] = 0;
  }
  if (ctrl && !shift && hotkey_symbol == XK_k) {
    tab->input_length = tab->input_cursor;
    tab->input_buffer[tab->input_length] = 0;
  }
  if (ctrl && !shift && hotkey_symbol == XK_w) {
    delete_previous_word(tab);
  }
  if (ctrl && !shift && hotkey_symbol == XK_l) {
    clear_scrollback(tab);
  }
  size_t visible_rows = terminal_visible_rows();
  if (!ctrl && !shift && hotkey_symbol == XK_Up)
    browse_history(app, tab, -1);
  if (!ctrl && !shift && hotkey_symbol == XK_Down)
    browse_history(app, tab, 1);
  if (key_symbol == XK_Page_Up)
    scrollback_scroll_up(&tab->scrollback, visible_rows,
                         visible_rows > 1 ? visible_rows - 1 : 1);
  if (key_symbol == XK_Page_Down)
    scrollback_scroll_down(&tab->scrollback, visible_rows,
                           visible_rows > 1 ? visible_rows - 1 : 1);
  if (n > 0 && !ctrl && key_symbol != XK_BackSpace && key_symbol != XK_Return &&
      key_symbol != XK_Left && key_symbol != XK_Right && key_symbol != XK_Up &&
      key_symbol != XK_Down && key_symbol != XK_Page_Up &&
      key_symbol != XK_Page_Down && key_symbol != XK_Home &&
      key_symbol != XK_End)
    insert_input_text(tab, buf, (size_t)n);
}
int gui_run(App *app) {
  /* Poll X events and every tab's output/status pipes without busy-waiting. */
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display\n");
    return 1;
  }
  int screen = DefaultScreen(dpy);
  app->x11_connection_fd = ConnectionNumber(dpy);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 30, 30,
                            (unsigned)width, (unsigned)height, 1,
                            BlackPixel(dpy, screen), 0x300a24);
  XStoreName(dpy, win, "Terminator");
  XSelectInput(dpy, win,
               ExposureMask | KeyPressMask | StructureNotifyMask |
                   ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);
  clipboard_atom = XInternAtom(dpy, "CLIPBOARD", False);
  utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
  targets_atom = XInternAtom(dpy, "TARGETS", False);
  text_atom = XInternAtom(dpy, "TEXT", False);
  clipboard_property = XInternAtom(dpy, "TERMINATOR_CLIPBOARD", False);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  gc = XCreateGC(dpy, win, 0, NULL);
  font = XLoadQueryFont(dpy, "9x15");
  if (!font)
    font = XLoadQueryFont(dpy, "fixed");
  XMapWindow(dpy, win);
  /* Send the map request before poll() waits on the X connection. */
  XFlush(dpy);
  for (size_t i = 0; i < app->tab_count; i++) {
    fcntl(app->tabs[i].worker_to_gui_fd, F_SETFL, O_NONBLOCK);
    fcntl(app->tabs[i].status_from_worker_fd, F_SETFL, O_NONBLOCK);
  }
  int xfd = ConnectionNumber(dpy);
  while (app->tab_count && !terminator_stopping) {
    struct pollfd pf[1 + TERM_MAX_TABS * 2];
    size_t map[1 + TERM_MAX_TABS * 2];
    int kind[1 + TERM_MAX_TABS * 2];
    nfds_t nf = 1;
    pf[0] = (struct pollfd){xfd, POLLIN, 0};
    for (size_t i = 0; i < app->tab_count; i++) {
      pf[nf] =
          (struct pollfd){app->tabs[i].worker_to_gui_fd, POLLIN | POLLHUP, 0};
      map[nf] = i;
      kind[nf++] = 1;
      pf[nf] = (struct pollfd){app->tabs[i].status_from_worker_fd, POLLIN, 0};
      map[nf] = i;
      kind[nf++] = 2;
    }
    /* Sleep until X11 or app worker pipe is ready; the GUI does not spin. */
    int pr = poll(pf, nf, -1);
    if (pr < 0) {
      if (errno == EINTR && terminator_stopping)
        break;
      if (errno == EINTR)
        continue;
      break;
    }
    if (pf[0].revents & POLLIN) {
      while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose)
          draw_terminal_window(app);
        else if (ev.type == ConfigureNotify) {
          width = ev.xconfigure.width;
          height = ev.xconfigure.height;
          draw_terminal_window(app);
        } else if (ev.type == KeyPress) {
          handle_key_press(app, &ev.xkey);
          draw_terminal_window(app);
        } else if (ev.type == ButtonPress) {
          if (ev.xbutton.y < 34) {
            clear_text_selection();
            int plus_x = calculate_add_tab_button_x(app->tab_count);
            if (ev.xbutton.x >= plus_x && ev.xbutton.x < plus_x + 32) {
              if (app_add_tab(app) < 0)
                append_output_text(&app->tabs[app->active_tab],
                                   "terminator: maximum tabs reached\n", 34);
            } else {
              int tab_width = calculate_tab_width(app->tab_count);
              size_t ix = ev.xbutton.x >= 8
                              ? (size_t)((ev.xbutton.x - 8) / tab_width)
                              : app->tab_count;
              if (ix < app->tab_count)
                app->active_tab = ix;
            }
            draw_terminal_window(app);
          } else if (ev.xbutton.y >= 54 &&
                     (ev.xbutton.button == Button4 ||
                      ev.xbutton.button == Button5)) {
            Tab *tab = &app->tabs[app->active_tab];
            size_t visible_rows = terminal_visible_rows();
            if (ev.xbutton.button == Button4)
              scrollback_scroll_up(&tab->scrollback, visible_rows, 3);
            else
              scrollback_scroll_down(&tab->scrollback, visible_rows, 3);
            draw_terminal_window(app);
          } else if (ev.xbutton.button == Button1) {
            size_t line_number, column;
            clear_text_selection();
            if (mouse_text_position(app, ev.xbutton.x, ev.xbutton.y,
                                    &line_number, &column)) {
              text_selection.dragging = 1;
              text_selection.tab_index = app->active_tab;
              text_selection.start_line = line_number;
              text_selection.start_column = column;
              text_selection.end_line = line_number;
              text_selection.end_column = column;
            }
            draw_terminal_window(app);
          }
        } else if (ev.type == MotionNotify && text_selection.dragging) {
          size_t line_number, column;
          if (mouse_text_position(app, ev.xmotion.x, ev.xmotion.y,
                                  &line_number, &column)) {
            text_selection.end_line = line_number;
            text_selection.end_column = column;
            text_selection.active =
                compare_position(text_selection.start_line,
                                 text_selection.start_column, line_number,
                                 column) != 0;
            draw_terminal_window(app);
          }
        } else if (ev.type == ButtonRelease &&
                   ev.xbutton.button == Button1) {
          size_t line_number, column;
          if (text_selection.dragging &&
              mouse_text_position(app, ev.xbutton.x, ev.xbutton.y,
                                  &line_number, &column)) {
            text_selection.end_line = line_number;
            text_selection.end_column = column;
            text_selection.active =
                compare_position(text_selection.start_line,
                                 text_selection.start_column, line_number,
                                 column) != 0;
          }
          text_selection.dragging = 0;
          draw_terminal_window(app);
        } else if (ev.type == SelectionRequest) {
          reply_to_selection_request(&ev.xselectionrequest);
        } else if (ev.type == SelectionNotify) {
          receive_paste(app, &ev.xselection);
        } else if (ev.type == ClientMessage &&
                   (Atom)ev.xclient.data.l[0] == wm_delete) {
          goto done;
        }
      }
    }
    int close_tab[TERM_MAX_TABS] = {0};
    for (nfds_t i = 1; i < nf; i++)
      if (pf[i].revents) {
        Tab *tab = &app->tabs[map[i]];
        if (kind[i] == 1) {
          char b[2048];
          ssize_t n;
          while ((n = read(pf[i].fd, b, sizeof(b))) > 0)
            append_output_text(tab, b, (size_t)n);
        } else {
          WorkerStatus message;
          ssize_t n;
          while ((n = read(pf[i].fd, &message, sizeof(message))) ==
                 (ssize_t)sizeof(message)) {
            tab->command_is_running = 0;
            if (message.clear_screen)
              clear_scrollback(tab);
            snprintf(tab->working_directory, sizeof(tab->working_directory),
                     "%s", message.working_directory);
          }
        }
        if (pf[i].revents & (POLLHUP | POLLERR))
          close_tab[map[i]] = 1;
      }
    for (size_t i = app->tab_count; i > 0; i--)
      if (close_tab[i - 1]) {
        if (text_selection.active || text_selection.dragging) {
          if (text_selection.tab_index == i - 1)
            clear_text_selection();
          else if (text_selection.tab_index > i - 1)
            text_selection.tab_index--;
        }
        app_close_tab(app, i - 1);
      }
    if (app->tab_count)
      draw_terminal_window(app);
  }
done:
  if (font)
    XFreeFont(dpy, font);
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  free(clipboard_text);
  clipboard_text = NULL;
  return 0;
}
