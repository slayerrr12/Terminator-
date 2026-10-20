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

/* Xlib and all per-tab drawing stay on this one GUI thread. */
static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static int width = 900, height = 600;
static Atom wm_delete;
static int add_line(Tab *t) {
  /* Bound per-tab scrollback while keeping the newest rows. */
  if (t->line_count == 4000) {
    free(t->lines[0]);
    memmove(t->lines, t->lines + 1,
            (t->line_count - 1) * sizeof(*t->lines));
    t->line_count--;
    if (t->scroll)
      t->scroll--;
  }
  if (t->line_count == t->line_cap) {
    size_t cap = t->line_cap ? t->line_cap * 2 : 64;
    char **lines = realloc(t->lines, cap * sizeof(*lines));
    if (!lines)
      return -1;
    t->lines = lines;
    t->line_cap = cap;
  }
  t->lines[t->line_count] = calloc(1, 1);
  if (!t->lines[t->line_count])
    return -1;
  t->line_count++;
  return 0;
}

static int append_to_current_line(Tab *t, const char *s, size_t n) {
  if (!n)
    return 0;
  if (!t->line_count && add_line(t) < 0)
    return -1;
  char *line = t->lines[t->line_count - 1];
  size_t old_len = strlen(line);
  char *grown = realloc(line, old_len + n + 1);
  if (!grown)
    return -1;
  memcpy(grown + old_len, s, n);
  grown[old_len + n] = '\0';
  t->lines[t->line_count - 1] = grown;
  return 0;
}

/*
 * Worker output arrives in arbitrary pipe-sized chunks. Append it to the
 * current row so a line split across reads is displayed as one terminal line.
 */
static void add_text(Tab *t, const char *s, size_t n) {
  size_t start = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i < n && s[i] != '\n' && s[i] != '\r' && s[i] != '\b' &&
        s[i] != '\t')
      continue;
    if (append_to_current_line(t, s + start, i - start) < 0)
      return;
    if (i == n)
      break;
    if (s[i] == '\n') {
      if (add_line(t) < 0)
        return;
    } else if (s[i] == '\r') {
      if (!t->line_count && add_line(t) < 0)
        return;
      t->lines[t->line_count - 1][0] = '\0';
    } else if (s[i] == '\b') {
      if (t->line_count) {
        size_t len = strlen(t->lines[t->line_count - 1]);
        if (len)
          t->lines[t->line_count - 1][len - 1] = '\0';
      }
    } else {
      size_t col = t->line_count ? strlen(t->lines[t->line_count - 1]) : 0;
      size_t spaces = 8 - col % 8;
      static const char blanks[] = "        ";
      if (append_to_current_line(t, blanks, spaces) < 0)
        return;
    }
    start = i + 1;
  }
}

