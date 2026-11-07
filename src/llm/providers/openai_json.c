#include "agent/context.h"
#include "openai_internal.h"
#include <stdio.h>

int openai_validate_request(const LLMRequest *request, const char *model) {
  if (request == NULL || model == NULL || request->context == NULL ||
      request->context->array == NULL)
    return AGENT_ERR_NULL_POINTER;
  const MessageArrary *messages = request->context->array;
  if (model[0] == '\0' || (messages->count && messages->items == NULL) ||
      (request->tool_count && request->tools == NULL))
    return AGENT_ERR_INVAILD_ARGUMENT;
  for (size_t i = 0; i < messages->count; ++i) {
    const Message *msg = &messages->items[i];
    if (msg->role < SYSTEM_MESSAGE || msg->role > TOOL_MESSAGE ||
        (msg->tool_call_count &&
         (msg->role != ASSISTANT_MESSAGE || msg->tool_calls == NULL)) ||
        (msg->content == NULL &&
         (msg->role != ASSISTANT_MESSAGE ||
          (!msg->tool_call_count && msg->refusal == NULL &&
           msg->continuation.items_json == NULL))) ||
        (msg->refusal != NULL && msg->role != ASSISTANT_MESSAGE) ||
        (msg->role == TOOL_MESSAGE &&
         (msg->tool_call_id == NULL || msg->tool_call_id[0] == '\0')) ||
        (msg->continuation.format != LLM_CONTINUATION_NONE &&
         msg->continuation.format != LLM_CONTINUATION_OPENAI_RESPONSES) ||
        ((msg->continuation.items_json != NULL) !=
         (msg->continuation.format != LLM_CONTINUATION_NONE)) ||
        (msg->continuation.items_json != NULL &&
         msg->role != ASSISTANT_MESSAGE))
      return AGENT_ERR_INVAILD_ARGUMENT;
    for (size_t j = 0; j < msg->tool_call_count; ++j) {
      const ToolCall *call = &msg->tool_calls[j];
      if (call->call_id == NULL || call->call_id[0] == '\0' ||
          call->tool_name == NULL || call->tool_name[0] == '\0' ||
          call->arguments_json == NULL)
        return AGENT_ERR_INVAILD_ARGUMENT;
    }
  }
  return AGENT_OK;
}

int openai_add_tools(cJSON *root, const LLMRequest *request,
                     OpenAIProtocol protocol) {
  if (request->tool_count == 0)
    return AGENT_OK;
  cJSON *tools = cJSON_AddArrayToObject(root, "tools");
  if (tools == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  for (size_t i = 0; i < request->tool_count; ++i) {
    const ToolDefinition *tool = request->tools[i];
    if (tool == NULL || tool->name == NULL || tool->name[0] == '\0' ||
        tool->parameters_json == NULL ||
        (tool->strict != 0 && tool->strict != 1))
      return AGENT_ERR_INVAILD_ARGUMENT;
    cJSON *parameters = cJSON_ParseWithOpts(tool->parameters_json, NULL, 1);
    if (!cJSON_IsObject(parameters)) {
      cJSON_Delete(parameters);
      return AGENT_ERR_INVAILD_ARGUMENT;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || !cJSON_AddItemToArray(tools, item)) {
      cJSON_Delete(item);
      cJSON_Delete(parameters);
      return AGENT_ERR_OUT_OF_MEMORY;
    }
    cJSON *function = protocol == PROTOCOL_CHAT_COMPLETIONS
                          ? cJSON_AddObjectToObject(item, "function")
                          : item;
    if (function == NULL ||
        cJSON_AddStringToObject(item, "type", "function") == NULL ||
        cJSON_AddStringToObject(function, "name", tool->name) == NULL ||
        (tool->description != NULL &&
         cJSON_AddStringToObject(function, "description", tool->description) ==
             NULL) ||
        cJSON_AddBoolToObject(function, "strict", tool->strict) == NULL ||
        !cJSON_AddItemToObject(function, "parameters", parameters)) {
      cJSON_Delete(parameters);
      return AGENT_ERR_OUT_OF_MEMORY;
    }
  }
  return AGENT_OK;
}

void openai_json_error(AgentError *error, const char *stage,
                       const cJSON *detail) {
  cJSON *code = cJSON_GetObjectItemCaseSensitive(detail, "code");
  cJSON *message = cJSON_GetObjectItemCaseSensitive(detail, "message");
  char text[256];
  snprintf(text, sizeof(text), "%s%s%s%s%s", stage,
           cJSON_IsString(code) ? ": " : "",
           cJSON_IsString(code) ? code->valuestring : "",
           cJSON_IsString(message) ? ": " : "",
           cJSON_IsString(message) ? message->valuestring : "");
  ERROR(error, text);
}
