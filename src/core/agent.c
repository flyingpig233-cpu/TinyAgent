#include "agent/agent.h"
#include "agent/tool.h"
#include <stdlib.h>

struct Agent {
  LLMProvider *llm_provider;
  ToolRegistry *registry;
  const AgentConfig *config;
};

static int run_error(AgentError *error, int status, const char *message) {
  if (error != NULL) {
    agent_error_set(error, message, __FILE__, __func__, __LINE__);
  }
  return status;
}

int agent_create(const AgentConfig *config, LLMProvider *provider, ToolRegistry *registry,
                 Agent **out) {
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

  Message msg = {.content = input, .role = USER_MESSAGE};
  int status = context_append(session->context, &msg);
  if (status != AGENT_OK)
    return run_error(error, status, "Could not append user message");
  LLMRequest request = {.context = session->context,
                        .tools = agent->registry->tools,
                        .tool_count = agent->registry->count};
  for (int i = 0; i < agent->config->max_round; ++i) {
    LLMResponse *response = NULL;
    AgentError provider_error = {0};
    status = llm_provider_complete(agent->llm_provider, &request, &response,
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
    case LLM_FINISH_TOOL_CALLS:
      if (response->tool_call_count == 0 || response->tool_calls == NULL) {
        status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                           "Tool call response contains no calls");
        break;
      }
      for (size_t i = 0; i < response->tool_call_count; ++i) {
        ToolCall call = response->tool_calls[i];
        if (call.call_id == NULL || call.call_id[0] == '\0' ||
            call.tool_name == NULL || call.tool_name[0] == '\0' ||
            call.arguments_json == NULL) {
          status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                             "Invalid tool call");
          break;
        }
        const ToolDefinition *tool = tool_registry_find(agent->registry, call.tool_name);
        if (tool == NULL) {
          status = run_error(error, AGENT_ERR_TOOL_UNSUPPORTED,
                             "Requested tool is not registered");
          break;
        }
        cJSON *args = cJSON_ParseWithOpts(call.arguments_json, NULL, 1);
        if (!cJSON_IsObject(args)) {
          cJSON_Delete(args);
          status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                             "Invalid tool call arguments");
          break;
        }
        ToolOutput *tool_output = NULL;
        AgentError tool_error = {0};
        status = tool->fn(NULL, args, &tool_output, &tool_error);
        cJSON_Delete(args);
        if (status == AGENT_OK &&
            (tool_output == NULL || tool_output->content == NULL)) {
          status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                             "Tool returned no output");
        } else if (status == AGENT_OK) {
          Message result = {.role = TOOL_MESSAGE,
                            .content = tool_output->content,
                            .tool_call_id = call.call_id,
                            .tool_name = call.tool_name};
          status = context_append(session->context, &result);
          if (status != AGENT_OK)
            run_error(error, status, "Could not append tool result");
        } else if (tool_error.message[0] != '\0') {
          if (error != NULL)
            *error = tool_error;
        } else {
          run_error(error, status, "Tool execution failed");
        }
        if (tool_output != NULL) {
          free(tool_output->content);
          free(tool_output);
        }
        if (status != AGENT_OK)
          break;
      }
      if (status == AGENT_OK) {
        llm_response_destroy(response);
        continue;
      }
      break;
    case LLM_FINISH_LENGTH:
      status = run_error(error, AGENT_ERR_LLM_TRUNCATED,
                         "Model response was truncated");
      break;
    case LLM_FINISH_FILTERED:
      status = run_error(error, AGENT_ERR_LLM_FILTERED,
                         "Model response was filtered");
      break;
    case LLM_FINISH_REFUSED:
      status =
          run_error(error, AGENT_ERR_LLM_REFUSED,
                    response->refusal != NULL ? response->refusal
                                              : "Model refused the request");
      break;
    default:
      status = run_error(error, AGENT_ERR_LLM_RESPONSE,
                         "Unknown model finish reason");
      break;
    }
    llm_response_destroy(response);
    return status;
  }
  return run_error(error, AGENT_ERR_MAX_ROUNDS, "Maximum rounds reached");
}

const char *agent_version(void) { return "1.0.0"; }
