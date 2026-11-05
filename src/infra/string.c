#include "agent/infra.h"
#include "agent/status.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static size_t string_grow_capacity(size_t current, size_t required) {
    size_t capacity = current == 0 ? 16 : current;
    while (capacity < required) {
        if (capacity > (SIZE_MAX - 1) / 2) return required;
        capacity *= 2;
    }
    return capacity;
}

int string_init(String *str) {
    if (str == NULL) return AGENT_ERR_NULL_POINTER;
    *str = (String){0};
    return AGENT_OK;
}

void string_destroy(String *str) {
    if (str == NULL) return;
    free(str->data);
    *str = (String){0};
}

int string_reserve(String *str, size_t capacity) {
    if (str == NULL) return AGENT_ERR_NULL_POINTER;
    if (capacity == SIZE_MAX) return AGENT_ERR_OUT_OF_MEMORY;
    if (capacity <= str->capacity) return AGENT_OK;

    size_t grown = string_grow_capacity(str->capacity, capacity);
    char *data = realloc(str->data, grown + 1);
    if (data == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    data[str->length] = '\0';
    str->data = data;
    str->capacity = grown;
    return AGENT_OK;
}

int string_assign(String *str, const char *src) {
    if (str == NULL || src == NULL) return AGENT_ERR_NULL_POINTER;
    size_t length = strlen(src);
    // An internal source already fits, so reserve cannot invalidate it.
    int status = string_reserve(str, length);
    if (status != AGENT_OK) return status;
    if (str->data != NULL) memmove(str->data, src, length + 1);
    str->length = length;
    return AGENT_OK;
}

int string_append(String *str, const char *src) {
    if (str == NULL || src == NULL) return AGENT_ERR_NULL_POINTER;
    size_t added = strlen(src);
    if (added == 0) return AGENT_OK;
    if (added > SIZE_MAX - 1 - str->length) return AGENT_ERR_OUT_OF_MEMORY;
    size_t length = str->length + added;

    if (length <= str->capacity) {
        memmove(str->data + str->length, src, added + 1);
    } else {
        size_t capacity = string_grow_capacity(str->capacity, length);
        char *data = malloc(capacity + 1);
        if (data == NULL) return AGENT_ERR_OUT_OF_MEMORY;
        if (str->length > 0) memcpy(data, str->data, str->length);
        // Keep the old allocation alive until a potentially internal src is copied.
        memcpy(data + str->length, src, added + 1);
        free(str->data);
        str->data = data;
        str->capacity = capacity;
    }
    str->length = length;
    return AGENT_OK;
}

void string_clear(String *str) {
    if (str == NULL) return;
    if (str->data != NULL) str->data[0] = '\0';
    str->length = 0;
}

const char *string_c_str(const String *str) {
    if (str == NULL) return NULL;
    return str->data == NULL ? "" : str->data;
}

int full_copy(char **dest, const char *src) {
    if (src == NULL) return AGENT_ERR_NULL_POINTER;
    size_t len = strlen(src);
    *dest = (char*)malloc(len + 1);
    if (*dest == NULL) return AGENT_ERR_OUT_OF_MEMORY;
    strcpy(*dest, src);
    return AGENT_OK;
}
