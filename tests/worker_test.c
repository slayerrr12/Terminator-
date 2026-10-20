#include "terminator.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int in, out, status;
  SharedHistory *history;
  SharedForeground *foreground;
  pthread_t thread;
  pid_t pid;
  int threaded;
} Harness;
typedef struct {
  int in, out, status;
  SharedHistory *history;
  SharedForeground *foreground;
  char cwd[4096];
} WorkerArgs;

static void *thread_runner(void *arg) {
  WorkerArgs *a = arg;
  worker_run(a->in, a->out, a->status, a->history, a->foreground, 0, a->cwd);
  free(a);
  return NULL;
}

static Harness start_shared_worker(int process_mode, SharedHistory *history,
                                   const char *cwd) {
  Harness h = {0};
  int in[2], out[2], status[2];
  assert(pipe(in) == 0 && pipe(out) == 0 && pipe(status) == 0);
  h.history = history;
  h.foreground = mmap(NULL, sizeof(*h.foreground), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(h.foreground != MAP_FAILED);
  atomic_init(&h.foreground->pgid, 0);
  h.in = in[1];
  h.out = out[0];
  h.status = status[0];
  h.threaded = !process_mode;
  if (process_mode) {
    h.pid = fork();
    assert(h.pid >= 0);
    if (h.pid == 0) {
      close(in[1]);
      close(out[0]);
      close(status[0]);
      worker_run(in[0], out[1], status[1], h.history, h.foreground, 0, cwd);
      _exit(0);
    }
    close(in[0]);
    close(out[1]);
    close(status[1]);
  } else {
    WorkerArgs *a = malloc(sizeof(*a));
    assert(a);
    *a = (WorkerArgs){.in = in[0],
                      .out = out[1],
                      .status = status[1],
                      .history = h.history,
                      .foreground = h.foreground};
    snprintf(a->cwd, sizeof(a->cwd), "%s", cwd);
    assert(pthread_create(&h.thread, NULL, thread_runner, a) == 0);
  }
  return h;
}

static Harness start_worker(int process_mode) {
  SharedHistory *history = mmap(NULL, sizeof(*history), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(history != MAP_FAILED);
  history_init(history, 1);
  return start_shared_worker(process_mode, history, "/tmp");
}

static void run_case(int process_mode) {
  Harness h = start_worker(process_mode);
  char dir[] = "/tmp/terminator-test-XXXXXX";
  assert(mkdtemp(dir));
  char file[512], absent[520], history_file[512], input_file[512];
  snprintf(file, sizeof(file), "%s/out.txt", dir);
  snprintf(absent, sizeof(absent), "%s/missing.txt", dir);
  snprintf(history_file, sizeof(history_file), "%s/history.txt", dir);
  snprintf(input_file, sizeof(input_file), "%s/input.txt", dir);
  char script[512];
  snprintf(script, sizeof(script), "%s/source.txt", dir);
  FILE *sf = fopen(script, "w");
  assert(sf);
  fputs("echo sourced-command\n", sf);
  fclose(sf);
  char commands[8192];
  snprintf(commands, sizeof(commands),
           "printf abc | wc -c\n"
           "echo '$HOME'\n"
           "echo \"$HOME\"\n"
           "echo \"double: \\q, expanded: $HOME\"\n"
           "echo \"$(printf nested)\"\n"
           "echo \"$(basename $(pwd))\"\n"
           "echo \"$(printf 'outer\ninner')\"\n"
           "echo `printf backtick`\n"
           "cd /tmp\n"
           "pwd\n"
           "cd %s\n"
           "printf a > one.txt\n"
           "printf b > two.txt\n"
           "echo *.txt\n"
           "echo '*.txt'\n"
           "cd /tmp\n"
           "printf 'z\\na\\n' | sort | head -1\n"
           "echo 'literal $HOME | >'\n"
           "echo \"first\nsecond\"\n"
           "echo continued\\\n value\n"
           "printf hello > %s\n"
           "printf ' world' >> %s\n"
           "cat < %s | wc -c\n"
           "cat < /terminator-file-that-does-not-exist > %s\n"
           "cd %s\n"
           "printf shared-input > %s\n"
           "cat < %s > combined.txt\n"
           "cat < %s > background-one.txt &\n"
           "cat < %s > background-two.txt &\n"
           "fg %%1\n"
           "fg %%2\n"
           "echo $?\n"
           "notacommand\n"
           "source %s\n"
           "cd %s\n"
           "history > history.txt\n"
           "cd /tmp\n"
           "history\n"
           "exit\n",
           dir, file, file, file, absent, dir, input_file, input_file,
           input_file, input_file, script, dir);
  size_t len = strlen(commands);
  assert(write(h.in, commands, len) == (ssize_t)len);
  close(h.in);

  char *result = calloc(1, 65536);
  assert(result);
  size_t used = 0;
  for (;;) {
    ssize_t n = read(h.out, result + used, 65535 - used);
    if (n == 0)
      break;
    if (n < 0 && errno == EINTR)
      continue;
    assert(n > 0);
    used += (size_t)n;
    assert(used < 65535);
  }
  close(h.out);
  close(h.status);
  if (h.threaded)
    assert(pthread_join(h.thread, NULL) == 0);
  else {
    int status;
    assert(waitpid(h.pid, &status, 0) == h.pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
  }

  assert(strstr(result, "3\n"));
  assert(strstr(result, "$HOME\n"));
  assert(strstr(result, getenv("HOME")));
  char quoted[1024];
  snprintf(quoted, sizeof(quoted), "double: \\q, expanded: %s\n",
           getenv("HOME"));
  assert(strstr(result, quoted));
  assert(strstr(result, "nested\n"));
  assert(strstr(result, "tmp\n"));
  assert(strstr(result, "outer\ninner\n"));
  assert(strstr(result, "backtick\n"));
  assert(strstr(result, "/tmp\n"));
  assert(strstr(result, "a\n"));
  assert(strstr(result, "literal $HOME | >\n"));
  assert(strstr(result, "one.txt source.txt two.txt\n"));
  assert(strstr(result, "*.txt\n"));
  assert(strstr(result, "first\nsecond\n"));
  assert(strstr(result, "continued value\n"));
  assert(strstr(result, "11\n"));
  assert(strstr(result, "No such file or directory"));
  assert(strstr(result, "1\n"));
  assert(strstr(result, "notacommand: command not found"));
  assert(strstr(result, "sourced-command\n"));
  assert(strstr(result, "history\n"));
  FILE *hf = fopen(history_file, "r");
  assert(hf);
  fclose(hf);
  FILE *f = fopen(file, "r");
  assert(f);
  char contents[16] = {0};
  assert(fread(contents, 1, sizeof(contents) - 1, f) == 11);
  fclose(f);
  assert(!strcmp(contents, "hello world"));
  assert(access(absent, F_OK) != 0);
  char path[520];
  const char *copied_files[] = {"combined.txt", "background-one.txt",
                                "background-two.txt"};
  for (size_t i = 0; i < sizeof(copied_files) / sizeof(copied_files[0]); ++i) {
    char copied[512] = {0};
    snprintf(path, sizeof(path), "%s/%s", dir, copied_files[i]);
    FILE *copy = fopen(path, "r");
    assert(copy);
    assert(fread(copied, 1, sizeof(copied) - 1, copy) == 12);
    fclose(copy);
    assert(!strcmp(copied, "shared-input"));
    unlink(path);
  }
  unlink(input_file);
  unlink(file);
  unlink(script);
  unlink(history_file);
  snprintf(path, sizeof(path), "%s/one.txt", dir);
  unlink(path);
  snprintf(path, sizeof(path), "%s/two.txt", dir);
  unlink(path);
  rmdir(dir);
  free(result);
  sem_destroy(&h.history->lock);
  munmap(h.history, sizeof(*h.history));
  munmap(h.foreground, sizeof(*h.foreground));
}

static void test_two_tabs(int process_mode) {
  SharedHistory *history = mmap(NULL, sizeof(*history), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(history != MAP_FAILED);
  history_init(history, 1);
  Harness one = start_shared_worker(process_mode, history, "/tmp");
  Harness two = start_shared_worker(process_mode, history, "/tmp");
  const char first[] = "cd /\necho from-one\n";
  assert(write(one.in, first, sizeof(first) - 1) ==
         (ssize_t)(sizeof(first) - 1));
  WorkerStatus status;
  assert(read(one.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(!strcmp(status.cwd, "/"));
  assert(read(one.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(write(one.in, "exit\n", 5) == 5);
  close(one.in);
  const char second[] = "pwd\nhistory\nexit\n";
  assert(write(two.in, second, sizeof(second) - 1) ==
         (ssize_t)(sizeof(second) - 1));
  close(two.in);
  char output[8192] = {0};
  size_t used = 0;
  for (;;) {
    ssize_t n = read(two.out, output + used, sizeof(output) - used - 1);
    if (!n)
      break;
    assert(n > 0);
    used += (size_t)n;
  }
  close(two.out);
  close(two.status);
  char discard[4096];
  while (read(one.out, discard, sizeof(discard)) > 0) {
  }
  close(one.out);
  close(one.status);
  if (process_mode) {
    int a, b;
    assert(waitpid(one.pid, &a, 0) == one.pid && WIFEXITED(a));
    assert(waitpid(two.pid, &b, 0) == two.pid && WIFEXITED(b));
  } else {
    assert(pthread_join(one.thread, NULL) == 0);
    assert(pthread_join(two.thread, NULL) == 0);
  }
  assert(strstr(output, "/tmp\n"));
  assert(strstr(output, "cd /\n"));
  assert(strstr(output, "echo from-one\n"));
  sem_destroy(&history->lock);
  munmap(history, sizeof(*history));
  munmap(one.foreground, sizeof(*one.foreground));
  munmap(two.foreground, sizeof(*two.foreground));
}

static void test_clear_builtin(int process_mode) {
  Harness h = start_worker(process_mode);
  assert(write(h.in, "clear\n", 6) == 6);
  WorkerStatus status;
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 0);
  assert(status.clear_screen == 1);

  assert(write(h.in, "exit\n", 5) == 5);
  close(h.in);
  char output[128] = {0};
  ssize_t n = read(h.out, output, sizeof(output) - 1);
  assert(n == 0);
  close(h.out);
  close(h.status);
  if (process_mode) {
    int child_status;
    assert(waitpid(h.pid, &child_status, 0) == h.pid &&
           WIFEXITED(child_status));
  } else {
    assert(pthread_join(h.thread, NULL) == 0);
  }
  sem_destroy(&h.history->lock);
  munmap(h.history, sizeof(*h.history));
  munmap(h.foreground, sizeof(*h.foreground));
}

static void test_interrupt_is_tab_scoped(int process_mode) {
  SharedHistory *history = mmap(NULL, sizeof(*history), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  assert(history != MAP_FAILED);
  history_init(history, 1);
  Harness h = start_shared_worker(process_mode, history, "/tmp");
  Harness other = start_shared_worker(process_mode, history, "/tmp");
  assert(write(h.in, "sleep 10\n", 9) == 9);
  assert(write(other.in, "sleep 10\n", 9) == 9);
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
  int pgid = 0, other_pgid = 0;
  for (int i = 0; i < 200 && (!pgid || !other_pgid); ++i) {
    pgid = atomic_load(&h.foreground->pgid);
    other_pgid = atomic_load(&other.foreground->pgid);
    if (!pgid || !other_pgid)
      nanosleep(&delay, NULL);
  }
  assert(pgid > 0 && other_pgid > 0 && pgid != other_pgid);
  assert(kill(-pgid, SIGINT) == 0);
  WorkerStatus status, other_status;
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 128 + SIGINT);
  assert(atomic_load(&other.foreground->pgid) == other_pgid &&
         kill(-other_pgid, 0) == 0);
  assert(kill(-other_pgid, SIGINT) == 0);
  assert(read(other.status, &other_status, sizeof(other_status)) ==
         (ssize_t)sizeof(other_status));
  assert(other_status.status == 128 + SIGINT);
  assert(write(h.in, "echo survived\nexit\n", 19) == 19);
  assert(write(other.in, "echo other-survived\nexit\n", 25) == 25);
  close(h.in);
  close(other.in);
  char output[2048] = {0}, other_output[2048] = {0};
  size_t used = 0, other_used = 0;
  for (;;) {
    ssize_t n = read(h.out, output + used, sizeof(output) - used - 1);
    if (!n)
      break;
    assert(n > 0);
    used += (size_t)n;
  }
  for (;;) {
    ssize_t n = read(other.out, other_output + other_used,
                     sizeof(other_output) - other_used - 1);
    if (!n)
      break;
    assert(n > 0);
    other_used += (size_t)n;
  }
  close(h.out);
  close(h.status);
  close(other.out);
  close(other.status);
  if (process_mode) {
    int st, other_st;
    assert(waitpid(h.pid, &st, 0) == h.pid && WIFEXITED(st));
    assert(waitpid(other.pid, &other_st, 0) == other.pid &&
           WIFEXITED(other_st));
  } else {
    assert(pthread_join(h.thread, NULL) == 0);
    assert(pthread_join(other.thread, NULL) == 0);
  }
  assert(strstr(output, "survived\n"));
  assert(strstr(other_output, "other-survived\n"));
  sem_destroy(&history->lock);
  munmap(history, sizeof(*history));
  munmap(h.foreground, sizeof(*h.foreground));
  munmap(other.foreground, sizeof(*other.foreground));
}

static int wait_for_foreground(Harness *h) {
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int i = 0; i < 200; ++i) {
    int pgid = atomic_load(&h->foreground->pgid);
    if (pgid > 0)
      return pgid;
    nanosleep(&delay, NULL);
  }
  return 0;
}

static void test_job_control(int process_mode) {
  Harness h = start_worker(process_mode);
  assert(write(h.in, "sleep 10\n", 9) == 9);
  int pgid = wait_for_foreground(&h);
  assert(pgid > 0 && kill(-pgid, SIGTSTP) == 0);
  WorkerStatus status;
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 128 + SIGTSTP);

  const char control[] = "jobs\nbg %1\n";
  assert(write(h.in, control, sizeof(control) - 1) ==
         (ssize_t)(sizeof(control) - 1));
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 0);
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 0);

  assert(write(h.in, "fg %1\n", 6) == 6);
  pgid = wait_for_foreground(&h);
  assert(pgid > 0 && kill(-pgid, SIGINT) == 0);
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 128 + SIGINT);
  assert(write(h.in, "exit\n", 5) == 5);
  close(h.in);
  char output[4096] = {0};
  size_t used = 0;
  for (;;) {
    ssize_t n = read(h.out, output + used, sizeof(output) - used - 1);
    if (!n)
      break;
    assert(n > 0);
    used += (size_t)n;
  }
  close(h.out);
  close(h.status);
  if (process_mode) {
    int child_status;
    assert(waitpid(h.pid, &child_status, 0) == h.pid &&
           WIFEXITED(child_status));
  } else {
    assert(pthread_join(h.thread, NULL) == 0);
  }
  assert(strstr(output, "Stopped"));
  assert(strstr(output, "sleep 10"));
  assert(strstr(output, "&"));
  sem_destroy(&h.history->lock);
  munmap(h.history, sizeof(*h.history));
  munmap(h.foreground, sizeof(*h.foreground));
}

static void test_multiwatch_interrupt(int process_mode) {
  Harness h = start_worker(process_mode);
  assert(fcntl(h.out, F_SETFL, O_NONBLOCK) == 0);
  const char command[] = "multiWatch [\"echo watched\", \"echo second\"]\n";
  assert(write(h.in, command, sizeof(command) - 1) ==
         (ssize_t)(sizeof(command) - 1));
  char output[4096] = {0};
  size_t used = 0;
  int pgid = 0;
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int i = 0; i < 300 && (!strstr(output, "watched") ||
                              !strstr(output, "\"echo watched\"") ||
                              !strstr(output, "\"echo second\""));
       ++i) {
    ssize_t n = read(h.out, output + used, sizeof(output) - used - 1);
    if (n > 0)
      used += (size_t)n;
    pgid = atomic_load(&h.foreground->pgid);
    nanosleep(&delay, NULL);
  }
  assert(strstr(output, "watched"));
  assert(strstr(output, "\"echo second\""));
  assert(pgid > 0 && kill(-pgid, SIGINT) == 0);
  WorkerStatus status;
  assert(read(h.status, &status, sizeof(status)) == (ssize_t)sizeof(status));
  assert(status.status == 128 + SIGINT);
  assert(write(h.in, "exit\n", 5) == 5);
  close(h.in);
  for (;;) {
    struct pollfd ready = {.fd = h.out, .events = POLLIN | POLLHUP};
    int poll_result = poll(&ready, 1, 1000);
    assert(poll_result >= 0);
    if (!poll_result)
      continue;
    ssize_t n = read(h.out, output + used, sizeof(output) - used - 1);
    if (!n)
      break;
    if (n < 0 && errno == EAGAIN)
      continue;
    if (n < 0 && errno == EINTR)
      continue;
    assert(n > 0);
    used += (size_t)n;
  }
  close(h.out);
  close(h.status);
  if (process_mode) {
    int child_status;
    assert(waitpid(h.pid, &child_status, 0) == h.pid &&
           WIFEXITED(child_status));
  } else {
    assert(pthread_join(h.thread, NULL) == 0);
  }
  assert(strstr(output, "\"echo watched\""));
  assert(strstr(output, "\"echo second\""));
  sem_destroy(&h.history->lock);
  munmap(h.history, sizeof(*h.history));
  munmap(h.foreground, sizeof(*h.foreground));
}

int main(void) {
  run_case(0);
  run_case(1);
  test_clear_builtin(0);
  test_clear_builtin(1);
  test_two_tabs(0);
  test_two_tabs(1);
  test_interrupt_is_tab_scoped(0);
  test_interrupt_is_tab_scoped(1);
  test_job_control(0);
  test_job_control(1);
  test_multiwatch_interrupt(0);
  test_multiwatch_interrupt(1);
  puts("worker integration tests passed (thread and process modes)");
  return 0;
}
