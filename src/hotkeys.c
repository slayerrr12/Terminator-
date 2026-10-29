#include "hotkeys.h"
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defaults, config-file loading, validation, and X11 key-event matching. */
static const char *action_names[HOTKEY_COUNT] = {
    [HOTKEY_MOVE_LEFT] = "move_left",
    [HOTKEY_MOVE_RIGHT] = "move_right",
    [HOTKEY_LINE_START] = "line_start",
    [HOTKEY_LINE_END] = "line_end",
    [HOTKEY_HISTORY_UP] = "history_up",
    [HOTKEY_HISTORY_DOWN] = "history_down",
    [HOTKEY_DELETE_TO_START] = "delete_to_start",
    [HOTKEY_DELETE_TO_END] = "delete_to_end",
    [HOTKEY_DELETE_PREVIOUS_WORD] = "delete_previous_word",
    [HOTKEY_CLEAR_SCREEN] = "clear_screen",
    [HOTKEY_CLOSE_TAB] = "close_tab",
    [HOTKEY_COPY] = "copy",
    [HOTKEY_PASTE] = "paste",
    [HOTKEY_INTERRUPT] = "interrupt",
    [HOTKEY_SUSPEND] = "suspend",
    [HOTKEY_NEW_TAB] = "new_tab",
};

static const unsigned int relevant_modifiers =
    ControlMask | ShiftMask | Mod1Mask;

static HotkeyBinding binding(KeySym key, unsigned int modifiers) {
  HotkeyBinding result = {key, modifiers};
  return result;
}

void hotkeys_set_defaults(HotkeyConfig *config) {
  memset(config, 0, sizeof(*config));
  config->bindings[HOTKEY_MOVE_LEFT] = binding(XK_Left, 0);
  config->bindings[HOTKEY_MOVE_RIGHT] = binding(XK_Right, 0);
  config->bindings[HOTKEY_LINE_START] = binding(XK_a, ControlMask);
  config->bindings[HOTKEY_LINE_END] = binding(XK_e, ControlMask);
  config->bindings[HOTKEY_HISTORY_UP] = binding(XK_Up, 0);
  config->bindings[HOTKEY_HISTORY_DOWN] = binding(XK_Down, 0);
  config->bindings[HOTKEY_DELETE_TO_START] = binding(XK_u, ControlMask);
  config->bindings[HOTKEY_DELETE_TO_END] = binding(XK_k, ControlMask);
  config->bindings[HOTKEY_DELETE_PREVIOUS_WORD] = binding(XK_w, ControlMask);
  config->bindings[HOTKEY_CLEAR_SCREEN] = binding(XK_l, ControlMask);
  config->bindings[HOTKEY_CLOSE_TAB] = binding(XK_d, ControlMask);
  config->bindings[HOTKEY_COPY] = binding(XK_c, ControlMask | ShiftMask);
  config->bindings[HOTKEY_PASTE] = binding(XK_v, ControlMask | ShiftMask);
  config->bindings[HOTKEY_INTERRUPT] = binding(XK_c, ControlMask);
  config->bindings[HOTKEY_SUSPEND] = binding(XK_z, ControlMask);
  config->bindings[HOTKEY_NEW_TAB] = binding(XK_t, ControlMask);
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text))
    text++;
  size_t length = strlen(text);
  while (length && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';
  return text;
}

static void add_warning(char *warnings, size_t size, size_t line_number,
                        const char *reason) {
  if (!warnings || !size)
    return;
  size_t used = strlen(warnings);
  if (used >= size - 1)
    return;
  if (line_number)
    snprintf(warnings + used, size - used,
             "terminator: ~/.terminatorrc:%zu: %s; using default\n",
             line_number, reason);
  else
    snprintf(warnings + used, size - used, "terminator: %s\n", reason);
}

static HotkeyAction find_action(const char *name) {
  for (int action = 1; action < HOTKEY_COUNT; action++)
    if (!strcmp(name, action_names[action]))
      return (HotkeyAction)action;
  return HOTKEY_NONE;
}

static KeySym parse_key_name(char *name) {
  if (name[0] && !name[1] && isalpha((unsigned char)name[0]))
    name[0] = (char)tolower((unsigned char)name[0]);
  return XStringToKeysym(name);
}

static int parse_binding(char *text, HotkeyBinding *result) {
  unsigned int modifiers = 0;
  KeySym key = NoSymbol;
  char *part = text;
  while (part && *part) {
    char *next = strchr(part, '+');
    if (next)
      *next++ = '\0';
    part = trim(part);
    unsigned int modifier = 0;
    if (!strcmp(part, "Ctrl") || !strcmp(part, "Control"))
      modifier = ControlMask;
    else if (!strcmp(part, "Shift"))
      modifier = ShiftMask;
    else if (!strcmp(part, "Alt"))
      modifier = Mod1Mask;

    if (modifier) {
      if (modifiers & modifier)
        return -1;
      modifiers |= modifier;
    } else {
      if (key != NoSymbol || !*part)
        return -1;
      key = parse_key_name(part);
      if (key == NoSymbol)
        return -1;
    }
    part = next;
  }
  if (key == NoSymbol)
    return -1;
  result->key = key;
  result->modifiers = modifiers;
  return 0;
}

