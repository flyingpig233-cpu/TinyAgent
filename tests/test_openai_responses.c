#include "agent/context.h"
#include "agent/mock_llm.h"
#include "agent/providers/openai.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "line %d: %s\n", __LINE__, #expr);                       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int is_string(const cJSON *object, const char *key,
                     const char *expected) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) && !strcmp(item->valuestring, expected);
}

static int test_encode(void) {
  ToolCall calls[] = {{"call_1", "weather", "{\"city\":\"北京\"}"},
                      {"call_2", "weather", "{}"}};
  Message messages[] = {
      {.role = SYSTEM_MESSAGE, .content = "system"},
      {.role = USER_MESSAGE, .content = "say \"hi\""},
      {.role = ASSISTANT_MESSAGE,
       .content = "checking",
       .tool_calls = calls,
       .tool_call_count = 2},
      {.role = TOOL_MESSAGE, .tool_call_id = "call_1", .content = "sunny"},
      {.role = TOOL_MESSAGE, .tool_call_id = "call_2", .content = "rainy"}};
  MessageArrary array = {.items = messages, .count = 5, .capacity = 5};
  Context ctx = {.array = &array};
  ToolDefinition tool = {.name = "weather",
                         .description = "Get weather",
                         .parameters_json =
                             "{\"type\":\"object\",\"properties\":{},"
                             "\"additionalProperties\":false}",
                         .strict = 1};
  LLMRequest request = {.context = &ctx, .tools = &tool, .tool_count = 1};
  for (int protocol = 0; protocol < 2; ++protocol) {
    char *json = NULL;
    int (*encode)(const LLMRequest *, const char *, char **) =
        protocol ? openai_responses_encode_request : openai_chat_encode_request;
    CHECK(encode(&request, "model", &json) == AGENT_OK);
    cJSON *root = cJSON_Parse(json);
    free(json);
    CHECK(root != NULL && is_string(root, "model", "model"));
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
    cJSON *function = cJSON_GetArrayItem(tools, 0);
    CHECK(is_string(function, "type", "function"));
    if (!protocol)
      function = cJSON_GetObjectItemCaseSensitive(function, "function");
    CHECK(is_string(function, "name", "weather"));
    CHECK(is_string(function, "description", "Get weather"));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(function, "strict")));
    CHECK(cJSON_IsObject(
        cJSON_GetObjectItemCaseSensitive(function, "parameters")));
    if (protocol) {
      CHECK(cJSON_GetObjectItemCaseSensitive(root, "messages") == NULL);
      CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "store")));
      cJSON *include = cJSON_GetArrayItem(
          cJSON_GetObjectItemCaseSensitive(root, "include"), 0);
      CHECK(cJSON_IsString(include) &&
            !strcmp(include->valuestring, "reasoning.encrypted_content"));
      cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
      CHECK(cJSON_GetArraySize(input) == 7);
      CHECK(is_string(cJSON_GetArrayItem(input, 0), "role", "system"));
      CHECK(is_string(cJSON_GetArrayItem(input, 1), "content", "say \"hi\""));
      CHECK(is_string(cJSON_GetArrayItem(input, 3), "type", "function_call"));
      CHECK(is_string(cJSON_GetArrayItem(input, 3), "call_id", "call_1"));
      CHECK(is_string(cJSON_GetArrayItem(input, 3), "arguments",
                      calls[0].arguments_json));
      CHECK(is_string(cJSON_GetArrayItem(input, 4), "call_id", "call_2"));
      CHECK(is_string(cJSON_GetArrayItem(input, 5), "type",
                      "function_call_output"));
      CHECK(is_string(cJSON_GetArrayItem(input, 5), "output", "sunny"));
    }
    cJSON_Delete(root);
    const char *invalid[] = {"[]", "{} trailing", "null"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      tool.parameters_json = invalid[i];
      CHECK(encode(&request, "model", &json) == AGENT_ERR_INVAILD_ARGUMENT &&
            json == NULL);
    }
    tool.parameters_json = "{}";
    tool.strict = 2;
    CHECK(encode(&request, "model", &json) == AGENT_ERR_INVAILD_ARGUMENT &&
          json == NULL);
    tool.strict = 0;
    CHECK(encode(&request, "model", &json) == AGENT_OK);
    root = cJSON_Parse(json);
    free(json);
    function =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "tools"), 0);
    if (!protocol)
      function = cJSON_GetObjectItemCaseSensitive(function, "function");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(function, "strict")));
    cJSON_Delete(root);
    request.tools = NULL;
    CHECK(encode(&request, "model", &json) == AGENT_ERR_INVAILD_ARGUMENT &&
          json == NULL);
    request.tools = &tool;
    tool.strict = 1;
  }
  char *json = NULL;
  messages[2].tool_calls = NULL;
  CHECK(openai_responses_encode_request(&request, "model", &json) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(openai_responses_encode_request(NULL, "model", &json) ==
        AGENT_ERR_NULL_POINTER);
  CHECK(openai_responses_encode_request(&request, "model", NULL) ==
        AGENT_ERR_NULL_POINTER);
  return 0;
}

