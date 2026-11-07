#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int make_pipe(int fd[2]) {
  int raw[2];
  if (pipe(raw) != 0) return 0;
  for (int i = 0; i < 2; ++i) {
    fd[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, 3);
    if (fd[i] < 0) {
      close(raw[0]); close(raw[1]);
      if (i == 1) { close(fd[0]); fd[0] = -1; }
      return 0;
    }
  }
  close(raw[0]); close(raw[1]);
  if (fcntl(fd[0], F_SETFL, O_NONBLOCK) != 0) {
    close(fd[0]); close(fd[1]); fd[0] = fd[1] = -1;
    return 0;
  }
  return 1;
}

// Preserve UTF-8 and replace invalid bytes/NULs, including partial sequences
// at the capture limit. This keeps JSON output usable for arbitrary commands.
static int sanitize(String *str) {
  char *text = malloc(str->length * 3 + 1);
  if (text == NULL) return 0;
  size_t at = 0;
  for (size_t i = 0; i < str->length;) {
    unsigned char c = (unsigned char)str->data[i];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n <= str->length - i && tool_utf8((const unsigned char *)str->data + i, n)) {
      memcpy(text + at, str->data + i, n); at += n; i += n;
    } else {
      memcpy(text + at, "\xef\xbf\xbd", 3); at += 3; ++i;
    }
  }
  text[at] = '\0';
  free(str->data);
  str->data = text; str->length = at; str->capacity = str->length;
  return 1;
}

void tool_process_destroy(ToolProcess *result) {
  string_destroy(&result->out);
  string_destroy(&result->err);
}

AgentStatus tool_process(char *const argv[], const char *cwd, int timeout_ms,
                         size_t limit, ToolContext *ctx, ToolProcess *result,
                         AgentError *error) {
  *result = (ToolProcess){.exit_code = -1};
  int pipes[2][2] = {{-1, -1}, {-1, -1}};
  pid_t child = -1;
  int reaped = 0, wait_status = 0;
  AgentStatus status = AGENT_ERR_IO;
  long long start = milliseconds();
  if (start < 0 || !make_pipe(pipes[0]) || !make_pipe(pipes[1])) goto done;
  child = fork();
  if (child < 0) goto done;
  if (child == 0) {
    close(pipes[0][0]); close(pipes[1][0]);
    if (dup2(pipes[0][1], STDOUT_FILENO) < 0 || dup2(pipes[1][1], STDERR_FILENO) < 0)
      _exit(126);
    close(pipes[0][1]); close(pipes[1][1]);
    int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(126);
    if (input != STDIN_FILENO) close(input);
    if (setpgid(0, 0) != 0) _exit(126);
    if (cwd != NULL && chdir(cwd) != 0) {
      const char message[] = "Could not change working directory\n";
      (void)write(STDERR_FILENO, message, sizeof(message) - 1);
      _exit(126);
    }
    execvp(argv[0], argv);
    const char message[] = "Could not execute program (check executable and PATH)\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }
  (void)setpgid(child, child);
  for (int i = 0; i < 2; ++i) { close(pipes[i][1]); pipes[i][1] = -1; }
  status = AGENT_OK;
  long long deadline = start + timeout_ms;
  for (;;) {
    if (!reaped) {
      pid_t waited = waitpid(child, &wait_status, WNOHANG);
      if (waited == child) reaped = 1;
      else if (waited < 0 && errno != EINTR) { status = AGENT_ERR_IO; goto done; }
    }
    if (reaped && pipes[0][0] < 0 && pipes[1][0] < 0) break;
    long long now = milliseconds();
    if (now < 0) { status = AGENT_ERR_IO; goto done; }
    if (now >= deadline && !result->timed_out) {
      result->timed_out = 1;
      (void)kill(-child, SIGKILL);
      if (!reaped) (void)kill(child, SIGKILL);
    }
    if (result->timed_out && now >= deadline + 200) break;
    /* Poll the host's cancel hook here as well, so interrupting a long command
     * kills the whole process group instead of waiting for the timeout. */
    if (ctx != NULL && ctx->is_cancelled != NULL &&
        ctx->is_cancelled(ctx->userdata)) {
      status = AGENT_ERR_CANCELLED;
      goto done;
    }
    struct pollfd fds[2] = {
        {.fd = pipes[0][0], .events = POLLIN},
        {.fd = pipes[1][0], .events = POLLIN}};
    int delay = result->timed_out ? 20 : (deadline - now > 20 ? 20 : (int)(deadline - now));
    int ready = poll(fds, 2, delay);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) { status = AGENT_ERR_IO; goto done; }
    for (int i = 0; i < 2; ++i) {
      if (!fds[i].revents) continue;
      // Bound each drain so continuous output cannot starve the deadline.
      for (int j = 0; j < 16; ++j) {
        char buffer[4096];
        ssize_t n = read(pipes[i][0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0) { status = AGENT_ERR_IO; goto done; }
        if (n == 0) { close(pipes[i][0]); pipes[i][0] = -1; break; }
        size_t available = limit - result->out.length - result->err.length;
        size_t keep = (size_t)n < available ? (size_t)n : available;
        String *stream = i == 0 ? &result->out : &result->err;
        if (keep < (size_t)n) result->truncated = 1;
        if (keep == 0) continue;
        if (string_reserve(stream, stream->length + keep) != AGENT_OK) {
          status = AGENT_ERR_OUT_OF_MEMORY; goto done;
        }
        memcpy(stream->data + stream->length, buffer, keep);
        stream->length += keep;
        stream->data[stream->length] = '\0';
      }
    }
  }
done:
  if (child > 0 && !reaped) {
    (void)kill(-child, SIGKILL);
    (void)kill(child, SIGKILL);
    pid_t waited;
    do { waited = waitpid(child, &wait_status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child) status = AGENT_ERR_IO;
  } else if (child > 0 && status != AGENT_OK) {
    (void)kill(-child, SIGKILL);
  }
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) if (pipes[i][j] >= 0) close(pipes[i][j]);
  if (status == AGENT_OK) {
    if (WIFEXITED(wait_status)) result->exit_code = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) {
      result->signal = WTERMSIG(wait_status);
      result->exit_code = 128 + result->signal;
    }
    if (!sanitize(&result->out) || !sanitize(&result->err)) status = AGENT_ERR_OUT_OF_MEMORY;
  }
  if (status != AGENT_OK) {
    tool_process_destroy(result);
    return tool_fail(error, status,
                     status == AGENT_ERR_CANCELLED
                         ? "Tool execution cancelled"
                         : "Could not execute or capture process");
  }
  return AGENT_OK;
}

cJSON *tool_process_json(const ToolProcess *result) {
  cJSON *json = cJSON_CreateObject();
  if (json == NULL ||
      !cJSON_AddStringToObject(json, "stdout", string_c_str(&result->out)) ||
      !cJSON_AddStringToObject(json, "stderr", string_c_str(&result->err)) ||
      !cJSON_AddNumberToObject(json, "exit_code", result->exit_code) ||
      !cJSON_AddNumberToObject(json, "signal", result->signal) ||
      !cJSON_AddBoolToObject(json, "timed_out", result->timed_out) ||
      !cJSON_AddBoolToObject(json, "truncated", result->truncated)) {
    cJSON_Delete(json); return NULL;
  }
  return json;
}
