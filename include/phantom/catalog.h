#ifndef PHANTOM_CATALOG_H
#define PHANTOM_CATALOG_H
#include "phantom/common.h"

typedef struct { int apps, tweaks, features, fixes, panels; } ph_catalog_counts;

/* One row of a catalog listing (tweaks, features, fixes, panels, or apps).
 * `extra` carries the kind-specific detail: featureName for features,
 * category for apps, and is empty otherwise. */
typedef struct {
    char id[128];
    char name[256];
    char description[512];
    char risk[32];
    bool reversible;
    bool destructive;
    char extra[128];
    char sources[96];   /* apps only: available package managers, e.g. "winget, choco" */
} ph_catalog_entry;

/* Called once per catalog entry; return false to stop enumeration early. */
typedef bool (*ph_catalog_entry_fn)(const ph_catalog_entry *entry, void *ctx);

bool ph_catalog_counts_load(const char *data_dir, ph_catalog_counts *counts, ph_error *err);
bool ph_catalog_integrity_validate(const char *data_dir, ph_error *err);
bool ph_catalog_regressions_validate(const char *data_dir, ph_error *err);
bool ph_catalog_has_tweak(const char *data_dir, const char *id);
bool ph_catalog_has_app_display(const char *data_dir, const char *display);

/* Enumerate entries of a catalog file ("tweaks.json", "features.json",
 * "fixes.json", "legacy-panels.json", or "catalog.apps.json"). */
bool ph_catalog_enumerate(const char *data_dir, const char *file, ph_catalog_entry_fn fn, void *ctx, ph_error *err);
#endif
