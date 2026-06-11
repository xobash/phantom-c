#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_native.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void wide_to_utf8(const wchar_t *w, char *out, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, out, (int)n, NULL, NULL) && n) out[0] = '\0';
}

static bool reg_read_string(HKEY key, const wchar_t *name, char *out, size_t out_len) {
    wchar_t buf[1024];
    DWORD len = sizeof buf;
    DWORD type = 0;
    out[0] = '\0';
    if (RegQueryValueExW(key, name, NULL, &type, (LPBYTE)buf, &len) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    buf[(len / sizeof(wchar_t) < 1023) ? len / sizeof(wchar_t) : 1023] = L'\0';
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[1024];
        if (ExpandEnvironmentStringsW(buf, expanded, 1024)) {
            wide_to_utf8(expanded, out, out_len);
            return out[0] != '\0';
        }
    }
    wide_to_utf8(buf, out, out_len);
    return out[0] != '\0';
}

static bool reg_read_dword(HKEY key, const wchar_t *name, DWORD *out) {
    DWORD len = sizeof *out;
    DWORD type = 0;
    return RegQueryValueExW(key, name, NULL, &type, (LPBYTE)out, &len) == ERROR_SUCCESS && type == REG_DWORD;
}

/* ------------------------------------------------------------------ */
/* Installed applications                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    ph_installed_app *items;
    int count, cap;
} app_vec;

static void app_vec_add(app_vec *v, const ph_installed_app *a) {
    if (v->count == v->cap) {
        int cap = v->cap ? v->cap * 2 : 64;
        void *p = realloc(v->items, (size_t)cap * sizeof *v->items);
        if (!p) return;
        v->items = (ph_installed_app *)p;
        v->cap = cap;
    }
    v->items[v->count++] = *a;
}

static bool already_listed(const app_vec *v, const char *name) {
    for (int i = 0; i < v->count; i++)
        if (_stricmp(v->items[i].name, name) == 0) return true;
    return false;
}

static void scan_uninstall_root(app_vec *v, HKEY root, REGSAM view) {
    HKEY key;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_READ | view, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        wchar_t sub[256];
        DWORD sub_len = 256;
        if (RegEnumKeyExW(key, i, sub, &sub_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY app_key;
        if (RegOpenKeyExW(key, sub, 0, KEY_READ | view, &app_key) != ERROR_SUCCESS) continue;

        ph_installed_app a;
        memset(&a, 0, sizeof a);
        DWORD system_component = 0;
        bool hidden =
            !reg_read_string(app_key, L"DisplayName", a.name, sizeof a.name) ||
            (reg_read_dword(app_key, L"SystemComponent", &system_component) && system_component == 1);
        if (!hidden) {
            char release_type[64] = {0}, parent[64] = {0};
            reg_read_string(app_key, L"ReleaseType", release_type, sizeof release_type);
            reg_read_string(app_key, L"ParentKeyName", parent, sizeof parent);
            if (release_type[0] || parent[0]) hidden = true; /* hotfixes/updates */
        }
        if (!hidden && !already_listed(v, a.name)) {
            reg_read_string(app_key, L"DisplayVersion", a.version, sizeof a.version);
            reg_read_string(app_key, L"Publisher", a.publisher, sizeof a.publisher);
            if (!reg_read_string(app_key, L"QuietUninstallString", a.uninstall, sizeof a.uninstall))
                reg_read_string(app_key, L"UninstallString", a.uninstall, sizeof a.uninstall);
            reg_read_string(app_key, L"InstallLocation", a.install_location, sizeof a.install_location);
            app_vec_add(v, &a);
        }
        RegCloseKey(app_key);
    }
    RegCloseKey(key);
}

static int app_cmp(const void *x, const void *y) {
    return _stricmp(((const ph_installed_app *)x)->name, ((const ph_installed_app *)y)->name);
}

ph_installed_app *ph_installed_apps_list(int *count) {
    app_vec v = {0};
    scan_uninstall_root(&v, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
    scan_uninstall_root(&v, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    scan_uninstall_root(&v, HKEY_CURRENT_USER, 0);
    if (v.items) qsort(v.items, (size_t)v.count, sizeof *v.items, app_cmp);
    *count = v.count;
    return v.items;
}

/* ------------------------------------------------------------------ */
/* Services                                                            */
/* ------------------------------------------------------------------ */

static const char *state_name(DWORD s) {
    switch (s) {
        case SERVICE_RUNNING: return "Running";
        case SERVICE_STOPPED: return "Stopped";
        case SERVICE_PAUSED: return "Paused";
        case SERVICE_START_PENDING: return "Starting";
        case SERVICE_STOP_PENDING: return "Stopping";
        case SERVICE_CONTINUE_PENDING: return "Resuming";
        case SERVICE_PAUSE_PENDING: return "Pausing";
        default: return "Unknown";
    }
}

static const char *start_type_name(DWORD t) {
    switch (t) {
        case SERVICE_AUTO_START: return "Automatic";
        case SERVICE_DEMAND_START: return "Manual";
        case SERVICE_DISABLED: return "Disabled";
        case SERVICE_BOOT_START: return "Boot";
        case SERVICE_SYSTEM_START: return "System";
        default: return "Unknown";
    }
}

static void fill_start_type(SC_HANDLE scm, const wchar_t *name, char *out, size_t out_len) {
    snprintf(out, out_len, "Unknown");
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_CONFIG);
    if (!svc) return;
    BYTE buf[8192];
    DWORD needed = 0;
    if (QueryServiceConfigW(svc, (LPQUERY_SERVICE_CONFIGW)buf, sizeof buf, &needed))
        snprintf(out, out_len, "%s", start_type_name(((LPQUERY_SERVICE_CONFIGW)buf)->dwStartType));
    CloseServiceHandle(svc);
}

static int service_cmp(const void *x, const void *y) {
    return _stricmp(((const ph_service_row *)x)->display, ((const ph_service_row *)y)->display);
}

ph_service_row *ph_services_list(int *count) {
    *count = 0;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return NULL;

    DWORD bytes = 0, returned = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          NULL, 0, &bytes, &returned, &resume, NULL);
    BYTE *buf = (BYTE *)malloc(bytes);
    if (!buf) { CloseServiceHandle(scm); return NULL; }
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                               buf, bytes, &bytes, &returned, &resume, NULL)) {
        free(buf);
        CloseServiceHandle(scm);
        return NULL;
    }

    ENUM_SERVICE_STATUS_PROCESSW *svcs = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
    ph_service_row *rows = (ph_service_row *)calloc(returned ? returned : 1, sizeof *rows);
    if (!rows) { free(buf); CloseServiceHandle(scm); return NULL; }
    int n = 0;
    for (DWORD i = 0; i < returned; i++) {
        ph_service_row *r = &rows[n];
        wide_to_utf8(svcs[i].lpServiceName, r->name, sizeof r->name);
        wide_to_utf8(svcs[i].lpDisplayName, r->display, sizeof r->display);
        snprintf(r->state, sizeof r->state, "%s", state_name(svcs[i].ServiceStatusProcess.dwCurrentState));
        fill_start_type(scm, svcs[i].lpServiceName, r->start_type, sizeof r->start_type);
        n++;
    }
    free(buf);
    CloseServiceHandle(scm);
    qsort(rows, (size_t)n, sizeof *rows, service_cmp);
    *count = n;
    return rows;
}

