#include "selection.h"
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int active;
  int dragging;
  size_t tab_index;
  size_t start_line, start_column;
  size_t end_line, end_column;
} TextSelection;

static TextSelection selection;
static Atom clipboard_atom, utf8_atom, targets_atom, text_atom;
static Atom clipboard_property, paste_selection;
static int paste_pending, paste_tried_string;
static char *clipboard_text;

/* Keep selection and clipboard state together; both use X11 selections. */
void selection_init(Display *display) {
  clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
  utf8_atom = XInternAtom(display, "UTF8_STRING", False);
  targets_atom = XInternAtom(display, "TARGETS", False);
  text_atom = XInternAtom(display, "TEXT", False);
  clipboard_property = XInternAtom(display, "TERMINATOR_CLIPBOARD", False);
}

void selection_clear(void) {
  selection.active = 0;
  selection.dragging = 0;
}

void selection_tab_closed(size_t tab_index) {
  if (!selection.active && !selection.dragging)
    return;
  if (selection.tab_index == tab_index)
    selection_clear();
  else if (selection.tab_index > tab_index)
    selection.tab_index--;
}

static int compare_positions(size_t line_a, size_t column_a, size_t line_b,
                             size_t column_b) {
  if (line_a != line_b)
    return line_a < line_b ? -1 : 1;
  if (column_a == column_b)
    return 0;
  return column_a < column_b ? -1 : 1;
}

static void ordered_positions(size_t *first_line, size_t *first_column,
                              size_t *last_line, size_t *last_column) {
  *first_line = selection.start_line;
  *first_column = selection.start_column;
  *last_line = selection.end_line;
  *last_column = selection.end_column;
  if (compare_positions(*first_line, *first_column, *last_line,
                        *last_column) > 0) {
    size_t line = *first_line, column = *first_column;
    *first_line = *last_line;
    *first_column = *last_column;
    *last_line = line;
    *last_column = column;
  }
}

static int selected_columns(size_t line_number, size_t line_length,
                            size_t *first, size_t *last) {
  if (!selection.active)
    return 0;

  size_t first_line, first_column, last_line, last_column;
  ordered_positions(&first_line, &first_column, &last_line, &last_column);
  if (line_number < first_line || line_number > last_line)
    return 0;

  *first = line_number == first_line ? first_column : 0;
  *last = line_number == last_line ? last_column : line_length;
  if (*first > line_length)
    *first = line_length;
  if (*last > line_length)
    *last = line_length;
  return *last > *first;
}

static int text_width(XFontStruct *font, const char *text, size_t length) {
  if (!length)
    return 0;
  return font ? XTextWidth(font, text, (int)length) : (int)length * 8;
}

void selection_draw_line(Display *display, Window window, GC graphics,
                         XFontStruct *font, size_t line_number,
                         const char *line, int x, int baseline) {
  size_t length = strlen(line), first = 0, last = 0;
  if (!selected_columns(line_number, length, &first, &last)) {
    XDrawString(display, window, graphics, x, baseline, line, (int)length);
    return;
  }

  XDrawString(display, window, graphics, x, baseline, line, (int)first);
  int selected_x = x + text_width(font, line, first);
  int selected_width = text_width(font, line + first, last - first);
  int ascent = font ? font->ascent : 12;
  int line_height = font ? font->ascent + font->descent : 15;

  XSetForeground(display, graphics, 0x85a8d0);
  XFillRectangle(display, window, graphics, selected_x, baseline - ascent,
                 (unsigned)(selected_width > 1 ? selected_width : 2),
                 (unsigned)line_height);
  XSetForeground(display, graphics, 0x171717);
  XDrawString(display, window, graphics, selected_x, baseline, line + first,
              (int)(last - first));
  XSetForeground(display, graphics, 0xf2f2f2);
  XDrawString(display, window, graphics, selected_x + selected_width, baseline,
              line + last, (int)(length - last));
}

static int mouse_text_position(App *app, int x, int y, size_t visible_rows,
                               int line_height, XFontStruct *font,
                               size_t *line_number, size_t *column) {
  Tab *tab = &app->tabs[app->active_tab];
  const int output_top = 54;
  if (y < output_top || !tab->scrollback.line_count)
    return 0;

  size_t row = (size_t)((y - output_top) / line_height);
  size_t line = scrollback_first_visible(&tab->scrollback, visible_rows) + row;
  if (line >= tab->scrollback.line_count)
    return 0;

  const char *text = scrollback_get_line(&tab->scrollback, line);
  int character_width = font ? font->max_bounds.width : 8;
  if (character_width < 1)
    character_width = 8;
  *line_number = line;
  *column = x <= 12 ? 0 : (size_t)(x - 12 + character_width / 2) /
                              (size_t)character_width;
  size_t length = strlen(text);
  if (*column > length)
    *column = length;
  return 1;
}

void selection_mouse_down(App *app, int x, int y, size_t visible_rows,
                          int line_height, XFontStruct *font) {
  size_t line, column;
  selection_clear();
  if (!mouse_text_position(app, x, y, visible_rows, line_height, font, &line,
                           &column))
    return;

  selection.dragging = 1;
  selection.tab_index = app->active_tab;
  selection.start_line = selection.end_line = line;
  selection.start_column = selection.end_column = column;
}

