#include "phantom/catalog.h"
#include "phantom/jsonlite.h"
#include "phantom/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void path_join(char *out, size_t n, const char *dir, const char *file) {
    snprintf(out, n, "%s/%s", dir, file);
}

/* Count top-level array objects that carry the given string key. */
static int count_entries(const char *dir, const char *file, const char *key, ph_error *err) {
    char path[512];
    path_join(path, sizeof path, dir, file);
    char *txt = ph_read_all_file(path);
    if (!txt) { ph_error_set(err, 1, "missing %s", path); return -1; }
    int count = 0;
    const char *cursor = txt, *start, *end;
    char value[256];
    while (ph_json_next_object(&cursor, &start, &end)) {
        if (ph_json_string_field(start, end, key, value, sizeof value)) count++;
    }
    free(txt);
    return count;
}

bool ph_catalog_counts_load(const char *dir, ph_catalog_counts *c, ph_error *err) {
    memset(c, 0, sizeof *c);
    struct { const char *file, *key; int *slot; } files[] = {
        {"catalog.apps.json", "displayName", &c->apps},
        {"tweaks.json", "id", &c->tweaks},
        {"features.json", "id", &c->features},
        {"fixes.json", "id", &c->fixes},
        {"legacy-panels.json", "id", &c->panels},
    };
    for (size_t i = 0; i < PH_ARRAY_LEN(files); i++) {
        int n = count_entries(dir, files[i].file, files[i].key, err);
        if (n < 0) return false;
        *files[i].slot = n;
    }
    return true;
}

bool ph_catalog_integrity_validate(const char *dir, ph_error *err) {
    static const struct { const char *file, *hash; } expected[] = {
        {"catalog.apps.json", "c7c9dd51c6d20adb0f22539a969d371ae76068b801c1a960e0443dbacbb35a7f"},
        {"features.json", "2182f2172ef035a5cc2995dbccd4172e5ccc91957f6dd1efdfddceb5cbd87462"},
        {"fixes.json", "2c63d665d1c959ef42e38f0425f12a8112ef6e16bdcd5b46a67edf52f66db904"},
        {"legacy-panels.json", "687791e8c17ddc6e90f00665851a08e0a501cb0ddcadf0b8fbf77c56fd38feb4"},
        {"tweaks.json", "8b6bc35fa6f67fdc63bbad9ee35fc20e6f3be17bf417dca044611ab29a7db80d"},
    };
    char path[512], hex[65];
    for (size_t i = 0; i < PH_ARRAY_LEN(expected); i++) {
        path_join(path, sizeof path, dir, expected[i].file);
        if (!ph_sha256_file_hex(path, hex) || strcmp(hex, expected[i].hash) != 0) {
            ph_error_set(err, 1, "integrity check failed for %s", expected[i].file);
            return false;
        }
    }
    return true;
}

static bool catalog_has_entry(const char *dir, const char *file, const char *field, const char *value) {
    char path[512];
    path_join(path, sizeof path, dir, file);
    ph_error err = {0};
    char *txt = NULL;
    const char *start, *end;
    if (!ph_json_load_object_by_field(path, NULL, field, value, &txt, &start, &end, &err)) return false;
    free(txt);
    return true;
}

bool ph_catalog_has_tweak(const char *dir, const char *id) {
    return catalog_has_entry(dir, "tweaks.json", "id", id);
}

bool ph_catalog_has_app_display(const char *dir, const char *display) {
    return catalog_has_entry(dir, "catalog.apps.json", "displayName", display);
}

bool ph_catalog_regressions_validate(const char *dir, ph_error *err) {
    static const char *required_tweaks[] = {
        "disable-recall", "disable-advertising-id", "disable-location-tracking",
        "remove-widgets", "disable-wpbt",
    };
    for (size_t i = 0; i < PH_ARRAY_LEN(required_tweaks); i++) {
        if (!ph_catalog_has_tweak(dir, required_tweaks[i])) {
            ph_error_set(err, 1, "required tweak missing: %s", required_tweaks[i]);
            return false;
        }
    }
    char path[512];
    path_join(path, sizeof path, dir, "tweaks.json");
    char *txt = ph_read_all_file(path);
    if (!txt) { ph_error_set(err, 1, "missing tweaks.json"); return false; }
    bool ok = true;
    if (!strstr(txt, "HKLM:\\\\SOFTWARE\\\\Policies\\\\Microsoft\\\\Windows\\\\DeliveryOptimization") ||
        strstr(txt, "CurrentVersion\\\\DeliveryOptimization\\\\Config")) {
        ph_error_set(err, 1, "delivery optimization regression");
        ok = false;
    } else if (!strstr(txt, "HibernateEnabled") || strstr(txt, "powercfg /a")) {
        ph_error_set(err, 1, "hibernation regression");
        ok = false;
    }
    free(txt);
    return ok;
}

bool ph_catalog_enumerate(const char *dir, const char *file, ph_catalog_entry_fn fn, void *ctx, ph_error *err) {
    char path[512];
    path_join(path, sizeof path, dir, file);
    char *txt = ph_read_all_file(path);
    if (!txt) { ph_error_set(err, 1, "missing %s", path); return false; }
    bool is_apps = strcmp(file, "catalog.apps.json") == 0;
    bool is_features = strcmp(file, "features.json") == 0;
    const char *cursor = txt, *start, *end;
    ph_catalog_entry e;
    while (ph_json_next_object(&cursor, &start, &end)) {
        memset(&e, 0, sizeof e);
        if (is_apps) {
            if (!ph_json_string_field(start, end, "displayName", e.id, sizeof e.id)) continue;
            snprintf(e.name, sizeof e.name, "%s", e.id);
            ph_json_string_field(start, end, "category", e.extra, sizeof e.extra);
        } else {
            if (!ph_json_string_field(start, end, "id", e.id, sizeof e.id)) continue;
            ph_json_string_field(start, end, "name", e.name, sizeof e.name);
            if (is_features) ph_json_string_field(start, end, "featureName", e.extra, sizeof e.extra);
        }
        ph_json_string_field(start, end, "description", e.description, sizeof e.description);
        ph_json_string_field(start, end, "riskTier", e.risk, sizeof e.risk);
        e.reversible = ph_json_bool_field(start, end, "reversible");
        e.destructive = ph_json_bool_field(start, end, "destructive");
        if (!fn(&e, ctx)) break;
    }
    free(txt);
    return true;
}
