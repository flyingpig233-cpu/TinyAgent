#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Preserve UTF-8 and replace invalid bytes/NULs, including partial sequences
// at the capture limit. This keeps JSON output usable for arbitrary commands.
static int sanitize(String *str) {
  char *text = malloc(str->length * 3 + 1);
  if (text == NULL)
    return 0;
  size_t at = 0;
  for (size_t i = 0; i < str->length;) {
    unsigned char c = (unsigned char)str->data[i];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (n <= str->length - i &&
        tool_utf8((const unsigned char *)str->data + i, n)) {
      memcpy(text + at, str->data + i, n);
      at += n;
      i += n;
    } else {
      memcpy(text + at, "\xef\xbf\xbd", 3);
      at += 3;
      ++i;
    }
  }
  text[at] = '\0';
  free(str->data);
  str->data = text;
  str->length = at;
  str->capacity = str->length;
  return 1;
}

void tool_process_destroy(ToolProcess *result) {
  string_destroy(&result->out);
  string_destroy(&result->err);
}

#if defined(_WIN32)

/* Quotes one argument using the rules expected by CreateProcessW. */
static int append_argument(String *command, const char *argument) {
  int needs_quotes = argument[0] == '\0' || strpbrk(argument, " \t\"") != NULL;
  if (!needs_quotes)
    return string_append(command, argument) == AGENT_OK;
  if (string_append(command, "\"") != AGENT_OK)
    return 0;
  const char *at = argument;
  while (*at != '\0') {
    size_t slashes = 0;
    while (*at == '\\') {
      ++slashes;
      ++at;
    }
    if (*at == '\0') {
      for (size_t i = 0; i < slashes * 2; ++i)
        if (string_append(command, "\\") != AGENT_OK)
          return 0;
      break;
    }
    if (*at == '"') {
      for (size_t i = 0; i < slashes * 2 + 1; ++i)
        if (string_append(command, "\\") != AGENT_OK)
          return 0;
      if (string_append(command, "\"") != AGENT_OK)
        return 0;
      ++at;
    } else {
      for (size_t i = 0; i < slashes; ++i)
        if (string_append(command, "\\") != AGENT_OK)
          return 0;
      char text[2] = {*at, '\0'};
      if (string_append(command, text) != AGENT_OK)
        return 0;
      ++at;
    }
  }
  return string_append(command, "\"") == AGENT_OK;
}

static wchar_t *build_command_line(char *const argv[]) {
  String command = {0};
  for (size_t i = 0; argv[i] != NULL; ++i) {
    if (i != 0 && string_append(&command, " ") != AGENT_OK) {
      string_destroy(&command);
      return NULL;
    }
    if (!append_argument(&command, argv[i])) {
      string_destroy(&command);
      return NULL;
    }
  }
  wchar_t *wide = ta_utf8_to_wide(string_c_str(&command));
  string_destroy(&command);
  return wide;
}

static void close_handle(HANDLE *handle) {
  if (*handle != NULL && *handle != INVALID_HANDLE_VALUE)
    CloseHandle(*handle);
  *handle = INVALID_HANDLE_VALUE;
}

static void terminate_child(HANDLE job, HANDLE process) {
  if (job != NULL)
    (void)TerminateJobObject(job, 1);
  else if (process != NULL)
    (void)TerminateProcess(process, 1);
}

static int append_output(ToolProcess *result, int stream, const char *data,
                         size_t bytes, size_t limit) {
  size_t used = result->out.length + result->err.length;
  size_t space = used < limit ? limit - used : 0;
  size_t keep = bytes < space ? bytes : space;
  if (keep < bytes)
    result->truncated = 1;
  if (keep == 0)
    return 1;
  String *target = stream == 0 ? &result->out : &result->err;
  if (string_reserve(target, target->length + keep) != AGENT_OK)
    return 0;
  memcpy(target->data + target->length, data, keep);
  target->length += keep;
  target->data[target->length] = '\0';
  return 1;
}

