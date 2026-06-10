/* Phantom — native Win32 GUI over the phantom-c core.
 *
 * Design notes:
 *  - Navigation is grouped for clarity: Overview (Home), Software (Store),
 *    System (Tweaks, Features, Legacy panels), Maintenance (Fixes, Windows
 *    Update), Automation, and Settings.
 *  - The Home uptime is a stopwatch: it ticks every second from the
 *    monotonic GetTickCount64 clock instead of polling system queries.
 *  - All operations run on a single worker thread through the same
 *    operation engine and PowerShell safety validation as the CLI, with
 *    the same dangerous-operation confirmation and restore-point gates as
 *    the original app.
 */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phantom/catalog.h"
#include "phantom/process_runner.h"
#include "phantom/config.h"
#include "phantom/operation.h"
#include "phantom/ps_runner.h"
#include "phantom/status_parser.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define APP_TITLE      L"Phantom"
#define WND_CLASS      L"PhantomMainWindow"

#define SIDEBAR_W      210
#define LOG_H          150
#define STATUS_H       24
#define MIN_W          980
#define MIN_H          640

#define WM_APP_LOG     (WM_APP + 1)   /* lParam: heap wchar_t* line */
#define WM_APP_DONE    (WM_APP + 2)
#define WM_APP_STATUS  (WM_APP + 3)   /* wParam: packed section|item, lParam: heap wchar_t* */

#define IDT_UPTIME     1

enum section {
    SEC_HOME, SEC_STORE, SEC_TWEAKS, SEC_FEATURES, SEC_PANELS,
    SEC_FIXES, SEC_UPDATES, SEC_AUTOMATION, SEC_SETTINGS, SEC_COUNT
};

/* Control IDs */
enum {
    ID_NAV_FIRST = 100,                 /* + section */
    ID_BTN_TWEAK_APPLY = 200, ID_BTN_TWEAK_UNDO, ID_BTN_TWEAK_DETECT, ID_BTN_TWEAK_DETECT_ALL,
    ID_BTN_FEATURE_ENABLE, ID_BTN_FEATURE_DISABLE, ID_BTN_FEATURE_DETECT,
    ID_BTN_FIX_RUN, ID_BTN_FIX_UNDO,
    ID_BTN_PANEL_OPEN,
    ID_BTN_STORE_INSTALL, ID_BTN_STORE_UNINSTALL, ID_BTN_STORE_UPGRADE, ID_BTN_STORE_STATUS,
    ID_EDIT_STORE_SEARCH,
    ID_BTN_UPDATE_APPLY, ID_BTN_UPDATE_DETECT,
    ID_RADIO_UPDATE_DEFAULT, ID_RADIO_UPDATE_SECURITY, ID_RADIO_UPDATE_DISABLE,
    ID_EDIT_AUTO_PATH, ID_BTN_AUTO_BROWSE, ID_BTN_AUTO_DRYRUN, ID_BTN_AUTO_RUN,
    ID_CHK_AUTO_FORCE, ID_EDIT_AUTO_ACK,
    ID_CHK_SET_DRYRUN, ID_CHK_SET_RESTORE, ID_CHK_SET_SKIPCAPTURE,
    ID_BTN_LOG_CLEAR,
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    ph_catalog_entry *items;
    int count, cap;
} entry_list;

typedef enum {
    JOB_OPERATION,      /* run g_job.op through the engine */
    JOB_DETECT,         /* run g_job.op detect step only */
    JOB_DETECT_ALL,     /* detect every tweak */
    JOB_AUTOMATION,     /* run automation config */
} job_kind;

typedef struct {
    job_kind kind;
    ph_operation op;
    int section;             /* list section for status updates */
    int item;                /* list item index, -1 none */
    bool undo;               /* run undo steps instead of run steps */
    bool dry_run;
    bool create_restore_point;
    bool skip_capture_check;
    bool force_dangerous;    /* user confirmed */
    char config_path[512];
    char acknowledgement[128];
} job;

static struct {
    HINSTANCE inst;
    HWND wnd, status_bar, log_edit, log_clear;
    HWND nav[SEC_COUNT];
    HWND nav_groups[8];
    int nav_group_count;
    int active;
    HFONT font, font_bold, font_big, font_group;

    /* Home */
    HWND home_title, home_uptime_label, home_uptime, home_info[8], home_counts;

