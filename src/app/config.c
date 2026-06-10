#include "phantom/config.h"
#include "phantom/jsonlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_key(const char *json, const char *key) {
    char needle[96];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    p = ph_json_skip_ws(p);
    if (*p != ':') return NULL;
    return ph_json_skip_ws(p + 1);
}

static bool parse_json_string_at(const char **pp, char *out, size_t out_len, ph_error *err) {
    const char *p = ph_json_skip_ws(*pp);
    if (*p != '"') { ph_error_set(err, 2, "expected JSON string"); return false; }
    p++;
    size_t j = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == '"' || c == '\\' || c == '/') { }
            else { ph_error_set(err, 2, "unsupported JSON escape"); return false; }
        }
        if (j + 1 >= out_len) { ph_error_set(err, 2, "JSON string too long"); return false; }
        out[j++] = c;
    }
    if (*p != '"') { ph_error_set(err, 2, "unterminated JSON string"); return false; }
    out[j] = '\0';
    *pp = p + 1;
    return true;
}

static bool parse_bool_key(const char *json, const char *key, bool *value) {
    const char *p = find_key(json, key);
    if (!p) return true;
    if (ph_starts_i(p, "true")) { *value = true; return true; }
    if (ph_starts_i(p, "false")) { *value = false; return true; }
    return false;
}

static bool parse_string_key(const char *json, const char *key, char *out, size_t out_len, ph_error *err) {
    const char *p = find_key(json, key);
    if (!p) { out[0] = '\0'; return true; }
    return parse_json_string_at(&p, out, out_len, err);
}

static bool parse_string_array_key(const char *json, const char *key, char out[PH_CONFIG_MAX_ITEMS][PH_CONFIG_MAX_TEXT], int *count, ph_error *err) {
    *count = 0;
    const char *p = find_key(json, key);
    if (!p) return true;
    if (*p != '[') { ph_error_set(err, 2, "%s must be an array", key); return false; }
    p++;
    for (;;) {
        p = ph_json_skip_ws(p);
        if (*p == ']') return true;
        if (*count >= PH_CONFIG_MAX_ITEMS) { ph_error_set(err, 2, "%s has too many items", key); return false; }
        if (!parse_json_string_at(&p, out[*count], PH_CONFIG_MAX_TEXT, err)) return false;
        (*count)++;
        p = ph_json_skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') return true;
        ph_error_set(err, 2, "malformed array for %s", key);
        return false;
    }
}

bool ph_config_parse_json(const char *json, ph_automation_config *cfg, ph_error *err) {
    if (!json || !cfg) { ph_error_set(err, 2, "missing config JSON"); return false; }
    memset(cfg, 0, sizeof *cfg);
    const char *p = ph_json_skip_ws(json);
    if (*p != '{') { ph_error_set(err, 2, "config must be a JSON object"); return false; }
    if (!parse_bool_key(json, "confirmDangerous", &cfg->confirm_dangerous)) { ph_error_set(err, 2, "confirmDangerous must be boolean"); return false; }
    if (!parse_string_key(json, "dangerousAcknowledgement", cfg->acknowledgement, sizeof cfg->acknowledgement, err)) return false;
    if (!parse_string_array_key(json, "storeSelections", cfg->store, &cfg->store_count, err)) return false;
    if (!parse_string_array_key(json, "tweaks", cfg->tweaks, &cfg->tweak_count, err)) return false;
    if (!parse_string_array_key(json, "features", cfg->features, &cfg->feature_count, err)) return false;
    if (!parse_string_array_key(json, "fixes", cfg->fixes, &cfg->fix_count, err)) return false;
    if (!parse_string_key(json, "updateMode", cfg->update_mode, sizeof cfg->update_mode, err)) return false;
    return true;
}

bool ph_config_load_file(const char *path, ph_automation_config *cfg, ph_error *err) {
    char *text = ph_read_all_file(path);
    if (!text) { ph_error_set(err, 2, "failed to read config: %s", path ? path : "(null)"); return false; }
    bool ok = ph_config_parse_json(text, cfg, err);
    free(text);
    return ok;
}

static bool append_op(ph_operation *ops, int max_ops, int *count, const ph_operation *op, ph_error *err) {
    if (*count >= max_ops) { ph_error_set(err, 6, "too many operations generated"); return false; }
    ops[*count] = *op;
    (*count)++;
    return true;
}

bool ph_config_build_operations_with_availability(const ph_automation_config *cfg, const ph_store_manager_availability *availability, ph_operation *ops, int max_ops, int *count, ph_error *err) {
    if (!cfg || !ops || !count) { ph_error_set(err, 6, "missing operation build argument"); return false; }
    *count = 0;
    ph_operation op;
    for (int i = 0; i < cfg->store_count; i++) { if (!ph_make_store_operation_with_availability(cfg->store[i], "install", availability, &op, err) || !append_op(ops, max_ops, count, &op, err)) return false; }
    for (int i = 0; i < cfg->tweak_count; i++) { if (!ph_make_tweak_operation(cfg->tweaks[i], &op, err) || !append_op(ops, max_ops, count, &op, err)) return false; }
    for (int i = 0; i < cfg->feature_count; i++) { if (!ph_make_feature_operation(cfg->features[i], &op, err) || !append_op(ops, max_ops, count, &op, err)) return false; }
    for (int i = 0; i < cfg->fix_count; i++) { if (!ph_make_fix_operation(cfg->fixes[i], &op, err) || !append_op(ops, max_ops, count, &op, err)) return false; }
    if (cfg->update_mode[0]) { if (!ph_make_update_operation(cfg->update_mode, &op, err) || !append_op(ops, max_ops, count, &op, err)) return false; }
    return true;
}

bool ph_config_build_operations(const ph_automation_config *cfg, ph_operation *ops, int max_ops, int *count, ph_error *err) {
    return ph_config_build_operations_with_availability(cfg, NULL, ops, max_ops, count, err);
}

bool ph_config_has_dangerous_selection(const ph_automation_config *cfg) {
    if (!cfg) return false;
    for (int i = 0; i < cfg->tweak_count; i++) if (ph_streqi(cfg->tweaks[i], "remove-onedrive")) return true;
    if (ph_streqi(cfg->update_mode, "Disable All") || ph_streqi(cfg->update_mode, "DisableAll")) return true;
    return false;
}

bool ph_config_dangerous_gate(const ph_automation_config *cfg, bool force_dangerous, const char *ack_arg, ph_error *err) {
    if (!ph_config_has_dangerous_selection(cfg)) return true;
    const char *ack = (ack_arg && *ack_arg) ? ack_arg : cfg->acknowledgement;
    if (!cfg->confirm_dangerous || !force_dangerous || !ph_streqi(ack, "I_UNDERSTAND_NO_ROLLBACK")) {
        ph_error_set(err, 3, "dangerous automation requires confirmDangerous, -ForceDangerous, and acknowledgement token");
        return false;
    }
    return true;
}
