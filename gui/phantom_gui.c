/* Phantom — native Win32 GUI over the phantom-c core.
 *
 * Modern dark design language derived from (not copied from) the original
 * WPF app: dark green-tinted shell, card dashboard, pill-style grouped
 * navigation with Segoe MDL2 icons and an accent indicator, banded list
 * views with accent selection, a timestamped embedded console with state
 * badge and file logging, and a dark title bar.
 *
 * The Home uptime is a stopwatch driven by the monotonic GetTickCount64
 * clock — never polled. Live tiles update on the same one-second tick.
 *
 * All catalog operations run on a single worker thread through the same
 * operation engine and PowerShell safety validation as the CLI, with the
 * same dangerous-operation confirmation and restore-point gates as the
 * original app. Installed apps and services use native registry/SCM APIs.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phantom/catalog.h"
#include "phantom/config.h"
#include "phantom/operation.h"
#include "phantom/process_runner.h"
#include "phantom/ps_runner.h"
#include "phantom/status_parser.h"

#include "gui_d2d.h"
#include "gui_native.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#endif

#define PHANTOM_VERSION   L"1.4.1"
#define PHANTOM_REPO_URL  L"https://github.com/xobash/phantom-c"

#include "gui_theme.h"

ph_theme g_theme; /* active runtime theme */

static void apply_theme(int theme_id);
static void save_settings(void);
static void d2d_render_constellation(void);

#define APP_TITLE    L"Phantom"
#define WND_CLASS    L"PhantomMainWindow"

#define SIDEBAR_W    212
#define CONSOLE_H    178
#define MIN_W        1040
#define MIN_H        700

#define WM_APP_LOG     (WM_APP + 1)
#define WM_APP_DONE    (WM_APP + 2)
#define WM_APP_STATUS  (WM_APP + 3)

#define IDT_TICK     1
#define IDT_ANIM     2   /* ~30 fps: particles, nav transitions, hover */

/* GWLP_USERDATA bits on owner-drawn buttons */
#define BTNF_PRIMARY 0x1
#define BTNF_HOVER   0x2

enum section {
    SEC_HOME, SEC_STORE, SEC_APPS, SEC_TWEAKS, SEC_FEATURES, SEC_SERVICES,
    SEC_PANELS, SEC_FIXES, SEC_UPDATES, SEC_SETTINGS, SEC_COUNT
};

enum {
    ID_NAV_FIRST = 100,
    ID_BTN_TWEAK_APPLY = 200, ID_BTN_TWEAK_UNDO, ID_BTN_TWEAK_DETECT, ID_BTN_TWEAK_DETECT_ALL,
    ID_BTN_FEATURE_ENABLE, ID_BTN_FEATURE_DISABLE, ID_BTN_FEATURE_DETECT,
    ID_BTN_FIX_RUN, ID_BTN_FIX_UNDO,
    ID_BTN_PANEL_OPEN,
    ID_BTN_STORE_INSTALL, ID_BTN_STORE_UNINSTALL, ID_BTN_STORE_UPGRADE, ID_BTN_STORE_STATUS,
    ID_BTN_APP_UNINSTALL, ID_BTN_APP_LOCATION, ID_BTN_APP_REFRESH,
    ID_BTN_SVC_START, ID_BTN_SVC_STOP, ID_BTN_SVC_RESTART, ID_BTN_SVC_SET_STARTUP, ID_BTN_SVC_REFRESH,
    ID_COMBO_SVC_STARTUP,
    ID_BTN_UPDATE_APPLY, ID_BTN_UPDATE_DETECT,
    ID_RADIO_UPDATE_DEFAULT, ID_RADIO_UPDATE_SECURITY, ID_RADIO_UPDATE_DISABLE,
    ID_EDIT_AUTO_PATH, ID_BTN_AUTO_BROWSE, ID_BTN_AUTO_DRYRUN, ID_BTN_AUTO_RUN,
    ID_CHK_AUTO_FORCE, ID_EDIT_AUTO_ACK,
    ID_CHK_SET_DRYRUN, ID_CHK_SET_RESTORE, ID_CHK_SET_SKIPCAPTURE,
    ID_BTN_ABOUT_GITHUB,
    ID_BTN_LOG_CLEAR, ID_BTN_LOG_COPY, ID_BTN_LOG_FOLDER,
    ID_BADGE,
    ID_COMBO_THEME, ID_COMBO_ACCENT, ID_EDIT_ACCENT_HEX, ID_BTN_ACCENT_APPLY,
    ID_PARTICLES,
    ID_SEARCH_FIRST = 300,
    ID_CARD_FIRST = 400,
};

#define MAX_BUTTONS 6
#define HOME_CARDS  4
#define HOME_INFO   6

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    ph_catalog_entry *items;
    int count, cap;
} entry_list;

typedef enum {
    JOB_OPERATION,
    JOB_DETECT,
    JOB_DETECT_ALL,
    JOB_AUTOMATION,
    JOB_APP_UNINSTALL,
    JOB_SERVICE,
} job_kind;

typedef struct {
    job_kind kind;
    ph_operation op;
    int section;
    int item;
    bool undo;
    bool dry_run;
    bool create_restore_point;
    bool skip_capture_check;
    bool force_dangerous;
    char config_path[512];
    char acknowledgement[128];
    char command[1024];
    char target[256];
} job;

typedef struct {
    const wchar_t *title;
    wchar_t value[96];
} home_card;

static struct {
    HINSTANCE inst;
    HWND wnd;
    HWND nav[SEC_COUNT];
    HWND nav_groups[8];
    int nav_group_count;
    int active;

    HFONT font, font_semibold, font_title, font_big, font_group, font_icon, font_mono;
    HBRUSH br_bg, br_shell, br_input;

    HWND section_title[SEC_COUNT], section_desc[SEC_COUNT];
    HWND list[SEC_COUNT];
    HWND search[SEC_COUNT];
    HWND buttons[SEC_COUNT][MAX_BUTTONS];
    int button_count[SEC_COUNT];

    home_card cards[HOME_CARDS];
    HWND card_wnd[HOME_CARDS];
    HWND home_info[HOME_INFO];

    int *store_filter, store_filter_count;
    int *iapp_filter, iapp_filter_count;
    int *svc_filter, svc_filter_count;

    HWND upd_radio[3];
    HWND svc_combo;

    /* Settings page: appearance, safety, automation, about */
    HWND set_head[4];
    HWND set_chk[3];
    HWND auto_path, auto_browse, auto_force, auto_ack_label, auto_ack;
    HWND about_line[3];
    HWND theme_combo, accent_combo, accent_hex;
    HWND particle_canvas;
    int theme_id;                /* PH_THEME_* */
    int accent_mode;             /* PH_ACCENT_* */
    COLORREF accent_custom;
    bool opt_dry, opt_restore, opt_skip;

    /* Console */
    HWND console_title, console_badge, log_edit;
    HWND status_label;
    wchar_t log_file[MAX_PATH];

    entry_list tweaks, features, fixes, panels, apps;
    ph_installed_app *iapps;
    int iapp_count;
    ph_service_row *services;
    int svc_count;
    char data_dir[512];

    HANDLE worker;
    volatile LONG busy;
    job current_job;
    job_kind last_job;

    ULONGLONG nav_anim_start;    /* selection transition start tick */
} g;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static wchar_t *utf8_to_wide_dup(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void wide_to_utf8(const wchar_t *w, char *out, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, out, (int)n, NULL, NULL) && n) out[0] = '\0';
}

static void post_log(const char *line) {
    wchar_t *w = utf8_to_wide_dup(line);
    if (w) PostMessageW(g.wnd, WM_APP_LOG, 0, (LPARAM)w);
}

static void post_logf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    post_log(buf);
}

static void post_item_status(int section, int item, const char *status) {
    if (item < 0) return;
    wchar_t *w = utf8_to_wide_dup(status);
    if (w) PostMessageW(g.wnd, WM_APP_STATUS, MAKEWPARAM(section, item), (LPARAM)w);
}

static void set_status_text(const wchar_t *text) {
    SetWindowTextW(g.status_label, text);
}

static bool entry_list_add(entry_list *l, const ph_catalog_entry *e) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        void *p = realloc(l->items, (size_t)cap * sizeof *l->items);
        if (!p) return false;
        l->items = (ph_catalog_entry *)p;
        l->cap = cap;
    }
    l->items[l->count++] = *e;
    return true;
}

