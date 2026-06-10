#include "phantom/compat.h"
#include "phantom/common.h"
#include <stdio.h>
#include <string.h>

static int cmpv(ph_version a, ph_version b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.build != b.build) return a.build < b.build ? -1 : 1;
    if (a.rev != b.rev) return a.rev < b.rev ? -1 : 1;
    return 0;
}

static bool parsev(const char *s, ph_version *out) {
    int n = sscanf(s, "%d.%d.%d.%d", &out->major, &out->minor, &out->build, &out->rev);
    if (n < 2) return false;
    if (n < 3) out->build = 0;
    if (n < 4) out->rev = 0;
    return true;
}

bool ph_compatible(const char **tokens, int count, ph_version v) {
    if (count <= 0) return true;
    bool had_known = false, has_era = false, era_match = false;
    bool have_low = false, have_high = false;
    ph_version low = {0, 0, 0, 0}, high = {0, 0, 0, 0};
    for (int i = 0; i < count; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;
        if (ph_streqi(t, "win10")) {
            had_known = has_era = true;
            if ((v.major == 10 && v.build < 22000) || v.major > 10) era_match = true;
        } else if (ph_streqi(t, "win11")) {
            had_known = has_era = true;
            if ((v.major == 10 && v.build >= 22000) || v.major > 10) era_match = true;
        } else if (t[0] == '>' && t[1] == '=') {
            ph_version x = {0, 0, 0, 0};
            if (parsev(t + 2, &x)) { had_known = true; have_low = true; low = x; }
        } else if (t[0] == '<' && t[1] == '=') {
            ph_version x = {0, 0, 0, 0};
            if (parsev(t + 2, &x)) { had_known = true; have_high = true; high = x; }
        }
    }
    if (!had_known) return true;
    if (have_low && cmpv(v, low) < 0) return false;
    if (have_high && cmpv(v, high) > 0) return false;
    if (has_era && !era_match) return false;
    return true;
}
