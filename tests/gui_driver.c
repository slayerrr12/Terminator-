#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
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

static void drag_select(Display *d, Window w, int y, int first_x, int last_x) {
  XEvent e = {0};
  e.xbutton.type = ButtonPress;
  e.xbutton.display = d;
  e.xbutton.window = w;
  e.xbutton.root = DefaultRootWindow(d);
  e.xbutton.time = CurrentTime;
  e.xbutton.x = first_x;
  e.xbutton.y = y;
  e.xbutton.button = Button1;
  e.xbutton.same_screen = True;
  XSendEvent(d, w, True, ButtonPressMask, &e);
  XFlush(d);
  usleep(20000);

  memset(&e, 0, sizeof(e));
  e.xmotion.type = MotionNotify;
  e.xmotion.display = d;
  e.xmotion.window = w;
  e.xmotion.root = DefaultRootWindow(d);
  e.xmotion.time = CurrentTime;
  e.xmotion.x = last_x;
  e.xmotion.y = y;
  e.xmotion.state = Button1Mask;
  e.xmotion.same_screen = True;
  XSendEvent(d, w, True, ButtonMotionMask, &e);
  XFlush(d);
  usleep(20000);

  memset(&e, 0, sizeof(e));
  e.xbutton.type = ButtonRelease;
  e.xbutton.display = d;
  e.xbutton.window = w;
  e.xbutton.root = DefaultRootWindow(d);
  e.xbutton.time = CurrentTime;
  e.xbutton.x = last_x;
  e.xbutton.y = y;
  e.xbutton.button = Button1;
  e.xbutton.same_screen = True;
  XSendEvent(d, w, True, ButtonReleaseMask, &e);
  XFlush(d);
  usleep(30000);
}

static void assert_clipboard_text(Display *d, const char *expected) {
  Window requestor = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1,
                                         0, 0, 0);
  Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
  Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
  Atom property = XInternAtom(d, "TEST_CLIPBOARD", False);
  XConvertSelection(d, clipboard, utf8, property, requestor, CurrentTime);
  XFlush(d);

  XEvent event;
  int received = 0;
  for (int i = 0; i < 200 && !received; ++i) {
    received = XCheckTypedWindowEvent(d, requestor, SelectionNotify, &event);
    if (!received)
      usleep(10000);
  }
  assert(received);
  assert(event.xselection.property == property);

  Atom actual_type;
  int format;
  unsigned long count, remaining;
  unsigned char *data = NULL;
  assert(XGetWindowProperty(d, requestor, property, 0, 64, True,
                            AnyPropertyType, &actual_type, &format, &count,
                            &remaining, &data) == Success);
  assert(format == 8 && data);
  assert(count == strlen(expected));
  assert(memcmp(data, expected, count) == 0);
  XFree(data);
  XDestroyWindow(d, requestor);
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

static void type_input(Display *d, Window w, const char *s) {
  for (; *s; ++s) {
    KeySym k = symbol(*s);
    assert(k != NoSymbol);
    key(d, w, k, *s == '_' ? ShiftMask : 0);
  }
}

static void type(Display *d, Window w, const char *s) {
  type_input(d, w, s);
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
  type(d, w, "echo clipboard-check");
  usleep(200000);
  drag_select(d, w, 90, 12, 147);
  key(d, w, XK_c, ControlMask | ShiftMask);
  usleep(50000);
  assert_clipboard_text(d, "clipboard-check");
  type_input(d, w, "touch /tmp/");
  key(d, w, XK_v, ControlMask | ShiftMask);
  usleep(100000);
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/clipboard-check", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/clipboard-check", F_OK) == 0);

  type_input(d, w, "touch /tmp/old");
  key(d, w, XK_w, ControlMask);
  type_input(d, w, "/tmp/terminator-ctrl-w");
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-ctrl-w", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-ctrl-w", F_OK) == 0);

  type_input(d, w, "garbage");
  key(d, w, XK_a, ControlMask);
  type_input(d, w, "touch /tmp/terminator-ctrl-k");
  key(d, w, XK_k, ControlMask);
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-ctrl-k", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-ctrl-k", F_OK) == 0);

  type_input(d, w, "garbage");
  key(d, w, XK_u, ControlMask);
  type_input(d, w, "touch /tmp/terminator-ctrl-u");
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-ctrl-u", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-ctrl-u", F_OK) == 0);

  type_input(d, w, "garbage");
  key(d, w, XK_a, ControlMask);
  key(d, w, XK_e, ControlMask);
  key(d, w, XK_w, ControlMask);
  type_input(d, w, "touch /tmp/terminator-ctrl-e");
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-ctrl-e", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-ctrl-e", F_OK) == 0);

  unlink("/tmp/terminator-ctrl-k");
  unlink("/tmp/terminator-ctrl-u");
  unlink("/tmp/terminator-ctrl-e");
  type_input(d, w, "touch /tmp/terminator-history-draft");
  key(d, w, XK_Up, 0);
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-ctrl-e", F_OK); ++i)
    usleep(50000);
  /* Up recalls the last completed command. */
  assert(access("/tmp/terminator-ctrl-e", F_OK) == 0);

  unlink("/tmp/terminator-history-draft");
  type_input(d, w, "touch /tmp/terminator-history-draft");
  key(d, w, XK_Up, 0);
  key(d, w, XK_Down, 0);
  key(d, w, XK_Return, 0);
  for (int i = 0; i < 100 && access("/tmp/terminator-history-draft", F_OK); ++i)
    usleep(50000);
  assert(access("/tmp/terminator-history-draft", F_OK) == 0);

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
  unlink("/tmp/clipboard-check");
  unlink("/tmp/terminator-ctrl-w");
  unlink("/tmp/terminator-ctrl-k");
  unlink("/tmp/terminator-history-draft");
  XCloseDisplay(d);
  return 0;
}
