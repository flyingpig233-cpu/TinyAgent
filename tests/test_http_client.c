#include "agent/http_client.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    HttpClient *client = NULL;
    HttpResponse response = {0};
    AgentError error;
    if (http_client_init(&client) != AGENT_OK) return 2;
    char url[256];
    snprintf(url, sizeof(url), "%s/test", argv[1]);
    const char body[] = {'a', '\0', 'b'};
    const char *headers[] = {"X-Test: hello"};
    HttpRequest request = {.method = "PUT", .url = url, .headers = headers,
                           .header_count = 1, .body = body, .body_len = sizeof(body)};
    if (http_client_request(client, &request, &response, &error) != AGENT_OK) {
        fprintf(stderr, "%s\n", error.message);
        return 3;
    }
    if (response.status_code != 201 || response.body_len != sizeof(body) ||
        memcmp(response.body, body, sizeof(body)) != 0 || response.body[3] != '\0') return 4;
    snprintf(url, sizeof(url), "%s/missing", argv[1]);
    request = (HttpRequest){.url = url};
    if (http_client_request(client, &request, &response, &error) != AGENT_OK ||
        response.status_code != 404 || strcmp(response.body, "missing") != 0) return 5;
    request.url = "file:///etc/passwd";
    if (http_client_request(client, &request, &response, &error) != AGENT_ERR_INVAILD_ARGUMENT ||
        response.status_code != 404 || strcmp(response.body, "missing") != 0) return 6;
    snprintf(url, sizeof(url), "%s/slow", argv[1]);
    request = (HttpRequest){.url = url, .timeout_ms = 50};
    if (http_client_request(client, &request, &response, &error) != AGENT_ERR_HTTP_TIMEOUT ||
        response.status_code != 404 || error.message[0] == '\0') return 7;
    snprintf(url, sizeof(url), "%s/json", argv[1]);
    if (http_client_post(client, url, "{}", &response) != AGENT_OK ||
        response.status_code != 204 || response.body_len != 0 || response.body[0] != '\0') return 8;
    http_response_free(&response);
    if (http_client_destroy(client) != AGENT_OK) return 9;
    return 0;
}
