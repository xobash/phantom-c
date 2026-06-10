#ifndef PHANTOM_PS_VALIDATOR_H
#define PHANTOM_PS_VALIDATOR_H
#include "phantom/common.h"
bool ph_is_encoded_command_alias(const char *param);
bool ph_validate_powershell_script(const char *script, ph_error *err);
bool ph_validate_operation_id(const char *opid, ph_error *err);
#endif
