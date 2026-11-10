#ifndef TINY_AGENT_CONFIG_H
#define TINY_AGENT_CONFIG_H

#include "agent/event.h"

enum { CONFIG_DEFAULT_MAX_ROUND = 40, CONFIG_DEFAULT_MAX_TOOL_RETRIES = 3 };

// Result of a host approval check before a tool call runs.
typedef enum {
  TOOL_DECISION_ALLOW = 0, // Run this call; ask again for the next one.
  TOOL_DECISION_DENY,      // Skip this call and report an error to the model.
  TOOL_DECISION_ALLOW_ALL  // Run this and all later calls without asking again.
} ToolDecision;

// Called synchronously before each tool call. NULL allows every call (the
// default). The callback sees every call but only `mutating` tools normally
// need approval; it may decide for itself. A denied call becomes a tool error
// result so the model can adapt and continue.
typedef ToolDecision (*ToolApprovalFn)(void *userdata, const char *tool_name,
                                       const char *arguments_json,
                                       int mutating);

typedef struct {
  int max_round;

  // Maximum retries after consecutive failed tool rounds. Zero selects the default.
  int max_tool_retries;

  // When nonzero, request incremental output from the provider and deliver it
  // through on_delta as it arrives.
  int stream;

  // Optional progress callback invoked synchronously during agent_run().
  AgentEventCallback on_event;
  void *userdata; // Passed to on_event and on_delta unchanged; may be NULL.

  // Optional streaming callback: borrowed text delta for the given 1-based
  // round. Only called when stream is set.
  void (*on_delta)(void *userdata, int round, const char *text);

  // Optional tool-approval callback run before each tool call. NULL allows all.
  ToolApprovalFn on_tool_approval;
  void *approval_userdata; // Passed to on_tool_approval unchanged; may be NULL.

  // Optional cooperative cancellation hook, polled before each round, before
  // each tool call and during provider requests and tool subprocesses. A
  // nonzero return aborts the run with AGENT_ERR_CANCELLED. As it is also
  // invoked periodically while waiting, it is a convenient place to pump UI
  // updates. NULL disables cancellation checks.
  int (*is_cancelled)(void *userdata);
  void *cancel_userdata; // Passed to is_cancelled unchanged; may be NULL.
} AgentConfig;

AgentConfig config_default(void);

#endif // TINY_AGENT_CONFIG_H
