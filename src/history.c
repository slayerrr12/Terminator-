#include "terminator.h"
#include <string.h>

void history_init(SharedHistory *h, int process_shared) {
  memset(h, 0, sizeof(*h));
  (void)sem_init(&h->lock, process_shared ? 1 : 0, 1);
}
void history_add(SharedHistory *h, const char *line) {
  if (!h || !line || !*line)
    return;
  sem_wait(&h->lock);
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
  sem_wait(&h->lock);
  size_t n = h->count < cap ? h->count : cap;
  size_t start = (h->next + TERM_HISTORY_CAP - h->count) % TERM_HISTORY_CAP;
  if (h->count > n)
    start = (start + h->count - n) % TERM_HISTORY_CAP;
  for (size_t i = 0; i < n; i++)
    memcpy(out[i], h->entries[(start + i) % TERM_HISTORY_CAP], TERM_LINE_MAX);
  sem_post(&h->lock);
  return n;
}