static bool wait_for_state(SC_HANDLE svc, DWORD want, int timeout_s) {
    SERVICE_STATUS_PROCESS st;
    DWORD needed;
    for (int i = 0; i < timeout_s * 10; i++) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&st, sizeof st, &needed)) return false;
        if (st.dwCurrentState == want) return true;
        Sleep(100);
    }
    return false;
}

static bool service_start(SC_HANDLE scm, const wchar_t *name, char *msg, size_t msg_len) {
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { snprintf(msg, msg_len, "open failed (run as administrator?): error %lu", GetLastError()); return false; }
    bool ok = StartServiceW(svc, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    if (ok) ok = wait_for_state(svc, SERVICE_RUNNING, 30);
    snprintf(msg, msg_len, ok ? "service running" : "start failed: error %lu", GetLastError());
    CloseServiceHandle(svc);
    return ok;
}

static bool service_stop(SC_HANDLE scm, const wchar_t *name, char *msg, size_t msg_len) {
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { snprintf(msg, msg_len, "open failed (run as administrator?): error %lu", GetLastError()); return false; }
    SERVICE_STATUS st;
    bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    if (ok) ok = wait_for_state(svc, SERVICE_STOPPED, 30);
    snprintf(msg, msg_len, ok ? "service stopped" : "stop failed: error %lu", GetLastError());
    CloseServiceHandle(svc);
    return ok;
}

bool ph_service_control(const char *name, const char *action, char *msg, size_t msg_len) {
    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { snprintf(msg, msg_len, "cannot connect to service manager: error %lu", GetLastError()); return false; }
    bool ok = false;
    if (strcmp(action, "start") == 0) ok = service_start(scm, wname, msg, msg_len);
    else if (strcmp(action, "stop") == 0) ok = service_stop(scm, wname, msg, msg_len);
    else if (strcmp(action, "restart") == 0) {
        ok = service_stop(scm, wname, msg, msg_len) && service_start(scm, wname, msg, msg_len);
    } else {
        snprintf(msg, msg_len, "unknown service action: %s", action);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool ph_service_set_startup(const char *name, const char *startup, char *msg, size_t msg_len) {
    DWORD type;
    if (strcmp(startup, "auto") == 0) type = SERVICE_AUTO_START;
    else if (strcmp(startup, "manual") == 0) type = SERVICE_DEMAND_START;
    else if (strcmp(startup, "disabled") == 0) type = SERVICE_DISABLED;
    else { snprintf(msg, msg_len, "unknown startup type: %s", startup); return false; }

    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { snprintf(msg, msg_len, "cannot connect to service manager: error %lu", GetLastError()); return false; }
    SC_HANDLE svc = OpenServiceW(scm, wname, SERVICE_CHANGE_CONFIG);
    if (!svc) {
        snprintf(msg, msg_len, "open failed (run as administrator?): error %lu", GetLastError());
        CloseServiceHandle(scm);
        return false;
    }
    bool ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    snprintf(msg, msg_len, ok ? "startup type changed" : "change failed: error %lu", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Live metrics                                                        */
/* ------------------------------------------------------------------ */

static ULONGLONG filetime_u64(FILETIME ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int ph_cpu_usage_percent(void) {
    static ULONGLONG prev_idle, prev_kernel, prev_user;
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
    ULONGLONG i = filetime_u64(idle), k = filetime_u64(kernel), u = filetime_u64(user);
    ULONGLONG di = i - prev_idle, dk = k - prev_kernel, du = u - prev_user;
    bool first = prev_kernel == 0 && prev_user == 0;
    prev_idle = i;
    prev_kernel = k;
    prev_user = u;
    ULONGLONG total = dk + du; /* kernel time includes idle */
    if (first || total == 0) return 0;
    ULONGLONG busy = total > di ? total - di : 0;
    return (int)(busy * 100 / total);
}

int ph_process_count(void) {
    DWORD pids[4096], bytes = 0;
    if (!EnumProcesses(pids, sizeof pids, &bytes)) return 0;
    return (int)(bytes / sizeof(DWORD));
}

#endif /* _WIN32 */
