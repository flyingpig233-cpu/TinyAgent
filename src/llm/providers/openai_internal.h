#ifndef TINY_AGENT_OPENAI_INTERNAL_H
#define TINY_AGENT_OPENAI_INTERNAL_H

#include "agent/providers/openai.h"
#include <cjson/cJSON.h>

int openai_validate_request(const LLMRequest *request, const char *model);
int openai_add_tools(cJSON *root, const LLMRequest *request,
                     OpenAIProtocol protocol);
void openai_json_error(AgentError *error, const char *stage,
                       const cJSON *detail);

#endif
