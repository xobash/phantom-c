#ifndef PHANTOM_RESTORE_POINT_H
#define PHANTOM_RESTORE_POINT_H
#include "phantom/common.h"
#define PH_RESTORE_POINT_DESCRIPTION_MAX 221
bool ph_restore_point_description(const char *operation_id, char *out, size_t out_len, ph_error *err);
bool ph_restore_point_create(const char *description, ph_error *err);
#endif