    /* generic list sections */
    HWND list[SEC_COUNT];
    HWND section_title[SEC_COUNT], section_desc[SEC_COUNT];
    HWND buttons[SEC_COUNT][4];
    int button_count[SEC_COUNT];

    /* Store */
    HWND store_search, store_search_label;
    int *store_filter;     /* visible row -> entry index */
    int store_filter_count;

    /* Updates */
    HWND upd_radio[3], upd_desc[3];

    /* Automation */
    HWND auto_path, auto_browse, auto_force, auto_ack_label, auto_ack, auto_dry, auto_run, auto_note;

    /* Settings */
    HWND set_chk[3], set_data_label;

    entry_list tweaks, features, fixes, panels, apps;
    char data_dir[512];

    HANDLE worker;
    volatile LONG busy;
    job current_job;
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
    SendMessageW(g.status_bar, SB_SETTEXTW, 0, (LPARAM)text);
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
        /* Swap run/undo so the engine drives the undo path with the same gates. */
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

static DWORD WINAPI worker_main(LPVOID param) {
    (void)param;
    job *j = &g.current_job;
    switch (j->kind) {
        case JOB_OPERATION:  run_single_operation(j); break;
        case JOB_DETECT:     run_detect(j); break;
        case JOB_DETECT_ALL: run_detect_all(j); break;
        case JOB_AUTOMATION: run_automation(j); break;
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
    enable_actions(FALSE);
    set_status_text(L"Working…");
    g.worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!g.worker) {
        InterlockedExchange(&g.busy, 0);
        enable_actions(TRUE);
        set_status_text(L"Failed to start worker thread.");
        return false;
    }
    return true;
}

/* Confirm dangerous/destructive work the same way the original app gates it. */
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
/* UI creation                                                         */
/* ------------------------------------------------------------------ */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10,
                             g.wnd, (HMENU)(INT_PTR)id, g.inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    return h;
}

static HWND mk_label(const wchar_t *text) { return mk(L"STATIC", text, WS_VISIBLE, 0); }

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
                              WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)(900 + section), g.inst, NULL);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g.font, TRUE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    return lv;
}

static const char *risk_label(const ph_catalog_entry *e) {
    if (e->risk[0]) return e->risk;
    return "Advanced";
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

static void fill_store(const char *filter) {
    HWND lv = g.list[SEC_STORE];
    ListView_DeleteAllItems(lv);
    g.store_filter_count = 0;
    int row = 0;
    for (int i = 0; i < g.apps.count; i++) {
        const ph_catalog_entry *e = &g.apps.items[i];
        if (filter && *filter &&
            !ph_contains_i(e->name, filter) &&
            !ph_contains_i(e->extra, filter) &&
            !ph_contains_i(e->description, filter)) continue;
        g.store_filter[g.store_filter_count++] = i;
        lv_set(lv, row, 0, e->name);
        lv_set(lv, row, 1, e->extra[0] ? e->extra : "—");
        lv_set(lv, row, 2, e->description);
        row++;
    }
    wchar_t label[64];
    _snwprintf(label, 64, L"%d of %d apps", row, g.apps.count);
    set_status_text(label);
}

static void create_nav(void) {
    static const struct { const wchar_t *group; int section; const wchar_t *label; } nav[] = {
        {L"OVERVIEW",   SEC_HOME,      L"Home"},
        {L"SOFTWARE",   SEC_STORE,     L"App store"},
        {L"SYSTEM",     SEC_TWEAKS,    L"Tweaks"},
        {NULL,          SEC_FEATURES,  L"Windows features"},
        {NULL,          SEC_PANELS,    L"Legacy panels"},
        {L"MAINTENANCE",SEC_FIXES,     L"Quick fixes"},
        {NULL,          SEC_UPDATES,   L"Windows Update"},
        {L"AUTOMATION", SEC_AUTOMATION,L"Run a config"},
        {L"",           SEC_SETTINGS,  L"Settings"},
    };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) {
        if (nav[i].group && nav[i].group[0]) {
            HWND grp = mk(L"STATIC", nav[i].group, WS_VISIBLE, 0);
            SendMessageW(grp, WM_SETFONT, (WPARAM)g.font_group, TRUE);
            g.nav_groups[g.nav_group_count++] = grp;
        } else if (nav[i].group) { /* empty string: spacer-only group */
            g.nav_groups[g.nav_group_count++] = mk(L"STATIC", L"", WS_VISIBLE, 0);
        }
        g.nav[nav[i].section] = mk(L"BUTTON", nav[i].label,
                                   WS_VISIBLE | BS_PUSHLIKE | BS_AUTOCHECKBOX,
                                   ID_NAV_FIRST + nav[i].section);
    }
}

