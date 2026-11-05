#ifndef TINY_AGENT_ERROR_H
#define TINY_AGENT_ERROR_H

#include "agent/status.h"

typedef struct {
    
    char message[256];
    
    const char *file;
    const char *function;

    int line;
} AgentError;


// Both functions accept NULL error. A NULL message is stored as an empty string.
// file and function are borrowed and must outlive the error.
void agent_error_clear(AgentError *error);

void agent_error_set(AgentError *error, const char *message, const char *file, const char *function, int line);

#define ERROR(error, message) (agent_error_set((error), (message), __FILE__, __func__, __LINE__))
#endif // TINY_AGENT_ERROR_H