static const char *tool_reply =
    "{\"status\":\"completed\",\"output\":["
    "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],\"encrypted_"
    "content\":\"opaque\"},"
    "{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"content\":"
    "["
    "{\"type\":\"output_text\",\"text\":\"hello "
    "\"},{\"type\":\"output_text\",\"text\":\"世界\"}]},"
    "{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\","
    "\"name\":\"weather\",\"arguments\":\"{}\"},"
    "{\"type\":\"function_call\",\"id\":\"fc_2\",\"call_id\":\"call_2\","
    "\"name\":\"weather\",\"arguments\":\"{}\"}]}";

static int test_replay(void) {
  LLMResponse *response = NULL;
  AgentError error = {0};
  CHECK(openai_responses_decode_response(tool_reply, &response, &error) ==
        AGENT_OK);
  CHECK(error.message[0] == '\0');
  CHECK(!strcmp(response->content, "hello 世界"));
  CHECK(!strcmp(response->provider_status, "completed"));
  CHECK(response->finish_reason == LLM_FINISH_TOOL_CALLS &&
        response->tool_call_count == 2);
  CHECK(!strcmp(response->tool_calls[0].call_id,
                "call_1")); // Not the fc_ item ID.
  CHECK(!strcmp(response->tool_calls[1].call_id, "call_2"));
  CHECK(response->continuation.format == LLM_CONTINUATION_OPENAI_RESPONSES);
  Context ctx = {0};
  CHECK(context_init(&ctx) == AGENT_OK);
  Message user = {.role = USER_MESSAGE, .content = "weather?"};
  CHECK(context_append(&ctx, &user) == AGENT_OK);
  CHECK(context_add_assistant_response(&ctx, response, &error) == AGENT_OK);
  CHECK(ctx.array->items[1].continuation.items_json !=
        response->continuation.items_json);
  LLMProvider mock = {0};
  CHECK(mock_llm_init(&mock, response, 1) == AGENT_OK);
  llm_response_destroy(response);
  CHECK(llm_provider_complete(&mock, NULL, &response, &error) == AGENT_OK);
  CHECK(!strcmp(response->provider_status, "completed"));
  CHECK(strstr(response->continuation.items_json, "opaque") != NULL);
  CHECK(llm_provider_destory(&mock, NULL) == AGENT_OK);
  llm_response_destroy(response);
  Message result = {
      .role = TOOL_MESSAGE, .content = "sunny", .tool_call_id = "call_1"};
  CHECK(context_append(&ctx, &result) == AGENT_OK);
  result.tool_call_id = "call_2";
  CHECK(context_append(&ctx, &result) == AGENT_OK);
  LLMRequest request = {.context = &ctx};
  char *json = NULL;
  CHECK(openai_responses_encode_request(&request, "model", &json) == AGENT_OK);
  cJSON *root = cJSON_Parse(json);
  free(json);
  cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
  CHECK(cJSON_GetArraySize(input) == 7);
  cJSON *original = cJSON_Parse(tool_reply);
  cJSON *items = cJSON_GetObjectItemCaseSensitive(original, "output");
  for (int i = 0; i < 4; ++i)
    CHECK(cJSON_Compare(cJSON_GetArrayItem(items, i),
                        cJSON_GetArrayItem(input, i + 1), 1));
  CHECK(is_string(cJSON_GetArrayItem(input, 5), "call_id", "call_1"));
  cJSON_Delete(original);
  cJSON_Delete(root);
  CHECK(openai_chat_encode_request(&request, "model", &json) == AGENT_OK);
  CHECK(strstr(json, "opaque") ==
        NULL); // Never leak Responses metadata to Chat.
  free(json);
  // Copying from within the context must retain independent history ownership.
  CHECK(context_append(&ctx, &ctx.array->items[1]) == AGENT_OK);
  CHECK(ctx.array->items[4].continuation.items_json !=
        ctx.array->items[1].continuation.items_json);
  CHECK(context_destory(&ctx) == AGENT_OK);
  return 0;
}

