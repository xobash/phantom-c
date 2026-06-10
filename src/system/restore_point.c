#define _POSIX_C_SOURCE 200809L
#include "phantom/restore_point.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <srrestoreptapi.h>
#endif

static bool is_token_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '-' || ch == '_';
}

bool ph_restore_point_description(const char *operation_id, char *out, size_t out_len, ph_error *err) {
    if (!out || out_len == 0) {
        ph_error_set(err, 1, "missing restore point description buffer");
        return false;
    }
    out[0] = '\0';
    char token[128];
    size_t j = 0;
    for (const char *p = operation_id ? operation_id : ""; *p && j + 1 < sizeof token; p++) {
        if (is_token_char(*p)) token[j++] = *p;
    }
    if (j == 0) {
        const char *fallback = "operation";
        while (*fallback && j + 1 < sizeof token) token[j++] = *fallback++;
    }
    token[j] = '\0';

    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv) == 0) snprintf(stamp, sizeof stamp, "unknown-time");
    snprintf(out, out_len, "Phantom %s %s", stamp, token);
    out[out_len - 1] = '\0';
    if (strlen(out) > 220) out[220] = '\0';
    return true;
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *text) {
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) len = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)len, sizeof *wide);
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len) <= 0 &&
        MultiByteToWideChar(CP_ACP, 0, text, -1, wide, len) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

bool ph_restore_point_create(const char *description, ph_error *err) {
    if (!description || !*description) {
        ph_error_set(err, 1, "missing restore point description");
        return false;
    }
#ifdef _WIN32
    wchar_t *wide_description = utf8_to_wide(description);
    if (!wide_description) {
        ph_error_set(err, 1, "failed to encode restore point description");
        return false;
    }

    RESTOREPOINTINFOW restore_point;
    STATEMGRSTATUS status;
    memset(&restore_point, 0, sizeof restore_point);
    memset(&status, 0, sizeof status);
    restore_point.dwEventType = BEGIN_SYSTEM_CHANGE;
    restore_point.dwRestorePtType = MODIFY_SETTINGS;
    restore_point.llSequenceNumber = 0;
    wcsncpy(restore_point.szDescription, wide_description, MAX_DESC_W - 1);
    restore_point.szDescription[MAX_DESC_W - 1] = L'\0';
    free(wide_description);

    if (!SRSetRestorePointW(&restore_point, &status)) {
        ph_error_set(err, (int)status.nStatus, "SRSetRestorePointW begin failed: status=%lu", (unsigned long)status.nStatus);
        return false;
    }

    restore_point.dwEventType = END_SYSTEM_CHANGE;
    restore_point.llSequenceNumber = status.llSequenceNumber;
    if (!SRSetRestorePointW(&restore_point, &status)) {
        ph_error_set(err, (int)status.nStatus, "SRSetRestorePointW end failed: status=%lu", (unsigned long)status.nStatus);
        return false;
    }
    return true;
#else
    (void)description;
    ph_error_set(err, 1, "restore point creation requires Windows System Restore");
    return false;
#endif
}
