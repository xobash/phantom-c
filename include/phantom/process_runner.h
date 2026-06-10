#ifndef PHANTOM_PROCESS_RUNNER_H
#define PHANTOM_PROCESS_RUNNER_H
#include "phantom/common.h"
#define PH_PROCESS_OUTPUT_MAX 8192
typedef struct {
    int exit_code;
    bool timed_out;
    char output[PH_PROCESS_OUTPUT_MAX];
} ph_process_result;
bool ph_process_run(const char *command, int timeout_seconds, ph_process_result *result, ph_error *err);
/* Like ph_process_run, but writes `input` to the child's stdin and closes it
 * before collecting output. Pass NULL for no input. */
bool ph_process_run_input(const char *command, const char *input, int timeout_seconds, ph_process_result *result, ph_error *err);
#endif
