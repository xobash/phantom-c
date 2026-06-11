/* Native data providers for the Phantom GUI: installed applications
 * (registry uninstall keys), Windows services (SCM), and live system
 * metrics. Windows-only. */
#ifndef PHANTOM_GUI_NATIVE_H
#define PHANTOM_GUI_NATIVE_H
#ifdef _WIN32

#include <stdbool.h>
#include <stddef.h>

/* ----- Installed applications (parity with HomeDataService) ----- */

typedef struct {
    char name[256];
    char version[64];
    char publisher[128];
    char uninstall[1024];        /* QuietUninstallString preferred */
    char install_location[512];
} ph_installed_app;

/* Enumerates HKLM (64-bit + 32-bit views) and HKCU uninstall keys,
 * skipping hidden entries (SystemComponent=1 or no display name).
 * Returns a malloc'd array the caller frees; sorted by name. */
ph_installed_app *ph_installed_apps_list(int *count);

/* ----- Windows services ----- */

typedef struct {
    char name[256];
    char display[256];
    char state[32];              /* Running / Stopped / ... */
    char start_type[32];         /* Automatic / Manual / Disabled */
} ph_service_row;

ph_service_row *ph_services_list(int *count);

/* action: "start" | "stop" | "restart".
 * Writes a human-readable result and returns success. */
bool ph_service_control(const char *name, const char *action, char *msg, size_t msg_len);

/* startup: "auto" | "manual" | "disabled" */
bool ph_service_set_startup(const char *name, const char *startup, char *msg, size_t msg_len);

/* ----- Live system metrics ----- */

/* Returns total CPU usage 0..100, computed from GetSystemTimes deltas.
 * Call once per second; the first call returns 0. */
int ph_cpu_usage_percent(void);

int ph_process_count(void);

#endif /* _WIN32 */
#endif
