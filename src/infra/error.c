#include "agent/error.h"
#include <stdio.h>

void agent_error_clear(AgentError *error) {
  if (error != NULL)
    *error = (AgentError){0};
}

void agent_error_set(AgentError *error, const char *message, const char *file,
                     const char *function, int line) {
  if (error == NULL)
    return;
  AgentError value = {.file = file, .function = function, .line = line};
  if (message != NULL)
    snprintf(value.message, sizeof(value.message), "%s", message);
  *error = value;
}
