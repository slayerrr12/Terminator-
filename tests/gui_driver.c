#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Send X11 key/click events to verify commands and tab switching end to end. */
static Window find_window(Display *d, Window root) {
  Window returned_root, parent, *children = NULL, found = 0;
  unsigned int count = 0;
  if (!XQueryTree(d, root, &returned_root, &parent, &children, &count))
    return 0;
  for (unsigned int i = 0; i < count && !found; ++i) {
    char *name = NULL;
    if (XFetchName(d, children[i], &name) && name) {
      if (!strcmp(name, "Terminator"))
        found = children[i];
      XFree(name);
    }
    if (!found)
      found = find_window(d, children[i]);
  }
  if (children)
    XFree(children);
  return found;
}

static void key(Display *d, Window w, KeySym sym, unsigned int state) {
  XEvent e = {0};
  e.xkey.type = KeyPress;
  e.xkey.display = d;
  e.xkey.window = w;
  e.xkey.root = DefaultRootWindow(d);
  e.xkey.time = CurrentTime;
  e.xkey.same_screen = True;
  e.xkey.keycode = XKeysymToKeycode(d, sym);
  e.xkey.state = state;
  XSendEvent(d, w, True, KeyPressMask, &e);
  XFlush(d);
  usleep(12000);
}

static void click_tab(Display *d, Window w, int x) {
  XEvent e = {0};
  e.xbutton.type = ButtonPress;
  e.xbutton.display = d;
  e.xbutton.window = w;
  e.xbutton.root = DefaultRootWindow(d);
  e.xbutton.time = CurrentTime;
  e.xbutton.x = x;
  e.xbutton.y = 12;
  e.xbutton.button = Button1;
  e.xbutton.same_screen = True;
  XSendEvent(d, w, True, ButtonPressMask, &e);
  XFlush(d);
  usleep(50000);
}

static void wheel(Display *d, Window w, unsigned int direction) {
  XEvent e = {0};
  e.xbutton.type = ButtonPress;
  e.xbutton.display = d;
  e.xbutton.window = w;
  e.xbutton.root = DefaultRootWindow(d);
  e.xbutton.time = CurrentTime;
  e.xbutton.x = 200;
  e.xbutton.y = 100;
  e.xbutton.button = direction;
  e.xbutton.same_screen = True;
  XSendEvent(d, w, True, ButtonPressMask, &e);
  XFlush(d);
  usleep(12000);
}

static KeySym symbol(char c) {
  if (c >= 'a' && c <= 'z')
    return (KeySym)(XK_a + c - 'a');
  if (c >= '0' && c <= '9')
    return (KeySym)(XK_0 + c - '0');
  switch (c) {
  case ' ':
    return XK_space;
  case '/':
    return XK_slash;
  case '.':
    return XK_period;
  case '-':
    return XK_minus;
  case '_':
    return XK_underscore;
  default:
    return NoSymbol;
  }
}

static void type(Display *d, Window w, const char *s) {
  for (; *s; ++s) {
    KeySym k = symbol(*s);
    assert(k != NoSymbol);
    key(d, w, k, *s == '_' ? ShiftMask : 0);
  }
  key(d, w, XK_Return, 0);
}

int main(int argument_count, char **arguments) {
  assert(argument_count == 2);
  Display *d = XOpenDisplay(NULL);
  assert(d);
  Window w = 0;
  for (int i = 0; i < 100 && !w; ++i) {
    w = find_window(d, DefaultRootWindow(d));
    usleep(50000);
  }
  assert(w);
  type(d, w, "touch /tmp/terminator-gui-first");
  for (int i = 0; i < 100 && access("/tmp/terminator-gui-first", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-gui-first", F_OK) == 0);
  type(d, w, "seq 1 500");
  usleep(300000);
  key(d, w, XK_Page_Up, 0);
  wheel(d, w, Button4);
  key(d, w, XK_Page_Down, 0);
  wheel(d, w, Button5);
  key(d, w, XK_t, ControlMask);
  char command[512];
  assert(snprintf(command, sizeof(command), "touch %s", arguments[1]) > 0);
  type(d, w, command);
  for (int i = 0; i < 100 && access(arguments[1], F_OK); ++i)
    usleep(50000);
  assert(access(arguments[1], F_OK) == 0);
  click_tab(d, w, 20);
  type(d, w, "touch /tmp/terminator-gui-tab-one");
  for (int i = 0; i < 100 && access("/tmp/terminator-gui-tab-one", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-gui-tab-one", F_OK) == 0);
  click_tab(d, w, 200);
  type(d, w, "touch /tmp/terminator-gui-tab-two");
  for (int i = 0; i < 100 && access("/tmp/terminator-gui-tab-two", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-gui-tab-two", F_OK) == 0);
  key(d, w, XK_d, ControlMask);
  usleep(100000);
  unlink("/tmp/terminator-gui-first");
  unlink(arguments[1]);
  unlink("/tmp/terminator-gui-tab-one");
  unlink("/tmp/terminator-gui-tab-two");
  XCloseDisplay(d);
  return 0;
}
