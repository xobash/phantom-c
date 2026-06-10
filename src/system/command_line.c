#include "phantom/command_line.h"
#include "phantom/common.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Quote one argument for a CreateProcess command line, following the
 * CommandLineToArgvW rules (Microsoft's ArgvQuote algorithm):
 *   - 2n backslashes before a '"' produce n backslashes, quote toggles;
 *   - 2n+1 backslashes before a '"' produce n backslashes + literal '"';
 *   - backslashes not before a '"' are literal. */
char *ph_quote_windows_arg(const char *arg) {
    if (!arg) arg = "";
    bool needs_quotes = *arg == '\0';
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return ph_strdup(arg);

    size_t cap = strlen(arg) * 2 + 3; /* worst case: every char escaped + quotes + NUL */
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (const char *p = arg;; p++) {
        size_t backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '\0') {
            /* Double trailing backslashes so the closing quote stays a quote. */
            for (size_t i = 0; i < backslashes * 2; i++) out[j++] = '\\';
            break;
        }
        if (*p == '"') {
            /* Double the run, plus one to escape the quote itself. */
            for (size_t i = 0; i < backslashes * 2 + 1; i++) out[j++] = '\\';
            out[j++] = '"';
        } else {
            for (size_t i = 0; i < backslashes; i++) out[j++] = '\\';
            out[j++] = *p;
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}
