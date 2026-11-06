#ifndef TINY_AGENT_OPENAI_H
#define TINY_AGENT_OPENAI_H

#include "agent/llm.h"
#include "agent/providers/provider_config.h"

typedef enum {
  PROTOCOL_CHAT_COMPLETIONS = 0,
  PROTOCOL_RESPONSES
} OpenAIProtocol;

typedef struct {
  ProviderConfig common;
  OpenAIProtocol protocol;
  // Optional full URL override; otherwise append the protocol path to
  // common.base_url (e.g. https://api.openai.com/v1).
  const char *endpoint_url;
} OpenAIConfig;

typedef struct OpenAIProvider OpenAIProvider;

// Allocates a provider and copies all required, non-empty config fields.
// On failure, *provider is NULL. Do not pass an existing provider as output.
int openai_provider_init(LLMProvider **provider, const OpenAIConfig *config);
// Uses the protocol selected at initialization. On failure, *out is
// NULL and error, when provided, describes the failing stage.
int openai_provider_complete(OpenAIProvider *provider,
                             const LLMRequest *request, LLMResponse **out,
                             AgentError *error);
// Protocol codecs for non-streaming text and custom function tools.
// Responses uses store:false and replays continuation items from Context.
// The caller frees *out
// with free() for JSON and llm_response_destroy() for responses.
// On failure, *out is NULL. The model is required and must not be empty.
int openai_chat_encode_request(const LLMRequest *request, const char *model,
                               char **out);
int openai_chat_decode_response(const char *json, LLMResponse **out,
                                AgentError *error);
int openai_responses_encode_request(const LLMRequest *request,
                                    const char *model, char **out);
int openai_responses_decode_response(const char *json, LLMResponse **out,
                                     AgentError *error);

// Compatibility wrappers for the original Chat Completions codec names.
int llm_request_to_json(const LLMRequest *request, const char *model,
                        char **out);
int llm_json_to_response(const char *json, LLMResponse **out);

// Releases a provider returned by openai_provider_init, including its wrapper.
// If using llm_provider_destory directly, the caller must also free the
// wrapper.
int openai_provider_destory(LLMProvider *provider);

#endif // TINY_AGENT_OPENAI_H
