#include "phantom/catalog.h"
#include "phantom/cli.h"
#include "phantom/config.h"
#include "phantom/ps_runner.h"
#include "phantom/ps_validator.h"
#include "phantom/package_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cli_runner(const ph_operation *op, const ph_step *step, bool dry_run, char *output, size_t out_len, void *ctx) {
    (void)ctx;
    ph_error err = {0};
    bool ok = ph_ps_runner_execute(op, step, dry_run, output, out_len, &err);
    printf("%s %s :: %s\n", dry_run ? "DRY-RUN" : "RUN", op->id, step->name);
    return ok;
}

/* Prefer ./Data; fall back to Data next to the executable so the CLI works
 * regardless of the caller's current directory. */
static void resolve_data_dir(const char *argv0, char *out, size_t n) {
    FILE *f = fopen("Data/tweaks.json", "rb");
    if (f) { fclose(f); snprintf(out, n, "Data"); return; }
    snprintf(out, n, "%s", argv0 ? argv0 : "");
    ph_slash_normalize(out);
    char *slash = strrchr(out, '/');
    if (slash && (size_t)(slash + 1 - out) < n) snprintf(slash + 1, n - (size_t)(slash + 1 - out), "Data");
    else snprintf(out, n, "Data");
}

int main(int argc, char **argv) {
    ph_error err = {0};
    ph_cli_options opt;
    if (!ph_cli_parse(argc, argv, &opt, &err)) {
        fprintf(stderr, "%s\n", err.message);
        return 2;
    }
    char data[512];
    resolve_data_dir(argv[0], data, sizeof data);
    ph_operation_set_data_dir(data);
    if (opt.validate_catalogs) {
        ph_catalog_counts c;
        if (!ph_catalog_counts_load(data, &c, &err) || !ph_catalog_integrity_validate(data, &err) || !ph_catalog_regressions_validate(data, &err)) {
            fprintf(stderr, "%s\n", err.message);
            return 1;
        }
        printf("catalogs ok: apps=%d tweaks=%d features=%d fixes=%d panels=%d\n", c.apps, c.tweaks, c.features, c.fixes, c.panels);
        return 0;
    }
    if (opt.list) {
        ph_catalog_counts c;
        if (!ph_catalog_counts_load(data, &c, &err)) {
            fprintf(stderr, "%s\n", err.message);
            return 1;
        }
        printf("Phantom C port catalog counts: apps=%d tweaks=%d features=%d fixes=%d panels=%d\n", c.apps, c.tweaks, c.features, c.fixes, c.panels);
        return 0;
    }
    if (opt.run) {
        char path[512];
        if (!ph_cli_validate_config_path(".", opt.config_path, path, sizeof path, &err)) {
            fprintf(stderr, "%s\n", err.message);
            return err.code ? err.code : 2;
        }
        ph_automation_config cfg;
        if (!ph_config_load_file(path, &cfg, &err)) {
            fprintf(stderr, "%s\n", err.message);
            return err.code ? err.code : 2;
        }
        if (!ph_config_dangerous_gate(&cfg, opt.force_dangerous, opt.ack, &err)) {
            fprintf(stderr, "%s\n", err.message);
            return err.code ? err.code : 3;
        }
        ph_operation *ops = (ph_operation *)calloc(256, sizeof *ops);
        ph_operation_result *results = (ph_operation_result *)calloc(512, sizeof *results);
        if (!ops || !results) {
            free(ops);
            free(results);
            fprintf(stderr, "allocation failed\n");
            return 1;
        }
        ph_store_manager_availability availability;
        const ph_store_manager_availability *availability_ptr = NULL;
        if (!opt.dry_run) {
            if (!ph_store_manager_availability_get(NULL, &availability)) {
                fprintf(stderr, "failed to resolve package manager availability\n");
                free(ops);
                free(results);
                return 1;
            }
            availability_ptr = &availability;
        }
        int op_count = 0;
        if (!ph_config_build_operations_with_availability(&cfg, availability_ptr, ops, 256, &op_count, &err)) {
            fprintf(stderr, "%s\n", err.message);
            free(ops);
            free(results);
            return err.code ? err.code : 6;
        }
        int result_count = 0;
        ph_operation_request req = {0};
        req.dry_run = opt.dry_run;
        req.enable_destructive = true;
        req.force_dangerous = opt.force_dangerous;
        req.skip_capture_check = opt.skip_capture_check;
        req.confirm_dangerous = !ph_config_has_dangerous_selection(&cfg) || opt.force_dangerous;
        bool ok = ph_operation_engine_run(ops, op_count, req, cli_runner, NULL, results, &result_count, &err);
        for (int i = 0; i < result_count; i++) printf("%s: %s\n", results[i].operation_id, results[i].success ? "success" : results[i].message);
        free(ops);
        free(results);
        return ok ? 0 : 1;
    }
    printf("Phantom C port core. Use --validate-catalogs, --list, or -Config <path> -Run [--dry-run].\n");
    return 0;
}
