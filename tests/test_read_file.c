#define _POSIX_C_SOURCE 200809L
#include "agent/tools/file_system/read_file.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

int main(void) {
  char name[] = "/tmp/tiny-agent-read-XXXXXX";
  int fd = mkstemp(name);
  CHECK(fd >= 0);
  const char text[] = "hello \xe4\xb8\x96\xe7\x95\x8c\n";
  CHECK(write(fd, text, sizeof(text) - 1) == (ssize_t)(sizeof(text) - 1));
  CHECK(close(fd) == 0);
  cJSON *args = cJSON_CreateObject();
  CHECK(args && cJSON_AddStringToObject(args, "path", name));
  ToolOutput *output = NULL;
  AgentError error = {0};
  CHECK(read_file(NULL, args, &output, &error) == AGENT_OK);
  CHECK(output && strcmp(output->content, text) == 0 && !output->is_error);
  free(output->content); free(output);
  fd = open(name, O_WRONLY | O_TRUNC);
  CHECK(fd >= 0 && write(fd, "a\0b", 3) == 3 && close(fd) == 0);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  fd = open(name, O_WRONLY | O_TRUNC);
  CHECK(fd >= 0 && write(fd, "\xc0\x80", 2) == 2 && close(fd) == 0);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  fd = open(name, O_WRONLY | O_TRUNC);
  CHECK(fd >= 0 && ftruncate(fd, 1024 * 1024 + 1) == 0 && close(fd) == 0);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  CHECK(unlink(name) == 0);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_ERR_IO && output == NULL);
  cJSON_Delete(args);
  args = cJSON_Parse("{\"path\":\"/tmp\"}");
  CHECK(read_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  cJSON_Delete(args);
  return 0;
}
