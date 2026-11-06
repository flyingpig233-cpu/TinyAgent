#include "agent/providers/openai.h"
#include <stdlib.h>
#include <string.h>

int main(void) {
  OpenAIConfig config = {.common = {.api_key = "key",
                                    .base_url = "https://example.com",
                                    .model = "model"},
                         .protocol = PROTOCOL_CHAT_COMPLETIONS};
  LLMProvider *provider = NULL;
  if (openai_provider_init(NULL, &config) != AGENT_ERR_NULL_POINTER)
    return 1;
  if (openai_provider_init(&provider, NULL) != AGENT_ERR_NULL_POINTER ||
      provider != NULL)
    return 2;
  OpenAIConfig invalid = config;
  invalid.common.api_key = NULL;
  if (openai_provider_init(&provider, &invalid) != AGENT_ERR_NULL_POINTER ||
      provider != NULL)
    return 3;
  invalid = config;
  invalid.common.base_url = "";
  if (openai_provider_init(&provider, &invalid) != AGENT_ERR_INVAILD_ARGUMENT ||
      provider != NULL)
    return 4;
  invalid = config;
  invalid.protocol = (OpenAIProtocol)99;
  if (openai_provider_init(&provider, &invalid) != AGENT_ERR_INVAILD_ARGUMENT ||
      provider != NULL)
    return 11;
  invalid = config;
  invalid.endpoint_url = "";
  if (openai_provider_init(&provider, &invalid) != AGENT_ERR_INVAILD_ARGUMENT ||
      provider != NULL)
    return 12;
  OpenAIConfig responses = config;
  responses.protocol = PROTOCOL_RESPONSES;
  if (openai_provider_init(&provider, &responses) != AGENT_OK ||
      provider == NULL)
    return 13;
  if (openai_provider_destory(provider) != AGENT_OK)
    return 14;
  if (openai_provider_destory(NULL) != AGENT_ERR_NULL_POINTER)
    return 5;
  if (openai_provider_init(&provider, &config) != AGENT_OK || provider == NULL)
    return 6;
  LLMRequest request = {0};
  LLMResponse *response = NULL;
  AgentError error;
  if (llm_provider_complete(provider, &request, &response, &error) !=
          AGENT_ERR_NULL_POINTER ||
      response != NULL || strstr(error.message, "serialize") == NULL)
    return 7;
  if (openai_provider_destory(provider) != AGENT_OK)
    return 8;
  if (openai_provider_init(&provider, &config) != AGENT_OK)
    return 9;
  if (llm_provider_destory(provider, NULL) != AGENT_OK || provider->ctx != NULL)
    return 10;
  free(provider);
  return 0;
}
