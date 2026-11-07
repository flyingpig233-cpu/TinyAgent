#include "agent/providers/openai.h"
#include "agent/context.h"
#include "agent/http_client.h"
#include "agent/infra.h"
#include "openai_internal.h"
#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct OpenAIProvider {
  OpenAIConfig config;
  HttpClient *client;
  char *endpoint_url;
};

static void openai_free_context(OpenAIProvider *provider) {
  if (provider == NULL)
    return;
  if (provider->client != NULL)
    http_client_destroy(provider->client);
  free((void *)provider->config.common.api_key);
  free((void *)provider->config.common.base_url);
  free((void *)provider->config.common.model);
  free(provider->endpoint_url);
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

int openai_provider_init(LLMProvider **provider, const OpenAIConfig *config) {
  if (provider == NULL)
    return AGENT_ERR_NULL_POINTER;
  *provider = NULL;
  if (config == NULL || config->common.api_key == NULL ||
      config->common.base_url == NULL || config->common.model == NULL)
    return AGENT_ERR_NULL_POINTER;
  if (config->common.api_key[0] == '\0' || config->common.base_url[0] == '\0' ||
      config->common.model[0] == '\0' || config->common.connect_timeout_ms < 0 ||
      config->common.timeout_ms < 0)
    return AGENT_ERR_INVAILD_ARGUMENT;

  if ((config->protocol != PROTOCOL_CHAT_COMPLETIONS &&
       config->protocol != PROTOCOL_RESPONSES) ||
      (config->endpoint_url != NULL && config->endpoint_url[0] == '\0'))
    return AGENT_ERR_INVAILD_ARGUMENT;

  OpenAIProvider *ctx = calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  LLMProvider *value = calloc(1, sizeof(*value));
  if (value == NULL) {
    free(ctx);
    return AGENT_ERR_OUT_OF_MEMORY;
  }

  ctx->config.protocol = config->protocol;
  ctx->config.common.connect_timeout_ms = config->common.connect_timeout_ms;
  ctx->config.common.timeout_ms = config->common.timeout_ms;
  char *copy = NULL;
  int status = full_copy(&copy, config->common.api_key);
  if (status != AGENT_OK)
    goto fail;
  ctx->config.common.api_key = copy;
  status = full_copy(&copy, config->common.base_url);
  if (status != AGENT_OK)
    goto fail;
  ctx->config.common.base_url = copy;
  status = full_copy(&copy, config->common.model);
  if (status != AGENT_OK)
    goto fail;
  ctx->config.common.model = copy;
  if (config->endpoint_url != NULL) {
    status = full_copy(&ctx->endpoint_url, config->endpoint_url);
  } else {
    String endpoint = {0};
    status = string_assign(&endpoint, config->common.base_url);
    if (status == AGENT_OK) {
      while (endpoint.length && endpoint.data[endpoint.length - 1] == '/')
        endpoint.data[--endpoint.length] = '\0';
      status = string_append(&endpoint, config->protocol == PROTOCOL_RESPONSES
                                            ? "/responses"
                                            : "/chat/completions");
    }
    if (status == AGENT_OK) {
      ctx->endpoint_url = endpoint.data;
      endpoint.data = NULL;
    }
    string_destroy(&endpoint);
  }
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
  if (strchr(provider->config.common.api_key, '\r') != NULL ||
      strchr(provider->config.common.api_key, '\n') != NULL) {
    ERROR(error, "OpenAI API key contains an invalid header character");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  char *req_json = NULL;
  HttpResponse response = {0};
  String authorization = {0};
  int status = provider->config.protocol == PROTOCOL_RESPONSES
                   ? openai_responses_encode_request(
                         request, provider->config.common.model, &req_json)
                   : openai_chat_encode_request(
                         request, provider->config.common.model, &req_json);
  if (status != AGENT_OK) {
    ERROR(error, "Could not serialize OpenAI request");
    goto done;
  }
  status = string_append(&authorization, "Authorization: Bearer ");
  if (status == AGENT_OK)
    status = string_append(&authorization, provider->config.common.api_key);
  if (status != AGENT_OK) {
    ERROR(error, "Could not allocate OpenAI authorization header");
    goto done;
  }
  const char *headers[] = {string_c_str(&authorization),
                           "Content-Type: application/json"};
  HttpRequest http_request = {.method = "POST",
                              .url = provider->endpoint_url,
                              .headers = headers,
                              .header_count = 2,
                              .body = req_json,
                              .body_len = strlen(req_json),
                              .connect_timeout_ms = provider->config.common.connect_timeout_ms,
                              .timeout_ms = provider->config.common.timeout_ms};
  if (request->stream) {
    status = openai_stream_complete(
        provider->client, &http_request, provider->config.protocol,
        request->on_delta, request->delta_userdata, out, error);
    goto done;
  }
  status =
      http_client_request(provider->client, &http_request, &response, error);
  if (status != AGENT_OK)
    goto done;
  if (response.status_code < 200 || response.status_code >= 300) {
    char message[96];
    snprintf(message, sizeof(message),
             "OpenAI request failed with HTTP status %ld",
             response.status_code);
    cJSON *body = cJSON_ParseWithLength(response.body, response.body_len);
    openai_json_error(error, message,
                      cJSON_GetObjectItemCaseSensitive(body, "error"));
    cJSON_Delete(body);
    status = AGENT_ERR_HTTP;
    goto done;
  }
  if (memchr(response.body, '\0', response.body_len) != NULL) {
    ERROR(error, "OpenAI response contains an embedded NUL byte");
    status = AGENT_ERR_LLM_RESPONSE;
    goto done;
  }
  status = provider->config.protocol == PROTOCOL_RESPONSES
               ? openai_responses_decode_response(response.body, out, error)
               : openai_chat_decode_response(response.body, out, error);
  if (status != AGENT_OK && (error == NULL || error->message[0] == '\0'))
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