static AgentStatus win_process(wchar_t *command_line, const char *cwd,
                               int timeout_ms, size_t limit, ToolContext *ctx,
                               ToolProcess *result, AgentError *error) {
  memset(result, 0, sizeof(*result));
  result->exit_code = -1;

  HANDLE pipes[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
  HANDLE out_write = INVALID_HANDLE_VALUE;
  HANDLE err_write = INVALID_HANDLE_VALUE;
  HANDLE input = INVALID_HANDLE_VALUE;
  HANDLE job = NULL;
  PROCESS_INFORMATION process;
  memset(&process, 0, sizeof(process));
  AgentStatus status = AGENT_ERR_IO;
  long long start = ta_monotonic_ms();
  int timed_out = 0;
  int created = 0;

  if (start < 0)
    goto done;

  SECURITY_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  if (!CreatePipe(&pipes[0], &out_write, &attributes, 0) ||
      !CreatePipe(&pipes[1], &err_write, &attributes, 0))
    goto done;
  if (!SetHandleInformation(pipes[0], HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(pipes[1], HANDLE_FLAG_INHERIT, 0))
    goto done;
  input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &attributes, OPEN_EXISTING, 0, NULL);
  if (input == INVALID_HANDLE_VALUE)
    goto done;

  wchar_t *working_directory = cwd != NULL ? ta_utf8_to_wide(cwd) : NULL;
  if (cwd != NULL && working_directory == NULL)
    goto done;

  STARTUPINFOW startup;
  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;

  if (!CreateProcessW(NULL, command_line, NULL, NULL, TRUE,
                      CREATE_SUSPENDED | CREATE_NO_WINDOW |
                          CREATE_UNICODE_ENVIRONMENT,
                      NULL, working_directory, &startup, &process)) {
    free(working_directory);
    goto done;
  }
  free(working_directory);
  created = 1;
  close_handle(&out_write);
  close_handle(&err_write);
  close_handle(&input);

  /* Assign the suspended child to a kill-on-close job before it can spawn
   * descendants of its own. */
  job = CreateJobObjectW(NULL, NULL);
  if (job != NULL) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
      CloseHandle(job);
      job = NULL;
    }
  }
  if (ResumeThread(process.hThread) == (DWORD)-1) {
    status = AGENT_ERR_IO;
    goto done;
  }
  CloseHandle(process.hThread);
  process.hThread = NULL;

  status = AGENT_OK;
  long long deadline = start + timeout_ms;
  for (;;) {
    int running = WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT;
    int open =
        (pipes[0] != INVALID_HANDLE_VALUE) + (pipes[1] != INVALID_HANDLE_VALUE);
    if (!running && open == 0)
      break;
    long long now = ta_monotonic_ms();
    if (now < 0) {
      status = AGENT_ERR_IO;
      goto done;
    }
    if (!timed_out && now >= deadline) {
      result->timed_out = 1;
      timed_out = 1;
      terminate_child(job, process.hProcess);
    }
    if (timed_out && now >= deadline + 200)
      break;
    if (ctx != NULL && ctx->is_cancelled != NULL &&
        ctx->is_cancelled(ctx->userdata)) {
      status = AGENT_ERR_CANCELLED;
      goto done;
    }
    for (int i = 0; i < 2; ++i) {
      if (pipes[i] == INVALID_HANDLE_VALUE)
        continue;
      DWORD available = 0;
      if (!PeekNamedPipe(pipes[i], NULL, 0, NULL, &available, NULL)) {
        DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) {
          close_handle(&pipes[i]);
          continue;
        }
        status = AGENT_ERR_IO;
        goto done;
      }
      while (available > 0) {
        char buffer[4096];
        DWORD request =
            (DWORD)(available < sizeof(buffer) ? available : sizeof(buffer));
        DWORD got = 0;
        if (!ReadFile(pipes[i], buffer, request, &got, NULL)) {
          DWORD code = GetLastError();
          if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) {
            close_handle(&pipes[i]);
            available = 0;
            break;
          }
          status = AGENT_ERR_IO;
          goto done;
        }
        if (got == 0)
          break;
        if (!append_output(result, i, buffer, (size_t)got, limit)) {
          status = AGENT_ERR_OUT_OF_MEMORY;
          goto done;
        }
        available = got < request ? 0 : available - got;
      }
    }
    Sleep(10);
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    status = AGENT_ERR_IO;
    goto done;
  }
  result->exit_code = (int)exit_code;