static bool collect_entry(const ph_catalog_entry *e, void *ctx) {
    return entry_list_add((entry_list *)ctx, e);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static bool gui_runner(const ph_operation *op, const ph_step *step, bool dry_run, char *output, size_t out_len, void *ctx) {
    (void)ctx;
    ph_error err = {0};
    post_logf("%s %s :: %s", dry_run ? "[dry-run]" : "[run]", op->id, step->name);
    bool ok = ph_ps_runner_execute(op, step, dry_run, output, out_len, &err);
    if (output[0]) post_log(output);
    if (!ok && err.message[0]) post_logf("error: %s", err.message);
    return ok;
}

static void run_single_operation(job *j) {
    ph_operation_result results[8];
    int rc = 0;
    ph_error err = {0};
    ph_operation op = j->op;
    if (j->undo) {
        ph_step tmp[8];
        int tmpc = op.run_count;
        memcpy(tmp, op.run, sizeof tmp);
        memcpy(op.run, op.undo, sizeof op.run);
        op.run_count = op.undo_count;
        memcpy(op.undo, tmp, sizeof op.undo);
        op.undo_count = tmpc;
        op.capture_count = 0;
        memset(&op.detect, 0, sizeof op.detect);
    }
    ph_operation_request req = {0};
    req.dry_run = j->dry_run;
    req.enable_destructive = true;
    req.force_dangerous = j->force_dangerous;
    req.confirm_dangerous = j->force_dangerous || (!op.destructive && op.risk != PH_RISK_DANGEROUS);
    req.skip_capture_check = j->skip_capture_check;
    req.create_restore_point = j->create_restore_point;
    bool ok = ph_operation_engine_run(&op, 1, req, gui_runner, NULL, results, &rc, &err);
    for (int i = 0; i < rc; i++) {
        post_logf("%s: %s", results[i].operation_id, results[i].success ? "success" : results[i].message);
        if (results[i].verification_attempted)
            post_logf("verification: %s", results[i].verification_passed ? "applied" : "not applied");
    }
    if (j->item >= 0 && !j->undo) {
        if (rc > 0 && results[0].verification_attempted)
            post_item_status(j->section, j->item, results[0].verification_passed ? "Applied" : "Not applied");
        else
            post_item_status(j->section, j->item, ok ? (j->dry_run ? "Dry-run ok" : "Done") : "Failed");
    } else if (j->item >= 0 && j->undo) {
        post_item_status(j->section, j->item, ok ? "Reverted" : "Undo failed");
    }
}

static void run_detect(job *j) {
    if (!j->op.detect.script[0]) {
        post_logf("%s has no detect script", j->op.id);
        post_item_status(j->section, j->item, "No detect");
        return;
    }
    char output[PH_PROCESS_OUTPUT_MAX];
    ph_error err = {0};
    bool ok = ph_ps_runner_execute(&j->op, &j->op.detect, false, output, sizeof output, &err);
    ph_operation_status st = ok ? ph_parse_operation_status(output) : PH_STATUS_UNKNOWN;
    const char *label = st == PH_STATUS_APPLIED ? "Applied" : st == PH_STATUS_NOT_APPLIED ? "Not applied" : "Unknown";
    post_logf("%s status: %s", j->op.id, label);
    post_item_status(j->section, j->item, label);
}

static void run_detect_all(job *j) {
    (void)j;
    for (int i = 0; i < g.tweaks.count; i++) {
        job d = {0};
        d.kind = JOB_DETECT;
        d.section = SEC_TWEAKS;
        d.item = i;
        ph_error err = {0};
        if (ph_make_tweak_operation(g.tweaks.items[i].id, &d.op, &err)) run_detect(&d);
        else post_logf("%s", err.message);
    }
}

static void run_automation(job *j) {
    ph_error err = {0};
    ph_automation_config cfg;
    if (!ph_config_load_file(j->config_path, &cfg, &err)) { post_logf("config error: %s", err.message); return; }
    if (!ph_config_dangerous_gate(&cfg, j->force_dangerous, j->acknowledgement, &err)) {
        post_logf("blocked: %s", err.message);
        return;
    }
    ph_operation *ops = (ph_operation *)calloc(256, sizeof *ops);
    ph_operation_result *results = (ph_operation_result *)calloc(512, sizeof *results);
    if (!ops || !results) { free(ops); free(results); post_log("allocation failed"); return; }
    ph_store_manager_availability availability;
    const ph_store_manager_availability *availability_ptr = NULL;
    if (!j->dry_run && ph_store_manager_availability_get(NULL, &availability)) availability_ptr = &availability;
    int count = 0;
    if (!ph_config_build_operations_with_availability(&cfg, availability_ptr, ops, 256, &count, &err)) {
        post_logf("build error: %s", err.message);
        free(ops); free(results);
        return;
    }
    ph_operation_request req = {0};
    req.dry_run = j->dry_run;
    req.enable_destructive = true;
    req.force_dangerous = j->force_dangerous;
    req.skip_capture_check = j->skip_capture_check;
    req.create_restore_point = j->create_restore_point;
    req.confirm_dangerous = !ph_config_has_dangerous_selection(&cfg) || j->force_dangerous;
    int rc = 0;
    bool ok = ph_operation_engine_run(ops, count, req, gui_runner, NULL, results, &rc, &err);
    for (int i = 0; i < rc; i++) post_logf("%s: %s", results[i].operation_id, results[i].success ? "success" : results[i].message);
    post_logf("automation %s (%d operation%s)", ok ? "completed" : "failed", count, count == 1 ? "" : "s");
    free(ops);
    free(results);
}

static void run_app_uninstall(job *j) {
    post_logf("[run] uninstall %s :: %s", j->target, j->command);
    ph_process_result pr;
    ph_error err = {0};
    bool ok = ph_process_run(j->command, 1800, &pr, &err);
    if (pr.output[0]) post_log(pr.output);
    post_logf("uninstall %s: %s", j->target, ok ? "completed" : err.message);
}

static void run_service_job(job *j) {
    char msg[256];
    if (strncmp(j->target, "startup:", 8) == 0)
        ph_service_set_startup(j->command, j->target + 8, msg, sizeof msg);
    else
        ph_service_control(j->command, j->target, msg, sizeof msg);
    post_logf("service %s (%s): %s", j->command, j->target, msg);
}

static DWORD WINAPI worker_main(LPVOID param) {
    (void)param;
    job *j = &g.current_job;
    switch (j->kind) {
        case JOB_OPERATION:     run_single_operation(j); break;
        case JOB_DETECT:        run_detect(j); break;
        case JOB_DETECT_ALL:    run_detect_all(j); break;
        case JOB_AUTOMATION:    run_automation(j); break;
        case JOB_APP_UNINSTALL: run_app_uninstall(j); break;
        case JOB_SERVICE:       run_service_job(j); break;
    }
    PostMessageW(g.wnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void enable_actions(BOOL enable);

static bool start_job(const job *j) {
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) {
        set_status_text(L"Busy — wait for the current operation to finish.");
        return false;
    }
    g.current_job = *j;
    g.last_job = j->kind;
    enable_actions(FALSE);
    set_status_text(L"Working…");
    InvalidateRect(g.console_badge, NULL, TRUE);
    g.worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!g.worker) {
        InterlockedExchange(&g.busy, 0);
        enable_actions(TRUE);
        set_status_text(L"Failed to start worker thread.");
        InvalidateRect(g.console_badge, NULL, TRUE);
        return false;
    }
    return true;
}

static bool confirm_if_dangerous(const ph_operation *op, bool *force_out) {
    *force_out = false;
    if (op->risk != PH_RISK_DANGEROUS && !op->destructive) return true;
    wchar_t *title = utf8_to_wide_dup(op->title);
    wchar_t msg[512];
    _snwprintf(msg, 512, L"\"%s\" is a %s operation%s.\n\nA system restore point can be created first (see Settings). Continue?",
               title ? title : L"operation",
               op->risk == PH_RISK_DANGEROUS ? L"dangerous" : L"destructive",
               op->reversible ? L"" : L" and it cannot be automatically undone");
    free(title);
    int r = MessageBoxW(g.wnd, msg, L"Confirm dangerous operation", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (r != IDYES) return false;
    *force_out = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Appearance: settings persistence + runtime theme application        */
/* ------------------------------------------------------------------ */

#include "phantom/jsonlite.h"

static void settings_path(wchar_t out[MAX_PATH]) {
    out[0] = L'\0';
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%s\\Phantom", base);
        CreateDirectoryW(dir, NULL);
        _snwprintf(out, MAX_PATH, L"%s\\settings.json", dir);
    }
}

static COLORREF parse_hex_color(const char *hex, COLORREF fallback) {
    if (!hex) return fallback;
    if (*hex == '#') hex++;
    if (strlen(hex) != 6) return fallback;
    unsigned v = 0;
    for (int i = 0; i < 6; i++) {
        char c = hex[i];
        unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0')
                   : (c >= 'a' && c <= 'f') ? (unsigned)(c - 'a' + 10)
                   : (c >= 'A' && c <= 'F') ? (unsigned)(c - 'A' + 10) : 16;
        if (d > 15) return fallback;
        v = v * 16 + d;
    }
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void load_settings(void) {
    g.theme_id = PH_THEME_VERDANT;
    g.accent_mode = PH_ACCENT_THEME;
    g.accent_custom = RGB(0x80, 0x52, 0xFF);
    g.opt_dry = false;
    g.opt_restore = true;
    g.opt_skip = false;
    wchar_t wpath[MAX_PATH];
    settings_path(wpath);
    if (!wpath[0]) return;
    char path[MAX_PATH * 3];
    wide_to_utf8(wpath, path, sizeof path);
    char *txt = ph_read_all_file(path);
    if (!txt) return;
    const char *end = txt + strlen(txt);
    char v[64];
    if (ph_json_string_field(txt, end, "theme", v, sizeof v) && _stricmp(v, "void") == 0) g.theme_id = PH_THEME_VOID;
    if (ph_json_string_field(txt, end, "accentMode", v, sizeof v)) {
        if (_stricmp(v, "windows") == 0) g.accent_mode = PH_ACCENT_WINDOWS;
        else if (_stricmp(v, "custom") == 0) g.accent_mode = PH_ACCENT_CUSTOM;
    }
    if (ph_json_string_field(txt, end, "accentColor", v, sizeof v)) g.accent_custom = parse_hex_color(v, g.accent_custom);
    if (ph_json_string_field(txt, end, "dryRun", v, sizeof v)) g.opt_dry = _stricmp(v, "on") == 0;
    if (ph_json_string_field(txt, end, "restorePoint", v, sizeof v)) g.opt_restore = _stricmp(v, "on") == 0;
    if (ph_json_string_field(txt, end, "skipCapture", v, sizeof v)) g.opt_skip = _stricmp(v, "on") == 0;
    free(txt);
}

static void save_settings(void) {
    wchar_t wpath[MAX_PATH];
    settings_path(wpath);
    if (!wpath[0]) return;
    FILE *f = _wfopen(wpath, L"wb");
    if (!f) return;
    bool dry = g.set_chk[0] && SendMessageW(g.set_chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool restore = g.set_chk[1] && SendMessageW(g.set_chk[1], BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool skip = g.set_chk[2] && SendMessageW(g.set_chk[2], BM_GETCHECK, 0, 0) == BST_CHECKED;
    fprintf(f,
        "{\n"
        "  \"theme\": \"%s\",\n"
        "  \"accentMode\": \"%s\",\n"
        "  \"accentColor\": \"#%02X%02X%02X\",\n"
        "  \"dryRun\": \"%s\",\n"
        "  \"restorePoint\": \"%s\",\n"
        "  \"skipCapture\": \"%s\"\n"
        "}\n",
        g.theme_id == PH_THEME_VOID ? "void" : "verdant",
        g.accent_mode == PH_ACCENT_WINDOWS ? "windows" : g.accent_mode == PH_ACCENT_CUSTOM ? "custom" : "theme",
        GetRValue(g.accent_custom), GetGValue(g.accent_custom), GetBValue(g.accent_custom),
        dry ? "on" : "off", restore ? "on" : "off", skip ? "on" : "off");
    fclose(f);
}

static COLORREF blend(COLORREF a, COLORREF b, int pct_a) {
    return RGB((GetRValue(a) * pct_a + GetRValue(b) * (100 - pct_a)) / 100,
               (GetGValue(a) * pct_a + GetGValue(b) * (100 - pct_a)) / 100,
               (GetBValue(a) * pct_a + GetBValue(b) * (100 - pct_a)) / 100);
}

static void apply_accent_override(void) {
    if (g.accent_mode == PH_ACCENT_THEME) return; /* keep curated theme colors */
    COLORREF a = g_theme.accent;
    if (g.accent_mode == PH_ACCENT_WINDOWS) {
        DWORD argb = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
            a = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    } else {
        a = g.accent_custom;
    }
    g_theme.accent = a;
    g_theme.accent_br = blend(a, RGB(255, 255, 255), 55);
    g_theme.btn_pri_pressed = blend(a, RGB(0, 0, 0), 75);
    g_theme.btn_pri_disabled = blend(a, g_theme.bg, 40);
    g_theme.row_sel = blend(a, g_theme.bg, 32);
    g_theme.nav_sel = blend(a, g_theme.bg, 15);
}

static void rebuild_theme_resources(void);
static void restyle_lists(void);

static void apply_theme(int theme_id) {
    g.theme_id = theme_id == PH_THEME_VOID ? PH_THEME_VOID : PH_THEME_VERDANT;
    g_theme = PH_THEMES[g.theme_id];
    apply_accent_override();
    rebuild_theme_resources();
    restyle_lists();
    if (g.about_line[0]) {
        wchar_t a0[192];
        _snwprintf(a0, 192, L"Phantom %s (C edition, %s theme) — native, zero runtime dependencies",
                   PHANTOM_VERSION, g_theme.name);
        SetWindowTextW(g.about_line[0], a0);
    }
    if (g.wnd)
        RedrawWindow(g.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (ph_d2d_active()) d2d_render_constellation();
}

/* ------------------------------------------------------------------ */
/* Dark-mode plumbing                                                  */
/* ------------------------------------------------------------------ */

typedef enum { AppModeDefault = 0, AppModeAllowDark = 1, AppModeForceDark = 2 } PreferredAppMode;
typedef PreferredAppMode(WINAPI *SetPreferredAppModeFn)(PreferredAppMode);
typedef BOOL(WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);

static AllowDarkModeForWindowFn g_allow_dark_for_window;

static void enable_dark_app_mode(void) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    SetPreferredAppModeFn set_mode = (SetPreferredAppModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_allow_dark_for_window = (AllowDarkModeForWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    if (set_mode) set_mode(AppModeForceDark);
}

static void dark_titlebar(HWND wnd) {
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(wnd, 20, &on, sizeof on)))
        DwmSetWindowAttribute(wnd, 19, &on, sizeof on);
}

static void apply_dark_to_list(HWND lv) {
    ListView_SetBkColor(lv, CLR_ROW);
    ListView_SetTextBkColor(lv, CLR_ROW);
    ListView_SetTextColor(lv, CLR_TEXT);
    SetWindowTheme(lv, L"DarkMode_Explorer", NULL);
    HWND header = ListView_GetHeader(lv);
    if (header) {
        SetWindowTheme(header, L"DarkMode_ItemsView", NULL);
        if (g_allow_dark_for_window) g_allow_dark_for_window(header, TRUE);
    }
    if (g_allow_dark_for_window) g_allow_dark_for_window(lv, TRUE);
    /* Taller, airier rows via a 1px-wide state image list. */
    HIMAGELIST il = ImageList_Create(1, 26, ILC_COLOR32, 1, 1);
    if (il) ListView_SetImageList(lv, il, LVSIL_SMALL);
}

/* Hover tracking for owner-drawn buttons: nav links shift smoke -> bone on
 * hover (no underline, no background, per the design spec); action pills
 * brighten their border. */
static LRESULT CALLBACK hover_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (msg == WM_MOUSEMOVE) {
        LONG_PTR d = GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!(d & BTNF_HOVER)) {
            SetWindowLongPtrW(h, GWLP_USERDATA, d | BTNF_HOVER);
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        LONG_PTR d = GetWindowLongPtrW(h, GWLP_USERDATA);
        if (d & BTNF_HOVER) {
            SetWindowLongPtrW(h, GWLP_USERDATA, d & ~(LONG_PTR)BTNF_HOVER);
            InvalidateRect(h, NULL, FALSE);
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void enable_hover(HWND h) {
    SetWindowSubclass(h, hover_subclass, 1, 0);
}

static void restyle_lists(void) {
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (!g.list[sec]) continue;
        ListView_SetBkColor(g.list[sec], CLR_ROW);
        ListView_SetTextBkColor(g.list[sec], CLR_ROW);
        ListView_SetTextColor(g.list[sec], CLR_TEXT);
    }
}

/* ------------------------------------------------------------------ */
/* Control factories                                                   */
/* ------------------------------------------------------------------ */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10,
                             g.wnd, (HMENU)(INT_PTR)id, g.inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    return h;
}

static HWND mk_check(const wchar_t *label, int id) {
    HWND h = mk(L"BUTTON", label, BS_AUTOCHECKBOX, id);
    SetWindowTheme(h, L"", L"");
    return h;
}

static HWND mk_radio(const wchar_t *label, int id, DWORD extra) {
    HWND h = mk(L"BUTTON", label, BS_AUTORADIOBUTTON | extra, id);
    SetWindowTheme(h, L"", L"");
    return h;
}

static HWND mk_edit(int id, const wchar_t *cue) {
    HWND h = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, id);
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    if (g_allow_dark_for_window) g_allow_dark_for_window(h, TRUE);
    return h;
}

static void lv_add_column(HWND lv, int idx, const wchar_t *name, int width) {
    LVCOLUMNW c = {0};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)name;
    c.cx = width;
    ListView_InsertColumn(lv, idx, &c);
}

static void lv_set(HWND lv, int row, int col, const char *utf8) {
    wchar_t *w = utf8_to_wide_dup(utf8);
    if (!w) return;
    if (col == 0) {
        LVITEMW it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = w;
        ListView_InsertItem(lv, &it);
    } else {
        ListView_SetItemText(lv, row, col, w);
    }
    free(w);
}

static int lv_selected(HWND lv) {
    return (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static HWND mk_list(int section) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)(900 + section), g.inst, NULL);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g.font, TRUE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
    apply_dark_to_list(lv);
    return lv;
}

static void mk_search(int sec, const wchar_t *cue) {
    g.search[sec] = mk_edit(ID_SEARCH_FIRST + sec, cue);
}

static const char *risk_label(const ph_catalog_entry *e) {
    return e->risk[0] ? e->risk : "Advanced";
}

/* ------------------------------------------------------------------ */
/* List fills                                                          */
/* ------------------------------------------------------------------ */

static void show_count(int shown, int total, const wchar_t *noun) {
    wchar_t label[96];
    if (shown == total) _snwprintf(label, 96, L"%d %s", total, noun);
    else _snwprintf(label, 96, L"%d of %d %s", shown, total, noun);
    set_status_text(label);
}

static void fill_tweaks(void) {
    HWND lv = g.list[SEC_TWEAKS];
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < g.tweaks.count; i++) {
        const ph_catalog_entry *e = &g.tweaks.items[i];
        lv_set(lv, i, 0, e->name[0] ? e->name : e->id);
        lv_set(lv, i, 1, risk_label(e));
        lv_set(lv, i, 2, e->reversible ? "Yes" : "No");
        lv_set(lv, i, 3, "—");
        lv_set(lv, i, 4, e->description);
    }
}

static void fill_features(void) {
    HWND lv = g.list[SEC_FEATURES];
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < g.features.count; i++) {
        const ph_catalog_entry *e = &g.features.items[i];
        lv_set(lv, i, 0, e->name[0] ? e->name : e->id);
        lv_set(lv, i, 1, "—");
        lv_set(lv, i, 2, e->description);
    }
}

