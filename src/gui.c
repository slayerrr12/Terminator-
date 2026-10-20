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
static void add_text(Tab *t, const char *s, size_t n) {
  while (n) {
    const char *nl = memchr(s, '\n', n);
    size_t z = nl ? (size_t)(nl - s) : n;
    if (t->line_count == t->line_cap) {
      size_t c = t->line_cap ? t->line_cap * 2 : 64;
      char **p = realloc(t->lines, c * sizeof(*p));
      if (!p)
        return;
      t->lines = p;
      t->line_cap = c;
    }
    char *line = strndup(s, z);
    if (!line)
      return;
    if (t->line_count >= 4000) {
      free(t->lines[0]);
      memmove(t->lines, t->lines + 1, (t->line_count - 1) * sizeof(*t->lines));
      t->line_count--;
    }
    t->lines[t->line_count++] = line;
    if (!nl)
      break;
    s = nl + 1;
    n -= z + 1;
  }
}
static void draw(App *a) {
  if (!dpy)
    return;
  XClearWindow(dpy, win);
  if (font)
    XSetFont(dpy, gc, font->fid);
  XSetForeground(dpy, gc, 0xdddddd);
  XFillRectangle(dpy, win, gc, 0, 0, (unsigned)width, 26);
  XSetForeground(dpy, gc, 0x16191f);
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
  XSetForeground(dpy, gc, 0xdddddd);
  Tab *t = &a->tabs[a->active];
  int lineh = font ? font->ascent + font->descent : 16;
  int y = 46;
  int available = (height - 80) / lineh;
  size_t total = t->line_count;
  size_t first = total > (size_t)available ? total - (size_t)available : 0;
  if (t->scroll && first > t->scroll)
    first -= t->scroll;
  for (size_t i = first; i < total && y < height - 35; i++, y += lineh)
    XDrawString(dpy, win, gc, 10, y, t->lines[i], (int)strlen(t->lines[i]));
  char prompt[128];
  snprintf(prompt, sizeof(prompt), "%s@terminator> ",
           getenv("USER") ? getenv("USER") : "user");
  if (t->multiline_len)
    strcpy(prompt, "> ");
  y = height - 12;
  XDrawString(dpy, win, gc, 10, y, prompt, (int)strlen(prompt));
  int px = 10 + (font ? XTextWidth(font, prompt, (int)strlen(prompt))
                      : (int)strlen(prompt) * 8);
  XDrawString(dpy, win, gc, px, y, t->input, (int)t->input_len);
  int cx = px + (font ? XTextWidth(font, t->input, (int)t->cursor)
                      : (int)t->cursor * 8);
  XDrawLine(dpy, win, gc, cx, y - (font ? font->ascent : 10), cx,
            y + (font ? font->descent : 3));
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
  const char *prompt = continuation ? "> " : "user@terminator> ";
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
                            BlackPixel(dpy, screen), BlackPixel(dpy, screen));
  XStoreName(dpy, win, "Terminator");
  XSelectInput(dpy, win,
               ExposureMask | KeyPressMask | StructureNotifyMask |
                   ButtonPressMask);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete, 1);
  gc = XCreateGC(dpy, win, 0, NULL);
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
