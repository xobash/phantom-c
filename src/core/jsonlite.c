#include "phantom/jsonlite.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *ph_read_all_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

const char *ph_json_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

const char *ph_json_object_end(const char *start) {
    bool in_string = false, escape = false;
    int depth = 0;
    for (const char *p = start; p && *p; p++) {
        char c = *p;
        if (escape) { escape = false; continue; }
        if (in_string && c == '\\') { escape = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (in_string) continue;
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

/* Locate the value position for "key": within [start, end]. */
static const char *field_value(const char *start, const char *end, const char *key) {
    char needle[96];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = start;
    while (p && p < end && (p = strstr(p, needle)) && p < end) {
        const char *q = ph_json_skip_ws(p + strlen(needle));
        if (q < end && *q == ':') return ph_json_skip_ws(q + 1);
        p = q;
    }
    return NULL;
}

bool ph_json_string_field(const char *start, const char *end, const char *key, char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    const char *q = field_value(start, end, key);
    if (!q || q >= end || *q != '"') return false;
    q++;
    size_t j = 0;
    while (q < end && *q && *q != '"') {
        char c = *q++;
        if (c == '\\' && q < end && *q) {
            char e = *q++;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else c = e; /* \" \\ \/ and anything else: keep the escaped char */
        }
        if (j + 1 < out_len) out[j++] = c;
    }
    if (out && out_len) out[j < out_len ? j : out_len - 1] = '\0';
    return true;
}

bool ph_json_bool_field(const char *start, const char *end, const char *key) {
    const char *q = field_value(start, end, key);
    return q && q + 4 <= end && strncmp(q, "true", 4) == 0;
}

bool ph_json_next_object(const char **cursor, const char **start, const char **end) {
    const char *p = cursor ? *cursor : NULL;
    if (!p) return false;
    while (*p && *p != '{') p++;
    if (!*p) return false;
    const char *e = ph_json_object_end(p);
    if (!e) return false;
    *start = p;
    *end = e;
    *cursor = e + 1;
    return true;
}

bool ph_json_load_object_by_field(const char *path1, const char *path2,
                                  const char *field, const char *value,
                                  char **text_out, const char **start_out, const char **end_out,
                                  ph_error *err) {
    char *txt = ph_read_all_file(path1);
    if (!txt && path2) txt = ph_read_all_file(path2);
    if (!txt) { ph_error_set(err, 1, "missing catalog file: %s", path1 ? path1 : ""); return false; }
    const char *cursor = txt, *start, *end;
    char got[256];
    while (ph_json_next_object(&cursor, &start, &end)) {
        if (ph_json_string_field(start, end, field, got, sizeof got) && strcmp(got, value ? value : "") == 0) {
            *text_out = txt;
            *start_out = start;
            *end_out = end;
            return true;
        }
    }
    free(txt);
    ph_error_set(err, 1, "catalog entry not found: %s=%s", field, value ? value : "");
    return false;
}
