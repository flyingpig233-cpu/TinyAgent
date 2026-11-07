#include "agent/http_client.h"
#include "agent/infra.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct HttpClient {
  CURL *handle;
};

/* A response body either collected into an HttpResponse and/or forwarded to a
 * caller-supplied write callback. Exactly one of collect/write is normally set. */
typedef struct {
  HttpResponse *collect;
  HttpWriteFn write;
  void *userdata;
  int failed;
} BodySink;

static size_t sink_write(char *data, size_t size, size_t count,
                         void *user_data) {
  BodySink *sink = user_data;
  if (size != 0 && count > SIZE_MAX / size) {
    sink->failed = 1;
    return 0;
  }
  size_t bytes = size * count;
  if (sink->collect != NULL) {
    if (bytes > SIZE_MAX - 1 - sink->collect->body_len) {
      sink->failed = 1;
      return 0;
    }
    size_t length = sink->collect->body_len + bytes;
    char *body = realloc(sink->collect->body, length + 1);
    if (body == NULL) {
      sink->failed = 1;
      return 0;
    }
    if (bytes != 0)
      memcpy(body + sink->collect->body_len, data, bytes);
    body[length] = '\0';
    sink->collect->body = body;
    sink->collect->body_len = length;
  }
  if (sink->write != NULL && sink->write(sink->userdata, data, bytes) != bytes) {
    sink->failed = 1;
    return 0;
  }
  return bytes;
}

int http_client_init(HttpClient **client) {
  if (client == NULL)
    return AGENT_ERR_NULL_POINTER;
  *client = NULL;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return AGENT_ERR_HTTP;
  HttpClient *value = malloc(sizeof(*value));
  if (value == NULL) {
    curl_global_cleanup();
    return AGENT_ERR_OUT_OF_MEMORY;
  }
  value->handle = curl_easy_init();
  if (value->handle == NULL) {
    free(value);
    curl_global_cleanup();
    return AGENT_ERR_HTTP;
  }
  *client = value;
  return AGENT_OK;
}

/* Performs one request. The body is delivered to sink; on a successful
 * transfer *status_code receives the HTTP status (which may be an error code). */
static int perform(HttpClient *client, const HttpRequest *request,
                   BodySink *sink, long *status_code, AgentError *error) {
  agent_error_clear(error);
  if (client == NULL || request == NULL || request->url == NULL ||
      sink == NULL) {
    ERROR(error, "HTTP client, request, URL and response are required");
    return AGENT_ERR_NULL_POINTER;
  }
  if ((strncmp(request->url, "http://", 7) != 0 &&
       strncmp(request->url, "https://", 8) != 0) ||
      (request->header_count != 0 && request->headers == NULL) ||
      (request->body_len != 0 && request->body == NULL) ||
      request->connect_timeout_ms < 0 || request->timeout_ms < 0 ||
      (curl_off_t)request->body_len < 0 ||
      (uintmax_t)(curl_off_t)request->body_len !=
          (uintmax_t)request->body_len) {
    ERROR(error, "Invalid HTTP request");
    return AGENT_ERR_INVAILD_ARGUMENT;
  }
  struct curl_slist *headers = NULL;
  CURLcode result = CURLE_OK;
  int status = AGENT_OK;
  char curl_error[CURL_ERROR_SIZE] = {0};
  for (size_t i = 0; i < request->header_count; ++i) {
    if (request->headers[i] == NULL) {
      ERROR(error, "HTTP header must not be NULL");
      status = AGENT_ERR_INVAILD_ARGUMENT;
      goto done;
    }
    struct curl_slist *next = curl_slist_append(headers, request->headers[i]);
    if (next == NULL) {
      ERROR(error, "Could not allocate HTTP headers");
      status = AGENT_ERR_OUT_OF_MEMORY;
      goto done;
    }
    headers = next;
  }
  curl_easy_reset(client->handle);
#define SETOPT(option, value)                                                  \
  do {                                                                         \
    result = curl_easy_setopt(client->handle, option, value);                  \
    if (result != CURLE_OK)                                                    \
      goto curl_failed;                                                        \
  } while (0)
  SETOPT(CURLOPT_ERRORBUFFER, curl_error);
  SETOPT(CURLOPT_URL, request->url);
  SETOPT(CURLOPT_NOSIGNAL, 1L);
  SETOPT(CURLOPT_CONNECTTIMEOUT_MS,
         request->connect_timeout_ms ? request->connect_timeout_ms : 10000L);
  SETOPT(CURLOPT_TIMEOUT_MS,
         request->timeout_ms ? request->timeout_ms : 60000L);
  SETOPT(CURLOPT_WRITEFUNCTION, sink_write);
  SETOPT(CURLOPT_WRITEDATA, sink);
  SETOPT(CURLOPT_HTTPHEADER, headers);
  if (request->body != NULL) {
    SETOPT(CURLOPT_POSTFIELDS, request->body);
    SETOPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_len);
  }
  if (request->method != NULL || request->body != NULL)
    SETOPT(CURLOPT_CUSTOMREQUEST,
           request->method != NULL ? request->method : "GET");
  result = curl_easy_perform(client->handle);
  if (result != CURLE_OK)
    goto curl_failed;
  if (status_code != NULL) {
    result = curl_easy_getinfo(client->handle, CURLINFO_RESPONSE_CODE,
                               status_code);
    if (result != CURLE_OK)
      goto curl_failed;
  }
  goto done;

