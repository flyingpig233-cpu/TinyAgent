#include "agent/config.h"

AgentConfig config_default(void) {
  AgentConfig config = {.max_round = CONFIG_DEFAULT_MAX_ROUND};
  return config;
}