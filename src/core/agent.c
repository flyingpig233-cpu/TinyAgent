#include "agent/agent.h"
#include "agent/tool.h"
#include <stdlib.h>
#include <string.h>

struct Agent {
  LLMProvider *llm_provider;
  ToolRegistry *registry;
  const AgentConfig *config;
  int approve_all; // Set after TOOL_DECISION_ALLOW_ALL; sticks for the session.
  int current_round; // 1-based round in progress, for delta callbacks.
};

static void agent_delta_bridge(void *userdata, const char *text) {
  Agent *agent = userdata;
  if (agent->config->on_delta != NULL && text != NULL)
    agent->config->on_delta(agent->config->userdata, agent->current_round,
                            text);
}

static int agent_cancelled(const Agent *agent) {
  return agent->config->is_cancelled != NULL &&
         agent->config->is_cancelled(agent->config->cancel_userdata) != 0;
}

static int run_error(AgentError *error, int status, const char *message) {
  if (error != NULL) {
    agent_error_set(error, message, __FILE__, __func__, __LINE__);
  }
  return status;
}

static void emit_event(const Agent *agent, const AgentEvent *event) {
  if (agent->config->on_event != NULL)
    agent->config->on_event(agent->config->userdata, event);
}

static int execute_tool_calls(Agent *agent, Session *session, int round,
                              LLMResponse *response, int *failed,
                              AgentError *error) {
  if (response->tool_call_count == 0 || response->tool_calls == NULL)
    return run_error(error, AGENT_ERR_LLM_RESPONSE,
                     "Tool call response contains no calls");

  *failed = 0;
  int cancelled = 0;
  ToolContext tool_ctx = {.is_cancelled = agent->config->is_cancelled,
                          .userdata = agent->config->cancel_userdata};
  for (size_t i = 0; i < response->tool_call_count; ++i) {
    if (!cancelled && agent_cancelled(agent))
      cancelled = 1;
    ToolCall call = response->tool_calls[i];
    if (call.call_id == NULL || call.call_id[0] == '\0' ||
        call.tool_name == NULL || call.tool_name[0] == '\0' ||
        call.arguments_json == NULL)
      return run_error(error, AGENT_ERR_LLM_RESPONSE, "Invalid tool call");

    AgentEvent call_event = {0};
    call_event.type = AGENT_EVENT_TOOL_CALL;
    call_event.round = round;
    call_event.tool_name = call.tool_name;
    call_event.call_id = call.call_id;
    call_event.arguments_json = call.arguments_json;
    emit_event(agent, &call_event);

    const ToolDefinition *tool =
        tool_registry_find(agent->registry, call.tool_name);
    ToolOutput *tool_output = NULL;
    AgentError tool_error = {0};
    const char *content = NULL;
    int is_error = 0;
    cJSON *args = NULL;
    if (cancelled) {
      content = "Error: tool call cancelled by host";
      is_error = 1;
    } else if (tool == NULL) {
      content = "Error: requested tool is not registered";
      is_error = 1;
    } else if ((args = cJSON_ParseWithOpts(call.arguments_json, NULL, 1)) ==
                   NULL ||
               !cJSON_IsObject(args)) {
      content = "Error: tool arguments must be a JSON object";
      is_error = 1;
    } else {
      ToolDecision decision = TOOL_DECISION_ALLOW;
      if (agent->approve_all) {
        decision = TOOL_DECISION_ALLOW;
      } else if (agent->config->on_tool_approval != NULL) {
        decision = agent->config->on_tool_approval(
            agent->config->approval_userdata, call.tool_name,
            call.arguments_json, tool->mutating);
        if (decision == TOOL_DECISION_ALLOW_ALL) {
          agent->approve_all = 1;
          decision = TOOL_DECISION_ALLOW;
        }
      }
      if (decision == TOOL_DECISION_DENY) {
        content = "Error: tool call denied by host";
        is_error = 1;
      } else {
        int status = tool->fn(&tool_ctx, args, &tool_output, &tool_error);
        if (status == AGENT_ERR_CANCELLED) {
          cancelled = 1;
          content = tool_error.message[0]
                        ? tool_error.message
                        : "Error: tool call cancelled by host";
          is_error = 1;
        } else if (status != AGENT_OK) {
          content = tool_error.message[0] ? tool_error.message
                                          : "Error: tool execution failed";
          is_error = 1;
        } else if (tool_output == NULL || tool_output->content == NULL) {
          content = "Error: tool returned no output";
          is_error = 1;
        } else {
          content = tool_output->content;
          is_error = tool_output->is_error != 0;
        }
      }
    }
    cJSON_Delete(args);
    *failed |= is_error;

    AgentEvent result_event = {0};
    result_event.type = AGENT_EVENT_TOOL_RESULT;
    result_event.round = round;
    result_event.tool_name = call.tool_name;
    result_event.call_id = call.call_id;
    result_event.tool_output = content;
    result_event.tool_is_error = is_error;
    emit_event(agent, &result_event);

    Message result = {.role = TOOL_MESSAGE,
                      .content = content,
                      .tool_call_id = call.call_id,
                      .tool_name = call.tool_name};
    int status = context_append(session->context, &result);
    if (tool_output != NULL) {
      free(tool_output->content);
      free(tool_output);
    }
    if (status != AGENT_OK)
      return run_error(error, status, "Could not append tool result");
  }
  return cancelled ? run_error(error, AGENT_ERR_CANCELLED, "Run cancelled")
                   : AGENT_OK;
}