curl_failed:
  status = sink->failed                        ? AGENT_ERR_OUT_OF_MEMORY
           : result == CURLE_OPERATION_TIMEDOUT ? AGENT_ERR_HTTP_TIMEOUT
                                                : AGENT_ERR_HTTP;
  ERROR(error, sink->failed   ? "Could not allocate HTTP response"
               : curl_error[0] ? curl_error
                               : curl_easy_strerror(result));
done:
  curl_slist_free_all(headers);
  return status;
#undef SETOPT
}

int http_client_request(HttpClient *client, const HttpRequest *request,
                        HttpResponse *response, AgentError *error) {
  if (response == NULL)
    return AGENT_ERR_NULL_POINTER;
  HttpResponse buffer = {0};
  BodySink sink = {.collect = &buffer};
  long status_code = 0;
  int status = perform(client, request, &sink, &status_code, error);
  if (status != AGENT_OK) {
    http_response_free(&buffer);
    return status;
  }
  if (buffer.body == NULL) {
    buffer.body = calloc(1, 1);
    if (buffer.body == NULL) {
      ERROR(error, "Could not allocate HTTP response");
      return AGENT_ERR_OUT_OF_MEMORY;
    }
  }
  buffer.status_code = status_code;
  http_response_free(response);
  *response = buffer;
  return AGENT_OK;
}

int http_client_request_stream(HttpClient *client, const HttpRequest *request,
                               HttpWriteFn write, void *userdata,
                               long *status_code, AgentError *error) {
  if (write == NULL)
    return AGENT_ERR_NULL_POINTER;
  BodySink sink = {.write = write, .userdata = userdata};
  long code = 0;
  int status = perform(client, request, &sink, &code, error);
  if (status == AGENT_OK && status_code != NULL)
    *status_code = code;
  return status;
}

int http_client_post(HttpClient *client, const char *url, const char *json,
                     HttpResponse *response) {
  if (json == NULL)
    return AGENT_ERR_NULL_POINTER;
  const char *headers[] = {"Content-Type: application/json"};
  HttpRequest request = {.method = "POST",
                         .url = url,
                         .headers = headers,
                         .header_count = 1,
                         .body = json,
                         .body_len = strlen(json)};
  return http_client_request(client, &request, response, NULL);
}

void http_response_free(HttpResponse *response) {
  if (response == NULL)
    return;
  free(response->body);
  *response = (HttpResponse){0};
}

int http_client_destroy(HttpClient *client) {
  if (client == NULL)
    return AGENT_ERR_NULL_POINTER;
  curl_easy_cleanup(client->handle);
  free(client);
  curl_global_cleanup();
  return AGENT_OK;
}
