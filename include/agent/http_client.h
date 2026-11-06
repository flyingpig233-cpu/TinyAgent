#ifndef TINY_AGENT_HTTP_CLIENT_H
#define TINY_AGENT_HTTP_CLIENT_H

#include "agent/error.h"
#include <stddef.h>

typedef struct {
  long status_code;
  char *body; // Owned, NUL-terminated; may contain embedded NUL bytes.
  size_t body_len;
} HttpResponse;

typedef struct HttpClient HttpClient;

typedef struct {
  const char *method;         // NULL defaults to GET.
  const char *url;            // HTTP or HTTPS URL, required.
  const char *const *headers; // Header lines, borrowed for this call.
  size_t header_count;
  const void *body; // Binary request body, borrowed for this call.
  size_t body_len;
  long connect_timeout_ms; // 0 uses the 10 second default.
  long timeout_ms;         // 0 uses the 60 second default.
} HttpRequest;

// Clients are reusable, but must not be used concurrently. Each successful
// initialization must be paired with http_client_destroy.
int http_client_init(HttpClient **client);

// response must be initialized to {0} or contain a previous response. On
// success it is replaced; on failure it is unchanged. HTTP error status codes
// are successful transfers and are returned in response->status_code.
// error may be NULL. Redirects are not followed automatically.
int http_client_request(HttpClient *client, const HttpRequest *request,
                        HttpResponse *response, AgentError *error);

// Convenience wrapper for a JSON POST. json must be NUL-terminated.
int http_client_post(HttpClient *client, const char *url, const char *json,
                     HttpResponse *response);

// Frees the body and resets the response. NULL is allowed.
void http_response_free(HttpResponse *response);
int http_client_destroy(HttpClient *client);

#endif