static int test_statuses(void) {
  const struct {
    const char *json;
    int status;
    LLMFinishReason finish;
  } cases[] = {
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
       "\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"\"}]}]"
       "}",
       AGENT_OK, LLM_FINISH_STOP},
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
       "\"assistant\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"cannot "
       "comply\"}]}]}",
       AGENT_OK, LLM_FINISH_REFUSED},
      {"{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_"
       "output_tokens\"},\"output\":[]}",
       AGENT_OK, LLM_FINISH_LENGTH},
      {"{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":"
       "\"content_filter\"},\"output\":[]}",
       AGENT_OK, LLM_FINISH_FILTERED},
      {"{\"status\":\"failed\",\"error\":{\"code\":\"server_error\","
       "\"message\":\"try again\"}}",
       AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\"}]}",
       AGENT_ERR_TOOL_UNSUPPORTED, 0},
      {"{\"status\":\"completed\",\"output\":[]}", AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\"}]}",
       AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"queued\",\"output\":[]}", AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"incomplete\",\"output\":[]}", AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":"
       "\"unknown\"},\"output\":[]}",
       AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":[{}]}", AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
       "\"id\":\"fc_wrong_id\",\"name\":\"weather\",\"arguments\":\"{}\"}]}",
       AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
       "\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":null}]}]"
       "}",
       AGENT_ERR_LLM_RESPONSE, 0},
      {"{\"status\":\"completed\",\"output\":{}}", AGENT_ERR_LLM_RESPONSE, 0},
      {"{} trailing", AGENT_ERR_LLM_RESPONSE, 0},
      {"not json", AGENT_ERR_LLM_RESPONSE, 0}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    LLMResponse *response = NULL;
    AgentError error = {0};
    CHECK(openai_responses_decode_response(cases[i].json, &response, &error) ==
          cases[i].status);
    if (cases[i].status == AGENT_OK) {
      CHECK(response != NULL && response->finish_reason == cases[i].finish);
      if (cases[i].finish == LLM_FINISH_REFUSED)
        CHECK(!strcmp(response->refusal, "cannot comply"));
      if (cases[i].finish == LLM_FINISH_LENGTH)
        CHECK(!strcmp(response->provider_reason, "max_output_tokens"));
      CHECK(error.message[0] == '\0');
      llm_response_destroy(response);
    } else {
      CHECK(response == NULL && error.message[0] != '\0');
      if (i == 4)
        CHECK(strstr(error.message, "server_error") &&
              strstr(error.message, "try again"));
    }
  }
  LLMResponse *response = NULL;
  CHECK(openai_responses_decode_response(NULL, &response, NULL) ==
        AGENT_ERR_NULL_POINTER);
  CHECK(openai_responses_decode_response("{}", NULL, NULL) ==
        AGENT_ERR_NULL_POINTER);
  AgentError error = {0};
  CHECK(openai_chat_decode_response(
            "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
            "\"content\":null,\"refusal\":\"no\"}}]}",
            &response, &error) == AGENT_OK);
  CHECK(response->finish_reason == LLM_FINISH_REFUSED &&
        !strcmp(response->refusal, "no"));
  llm_response_destroy(response);
  return 0;
}

