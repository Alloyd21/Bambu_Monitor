#pragma once
#include <stdlib.h>
#include <string.h>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#ifdef _WIN32
static inline char* strsep(char** s, const char* delim) {
    char* start = *s;
    if (!start) return NULL;
    char* end = start + strcspn(start, delim);
    *s = *end ? end + 1 : NULL;
    *end = '\0';
    return start;
}
#endif
