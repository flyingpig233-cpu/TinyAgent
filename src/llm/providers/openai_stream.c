#include "openai_internal.h"
#include "agent/infra.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends raw bytes to a String, keeping it NUL-terminated. */
static int buffer_append(String *s, const char *data, size_t length) {
  if (length == 0)
    return AGENT_OK; /* Nothing to append; string_c_str reports "". */
  if (s->length > SIZE_MAX - length - 1)
    return AGENT_ERR_OUT_OF_MEMORY;
  if (string_reserve(s, s->length + length) != AGENT_OK)
    return AGENT_ERR_OUT_OF_MEMORY;
  memcpy(s->data + s->length, data, length);
  s->length += length;
  s->data[s->length] = '\0';
  return AGENT_OK;
}

/* ------------------------------------------------------------------ */
/* Chat Completions accumulator                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  String id;
  String name;
  String arguments;
  int used;
} ChatCall;

typedef struct {
  String content;
  int has_content;
  String refusal;
  int has_refusal;
  char *finish_reason;
  ChatCall *calls;
  size_t count;
  size_t capacity;
  OpenAIDeltaFn on_delta;
  void *delta_userdata;
} ChatAccum;

static ChatCall *chat_slot(ChatAccum *accum, long index) {
  if ((size_t)index >= accum->capacity) {
    size_t capacity = accum->capacity ? accum->capacity : 4;
    while (capacity <= (size_t)index) {
      if (capacity > SIZE_MAX / 2)
        return NULL;
      capacity *= 2;
    }
    ChatCall *calls = realloc(accum->calls, capacity * sizeof(*calls));
    if (calls == NULL)
      return NULL;
    for (size_t i = accum->capacity; i < capacity; ++i)
      calls[i] = (ChatCall){0};
    accum->calls = calls;
    accum->capacity = capacity;
  }
  if ((size_t)index >= accum->count)
    accum->count = (size_t)index + 1;
  return &accum->calls[index];
}

static int chat_feed(void *impl, const char *payload) {
  ChatAccum *accum = impl;
  cJSON *root = cJSON_ParseWithOpts(payload, NULL, 1);
  if (root == NULL)
    return AGENT_ERR_LLM_RESPONSE;
  int status = AGENT_OK;
  cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  if (cJSON_IsObject(choice)) {
    cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    if (cJSON_IsString(finish)) {
      free(accum->finish_reason);
      accum->finish_reason = NULL;
      if (full_copy(&accum->finish_reason, finish->valuestring) != AGENT_OK)
        status = AGENT_ERR_OUT_OF_MEMORY;
    }
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    if (status == AGENT_OK && cJSON_IsObject(delta)) {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
      if (cJSON_IsString(content)) {
        accum->has_content = 1;
        status = buffer_append(&accum->content, content->valuestring,
                               strlen(content->valuestring));
        if (status == AGENT_OK && accum->on_delta != NULL)
          accum->on_delta(accum->delta_userdata, content->valuestring);
      }
      if (status == AGENT_OK) {
        cJSON *refusal = cJSON_GetObjectItemCaseSensitive(delta, "refusal");
        if (cJSON_IsString(refusal)) {
          accum->has_refusal = 1;
          status = buffer_append(&accum->refusal, refusal->valuestring,
                                 strlen(refusal->valuestring));
        }
      }
      if (status == AGENT_OK) {
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        cJSON *call = NULL;
        cJSON_ArrayForEach(call, calls) {
          cJSON *index = cJSON_GetObjectItemCaseSensitive(call, "index");
          if (!cJSON_IsNumber(index) || index->valueint < 0 ||
              index->valuedouble != (double)index->valueint ||
              (size_t)index->valueint > accum->count) {
            status = AGENT_ERR_LLM_RESPONSE;
            break;
          }
          long slot_index = index->valueint;
          ChatCall *slot = chat_slot(accum, slot_index);
          if (slot == NULL) {
            status = AGENT_ERR_OUT_OF_MEMORY;
            break;
          }
          slot->used = 1;
          cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
          if (cJSON_IsString(id) && id->valuestring[0] != '\0' &&
              slot->id.length == 0)
            status = buffer_append(&slot->id, id->valuestring,
                                   strlen(id->valuestring));
          cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
          if (status == AGENT_OK && cJSON_IsObject(function)) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(function, "name");
            if (cJSON_IsString(name) && name->valuestring[0] != '\0' &&
                slot->name.length == 0)
              status = buffer_append(&slot->name, name->valuestring,
                                     strlen(name->valuestring));
            cJSON *arguments =
                cJSON_GetObjectItemCaseSensitive(function, "arguments");
            if (status == AGENT_OK && cJSON_IsString(arguments))
              status = buffer_append(&slot->arguments, arguments->valuestring,
                                     strlen(arguments->valuestring));
          }
          if (status != AGENT_OK)
            break;
        }
      }
    }
  }
  cJSON_Delete(root);
  return status;
}

static int chat_finalize(ChatAccum *accum, LLMResponse **out, AgentError *error) {
  if (accum->finish_reason == NULL) {
    ERROR(error, "OpenAI stream ended without a finish reason");
    return AGENT_ERR_LLM_RESPONSE;
  }
  size_t call_total = 0;
  for (size_t i = 0; i < accum->count; ++i)
    if (accum->calls[i].used && accum->calls[i].name.length != 0 &&
        accum->calls[i].id.length != 0)
      ++call_total;

  int status = AGENT_ERR_OUT_OF_MEMORY;
  cJSON *root = cJSON_CreateObject();
  cJSON *choices = root ? cJSON_AddArrayToObject(root, "choices") : NULL;
  cJSON *choice = cJSON_CreateObject();
  if (root == NULL || choices == NULL || choice == NULL ||
      !cJSON_AddItemToArray(choices, choice)) {
    cJSON_Delete(choice);
    cJSON_Delete(root);
    return status;
  }
  cJSON *message = cJSON_AddObjectToObject(choice, "message");
  if (message == NULL)
    goto done;
  const char *content =
      accum->has_content ? string_c_str(&accum->content)
      : call_total != 0  ? NULL
                         : "";
  if (content != NULL) {
    if (cJSON_AddStringToObject(message, "content", content) == NULL)
      goto done;
  } else if (cJSON_AddNullToObject(message, "content") == NULL) {
    goto done;
  }
  if (accum->has_refusal &&
      cJSON_AddStringToObject(message, "refusal",
                              string_c_str(&accum->refusal)) == NULL)
    goto done;
  if (call_total != 0) {
    cJSON *calls = cJSON_AddArrayToObject(message, "tool_calls");
    if (calls == NULL)
      goto done;
    for (size_t i = 0; i < accum->count; ++i) {
      ChatCall *slot = &accum->calls[i];
      if (!slot->used || slot->name.length == 0 || slot->id.length == 0)
        continue;
      cJSON *item = cJSON_CreateObject();
      if (item == NULL || !cJSON_AddItemToArray(calls, item)) {
        cJSON_Delete(item);
        goto done;
      }
      cJSON *function = cJSON_AddObjectToObject(item, "function");
      if (cJSON_AddStringToObject(item, "id", string_c_str(&slot->id)) ==
              NULL ||
          cJSON_AddStringToObject(item, "type", "function") == NULL ||
          function == NULL ||
          cJSON_AddStringToObject(function, "name",
                                  string_c_str(&slot->name)) == NULL ||
          cJSON_AddStringToObject(function, "arguments",
                                  string_c_str(&slot->arguments)) == NULL)
        goto done;
    }
  }
  if (cJSON_AddStringToObject(choice, "finish_reason", accum->finish_reason) ==
      NULL)
    goto done;
  char *json = cJSON_PrintUnformatted(root);
  if (json == NULL)
    goto done;
  status = openai_chat_decode_response(json, out, error);
  free(json);
done:
  cJSON_Delete(root);
  return status;
}

static void chat_free(ChatAccum *accum) {
  string_destroy(&accum->content);
  string_destroy(&accum->refusal);
  free(accum->finish_reason);
  for (size_t i = 0; i < accum->count; ++i) {
    string_destroy(&accum->calls[i].id);
    string_destroy(&accum->calls[i].name);
    string_destroy(&accum->calls[i].arguments);
  }
  free(accum->calls);
}

/* ------------------------------------------------------------------ */
/* Responses accumulator                                               */
/* ------------------------------------------------------------------ */

