#ifndef SAFETY_HARNESS_H
#define SAFETY_HARNESS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *sh_malloc(size_t size);
void *sh_calloc(size_t count, size_t size);
void sh_free(void *ptr);
char *sh_strdup(const char *value);

#define SH_ALLOC_BYTES(size) sh_malloc((size))
#define SH_CALLOC(count, size) sh_calloc((count), (size))
#define SH_FREE(ptr) \
    do { \
        sh_free((void *) (ptr)); \
        (ptr) = NULL; \
    } while (0)
#define SH_ARRAY_LEN(value) (sizeof(value) / sizeof((value)[0]))
#define SH_SAFE_RETURN_IF_NULL(ptr, error_code) \
    do { \
        if ((ptr) == NULL) { \
            return (error_code); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
