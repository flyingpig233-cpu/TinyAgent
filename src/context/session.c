#include "agent/session.h"
#include "agent/infra.h"
#include <stdlib.h>

int session_init(Session **session, const char *session_id, const char *system_prompt) {
    if (session == NULL) return AGENT_ERR_NULL_POINTER;
    *session = NULL;
    if (session_id == NULL || system_prompt == NULL) return AGENT_ERR_NULL_POINTER;

    Session *value = calloc(1, sizeof(*value));
    if (value == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    int status = full_copy(&value->session_id, session_id);
    if (status != AGENT_OK) goto fail;

    value->context = calloc(1, sizeof(*value->context));
    if (value->context == NULL) {
        status = AGENT_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    status = context_init(value->context);
    if (status != AGENT_OK) goto fail;

    Message message = {.content = system_prompt, .role = SYSTEM_MESSAGE};
    status = context_append(value->context, &message);
    if (status != AGENT_OK) goto fail;

    *session = value;
    return AGENT_OK;

fail:
    session_destory(value);
    return status;
}

int session_destory(Session *session) {
    if (session == NULL) return AGENT_OK;
    if (session->context != NULL) {
        int status = context_destory(session->context);
        if (status != AGENT_OK) return status;
        free(session->context);
    }
    free(session->session_id);
    free(session);
    return AGENT_OK;
}
