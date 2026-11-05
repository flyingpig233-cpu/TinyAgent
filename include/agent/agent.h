#ifndef TINY_AGENT_AGENT_H
#define TINY_AGENT_AGENT_H

#include "agent/config.h"
#include "agent/llm.h"
#include "agent/infra.h"
#include "agent/session.h"

typedef struct Agent Agent;


int agent_create(const AgentConfig *config, LLMProvider *provider, Agent **out);

// output must be an initialized String; on success it is replaced.
// On failure output is unchanged, but accepted messages remain in context.
// error may be NULL. Agent borrows config and provider until destroyed.
int agent_run(Agent *agent, Session *session, const char *input, String *output, AgentError *error);

void agent_destroy(Agent *agent);

const char *agent_version(void);

#endif // TINY_AGENT_AGENT_H