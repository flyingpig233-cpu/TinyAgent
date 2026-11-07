#include "agent/tools/run_command.h"
#include "internal.h"

const ToolDefinition run_command_tool = {
    .name = "run_command", .description = RUN_COMMAND_DESCRIPTION,
    .parameters_json = RUN_COMMAND_PARAMETERS_JSON, .strict = 1, .mutating = 1, .fn = run_command};

AgentStatus run_command_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &run_command_tool, error);
}

AgentStatus run_command(ToolContext *ctx, const cJSON *args, ToolOutput **out, AgentError *error) {
  (void)ctx;
  AgentStatus status = tool_begin(args, out, error);
  if (status != AGENT_OK) return status;
  const char *command = tool_string(args, "command"), *cwd;
  int timeout, limit;
  if (command == NULL || *command == '\0' || !tool_optional_string(args, "cwd", &cwd) ||
      !tool_integer(args, "timeout_ms", 30000, 1, 300000, &timeout) ||
      !tool_integer(args, "max_output_bytes", 65536, 1, TOOL_FILE_LIMIT, &limit))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "Invalid command, cwd, timeout_ms or max_output_bytes");
  char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
  ToolProcess result;
  status = tool_process(argv, cwd, timeout, (size_t)limit, &result, error);
  if (status != AGENT_OK) return status;
  cJSON *json = tool_process_json(&result);
  status = tool_json(json, result.exit_code != 0 || result.timed_out, out, error);
  cJSON_Delete(json);
  tool_process_destroy(&result);
  return status;
}