done:
  if (status != AGENT_OK && created)
    terminate_child(job, process.hProcess);
  if (process.hThread != NULL)
    CloseHandle(process.hThread);
  if (process.hProcess != NULL) {
    if (created)
      (void)WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
  }
  if (job != NULL)
    CloseHandle(job);
  close_handle(&pipes[0]);
  close_handle(&pipes[1]);
  close_handle(&out_write);
  close_handle(&err_write);
  close_handle(&input);
  if (status == AGENT_OK &&
      (!sanitize(&result->out) || !sanitize(&result->err)))
    status = AGENT_ERR_OUT_OF_MEMORY;
  if (status != AGENT_OK) {
    tool_process_destroy(result);
    return tool_fail(error, status,
                     status == AGENT_ERR_CANCELLED
                         ? "Tool execution cancelled"
                         : "Could not execute or capture process");
  }
  return AGENT_OK;
}

AgentStatus tool_process(char *const argv[], const char *cwd, int timeout_ms,
                         size_t limit, ToolContext *ctx, ToolProcess *result,
                         AgentError *error) {
  wchar_t *command = build_command_line(argv);
  if (command == NULL)
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY,
                     "Could not build the process command line");
  AgentStatus status =
      win_process(command, cwd, timeout_ms, limit, ctx, result, error);
  free(command);
  return status;
}

AgentStatus tool_process_shell(const char *command, const char *cwd,
                               int timeout_ms, size_t limit, ToolContext *ctx,
                               ToolProcess *result, AgentError *error) {
  if (command == NULL)
    return tool_fail(error, AGENT_ERR_NULL_POINTER, "Command is required");
  const char *shell = getenv("COMSPEC");
  if (shell == NULL || *shell == '\0')
    shell = "cmd.exe";
  String line = {0};
  int built = string_append(&line, "\"") == AGENT_OK &&
              string_append(&line, shell) == AGENT_OK &&
              string_append(&line, "\" /D /S /C \"") == AGENT_OK &&
              string_append(&line, command) == AGENT_OK &&
              string_append(&line, "\"") == AGENT_OK;
  wchar_t *wide = built ? ta_utf8_to_wide(string_c_str(&line)) : NULL;
  string_destroy(&line);
  if (wide == NULL)
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY,
                     "Could not build the shell command line");
  AgentStatus status =
      win_process(wide, cwd, timeout_ms, limit, ctx, result, error);
  free(wide);
  return status;
}

#else /* POSIX */

static int make_pipe(int fd[2]) {
  int raw[2];
  if (pipe(raw) != 0)
    return 0;
  for (int i = 0; i < 2; ++i) {
    fd[i] = fcntl(raw[i], F_DUPFD_CLOEXEC, 3);
    if (fd[i] < 0) {
      close(raw[0]);
      close(raw[1]);
      if (i == 1) {
        close(fd[0]);
        fd[0] = -1;
      }
      return 0;
    }
  }
  close(raw[0]);
  close(raw[1]);
  if (fcntl(fd[0], F_SETFL, O_NONBLOCK) != 0) {
    close(fd[0]);
    close(fd[1]);
    fd[0] = fd[1] = -1;
    return 0;
  }
  return 1;
}

