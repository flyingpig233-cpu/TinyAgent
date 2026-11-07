#ifndef TINY_AGENT_LIST_DIR_H
#define TINY_AGENT_LIST_DIR_H

#include "agent/tool.h"

#define LIST_DIR_DESCRIPTION \
  "List information about immediate entries in a directory"
#define LIST_DIR_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path\"}},\"required\":[\"path\"],\"additionalProperties\":false}"

// Arguments: {"path": "directory"}. Returns a JSON array of immediate
// entries (excluding . and ..), including hidden entries. Each entry contains
// name, type, size in bytes, mode and modification time in Unix seconds.
// Symbolic links are described without following them. Order is unspecified.
extern const ToolDefinition list_dir_tool;

AgentStatus list_dir(ToolContext *ctx, const cJSON *arguments,
                     ToolOutput **output, AgentError *error);

AgentStatus list_dir_register(ToolRegistry *reg, AgentError *error);

#endif // TINY_AGENT_LIST_DIR_H
