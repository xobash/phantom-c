#ifndef PHANTOM_STATUS_PARSER_H
#define PHANTOM_STATUS_PARSER_H
#include <stdbool.h>
typedef enum { PH_STATUS_UNKNOWN=0, PH_STATUS_APPLIED=1, PH_STATUS_NOT_APPLIED=2 } ph_operation_status;
ph_operation_status ph_parse_operation_status(const char *output);
bool ph_status_is_applied(const char *output);
const char *ph_status_name(ph_operation_status st);
#endif
