#include "agent/context.h"
#include "agent/infra.h"
#include "agent/providers/openai.h"
#include "openai_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int append_replay(cJSON *input, const char *json) {
  cJSON *items = cJSON_ParseWithOpts(json, NULL, 1);
  if (!cJSON_IsArray(items)) {
    cJSON_Delete(items);
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  int status = AGENT_OK;
  cJSON *item = NULL;
  cJSON_ArrayForEach(item, items) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    if (!cJSON_IsString(type) || (strcmp(type->valuestring, "message") &&
                                  strcmp(type->valuestring, "reasoning") &&
                                  strcmp(type->valuestring, "function_call"))) {
      status = AGENT_ERR_INVAILD_ARGUMENT;
      break;
    }
    cJSON *copy = cJSON_Duplicate(item, 1);
    if (copy == NULL || !cJSON_AddItemToArray(input, copy)) {
      cJSON_Delete(copy);
      status = AGENT_ERR_OUT_OF_MEMORY;
      break;
    }
  }
  cJSON_Delete(items);
  return status;
}

static int append_message(cJSON *input, const Message *message) {
  cJSON *item = cJSON_CreateObject();
  if (item == NULL || !cJSON_AddItemToArray(input, item)) {
    cJSON_Delete(item);
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  if (message->role == TOOL_MESSAGE) {
    if (cJSON_AddStringToObject(item, "type", "function_call_output") == NULL ||
        cJSON_AddStringToObject(item, "call_id", message->tool_call_id) ==
            NULL ||
        cJSON_AddStringToObject(item, "output", message->content) == NULL)
      return AGENT_ERR_OUT_OF_MEMORY;
  } else {
    const char *role = message->role == SYSTEM_MESSAGE ? "system"
                       : message->role == USER_MESSAGE ? "user"
                                                       : "assistant";
    if (cJSON_AddStringToObject(item, "role", role) == NULL)
      return AGENT_ERR_OUT_OF_MEMORY;
    if (message->refusal == NULL) {
      if (cJSON_AddStringToObject(item, "content", message->content) == NULL)
        return AGENT_ERR_OUT_OF_MEMORY;
    } else {
      if (cJSON_AddStringToObject(item, "type", "message") == NULL)
        return AGENT_ERR_OUT_OF_MEMORY;
      cJSON *content = cJSON_AddArrayToObject(item, "content");
      cJSON *part = cJSON_CreateObject();
      if (content == NULL || part == NULL ||
          !cJSON_AddItemToArray(content, part)) {
        cJSON_Delete(part);
        return AGENT_ERR_OUT_OF_MEMORY;
      }
      if (cJSON_AddStringToObject(part, "type", "refusal") == NULL ||
          cJSON_AddStringToObject(part, "refusal", message->refusal) == NULL)
        return AGENT_ERR_OUT_OF_MEMORY;
      if (message->content != NULL) {
        part = cJSON_CreateObject();
        if (part == NULL || !cJSON_AddItemToArray(content, part)) {
          cJSON_Delete(part);
          return AGENT_ERR_OUT_OF_MEMORY;
        }
        if (cJSON_AddStringToObject(part, "type", "output_text") == NULL ||
            cJSON_AddStringToObject(part, "text", message->content) == NULL)
          return AGENT_ERR_OUT_OF_MEMORY;
      }
    }
  }
  return AGENT_OK;
}

int openai_responses_encode_request(const LLMRequest *request,
                                    const char *model, char **out) {
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  int status = openai_validate_request(request, model);
  if (status != AGENT_OK)
    return status;
  status = AGENT_ERR_OUT_OF_MEMORY;
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return status;
  if (cJSON_AddStringToObject(root, "model", model) == NULL ||
      cJSON_AddBoolToObject(root, "store", 0) == NULL)
    goto done;
  cJSON *include = cJSON_AddArrayToObject(root, "include");
  cJSON *encrypted = cJSON_CreateString("reasoning.encrypted_content");
  if (include == NULL || encrypted == NULL ||
      !cJSON_AddItemToArray(include, encrypted)) {
    cJSON_Delete(encrypted);
    goto done;
  }
  cJSON *input = cJSON_AddArrayToObject(root, "input");
  if (input == NULL)
    goto done;
  const MessageArrary *messages = request->context->array;
  for (size_t i = 0; i < messages->count; ++i) {
    const Message *msg = &messages->items[i];
    if (msg->continuation.format == LLM_CONTINUATION_OPENAI_RESPONSES) {
      status = append_replay(input, msg->continuation.items_json);
      if (status != AGENT_OK)
        goto done;
      continue; // Raw items already contain the normalized text and calls.
    }
    if (msg->content != NULL || msg->refusal != NULL) {
      status = append_message(input, msg);
      if (status != AGENT_OK)
        goto done;
    }
    for (size_t j = 0; j < msg->tool_call_count; ++j) {
      const ToolCall *call = &msg->tool_calls[j];
      cJSON *item = cJSON_CreateObject();
      status = AGENT_ERR_OUT_OF_MEMORY;
      if (item == NULL || !cJSON_AddItemToArray(input, item)) {
        cJSON_Delete(item);
        goto done;
      }
      if (cJSON_AddStringToObject(item, "type", "function_call") == NULL ||
          cJSON_AddStringToObject(item, "call_id", call->call_id) == NULL ||
          cJSON_AddStringToObject(item, "name", call->tool_name) == NULL ||
          cJSON_AddStringToObject(item, "arguments", call->arguments_json) ==
              NULL)
        goto done;
    }
  }
  status = openai_add_tools(root, request, PROTOCOL_RESPONSES);
  if (status != AGENT_OK)
    goto done;
  *out = cJSON_PrintUnformatted(root);
  status = *out == NULL ? AGENT_ERR_OUT_OF_MEMORY : AGENT_OK;
done:
  cJSON_Delete(root);
  return status;
}

static int read_message(const cJSON *message, String *text, String *refusal,
                        int *has_text, int *has_refusal) {
  cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
  cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
  if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") ||
      !cJSON_IsArray(content))
    return AGENT_ERR_LLM_RESPONSE;
  cJSON *part = NULL;
  cJSON_ArrayForEach(part, content) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(part, "type");
    if (!cJSON_IsString(type))
      return AGENT_ERR_LLM_RESPONSE;
    cJSON *value;
    int status;
    if (strcmp(type->valuestring, "output_text") == 0) {
      value = cJSON_GetObjectItemCaseSensitive(part, "text");
      if (!cJSON_IsString(value))
        return AGENT_ERR_LLM_RESPONSE;
      *has_text = 1;
      status = string_append(text, value->valuestring);
    } else if (strcmp(type->valuestring, "refusal") == 0) {
      value = cJSON_GetObjectItemCaseSensitive(part, "refusal");
      if (!cJSON_IsString(value))
        return AGENT_ERR_LLM_RESPONSE;
      *has_refusal = 1;
      status = string_append(refusal, value->valuestring);
    } else {
      return AGENT_ERR_LLM_RESPONSE;
    }
    if (status != AGENT_OK)
      return status;
  }
  return AGENT_OK;
}