static int test_refusal_history(void) {
  const char *replies[] = {
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"refusal\":"
      "\"cannot comply\"}}]}",
      "{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":"
      "\"assistant\",\"content\":[{\"type\":\"refusal\",\"refusal\":\"cannot "
      "comply\"}]}]}"};
  for (int protocol = 0; protocol < 2; ++protocol) {
    Context ctx = {0};
    CHECK(context_init(&ctx) == AGENT_OK);
    LLMResponse *response = NULL;
    int status = protocol ? openai_responses_decode_response(replies[protocol],
                                                             &response, NULL)
                          : openai_chat_decode_response(replies[protocol],
                                                        &response, NULL);
    CHECK(status == AGENT_OK);
    CHECK(context_add_assistant_response(&ctx, response, NULL) == AGENT_OK);
    CHECK(ctx.array->items[0].refusal != response->refusal);
    llm_response_destroy(response);
    Message next = {.role = USER_MESSAGE, .content = "another question"};
    CHECK(context_append(&ctx, &next) == AGENT_OK);
    LLMRequest request = {.context = &ctx};
    for (int target = 0; target < 2; ++target) {
      char *json = NULL;
      status = target
                   ? openai_responses_encode_request(&request, "model", &json)
                   : openai_chat_encode_request(&request, "model", &json);
      CHECK(status == AGENT_OK);
      cJSON *root = cJSON_Parse(json);
      free(json);
      cJSON *history =
          cJSON_GetObjectItemCaseSensitive(root, target ? "input" : "messages");
      CHECK(cJSON_GetArraySize(history) == 2);
      cJSON *assistant = cJSON_GetArrayItem(history, 0);
      if (target)
        assistant = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(assistant, "content"), 0);
      CHECK(is_string(assistant, "refusal", "cannot comply"));
      cJSON_Delete(root);
    }
    CHECK(context_destory(&ctx) == AGENT_OK);
  }
  return 0;
}

static int test_invalid_history(void) {
  Context ctx = {0};
  CHECK(context_init(&ctx) == AGENT_OK);
  Message msg = {.role = ASSISTANT_MESSAGE,
                 .content = "text",
                 .continuation = {.format = LLM_CONTINUATION_OPENAI_RESPONSES,
                                  .items_json = "{}"}};
  CHECK(context_append(&ctx, &msg) == AGENT_OK);
  LLMRequest request = {.context = &ctx};
  char *json = NULL;
  CHECK(openai_responses_encode_request(&request, "model", &json) ==
        AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(json == NULL);
  CHECK(context_destory(&ctx) == AGENT_OK);
  CHECK(context_init(&ctx) == AGENT_OK);
  msg.role = USER_MESSAGE;
  CHECK(context_append(&ctx, &msg) == AGENT_ERR_INVAILD_ARGUMENT);
  msg.role = ASSISTANT_MESSAGE;
  msg.continuation.format = LLM_CONTINUATION_NONE;
  CHECK(context_append(&ctx, &msg) == AGENT_ERR_INVAILD_ARGUMENT);
  CHECK(ctx.array->count == 0);
  CHECK(context_destory(&ctx) == AGENT_OK);
  return 0;
}

int main(void) {
  CHECK(test_encode() == 0);
  CHECK(test_replay() == 0);
  CHECK(test_statuses() == 0);
  CHECK(test_refusal_history() == 0);
  CHECK(test_invalid_history() == 0);
  return 0;
}
