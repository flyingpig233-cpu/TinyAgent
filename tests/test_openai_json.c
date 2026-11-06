#include "agent/context.h"
#include "agent/providers/openai.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  ToolCall call = {.call_id = "call_1",
                   .tool_name = "weather",
                   .arguments_json = "{\"city\":\"北京\"}"};
  Message messages[] = {
      {.role = SYSTEM_MESSAGE, .content = "system"},
      {.role = USER_MESSAGE, .content = "say \"hi\""},
      {.role = ASSISTANT_MESSAGE, .tool_calls = &call, .tool_call_count = 1},
      {.role = TOOL_MESSAGE, .content = "sunny", .tool_call_id = "call_1"},
  };
  MessageArrary array = {.items = messages, .count = 4, .capacity = 4};
  Context ctx = {.array = &array};
  LLMRequest request = {.context = &ctx};
  char *json = NULL;
  if (llm_request_to_json(&request, "model", &json) != AGENT_OK)
    return 1;
  cJSON *root = cJSON_Parse(json);
  free(json);
  if (root == NULL)
    return 2;
  cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
  cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "messages");
  cJSON *assistant = cJSON_GetArrayItem(items, 2);
  cJSON *calls = cJSON_GetObjectItemCaseSensitive(assistant, "tool_calls");
  cJSON *function = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(calls, 0), "function");
  cJSON *args = cJSON_GetObjectItemCaseSensitive(function, "arguments");
  cJSON *tool = cJSON_GetArrayItem(items, 3);
  cJSON *id = cJSON_GetObjectItemCaseSensitive(tool, "tool_call_id");
  if (!cJSON_IsString(model) || strcmp(model->valuestring, "model") ||
      cJSON_GetArraySize(items) != 4 ||
      !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(assistant, "content")) ||
      !cJSON_IsString(args) || strcmp(args->valuestring, call.arguments_json) ||
      !cJSON_IsString(id) || strcmp(id->valuestring, "call_1"))
    return 3;
  cJSON_Delete(root);
  messages[2].tool_calls = NULL;
  if (llm_request_to_json(&request, "model", &json) !=
          AGENT_ERR_INVAILD_ARGUMENT ||
      json != NULL)
    return 4;
  if (llm_request_to_json(NULL, "model", &json) != AGENT_ERR_NULL_POINTER)
    return 5;
  const char *reply =
      "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":"
      "\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"weather\","
      "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
  LLMResponse *response = NULL;
  if (llm_json_to_response(reply, &response) != AGENT_OK || response == NULL ||
      response->finish_reason != LLM_FINISH_TOOL_CALLS ||
      response->content != NULL || response->tool_call_count != 1 ||
      strcmp(response->tool_calls[0].tool_name, "weather") ||
      strcmp(response->tool_calls[0].call_id, "call_1"))
    return 6;
  llm_response_destroy(response);
  const char *normal = "{\"choices\":[{\"message\":{\"content\":\"hello\"},"
                       "\"finish_reason\":\"stop\"}]}";
  if (llm_json_to_response(normal, &response) != AGENT_OK ||
      strcmp(response->content, "hello") ||
      response->finish_reason != LLM_FINISH_STOP)
    return 7;
  llm_response_destroy(response);
  if (llm_json_to_response("{\"choices\":[]}", &response) !=
          AGENT_ERR_LLM_RESPONSE ||
      response != NULL)
    return 8;
  if (llm_json_to_response("{} trailing", &response) !=
          AGENT_ERR_LLM_RESPONSE ||
      response != NULL)
    return 9;
  if (llm_json_to_response(
          "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{}]},"
          "\"finish_reason\":\"tool_calls\"}]}",
          &response) != AGENT_ERR_LLM_RESPONSE ||
      response != NULL)
    return 10;
  return 0;
}
