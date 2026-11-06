#include "agent/providers/openai.h"
#include "agent/context.h"
#include "agent/http_client.h"
#include "agent/infra.h"
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OpenAIProvider {
  char *api_key;
  char *base_url;
  char *model;
  HttpClient *client;
};

static void openai_free_context(OpenAIProvider *provider) {
  if (provider == NULL)
    return;
  if (provider->client != NULL)
    http_client_destroy(provider->client);
  free(provider->api_key);
  free(provider->base_url);
  free(provider->model);
  free(provider);
}

static int openai_destroy_context(void *ctx) {
  if (ctx == NULL)
    return AGENT_ERR_NULL_POINTER;
  openai_free_context(ctx);
  return AGENT_OK;
}

static int openai_complete_callback(void *ctx, const LLMRequest *request,
                                    LLMResponse **out, AgentError *error) {
  return openai_provider_complete(ctx, request, out, error);
}

int openai_provider_init(LLMProvider **provider, ProviderConfig *config) {
  if (provider == NULL)
    return AGENT_ERR_NULL_POINTER;
  *provider = NULL;
  if (config == NULL || config->api_key == NULL || config->base_url == NULL ||
      config->model == NULL)
    return AGENT_ERR_NULL_POINTER;
  if (config->api_key[0] == '\0' || config->base_url[0] == '\0' ||
      config->model[0] == '\0')
    return AGENT_ERR_INVAILD_ARGUMENT;

  OpenAIProvider *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  LLMProvider *value = calloc(1, sizeof(*value));
  if (value == NULL) {
    free(ctx);
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  int status = full_copy(&ctx->api_key, config->api_key);
  if (status != AGENT_OK)
    goto fail;
  status = full_copy(&ctx->base_url, config->base_url);
  if (status != AGENT_OK)
    goto fail;
  status = full_copy(&ctx->model, config->model);
  if (status != AGENT_OK)
    goto fail;
  status = http_client_init(&ctx->client);
  if (status != AGENT_OK)
    goto fail;
  *value = (LLMProvider){
      .ops = {openai_complete_callback, openai_destroy_context}, .ctx = ctx};
  *provider = value;
  return AGENT_OK;

fail:
  openai_free_context(ctx);
  free(value);
  return status;
}

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

int llm_request_to_json(const LLMRequest *request, const char *model,
                        char **out) {
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  if (request == NULL || model == NULL || request->context == NULL ||
      request->context->array == NULL)
    return AGENT_ERR_NULL_POINTER;
  const MessageArrary *messages = request->context->array;
  if (model[0] == '\0' || (messages->count && messages->items == NULL))
    return AGENT_ERR_INVAILD_ARGUMENT;
  int status = AGENT_ERR_OUT_OF_MEMORY;
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
    return status;
  if (cJSON_AddStringToObject(root, "model", model) == NULL)
    goto done;
  cJSON *array = cJSON_AddArrayToObject(root, "messages");
  if (array == NULL)
    goto done;
  for (size_t i = 0; i < messages->count; ++i) {
    const Message *msg = &messages->items[i];
    const char *role = message_role(msg->role);
    if (role == NULL ||
        (msg->tool_call_count &&
         (msg->role != ASSISTANT_MESSAGE || msg->tool_calls == NULL)) ||
        (msg->content == NULL &&
         (msg->role != ASSISTANT_MESSAGE || !msg->tool_call_count)) ||
        (msg->role == TOOL_MESSAGE && msg->tool_call_id == NULL)) {
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
        if (call->call_id == NULL || call->tool_name == NULL ||
            call->arguments_json == NULL) {
          status = AGENT_ERR_INVAILD_ARGUMENT;
          goto done;
        }
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
  *out = cJSON_PrintUnformatted(root);
  if (*out != NULL)
    status = AGENT_OK;
done:
  cJSON_Delete(root);
  return status;
}

int llm_json_to_response(const char *str, LLMResponse **out) {
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  if (str == NULL)
    return AGENT_ERR_NULL_POINTER;
  const char *end = NULL;
  cJSON *root = cJSON_ParseWithOpts(str, &end, 1);
  if (root == NULL)
    return AGENT_ERR_LLM_RESPONSE;
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
  if ((!cJSON_IsString(content) && !cJSON_IsNull(content)) ||
      (calls != NULL && !cJSON_IsNull(calls) && !cJSON_IsArray(calls)))
    goto done;
  response = calloc(1, sizeof(*response));
  if (response == NULL) {
    status = AGENT_ERR_OUT_OF_MEMORY;
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
      if (!cJSON_IsString(id) || !cJSON_IsString(type) ||
          strcmp(type->valuestring, "function") || !cJSON_IsString(name) ||
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
  status = AGENT_OK;
  *out = response;
  response = NULL;
done:
  llm_response_destroy(response);
  cJSON_Delete(root);
  return status;
}

int openai_provider_complete(OpenAIProvider *provider,
                             const LLMRequest *request, LLMResponse **out,
                             AgentError *error) {
  agent_error_clear(error);
  if (out != NULL)
    *out = NULL;
  if (provider == NULL || request == NULL || out == NULL) {
    ERROR(error, "OpenAI provider, request and output are required");
    return AGENT_ERR_NULL_POINTER;
  }
  if (strchr(provider->api_key, '\r') != NULL ||
      strchr(provider->api_key, '\n') != NULL) {
    ERROR(error, "OpenAI API key contains an invalid header character");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  char *req_json = NULL;
  HttpResponse response = {0};
  String authorization = {0};
  int status = llm_request_to_json(request, provider->model, &req_json);
  if (status != AGENT_OK) {
    ERROR(error, "Could not serialize OpenAI request");
    goto done;
  }
  status = string_append(&authorization, "Authorization: Bearer ");
  if (status == AGENT_OK)
    status = string_append(&authorization, provider->api_key);
  if (status != AGENT_OK) {
    ERROR(error, "Could not allocate OpenAI authorization header");
    goto done;
  }
  const char *headers[] = {string_c_str(&authorization),
                           "Content-Type: application/json"};
  HttpRequest http_request = {.method = "POST",
                              .url = provider->base_url,
                              .headers = headers,
                              .header_count = 2,
                              .body = req_json,
                              .body_len = strlen(req_json)};
  status =
      http_client_request(provider->client, &http_request, &response, error);
  if (status != AGENT_OK)
    goto done;
  if (response.status_code < 200 || response.status_code >= 300) {
    char message[96];
    snprintf(message, sizeof(message),
             "OpenAI request failed with HTTP status %ld",
             response.status_code);
    ERROR(error, message);
    status = AGENT_ERR_HTTP;
    goto done;
  }
  if (memchr(response.body, '\0', response.body_len) != NULL) {
    ERROR(error, "OpenAI response contains an embedded NUL byte");
    status = AGENT_ERR_LLM_RESPONSE;
    goto done;
  }
  status = llm_json_to_response(response.body, out);
  if (status != AGENT_OK)
    ERROR(error, "Could not parse OpenAI response");
done:
  free(req_json);
  string_destroy(&authorization);
  http_response_free(&response);
  return status;
}

int openai_provider_destory(LLMProvider *provider) {
  if (provider == NULL)
    return AGENT_ERR_NULL_POINTER;
  int status = llm_provider_destory(provider, NULL);
  if (status != AGENT_OK)
    return status;
  free(provider);
  return AGENT_OK;
}
