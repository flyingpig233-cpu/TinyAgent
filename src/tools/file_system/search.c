#include "agent/tools/file_system/search.h"
#include "../internal.h"
#include <string.h>

const ToolDefinition search_tool = {
    .name = "search", .description = SEARCH_DESCRIPTION,
    .parameters_json = SEARCH_PARAMETERS_JSON, .strict = 1, .fn = search};

AgentStatus search_register(ToolRegistry *reg, AgentError *error) {
  return tool_registry_register(reg, &search_tool, error);
}

AgentStatus search(ToolContext *ctx, const cJSON *args, ToolOutput **out, AgentError *error) {
  (void)ctx;
  AgentStatus status = tool_begin(args, out, error);
  if (status != AGENT_OK) return status;
  const char *pattern = tool_string(args, "pattern"), *path, *glob;
  const cJSON *fixed = cJSON_GetObjectItemCaseSensitive(args, "fixed_string");
  int max_results;
  if (pattern == NULL || !tool_optional_string(args, "path", &path) ||
      !tool_optional_string(args, "glob", &glob) ||
      (fixed != NULL && !cJSON_IsNull(fixed) && !cJSON_IsBool(fixed)) ||
      !tool_integer(args, "max_results", 100, 1, 1000, &max_results))
    return tool_fail(error, AGENT_ERR_INVAILD_ARGUMENT, "Invalid search pattern, path, glob, fixed_string or max_results");
  char *argv[16];
  int count = 0;
  argv[count++] = "rg"; argv[count++] = "--no-config"; argv[count++] = "--json";
  argv[count++] = "--line-number"; argv[count++] = "--color=never";
  if (cJSON_IsTrue(fixed)) argv[count++] = "--fixed-strings";
  if (glob != NULL) { argv[count++] = "--glob"; argv[count++] = (char *)glob; }
  argv[count++] = "-e"; argv[count++] = (char *)pattern;
  argv[count++] = "--"; argv[count++] = (char *)(path == NULL ? "." : path);
  argv[count] = NULL;
  ToolProcess process;
  status = tool_process(argv, NULL, 10000, TOOL_FILE_LIMIT, &process, error);
  if (status != AGENT_OK) return status;
  cJSON *json = cJSON_CreateObject(), *matches = cJSON_CreateArray();
  if (json == NULL || matches == NULL || !cJSON_AddItemToObject(json, "matches", matches)) {
    cJSON_Delete(matches); cJSON_Delete(json);
    tool_process_destroy(&process);
    return tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate search result");
  }
  int found = 0;
  const char *line = string_c_str(&process.out);
  const char *end = line + process.out.length;
  while (line < end) {
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    if (newline == NULL) { process.truncated = 1; break; }
    cJSON *event = cJSON_ParseWithLength(line, (size_t)(newline - line));
    line = newline + 1;
    if (event == NULL) {
      status = tool_fail(error, AGENT_ERR_IO, "Could not parse ripgrep JSON output");
      break;
    }
    const char *type = tool_string(event, "type");
    if (type != NULL && strcmp(type, "match") == 0) {
      const cJSON *data = cJSON_GetObjectItemCaseSensitive(event, "data");
      const char *file = tool_string(cJSON_GetObjectItemCaseSensitive(data, "path"), "text");
      const char *text = tool_string(cJSON_GetObjectItemCaseSensitive(data, "lines"), "text");
      const cJSON *number = cJSON_GetObjectItemCaseSensitive(data, "line_number");
      if (found == max_results || file == NULL || text == NULL || !cJSON_IsNumber(number)) {
        process.truncated = 1;
      } else {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddStringToObject(item, "path", file) ||
            !cJSON_AddNumberToObject(item, "line", number->valuedouble) ||
            !cJSON_AddStringToObject(item, "text", text) || !cJSON_AddItemToArray(matches, item)) {
          cJSON_Delete(item);
          cJSON_Delete(event);
          status = tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate search match");
          break;
        }
        cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "text");
        size_t length = strlen(value->valuestring);
        if (length > 0 && value->valuestring[length - 1] == '\n') value->valuestring[--length] = '\0';
        if (length > 0 && value->valuestring[length - 1] == '\r') value->valuestring[length - 1] = '\0';
        ++found;
      }
    }
    cJSON_Delete(event);
  }
  if (status == AGENT_OK) {
    if (!cJSON_AddStringToObject(json, "stderr", string_c_str(&process.err)) ||
        !cJSON_AddNumberToObject(json, "exit_code", process.exit_code) ||
        !cJSON_AddBoolToObject(json, "timed_out", process.timed_out) ||
        !cJSON_AddBoolToObject(json, "truncated", process.truncated))
      status = tool_fail(error, AGENT_ERR_OUT_OF_MEMORY, "Could not allocate search result");
    else
      status = tool_json(json, process.exit_code > 1 || process.exit_code < 0 || process.timed_out, out, error);
  }
  cJSON_Delete(json);
  tool_process_destroy(&process);
  return status;
}