static void fill_fixes(void) {
    HWND lv = g.list[SEC_FIXES];
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < g.fixes.count; i++) {
        const ph_catalog_entry *e = &g.fixes.items[i];
        lv_set(lv, i, 0, e->name[0] ? e->name : e->id);
        lv_set(lv, i, 1, risk_label(e));
        lv_set(lv, i, 2, e->reversible ? "Yes" : "No");
        lv_set(lv, i, 3, e->description);
    }
}

static void fill_panels(void) {
    HWND lv = g.list[SEC_PANELS];
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < g.panels.count; i++) {
        const ph_catalog_entry *e = &g.panels.items[i];
        lv_set(lv, i, 0, e->name[0] ? e->name : e->id);
        lv_set(lv, i, 1, e->description);
    }
}

static void get_filter_text(int sec, char *out, size_t n) {
    wchar_t w[128] = L"";
    if (g.search[sec]) GetWindowTextW(g.search[sec], w, 128);
    wide_to_utf8(w, out, n);
}

static void fill_store(void) {
    char filter[256];
    get_filter_text(SEC_STORE, filter, sizeof filter);
    HWND lv = g.list[SEC_STORE];
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    g.store_filter_count = 0;
    int row = 0;
    for (int i = 0; i < g.apps.count; i++) {
        const ph_catalog_entry *e = &g.apps.items[i];
        if (filter[0] &&
            !ph_contains_i(e->name, filter) &&
            !ph_contains_i(e->extra, filter) &&
            !ph_contains_i(e->sources, filter) &&
            !ph_contains_i(e->description, filter)) continue;
        g.store_filter[g.store_filter_count++] = i;
        lv_set(lv, row, 0, e->name);
        lv_set(lv, row, 1, e->extra[0] ? e->extra : "—");
        lv_set(lv, row, 2, e->sources[0] ? e->sources : "—");
        lv_set(lv, row, 3, e->description);
        row++;
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    if (g.active == SEC_STORE) show_count(row, g.apps.count, L"apps");
}

static void fill_installed_apps(void) {
    char filter[256];
    get_filter_text(SEC_APPS, filter, sizeof filter);
    HWND lv = g.list[SEC_APPS];
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    g.iapp_filter_count = 0;
    int row = 0;
    for (int i = 0; i < g.iapp_count; i++) {
        const ph_installed_app *a = &g.iapps[i];
        if (filter[0] &&
            !ph_contains_i(a->name, filter) &&
            !ph_contains_i(a->publisher, filter)) continue;
        g.iapp_filter[g.iapp_filter_count++] = i;
        lv_set(lv, row, 0, a->name);
        lv_set(lv, row, 1, a->version[0] ? a->version : "—");
        lv_set(lv, row, 2, a->publisher[0] ? a->publisher : "—");
        row++;
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    if (g.active == SEC_APPS) show_count(row, g.iapp_count, L"installed apps");
}

static void fill_services(void) {
    char filter[256];
    get_filter_text(SEC_SERVICES, filter, sizeof filter);
    HWND lv = g.list[SEC_SERVICES];
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    g.svc_filter_count = 0;
    int row = 0;
    for (int i = 0; i < g.svc_count; i++) {
        const ph_service_row *s = &g.services[i];
        if (filter[0] &&
            !ph_contains_i(s->display, filter) &&
            !ph_contains_i(s->name, filter) &&
            !ph_contains_i(s->state, filter) &&
            !ph_contains_i(s->start_type, filter)) continue;
        g.svc_filter[g.svc_filter_count++] = i;
        lv_set(lv, row, 0, s->display);
        lv_set(lv, row, 1, s->state);
        lv_set(lv, row, 2, s->start_type);
        lv_set(lv, row, 3, s->name);
        row++;
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    if (g.active == SEC_SERVICES) show_count(row, g.svc_count, L"services");
}

static void reload_installed_apps(void) {
    free(g.iapps);
    g.iapps = ph_installed_apps_list(&g.iapp_count);
    free(g.iapp_filter);
    g.iapp_filter = (int *)calloc((size_t)(g.iapp_count ? g.iapp_count : 1), sizeof *g.iapp_filter);
    fill_installed_apps();
}

static void reload_services(void) {
    free(g.services);
    g.services = ph_services_list(&g.svc_count);
    free(g.svc_filter);
    g.svc_filter = (int *)calloc((size_t)(g.svc_count ? g.svc_count : 1), sizeof *g.svc_filter);
    fill_services();
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const wchar_t *group;
    const wchar_t *label;
    const wchar_t *icon;
} nav_def;

/* Glyphs follow the original MainViewModel icon set (Segoe MDL2 Assets). */
static const nav_def NAV[SEC_COUNT] = {
    [SEC_HOME]     = {L"OVERVIEW",    L"Home",             L"\xE80F"},
    [SEC_STORE]    = {L"SOFTWARE",    L"App store",        L"\xE719"},
    [SEC_APPS]     = {NULL,           L"Installed apps",   L"\xE8F1"},
    [SEC_TWEAKS]   = {L"SYSTEM",      L"Tweaks",           L"\xE713"},
    [SEC_FEATURES] = {NULL,           L"Windows features", L"\xE115"},
    [SEC_SERVICES] = {NULL,           L"Services",         L"\xE895"},
    [SEC_PANELS]   = {NULL,           L"Legacy panels",    L"\xE8A7"},
    [SEC_FIXES]    = {L"MAINTENANCE", L"Quick fixes",      L"\xE90F"},
    [SEC_UPDATES]  = {NULL,           L"Windows Update",   L"\xE777"},
    [SEC_SETTINGS] = {L"",            L"Settings",         L"\xE115"},
};

static void create_nav(void) {
    for (int i = 0; i < SEC_COUNT; i++) {
        if (NAV[i].group) {
            HWND grp = mk(L"STATIC", NAV[i].group, WS_VISIBLE | SS_OWNERDRAW, 0);
            SendMessageW(grp, WM_SETFONT, (WPARAM)g.font_group, TRUE);
            g.nav_groups[g.nav_group_count++] = grp;
        }
        g.nav[i] = mk(L"BUTTON", NAV[i].label, WS_VISIBLE | BS_OWNERDRAW, ID_NAV_FIRST + i);
        enable_hover(g.nav[i]);
    }
}

/* Eased progress of the nav selection transition (0..1). */
static double nav_anim_progress(void) {
    if (!g.nav_anim_start) return 1.0;
    double t = (double)(GetTickCount64() - g.nav_anim_start) / 180.0;
    if (t >= 1.0) return 1.0;
    double inv = 1.0 - t;
    return 1.0 - inv * inv * inv; /* ease-out cubic */
}

static void draw_nav_button(const DRAWITEMSTRUCT *dis) {
    int sec = (int)dis->CtlID - ID_NAV_FIRST;
    bool selected = sec == g.active;
    bool hovered = (GetWindowLongPtrW(dis->hwndItem, GWLP_USERDATA) & BTNF_HOVER) != 0;
    RECT rc = dis->rcItem;
    HDC dc = dis->hDC;

    FillRect(dc, &rc, g.br_shell);

    if (selected) {
        double e = nav_anim_progress();
        /* pill fades in, accent bar grows from center — eased */
        HBRUSH br = CreateSolidBrush(blend(CLR_NAV_SEL, CLR_SHELL, (int)(e * 100.0)));
        HPEN pen = CreatePen(PS_SOLID, 1, blend(CLR_BORDER, CLR_SHELL, (int)(e * 100.0)));
        HBRUSH ob = (HBRUSH)SelectObject(dc, br);
        HPEN op = (HPEN)SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, RAD_NAV, RAD_NAV);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
        int full = (rc.bottom - rc.top) - 16;
        int bar_h = (int)(full * e);
        if (bar_h > 1) {
            int mid = (rc.top + rc.bottom) / 2;
            RECT bar = { rc.left + 4, mid - bar_h / 2, rc.left + 7, mid + bar_h / 2 };
            HBRUSH accent = CreateSolidBrush(CLR_ACCENT);
            FillRect(dc, &bar, accent);
            DeleteObject(accent);
        }
    }

    COLORREF glyph = selected ? CLR_ACCENT_BR : hovered ? CLR_TEXT : CLR_MUTED;
    COLORREF label = selected ? CLR_TEXT : hovered ? CLR_TEXT : CLR_MUTED;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, glyph);
    HFONT old = (HFONT)SelectObject(dc, g.font_icon);
    RECT icon_rc = { rc.left + 14, rc.top, rc.left + 40, rc.bottom };
    DrawTextW(dc, NAV[sec].icon, -1, &icon_rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    SelectObject(dc, selected ? g.font_semibold : g.font);
    SetTextColor(dc, label);
    RECT text_rc = { rc.left + 46, rc.top, rc.right - 8, rc.bottom };
    DrawTextW(dc, NAV[sec].label, -1, &text_rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

static void draw_nav_group(const DRAWITEMSTRUCT *dis, HWND wnd) {
    wchar_t text[64];
    GetWindowTextW(wnd, text, 64);
    FillRect(dis->hDC, &dis->rcItem, g.br_shell);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, CLR_FAINT);
    HFONT old = (HFONT)SelectObject(dis->hDC, g.font_group);
    RECT rc = dis->rcItem;
    rc.left += 18;
    DrawTextW(dis->hDC, text, -1, &rc, DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

/* ------------------------------------------------------------------ */
/* Buttons, cards, badge                                               */
/* ------------------------------------------------------------------ */

static void draw_action_button(const DRAWITEMSTRUCT *dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    LONG_PTR flags = GetWindowLongPtrW(dis->hwndItem, GWLP_USERDATA);
    bool primary = (flags & BTNF_PRIMARY) != 0;
    bool hovered = (flags & BTNF_HOVER) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF fill = primary ? (disabled ? CLR_BTN_PRI_DISABLED : pressed ? CLR_BTN_PRI_PRESSED
                               : hovered ? blend(CLR_ACCENT, RGB(255, 255, 255), 85) : CLR_ACCENT)
                            : (pressed ? CLR_NAV_SEL : hovered ? blend(CLR_CARD2, CLR_TEXT, 92) : CLR_CARD2);
    COLORREF border = primary ? (disabled ? CLR_BORDER : CLR_ACCENT_BR)
                              : (hovered && !disabled ? CLR_FAINT : CLR_BORDER);
    COLORREF text = disabled ? CLR_BTN_TXT_DISABLED : CLR_TEXT;

    FillRect(dc, &rc, g.br_bg);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH old_br = (HBRUSH)SelectObject(dc, br);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, RAD_BUTTON, RAD_BUTTON);
    SelectObject(dc, old_br);
    SelectObject(dc, old_pen);
    DeleteObject(br);
    DeleteObject(pen);

    wchar_t label[96];
    GetWindowTextW(dis->hwndItem, label, 96);
    if (g_theme.uppercase_buttons) CharUpperBuffW(label, (DWORD)lstrlenW(label));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HFONT old = (HFONT)SelectObject(dc, g.font_semibold);
    DrawTextW(dc, label, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

static HWND mk_action(int sec, const wchar_t *label, int id, bool primary) {
    HWND b = mk(L"BUTTON", label, BS_OWNERDRAW, id);
    SetWindowLongPtrW(b, GWLP_USERDATA, primary ? BTNF_PRIMARY : 0);
    enable_hover(b);
    g.buttons[sec][g.button_count[sec]++] = b;
    return b;
}

static void draw_home_card(const DRAWITEMSTRUCT *dis) {
    int idx = (int)dis->CtlID - ID_CARD_FIRST;
    if (idx < 0 || idx >= HOME_CARDS) return;
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;

    FillRect(dc, &rc, g.br_bg);
    HBRUSH br = CreateSolidBrush(CLR_CARD);
    HPEN pen = CreatePen(PS_SOLID, 1, CLR_BORDER);
    HBRUSH old_br = (HBRUSH)SelectObject(dc, br);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, RAD_CARD, RAD_CARD);
    SelectObject(dc, old_br);
    SelectObject(dc, old_pen);
    DeleteObject(br);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_FAINT);
    HFONT old = (HFONT)SelectObject(dc, g.font_group);
    RECT title_rc = { rc.left + 16, rc.top + 12, rc.right - 16, rc.top + 30 };
    DrawTextW(dc, g.cards[idx].title, -1, &title_rc, DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, idx == 0 ? CLR_ACCENT_BR : CLR_TEXT);
    SelectObject(dc, idx == 0 ? g.font_big : g.font_title);
    RECT value_rc = { rc.left + 16, rc.top + 30, rc.right - 16, rc.bottom - 10 };
    DrawTextW(dc, g.cards[idx].value, -1, &value_rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void draw_badge(const DRAWITEMSTRUCT *dis) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    bool working = g.busy != 0;
    FillRect(dc, &rc, g.br_bg);
    HBRUSH br = CreateSolidBrush(working ? CLR_ACCENT : CLR_CARD2);
    HPEN pen = CreatePen(PS_SOLID, 1, working ? CLR_ACCENT_BR : CLR_BORDER);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, RAD_BADGE, RAD_BADGE);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, working ? CLR_TEXT : CLR_MUTED);
    HFONT old = (HFONT)SelectObject(dc, g.font_group);
    DrawTextW(dc, working ? L"WORKING" : L"IDLE", -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void d2d_render_constellation(void) {
    ph_d2d_scene scene = {
        .bg = CLR_BG, .bone = CLR_TEXT, .accent = CLR_ACCENT,
        .accent_br = CLR_ACCENT_BR, .spark = g_theme.spark,
        .lichen = g_theme.lichen, .particles = g_theme.particles,
    };
    ph_d2d_render(&scene, (double)GetTickCount64() / 1000.0);
}

/* The Void constellation: a deterministic micro-shape field clustered
 * toward a focal point. Every particle orbits its anchor at its own
 * radius and angular speed and twinkles slowly — alive, never solid.
 * Double-buffered GDI at ~30 fps; no textures, no gradients, per spec. */
static void draw_particles(const DRAWITEMSTRUCT *dis) {
    HDC out = dis->hDC;
    RECT orc = dis->rcItem;
    int w = orc.right - orc.left, h = orc.bottom - orc.top;
    if (w < 40 || h < 40) { FillRect(out, &orc, g.br_bg); return; }

    HDC dc = CreateCompatibleDC(out);
    HBITMAP bmp = CreateCompatibleBitmap(out, w, h);
    HBITMAP obmp = (HBITMAP)SelectObject(dc, bmp);
    RECT rc = { 0, 0, w, h };
    FillRect(dc, &rc, g.br_bg);
    if (!g_theme.particles) {
        BitBlt(out, orc.left, orc.top, w, h, dc, 0, 0, SRCCOPY);
        SelectObject(dc, obmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        return;
    }

    double tsec = (double)GetTickCount64() / 1000.0;
    unsigned seed = 0x9E3779B9u;
    for (int i = 0; i < 420; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned r1 = (seed >> 8) & 0xFFFF;
        seed = seed * 1664525u + 1013904223u;
        unsigned r2 = (seed >> 8) & 0xFFFF;
        seed = seed * 1664525u + 1013904223u;
        unsigned r3 = (seed >> 8) & 0xFFFF;
        /* anchor: average of two uniforms biases toward the focal center */
        double fx = (((double)r1 / 65535.0) + ((double)r2 / 65535.0)) / 2.0;
        double fy = (((double)r2 / 65535.0) + ((double)r3 / 65535.0)) / 2.0;
        /* per-particle orbit: radius 3..15 px, speed 0.15..1.0 rad/s */
        double orbit_r = 3.0 + (double)(r3 % 1200) / 100.0;
        double speed = 0.15 + (double)(r1 % 85) / 100.0;
        if (i & 1) speed = -speed;
        double ang = speed * tsec + (double)(r2 % 628) / 100.0;
        int x = rc.left + (int)(fx * w + orbit_r * cos(ang));
        int y = rc.top + (int)(fy * h + orbit_r * 0.6 * sin(ang));
        if (x < rc.left + 2 || x > rc.right - 7 || y < rc.top + 2 || y > rc.bottom - 7) continue;
        int size = 2 + (int)(r3 % 4);
        /* slow twinkle: each particle swells briefly on its own beat */
        double tw = sin(tsec * 1.7 + (double)(i % 41));
        if (tw > 0.86) size += 2;
        else if (tw > 0.6) size += 1;
        COLORREF c;
        unsigned roll = r1 % 100;
        if (roll < 52) c = blend(g_theme.text, g_theme.bg, 35 + (int)(r2 % 40));
        else if (roll < 74) c = g_theme.accent;
        else if (roll < 86) c = g_theme.lichen;
        else if (roll < 94) c = g_theme.spark;
        else c = g_theme.accent_br;
        HBRUSH br = CreateSolidBrush(c);
        switch (r2 % 3) {
            case 0: { /* circle */
                HPEN pen = CreatePen(PS_SOLID, 1, c);
                HBRUSH ob = (HBRUSH)SelectObject(dc, br);
                HPEN op = (HPEN)SelectObject(dc, pen);
                Ellipse(dc, x, y, x + size, y + size);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(pen);
                break;
            }
            case 1: { /* square */
                RECT pr = { x, y, x + size, y + size };
                FillRect(dc, &pr, br);
                break;
            }
            default: { /* diamond */
                POINT pts[4] = { {x + size / 2, y}, {x + size, y + size / 2}, {x + size / 2, y + size}, {x, y + size / 2} };
                HPEN pen = CreatePen(PS_SOLID, 1, c);
                HBRUSH ob = (HBRUSH)SelectObject(dc, br);
                HPEN op = (HPEN)SelectObject(dc, pen);
                Polygon(dc, pts, 4);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(pen);
                break;
            }
        }
        DeleteObject(br);
    }
    BitBlt(out, orc.left, orc.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ------------------------------------------------------------------ */
/* Live tiles + system info                                            */
/* ------------------------------------------------------------------ */

static void update_live_tiles(void) {
    ULONGLONG s = GetTickCount64() / 1000;
    _snwprintf(g.cards[0].value, 96, L"%llud %02llu:%02llu:%02llu",
               s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);

    _snwprintf(g.cards[1].value, 96, L"%d%%", ph_cpu_usage_percent());

    MEMORYSTATUSEX mem;
    memset(&mem, 0, sizeof mem);
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        _snwprintf(g.cards[2].value, 96, L"%lu%%  ·  %.1f / %.1f GB", mem.dwMemoryLoad,
                   (double)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024 * 1024),
                   (double)mem.ullTotalPhys / (1024.0 * 1024 * 1024));
    }

    _snwprintf(g.cards[3].value, 96, L"%d", ph_process_count());

    if (g.active == SEC_HOME)
        for (int i = 0; i < HOME_CARDS; i++) InvalidateRect(g.card_wnd[i], NULL, FALSE);
}

typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static void fill_home_info(void) {
    wchar_t buf[256];

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &n)) {
        wchar_t user[256];
        DWORD un = 256;
        if (!GetUserNameW(user, &un)) user[0] = L'\0';
        _snwprintf(buf, 256, L"Computer       %s  ·  %s", name, user);
        SetWindowTextW(g.home_info[0], buf);
    }

    RTL_OSVERSIONINFOW ver;
    memset(&ver, 0, sizeof ver);
    ver.dwOSVersionInfoSize = sizeof ver;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtl = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rtl && rtl(&ver) == 0) {
        const wchar_t *era = (ver.dwMajorVersion == 10 && ver.dwBuildNumber >= 22000) ? L"Windows 11" : L"Windows 10";
        _snwprintf(buf, 256, L"Windows        %s  ·  build %lu", era, ver.dwBuildNumber);
        SetWindowTextW(g.home_info[1], buf);
    }

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t cpu[256];
        DWORD len = sizeof cpu;
        if (RegQueryValueExW(key, L"ProcessorNameString", NULL, NULL, (LPBYTE)cpu, &len) == ERROR_SUCCESS) {
            SYSTEM_INFO si;
            GetNativeSystemInfo(&si);
            _snwprintf(buf, 256, L"Processor      %s  ·  %lu threads", cpu, si.dwNumberOfProcessors);
            SetWindowTextW(g.home_info[2], buf);
        }
        RegCloseKey(key);
    }

    wchar_t windir[MAX_PATH];
    if (GetWindowsDirectoryW(windir, MAX_PATH)) {
        wchar_t drive[4] = { windir[0], L':', L'\\', 0 };
        ULARGE_INTEGER total, free_b;
        if (GetDiskFreeSpaceExW(drive, NULL, &total, &free_b)) {
            _snwprintf(buf, 256, L"Storage        %s  %.0f GB free of %.0f GB", drive,
                       (double)free_b.QuadPart / (1024.0 * 1024 * 1024),
                       (double)total.QuadPart / (1024.0 * 1024 * 1024));
            SetWindowTextW(g.home_info[3], buf);
        }
    }

    _snwprintf(buf, 256, L"Software       %d installed apps  ·  %d services", g.iapp_count, g.svc_count);
    SetWindowTextW(g.home_info[4], buf);

    _snwprintf(buf, 256, L"Catalogs       %d apps · %d tweaks · %d features · %d fixes · %d panels",
               g.apps.count, g.tweaks.count, g.features.count, g.fixes.count, g.panels.count);
    SetWindowTextW(g.home_info[5], buf);
}

/* ------------------------------------------------------------------ */
/* Section creation                                                    */
/* ------------------------------------------------------------------ */

static void create_section_header(int sec, const wchar_t *title, const wchar_t *desc) {
    g.section_title[sec] = mk(L"STATIC", title, 0, 0);
    SendMessageW(g.section_title[sec], WM_SETFONT, (WPARAM)g.font_title, TRUE);
    g.section_desc[sec] = mk(L"STATIC", desc, 0, 0);
}

static void create_home(void) {
    create_section_header(SEC_HOME, L"Home", L"System overview. Uptime counts live; tiles refresh every second.");
    g.cards[0].title = L"UPTIME";
    g.cards[1].title = L"CPU";
    g.cards[2].title = L"MEMORY";
    g.cards[3].title = L"PROCESSES";
    for (int i = 0; i < HOME_CARDS; i++) {
        wcscpy(g.cards[i].value, L"—");
        g.card_wnd[i] = mk(L"STATIC", L"", SS_OWNERDRAW, ID_CARD_FIRST + i);
    }
    for (int i = 0; i < HOME_INFO; i++) g.home_info[i] = mk(L"STATIC", L"", 0, 0);
    g.particle_canvas = mk(L"STATIC", L"", SS_OWNERDRAW, ID_PARTICLES);
}

static void create_sections(void) {
    create_home();

    create_section_header(SEC_STORE, L"App store",
        L"Install, remove, and upgrade applications from the curated catalog — winget, scoop, choco, pip, npm, dotnet, PowerShell Gallery.");
    mk_search(SEC_STORE, L"Search 375 apps by name, category, or source…");
    g.list[SEC_STORE] = mk_list(SEC_STORE);
    lv_add_column(g.list[SEC_STORE], 0, L"Application", 230);
    lv_add_column(g.list[SEC_STORE], 1, L"Category", 130);
    lv_add_column(g.list[SEC_STORE], 2, L"Sources", 140);
    lv_add_column(g.list[SEC_STORE], 3, L"Description", 340);
    mk_action(SEC_STORE, L"Install", ID_BTN_STORE_INSTALL, true);
    mk_action(SEC_STORE, L"Uninstall", ID_BTN_STORE_UNINSTALL, false);
    mk_action(SEC_STORE, L"Upgrade", ID_BTN_STORE_UPGRADE, false);
    mk_action(SEC_STORE, L"Check installed", ID_BTN_STORE_STATUS, false);

    create_section_header(SEC_APPS, L"Installed apps",
        L"Everything registered on this PC — the same data the classic Programs and Features panel shows.");
    mk_search(SEC_APPS, L"Search installed apps…");
    g.list[SEC_APPS] = mk_list(SEC_APPS);
    lv_add_column(g.list[SEC_APPS], 0, L"Application", 320);
    lv_add_column(g.list[SEC_APPS], 1, L"Version", 110);
    lv_add_column(g.list[SEC_APPS], 2, L"Publisher", 240);
    mk_action(SEC_APPS, L"Uninstall", ID_BTN_APP_UNINSTALL, true);
    mk_action(SEC_APPS, L"Open location", ID_BTN_APP_LOCATION, false);
    mk_action(SEC_APPS, L"Refresh", ID_BTN_APP_REFRESH, false);

    create_section_header(SEC_TWEAKS, L"Tweaks",
        L"Privacy and system tweaks. Select one, then apply, undo, or check its current state.");
    mk_search(SEC_TWEAKS, L"Search tweaks…");
    g.list[SEC_TWEAKS] = mk_list(SEC_TWEAKS);
    lv_add_column(g.list[SEC_TWEAKS], 0, L"Tweak", 230);
    lv_add_column(g.list[SEC_TWEAKS], 1, L"Risk", 90);
    lv_add_column(g.list[SEC_TWEAKS], 2, L"Reversible", 84);
    lv_add_column(g.list[SEC_TWEAKS], 3, L"Status", 100);
    lv_add_column(g.list[SEC_TWEAKS], 4, L"Description", 300);
    mk_action(SEC_TWEAKS, L"Apply", ID_BTN_TWEAK_APPLY, true);
    mk_action(SEC_TWEAKS, L"Undo", ID_BTN_TWEAK_UNDO, false);
    mk_action(SEC_TWEAKS, L"Check status", ID_BTN_TWEAK_DETECT, false);
    mk_action(SEC_TWEAKS, L"Check all", ID_BTN_TWEAK_DETECT_ALL, false);

    create_section_header(SEC_FEATURES, L"Windows features",
        L"Enable or disable optional Windows features (WSL, Hyper-V, Sandbox…). Changes may require a reboot.");
    g.list[SEC_FEATURES] = mk_list(SEC_FEATURES);
    lv_add_column(g.list[SEC_FEATURES], 0, L"Feature", 230);
    lv_add_column(g.list[SEC_FEATURES], 1, L"Status", 100);
    lv_add_column(g.list[SEC_FEATURES], 2, L"Description", 420);
    mk_action(SEC_FEATURES, L"Enable", ID_BTN_FEATURE_ENABLE, true);
    mk_action(SEC_FEATURES, L"Disable", ID_BTN_FEATURE_DISABLE, false);
    mk_action(SEC_FEATURES, L"Check status", ID_BTN_FEATURE_DETECT, false);

    create_section_header(SEC_SERVICES, L"Services",
        L"Start, stop, and reconfigure Windows services. Changing services usually requires running Phantom as administrator.");
    mk_search(SEC_SERVICES, L"Search services…");
    g.list[SEC_SERVICES] = mk_list(SEC_SERVICES);
    lv_add_column(g.list[SEC_SERVICES], 0, L"Service", 280);
    lv_add_column(g.list[SEC_SERVICES], 1, L"Status", 90);
    lv_add_column(g.list[SEC_SERVICES], 2, L"Startup", 100);
    lv_add_column(g.list[SEC_SERVICES], 3, L"Name", 170);
    mk_action(SEC_SERVICES, L"Start", ID_BTN_SVC_START, true);
    mk_action(SEC_SERVICES, L"Stop", ID_BTN_SVC_STOP, false);
    mk_action(SEC_SERVICES, L"Restart", ID_BTN_SVC_RESTART, false);
    mk_action(SEC_SERVICES, L"Set startup", ID_BTN_SVC_SET_STARTUP, false);
    mk_action(SEC_SERVICES, L"Refresh", ID_BTN_SVC_REFRESH, false);
    g.svc_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_COMBO_SVC_STARTUP);
    SendMessageW(g.svc_combo, CB_ADDSTRING, 0, (LPARAM)L"Automatic");
    SendMessageW(g.svc_combo, CB_ADDSTRING, 0, (LPARAM)L"Manual");
    SendMessageW(g.svc_combo, CB_ADDSTRING, 0, (LPARAM)L"Disabled");
    SendMessageW(g.svc_combo, CB_SETCURSEL, 0, 0);
    if (g_allow_dark_for_window) g_allow_dark_for_window(g.svc_combo, TRUE);
    SetWindowTheme(g.svc_combo, L"DarkMode_CFD", NULL);

    create_section_header(SEC_PANELS, L"Legacy panels",
        L"Shortcuts to the classic Windows control panels that newer Settings pages hide.");
    g.list[SEC_PANELS] = mk_list(SEC_PANELS);
    lv_add_column(g.list[SEC_PANELS], 0, L"Panel", 230);
    lv_add_column(g.list[SEC_PANELS], 1, L"Description", 500);
    mk_action(SEC_PANELS, L"Open", ID_BTN_PANEL_OPEN, true);

    create_section_header(SEC_FIXES, L"Quick fixes",
        L"One-shot repairs: DNS flush, Windows Update reset, WinGet repair, and more.");
    g.list[SEC_FIXES] = mk_list(SEC_FIXES);
    lv_add_column(g.list[SEC_FIXES], 0, L"Fix", 230);
    lv_add_column(g.list[SEC_FIXES], 1, L"Risk", 90);
    lv_add_column(g.list[SEC_FIXES], 2, L"Reversible", 84);
    lv_add_column(g.list[SEC_FIXES], 3, L"Description", 380);
    mk_action(SEC_FIXES, L"Run fix", ID_BTN_FIX_RUN, true);
    mk_action(SEC_FIXES, L"Undo", ID_BTN_FIX_UNDO, false);

    create_section_header(SEC_UPDATES, L"Windows Update",
        L"Choose how Windows Update behaves. \"Disable all\" is dangerous and stops update services entirely.");
    g.upd_radio[0] = mk_radio(L"Default — Windows manages updates normally", ID_RADIO_UPDATE_DEFAULT, WS_GROUP);
    g.upd_radio[1] = mk_radio(L"Security focused — defer feature updates 365 days, quality updates 4 days", ID_RADIO_UPDATE_SECURITY, 0);
    g.upd_radio[2] = mk_radio(L"Disable all — stop and disable update services (dangerous)", ID_RADIO_UPDATE_DISABLE, 0);
    SendMessageW(g.upd_radio[0], BM_SETCHECK, BST_CHECKED, 0);
    mk_action(SEC_UPDATES, L"Apply mode", ID_BTN_UPDATE_APPLY, true);
    mk_action(SEC_UPDATES, L"Check current mode", ID_BTN_UPDATE_DETECT, false);

    /* Settings: appearance + safety + automation + about */
    create_section_header(SEC_SETTINGS, L"Settings",
        L"Appearance, safety options, unattended automation, and information about this build.");

    g.set_head[0] = mk(L"STATIC", L"APPEARANCE", 0, 0);
    SendMessageW(g.set_head[0], WM_SETFONT, (WPARAM)g.font_group, TRUE);
    g.theme_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_COMBO_THEME);
    SendMessageW(g.theme_combo, CB_ADDSTRING, 0, (LPARAM)L"Verdant — dark green");
    SendMessageW(g.theme_combo, CB_ADDSTRING, 0, (LPARAM)L"Void — black · violet");
    SendMessageW(g.theme_combo, CB_SETCURSEL, (WPARAM)g.theme_id, 0);
    SetWindowTheme(g.theme_combo, L"DarkMode_CFD", NULL);
    g.accent_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_COMBO_ACCENT);
    SendMessageW(g.accent_combo, CB_ADDSTRING, 0, (LPARAM)L"Theme accent");
    SendMessageW(g.accent_combo, CB_ADDSTRING, 0, (LPARAM)L"Windows accent");
    SendMessageW(g.accent_combo, CB_ADDSTRING, 0, (LPARAM)L"Custom…");
    SendMessageW(g.accent_combo, CB_SETCURSEL, (WPARAM)g.accent_mode, 0);
    SetWindowTheme(g.accent_combo, L"DarkMode_CFD", NULL);
    g.accent_hex = mk_edit(ID_EDIT_ACCENT_HEX, L"#8052FF");
    {
        wchar_t hex[16];
        _snwprintf(hex, 16, L"#%02X%02X%02X", GetRValue(g.accent_custom), GetGValue(g.accent_custom), GetBValue(g.accent_custom));
        SetWindowTextW(g.accent_hex, hex);
    }
    mk_action(SEC_SETTINGS, L"Apply accent", ID_BTN_ACCENT_APPLY, false);

    g.set_head[1] = mk(L"STATIC", L"SAFETY", 0, 0);
    SendMessageW(g.set_head[1], WM_SETFONT, (WPARAM)g.font_group, TRUE);
    g.set_chk[0] = mk_check(L"Dry run only — log what would happen, change nothing", ID_CHK_SET_DRYRUN);
    g.set_chk[1] = mk_check(L"Create a system restore point before dangerous operations", ID_CHK_SET_RESTORE);
    g.set_chk[2] = mk_check(L"Continue even if pre-change state capture fails", ID_CHK_SET_SKIPCAPTURE);
    SendMessageW(g.set_chk[0], BM_SETCHECK, g.opt_dry ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.set_chk[1], BM_SETCHECK, g.opt_restore ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.set_chk[2], BM_SETCHECK, g.opt_skip ? BST_CHECKED : BST_UNCHECKED, 0);

    g.set_head[2] = mk(L"STATIC", L"AUTOMATION — RUN A CONFIG", 0, 0);
    SendMessageW(g.set_head[2], WM_SETFONT, (WPARAM)g.font_group, TRUE);
    g.auto_path = mk_edit(ID_EDIT_AUTO_PATH, L"Path to automation config (.json)…");
    g.auto_browse = mk(L"BUTTON", L"Browse…", BS_OWNERDRAW, ID_BTN_AUTO_BROWSE);
    enable_hover(g.auto_browse);
    g.auto_force = mk_check(L"Allow dangerous operations (-ForceDangerous)", ID_CHK_AUTO_FORCE);
    g.auto_ack_label = mk(L"STATIC", L"Acknowledgement token (dangerous configs):", 0, 0);
    g.auto_ack = mk_edit(ID_EDIT_AUTO_ACK, L"I_UNDERSTAND_NO_ROLLBACK");
    mk_action(SEC_SETTINGS, L"Dry run", ID_BTN_AUTO_DRYRUN, true);
    mk_action(SEC_SETTINGS, L"Run config", ID_BTN_AUTO_RUN, false);

    g.set_head[3] = mk(L"STATIC", L"ABOUT", 0, 0);
    SendMessageW(g.set_head[3], WM_SETFONT, (WPARAM)g.font_group, TRUE);
    g.about_line[0] = mk(L"STATIC", L"", 0, 0);
    g.about_line[1] = mk(L"STATIC", L"GPL-3.0 · " PHANTOM_REPO_URL, 0, 0);
    g.about_line[2] = mk(L"STATIC", L"", 0, 0);
    mk_action(SEC_SETTINGS, L"Open GitHub", ID_BTN_ABOUT_GITHUB, false);
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static void init_log_file(void) {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%s\\Phantom", base);
    CreateDirectoryW(dir, NULL);
    _snwprintf(dir, MAX_PATH, L"%s\\Phantom\\logs", base);
    CreateDirectoryW(dir, NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(g.log_file, MAX_PATH, L"%s\\phantom-%04u%02u%02u.log", dir, st.wYear, st.wMonth, st.wDay);
}

static HWND mk_small_button(const wchar_t *label, int id) {
    HWND b = mk(L"BUTTON", label, WS_VISIBLE | BS_OWNERDRAW, id);
    SetWindowLongPtrW(b, GWLP_USERDATA, 0);
    enable_hover(b);
    return b;
}

static void create_console(void) {
    g.console_title = mk(L"STATIC", L"Console", WS_VISIBLE, 0);
    SendMessageW(g.console_title, WM_SETFONT, (WPARAM)g.font_semibold, TRUE);
    g.console_badge = mk(L"STATIC", L"", WS_VISIBLE | SS_OWNERDRAW, ID_BADGE);
    g.log_edit = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                 0, 0, 10, 10, g.wnd, NULL, g.inst, NULL);
    SendMessageW(g.log_edit, WM_SETFONT, (WPARAM)g.font_mono, TRUE);
    if (g_allow_dark_for_window) g_allow_dark_for_window(g.log_edit, TRUE);
    SetWindowTheme(g.log_edit, L"DarkMode_Explorer", NULL);
    mk_small_button(L"Copy", ID_BTN_LOG_COPY);
    mk_small_button(L"Clear", ID_BTN_LOG_CLEAR);
    mk_small_button(L"Logs folder", ID_BTN_LOG_FOLDER);
    g.status_label = mk(L"STATIC", L"Ready", WS_VISIBLE, 0);
}

