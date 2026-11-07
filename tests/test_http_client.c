#include "agent/http_client.h"
#include <stdio.h>
#include <string.h>

static size_t reject_body(void *userdata, const char *data, size_t length) {
  (void)userdata;
  (void)data;
  (void)length;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  HttpClient *client = NULL;
  HttpResponse response = {0};
  AgentError error;
  if (http_client_init(&client) != AGENT_OK)
    return 2;
  char url[256];
  snprintf(url, sizeof(url), "%s/test", argv[1]);
  const char body[] = {'a', '\0', 'b'};
  const char *headers[] = {"X-Test: hello"};
  HttpRequest request = {.method = "PUT",
                         .url = url,
                         .headers = headers,
                         .header_count = 1,
                         .body = body,
                         .body_len = sizeof(body)};
  if (http_client_request(client, &request, &response, &error) != AGENT_OK) {
    fprintf(stderr, "%s\n", error.message);
    return 3;
  }
  if (response.status_code != 201 || response.body_len != sizeof(body) ||
      memcmp(response.body, body, sizeof(body)) != 0 ||
      response.body[3] != '\0')
    return 4;
  snprintf(url, sizeof(url), "%s/missing", argv[1]);
  request = (HttpRequest){.url = url};
  if (http_client_request(client, &request, &response, &error) != AGENT_OK ||
      response.status_code != 404 || strcmp(response.body, "missing") != 0)
    return 5;
  request.url = "file:///etc/passwd";
  if (http_client_request(client, &request, &response, &error) !=
          AGENT_ERR_INVAILD_ARGUMENT ||
      response.status_code != 404 || strcmp(response.body, "missing") != 0)
    return 6;
  snprintf(url, sizeof(url), "%s/slow", argv[1]);
  request = (HttpRequest){.url = url, .timeout_ms = 50};
  if (http_client_request(client, &request, &response, &error) !=
          AGENT_ERR_HTTP_TIMEOUT ||
      response.status_code != 404 || error.message[0] == '\0')
    return 7;
  snprintf(url, sizeof(url), "%s/json", argv[1]);
  if (http_client_post(client, url, "{}", &response) != AGENT_OK ||
      response.status_code != 204 || response.body_len != 0 ||
      response.body[0] != '\0')
    return 8;
  /* Cross multiple response-buffer growth boundaries, including binary data. */
  char large_body[65537];
  for (size_t i = 0; i < sizeof(large_body); ++i)
    large_body[i] = (char)(i % 256);
  snprintf(url, sizeof(url), "%s/test", argv[1]);
  request = (HttpRequest){.method = "PUT",
                          .url = url,
                          .headers = headers,
                          .header_count = 1,
                          .body = large_body,
                          .body_len = sizeof(large_body)};
  if (http_client_request(client, &request, &response, &error) != AGENT_OK ||
      response.status_code != 201 ||
      response.body_len != sizeof(large_body) ||
      memcmp(response.body, large_body, sizeof(large_body)) != 0 ||
      response.body[sizeof(large_body)] != '\0')
    return 10;
  /* A consumer rejecting bytes is not a transport allocation failure. */
  request = (HttpRequest){.url = url};
  long status_code = 123;
  if (http_client_request_stream(client, &request, reject_body, NULL,
                                 &status_code, &error) != AGENT_ERR_HTTP ||
      status_code != 123)
    return 11;
  http_response_free(&response);
  if (http_client_destroy(client) != AGENT_OK)
    return 9;
  return 0;
}