static int same_binding(HotkeyBinding first, HotkeyBinding second) {
  return first.key == second.key && first.modifiers == second.modifiers;
}

/* Keep keys already used by Enter, deletion, and viewport controls reserved. */
static int binding_uses_terminal_key(HotkeyBinding candidate) {
  switch (candidate.key) {
  case XK_Return:
  case XK_KP_Enter:
  case XK_BackSpace:
  case XK_Home:
  case XK_End:
  case XK_Page_Up:
  case XK_Page_Down:
    return 1;
  default:
    return 0;
  }
}

static void reject_binding(HotkeyAction action, int *rejected, char *warnings,
                          size_t warnings_size, size_t line_number) {
  rejected[action] = 1;
  char reason[128];
  snprintf(reason, sizeof(reason), "binding for %s conflicts with another key",
           action_names[action]);
  add_warning(warnings, warnings_size, line_number, reason);
}

int hotkeys_load_user_config(HotkeyConfig *config, char *warnings,
                             size_t warnings_size) {
  HotkeyBinding requested[HOTKEY_COUNT];
  size_t config_line[HOTKEY_COUNT] = {0};
  int configured[HOTKEY_COUNT] = {0};
  int rejected[HOTKEY_COUNT] = {0};
  int seen[HOTKEY_COUNT] = {0};
  char path[4096];
  const char *home = getenv("HOME");
  if (warnings && warnings_size)
    warnings[0] = '\0';
  hotkeys_set_defaults(config);
  if (!home || !*home ||
      snprintf(path, sizeof(path), "%s/.terminatorrc", home) >=
                   (int)sizeof(path)) {
    add_warning(warnings, warnings_size, 0,
                "HOME is missing or the hotkey config path is too long");
    return -1;
  }

  FILE *file = fopen(path, "r");
  if (!file) {
    if (errno == ENOENT)
      return 0;
    char message[160];
    snprintf(message, sizeof(message), "cannot read ~/.terminatorrc: %s",
             strerror(errno));
    add_warning(warnings, warnings_size, 0, message);
    return -1;
  }

  char line[512];
  size_t line_number = 0;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    if (!strchr(line, '\n') && !feof(file)) {
      int character;
      while ((character = fgetc(file)) != '\n' && character != EOF) {
      }
      add_warning(warnings, warnings_size, line_number, "line is too long");
      continue;
    }
    char *content = trim(line);
    if (!*content || *content == '#')
      continue;
    char *equals = strchr(content, '=');
    if (!equals) {
      add_warning(warnings, warnings_size, line_number,
                  "expected action = key binding");
      continue;
    }
    *equals++ = '\0';
    char *action_text = trim(content);
    char *key_text = trim(equals);
    HotkeyAction action = find_action(action_text);
    if (action == HOTKEY_NONE) {
      add_warning(warnings, warnings_size, line_number, "unknown action");
      continue;
    }
    if (seen[action]) {
      rejected[action] = 1;
      configured[action] = 0;
      add_warning(warnings, warnings_size, line_number,
                  "action appears more than once");
      continue;
    }
    seen[action] = 1;
    if (parse_binding(key_text, &requested[action]) < 0) {
      rejected[action] = 1;
      add_warning(warnings, warnings_size, line_number,
                  "invalid key binding");
      continue;
    }
    if (binding_uses_terminal_key(requested[action])) {
      rejected[action] = 1;
      add_warning(warnings, warnings_size, line_number,
                  "key is reserved for terminal input");
      continue;
    }
    configured[action] = 1;
    config_line[action] = line_number;
  }
  fclose(file);

  /* Resolve collisions against the final custom bindings and defaults. */
  int changed;
  do {
    changed = 0;
    for (int first = 1; first < HOTKEY_COUNT && !changed; first++) {
      HotkeyBinding first_binding =
          configured[first] && !rejected[first] ? requested[first]
                                                : config->bindings[first];
      for (int second = first + 1; second < HOTKEY_COUNT; second++) {
        HotkeyBinding second_binding =
            configured[second] && !rejected[second] ? requested[second]
                                                    : config->bindings[second];
        if (!same_binding(first_binding, second_binding))
          continue;
        if (configured[first] && !rejected[first])
          reject_binding((HotkeyAction)first, rejected, warnings,
                         warnings_size, config_line[first]);
        if (configured[second] && !rejected[second])
          reject_binding((HotkeyAction)second, rejected, warnings,
                         warnings_size, config_line[second]);
        changed = 1;
        break;
      }
    }
  } while (changed);

  for (int action = 1; action < HOTKEY_COUNT; action++)
    if (configured[action] && !rejected[action])
      config->bindings[action] = requested[action];
  return warnings && warnings_size && warnings[0] ? 1 : 0;
}

HotkeyAction hotkeys_find_action(const HotkeyConfig *config, KeySym key,
                                 unsigned int event_state) {
  unsigned int modifiers = event_state & relevant_modifiers;
  for (int action = 1; action < HOTKEY_COUNT; action++) {
    HotkeyBinding current = config->bindings[action];
    if (current.key == key && current.modifiers == modifiers)
      return (HotkeyAction)action;
  }
  return HOTKEY_NONE;
}
