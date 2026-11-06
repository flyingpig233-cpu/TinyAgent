#ifndef TINY_AGENT_MOCK_LLM_H
#define TINY_AGENT_MOCK_LLM_H

#include "agent/llm.h"

// Copies the responses. Each complete call returns the next response; once
// exhausted, complete returns AGENT_ERR_INVAILD_ARGUMENT. Destroy the provider
// with llm_provider_destory. A request may be NULL when using the mock.
int mock_llm_init(LLMProvider *provider, const LLMResponse *responses,
                  size_t count);

#endif