static HWND console_button(int id) {
    return GetDlgItem(g.wnd, id);
}

static void append_log(const wchar_t *line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamped[2200];
    _snwprintf(stamped, 2200, L"[%02u:%02u:%02u]  %s", st.wHour, st.wMinute, st.wSecond, line);

    int len = GetWindowTextLengthW(g.log_edit);
    SendMessageW(g.log_edit, EM_SETSEL, len, len);
    SendMessageW(g.log_edit, EM_REPLACESEL, FALSE, (LPARAM)stamped);
    SendMessageW(g.log_edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g.log_edit, EM_SCROLLCARET, 0, 0);

    if (g.log_file[0]) {
        FILE *f = _wfopen(g.log_file, L"ab");
        if (f) {
            char utf8[4400];
            wide_to_utf8(stamped, utf8, sizeof utf8);
            fprintf(f, "%s\n", utf8);
            fclose(f);
        }
    }
}

static void copy_log_to_clipboard(void) {
    int len = GetWindowTextLengthW(g.log_edit);
    if (len <= 0 || !OpenClipboard(g.wnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)len + 1) * sizeof(wchar_t));
    if (mem) {
        wchar_t *p = (wchar_t *)GlobalLock(mem);
        GetWindowTextW(g.log_edit, p, len + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    set_status_text(L"Console copied to clipboard.");
}

static void open_logs_folder(void) {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%s\\Phantom\\logs", base);
        ShellExecuteW(g.wnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* ------------------------------------------------------------------ */
/* Layout and section switching                                        */
/* ------------------------------------------------------------------ */

static void show_section_controls(int sec, BOOL show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    if (g.section_title[sec]) ShowWindow(g.section_title[sec], cmd);
    if (g.section_desc[sec]) ShowWindow(g.section_desc[sec], cmd);
    if (g.list[sec]) ShowWindow(g.list[sec], cmd);
    if (g.search[sec]) ShowWindow(g.search[sec], cmd);
    for (int i = 0; i < g.button_count[sec]; i++) ShowWindow(g.buttons[sec][i], cmd);
    switch (sec) {
        case SEC_HOME:
            for (int i = 0; i < HOME_CARDS; i++) ShowWindow(g.card_wnd[i], cmd);
            for (int i = 0; i < HOME_INFO; i++) ShowWindow(g.home_info[i], cmd);
            ShowWindow(g.particle_canvas, cmd);
            break;
        case SEC_SERVICES:
            ShowWindow(g.svc_combo, cmd);
            break;
        case SEC_UPDATES:
            for (int i = 0; i < 3; i++) ShowWindow(g.upd_radio[i], cmd);
            break;
        case SEC_SETTINGS:
            for (int i = 0; i < 4; i++) ShowWindow(g.set_head[i], cmd);
            for (int i = 0; i < 3; i++) ShowWindow(g.set_chk[i], cmd);
            ShowWindow(g.theme_combo, cmd); ShowWindow(g.accent_combo, cmd);
            ShowWindow(g.accent_hex, cmd);
            ShowWindow(g.auto_path, cmd); ShowWindow(g.auto_browse, cmd);
            ShowWindow(g.auto_force, cmd); ShowWindow(g.auto_ack_label, cmd);
            ShowWindow(g.auto_ack, cmd);
            for (int i = 0; i < 3; i++) ShowWindow(g.about_line[i], cmd);
            break;
        default:
            break;
    }
}

static void select_section(int sec) {
    if (sec < 0 || sec >= SEC_COUNT) return;
    if (sec != g.active) g.nav_anim_start = GetTickCount64();
    show_section_controls(g.active, FALSE);
    g.active = sec;
    show_section_controls(sec, TRUE);
    for (int i = 0; i < SEC_COUNT; i++) InvalidateRect(g.nav[i], NULL, TRUE);
    if (sec == SEC_APPS && !g.iapps) reload_installed_apps();
    if (sec == SEC_SERVICES && !g.services) reload_services();
    if (sec == SEC_STORE) show_count(g.store_filter_count, g.apps.count, L"apps");
    else if (sec == SEC_APPS) show_count(g.iapp_filter_count, g.iapp_count, L"installed apps");
    else if (sec == SEC_SERVICES) show_count(g.svc_filter_count, g.svc_count, L"services");
    else set_status_text(L"Ready");
}

static void layout(void) {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int W = rc.right, H = rc.bottom;
    int content_x = SIDEBAR_W + 24;
    int content_w = W - content_x - 24;
    int status_y = H - 26;
    int console_top = status_y - CONSOLE_H;

    /* Sidebar */
    int y = 14;
    int gi = 0;
    for (int i = 0; i < SEC_COUNT; i++) {
        if (NAV[i].group) {
            int gh = NAV[i].group[0] ? 22 : 10;
            MoveWindow(g.nav_groups[gi], 0, y, SIDEBAR_W, gh, TRUE);
            y += gh + 4;
            gi++;
        }
        MoveWindow(g.nav[i], 8, y, SIDEBAR_W - 16, 34, TRUE);
        y += 37;
    }

    /* Console: header row + log + status */
    MoveWindow(g.console_title, content_x, console_top + 2, 64, 22, TRUE);
    MoveWindow(g.console_badge, content_x + 70, console_top + 1, 76, 22, TRUE);
    int bx = content_x + content_w - 3 * 92 + 8;
    MoveWindow(console_button(ID_BTN_LOG_COPY), bx, console_top - 2, 84, 26, TRUE);
    MoveWindow(console_button(ID_BTN_LOG_CLEAR), bx + 92, console_top - 2, 84, 26, TRUE);
    MoveWindow(console_button(ID_BTN_LOG_FOLDER), bx + 184, console_top - 2, 84, 26, TRUE);
    MoveWindow(g.log_edit, content_x, console_top + 30, content_w, CONSOLE_H - 36, TRUE);
    MoveWindow(g.status_label, content_x, status_y + 2, content_w, 20, TRUE);

    /* Headers */
    int cy = 18;
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (g.section_title[sec]) MoveWindow(g.section_title[sec], content_x, cy, content_w, 30, TRUE);
        if (g.section_desc[sec]) MoveWindow(g.section_desc[sec], content_x, cy + 34, content_w, 34, TRUE);
    }
    int body_y = cy + 78;
    int btn_y = console_top - 46;

    /* Home */
    int card_w = (content_w - 3 * 14) / 4;
    int first_w = content_w - 3 * (card_w + 14);
    int cx = content_x;
    for (int i = 0; i < HOME_CARDS; i++) {
        int w = i == 0 ? first_w : card_w;
        MoveWindow(g.card_wnd[i], cx, body_y, w, 88, TRUE);
        cx += w + 14;
    }
    {
        int info_w = content_w * 52 / 100;
        for (int i = 0; i < HOME_INFO; i++)
            MoveWindow(g.home_info[i], content_x + 2, body_y + 110 + i * 27, info_w, 24, TRUE);
        int px = content_x + info_w + 24;
        int ph = (console_top - 56) - (body_y + 104);
        if (ph < 120) ph = 120;
        MoveWindow(g.particle_canvas, px, body_y + 104, content_x + content_w - px, ph, TRUE);
        ph_d2d_resize(content_x + content_w - px, ph);
    }

    /* Search rows */
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (!g.search[sec]) continue;
        MoveWindow(g.search[sec], content_x, body_y, 340, 28, TRUE);
    }

    /* Lists */
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (!g.list[sec]) continue;
        int ly = g.search[sec] ? body_y + 38 : body_y;
        MoveWindow(g.list[sec], content_x, ly, content_w, btn_y - ly - 12, TRUE);
    }

    /* Button rows */
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (sec == SEC_SETTINGS) continue; /* settings buttons are placed inline */
        int x = content_x;
        for (int i = 0; i < g.button_count[sec]; i++) {
            MoveWindow(g.buttons[sec][i], x, btn_y, 132, 31, TRUE);
            x += 140;
        }
        if (sec == SEC_SERVICES) MoveWindow(g.svc_combo, x, btn_y + 2, 124, 200, TRUE);
    }

    /* Updates radios */
    for (int i = 0; i < 3; i++)
        MoveWindow(g.upd_radio[i], content_x, body_y + i * 32, content_w, 26, TRUE);

    /* Settings page: APPEARANCE / SAFETY / AUTOMATION / ABOUT */
    int sy = body_y;
    MoveWindow(g.set_head[0], content_x, sy, content_w, 18, TRUE);
    sy += 22;
    MoveWindow(g.theme_combo, content_x, sy, 200, 200, TRUE);
    MoveWindow(g.accent_combo, content_x + 212, sy, 160, 200, TRUE);
    MoveWindow(g.accent_hex, content_x + 384, sy, 100, 26, TRUE);
    MoveWindow(g.buttons[SEC_SETTINGS][3], content_x + 496, sy - 1, 120, 28, TRUE);
    sy += 38;
    MoveWindow(g.set_head[1], content_x, sy, content_w, 18, TRUE);
    sy += 22;
    for (int i = 0; i < 3; i++) { MoveWindow(g.set_chk[i], content_x, sy, content_w, 24, TRUE); sy += 26; }
    sy += 12;
    MoveWindow(g.set_head[2], content_x, sy, content_w, 18, TRUE);
    sy += 22;
    MoveWindow(g.auto_path, content_x, sy, content_w - 466, 27, TRUE);
    MoveWindow(g.auto_browse, content_x + content_w - 456, sy - 1, 96, 29, TRUE);
    MoveWindow(g.buttons[SEC_SETTINGS][0], content_x + content_w - 350, sy - 1, 116, 29, TRUE);
    MoveWindow(g.buttons[SEC_SETTINGS][1], content_x + content_w - 226, sy - 1, 116, 29, TRUE);
    sy += 34;
    MoveWindow(g.auto_force, content_x, sy, 350, 24, TRUE);
    MoveWindow(g.auto_ack_label, content_x + 370, sy + 2, 290, 20, TRUE);
    MoveWindow(g.auto_ack, content_x + 664, sy - 2, 250, 26, TRUE);
    sy += 34;
    MoveWindow(g.set_head[3], content_x, sy, content_w, 18, TRUE);
    sy += 22;
    for (int i = 0; i < 3; i++) { MoveWindow(g.about_line[i], content_x, sy, content_w - 160, 22, TRUE); sy += 24; }
    MoveWindow(g.buttons[SEC_SETTINGS][2], content_x + content_w - 132, sy - 70, 132, 29, TRUE);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void enable_actions(BOOL enable) {
    for (int sec = 0; sec < SEC_COUNT; sec++)
        for (int i = 0; i < g.button_count[sec]; i++)
            EnableWindow(g.buttons[sec][i], enable);
}