static int run_rounds(Agent *agent, Session *session, String *output,
                      AgentError *error) {
  int failed_rounds = 0;
  int retry_limit = agent->config->max_tool_retries > 0
                        ? agent->config->max_tool_retries
                        : CONFIG_DEFAULT_MAX_TOOL_RETRIES;
  for (int i = 0; i < agent->config->max_round; ++i) {
    if (agent_cancelled(agent))
      return run_error(error, AGENT_ERR_CANCELLED, "Run cancelled");
    int round = i + 1;
    agent->current_round = round;
    LLMRequest request = {.context = session->context,
                          .tools = agent->registry->tools,
                          .tool_count = agent->registry->count,
                          .stream = agent->config->stream,
                          .on_delta = agent_delta_bridge,
                          .delta_userdata = agent,
                          .is_cancelled = agent->config->is_cancelled,
                          .cancel_userdata = agent->config->cancel_userdata};
    LLMResponse *response = NULL;
    AgentError provider_error = {0};
    int status = llm_provider_complete(agent->llm_provider, &request, &response,
                                       &provider_error);
    if (status != AGENT_OK) {
      llm_response_destroy(response);
      if (provider_error.message[0] == '\0')
        return run_error(error, status, "LLM provider failed");
      if (error != NULL)
        *error = provider_error;
      return status;
    }
    if (response == NULL)
      return run_error(error, AGENT_ERR_LLM_RESPONSE,
                       "Provider returned no response");

    status = context_add_assistant_response(session->context, response, NULL);
    if (status != AGENT_OK) {
      llm_response_destroy(response);
      return run_error(error, status, "Could not append assistant response");
    }

    AgentEvent response_event = {0};
    response_event.type = AGENT_EVENT_LLM_RESPONSE;
    response_event.round = round;
    response_event.content = response->content;
    response_event.finish_reason = response->finish_reason;
    response_event.tool_call_count = response->tool_call_count;
    emit_event(agent, &response_event);

    switch (response->finish_reason) {
    case LLM_FINISH_STOP:
      if (response->tool_call_count != 0) {
        status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                           "Unexpected tool calls in final response");
      } else {
        status = string_assign(
            output, response->content != NULL ? response->content : "");
        if (status != AGENT_OK)
          run_error(error, status, "Could not write output");
      }
      llm_response_destroy(response);
      return status;
    case LLM_FINISH_TOOL_CALLS: {
      int failed = 0;
      status =
          execute_tool_calls(agent, session, round, response, &failed, error);
      llm_response_destroy(response);
      if (status != AGENT_OK)
        return status;
      failed_rounds = failed ? failed_rounds + 1 : 0;
      if (failed_rounds > retry_limit)
        return run_error(error, AGENT_ERR_TOOL_RETRIES,
                         "Maximum tool retries reached");
      continue;
    }
    case LLM_FINISH_LENGTH:
      llm_response_destroy(response);
      return run_error(error, AGENT_ERR_LLM_TRUNCATED,
                       "Model response was truncated");
    case LLM_FINISH_FILTERED:
      llm_response_destroy(response);
      return run_error(error, AGENT_ERR_LLM_FILTERED,
                       "Model response was filtered");
    case LLM_FINISH_REFUSED:
      status =
          run_error(error, AGENT_ERR_LLM_REFUSED,
                    response->refusal != NULL ? response->refusal
                                              : "Model refused the request");
      llm_response_destroy(response);
      return status;
    default:
      llm_response_destroy(response);
      return run_error(error, AGENT_ERR_LLM_RESPONSE,
                       "Unknown model finish reason");
    }
  }
  return run_error(error, AGENT_ERR_MAX_ROUNDS, "Maximum rounds reached");
}

int agent_create(const AgentConfig *config, LLMProvider *provider,
                 ToolRegistry *registry, Agent **out) {
  if (out == NULL)
    return AGENT_ERR_NULL_POINTER;
  *out = NULL;
  if (config == NULL || provider == NULL || registry == NULL)
    return AGENT_ERR_NULL_POINTER;
  Agent *agent = malloc(sizeof(*agent));
  if (agent == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  agent->llm_provider = provider;
  agent->registry = registry;
  agent->config = config;
  agent->approve_all = 0;
  agent->current_round = 0;
  *out = agent;
  return AGENT_OK;
}

void agent_destroy(Agent *agent) { free(agent); }

int agent_run(Agent *agent, Session *session, const char *input, String *output,
              AgentError *error) {
  agent_error_clear(error);
  if (agent == NULL || session == NULL || input == NULL || output == NULL ||
      agent->config == NULL || agent->llm_provider == NULL ||
      session->context == NULL || agent->registry == NULL)
    return run_error(error, AGENT_ERR_NULL_POINTER,
                     "Missing agent, session, input, output, or dependency");
  if (agent->config->max_round <= 0)
    return run_error(error, AGENT_ERR_INVAILD_ARGUMENT,
                     "max_round must be positive");
  if (agent_cancelled(agent))
    return run_error(error, AGENT_ERR_CANCELLED, "Run cancelled");

  Message msg = {.content = input, .role = USER_MESSAGE};
  int status = context_append(session->context, &msg);
  if (status != AGENT_OK)
    return run_error(error, status, "Could not append user message");

  AgentEvent event = {0};
  event.type = AGENT_EVENT_RUN_START;
  event.content = input;
  emit_event(agent, &event);

  status = run_rounds(agent, session, output, error);

  memset(&event, 0, sizeof(event));
  event.type = AGENT_EVENT_RUN_END;
  event.status = status;
  event.content = status == AGENT_OK ? string_c_str(output) : NULL;
  event.error_message =
      (error != NULL && error->message[0] != '\0') ? error->message : NULL;
  emit_event(agent, &event);
  return status;
}

const char *agent_version(void) { return "1.0.0"; }
