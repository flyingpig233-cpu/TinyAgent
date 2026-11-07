#ifndef TINY_AGENT_SEARCH_H
#define TINY_AGENT_SEARCH_H
#include "agent/tool.h"

#define SEARCH_DESCRIPTION "Search file contents with ripgrep; return matching paths, 1-based line numbers and text. Honors ignore files, skips hidden files and does not follow symlinks."
#define SEARCH_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{" \
  "\"pattern\":{\"type\":\"string\"}," \
  "\"path\":{\"type\":[\"string\",\"null\"],\"description\":\"File or directory; null means current directory\"}," \
  "\"glob\":{\"type\":[\"string\",\"null\"],\"description\":\"File glob, e.g. *.c; null means all files\"}," \
  "\"fixed_string\":{\"type\":[\"boolean\",\"null\"],\"description\":\"Use literal matching; null means regex\"}," \
  "\"max_results\":{\"type\":[\"integer\",\"null\"],\"minimum\":1,\"maximum\":1000,\"description\":\"Matching lines; null defaults to 100\"}}," \
  "\"required\":[\"pattern\",\"path\",\"glob\",\"fixed_string\",\"max_results\"],\"additionalProperties\":false}"

// Requires rg on PATH. Bounded to 10 seconds and 1 MiB captured output.
// Returns {matches:[{path,line,text}], truncated, stderr, exit_code, timed_out}.
extern const ToolDefinition search_tool;
AgentStatus search_register(ToolRegistry *reg, AgentError *error);
AgentStatus search(ToolContext *ctx, const cJSON *arguments,
                   ToolOutput **output, AgentError *error);
#endif
