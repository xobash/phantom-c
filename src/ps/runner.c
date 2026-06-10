#include "phantom/ps_runner.h"
#include "phantom/process_runner.h"
#include "phantom/ps_validator.h"
#include "phantom/restore_point.h"
#include <stdio.h>
#include <string.h>

#define PH_PS_TIMEOUT_SECONDS 180

/* The script is delivered over stdin (`-Command -`), matching the C# runner.
 * This avoids command-line quoting entirely, so scripts cannot break out of
 * the host invocation regardless of the quotes they contain. */
#ifdef _WIN32
#define PH_PS_HOST "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command -"
#else
#define PH_PS_HOST "pwsh -NoLogo -NoProfile -NonInteractive -Command -"
#endif

bool ph_ps_runner_execute(const ph_operation *op, const ph_step *step, bool dry_run, char *output, size_t out_len, ph_error *err) {
    if (!op || !step || !output || out_len == 0) {
        ph_error_set(err, 1, "missing PowerShell execution argument");
        return false;
    }
    output[0] = '\0';
    if (!ph_validate_operation_id(op->id, err)) {
        snprintf(output, out_len, "%s", err ? err->message : "invalid operation id");
        return false;
    }
    if (!ph_validate_powershell_script(step->script, err)) {
        snprintf(output, out_len, "%s", err ? err->message : "invalid script");
        return false;
    }
    if (dry_run) {
        snprintf(output, out_len, "DRY-RUN op=%s step=%s", op->id, step->name);
        return true;
    }
    if (strcmp(op->id, "safety.restore-point") == 0) {
        bool ok = ph_restore_point_create(step->script, err);
        snprintf(output, out_len, "%s", ok ? "restore point created" : (err ? err->message : "restore point creation failed"));
        return ok;
    }
    ph_process_result pr;
    bool ok = ph_process_run_input(PH_PS_HOST, step->script, PH_PS_TIMEOUT_SECONDS, &pr, err);
    snprintf(output, out_len, "%s", pr.output);
    return ok;
}
