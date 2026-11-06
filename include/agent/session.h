#ifndef TINY_AGENT_SESSION_H
#define TINY_AGENT_SESSION_H

#include "agent/context.h"

typedef struct {
  char *session_id;
  Context *context;
} Session;

// Creates an owned session and copies session_id and system_prompt (both
// required). On failure *session is NULL. Destroy any existing session before
// reusing its pointer.
int session_init(Session **session, const char *session_id,
                 const char *system_prompt);

// Frees a session returned by session_init and all its resources; NULL is
// allowed. The caller must not use the pointer again after successful
// destruction.
int session_destory(Session *session);

#endif // TINY_AGENT_SESSION_H