static void update_selection(App *app, int x, int y, size_t visible_rows,
                             int line_height, XFontStruct *font) {
  size_t line, column;
  if (!selection.dragging ||
      !mouse_text_position(app, x, y, visible_rows, line_height, font, &line,
                           &column))
    return;

  selection.end_line = line;
  selection.end_column = column;
  selection.active = compare_positions(selection.start_line,
                                       selection.start_column, line, column) !=
                     0;
}

void selection_mouse_move(App *app, int x, int y, size_t visible_rows,
                          int line_height, XFontStruct *font) {
  update_selection(app, x, y, visible_rows, line_height, font);
}

void selection_mouse_up(App *app, int x, int y, size_t visible_rows,
                        int line_height, XFontStruct *font) {
  update_selection(app, x, y, visible_rows, line_height, font);
  selection.dragging = 0;
}

static char *copy_selected_text(const Scrollback *scrollback,
                                size_t *copied_length) {
  if (!selection.active)
    return NULL;

  size_t first_line, first_column, last_line, last_column;
  ordered_positions(&first_line, &first_column, &last_line, &last_column);
  size_t capacity = TERM_SCROLLBACK_MAX_BYTES + TERM_SCROLLBACK_MAX_LINES + 1;
  char *text = malloc(capacity);
  if (!text)
    return NULL;

  size_t used = 0;
  for (size_t line_number = first_line;
       line_number <= last_line && line_number < scrollback->line_count;
       line_number++) {
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

void selection_copy_to_clipboard(App *app, Display *display, Window window) {
  if (!selection.active || selection.tab_index != app->active_tab)
    return;

  size_t length;
  char *text = copy_selected_text(&app->tabs[selection.tab_index].scrollback,
                                  &length);
  if (!text || !length) {
    free(text);
    return;
  }

  free(clipboard_text);
  clipboard_text = text;
  XSetSelectionOwner(display, clipboard_atom, window, CurrentTime);
  XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
  XFlush(display);
}

void selection_request_paste(Display *display, Window window) {
  paste_selection = clipboard_atom;
  if (XGetSelectionOwner(display, paste_selection) == None)
    paste_selection = XA_PRIMARY;
  if (XGetSelectionOwner(display, paste_selection) == None)
    return;

  paste_pending = 1;
  paste_tried_string = 0;
  XConvertSelection(display, paste_selection, utf8_atom, clipboard_property,
                    window, CurrentTime);
  XFlush(display);
}

static void reply_to_selection_request(Display *display,
                                       XSelectionRequestEvent *request) {
  XSelectionEvent reply = {0};
  reply.type = SelectionNotify;
  reply.display = display;
  reply.requestor = request->requestor;
  reply.selection = request->selection;
  reply.target = request->target;
  reply.time = request->time;
  reply.property = None;

  Atom property = request->property == None ? request->target : request->property;
  if (request->target == targets_atom) {
    Atom targets[] = {targets_atom, utf8_atom, text_atom, XA_STRING};
    XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)targets,
                    (int)(sizeof(targets) / sizeof(targets[0])));
    reply.property = property;
  } else if (clipboard_text &&
             (request->target == utf8_atom || request->target == text_atom ||
              request->target == XA_STRING)) {
    Atom type = request->target == XA_STRING ? XA_STRING : utf8_atom;
    XChangeProperty(display, request->requestor, property, type, 8,
                    PropModeReplace, (unsigned char *)clipboard_text,
                    (int)strlen(clipboard_text));
    reply.property = property;
  }

  XSendEvent(display, request->requestor, False, 0, (XEvent *)&reply);
  XFlush(display);
}

static size_t receive_paste(Display *display, Window window,
                            XSelectionEvent *event, char *paste,
                            size_t paste_size) {
  if (!paste_pending || event->selection != paste_selection)
    return 0;
  if (event->property == None) {
    if (!paste_tried_string) {
      paste_tried_string = 1;
      XConvertSelection(display, paste_selection, XA_STRING,
                        clipboard_property, window, CurrentTime);
      XFlush(display);
    } else {
      paste_pending = 0;
    }
    return 0;
  }

  Atom actual_type;
  int format;
  unsigned long item_count, bytes_after;
  unsigned char *data = NULL;
  long max_items = (long)((paste_size + 3) / 4);
  int result = XGetWindowProperty(display, window, clipboard_property, 0,
                                  max_items, True, AnyPropertyType,
                                  &actual_type, &format, &item_count,
                                  &bytes_after, &data);
  size_t length = 0;
  if (result == Success && format == 8 && data) {
    length = item_count < paste_size ? (size_t)item_count : paste_size;
    for (size_t i = 0; i < length; i++)
      paste[i] = data[i] == '\n' || data[i] == '\r' ? ' ' : (char)data[i];
  }
  if (data)
    XFree(data);
  paste_pending = 0;
  return length;
}

size_t selection_handle_event(Display *display, Window window, XEvent *event,
                              char *paste, size_t paste_size) {
  if (event->type == SelectionRequest) {
    reply_to_selection_request(display, &event->xselectionrequest);
    return 0;
  }
  if (event->type == SelectionNotify)
    return receive_paste(display, window, &event->xselection, paste,
                         paste_size);
  return 0;
}

void selection_destroy(void) {
  free(clipboard_text);
  clipboard_text = NULL;
}
