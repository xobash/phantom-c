#ifndef PHANTOM_PS_RUNNER_H
#define PHANTOM_PS_RUNNER_H
#include "phantom/common.h"
#include "phantom/operation.h"
bool ph_ps_runner_execute(const ph_operation *op, const ph_step *step, bool dry_run, char *output, size_t out_len, ph_error *err);
#endif
