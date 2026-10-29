#ifndef TERMINATOR_SELECTION_H
#define TERMINATOR_SELECTION_H

#include "terminator.h"
#include <X11/Xlib.h>
#include <stddef.h>

/* Text selection and X11 clipboard support used by the GUI. */
void selection_init(Display *display);
void selection_clear(void);
void selection_tab_closed(size_t tab_index);
void selection_draw_line(Display *display, Window window, GC graphics,
                         XFontStruct *font, size_t line_number,
                         const char *line, int x, int baseline);
void selection_mouse_down(App *app, int x, int y, size_t visible_rows,
                          int line_height, XFontStruct *font);
void selection_mouse_move(App *app, int x, int y, size_t visible_rows,
                         int line_height, XFontStruct *font);
void selection_mouse_up(App *app, int x, int y, size_t visible_rows,
                        int line_height, XFontStruct *font);
void selection_copy_to_clipboard(App *app, Display *display, Window window);
void selection_request_paste(Display *display, Window window);
size_t selection_handle_event(Display *display, Window window, XEvent *event,
                              char *paste, size_t paste_size);
void selection_destroy(void);

#endif
