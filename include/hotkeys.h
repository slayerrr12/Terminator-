#ifndef TERM_HOTKEYS_H
#define TERM_HOTKEYS_H

#include <X11/Xlib.h>
#include <stddef.h>

typedef enum {
  HOTKEY_NONE,
  HOTKEY_MOVE_LEFT,
  HOTKEY_MOVE_RIGHT,
  HOTKEY_LINE_START,
  HOTKEY_LINE_END,
  HOTKEY_HISTORY_UP,
  HOTKEY_HISTORY_DOWN,
  HOTKEY_DELETE_TO_START,
  HOTKEY_DELETE_TO_END,
  HOTKEY_DELETE_PREVIOUS_WORD,
  HOTKEY_CLEAR_SCREEN,
  HOTKEY_CLOSE_TAB,
  HOTKEY_COPY,
  HOTKEY_PASTE,
  HOTKEY_INTERRUPT,
  HOTKEY_SUSPEND,
  HOTKEY_NEW_TAB,
  HOTKEY_COUNT
} HotkeyAction;

typedef struct {
  KeySym key;
  unsigned int modifiers;
} HotkeyBinding;

typedef struct {
  HotkeyBinding bindings[HOTKEY_COUNT];
} HotkeyConfig;

void hotkeys_set_defaults(HotkeyConfig *config);
int hotkeys_load_user_config(HotkeyConfig *config, char *warnings,
                             size_t warnings_size);
HotkeyAction hotkeys_find_action(const HotkeyConfig *config, KeySym key,
                                 unsigned int event_state);

#endif
