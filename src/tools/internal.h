#ifndef TINY_AGENT_TOOLS_INTERNAL_H
#define TINY_AGENT_TOOLS_INTERNAL_H
#include "agent/tool.h"
#include "agent/infra.h"
#include <sys/stat.h>

enum { TOOL_FILE_LIMIT = 1024 * 1024 };
AgentStatus tool_begin(const cJSON *args, ToolOutput **out, AgentError *error);
const char *tool_string(const cJSON *args, const char *key);
int tool_integer(const cJSON *args, const char *key, int fallback, int min,
                 int max, int *value);
int tool_optional_string(const cJSON *args, const char *key, const char **value);
AgentStatus tool_fail(AgentError *error, AgentStatus status, const char *message);
AgentStatus tool_json(cJSON *json, int is_error, ToolOutput **out, AgentError *error);
int tool_utf8(const unsigned char *s, size_t length);
AgentStatus tool_atomic_write(const char *path, const char *content, int create,
                              const struct stat *expected, AgentError *error);
typedef struct {
  String out, err;
  int exit_code, signal, timed_out, truncated;
} ToolProcess;
void tool_process_destroy(ToolProcess *result);
AgentStatus tool_process(char *const argv[], const char *cwd, int timeout_ms,
                         size_t limit, ToolContext *ctx, ToolProcess *result,
                         AgentError *error);
// Owns neither result nor its strings; the returned JSON is owned by the caller.
cJSON *tool_process_json(const ToolProcess *result);
#endif
