#include "agent/session.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expr);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  Session *session = NULL;
  CHECK(session_init(NULL, "id", "prompt") == AGENT_ERR_NULL_POINTER);
  CHECK(session_init(&session, NULL, "prompt") == AGENT_ERR_NULL_POINTER);
  CHECK(session == NULL);
  CHECK(session_init(&session, "id", NULL) == AGENT_ERR_NULL_POINTER);
  CHECK(session == NULL);
  CHECK(session_destory(NULL) == AGENT_OK);

  char id[] = "session-1";
  char prompt[] = "system prompt";
  CHECK(session_init(&session, id, prompt) == AGENT_OK);
  id[0] = 'X';
  prompt[0] = 'X';
  CHECK(strcmp(session->session_id, "session-1") == 0);
  CHECK(session->context->array->count == 1);
  CHECK(session->context->array->items[0].role == SYSTEM_MESSAGE);
  CHECK(strcmp(session->context->array->items[0].content, "system prompt") ==
        0);
  Message message = {.role = USER_MESSAGE, .content = "hello"};
  CHECK(context_append(session->context, &message) == AGENT_OK);
  CHECK(session_destory(session) == AGENT_OK);
  session = NULL;

  CHECK(session_init(&session, "", "") == AGENT_OK);
  CHECK(session->context->array->count == 1);
  CHECK(strcmp(session->context->array->items[0].content, "") == 0);
  CHECK(session_destory(session) == AGENT_OK);
  return 0;
}
