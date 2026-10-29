#include "shell.h"
#include "terminator.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
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
static int append_scrollback_line(Tab *tab) {
  /* Bound per-tab scrollback while keeping the newest rows. */
  if (tab->line_count == 4000) {
    free(tab->lines[0]);
    memmove(tab->lines, tab->lines + 1,
            (tab->line_count - 1) * sizeof(*tab->lines));
    tab->line_count--;
    if (tab->scroll)
      tab->scroll--;
  }
  if (tab->line_count == tab->line_cap) {
    size_t cap = tab->line_cap ? tab->line_cap * 2 : 64;
    char **lines = realloc(tab->lines, cap * sizeof(*lines));
    if (!lines)
      return -1;
    tab->lines = lines;
    tab->line_cap = cap;
  }
  tab->lines[tab->line_count] = calloc(1, 1);
  if (!tab->lines[tab->line_count])
    return -1;
  tab->line_count++;
  return 0;
}

static int append_to_current_scrollback_line(Tab *tab, const char *text,
                                             size_t n) {
  if (!n)
    return 0;
  if (!tab->line_count && append_scrollback_line(tab) < 0)
    return -1;
  char *line = tab->lines[tab->line_count - 1];
  size_t old_len = strlen(line);
  char *grown = realloc(line, old_len + n + 1);
  if (!grown)
    return -1;
  memcpy(grown + old_len, text, n);
  grown[old_len + n] = '\0';
  tab->lines[tab->line_count - 1] = grown;
  return 0;
}

static void clear_scrollback(Tab *tab) {
  for (size_t i = 0; i < tab->line_count; i++)
    free(tab->lines[i]);
  tab->line_count = 0;
  tab->scroll = 0;
}

/*
 * Worker output arrives in arbitrary pipe-sized chunks. Append it to the
 * current row so output split across reads is displayed as one terminal line.
 */
static void append_output_text(Tab *tab, const char *text, size_t n) {
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i < n && text[i] != '\n' && text[i] != '\r' && text[i] != '\b' &&
        text[i] != '\t')
      continue;
    if (append_to_current_scrollback_line(tab, text + start, i - start) < 0)
      return;
    if (i == n)
      break;
    if (text[i] == '\n') {
      if (append_scrollback_line(tab) < 0)
        return;
    } else if (text[i] == '\r') {
      if (!tab->line_count && append_scrollback_line(tab) < 0)
        return;
      tab->lines[tab->line_count - 1][0] = '\0';
    } else if (text[i] == '\b') {
      if (tab->line_count) {
        size_t len = strlen(tab->lines[tab->line_count - 1]);
        if (len)
          tab->lines[tab->line_count - 1][len - 1] = '\0';
      }
    } else {
      size_t col =
          tab->line_count ? strlen(tab->lines[tab->line_count - 1]) : 0;
      size_t spaces = 8 - col % 8;
      static const char blanks[] = "        ";
      if (append_to_current_scrollback_line(tab, blanks, spaces) < 0)
        return;
    }
    start = i + 1;
  }
}