typedef struct {
  String content;
  int has_content;
  String refusal;
  int has_refusal;
  String final_json;
  int has_final;
  String error_message;
  int has_error;
  OpenAIDeltaFn on_delta;
  void *delta_userdata;
} ResponsesAccum;

static int responses_feed(void *impl, const char *payload) {
  ResponsesAccum *accum = impl;
  cJSON *root = cJSON_ParseWithOpts(payload, NULL, 1);
  if (root == NULL)
    return AGENT_ERR_LLM_RESPONSE;
  int status = AGENT_OK;
  cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const char *name = cJSON_IsString(type) ? type->valuestring : "";
  if (strcmp(name, "response.output_text.delta") == 0) {
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta)) {
      accum->has_content = 1;
      status = buffer_append(&accum->content, delta->valuestring,
                             strlen(delta->valuestring));
      if (status == AGENT_OK && accum->on_delta != NULL)
        accum->on_delta(accum->delta_userdata, delta->valuestring);
    }
  } else if (strcmp(name, "response.refusal.delta") == 0) {
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta)) {
      accum->has_refusal = 1;
      status = buffer_append(&accum->refusal, delta->valuestring,
                             strlen(delta->valuestring));
    }
  } else if (strcmp(name, "response.completed") == 0 ||
             strcmp(name, "response.incomplete") == 0) {
    cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    if (cJSON_IsObject(response)) {
      char *json = cJSON_PrintUnformatted(response);
      if (json == NULL) {
        status = AGENT_ERR_OUT_OF_MEMORY;
      } else {
        string_clear(&accum->final_json);
        status = buffer_append(&accum->final_json, json, strlen(json));
        free(json);
        if (status == AGENT_OK)
          accum->has_final = 1;
      }
    }
  } else if (strcmp(name, "error") == 0 ||
             strcmp(name, "response.failed") == 0) {
    cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(detail, "message");
    if (!cJSON_IsString(message))
      message = cJSON_GetObjectItemCaseSensitive(root, "message");
    accum->has_error = 1;
    if (cJSON_IsString(message))
      status = buffer_append(&accum->error_message, message->valuestring,
                             strlen(message->valuestring));
  }
  cJSON_Delete(root);
  return status;
}

