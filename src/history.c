#include "terminator.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Bounded history ring usable from heap memory or MAP_SHARED process memory. */
int history_init(SharedHistory *history, int process_shared) {
  memset(history, 0, sizeof(*history));
  return sem_init(&history->semaphore, process_shared ? 1 : 0, 1);
}

static int lock_history(SharedHistory *history) {
  /* sem_wait can be interrupted by a signal; retry without losing the lock. */
  int result;
  do {
    result = sem_wait(&history->semaphore);
  } while (result < 0 && errno == EINTR);
  return result;
}

void history_add(SharedHistory *history, const char *line) {
  if (!history || !line || !*line)
    return;
  if (lock_history(history) < 0)
    return;
  size_t entry_index = history->next_entry++ % TERM_HISTORY_CAP;
  snprintf(history->entries[entry_index], TERM_LINE_MAX, "%s", line);
  if (history->entry_count < TERM_HISTORY_CAP)
    history->entry_count++;
  sem_post(&history->semaphore);
}
size_t history_snapshot(SharedHistory *history, char (*snapshot)[TERM_LINE_MAX],
                        size_t max_entries) {
  if (!history || !snapshot || !max_entries)
    return 0;
  if (lock_history(history) < 0)
    return 0;
  size_t entry_count =
      history->entry_count < max_entries ? history->entry_count : max_entries;
  /* Copy oldest-to-newest while locked so callers never see a torn entry. */
  size_t first_entry =
      (history->next_entry + TERM_HISTORY_CAP - history->entry_count) %
      TERM_HISTORY_CAP;
  if (history->entry_count > entry_count)
    first_entry =
        (first_entry + history->entry_count - entry_count) % TERM_HISTORY_CAP;
  for (size_t offset = 0; offset < entry_count; offset++) {
    size_t history_index = (first_entry + offset) % TERM_HISTORY_CAP;
    memcpy(snapshot[offset], history->entries[history_index], TERM_LINE_MAX);
  }
  sem_post(&history->semaphore);
  return entry_count;
}

/* Copy one history entry without allocating a full snapshot in the GUI. */
int history_get_recent(SharedHistory *history, size_t offset_from_newest,
                       char *buffer, size_t buffer_size) {
  if (!history || !buffer || !buffer_size || lock_history(history) < 0)
    return 0;
  if (offset_from_newest >= history->entry_count) {
    sem_post(&history->semaphore);
    return 0;
  }

  size_t newest =
      (history->next_entry + TERM_HISTORY_CAP - 1) % TERM_HISTORY_CAP;
  size_t entry_index =
      (newest + TERM_HISTORY_CAP - offset_from_newest) % TERM_HISTORY_CAP;
  snprintf(buffer, buffer_size, "%s", history->entries[entry_index]);
  sem_post(&history->semaphore);
  return 1;
}
