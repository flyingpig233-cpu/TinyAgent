#include "agent/context.h"
#include "agent/infra.h"
#include "agent/providers/openai.h"
#include "openai_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *message_role(MessageRole role) {
  switch (role) {
  case SYSTEM_MESSAGE:
    return "system";
  case USER_MESSAGE:
    return "user";
  case ASSISTANT_MESSAGE:
    return "assistant";
  case TOOL_MESSAGE:
    return "tool";
  default:
    return NULL;
  }
}

int openai_chat_encode_request(const LLMRequest *request, const char *model,
                               char **out) {
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  int status = openai_validate_request(request, model);
  if (status != AGENT_OK)
    return status;
  const MessageArrary *messages = request->context->array;
  status = AGENT_ERR_OUT_OF_MEMORY;
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return status;
  if (cJSON_AddStringToObject(root, "model", model) == NULL)
    goto done;
  if (request->stream && cJSON_AddBoolToObject(root, "stream", 1) == NULL)
    goto done;
  cJSON *array = cJSON_AddArrayToObject(root, "messages");
  if (array == NULL)
    goto done;
  for (size_t i = 0; i < messages->count; ++i) {
    const Message *msg = &messages->items[i];
    const char *role = message_role(msg->role);
    if (msg->content == NULL && msg->tool_call_count == 0 &&
        msg->refusal == NULL) {
      status = AGENT_ERR_INVAILD_ARGUMENT;
      goto done;
    }
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL)
      goto done;
    if (!cJSON_AddItemToArray(array, entry)) {
      cJSON_Delete(entry);
      goto done;
    }
    if (cJSON_AddStringToObject(entry, "role", role) == NULL)
      goto done;
    if (msg->content != NULL) {
      if (cJSON_AddStringToObject(entry, "content", msg->content) == NULL)
        goto done;
    } else if (cJSON_AddNullToObject(entry, "content") == NULL)
      goto done;
    if (msg->refusal != NULL &&
        cJSON_AddStringToObject(entry, "refusal", msg->refusal) == NULL)
      goto done;
    if (msg->role == TOOL_MESSAGE &&
        cJSON_AddStringToObject(entry, "tool_call_id", msg->tool_call_id) ==
            NULL)
      goto done;
    if (msg->tool_call_count) {
      cJSON *calls = cJSON_AddArrayToObject(entry, "tool_calls");
      if (calls == NULL)
        goto done;
      for (size_t j = 0; j < msg->tool_call_count; ++j) {
        const ToolCall *call = &msg->tool_calls[j];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL)
          goto done;
        if (!cJSON_AddItemToArray(calls, item)) {
          cJSON_Delete(item);
          goto done;
        }
        if (cJSON_AddStringToObject(item, "id", call->call_id) == NULL ||
            cJSON_AddStringToObject(item, "type", "function") == NULL)
          goto done;
        cJSON *function = cJSON_AddObjectToObject(item, "function");
        if (function == NULL ||
            cJSON_AddStringToObject(function, "name", call->tool_name) ==
                NULL ||
            cJSON_AddStringToObject(function, "arguments",
                                    call->arguments_json) == NULL)
          goto done;
      }
    }
  }
  status = openai_add_tools(root, request, PROTOCOL_CHAT_COMPLETIONS);
  if (status != AGENT_OK)
    goto done;
  status = AGENT_ERR_OUT_OF_MEMORY;
  *out = cJSON_PrintUnformatted(root);
  if (*out != NULL)
    status = AGENT_OK;
done:
  cJSON_Delete(root);
  return status;
}