static int responses_finalize(ResponsesAccum *accum, LLMResponse **out,
                              AgentError *error) {
  if (accum->has_error) {
    ERROR(error, accum->error_message.length
                     ? string_c_str(&accum->error_message)
                     : "OpenAI stream reported an error");
    return AGENT_ERR_LLM_RESPONSE;
  }
  if (!accum->has_final) {
    ERROR(error, "OpenAI stream ended without a completed response");
    return AGENT_ERR_LLM_RESPONSE;
  }
  return openai_responses_decode_response(string_c_str(&accum->final_json),
                                          out, error);
}

static void responses_free(ResponsesAccum *accum) {
  string_destroy(&accum->content);
  string_destroy(&accum->refusal);
  string_destroy(&accum->final_json);
  string_destroy(&accum->error_message);
}

/* ------------------------------------------------------------------ */
/* SSE framing                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  String raw;
  String line;
  int status;
  OpenAIProtocol protocol;
  void *impl;
  int (*feed)(void *impl, const char *payload);
} StreamState;

static int stream_line(StreamState *state, const char *text) {
  size_t length = strlen(text);
  if (length != 0 && text[length - 1] == '\r')
    --length;
  if (length == 0 || strncmp(text, "data:", 5) != 0)
    return AGENT_OK; /* Blank lines, comments and event:/id: lines. */
  const char *payload = text + 5;
  if (*payload == ' ')
    ++payload;
  size_t payload_len = (size_t)((text + length) - payload);
  if (payload_len == 6 && strncmp(payload, "[DONE]", 6) == 0)
    return AGENT_OK;
  char *copy = malloc(payload_len + 1);
  if (copy == NULL)
    return AGENT_ERR_OUT_OF_MEMORY;
  memcpy(copy, payload, payload_len);
  copy[payload_len] = '\0';
  int status = state->feed(state->impl, copy);
  free(copy);
  return status;
}

