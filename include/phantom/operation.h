#ifndef PHANTOM_OPERATION_H
#define PHANTOM_OPERATION_H
#include "phantom/common.h"
#include "phantom/package_manager.h"
#include "phantom/status_parser.h"
typedef enum { PH_RISK_BASIC=0, PH_RISK_ADVANCED=1, PH_RISK_DANGEROUS=2 } ph_risk;
typedef struct { char name[64]; char script[4096]; bool requires_network; } ph_step;
typedef struct { char id[128]; char title[256]; ph_risk risk; bool reversible; bool destructive; bool requires_reboot; ph_step detect; ph_step run[8]; int run_count; ph_step undo[8]; int undo_count; ph_step capture[8]; int capture_count; } ph_operation;
typedef struct { bool dry_run, enable_destructive, force_dangerous, skip_capture_check, confirm_dangerous, create_restore_point; } ph_operation_request;
typedef struct { char operation_id[128]; bool success,cancelled,verification_attempted,verification_passed,capture_failed; char message[512]; } ph_operation_result;
typedef bool (*ph_runner_fn)(const ph_operation *op, const ph_step *step, bool dry_run, char *output, size_t out_len, void *ctx);
bool ph_make_update_operation(const char *mode, ph_operation *op, ph_error *err);
bool ph_make_store_operation(const char *display, const char *action, ph_operation *op, ph_error *err);
bool ph_make_store_operation_with_availability(const char *display, const char *action, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err);
bool ph_make_store_discovery_operation(const char *display, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err);
bool ph_make_store_status_operation(const char *display, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err);
bool ph_make_tweak_operation(const char *id, ph_operation *op, ph_error *err);
bool ph_make_feature_operation(const char *id, ph_operation *op, ph_error *err);
bool ph_make_fix_operation(const char *id, ph_operation *op, ph_error *err);
bool ph_make_panel_operation(const char *id, ph_operation *op, ph_error *err);
/* Override the catalog data directory (default "Data"). */
void ph_operation_set_data_dir(const char *dir);
bool ph_operation_engine_run(ph_operation *ops, int count, ph_operation_request req, ph_runner_fn runner, void *ctx, ph_operation_result *results, int *result_count, ph_error *err);
#endif
