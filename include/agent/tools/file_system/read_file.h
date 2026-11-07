#ifndef TINY_AGENT_READ_FILE_H
#define TINY_AGENT_READ_FILE_H

#include "agent/tool.h"

#define READ_FILE_DESCRIPTION "Read the complete contents of a UTF-8 text file (up to 1 MiB)"
#define READ_FILE_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to a regular UTF-8 text file\"}},\"required\":[\"path\"],\"additionalProperties\":false}"

// Returns the complete text of a regular UTF-8 file, up to 1 MiB. Embedded
// NUL bytes, invalid UTF-8, non-regular files, and oversized files are rejected.
// Symbolic links are followed. On failure, *output is NULL.
extern const ToolDefinition read_file_tool;
AgentStatus read_file_register(ToolRegistry *reg, AgentError *error);

AgentStatus read_file(ToolContext *ctx, const cJSON *arguments,
                     ToolOutput **output, AgentError *error);

#endif // TINY_AGENT_READ_FILE_H
