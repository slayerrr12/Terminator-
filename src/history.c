#include "terminator.h"
#include <errno.h>
#include <string.h>

/* Bounded history ring usable from heap memory or MAP_SHARED process memory. */
void history_init(SharedHistory *history, int process_shared) {
  memset(history, 0, sizeof(*history));
  (void)sem_init(&history->lock, process_shared ? 1 : 0, 1);
}

static int lock_history(SharedHistory *history) {
  /* sem_wait can be interrupted by a signal; retry without losing the lock. */
  int result;
  do {
    result = sem_wait(&history->lock);
  } while (result < 0 && errno == EINTR);
  return result;
}

void history_add(SharedHistory *history, const char *line) {
  if (!history || !line || !*line)
    return;
  if (lock_history(history) < 0)
    return;
  size_t entry_offset = history->next++ % TERM_HISTORY_CAP;
  strncpy(history->entries[entry_offset], line, TERM_LINE_MAX - 1);
  history->entries[entry_offset][TERM_LINE_MAX - 1] = '\0';
  if (history->count < TERM_HISTORY_CAP)
    history->count++;
  sem_post(&history->lock);
}
size_t history_snapshot(SharedHistory *history, char (*snapshot)[TERM_LINE_MAX],
                        size_t max_entries) {
  if (!history || !snapshot || !max_entries)
    return 0;
  if (lock_history(history) < 0)
    return 0;
  size_t entry_count =
      history->count < max_entries ? history->count : max_entries;
  /* Copy oldest-to-newest while locked so callers never see a torn entry. */
  size_t first_entry_index =
      (history->next + TERM_HISTORY_CAP - history->count) % TERM_HISTORY_CAP;
  if (history->count > entry_count)
    first_entry_index =
        (first_entry_index + history->count - entry_count) % TERM_HISTORY_CAP;
  for (size_t entry_offset = 0; entry_offset < entry_count; entry_offset++)
    memcpy(
        snapshot[entry_offset],
        history->entries[(first_entry_index + entry_offset) % TERM_HISTORY_CAP],
        TERM_LINE_MAX);
  sem_post(&history->lock);
  return entry_count;
}
