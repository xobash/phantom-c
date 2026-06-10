#ifndef PHANTOM_CONFIG_H
#define PHANTOM_CONFIG_H
#include "phantom/common.h"
#include "phantom/operation.h"
#define PH_CONFIG_MAX_ITEMS 64
#define PH_CONFIG_MAX_TEXT 128
typedef struct {
    bool confirm_dangerous;
    char acknowledgement[PH_CONFIG_MAX_TEXT];
    char store[PH_CONFIG_MAX_ITEMS][PH_CONFIG_MAX_TEXT]; int store_count;
    char tweaks[PH_CONFIG_MAX_ITEMS][PH_CONFIG_MAX_TEXT]; int tweak_count;
    char features[PH_CONFIG_MAX_ITEMS][PH_CONFIG_MAX_TEXT]; int feature_count;
    char fixes[PH_CONFIG_MAX_ITEMS][PH_CONFIG_MAX_TEXT]; int fix_count;
    char update_mode[PH_CONFIG_MAX_TEXT];
} ph_automation_config;
bool ph_config_parse_json(const char *json, ph_automation_config *cfg, ph_error *err);
bool ph_config_load_file(const char *path, ph_automation_config *cfg, ph_error *err);
bool ph_config_build_operations(const ph_automation_config *cfg, ph_operation *ops, int max_ops, int *count, ph_error *err);
bool ph_config_build_operations_with_availability(const ph_automation_config *cfg, const ph_store_manager_availability *availability, ph_operation *ops, int max_ops, int *count, ph_error *err);
bool ph_config_has_dangerous_selection(const ph_automation_config *cfg);
bool ph_config_dangerous_gate(const ph_automation_config *cfg, bool force_dangerous, const char *ack_arg, ph_error *err);
#endif
