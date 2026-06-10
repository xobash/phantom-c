#ifndef PHANTOM_SANITIZER_H
#define PHANTOM_SANITIZER_H
#include "phantom/common.h"
bool ph_ensure_service_name(const char *input, char *out, size_t out_len, ph_error *err);
bool ph_safe_package_id(const char *s);
bool ph_safe_feature_name(const char *s);
#endif