static void create_home(void) {
    g.home_title = mk(L"STATIC", L"Home", WS_VISIBLE, 0);
    SendMessageW(g.home_title, WM_SETFONT, (WPARAM)g.font_bold, TRUE);
    g.home_uptime_label = mk_label(L"System uptime");
    g.home_uptime = mk(L"STATIC", L"0:00:00:00", WS_VISIBLE, 0);
    SendMessageW(g.home_uptime, WM_SETFONT, (WPARAM)g.font_big, TRUE);
    for (int i = 0; i < 8; i++) g.home_info[i] = mk_label(L"");
    g.home_counts = mk_label(L"");
}

static void create_section_header(int sec, const wchar_t *title, const wchar_t *desc) {
    g.section_title[sec] = mk(L"STATIC", title, 0, 0);
    SendMessageW(g.section_title[sec], WM_SETFONT, (WPARAM)g.font_bold, TRUE);
    g.section_desc[sec] = mk(L"STATIC", desc, 0, 0);
}

static HWND mk_button(int sec, const wchar_t *label, int id) {
    HWND b = mk(L"BUTTON", label, BS_PUSHBUTTON, id);
    g.buttons[sec][g.button_count[sec]++] = b;
    return b;
}

static void create_sections(void) {
    /* Store */
    create_section_header(SEC_STORE, L"App store",
        L"Install, remove, and upgrade applications from the curated catalog. Sources: winget, scoop, choco, pip, npm, dotnet, PowerShell Gallery.");
    g.store_search_label = mk(L"STATIC", L"Search:", 0, 0);
    g.store_search = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_STORE_SEARCH);
    g.list[SEC_STORE] = mk_list(SEC_STORE);
    lv_add_column(g.list[SEC_STORE], 0, L"Application", 220);
    lv_add_column(g.list[SEC_STORE], 1, L"Category", 130);
    lv_add_column(g.list[SEC_STORE], 2, L"Description", 380);
    mk_button(SEC_STORE, L"Install", ID_BTN_STORE_INSTALL);
    mk_button(SEC_STORE, L"Uninstall", ID_BTN_STORE_UNINSTALL);
    mk_button(SEC_STORE, L"Upgrade", ID_BTN_STORE_UPGRADE);
    mk_button(SEC_STORE, L"Check installed", ID_BTN_STORE_STATUS);

    /* Tweaks */
    create_section_header(SEC_TWEAKS, L"Tweaks",
        L"Privacy and system tweaks. Select a tweak, then apply, undo, or check its current state.");
    g.list[SEC_TWEAKS] = mk_list(SEC_TWEAKS);
    lv_add_column(g.list[SEC_TWEAKS], 0, L"Tweak", 230);
    lv_add_column(g.list[SEC_TWEAKS], 1, L"Risk", 90);
    lv_add_column(g.list[SEC_TWEAKS], 2, L"Reversible", 80);
    lv_add_column(g.list[SEC_TWEAKS], 3, L"Status", 100);
    lv_add_column(g.list[SEC_TWEAKS], 4, L"Description", 280);
    mk_button(SEC_TWEAKS, L"Apply", ID_BTN_TWEAK_APPLY);
    mk_button(SEC_TWEAKS, L"Undo", ID_BTN_TWEAK_UNDO);
    mk_button(SEC_TWEAKS, L"Check status", ID_BTN_TWEAK_DETECT);
    mk_button(SEC_TWEAKS, L"Check all", ID_BTN_TWEAK_DETECT_ALL);

    /* Features */
    create_section_header(SEC_FEATURES, L"Windows features",
        L"Enable or disable optional Windows features (WSL, Hyper-V, Sandbox…). Changes may require a reboot.");
    g.list[SEC_FEATURES] = mk_list(SEC_FEATURES);
    lv_add_column(g.list[SEC_FEATURES], 0, L"Feature", 220);
    lv_add_column(g.list[SEC_FEATURES], 1, L"Status", 100);
    lv_add_column(g.list[SEC_FEATURES], 2, L"Description", 400);
    mk_button(SEC_FEATURES, L"Enable", ID_BTN_FEATURE_ENABLE);
    mk_button(SEC_FEATURES, L"Disable", ID_BTN_FEATURE_DISABLE);
    mk_button(SEC_FEATURES, L"Check status", ID_BTN_FEATURE_DETECT);

    /* Panels */
    create_section_header(SEC_PANELS, L"Legacy panels",
        L"Shortcuts to the classic Windows control panels that newer Settings pages hide.");
    g.list[SEC_PANELS] = mk_list(SEC_PANELS);
    lv_add_column(g.list[SEC_PANELS], 0, L"Panel", 220);
    lv_add_column(g.list[SEC_PANELS], 1, L"Description", 480);
    mk_button(SEC_PANELS, L"Open", ID_BTN_PANEL_OPEN);

    /* Fixes */
    create_section_header(SEC_FIXES, L"Quick fixes",
        L"One-shot repairs: DNS flush, Windows Update reset, WinGet repair, and more.");
    g.list[SEC_FIXES] = mk_list(SEC_FIXES);
    lv_add_column(g.list[SEC_FIXES], 0, L"Fix", 230);
    lv_add_column(g.list[SEC_FIXES], 1, L"Risk", 90);
    lv_add_column(g.list[SEC_FIXES], 2, L"Reversible", 80);
    lv_add_column(g.list[SEC_FIXES], 3, L"Description", 360);
    mk_button(SEC_FIXES, L"Run fix", ID_BTN_FIX_RUN);
    mk_button(SEC_FIXES, L"Undo", ID_BTN_FIX_UNDO);

    /* Updates */
    create_section_header(SEC_UPDATES, L"Windows Update",
        L"Choose how Windows Update behaves. \"Disable all\" is dangerous and stops update services entirely.");
    g.upd_radio[0] = mk(L"BUTTON", L"Default — Windows manages updates normally", WS_GROUP | BS_AUTORADIOBUTTON, ID_RADIO_UPDATE_DEFAULT);
    g.upd_radio[1] = mk(L"BUTTON", L"Security focused — defer feature updates 365 days, quality updates 4 days", BS_AUTORADIOBUTTON, ID_RADIO_UPDATE_SECURITY);
    g.upd_radio[2] = mk(L"BUTTON", L"Disable all — stop and disable update services (dangerous)", BS_AUTORADIOBUTTON, ID_RADIO_UPDATE_DISABLE);
    SendMessageW(g.upd_radio[0], BM_SETCHECK, BST_CHECKED, 0);
    mk_button(SEC_UPDATES, L"Apply mode", ID_BTN_UPDATE_APPLY);
    mk_button(SEC_UPDATES, L"Check current mode", ID_BTN_UPDATE_DETECT);

    /* Automation */
    create_section_header(SEC_AUTOMATION, L"Run a config",
        L"Run an unattended automation config (JSON): store installs, tweaks, features, fixes, and an update mode in one pass.");
    g.auto_path = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_AUTO_PATH);
    g.auto_browse = mk(L"BUTTON", L"Browse…", BS_PUSHBUTTON, ID_BTN_AUTO_BROWSE);
    g.auto_force = mk(L"BUTTON", L"Allow dangerous operations (-ForceDangerous)", BS_AUTOCHECKBOX, ID_CHK_AUTO_FORCE);
    g.auto_ack_label = mk(L"STATIC", L"Acknowledgement token (required for dangerous configs):", 0, 0);
    g.auto_ack = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_AUTO_ACK);
    g.auto_dry = mk(L"BUTTON", L"Dry run", BS_PUSHBUTTON, ID_BTN_AUTO_DRYRUN);
    g.auto_run = mk(L"BUTTON", L"Run", BS_PUSHBUTTON, ID_BTN_AUTO_RUN);
    g.auto_note = mk(L"STATIC", L"Dry run validates the config and lists every operation without changing anything. Start there.", 0, 0);
    g.buttons[SEC_AUTOMATION][g.button_count[SEC_AUTOMATION]++] = g.auto_dry;
    g.buttons[SEC_AUTOMATION][g.button_count[SEC_AUTOMATION]++] = g.auto_run;

    /* Settings */
    create_section_header(SEC_SETTINGS, L"Settings",
        L"Safety options applied to every operation started from this window.");
    g.set_chk[0] = mk(L"BUTTON", L"Dry run only — log what would happen, change nothing", BS_AUTOCHECKBOX, ID_CHK_SET_DRYRUN);
    g.set_chk[1] = mk(L"BUTTON", L"Create a system restore point before dangerous operations", BS_AUTOCHECKBOX, ID_CHK_SET_RESTORE);
    g.set_chk[2] = mk(L"BUTTON", L"Continue even if pre-change state capture fails", BS_AUTOCHECKBOX, ID_CHK_SET_SKIPCAPTURE);
    SendMessageW(g.set_chk[1], BM_SETCHECK, BST_CHECKED, 0);
    g.set_data_label = mk(L"STATIC", L"", 0, 0);
}