int openai_chat_decode_response(const char *str, LLMResponse **out,
                                AgentError *error) {
  agent_error_clear(error);
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  if (str == NULL)
    return AGENT_ERR_NULL_POINTER;
  cJSON *root = cJSON_ParseWithOpts(str, NULL, 1);
  if (root == NULL) {
    ERROR(error, "Could not parse OpenAI Chat Completions response");
    return AGENT_ERR_LLM_RESPONSE;
  }
  int status = AGENT_ERR_LLM_RESPONSE;
  LLMResponse *response = NULL;
  cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  cJSON *choice =
      cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
  cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
  if (!cJSON_IsObject(message) || !cJSON_IsString(finish))
    goto done;
  cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
  cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
  cJSON *refusal = cJSON_GetObjectItemCaseSensitive(message, "refusal");
  if ((content != NULL && !cJSON_IsString(content) && !cJSON_IsNull(content)) ||
      (calls != NULL && !cJSON_IsNull(calls) && !cJSON_IsArray(calls)) ||
      (refusal != NULL && !cJSON_IsNull(refusal) && !cJSON_IsString(refusal)))
    goto done;
  response = calloc(1, sizeof(*response));
  if (response == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    goto done;
  }
  status = full_copy(&response->provider_status, finish->valuestring);
  if (status != AGENT_OK)
    goto done;
  if (cJSON_IsString(refusal)) {
    status = full_copy(&response->refusal, refusal->valuestring);
    if (status != AGENT_OK)
      goto done;
  }
  if (cJSON_IsString(content)) {
    status = full_copy(&response->content, content->valuestring);
    if (status != AGENT_OK)
      goto done;
  }
  if (strcmp(finish->valuestring, "stop") == 0)
    response->finish_reason = LLM_FINISH_STOP;
  else if (strcmp(finish->valuestring, "tool_calls") == 0)
    response->finish_reason = LLM_FINISH_TOOL_CALLS;
  else if (strcmp(finish->valuestring, "length") == 0)
    response->finish_reason = LLM_FINISH_LENGTH;
  else if (strcmp(finish->valuestring, "content_filter") == 0)
    response->finish_reason = LLM_FINISH_FILTERED;
  else
    response->finish_reason = LLM_FINISH_UNKNOWN;
  if (response->refusal != NULL)
    response->finish_reason = LLM_FINISH_REFUSED;
  if (cJSON_IsArray(calls)) {
    size_t count = (size_t)cJSON_GetArraySize(calls);
    if (count > SIZE_MAX / sizeof(*response->tool_calls)) {
      status = AGENT_ERR_OUT_OF_MEMORY;
      goto done;
    }
    if (count) {
      response->tool_calls = calloc(count, sizeof(*response->tool_calls));
      if (response->tool_calls == NULL) {
        status = AGENT_ERR_OUT_OF_MEMORY;
        goto done;
      }
      response->tool_call_count = count;
    }
    cJSON *item = NULL;
    size_t i = 0;
    cJSON_ArrayForEach(item, calls) {
      cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
      cJSON *function = cJSON_GetObjectItemCaseSensitive(item, "function");
      cJSON *name = cJSON_GetObjectItemCaseSensitive(function, "name");
      cJSON *arguments =
          cJSON_GetObjectItemCaseSensitive(function, "arguments");
      if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
          !cJSON_IsString(type) || strcmp(type->valuestring, "function") ||
          !cJSON_IsString(name) || name->valuestring[0] == '\0' ||
          !cJSON_IsString(arguments)) {
        status = AGENT_ERR_LLM_RESPONSE;
        goto done;
      }
      ToolCall *call = &response->tool_calls[i++];
      status = full_copy(&call->call_id, id->valuestring);
      if (status != AGENT_OK)
        goto done;
      status = full_copy(&call->tool_name, name->valuestring);
      if (status != AGENT_OK)
        goto done;
      status = full_copy(&call->arguments_json, arguments->valuestring);
      if (status != AGENT_OK)
        goto done;
    }
  }
  if ((response->finish_reason == LLM_FINISH_TOOL_CALLS &&
       response->tool_call_count == 0) ||
      (response->finish_reason == LLM_FINISH_STOP &&
       (response->tool_call_count != 0 || response->content == NULL))) {
    status = AGENT_ERR_LLM_RESPONSE;
    goto done;
  }
  status = AGENT_OK;
  *out = response;
  response = NULL;
done:
  llm_response_destroy(response);
  cJSON_Delete(root);
  if (status != AGENT_OK)
    ERROR(error, "Could not parse OpenAI Chat Completions response");
  return status;
}

int llm_request_to_json(const LLMRequest *request, const char *model,
                        char **out) {
  return openai_chat_encode_request(request, model, out);
}

int llm_json_to_response(const char *json, LLMResponse **out) {
  return openai_chat_decode_response(json, out, NULL);
}
