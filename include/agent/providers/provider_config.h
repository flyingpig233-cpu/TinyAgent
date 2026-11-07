#ifndef TINY_AGENT_PROVIDER_CONFIG_H
#define TINY_AGENT_PROVIDER_CONFIG_H

typedef struct {
  const char *api_key;
  const char *base_url;
  const char *model;
  long connect_timeout_ms; // 0 uses the HTTP client's 10 second default.
  long timeout_ms;         // Total per-request timeout; 0 defaults to 60 seconds.
} ProviderConfig;

#endif // TINY_AGENT_PROVIDER_CINFIG_H
