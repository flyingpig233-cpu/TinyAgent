#ifndef TINY_AGENT_RUN_COMMAND_H
#define TINY_AGENT_RUN_COMMAND_H
#include "agent/tool.h"

#define RUN_COMMAND_DESCRIPTION "Execute a shell command using /bin/sh -c in cwd, with closed stdin, timeout and bounded stdout/stderr. Returns exit_code, signal, stdout, stderr, timed_out and truncated."
#define RUN_COMMAND_PARAMETERS_JSON \
  "{\"type\":\"object\",\"properties\":{" \
  "\"command\":{\"type\":\"string\"}," \
  "\"cwd\":{\"type\":[\"string\",\"null\"],\"description\":\"Working directory; null inherits current directory\"}," \
  "\"timeout_ms\":{\"type\":[\"integer\",\"null\"],\"minimum\":1,\"maximum\":300000,\"description\":\"null defaults to 30000\"}," \
  "\"max_output_bytes\":{\"type\":[\"integer\",\"null\"],\"minimum\":1,\"maximum\":1048576,\"description\":\"Combined stdout/stderr cap; null defaults to 65536\"}}," \
  "\"required\":[\"command\",\"cwd\",\"timeout_ms\",\"max_output_bytes\"],\"additionalProperties\":false}"

// Nonzero exit and timeout are successful tool invocations with is_error=1.
// Output is UTF-8; invalid bytes and NULs are replaced with U+FFFD.
// On timeout the command's process group is killed and the child is reaped.
extern const ToolDefinition run_command_tool;
AgentStatus run_command_register(ToolRegistry *reg, AgentError *error);
AgentStatus run_command(ToolContext *ctx, const cJSON *arguments,
                        ToolOutput **output, AgentError *error);
#endif
