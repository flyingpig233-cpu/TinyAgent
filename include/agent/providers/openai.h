#ifndef TINY_AGENT_OPENAI_H
#define TINY_AGENT_OPENAI_H

#include "agent/llm.h"
#include "agent/providers/provider_config.h"

typedef struct OpenAIProvider OpenAIProvider;

// Allocates a provider and copies all required, non-empty config fields.
// On failure, *provider is NULL. Do not pass an existing provider as output.
int openai_provider_init(LLMProvider **provider, ProviderConfig *config);
// Sends to config.base_url as the complete endpoint URL. On failure, *out is
// NULL and error, when provided, describes the failing stage.
int openai_provider_complete(OpenAIProvider *provider,
                             const LLMRequest *request, LLMResponse **out,
                             AgentError *error);
// Convert Chat Completions messages and first choice. The caller frees *out
// with free() for JSON and llm_response_destroy() for responses.
// On failure, *out is NULL. The model is required and must not be empty.
int llm_request_to_json(const LLMRequest *request, const char *model,
                        char **out);
int llm_json_to_response(const char *json, LLMResponse **out);

// Releases a provider returned by openai_provider_init, including its wrapper.
// If using llm_provider_destory directly, the caller must also free the
// wrapper.
int openai_provider_destory(LLMProvider *provider);

#endif // TINY_AGENT_OPENAI_H