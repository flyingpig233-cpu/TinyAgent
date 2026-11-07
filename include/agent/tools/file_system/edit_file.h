#ifndef TINY_AGENT_EDIT_FILE_H
#define TINY_AGENT_EDIT_FILE_H
#include "agent/tool.h"

#define EDIT_FILE_DESCRIPTION "Atomically replace one exact occurrence of old_text with new_text in a UTF-8 text file up to 1 MiB. old_text must be nonempty and unique; symlinks are rejected."
#define EDIT_FILE_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{" \
  "\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"}," \
  "\"new_text\":{\"type\":\"string\"}}," \
  "\"required\":[\"path\",\"old_text\",\"new_text\"],\"additionalProperties\":false}"

extern const ToolDefinition edit_file_tool;
AgentStatus edit_file_register(ToolRegistry *reg, AgentError *error);
AgentStatus edit_file(ToolContext *ctx, const cJSON *arguments,
                      ToolOutput **output, AgentError *error);
#endif