static void build_shell_prompt(const Tab *tab, char *prompt, size_t size) {
  const char *user = getenv("USER");
  char host[128] = "localhost";
  if (!user || !*user)
    user = "user";
  if (gethostname(host, sizeof(host)) < 0)
    strcpy(host, "localhost");
  host[sizeof(host) - 1] = '\0';

  const char *cwd = tab->cwd;
  const char *home = getenv("HOME");
  char display_cwd[sizeof(tab->cwd) + 2];
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

static void draw_terminal_window(App *app) {
  if (!dpy)
    return;
  XClearWindow(dpy, win);
  if (font)
    XSetFont(dpy, gc, font->fid);
  XSetForeground(dpy, gc, 0x3b1d34);
  XFillRectangle(dpy, win, gc, 0, 0, (unsigned)width, 34);
  const int left = 8;
  int tab_width = calculate_tab_width(app->count);
  for (size_t i = 0; i < app->count; i++) {
    int x = left + (int)i * tab_width;
    XSetForeground(dpy, gc, i == app->active ? 0x69435f : 0x4a2944);
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
  int plus_x = calculate_add_tab_button_x(app->count);
  XSetForeground(dpy, gc, 0x4a2944);
  XFillRectangle(dpy, win, gc, plus_x, 4, 32, 26);
  XSetForeground(dpy, gc, 0xf2f2f2);
  XDrawString(dpy, win, gc, plus_x + 12, 22, "+", 1);
  /* Keep terminal output and the input prompt readable on the dark background.
   */
  XSetForeground(dpy, gc, 0xf2f2f2);
  Tab *tab = &app->tabs[app->active];
  int lineh = font ? font->ascent + font->descent + 3 : 18;
  const int top = 54;
  int visible = (height - top - 10) / lineh;
  if (visible < 1)
    visible = 1;
  int prompt_visible = !tab->busy;
  size_t total = tab->line_count;
  if (prompt_visible && total == 0)
    total = 1;
  size_t max_first = total > (size_t)visible ? total - (size_t)visible : 0;
  size_t first = max_first > tab->scroll ? max_first - tab->scroll : 0;
  int y = top;
  for (size_t row = first; row < total && y < height - 5; row++, y += lineh) {
    const char *line = row < tab->line_count ? tab->lines[row] : "";
    int line_x = 12;
    if (line[0]) {
      XDrawString(dpy, win, gc, line_x, y, line, (int)strlen(line));
      line_x += font ? XTextWidth(font, line, (int)strlen(line))
                     : (int)strlen(line) * 8;
    }

    /* Append the live prompt to the current row, like a shell prompt. */
    if (prompt_visible && row + 1 == total) {
      char prompt[sizeof(tab->cwd) + 256];
      if (tab->multiline_len)
        strcpy(prompt, "> ");
      else
        build_shell_prompt(tab, prompt, sizeof(prompt));
      XDrawString(dpy, win, gc, line_x, y, prompt, (int)strlen(prompt));
      int px = line_x + (font ? XTextWidth(font, prompt, (int)strlen(prompt))
                              : (int)strlen(prompt) * 8);
      XDrawString(dpy, win, gc, px, y, tab->input, (int)tab->input_len);
      if (!tab->scroll) {
        int cx = px + (font ? XTextWidth(font, tab->input, (int)tab->cursor)
                            : (int)tab->cursor * 8);
        XDrawLine(dpy, win, gc, cx, y - (font ? font->ascent : 10), cx,
                  y + (font ? font->descent : 3));
      }
    }
  }
  XFlush(dpy);
}
static void insert_input_text(Tab *tab, const char *text, size_t n) {
  if (n > TERM_LINE_MAX - 1 - tab->input_len)
    n = TERM_LINE_MAX - 1 - tab->input_len;
  memmove(tab->input + tab->cursor + n, tab->input + tab->cursor,
          tab->input_len - tab->cursor);
  memcpy(tab->input + tab->cursor, text, n);
  tab->input_len += n;
  tab->cursor += n;
  tab->input[tab->input_len] = 0;
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
  if (tab->multiline_len + tab->input_len + 2 >= TERM_LINE_MAX) {
    append_output_text(tab, "terminator: input too long\n", 27);
    tab->multiline_len = 0;
    return;
  }
  int continuation = tab->multiline_len > 0;
  char prompt[sizeof(tab->cwd) + 256];
  if (continuation)
    strcpy(prompt, "> ");
  else
    build_shell_prompt(tab, prompt, sizeof(prompt));
  append_output_text(tab, prompt, strlen(prompt));
  append_output_text(tab, tab->input, tab->input_len);
  append_output_text(tab, "\n", 1);
  memcpy(tab->multiline + tab->multiline_len, tab->input, tab->input_len);
  tab->multiline_len += tab->input_len;
  tab->multiline[tab->multiline_len] = 0;
  if (!shell_is_complete(tab->multiline)) {
    size_t n = tab->input_len;
    if (n && tab->input[n - 1] == '\\') {
      tab->multiline_len--;
      tab->multiline[tab->multiline_len] = 0;
    } else
      tab->multiline[tab->multiline_len++] = '\n';
    tab->input_len = tab->cursor = 0;
    tab->input[0] = 0;
    draw_terminal_window(app);
    return;
  }
  tab->multiline[tab->multiline_len++] = '\n';
  if (send_command_to_worker(tab->input_fd, tab->multiline,
                             tab->multiline_len) < 0)
    append_output_text(tab, "terminator: tab worker is unavailable\n", 38);
  tab->busy = 1;
  tab->scroll = 0;
  tab->multiline_len = 0;
  tab->multiline[0] = 0;
  tab->input_len = tab->cursor = 0;
  tab->input[0] = 0;
  (void)app;
}
static void handle_key_press(App *app, XKeyEvent *key_event) {
  /* Apply editing and control shortcuts to the currently selected tab. */
  Tab *tab = &app->tabs[app->active];
  KeySym key_symbol;
  char buf[64];
  int n = XLookupString(key_event, buf, sizeof(buf), &key_symbol, NULL);
  int ctrl = (key_event->state & ControlMask) != 0;
  if (ctrl && key_symbol == XK_c) {
    int pgid = atomic_load(&tab->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGINT);
      append_output_text(tab, "^C\n", 3);
    }
    return;
  }
  if (ctrl && key_symbol == XK_z) {
    int pgid = atomic_load(&tab->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGTSTP);
      append_output_text(tab, "^Z\n", 3);
    }
    return;
  }
  if (ctrl && key_symbol == XK_d) {
    if (!tab->input_len && !tab->multiline_len) {
      app_close_tab(app, app->active);
      if (!app->count)
        terminator_stopping = 1;
    }
    return;
  }
  if (ctrl && key_symbol == XK_t) {
    if (app_add_tab(app) < 0)
      append_output_text(tab, "terminator: maximum tabs reached\n", 34);
    return;
  }
  if (key_symbol == XK_Return || key_symbol == XK_KP_Enter) {
    submit_input(app, tab);
    return;
  }
  if (key_symbol == XK_BackSpace && tab->cursor) {
    memmove(tab->input + tab->cursor - 1, tab->input + tab->cursor,
            tab->input_len - tab->cursor);
    tab->cursor--;
    tab->input_len--;
    tab->input[tab->input_len] = 0;
    return;
  }
  if (key_symbol == XK_Left && tab->cursor)
    tab->cursor--;
  if (key_symbol == XK_Right && tab->cursor < tab->input_len)
    tab->cursor++;
  if (key_symbol == XK_Home || (ctrl && key_symbol == XK_a))
    tab->cursor = 0;
  if (key_symbol == XK_End || (ctrl && key_symbol == XK_e))
    tab->cursor = tab->input_len;
  if (ctrl && key_symbol == XK_u) {
    memmove(tab->input, tab->input + tab->cursor, tab->input_len - tab->cursor);
    tab->input_len -= tab->cursor;
    tab->cursor = 0;
    tab->input[tab->input_len] = 0;
  }
  if (ctrl && key_symbol == XK_k) {
    tab->input_len = tab->cursor;
    tab->input[tab->input_len] = 0;
  }
  if (ctrl && key_symbol == XK_l) {
    clear_scrollback(tab);
  }
  if (key_symbol == XK_Up && tab->scroll < tab->line_count)
    tab->scroll += 10;
  if (key_symbol == XK_Down && tab->scroll >= 10)
    tab->scroll -= 10;
  if (n > 0 && !ctrl && key_symbol != XK_BackSpace && key_symbol != XK_Return &&
      key_symbol != XK_Left && key_symbol != XK_Right && key_symbol != XK_Up &&
      key_symbol != XK_Down && key_symbol != XK_Home && key_symbol != XK_End)
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
  app->x_fd = ConnectionNumber(dpy);
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 30, 30,
                            (unsigned)width, (unsigned)height, 1,
                            BlackPixel(dpy, screen), 0x300a24);
  XStoreName(dpy, win, "Terminator");
  XSelectInput(dpy, win,
               ExposureMask | KeyPressMask | StructureNotifyMask |
                   ButtonPressMask);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  gc = XCreateGC(dpy, win, 0, NULL);
  font = XLoadQueryFont(dpy, "9x15");
  if (!font)
    font = XLoadQueryFont(dpy, "fixed");
  XMapWindow(dpy, win);
  /* Send the map request before poll() waits on the X connection. */
  XFlush(dpy);
  for (size_t i = 0; i < app->count; i++) {
    fcntl(app->tabs[i].output_fd, F_SETFL, O_NONBLOCK);
    fcntl(app->tabs[i].status_fd, F_SETFL, O_NONBLOCK);
  }
  int xfd = ConnectionNumber(dpy);
  while (app->count && !terminator_stopping) {
    struct pollfd pf[1 + TERM_MAX_TABS * 2];
    size_t map[1 + TERM_MAX_TABS * 2];
    int kind[1 + TERM_MAX_TABS * 2];
    nfds_t nf = 1;
    pf[0] = (struct pollfd){xfd, POLLIN, 0};
    for (size_t i = 0; i < app->count; i++) {
      pf[nf] = (struct pollfd){app->tabs[i].output_fd, POLLIN | POLLHUP, 0};
      map[nf] = i;
      kind[nf++] = 1;
      pf[nf] = (struct pollfd){app->tabs[i].status_fd, POLLIN, 0};
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
            int plus_x = calculate_add_tab_button_x(app->count);
            if (ev.xbutton.x >= plus_x && ev.xbutton.x < plus_x + 32) {
              if (app_add_tab(app) < 0)
                append_output_text(&app->tabs[app->active],
                                   "terminator: maximum tabs reached\n", 34);
            } else {
              int tab_width = calculate_tab_width(app->count);
              size_t ix = ev.xbutton.x >= 8
                              ? (size_t)((ev.xbutton.x - 8) / tab_width)
                              : app->count;
              if (ix < app->count)
                app->active = ix;
            }
            draw_terminal_window(app);
          }
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
            tab->busy = 0;
            if (message.clear_screen)
              clear_scrollback(tab);
            strncpy(tab->cwd, message.cwd, sizeof(tab->cwd) - 1);
            tab->cwd[sizeof(tab->cwd) - 1] = 0;
          }
        }
        if (pf[i].revents & (POLLHUP | POLLERR))
          close_tab[map[i]] = 1;
      }
    for (size_t i = app->count; i > 0; i--)
      if (close_tab[i - 1])
        app_close_tab(app, i - 1);
    if (app->count)
      draw_terminal_window(app);
  }
done:
  if (font)
    XFreeFont(dpy, font);
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return 0;
}
