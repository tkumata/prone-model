#include "safety_harness.h"

#include <string.h>
#include <stdint.h>

#include "esp_heap_caps.h"

static bool is_valid_allocation_size(size_t size)
{
    return size > 0 && size <= SIZE_MAX;
}

void *sh_malloc(size_t size)
{
    if (!is_valid_allocation_size(size)) {
        return NULL;
    }

    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *sh_calloc(size_t count, size_t size)
{
    size_t total_size;
    void *ptr;

    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > SIZE_MAX / size) {
        return NULL;
    }

    total_size = count * size;
    ptr = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void sh_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    heap_caps_free(ptr);
}

char *sh_strdup(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }

    length = strlen(value) + 1U;
    copy = (char *) sh_malloc(length);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length);
    return copy;
}
