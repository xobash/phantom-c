#ifndef PHANTOM_COMPAT_H
#define PHANTOM_COMPAT_H
#include <stdbool.h>
typedef struct { int major, minor, build, rev; } ph_version;
bool ph_compatible(const char **tokens, int count, ph_version v);
#endif
