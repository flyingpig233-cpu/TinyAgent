#ifndef TINY_AGENT_OPENAI_INTERNAL_H
#define TINY_AGENT_OPENAI_INTERNAL_H

#include "agent/http_client.h"
#include "agent/providers/openai.h"
#include <cjson/cJSON.h>

int openai_validate_request(const LLMRequest *request, const char *model);
int openai_add_tools(cJSON *root, const LLMRequest *request,
                     OpenAIProtocol protocol);
void openai_json_error(AgentError *error, const char *stage,
                       const cJSON *detail);

typedef void (*OpenAIDeltaFn)(void *userdata, const char *text);

// Performs a streaming (SSE) request against http_request and assembles a
// complete LLMResponse. Text deltas are forwarded to on_delta as they arrive.
// Both protocols are supported; HTTP errors are reported through *error.
int openai_stream_complete(HttpClient *client, const HttpRequest *http_request,
                           OpenAIProtocol protocol, OpenAIDeltaFn on_delta,
                           void *delta_userdata, LLMResponse **out,
                           AgentError *error);

#endif
