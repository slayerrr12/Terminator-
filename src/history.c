#include "terminator.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* The file stores one escaped command per physical line. */
static int write_all(int fd, const char *text, size_t length) {
  size_t written = 0;
  while (written < length) {
    ssize_t count = write(fd, text + written, length - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    written += (size_t)count;
  }
  return 0;
}

static int make_lock_path(const SharedHistory *history, char *path,
                          size_t path_size) {
  int length = snprintf(path, path_size, "%s.lock", history->file_path);
  if (length < 0 || (size_t)length >= path_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

/* The semaphore protects tabs in this app; flock also protects other launches. */
static int lock_history(SharedHistory *history, int *lock_fd) {
  while (sem_wait(&history->semaphore) < 0) {
    if (errno != EINTR)
      return -1;
  }

  char lock_path[TERM_PATH_BUFFER + 8];
  if (make_lock_path(history, lock_path, sizeof(lock_path)) < 0)
    goto fail_semaphore;

  *lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (*lock_fd < 0)
    goto fail_semaphore;
  if (flock(*lock_fd, LOCK_EX) < 0) {
    int saved_error = errno;
    close(*lock_fd);
    *lock_fd = -1;
    errno = saved_error;
    goto fail_semaphore;
  }
  return 0;

fail_semaphore:
  sem_post(&history->semaphore);
  return -1;
}

static void unlock_history(SharedHistory *history, int lock_fd) {
  (void)flock(lock_fd, LOCK_UN);
  close(lock_fd);
  sem_post(&history->semaphore);
}

static char *history_record_path(void) {
  const char *configured_path = getenv("TERMINATOR_HISTORY_FILE");
  if (configured_path && *configured_path)
    return strdup(configured_path);

  const char *home = getenv("HOME");
  if (!home || !*home) {
    errno = ENOENT;
    return NULL;
  }
  size_t size = strlen(home) + sizeof("/.terminator_history");
  char *path = malloc(size);
  if (path)
    snprintf(path, size, "%s/.terminator_history", home);
  return path;
}

static char *decode_record(const char *record) {
  size_t length = strlen(record);
  char *command = malloc(length + 1);
  if (!command)
    return NULL;

  size_t output = 0;
  for (size_t i = 0; i < length; i++) {
    if (record[i] == '\\' && i + 1 < length) {
      char escaped = record[++i];
      if (escaped == 'n')
        command[output++] = '\n';
      else if (escaped == 'r')
        command[output++] = '\r';
      else
        command[output++] = escaped;
    } else if (record[i] != '\n') {
      command[output++] = record[i];
    }
  }
  command[output] = '\0';
  return command;
}

static char *encode_record(const char *command) {
  size_t length = strlen(command);
  if (length >= TERM_LINE_MAX) {
    errno = E2BIG;
    return NULL;
  }
  char *record = malloc(length * 2 + 1);
  if (!record)
    return NULL;

  size_t output = 0;
  for (size_t i = 0; i < length; i++) {
    if (command[i] == '\\') {
      record[output++] = '\\';
      record[output++] = '\\';
    } else if (command[i] == '\n' || command[i] == '\r') {
      record[output++] = '\\';
      record[output++] = command[i] == '\n' ? 'n' : 'r';
    } else {
      record[output++] = command[i];
    }
  }
  record[output] = '\0';
  return record;
}

/* Remember only the newest offsets, so history size never grows in memory. */
static int collect_offsets(FILE *file, off_t *offsets, size_t capacity,
                           size_t *record_count) {
  char *line = NULL;
  size_t line_capacity = 0;
  size_t count = 0;
  ssize_t length;
  rewind(file);

  while (1) {
    off_t offset = ftello(file);
    if (offset < 0) {
      free(line);
      return -1;
    }
    length = getline(&line, &line_capacity, file);
    if (length < 0)
      break;
    if (capacity)
      offsets[count % capacity] = offset;
    count++;
  }
  free(line);
  if (ferror(file))
    return -1;
  *record_count = count;
  return 0;
}

/* Keep exactly the last 10,000 commands after an append. */
static int trim_history_file(const char *path) {
  FILE *source = fopen(path, "r");
  if (!source)
    return -1;

  off_t *offsets = malloc(TERM_HISTORY_CAP * sizeof(*offsets));
  if (!offsets) {
    fclose(source);
    return -1;
  }
  size_t count = 0;
  if (collect_offsets(source, offsets, TERM_HISTORY_CAP, &count) < 0) {
    free(offsets);
    fclose(source);
    return -1;
  }
  if (count <= TERM_HISTORY_CAP) {
    free(offsets);
    fclose(source);
    return 0;
  }

  size_t path_length = strlen(path);
  char *temporary_path = malloc(path_length + sizeof(".tmp.XXXXXX"));
  if (!temporary_path) {
    free(offsets);
    fclose(source);
    return -1;
  }
  snprintf(temporary_path, path_length + sizeof(".tmp.XXXXXX"),
           "%s.tmp.XXXXXX", path);
  int temporary_fd = mkstemp(temporary_path);
  if (temporary_fd < 0) {
    free(temporary_path);
    free(offsets);
    fclose(source);
    return -1;
  }
  (void)fchmod(temporary_fd, 0600);
  FILE *temporary = fdopen(temporary_fd, "w");
  if (!temporary) {
    close(temporary_fd);
    unlink(temporary_path);
    free(temporary_path);
    free(offsets);
    fclose(source);
    return -1;
  }

  char *line = NULL;
  size_t line_capacity = 0;
  int result = 0;
  size_t first = count % TERM_HISTORY_CAP;
  for (size_t i = 0; i < TERM_HISTORY_CAP; i++) {
    off_t offset = offsets[(first + i) % TERM_HISTORY_CAP];
    if (fseeko(source, offset, SEEK_SET) < 0 ||
        getline(&line, &line_capacity, source) < 0 ||
        fputs(line, temporary) == EOF) {
      result = -1;
      break;
    }
  }
  free(line);
  free(offsets);
  if (fclose(source) < 0)
    result = -1;
  if (fflush(temporary) < 0)
    result = -1;
  if (fclose(temporary) < 0)
    result = -1;
  if (!result && rename(temporary_path, path) < 0)
    result = -1;
  if (result)
    unlink(temporary_path);
  free(temporary_path);
  return result;
}

int history_init(SharedHistory *history, int process_shared) {
  if (!history) {
    errno = EINVAL;
    return -1;
  }
  memset(history, 0, sizeof(*history));
  if (sem_init(&history->semaphore, process_shared ? 1 : 0, 1) < 0)
    return -1;

  char *path = history_record_path();
  if (!path) {
    int saved_error = errno;
    sem_destroy(&history->semaphore);
    errno = saved_error;
    return -1;
  }
  if (strlen(path) >= sizeof(history->file_path) - 6) {
    free(path);
    sem_destroy(&history->semaphore);
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(history->file_path, path);
  free(path);

  int lock_fd = -1;
  if (lock_history(history, &lock_fd) < 0) {
    sem_destroy(&history->semaphore);
    return -1;
  }
  int file_fd = open(history->file_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  int result = file_fd < 0 ? -1 : 0;
  if (file_fd >= 0)
    close(file_fd);
  if (!result)
    result = trim_history_file(history->file_path);
  if (!result) {
    FILE *file = fopen(history->file_path, "r");
    struct stat info;
    if (!file || stat(history->file_path, &info) < 0) {
      result = -1;
    } else {
      result = collect_offsets(file, NULL, 0, &history->entry_count);
      history->file_size = info.st_size;
    }
    if (file)
      fclose(file);
  }
  unlock_history(history, lock_fd);
  if (result < 0)
    sem_destroy(&history->semaphore);
  return result;
}

int history_add(SharedHistory *history, const char *command) {
  if (!history || !command || !*command)
    return 0;
  char *record = encode_record(command);
  if (!record)
    return -1;

  int lock_fd = -1;
  if (lock_history(history, &lock_fd) < 0) {
    free(record);
    return -1;
  }
  int file_fd = open(history->file_path,
                     O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  int result = file_fd < 0 ? -1 : 0;
  if (file_fd >= 0) {
    struct stat info;
    if (fstat(file_fd, &info) < 0) {
      result = -1;
    } else if (info.st_size != history->file_size) {
      FILE *current_file = fopen(history->file_path, "r");
      if (!current_file ||
          collect_offsets(current_file, NULL, 0, &history->entry_count) < 0)
        result = -1;
      if (current_file)
        fclose(current_file);
      history->file_size = info.st_size;
    }
    if (!result) {
      result = write_all(file_fd, record, strlen(record));
      if (!result)
        result = write_all(file_fd, "\n", 1);
      if (!result)
        history->entry_count++;
    }
    int saved_error = errno;
    if (!result && fstat(file_fd, &info) == 0)
      history->file_size = info.st_size;
    else if (!result)
      result = -1;
    close(file_fd);
    errno = saved_error;
  }
  free(record);
  if (!result && history->entry_count > TERM_HISTORY_CAP) {
    result = trim_history_file(history->file_path);
    if (!result) {
      FILE *file = fopen(history->file_path, "r");
      struct stat info;
      if (!file || stat(history->file_path, &info) < 0) {
        result = -1;
      } else {
        result = collect_offsets(file, NULL, 0, &history->entry_count);
        history->file_size = info.st_size;
      }
      if (file)
        fclose(file);
    }
  }
  int saved_error = errno;
  unlock_history(history, lock_fd);
  errno = saved_error;
  return result;
}

static int read_record_at(FILE *file, off_t offset, char **command) {
  if (fseeko(file, offset, SEEK_SET) < 0)
    return -1;
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length = getline(&line, &capacity, file);
  if (length < 0) {
    free(line);
    return -1;
  }
  *command = decode_record(line);
  free(line);
  return *command ? 0 : -1;
}

size_t history_snapshot(SharedHistory *history,
                        char (*snapshot)[TERM_LINE_MAX], size_t max_entries) {
  if (!history || !snapshot || !max_entries)
    return 0;
  if (max_entries > TERM_HISTORY_CAP)
    max_entries = TERM_HISTORY_CAP;
  int lock_fd = -1;
  if (lock_history(history, &lock_fd) < 0)
    return 0;

  off_t *offsets = malloc(max_entries * sizeof(*offsets));
  FILE *file = fopen(history->file_path, "r");
  size_t count = 0;
  int result = offsets && file ? collect_offsets(file, offsets, max_entries,
                                                 &count)
                               : -1;
  size_t selected = count < max_entries ? count : max_entries;
  size_t first = count < max_entries ? 0 : count % max_entries;
  for (size_t i = 0; !result && i < selected; i++) {
    char *command = NULL;
    if (read_record_at(file, offsets[(first + i) % max_entries], &command) < 0) {
      result = -1;
      break;
    }
    snprintf(snapshot[i], TERM_LINE_MAX, "%s", command);
    free(command);
  }
  free(offsets);
  if (file)
    fclose(file);
  unlock_history(history, lock_fd);
  return result ? 0 : selected;
}

int history_get_recent(SharedHistory *history, size_t offset_from_newest,
                       char *buffer, size_t buffer_size) {
  if (!history || !buffer || !buffer_size ||
      offset_from_newest >= TERM_HISTORY_CAP)
    return 0;
  int lock_fd = -1;
  if (lock_history(history, &lock_fd) < 0)
    return 0;

  off_t *offsets = malloc(TERM_HISTORY_CAP * sizeof(*offsets));
  FILE *file = fopen(history->file_path, "r");
  size_t count = 0;
  int result = offsets && file ? collect_offsets(file, offsets,
                                                TERM_HISTORY_CAP, &count)
                               : -1;
  if (!result && offset_from_newest < count) {
    size_t index = (count - 1 - offset_from_newest) % TERM_HISTORY_CAP;
    char *command = NULL;
    result = read_record_at(file, offsets[index], &command);
    if (!result) {
      snprintf(buffer, buffer_size, "%s", command);
      free(command);
      result = 1;
    }
  } else if (!result) {
    result = 0;
  }
  free(offsets);
  if (file)
    fclose(file);
  unlock_history(history, lock_fd);
  return result > 0;
}

/* A suffix automaton finds the longest shared substring in linear time. */
typedef struct {
  int next[256];
  int link;
  size_t length;
} SearchState;

static int build_search_states(const char *term, SearchState *states,
                               size_t *state_count) {
  size_t term_length = strlen(term);
  *state_count = 1;
  states[0].link = -1;
  states[0].length = 0;
  for (size_t byte = 0; byte < 256; byte++)
    states[0].next[byte] = -1;

  int last = 0;
  for (size_t i = 0; i < term_length; i++) {
    unsigned char value = (unsigned char)term[i];
    int current = (int)(*state_count)++;
    states[current].length = states[last].length + 1;
    for (size_t byte = 0; byte < 256; byte++)
      states[current].next[byte] = -1;

    int previous = last;
    while (previous >= 0 && states[previous].next[value] < 0) {
      states[previous].next[value] = current;
      previous = states[previous].link;
    }
    if (previous < 0) {
      states[current].link = 0;
    } else {
      int next_state = states[previous].next[value];
      if (states[previous].length + 1 == states[next_state].length) {
        states[current].link = next_state;
      } else {
        int clone = (int)(*state_count)++;
        states[clone] = states[next_state];
        states[clone].length = states[previous].length + 1;
        while (previous >= 0 && states[previous].next[value] == next_state) {
          states[previous].next[value] = clone;
          previous = states[previous].link;
        }
        states[next_state].link = clone;
        states[current].link = clone;
      }
    }
    last = current;
  }
  return 0;
}

static size_t longest_shared_substring(const SearchState *states,
                                      const char *command) {
  int state = 0;
  size_t matched = 0;
  size_t best = 0;
  for (const unsigned char *byte = (const unsigned char *)command; *byte;
       byte++) {
    while (state && states[state].next[*byte] < 0) {
      state = states[state].link;
      matched = states[state].length;
    }
    if (states[state].next[*byte] >= 0) {
      state = states[state].next[*byte];
      matched++;
      if (matched > best)
        best = matched;
    } else {
      state = 0;
      matched = 0;
    }
  }
  return best;
}

int history_search(SharedHistory *history, const char *term,
                   HistorySearchCallback callback, void *context,
                   size_t *best_match_length) {
  if (best_match_length)
    *best_match_length = 0;
  if (!history || !term || !*term)
    return 0;
  size_t term_length = strlen(term);
  if (term_length >= TERM_LINE_MAX)
    return 0;

  int lock_fd = -1;
  if (lock_history(history, &lock_fd) < 0)
    return -1;
  FILE *file = fopen(history->file_path, "r");
  if (!file) {
    unlock_history(history, lock_fd);
    return -1;
  }

  char *line = NULL;
  size_t capacity = 0;
  int matches = 0;
  ssize_t line_length;
  while ((line_length = getline(&line, &capacity, file)) >= 0) {
    (void)line_length;
    char *command = decode_record(line);
    if (!command) {
      matches = -1;
      break;
    }
    if (strstr(command, term)) {
      if (callback)
        callback(command, context);
      matches++;
    }
    free(command);
  }
  if (ferror(file) && matches >= 0)
    matches = -1;

  size_t best = 0;
  if (matches == 0) {
    size_t maximum_states = 2 * term_length + 1;
    SearchState *states = calloc(maximum_states, sizeof(*states));
    if (!states || build_search_states(term, states, &maximum_states) < 0) {
      free(states);
      matches = -1;
    } else {
      rewind(file);
      while ((line_length = getline(&line, &capacity, file)) >= 0) {
        char *command = decode_record(line);
        if (!command) {
          matches = -1;
          break;
        }
        size_t shared = longest_shared_substring(states, command);
        if (shared > best)
          best = shared;
        free(command);
      }
      if (ferror(file) && matches >= 0)
        matches = -1;
      if (matches >= 0 && best > 2) {
        rewind(file);
        while ((line_length = getline(&line, &capacity, file)) >= 0) {
          char *command = decode_record(line);
          if (!command) {
            matches = -1;
            break;
          }
          if (longest_shared_substring(states, command) == best) {
            if (callback)
              callback(command, context);
            matches++;
          }
          free(command);
        }
        if (ferror(file) && matches >= 0)
          matches = -1;
      }
      free(states);
    }
  }
  free(line);
  fclose(file);
  if (best_match_length)
    *best_match_length = best;
  unlock_history(history, lock_fd);
  return matches;
}
