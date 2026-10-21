#include "terminator.h"
#include <errno.h>
#include <string.h>

/* Bounded history ring usable from heap memory or MAP_SHARED process memory. */
void history_init(SharedHistory *h, int process_shared) {
  memset(h, 0, sizeof(*h));
  (void)sem_init(&h->lock, process_shared ? 1 : 0, 1);
}

static int history_lock(SharedHistory *h) {
  /* sem_wait can be interrupted by a signal; retry without losing the lock. */
  int result;
  do {
    result = sem_wait(&h->lock);
  } while (result < 0 && errno == EINTR);
  return result;
}

void history_add(SharedHistory *h, const char *line) {
  if (!h || !line || !*line)
    return;
  if (history_lock(h) < 0)
    return;
  size_t i = h->next++ % TERM_HISTORY_CAP;
  strncpy(h->entries[i], line, TERM_LINE_MAX - 1);
  h->entries[i][TERM_LINE_MAX - 1] = '\0';
  if (h->count < TERM_HISTORY_CAP)
    h->count++;
  sem_post(&h->lock);
}
size_t history_snapshot(SharedHistory *h, char (*out)[TERM_LINE_MAX],
                        size_t cap) {
  if (!h || !out || !cap)
    return 0;
  if (history_lock(h) < 0)
    return 0;
  size_t n = h->count < cap ? h->count : cap;
  /* Copy oldest-to-newest while locked so callers never see a torn entry. */
  size_t start = (h->next + TERM_HISTORY_CAP - h->count) % TERM_HISTORY_CAP;
  if (h->count > n)
    start = (start + h->count - n) % TERM_HISTORY_CAP;
  for (size_t i = 0; i < n; i++)
    memcpy(out[i], h->entries[(start + i) % TERM_HISTORY_CAP], TERM_LINE_MAX);
  sem_post(&h->lock);
  return n;
}