static void create_log(void) {
    g.log_edit = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                                 ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                 0, 0, 10, 10, g.wnd, NULL, g.inst, NULL);
    HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(g.log_edit, WM_SETFONT, (WPARAM)mono, TRUE);
    g.log_clear = mk(L"BUTTON", L"Clear log", WS_VISIBLE | BS_PUSHBUTTON, ID_BTN_LOG_CLEAR);
}

/* ------------------------------------------------------------------ */
/* Layout & section switching                                          */
/* ------------------------------------------------------------------ */

static void show_section_controls(int sec, BOOL show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    if (g.section_title[sec]) ShowWindow(g.section_title[sec], cmd);
    if (g.section_desc[sec]) ShowWindow(g.section_desc[sec], cmd);
    if (g.list[sec]) ShowWindow(g.list[sec], cmd);
    for (int i = 0; i < g.button_count[sec]; i++) ShowWindow(g.buttons[sec][i], cmd);
    if (sec == SEC_HOME) {
        ShowWindow(g.home_title, cmd);
        ShowWindow(g.home_uptime_label, cmd);
        ShowWindow(g.home_uptime, cmd);
        for (int i = 0; i < 8; i++) ShowWindow(g.home_info[i], cmd);
        ShowWindow(g.home_counts, cmd);
    }
    if (sec == SEC_STORE) {
        ShowWindow(g.store_search, cmd);
        ShowWindow(g.store_search_label, cmd);
    }
    if (sec == SEC_UPDATES) for (int i = 0; i < 3; i++) ShowWindow(g.upd_radio[i], cmd);
    if (sec == SEC_AUTOMATION) {
        ShowWindow(g.auto_path, cmd); ShowWindow(g.auto_browse, cmd);
        ShowWindow(g.auto_force, cmd); ShowWindow(g.auto_ack_label, cmd);
        ShowWindow(g.auto_ack, cmd); ShowWindow(g.auto_note, cmd);
    }
    if (sec == SEC_SETTINGS) {
        for (int i = 0; i < 3; i++) ShowWindow(g.set_chk[i], cmd);
        ShowWindow(g.set_data_label, cmd);
    }
}