AgentStatus tool_process(char *const argv[], const char *cwd, int timeout_ms,
                         size_t limit, ToolContext *ctx, ToolProcess *result,
                         AgentError *error) {
  memset(result, 0, sizeof(*result));
  result->exit_code = -1;
  int pipes[2][2] = {{-1, -1}, {-1, -1}};
  pid_t child = -1;
  int reaped = 0, wait_status = 0;
  AgentStatus status = AGENT_ERR_IO;
  long long start = ta_monotonic_ms();
  if (start < 0 || !make_pipe(pipes[0]) || !make_pipe(pipes[1]))
    goto done;
  child = fork();
  if (child < 0)
    goto done;
  if (child == 0) {
    close(pipes[0][0]);
    close(pipes[1][0]);
    if (dup2(pipes[0][1], STDOUT_FILENO) < 0 ||
        dup2(pipes[1][1], STDERR_FILENO) < 0)
      _exit(126);
    close(pipes[0][1]);
    close(pipes[1][1]);
    int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, STDIN_FILENO) < 0)
      _exit(126);
    if (input != STDIN_FILENO)
      close(input);
    if (setpgid(0, 0) != 0)
      _exit(126);
    if (cwd != NULL && chdir(cwd) != 0) {
      const char message[] = "Could not change working directory\n";
      (void)write(STDERR_FILENO, message, sizeof(message) - 1);
      _exit(126);
    }
    execvp(argv[0], argv);
    const char message[] =
        "Could not execute program (check executable and PATH)\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }
  (void)setpgid(child, child);
  for (int i = 0; i < 2; ++i) {
    close(pipes[i][1]);
    pipes[i][1] = -1;
  }
  status = AGENT_OK;
  long long deadline = start + timeout_ms;
  for (;;) {
    if (!reaped) {
      pid_t waited = waitpid(child, &wait_status, WNOHANG);
      if (waited == child)
        reaped = 1;
      else if (waited < 0 && errno != EINTR) {
        status = AGENT_ERR_IO;
        goto done;
      }
    }
    if (reaped && pipes[0][0] < 0 && pipes[1][0] < 0)
      break;
    long long now = ta_monotonic_ms();
    if (now < 0) {
      status = AGENT_ERR_IO;
      goto done;
    }
    if (now >= deadline && !result->timed_out) {
      result->timed_out = 1;
      (void)kill(-child, SIGKILL);
      if (!reaped)
        (void)kill(child, SIGKILL);
    }
    if (result->timed_out && now >= deadline + 200)
      break;
    /* Poll the host's cancel hook here as well, so interrupting a long command
     * kills the whole process group instead of waiting for the timeout. */
    if (ctx != NULL && ctx->is_cancelled != NULL &&
        ctx->is_cancelled(ctx->userdata)) {
      status = AGENT_ERR_CANCELLED;
      goto done;
    }
    struct pollfd fds[2] = {{.fd = pipes[0][0], .events = POLLIN},
                            {.fd = pipes[1][0], .events = POLLIN}};
    int delay = result->timed_out
                    ? 20
                    : (deadline - now > 20 ? 20 : (int)(deadline - now));
    int ready = poll(fds, 2, delay);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0) {
      status = AGENT_ERR_IO;
      goto done;
    }
    for (int i = 0; i < 2; ++i) {
      if (!fds[i].revents)
        continue;
      // Bound each drain so continuous output cannot starve the deadline.
      for (int j = 0; j < 16; ++j) {
        char buffer[4096];
        ssize_t n = read(pipes[i][0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
          continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        if (n < 0) {
          status = AGENT_ERR_IO;
          goto done;
        }
        if (n == 0) {
          close(pipes[i][0]);
          pipes[i][0] = -1;
          break;
        }
        size_t used = result->out.length + result->err.length;
        size_t available = used < limit ? limit - used : 0;
        size_t keep = (size_t)n < available ? (size_t)n : available;
        String *stream = i == 0 ? &result->out : &result->err;
        if (keep < (size_t)n)
          result->truncated = 1;
        if (keep == 0)
          continue;
        if (string_reserve(stream, stream->length + keep) != AGENT_OK) {
          status = AGENT_ERR_OUT_OF_MEMORY;
          goto done;
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
    do {
      waited = waitpid(child, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child)
      status = AGENT_ERR_IO;
  } else if (child > 0 && status != AGENT_OK) {
    (void)kill(-child, SIGKILL);
  }
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      if (pipes[i][j] >= 0)
        close(pipes[i][j]);
  if (status == AGENT_OK) {
    if (WIFEXITED(wait_status))
      result->exit_code = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) {
      result->signal = WTERMSIG(wait_status);
      result->exit_code = 128 + result->signal;
    }
    if (!sanitize(&result->out) || !sanitize(&result->err))
      status = AGENT_ERR_OUT_OF_MEMORY;
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

AgentStatus tool_process_shell(const char *command, const char *cwd,
                               int timeout_ms, size_t limit, ToolContext *ctx,
                               ToolProcess *result, AgentError *error) {
  if (command == NULL)
    return tool_fail(error, AGENT_ERR_NULL_POINTER, "Command is required");
  char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
  return tool_process(argv, cwd, timeout_ms, limit, ctx, result, error);
}

#endif /* _WIN32 */

cJSON *tool_process_json(const ToolProcess *result) {
  cJSON *json = cJSON_CreateObject();
  if (json == NULL ||
      !cJSON_AddStringToObject(json, "stdout", string_c_str(&result->out)) ||
      !cJSON_AddStringToObject(json, "stderr", string_c_str(&result->err)) ||
      !cJSON_AddNumberToObject(json, "exit_code", result->exit_code) ||
      !cJSON_AddNumberToObject(json, "signal", result->signal) ||
      !cJSON_AddBoolToObject(json, "timed_out", result->timed_out) ||
      !cJSON_AddBoolToObject(json, "truncated", result->truncated)) {
    cJSON_Delete(json);
    return NULL;
  }
  return json;
}
