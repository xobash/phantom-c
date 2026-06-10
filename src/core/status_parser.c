#include "phantom/status_parser.h"
#include "phantom/common.h"
#include <stdlib.h>
#include <string.h>

static char *strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        memmove(s, s + 1, n - 1);
    }
    return s;
}

static ph_operation_status token(const char *s) {
    if (!s) return PH_STATUS_UNKNOWN;
    if (ph_streqi(s, "Applied") || ph_streqi(s, "Detected") || ph_streqi(s, "Installed") ||
        ph_streqi(s, "Enabled") || ph_streqi(s, "True") || ph_streqi(s, "1"))
        return PH_STATUS_APPLIED;
    if (ph_streqi(s, "Not Applied") || ph_streqi(s, "Not Installed") || ph_streqi(s, "Disabled") ||
        ph_streqi(s, "False") || ph_streqi(s, "0") || ph_streqi(s, "Unknown") ||
        ph_streqi(s, "Error") || ph_streqi(s, "Managed / Restricted"))
        return PH_STATUS_NOT_APPLIED;
    return PH_STATUS_UNKNOWN;
}

static ph_operation_status substr(const char *s) {
    if (ph_contains_i(s, "not applied") || ph_contains_i(s, "not installed") || ph_contains_i(s, "disabled") ||
        ph_contains_i(s, "unknown") || ph_contains_i(s, "error") || ph_contains_i(s, "managed") ||
        ph_contains_i(s, "restricted"))
        return PH_STATUS_NOT_APPLIED;
    if (ph_contains_i(s, "applied") || ph_contains_i(s, "installed") || ph_contains_i(s, "enabled") ||
        ph_contains_i(s, "detected"))
        return PH_STATUS_APPLIED;
    return PH_STATUS_UNKNOWN;
}

static const char *prefixes[] = { "PHANTOM_STATUS=", "PHANTOM_STATUS:", "STATUS=", "STATUS:" };

ph_operation_status ph_parse_operation_status(const char *output) {
    if (!output || !*output) return PH_STATUS_UNKNOWN;
    char *last = NULL;
    const char *line = output;
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        if (len > 0) {
            char *t;
            {
                char *raw = (char *)malloc(len + 1);
                if (!raw) { free(last); return PH_STATUS_UNKNOWN; }
                memcpy(raw, line, len);
                raw[len] = '\0';
                t = ph_trim_dup(raw);
                free(raw);
            }
            if (!t) { free(last); return PH_STATUS_UNKNOWN; }
            if (*t) {
                for (size_t i = 0; i < PH_ARRAY_LEN(prefixes); i++) {
                    if (ph_starts_i(t, prefixes[i])) {
                        char *v = ph_trim_dup(t + strlen(prefixes[i]));
                        ph_operation_status st = PH_STATUS_UNKNOWN;
                        if (v) { st = token(strip_quotes(v)); free(v); }
                        free(t);
                        free(last);
                        return st;
                    }
                }
                free(last);
                last = t;
            } else {
                free(t);
            }
        }
        line += len;
        while (*line == '\r' || *line == '\n') line++;
    }
    ph_operation_status st = PH_STATUS_UNKNOWN;
    if (last) {
        strip_quotes(last);
        st = token(last);
        if (st == PH_STATUS_UNKNOWN) st = substr(last);
        free(last);
    }
    return st;
}

bool ph_status_is_applied(const char *output) {
    return ph_parse_operation_status(output) == PH_STATUS_APPLIED;
}

const char *ph_status_name(ph_operation_status st) {
    return st == PH_STATUS_APPLIED ? "Applied" : st == PH_STATUS_NOT_APPLIED ? "NotApplied" : "Unknown";
}