static void job_defaults(job *j) {
    memset(j, 0, sizeof *j);
    j->item = -1;
    j->dry_run = SendMessageW(g.set_chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED;
    j->create_restore_point = SendMessageW(g.set_chk[1], BM_GETCHECK, 0, 0) == BST_CHECKED;
    j->skip_capture_check = SendMessageW(g.set_chk[2], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static const ph_catalog_entry *selected_entry(int sec, const entry_list *list, int *item_out) {
    int item = lv_selected(g.list[sec]);
    if (item_out) *item_out = item;
    if (item < 0) {
        set_status_text(L"Select an item first.");
        return NULL;
    }
    if (sec == SEC_STORE) {
        if (item >= g.store_filter_count) return NULL;
        return &g.apps.items[g.store_filter[item]];
    }
    if (item >= list->count) return NULL;
    return &list->items[item];
}

static void start_catalog_job(int sec, const entry_list *list,
                              bool (*factory)(const char *, ph_operation *, ph_error *),
                              job_kind kind, bool undo) {
    int item;
    const ph_catalog_entry *e = selected_entry(sec, list, &item);
    if (!e) return;
    job j;
    job_defaults(&j);
    j.kind = kind;
    j.section = sec;
    j.item = item;
    j.undo = undo;
    ph_error err = {0};
    if (!factory(e->id, &j.op, &err)) { post_logf("%s", err.message); return; }
    if (undo && j.op.undo_count == 0) {
        post_logf("%s has no undo script", j.op.id);
        set_status_text(L"This item cannot be undone automatically.");
        return;
    }
    if (kind == JOB_OPERATION && !undo && !j.dry_run) {
        bool force = false;
        if (!confirm_if_dangerous(&j.op, &force)) return;
        j.force_dangerous = force || (!j.op.destructive && j.op.risk != PH_RISK_DANGEROUS);
    } else {
        j.force_dangerous = true;
    }
    start_job(&j);
}

static void start_store_job(const char *action) {
    int item;
    const ph_catalog_entry *e = selected_entry(SEC_STORE, &g.apps, &item);
    if (!e) return;
    job j;
    job_defaults(&j);
    j.kind = JOB_OPERATION;
    j.section = SEC_STORE;
    j.item = item;
    ph_error err = {0};
    bool ok;
    if (strcmp(action, "status") == 0) ok = ph_make_store_status_operation(e->id, NULL, &j.op, &err);
    else ok = ph_make_store_operation(e->id, action, &j.op, &err);
    if (!ok) { post_logf("%s", err.message); return; }
    j.force_dangerous = true;
    start_job(&j);
}

static void start_app_uninstall(void) {
    int item = lv_selected(g.list[SEC_APPS]);
    if (item < 0 || item >= g.iapp_filter_count) { set_status_text(L"Select an app first."); return; }
    const ph_installed_app *a = &g.iapps[g.iapp_filter[item]];
    if (!a->uninstall[0]) { set_status_text(L"This app does not provide an uninstall command."); return; }

    wchar_t *name = utf8_to_wide_dup(a->name);
    wchar_t msg[512];
    _snwprintf(msg, 512, L"Uninstall \"%s\"?\n\nThe app's own uninstaller will run and may show its own windows.", name ? name : L"app");
    free(name);
    if (MessageBoxW(g.wnd, msg, L"Confirm uninstall", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;

    job j;
    job_defaults(&j);
    j.kind = JOB_APP_UNINSTALL;
    snprintf(j.command, sizeof j.command, "%s", a->uninstall);
    snprintf(j.target, sizeof j.target, "%s", a->name);
    start_job(&j);
}

static void open_app_location(void) {
    int item = lv_selected(g.list[SEC_APPS]);
    if (item < 0 || item >= g.iapp_filter_count) { set_status_text(L"Select an app first."); return; }
    const ph_installed_app *a = &g.iapps[g.iapp_filter[item]];
    if (!a->install_location[0]) { set_status_text(L"No install location is recorded for this app."); return; }
    wchar_t *w = utf8_to_wide_dup(a->install_location);
    if (w) {
        ShellExecuteW(g.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        free(w);
    }
}

static void start_service_job(const char *action) {
    int item = lv_selected(g.list[SEC_SERVICES]);
    if (item < 0 || item >= g.svc_filter_count) { set_status_text(L"Select a service first."); return; }
    const ph_service_row *s = &g.services[g.svc_filter[item]];
    job j;
    job_defaults(&j);
    j.kind = JOB_SERVICE;
    snprintf(j.command, sizeof j.command, "%s", s->name);
    if (strcmp(action, "set-startup") == 0) {
        int sel = (int)SendMessageW(g.svc_combo, CB_GETCURSEL, 0, 0);
        const char *startup = sel == 0 ? "auto" : sel == 1 ? "manual" : "disabled";
        snprintf(j.target, sizeof j.target, "startup:%s", startup);
    } else {
        snprintf(j.target, sizeof j.target, "%s", action);
    }
    start_job(&j);
}

static void start_update_job(bool detect) {
    const char *mode = "Default";
    if (SendMessageW(g.upd_radio[1], BM_GETCHECK, 0, 0) == BST_CHECKED) mode = "Security";
    if (SendMessageW(g.upd_radio[2], BM_GETCHECK, 0, 0) == BST_CHECKED) mode = "Disable All";
    job j;
    job_defaults(&j);
    j.kind = detect ? JOB_DETECT : JOB_OPERATION;
    j.section = SEC_UPDATES;
    j.item = -1;
    ph_error err = {0};
    if (!ph_make_update_operation(mode, &j.op, &err)) { post_logf("%s", err.message); return; }
    if (!detect && !j.dry_run) {
        bool force = false;
        if (!confirm_if_dangerous(&j.op, &force)) return;
        j.force_dangerous = force || (!j.op.destructive && j.op.risk != PH_RISK_DANGEROUS);
    } else {
        j.force_dangerous = true;
    }
    start_job(&j);
}

static void start_automation(bool dry_run) {
    job j;
    job_defaults(&j);
    j.kind = JOB_AUTOMATION;
    j.dry_run = j.dry_run || dry_run;
    wchar_t wpath[512], wack[128];
    GetWindowTextW(g.auto_path, wpath, 512);
    GetWindowTextW(g.auto_ack, wack, 128);
    wide_to_utf8(wpath, j.config_path, sizeof j.config_path);
    wide_to_utf8(wack, j.acknowledgement, sizeof j.acknowledgement);
    if (!j.config_path[0]) { set_status_text(L"Choose a config file first."); return; }
    j.force_dangerous = SendMessageW(g.auto_force, BM_GETCHECK, 0, 0) == BST_CHECKED;
    start_job(&j);
}

static void browse_config(void) {
    wchar_t path[512] = L"";
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"JSON config (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = 512;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g.auto_path, path);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static void on_command(WORD id, WORD code) {
    if (id >= ID_NAV_FIRST && id < ID_NAV_FIRST + SEC_COUNT) {
        select_section(id - ID_NAV_FIRST);
        return;
    }
    if (id >= ID_SEARCH_FIRST && id < ID_SEARCH_FIRST + SEC_COUNT && code == EN_CHANGE) {
        int sec = id - ID_SEARCH_FIRST;
        if (sec == SEC_STORE) fill_store();
        else if (sec == SEC_APPS) fill_installed_apps();
        else if (sec == SEC_SERVICES) fill_services();
        return;
    }
    switch (id) {
        case ID_BTN_TWEAK_APPLY:   start_catalog_job(SEC_TWEAKS, &g.tweaks, ph_make_tweak_operation, JOB_OPERATION, false); break;
        case ID_BTN_TWEAK_UNDO:    start_catalog_job(SEC_TWEAKS, &g.tweaks, ph_make_tweak_operation, JOB_OPERATION, true); break;
        case ID_BTN_TWEAK_DETECT:  start_catalog_job(SEC_TWEAKS, &g.tweaks, ph_make_tweak_operation, JOB_DETECT, false); break;
        case ID_BTN_TWEAK_DETECT_ALL: {
            job j;
            job_defaults(&j);
            j.kind = JOB_DETECT_ALL;
            start_job(&j);
            break;
        }
        case ID_BTN_FEATURE_ENABLE:  start_catalog_job(SEC_FEATURES, &g.features, ph_make_feature_operation, JOB_OPERATION, false); break;
        case ID_BTN_FEATURE_DISABLE: start_catalog_job(SEC_FEATURES, &g.features, ph_make_feature_operation, JOB_OPERATION, true); break;
        case ID_BTN_FEATURE_DETECT:  start_catalog_job(SEC_FEATURES, &g.features, ph_make_feature_operation, JOB_DETECT, false); break;
        case ID_BTN_FIX_RUN:  start_catalog_job(SEC_FIXES, &g.fixes, ph_make_fix_operation, JOB_OPERATION, false); break;
        case ID_BTN_FIX_UNDO: start_catalog_job(SEC_FIXES, &g.fixes, ph_make_fix_operation, JOB_OPERATION, true); break;
        case ID_BTN_PANEL_OPEN: start_catalog_job(SEC_PANELS, &g.panels, ph_make_panel_operation, JOB_OPERATION, false); break;
        case ID_BTN_STORE_INSTALL:   start_store_job("install"); break;
        case ID_BTN_STORE_UNINSTALL: start_store_job("uninstall"); break;
        case ID_BTN_STORE_UPGRADE:   start_store_job("upgrade"); break;
        case ID_BTN_STORE_STATUS:    start_store_job("status"); break;
        case ID_BTN_APP_UNINSTALL: start_app_uninstall(); break;
        case ID_BTN_APP_LOCATION:  open_app_location(); break;
        case ID_BTN_APP_REFRESH:   reload_installed_apps(); fill_home_info(); break;
        case ID_BTN_SVC_START:   start_service_job("start"); break;
        case ID_BTN_SVC_STOP:    start_service_job("stop"); break;
        case ID_BTN_SVC_RESTART: start_service_job("restart"); break;
        case ID_BTN_SVC_SET_STARTUP: start_service_job("set-startup"); break;
        case ID_BTN_SVC_REFRESH: reload_services(); fill_home_info(); break;
        case ID_BTN_UPDATE_APPLY:  start_update_job(false); break;
        case ID_BTN_UPDATE_DETECT: start_update_job(true); break;
        case ID_BTN_AUTO_BROWSE: browse_config(); break;
        case ID_BTN_AUTO_DRYRUN: start_automation(true); break;
        case ID_BTN_AUTO_RUN:    start_automation(false); break;
        case ID_BTN_ABOUT_GITHUB: ShellExecuteW(g.wnd, L"open", PHANTOM_REPO_URL, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_COMBO_THEME:
            if (code == CBN_SELCHANGE) {
                apply_theme((int)SendMessageW(g.theme_combo, CB_GETCURSEL, 0, 0));
                save_settings();
            }
            break;
        case ID_COMBO_ACCENT:
            if (code == CBN_SELCHANGE) {
                g.accent_mode = (int)SendMessageW(g.accent_combo, CB_GETCURSEL, 0, 0);
                apply_theme(g.theme_id);
                save_settings();
            }
            break;
        case ID_BTN_ACCENT_APPLY: {
            wchar_t whex[16];
            char hex[16];
            GetWindowTextW(g.accent_hex, whex, 16);
            wide_to_utf8(whex, hex, sizeof hex);
            COLORREF c = parse_hex_color(hex, 0xFFFFFFFF);
            if (c == 0xFFFFFFFF) { set_status_text(L"Enter a color as #RRGGBB."); break; }
            g.accent_custom = c;
            g.accent_mode = PH_ACCENT_CUSTOM;
            SendMessageW(g.accent_combo, CB_SETCURSEL, PH_ACCENT_CUSTOM, 0);
            apply_theme(g.theme_id);
            save_settings();
            break;
        }
        case ID_CHK_SET_DRYRUN:
        case ID_CHK_SET_RESTORE:
        case ID_CHK_SET_SKIPCAPTURE:
            save_settings();
            break;
        case ID_BTN_LOG_CLEAR:   SetWindowTextW(g.log_edit, L""); break;
        case ID_BTN_LOG_COPY:    copy_log_to_clipboard(); break;
        case ID_BTN_LOG_FOLDER:  open_logs_folder(); break;
        default: break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            RECT rc;
            GetClientRect(wnd, &rc);
            RECT side = { 0, 0, SIDEBAR_W, rc.bottom };
            FillRect(dc, &side, g.br_shell);
            RECT divider = { SIDEBAR_W, 0, SIDEBAR_W + 1, rc.bottom };
            HBRUSH border = CreateSolidBrush(CLR_BORDER);
            FillRect(dc, &divider, border);
            DeleteObject(border);
            RECT body = { SIDEBAR_W + 1, 0, rc.right, rc.bottom };
            FillRect(dc, &body, g.br_bg);
            return 1;
        }
        case WM_NOTIFY: {
            LPNMHDR hdr = (LPNMHDR)lp;
            if (hdr->code == NM_CUSTOMDRAW && hdr->idFrom >= 900 && hdr->idFrom < 900 + SEC_COUNT) {
                LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lp;
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT:
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT: {
                        bool sel = (ListView_GetItemState(hdr->hwndFrom, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        cd->clrText = CLR_TEXT;
                        cd->clrTextBk = sel ? CLR_ROW_SEL : ((cd->nmcd.dwItemSpec & 1) ? CLR_ROW_ALT : CLR_ROW);
                        return CDRF_NEWFONT;
                    }
                }
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lp;
            if (dis->CtlID >= ID_NAV_FIRST && dis->CtlID < ID_NAV_FIRST + SEC_COUNT) draw_nav_button(dis);
            else if (dis->CtlID >= ID_CARD_FIRST && dis->CtlID < ID_CARD_FIRST + HOME_CARDS) draw_home_card(dis);
            else if (dis->CtlID == ID_BADGE) draw_badge(dis);
            else if (dis->CtlID == ID_PARTICLES) {
                if (ph_d2d_active()) d2d_render_constellation();
                else draw_particles(dis);
            }
            else if (dis->CtlType == ODT_BUTTON) draw_action_button(dis);
            else if (dis->CtlType == ODT_STATIC) draw_nav_group(dis, dis->hwndItem);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            SetBkMode(dc, TRANSPARENT);
            if (ctl == g.log_edit) {
                SetTextColor(dc, CLR_MUTED);
                SetBkColor(dc, CLR_SHELL);
                return (LRESULT)g.br_shell;
            }
            bool muted = ctl == g.status_label || ctl == g.auto_ack_label ||
                         ctl == g.about_line[1] || ctl == g.about_line[2];
            for (int s = 0; s < SEC_COUNT && !muted; s++) muted = ctl == g.section_desc[s];
            bool faint = false;
            for (int i = 0; i < 4 && !faint; i++) faint = ctl == g.set_head[i];
            SetTextColor(dc, faint ? CLR_FAINT : muted ? CLR_MUTED : CLR_TEXT);
            return (LRESULT)g.br_bg;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, CLR_TEXT);
            SetBkColor(dc, CLR_INPUT);
            return (LRESULT)g.br_input;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, CLR_TEXT);
            SetBkColor(dc, CLR_INPUT);
            return (LRESULT)g.br_input;
        }
        case WM_CTLCOLORBTN:
            return (LRESULT)g.br_bg;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = MIN_W;
            mmi->ptMinTrackSize.y = MIN_H;
            return 0;
        }
        case WM_TIMER:
            if (wp == IDT_TICK) update_live_tiles();
            else if (wp == IDT_ANIM) {
                if (g.nav_anim_start) {
                    InvalidateRect(g.nav[g.active], NULL, FALSE);
                    if (nav_anim_progress() >= 1.0) g.nav_anim_start = 0;
                }
                if (g.active == SEC_HOME && g_theme.particles && g.particle_canvas) {
                    if (ph_d2d_active()) d2d_render_constellation();
                    else InvalidateRect(g.particle_canvas, NULL, FALSE);
                }
            }
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_APP_LOG:
            append_log((const wchar_t *)lp);
            free((void *)lp);
            return 0;
        case WM_APP_STATUS: {
            int sec = LOWORD(wp), item = HIWORD(wp);
            int col = (sec == SEC_TWEAKS) ? 3 : (sec == SEC_FEATURES) ? 1 : -1;
            if (col >= 0 && g.list[sec])
                ListView_SetItemText(g.list[sec], item, col, (LPWSTR)lp);
            free((void *)lp);
            return 0;
        }
        case WM_APP_DONE:
            if (g.worker) { CloseHandle(g.worker); g.worker = NULL; }
            InterlockedExchange(&g.busy, 0);
            enable_actions(TRUE);
            set_status_text(L"Ready");
            InvalidateRect(g.console_badge, NULL, TRUE);
            if (g.last_job == JOB_SERVICE) reload_services();
            if (g.last_job == JOB_APP_UNINSTALL) { reload_installed_apps(); fill_home_info(); }
            return 0;
        case WM_DESTROY:
            save_settings();
            ph_d2d_shutdown();
            KillTimer(wnd, IDT_TICK);
            KillTimer(wnd, IDT_ANIM);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

static void resolve_data_dir(void) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0';
    wchar_t wdata[MAX_PATH + 8];
    _snwprintf(wdata, MAX_PATH + 8, L"%s\\Data", exe);
    DWORD attr = GetFileAttributesW(wdata);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wide_to_utf8(wdata, g.data_dir, sizeof g.data_dir);
    } else {
        snprintf(g.data_dir, sizeof g.data_dir, "Data");
    }
    ph_operation_set_data_dir(g.data_dir);
}

static bool load_catalogs(void) {
    ph_error err = {0};
    if (!ph_catalog_integrity_validate(g.data_dir, &err)) {
        wchar_t *w = utf8_to_wide_dup(err.message);
        MessageBoxW(NULL, w ? w : L"Catalog integrity validation failed.", APP_TITLE, MB_ICONERROR | MB_OK);
        free(w);
        return false;
    }
    bool ok =
        ph_catalog_enumerate(g.data_dir, "tweaks.json", collect_entry, &g.tweaks, &err) &&
        ph_catalog_enumerate(g.data_dir, "features.json", collect_entry, &g.features, &err) &&
        ph_catalog_enumerate(g.data_dir, "fixes.json", collect_entry, &g.fixes, &err) &&
        ph_catalog_enumerate(g.data_dir, "legacy-panels.json", collect_entry, &g.panels, &err) &&
        ph_catalog_enumerate(g.data_dir, "catalog.apps.json", collect_entry, &g.apps, &err);
    if (!ok) {
        wchar_t *w = utf8_to_wide_dup(err.message);
        MessageBoxW(NULL, w ? w : L"Failed to load catalogs.", APP_TITLE, MB_ICONERROR | MB_OK);
        free(w);
        return false;
    }
    g.store_filter = (int *)calloc((size_t)(g.apps.count ? g.apps.count : 1), sizeof *g.store_filter);
    return g.store_filter != NULL;
}

static HFONT make_font(int height, int weight, const wchar_t *face) {
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

static void create_resources(void) {
    g.font = make_font(-13, FW_NORMAL, L"Segoe UI");
    g.font_semibold = make_font(-13, FW_SEMIBOLD, L"Segoe UI");
    g.font_group = make_font(-11, FW_SEMIBOLD, L"Segoe UI");
    g.font_icon = make_font(-16, FW_NORMAL, L"Segoe MDL2 Assets");
    g.font_mono = make_font(-12, FW_NORMAL, L"Consolas");
    rebuild_theme_resources();
}

/* (Re)creates everything that depends on the active theme: surfaces and
 * the display/title faces (Void uses ultra-thin weights, per DESIGN.md). */
static void rebuild_theme_resources(void) {
    if (g.br_bg) DeleteObject(g.br_bg);
    if (g.br_shell) DeleteObject(g.br_shell);
    if (g.br_input) DeleteObject(g.br_input);
    g.br_bg = CreateSolidBrush(CLR_BG);
    g.br_shell = CreateSolidBrush(CLR_SHELL);
    g.br_input = CreateSolidBrush(CLR_INPUT);
    if (g.font_title) DeleteObject(g.font_title);
    if (g.font_big) DeleteObject(g.font_big);
    g.font_title = make_font(-22, g_theme.title_weight, L"Segoe UI");
    g.font_big = make_font(g_theme.particles ? -36 : -30, g_theme.display_weight, L"Segoe UI");
    for (int sec = 0; sec < SEC_COUNT; sec++)
        if (g.section_title[sec]) SendMessageW(g.section_title[sec], WM_SETFONT, (WPARAM)g.font_title, TRUE);
}

/* Last-resort crash logger: a silent failure must never be silent again.
 * Writes the exception code, faulting address, and module base to
 * %LOCALAPPDATA%\Phantom\logs\crash.log. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        wchar_t dir[MAX_PATH], file[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%s\\Phantom", base);
        CreateDirectoryW(dir, NULL);
        _snwprintf(dir, MAX_PATH, L"%s\\Phantom\\logs", base);
        CreateDirectoryW(dir, NULL);
        _snwprintf(file, MAX_PATH, L"%s\\crash.log", dir);
        FILE *f = _wfopen(file, L"ab");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u] Phantom crashed: code=0x%08lX addr=%p base=%p\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
                    ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL,
                    (void *)GetModuleHandleW(NULL));
            fclose(f);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    SetUnhandledExceptionFilter(crash_handler);
    g.inst = inst;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    enable_dark_app_mode();

    resolve_data_dir();
    if (!load_catalogs()) return 1;

    load_settings();
    g_theme = PH_THEMES[g.theme_id];
    apply_accent_override();
    create_resources();
    init_log_file();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = WND_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    g.wnd = CreateWindowExW(0, WND_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780, NULL, NULL, inst, NULL);
    if (!g.wnd) return 1;
    dark_titlebar(g.wnd);

    create_nav();
    create_sections();
    create_console();

    wchar_t *wdata = utf8_to_wide_dup(g.data_dir);
    if (wdata) {
        wchar_t note[600];
        _snwprintf(note, 600, L"Catalog data: %s (SHA-256 verified at startup)", wdata);
        SetWindowTextW(g.about_line[2], note);
        free(wdata);
    }

    fill_tweaks();
    fill_features();
    fill_fixes();
    fill_panels();
    fill_store();
    reload_installed_apps();
    reload_services();
    fill_home_info();
    update_live_tiles();

    bool gpu = ph_d2d_init(g.particle_canvas);
    apply_theme(g.theme_id);
    g.active = SEC_HOME;
    select_section(SEC_HOME);
    SetTimer(g.wnd, IDT_TICK, 1000, NULL);
    SetTimer(g.wnd, IDT_ANIM, 33, NULL);
    post_logf("Phantom %ls ready — catalogs verified, %d apps / %d tweaks loaded.", PHANTOM_VERSION, g.apps.count, g.tweaks.count);
    post_logf("constellation renderer: %s", gpu ? "Direct2D (GPU, antialiased, vsync)" : "GDI (software fallback)");

    layout();
    ShowWindow(g.wnd, show);
    UpdateWindow(g.wnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

#else  /* !_WIN32 */

typedef int ph_gui_requires_windows;

#endif
