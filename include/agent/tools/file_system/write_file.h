#ifndef TINY_AGENT_WRITE_FILE_H
#define TINY_AGENT_WRITE_FILE_H
#include "agent/tool.h"

#define WRITE_FILE_DESCRIPTION "Atomically create or overwrite a UTF-8 text file up to 1 MiB. create requires an absent path; overwrite requires an existing regular file. Parent directory must exist; symlinks are rejected."
#define WRITE_FILE_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{" \
  "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}," \
  "\"mode\":{\"type\":\"string\",\"enum\":[\"create\",\"overwrite\"]}}," \
  "\"required\":[\"path\",\"content\",\"mode\"],\"additionalProperties\":false}"

extern const ToolDefinition write_file_tool;
AgentStatus write_file_register(ToolRegistry *reg, AgentError *error);
AgentStatus write_file(ToolContext *ctx, const cJSON *arguments,
                       ToolOutput **output, AgentError *error);
#endif
