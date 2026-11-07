#ifndef TINY_AGENT_CONFIG_H
#define TINY_AGENT_CONFIG_H

enum { CONFIG_DEFAULT_MAX_ROUND = 40 };

typedef struct {
  int max_round;
} AgentConfig;

AgentConfig config_default();

#endif // TINY_AGENT_CONFIG_H