int openai_responses_decode_response(const char *json, LLMResponse **out,
                                     AgentError *error) {
  agent_error_clear(error);
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  if (json == NULL)
    return AGENT_ERR_NULL_POINTER;
  int status = AGENT_ERR_LLM_RESPONSE;
  cJSON *root = cJSON_ParseWithOpts(json, NULL, 1);
  LLMResponse *response = NULL;
  String text = {0}, refusal = {0};
  int has_text = 0, has_refusal = 0;
  cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "status");
  cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
  if (!cJSON_IsString(state))
    goto done;
  if (strcmp(state->valuestring, "failed") == 0) {
    openai_json_error(error, "OpenAI Responses failed",
                      cJSON_GetObjectItemCaseSensitive(root, "error"));
    goto done;
  }
  if ((strcmp(state->valuestring, "completed") &&
       strcmp(state->valuestring, "incomplete")) ||
      !cJSON_IsArray(output))
    goto done;
  response = calloc(1, sizeof(*response));
  if (response == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    goto done;
  }
  status = full_copy(&response->provider_status, state->valuestring);
  if (status != AGENT_OK)
    goto done;
  if (strcmp(state->valuestring, "incomplete") == 0) {
    cJSON *details =
        cJSON_GetObjectItemCaseSensitive(root, "incomplete_details");
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(details, "reason");
    status = AGENT_ERR_LLM_RESPONSE;
    if (!cJSON_IsString(reason))
      goto done;
    if (strcmp(reason->valuestring, "max_output_tokens") == 0)
      response->finish_reason = LLM_FINISH_LENGTH;
    else if (strcmp(reason->valuestring, "content_filter") == 0)
      response->finish_reason = LLM_FINISH_FILTERED;
    else {
      ERROR(error, reason->valuestring);
      goto done;
    }
    status = full_copy(&response->provider_reason, reason->valuestring);
    if (status != AGENT_OK)
      goto done;
  }
  size_t count = 0;
  cJSON *item = NULL;
  cJSON_ArrayForEach(item, output) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    status = AGENT_ERR_LLM_RESPONSE;
    if (!cJSON_IsString(type))
      goto done;
    if (strcmp(type->valuestring, "function_call") == 0)
      ++count;
  }
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
  size_t index = 0;
  cJSON_ArrayForEach(item, output) {
    const char *type =
        cJSON_GetObjectItemCaseSensitive(item, "type")->valuestring;
    if (strcmp(type, "message") == 0) {
      status = read_message(item, &text, &refusal, &has_text, &has_refusal);
      if (status != AGENT_OK)
        goto done;
    } else if (strcmp(type, "function_call") == 0) {
      cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      cJSON *arguments = cJSON_GetObjectItemCaseSensitive(item, "arguments");
      status = AGENT_ERR_LLM_RESPONSE;
      if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
          !cJSON_IsString(name) || name->valuestring[0] == '\0' ||
          !cJSON_IsString(arguments))
        goto done;
      ToolCall *call = &response->tool_calls[index++];
      status = full_copy(&call->call_id, id->valuestring);
      if (status != AGENT_OK)
        goto done;
      status = full_copy(&call->tool_name, name->valuestring);
      if (status != AGENT_OK)
        goto done;
      status = full_copy(&call->arguments_json, arguments->valuestring);
      if (status != AGENT_OK)
        goto done;
    } else if (strcmp(type, "reasoning") != 0) {
      ERROR(error, "Unsupported OpenAI Responses output item type");
      status = AGENT_ERR_TOOL_UNSUPPORTED;
      goto done;
    }
  }
  if (has_text) {
    status = full_copy(&response->content, string_c_str(&text));
    if (status != AGENT_OK)
      goto done;
  }
  if (has_refusal) {
    status = full_copy(&response->refusal, string_c_str(&refusal));
    if (status != AGENT_OK)
      goto done;
  }
  if (strcmp(state->valuestring, "completed") == 0) {
    status = AGENT_ERR_LLM_RESPONSE;
    if (!has_text && !has_refusal && !count)
      goto done;
    response->finish_reason = has_refusal ? LLM_FINISH_REFUSED
                              : count     ? LLM_FINISH_TOOL_CALLS
                                          : LLM_FINISH_STOP;
  }
  response->continuation.items_json = cJSON_PrintUnformatted(output);
  if (response->continuation.items_json == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
    goto done;
  }
  response->continuation.format = LLM_CONTINUATION_OPENAI_RESPONSES;
  *out = response;
  response = NULL;
  status = AGENT_OK;
done:
  llm_response_destroy(response);
  cJSON_Delete(root);
  string_destroy(&text);
  string_destroy(&refusal);
  if (status != AGENT_OK && (error == NULL || error->message[0] == '\0'))
    ERROR(error, "Could not parse OpenAI Responses response");
  return status;
}
