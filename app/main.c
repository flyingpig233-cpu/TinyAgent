#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent.h"
#include "agent/providers/openai.h"
#include "agent/tools/file_system/list_dir.h"
#include "agent/tools/file_system/read_file.h"
#include "agent/tools/file_system/search.h"
#include "agent/tools/file_system/write_file.h"
#include "agent/tools/file_system/edit_file.h"
#include "agent/tools/run_command.h"

int main(void) {
  Agent *agent = NULL;
  ToolRegistry *registry = NULL;
  Session *session = NULL;
  LLMProvider *provider = NULL;
  String output = {0};
  AgentError error = {0};
  AgentConfig config = config_default();
  int status = AGENT_OK;
  int exit_code = EXIT_SUCCESS;
  const char *stage = "provider initialization";

  const char *api_key = getenv("DEEPSEEK_API_KEY");
  if (api_key == NULL || api_key[0] == '\0') {
    fprintf(stderr, "DEEPSEEK_API_KEY is not set or is empty.\n");
    return EXIT_FAILURE;
  }
  OpenAIConfig provider_config = {
      .common = {.api_key = api_key,
                 .base_url = "https://api.deepseek.com/",
                 .model = "deepseek-flash"},
      .protocol = PROTOCOL_RESPONSES};
  status = openai_provider_init(&provider, &provider_config);
  if (status != AGENT_OK)
    goto done;

  stage = "tool registry initialization";
  status = tool_registry_init(&registry);
  if (status != AGENT_OK)
    goto done;
  stage = "tool registration";
  status = list_dir_register(registry, &error);
  if (status != AGENT_OK)
    goto done;
  status = read_file_register(registry, &error);
  if (status != AGENT_OK)
    goto done;
  status = search_register(registry, &error);
  if (status != AGENT_OK)
    goto done;
  status = write_file_register(registry, &error);
  if (status != AGENT_OK)
    goto done;
  status = edit_file_register(registry, &error);
  if (status != AGENT_OK)
    goto done;
  status = run_command_register(registry, &error);
  if (status != AGENT_OK)
    goto done;

  stage = "session initialization";
  status =
      session_init(&session, "Test Session", "You are a helpful assistant");
  if (status != AGENT_OK)
    goto done;
  stage = "agent initialization";
  status = agent_create(&config, provider, registry, &agent);
  if (status != AGENT_OK)
    goto done;

  char user_input[512];
  printf("init successfully\n");
  for (;;) {
    if (printf("Input: ") < 0 || fflush(stdout) == EOF) {
      fprintf(stderr, "Failed to write prompt.\n");
      exit_code = EXIT_FAILURE;
      break;
    }
    if (fgets(user_input, sizeof(user_input), stdin) == NULL) {
      if (ferror(stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        exit_code = EXIT_FAILURE;
      }
      break;
    }
    if (strchr(user_input, '\n') == NULL && !feof(stdin)) {
      int ch = getchar();
      if (ch != '\n' && ch != EOF) {
        while ((ch = getchar()) != '\n' && ch != EOF) {}
        fprintf(stderr, "Input is too long.\n");
        if (ferror(stdin)) {
          exit_code = EXIT_FAILURE;
          break;
        }
        continue;
      }
      if (ch == EOF && ferror(stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        exit_code = EXIT_FAILURE;
        break;
      }
    }
    user_input[strcspn(user_input, "\r\n")] = '\0';
    if (strcmp(user_input, "/exit") == 0)
      break;
    int result = agent_run(agent, session, user_input, &output, &error);
    if (result != AGENT_OK) {
      fprintf(stderr, "Agent run failed: %s (error code: %d)\n",
              error.message[0] ? error.message : "Unknown error", result);
      continue;
    }
    if (printf("Output: %s\n", string_c_str(&output)) < 0) {
      fprintf(stderr, "Failed to write output.\n");
      exit_code = EXIT_FAILURE;
      break;
    }
  }

done:
  if (status != AGENT_OK) {
    fprintf(stderr, "%s failed (error code: %d)%s%s.\n", stage, status,
            error.message[0] ? ": " : "", error.message);
    exit_code = EXIT_FAILURE;
  }
  string_destroy(&output);
  agent_destroy(agent);
  if (session_destory(session) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy session.\n");
    exit_code = EXIT_FAILURE;
  }
  if (tool_registry_destory(registry) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy tool registry.\n");
    exit_code = EXIT_FAILURE;
  }
  if (provider != NULL && openai_provider_destory(provider) != AGENT_OK) {
    fprintf(stderr, "Failed to destroy provider.\n");
    exit_code = EXIT_FAILURE;
  }
  return exit_code;
}
