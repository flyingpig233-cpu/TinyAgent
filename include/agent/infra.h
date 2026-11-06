#ifndef TINY_AGENT_INFRA_H
#define TINY_AGENT_INFRA_H

#include "agent/status.h"
#include <stddef.h>

// Owns a NUL-terminated string. length and capacity exclude the terminator.
// Initialize with {0} or string_init before use. Do not shallow-copy ownership.
typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} String;

// Initialize a fresh object; destroy an existing string before reinitializing.
int string_init(String *str);
void string_destroy(String *str);

// Reserve space for at least capacity characters without changing the content.
// Allocation failures leave the string unchanged.
int string_reserve(String *str, size_t capacity);

// src must be NUL-terminated; it may refer to the string's own content.
// Allocation failures leave the string unchanged. NULL src is an error.
int string_assign(String *str, const char *src);
int string_append(String *str, const char *src);

// Clear the content while retaining the allocation for reuse.
void string_clear(String *str);

// Returns "" for an unallocated empty string, NULL for a NULL object.
// The pointer is borrowed and may be invalidated by mutation or destruction.
const char *string_c_str(const String *str);

int full_copy(char **dest, const char *str);

#endif // TINY_AGENT_INFRA_H