static void select_section(int sec) {
    if (sec < 0 || sec >= SEC_COUNT) return;
    show_section_controls(g.active, FALSE);
    g.active = sec;
    show_section_controls(sec, TRUE);
    for (int i = 0; i < SEC_COUNT; i++)
        SendMessageW(g.nav[i], BM_SETCHECK, i == sec ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void layout(void) {
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int W = rc.right, H = rc.bottom;
    int content_x = SIDEBAR_W + 12;
    int content_w = W - content_x - 12;
    int log_top = H - STATUS_H - LOG_H;
    int content_h = log_top - 12;

    SendMessageW(g.status_bar, WM_SIZE, 0, 0);

    /* Sidebar */
    int y = 12;
    static const int nav_order[] = {SEC_HOME, SEC_STORE, SEC_TWEAKS, SEC_FEATURES, SEC_PANELS, SEC_FIXES, SEC_UPDATES, SEC_AUTOMATION, SEC_SETTINGS};
    static const int group_before[] = {1, 1, 1, 0, 0, 1, 0, 1, 1}; /* group label precedes this nav item */
    int gi = 0;
    for (int i = 0; i < SEC_COUNT; i++) {
        if (group_before[i]) {
            int gh = (nav_order[i] == SEC_SETTINGS) ? 8 : 18;
            if (g.nav_groups[gi]) MoveWindow(g.nav_groups[gi], 16, y + 4, SIDEBAR_W - 28, gh, TRUE);
            y += gh + 6;
            gi++;
        }
        MoveWindow(g.nav[nav_order[i]], 10, y, SIDEBAR_W - 20, 30, TRUE);
        y += 34;
    }

    /* Log pane */
    MoveWindow(g.log_edit, content_x, log_top, content_w - 90, LOG_H - 8, TRUE);
    MoveWindow(g.log_clear, content_x + content_w - 84, log_top, 84, 26, TRUE);

    /* Content header */
    int cy = 12;
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (g.section_title[sec]) MoveWindow(g.section_title[sec], content_x, cy, content_w, 24, TRUE);
        if (g.section_desc[sec]) MoveWindow(g.section_desc[sec], content_x, cy + 26, content_w, 32, TRUE);
    }
    int body_y = cy + 64;
    int body_h = content_h - body_y;
    int btn_y = log_top - 38;

    /* Home */
    MoveWindow(g.home_title, content_x, cy, content_w, 24, TRUE);
    MoveWindow(g.home_uptime_label, content_x, cy + 40, 240, 20, TRUE);
    MoveWindow(g.home_uptime, content_x, cy + 60, content_w, 44, TRUE);
    for (int i = 0; i < 8; i++)
        MoveWindow(g.home_info[i], content_x, cy + 120 + i * 24, content_w, 22, TRUE);
    MoveWindow(g.home_counts, content_x, cy + 120 + 8 * 24 + 8, content_w, 22, TRUE);

    /* Store search row */
    MoveWindow(g.store_search_label, content_x, body_y, 56, 22, TRUE);
    MoveWindow(g.store_search, content_x + 60, body_y - 2, 280, 24, TRUE);

    /* Lists */
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        if (!g.list[sec]) continue;
        int ly = (sec == SEC_STORE) ? body_y + 30 : body_y;
        MoveWindow(g.list[sec], content_x, ly, content_w, btn_y - ly - 8, TRUE);
    }

    /* Buttons row per section */
    for (int sec = 0; sec < SEC_COUNT; sec++) {
        int x = content_x;
        for (int i = 0; i < g.button_count[sec]; i++) {
            int bw = 130;
            MoveWindow(g.buttons[sec][i], x, btn_y, bw, 28, TRUE);
            x += bw + 8;
        }
    }

    /* Updates radios */
    for (int i = 0; i < 3; i++)
        MoveWindow(g.upd_radio[i], content_x, body_y + i * 30, content_w, 24, TRUE);

    /* Automation */
    MoveWindow(g.auto_path, content_x, body_y, content_w - 110, 24, TRUE);
    MoveWindow(g.auto_browse, content_x + content_w - 100, body_y - 1, 100, 26, TRUE);
    MoveWindow(g.auto_force, content_x, body_y + 36, content_w, 22, TRUE);
    MoveWindow(g.auto_ack_label, content_x, body_y + 64, content_w, 20, TRUE);
    MoveWindow(g.auto_ack, content_x, body_y + 86, 360, 24, TRUE);
    MoveWindow(g.auto_note, content_x, body_y + 122, content_w, 40, TRUE);

    /* Settings */
    for (int i = 0; i < 3; i++)
        MoveWindow(g.set_chk[i], content_x, body_y + i * 30, content_w, 24, TRUE);
    MoveWindow(g.set_data_label, content_x, body_y + 3 * 30 + 12, content_w, 22, TRUE);

    (void)body_h;
}