static void make_prompt(const Tab *t, char *prompt, size_t size) {
  const char *user = getenv("USER");
  char host[128] = "localhost";
  if (!user || !*user)
    user = "user";
  if (gethostname(host, sizeof(host)) < 0)
    strcpy(host, "localhost");
  host[sizeof(host) - 1] = '\0';

  const char *cwd = t->cwd;
  const char *home = getenv("HOME");
  char display_cwd[sizeof(t->cwd) + 2];
  if (home && *home && !strncmp(cwd, home, strlen(home)) &&
      (cwd[strlen(home)] == '\0' || cwd[strlen(home)] == '/')) {
    snprintf(display_cwd, sizeof(display_cwd), "~%s", cwd + strlen(home));
    cwd = display_cwd;
  }
  snprintf(prompt, size, "%s@%s:%s$ ", user, host, cwd);
}
static void draw(App *a) {
  if (!dpy)
    return;
  XClearWindow(dpy, win);
  if (font)
    XSetFont(dpy, gc, font->fid);
  XSetForeground(dpy, gc, 0x4a2944);
  XFillRectangle(dpy, win, gc, 0, 0, (unsigned)width, 26);
  XSetForeground(dpy, gc, 0xf2f2f2);
  int x = 10;
  for (size_t i = 0; i < a->count; i++) {
    char label[128];
    snprintf(label, sizeof(label), "[%zu: %s]%s", i + 1, a->tabs[i].cwd,
             i == a->active ? " *" : "");
    XDrawString(dpy, win, gc, x, 18, label, (int)strlen(label));
    x += 180;
  }
  XDrawString(dpy, win, gc, width - 35, 18, "[+]", 3);
  /* Keep terminal output and the input prompt readable on the dark background. */
  XSetForeground(dpy, gc, 0xf2f2f2);
  Tab *t = &a->tabs[a->active];
  int lineh = font ? font->ascent + font->descent + 3 : 18;
  const int top = 48;
  int visible = (height - top - 10) / lineh;
  if (visible < 1)
    visible = 1;
  int prompt_visible = !t->busy;
  size_t total = t->line_count;
  if (prompt_visible && total == 0)
    total = 1;
  size_t max_first = total > (size_t)visible ? total - (size_t)visible : 0;
  size_t first = max_first > t->scroll ? max_first - t->scroll : 0;
  int y = top;
  for (size_t row = first; row < total && y < height - 5;
       row++, y += lineh) {
    const char *line = row < t->line_count ? t->lines[row] : "";
    int line_x = 12;
    if (line[0]) {
      XDrawString(dpy, win, gc, line_x, y, line, (int)strlen(line));
      line_x += font ? XTextWidth(font, line, (int)strlen(line))
                     : (int)strlen(line) * 8;
    }

    /* Append the live prompt to the current row, like a shell prompt. */
    if (prompt_visible && row + 1 == total) {
      char prompt[sizeof(t->cwd) + 256];
      if (t->multiline_len)
        strcpy(prompt, "> ");
      else
        make_prompt(t, prompt, sizeof(prompt));
      XDrawString(dpy, win, gc, line_x, y, prompt, (int)strlen(prompt));
      int px = line_x + (font ? XTextWidth(font, prompt, (int)strlen(prompt))
                              : (int)strlen(prompt) * 8);
      XDrawString(dpy, win, gc, px, y, t->input, (int)t->input_len);
      if (!t->scroll) {
        int cx = px + (font ? XTextWidth(font, t->input, (int)t->cursor)
                            : (int)t->cursor * 8);
        XDrawLine(dpy, win, gc, cx, y - (font ? font->ascent : 10), cx,
                  y + (font ? font->descent : 3));
      }
    }
  }
  XFlush(dpy);
}
static void append_input(Tab *t, const char *s, size_t n) {
  if (n > TERM_LINE_MAX - 1 - t->input_len)
    n = TERM_LINE_MAX - 1 - t->input_len;
  memmove(t->input + t->cursor + n, t->input + t->cursor,
          t->input_len - t->cursor);
  memcpy(t->input + t->cursor, s, n);
  t->input_len += n;
  t->cursor += n;
  t->input[t->input_len] = 0;
}
static int write_command(int fd, const char *s, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = write(fd, s + sent, n - sent);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    sent += (size_t)w;
  }
  return 0;
}
static void submit(App *a, Tab *t) {
  if (t->multiline_len + t->input_len + 2 >= TERM_LINE_MAX) {
    add_text(t, "terminator: input too long\n", 27);
    t->multiline_len = 0;
    return;
  }
  int continuation = t->multiline_len > 0;
  char prompt[sizeof(t->cwd) + 256];
  if (continuation)
    strcpy(prompt, "> ");
  else
    make_prompt(t, prompt, sizeof(prompt));
  add_text(t, prompt, strlen(prompt));
  add_text(t, t->input, t->input_len);
  add_text(t, "\n", 1);
  memcpy(t->multiline + t->multiline_len, t->input, t->input_len);
  t->multiline_len += t->input_len;
  t->multiline[t->multiline_len] = 0;
  if (!shell_is_complete(t->multiline)) {
    size_t n = t->input_len;
    if (n && t->input[n - 1] == '\\') {
      t->multiline_len--;
      t->multiline[t->multiline_len] = 0;
    } else
      t->multiline[t->multiline_len++] = '\n';
    t->input_len = t->cursor = 0;
    t->input[0] = 0;
    draw(a);
    return;
  }
  t->multiline[t->multiline_len++] = '\n';
  if (write_command(t->input_fd, t->multiline, t->multiline_len) < 0)
    add_text(t, "terminator: tab worker is unavailable\n", 38);
  t->busy = 1;
  t->scroll = 0;
  t->multiline_len = 0;
  t->multiline[0] = 0;
  t->input_len = t->cursor = 0;
  t->input[0] = 0;
  (void)a;
}
static void handle_key(App *a, XKeyEvent *e) {
  Tab *t = &a->tabs[a->active];
  KeySym k;
  char buf[64];
  int n = XLookupString(e, buf, sizeof(buf), &k, NULL);
  int ctrl = (e->state & ControlMask) != 0;
  if (ctrl && k == XK_c) {
    int pgid = atomic_load(&t->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGINT);
      add_text(t, "^C\n", 3);
    }
    return;
  }
  if (ctrl && k == XK_z) {
    int pgid = atomic_load(&t->foreground->pgid);
    if (pgid > 0) {
      (void)kill(-pgid, SIGTSTP);
      add_text(t, "^Z\n", 3);
    }
    return;
  }
  if (ctrl && k == XK_d) {
    if (!t->input_len && !t->multiline_len) {
      app_close_tab(a, a->active);
      if (!a->count)
        terminator_stopping = 1;
    }
    return;
  }
  if (ctrl && k == XK_t) {
    if (app_add_tab(a) < 0)
      add_text(t, "terminator: maximum tabs reached\n", 34);
    return;
  }
  if (k == XK_Return || k == XK_KP_Enter) {
    submit(a, t);
    return;
  }
  if (k == XK_BackSpace && t->cursor) {
    memmove(t->input + t->cursor - 1, t->input + t->cursor,
            t->input_len - t->cursor);
    t->cursor--;
    t->input_len--;
    t->input[t->input_len] = 0;
    return;
  }
  if (k == XK_Left && t->cursor)
    t->cursor--;
  if (k == XK_Right && t->cursor < t->input_len)
    t->cursor++;
  if (k == XK_Home || (ctrl && k == XK_a))
    t->cursor = 0;
  if (k == XK_End || (ctrl && k == XK_e))
    t->cursor = t->input_len;
  if (ctrl && k == XK_u) {
    memmove(t->input, t->input + t->cursor, t->input_len - t->cursor);
    t->input_len -= t->cursor;
    t->cursor = 0;
    t->input[t->input_len] = 0;
  }
  if (ctrl && k == XK_k) {
    t->input_len = t->cursor;
    t->input[t->input_len] = 0;
  }
  if (ctrl && k == XK_l) {
    for (size_t i = 0; i < t->line_count; i++)
      free(t->lines[i]);
    t->line_count = 0;
    t->scroll = 0;
  }
  if (k == XK_Up && t->scroll < t->line_count)
    t->scroll += 10;
  if (k == XK_Down && t->scroll >= 10)
    t->scroll -= 10;
  if (n > 0 && !ctrl && k != XK_BackSpace && k != XK_Return && k != XK_Left &&
      k != XK_Right && k != XK_Up && k != XK_Down && k != XK_Home &&
      k != XK_End)
    append_input(t, buf, (size_t)n);
}
int gui_run(App *a) {
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display\n");
    return 1;
  }
  int screen = DefaultScreen(dpy);
  a->x_fd = ConnectionNumber(dpy);
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
  for (size_t i = 0; i < a->count; i++) {
    fcntl(a->tabs[i].output_fd, F_SETFL, O_NONBLOCK);
    fcntl(a->tabs[i].status_fd, F_SETFL, O_NONBLOCK);
  }
  int xfd = ConnectionNumber(dpy);
  while (a->count && !terminator_stopping) {
    struct pollfd pf[1 + TERM_MAX_TABS * 2];
    size_t map[1 + TERM_MAX_TABS * 2];
    int kind[1 + TERM_MAX_TABS * 2];
    nfds_t nf = 1;
    pf[0] = (struct pollfd){xfd, POLLIN, 0};
    for (size_t i = 0; i < a->count; i++) {
      pf[nf] = (struct pollfd){a->tabs[i].output_fd, POLLIN | POLLHUP, 0};
      map[nf] = i;
      kind[nf++] = 1;
      pf[nf] = (struct pollfd){a->tabs[i].status_fd, POLLIN, 0};
      map[nf] = i;
      kind[nf++] = 2;
    }
    /* Sleep until X11 or a worker pipe is ready; the GUI does not spin. */
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
          draw(a);
        else if (ev.type == ConfigureNotify) {
          width = ev.xconfigure.width;
          height = ev.xconfigure.height;
          draw(a);
        } else if (ev.type == KeyPress) {
          handle_key(a, &ev.xkey);
          draw(a);
        } else if (ev.type == ButtonPress) {
          if (ev.xbutton.y < 26) {
            if (ev.xbutton.x > width - 50) {
              if (app_add_tab(a) < 0)
                add_text(&a->tabs[a->active],
                         "terminator: maximum tabs reached\n", 34);
            } else {
              size_t ix = (size_t)(ev.xbutton.x / 180);
              if (ix < a->count)
                a->active = ix;
            }
            draw(a);
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
        Tab *t = &a->tabs[map[i]];
        if (kind[i] == 1) {
          char b[2048];
          ssize_t n;
          while ((n = read(pf[i].fd, b, sizeof(b))) > 0)
            add_text(t, b, (size_t)n);
        } else {
          WorkerStatus message;
          ssize_t n;
          while ((n = read(pf[i].fd, &message, sizeof(message))) ==
                 (ssize_t)sizeof(message)) {
            t->busy = 0;
            strncpy(t->cwd, message.cwd, sizeof(t->cwd) - 1);
            t->cwd[sizeof(t->cwd) - 1] = 0;
          }
        }
        if (pf[i].revents & (POLLHUP | POLLERR))
          close_tab[map[i]] = 1;
      }
    for (size_t i = a->count; i > 0; i--)
      if (close_tab[i - 1])
        app_close_tab(a, i - 1);
    if (a->count)
      draw(a);
  }
done:
  if (font)
    XFreeFont(dpy, font);
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return 0;
}
