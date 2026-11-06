#ifndef TINY_AGENT_TOOL_H
#define TINY_AGENT_TOOL_H

typedef struct {
  char *call_id;
  char *tool_name;
  char *arguments_json;
} ToolCall;

#endif // TINY_AGENT_TOOL_H