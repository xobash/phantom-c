#ifndef PHANTOM_COMMON_H
#define PHANTOM_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PH_ARRAY_LEN(a) (sizeof(a)/sizeof((a)[0]))
typedef enum { PH_OK=0, PH_ERR=1 } ph_status;
typedef struct { int code; char message[512]; } ph_error;
void ph_error_set(ph_error *err, int code, const char *fmt, ...);
char *ph_strdup(const char *s);
char *ph_trim_dup(const char *s);
bool ph_streqi(const char *a, const char *b);
bool ph_contains_i(const char *haystack, const char *needle);
bool ph_starts_i(const char *s, const char *prefix);
bool ph_ends_i(const char *s, const char *suffix);
void ph_slash_normalize(char *s);
#endif
