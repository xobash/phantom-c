#ifndef PHANTOM_CLI_H
#define PHANTOM_CLI_H
#include "phantom/common.h"
typedef struct { const char *config_path; bool run, force_dangerous, skip_capture_check, dry_run, validate_catalogs, list; const char *ack; } ph_cli_options;
bool ph_cli_parse(int argc, char **argv, ph_cli_options *opt, ph_error *err);
bool ph_cli_validate_config_path(const char *base_runtime, const char *path, char *resolved, size_t len, ph_error *err);
#endif