/* ------------------------------------------------------------------ */
/* Home data                                                           */
/* ------------------------------------------------------------------ */

static void update_uptime(void) {
    ULONGLONG ms = GetTickCount64();
    ULONGLONG s = ms / 1000;
    wchar_t buf[64];
    _snwprintf(buf, 64, L"%llu:%02llu:%02llu:%02llu",
               s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    SetWindowTextW(g.home_uptime, buf);

    MEMORYSTATUSEX mem; memset(&mem, 0, sizeof mem); mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        wchar_t m[160];
        _snwprintf(m, 160, L"Memory: %.1f GB of %.1f GB in use (%lu%%)",
                   (double)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0),
                   (double)mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
                   mem.dwMemoryLoad);
        SetWindowTextW(g.home_info[3], m);
    }
}

typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static void fill_home_info(void) {
    wchar_t buf[256];

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &n)) {
        _snwprintf(buf, 256, L"Computer: %s", name);
        SetWindowTextW(g.home_info[0], buf);
    }

    RTL_OSVERSIONINFOW ver; memset(&ver, 0, sizeof ver); ver.dwOSVersionInfoSize = sizeof ver;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn rtl = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rtl && rtl(&ver) == 0) {
        const wchar_t *era = (ver.dwMajorVersion == 10 && ver.dwBuildNumber >= 22000) ? L"Windows 11" : L"Windows 10";
        _snwprintf(buf, 256, L"Windows: %s (build %lu)", era, ver.dwBuildNumber);
        SetWindowTextW(g.home_info[1], buf);
    }

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t cpu[256];
        DWORD len = sizeof cpu;
        if (RegQueryValueExW(key, L"ProcessorNameString", NULL, NULL, (LPBYTE)cpu, &len) == ERROR_SUCCESS) {
            _snwprintf(buf, 256, L"Processor: %s", cpu);
            SetWindowTextW(g.home_info[2], buf);
        }
        RegCloseKey(key);
    }

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    _snwprintf(buf, 256, L"Logical processors: %lu", si.dwNumberOfProcessors);
    SetWindowTextW(g.home_info[4], buf);

    wchar_t user[256];
    DWORD un = 256;
    if (GetUserNameW(user, &un)) {
        _snwprintf(buf, 256, L"User: %s", user);
        SetWindowTextW(g.home_info[5], buf);
    }

    _snwprintf(buf, 256, L"Catalogs: %d apps · %d tweaks · %d features · %d fixes · %d panels",
               g.apps.count, g.tweaks.count, g.features.count, g.fixes.count, g.panels.count);
    SetWindowTextW(g.home_counts, buf);
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
        j.force_dangerous = true; /* undo/detect/dry-run paths are safe to gate through */
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
    j.force_dangerous = true; /* store ops are not flagged dangerous */
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
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static void append_log(const wchar_t *line) {
    int len = GetWindowTextLengthW(g.log_edit);
    SendMessageW(g.log_edit, EM_SETSEL, len, len);
    SendMessageW(g.log_edit, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g.log_edit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g.log_edit, EM_SCROLLCARET, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                    */
/* ------------------------------------------------------------------ */

static void on_command(WORD id, WORD code) {
    if (id >= ID_NAV_FIRST && id < ID_NAV_FIRST + SEC_COUNT) {
        select_section(id - ID_NAV_FIRST);
        return;
    }
    if (id == ID_EDIT_STORE_SEARCH && code == EN_CHANGE) {
        wchar_t w[128];
        char filter[256];
        GetWindowTextW(g.store_search, w, 128);
        wide_to_utf8(w, filter, sizeof filter);
        fill_store(filter);
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
        case ID_BTN_UPDATE_APPLY:  start_update_job(false); break;
        case ID_BTN_UPDATE_DETECT: start_update_job(true); break;
        case ID_BTN_AUTO_BROWSE: browse_config(); break;
        case ID_BTN_AUTO_DRYRUN: start_automation(true); break;
        case ID_BTN_AUTO_RUN:    start_automation(false); break;
        case ID_BTN_LOG_CLEAR:   SetWindowTextW(g.log_edit, L""); break;
        default: break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            if (wp == IDT_UPTIME) update_uptime();
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
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_DESTROY:
            KillTimer(wnd, IDT_UPTIME);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(wnd, msg, wp, lp);
    }
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

static void create_fonts(void) {
    g.font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.font_bold = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.font_big = CreateFontW(-34, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g.font_group = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    g.inst = inst;

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    resolve_data_dir();
    if (!load_catalogs()) return 1;

    create_fonts();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = WND_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    g.wnd = CreateWindowExW(0, WND_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720, NULL, NULL, inst, NULL);
    if (!g.wnd) return 1;

    g.status_bar = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, g.wnd, NULL, inst, NULL);

    create_nav();
    create_home();
    create_sections();
    create_log();

    char data_note[600];
    snprintf(data_note, sizeof data_note, "Catalog data: %s (integrity verified)", g.data_dir);
    wchar_t *wnote = utf8_to_wide_dup(data_note);
    if (wnote) { SetWindowTextW(g.set_data_label, wnote); free(wnote); }

    fill_tweaks();
    fill_features();
    fill_fixes();
    fill_panels();
    fill_store(NULL);
    fill_home_info();
    update_uptime();

    g.active = SEC_HOME;
    select_section(SEC_HOME);
    SetTimer(g.wnd, IDT_UPTIME, 1000, NULL);

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

/* The GUI is Windows-only; keep non-Windows builds of this file valid. */
typedef int ph_gui_requires_windows;

#endif
