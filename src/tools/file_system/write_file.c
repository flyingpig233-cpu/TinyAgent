#include "agent/tools/file_system/write_file.h"
#include "../internal.h"
#include <stdlib.h>
#include <string.h>

const ToolDefinition write_file_tool = {
    .name = "write_file", .description = WRITE_FILE_DESCRIPTION,
    .parameters_json = WRITE_FILE_PARAMETERS_JSON, .strict = 1, .mutating = 1, .fn = write_file};

AgentStatus write_file_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &write_file_tool, error);
}

AgentStatus write_file(ToolContext *ctx, const cJSON *args, ToolOutput **out, AgentError *error) {
  (void)ctx;
  AgentStatus status = tool_begin(args, out, error);
  if (status != AGENT_OK) return status;
  const char *path = tool_string(args, "path"), *content = tool_string(args, "content");
  const char *mode = tool_string(args, "mode");
  if (path == NULL || *path == '\0' || content == NULL || mode == NULL ||
      (strcmp(mode, "create") != 0 && strcmp(mode, "overwrite") != 0))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "path, content and mode (create or overwrite) are required");
  size_t length = strlen(content);
  if (length > TOOL_FILE_LIMIT || !tool_utf8((const unsigned char *)content, length))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "Content must be UTF-8 text up to 1 MiB");
  cJSON *json = cJSON_CreateObject();
  if (json == NULL || !cJSON_AddStringToObject(json, "path", path) ||
      !cJSON_AddStringToObject(json, "mode", mode) ||
      !cJSON_AddNumberToObject(json, "bytes_written", (double)length)) {
    cJSON_Delete(json);
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate write result");
  }
  status = tool_json(json, 0, out, error);
  cJSON_Delete(json);
  if (status != AGENT_OK) return status;
  status = tool_atomic_write(path, content, strcmp(mode, "create") == 0, NULL, error);
  if (status != AGENT_OK) { free((*out)->content); free(*out); *out = NULL; }
  return status;
}