static size_t stream_write(void *userdata, const char *data, size_t length) {
  StreamState *state = userdata;
  if (buffer_append(&state->raw, data, length) != AGENT_OK) {
    state->status = AGENT_ERR_OUT_OF_MEMORY;
    return 0;
  }
  for (size_t i = 0; i < length; ++i) {
    if (data[i] == '\n') {
      int status = stream_line(state, state->line.data ? state->line.data : "");
      string_clear(&state->line);
      if (status != AGENT_OK) {
        state->status = status;
        return 0;
      }
    } else if (buffer_append(&state->line, &data[i], 1) != AGENT_OK) {
      state->status = AGENT_ERR_OUT_OF_MEMORY;
      return 0;
    }
  }
  return length;
}

int openai_stream_complete(HttpClient *client, const HttpRequest *http_request,
                           OpenAIProtocol protocol, OpenAIDeltaFn on_delta,
                           void *delta_userdata, LLMResponse **out,
                           AgentError *error) {
  agent_error_clear(error);
  if (out != NULL)
    *out = NULL;
  if (client == NULL || http_request == NULL || out == NULL) {
    ERROR(error, "OpenAI streaming requires a client, request and output");
    return AGENT_ERR_NULL_POINTER;
  }
  ChatAccum chat = {.on_delta = on_delta, .delta_userdata = delta_userdata};
  ResponsesAccum responses = {.on_delta = on_delta,
                              .delta_userdata = delta_userdata};
  StreamState state = {.protocol = protocol};
  state.feed = protocol == PROTOCOL_RESPONSES ? responses_feed : chat_feed;
  state.impl = protocol == PROTOCOL_RESPONSES ? (void *)&responses : (void *)&chat;
  string_init(&state.raw);
  string_init(&state.line);

  long status_code = 0;
  int status = http_client_request_stream(client, http_request, stream_write,
                                          &state, &status_code, error);
  if (status != AGENT_OK) {
    if (state.status != AGENT_OK) {
      status = state.status;
      ERROR(error, status == AGENT_ERR_OUT_OF_MEMORY
                       ? "Could not allocate OpenAI stream"
                       : "Could not parse OpenAI stream");
    }
    goto done;
  }
  if (state.line.length != 0) {
    status = stream_line(&state, string_c_str(&state.line));
    string_clear(&state.line);
    if (status != AGENT_OK)
      goto done;
  }
  if (status_code < 200 || status_code >= 300) {
    char message[96];
    snprintf(message, sizeof(message),
             "OpenAI request failed with HTTP status %ld", status_code);
    cJSON *body = cJSON_ParseWithLength(state.raw.data ? state.raw.data : "",
                                        state.raw.length);
    openai_json_error(error, message,
                      cJSON_GetObjectItemCaseSensitive(body, "error"));
    cJSON_Delete(body);
    status = AGENT_ERR_HTTP;
    goto done;
  }
  status = protocol == PROTOCOL_RESPONSES ? responses_finalize(&responses, out, error)
                                          : chat_finalize(&chat, out, error);

done:
  chat_free(&chat);
  responses_free(&responses);
  string_destroy(&state.raw);
  string_destroy(&state.line);
  if (status != AGENT_OK && (error == NULL || error->message[0] == '\0'))
    ERROR(error, "Could not parse OpenAI stream");
  return status;
}
