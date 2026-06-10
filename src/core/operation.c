#include "phantom/operation.h"
#include "phantom/jsonlite.h"
#include "phantom/restore_point.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Data directory resolution                                           */
/* ------------------------------------------------------------------ */

static char g_data_dir[260] = "Data";

void ph_operation_set_data_dir(const char *dir) {
    if (dir && *dir) snprintf(g_data_dir, sizeof g_data_dir, "%s", dir);
}

static bool load_object_by_id(const char *file, const char *field, const char *value,
                              char **txt, const char **start, const char **end, ph_error *err) {
    char primary[512], fallback[512];
    snprintf(primary, sizeof primary, "%s/%s", g_data_dir, file);
    snprintf(fallback, sizeof fallback, "../Phantom/Data/%s", file);
    return ph_json_load_object_by_field(primary, fallback, field, value, txt, start, end, err);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void step(ph_step *s, const char *n, const char *script, bool net) {
    snprintf(s->name, sizeof s->name, "%s", n);
    snprintf(s->script, sizeof s->script, "%s", script);
    s->requires_network = net;
}

static void sanitize_store_id(const char *display, char *out, size_t out_len) {
    size_t j = 0;
    if (!out || out_len == 0) return;
    for (const char *p = display ? display : ""; *p && j + 1 < out_len; p++) {
        if (*p >= 'a' && *p <= 'z') out[j++] = *p;
        else if (*p >= '0' && *p <= '9') out[j++] = *p;
        else if (*p >= 'A' && *p <= 'Z') out[j++] = (char)(*p - 'A' + 'a');
    }
    out[j] = '\0';
}

static void ps_single_quote(const char *s, char *out, size_t out_len) {
    size_t j = 0;
    if (!out || out_len == 0) return;
    out[j++] = '\'';
    for (const char *p = s ? s : ""; *p && j + 2 < out_len; p++) {
        out[j++] = *p;
        if (*p == '\'' && j + 1 < out_len) out[j++] = '\'';
    }
    if (j + 1 < out_len) out[j++] = '\'';
    out[j] = '\0';
}

static ph_risk risk_from_tier(const char *tier) {
    if (ph_streqi(tier, "Dangerous")) return PH_RISK_DANGEROUS;
    if (ph_streqi(tier, "Basic")) return PH_RISK_BASIC;
    return PH_RISK_ADVANCED;
}

/* ------------------------------------------------------------------ */
/* Store operations                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char winget[128];
    char scoop[128];
    char choco[128];
    char pip[128];
    char npm[128];
    char dotnet[128];
    char psgallery[128];
    bool manual_only;
} package_sources;

static bool object_priority_contains(const char *start, const char *end, const char *manager) {
    const char *p = strstr(start, "\"packageSourcePriority\"");
    if (!p || p >= end) return false;
    const char *close = strchr(p, ']');
    if (!close || close > end) return false;
    char quoted[32];
    snprintf(quoted, sizeof quoted, "\"%s\"", manager);
    const char *hit = strstr(p, quoted);
    return hit && hit < close;
}

static void clear_unpreferred_sources(const char *start, const char *end, package_sources *sources) {
    bool prefer_winget = object_priority_contains(start, end, "winget");
    bool prefer_scoop = object_priority_contains(start, end, "scoop");
    bool prefer_choco = object_priority_contains(start, end, "choco") || object_priority_contains(start, end, "chocolatey");
    bool prefer_pip = object_priority_contains(start, end, "pip");
    bool prefer_npm = object_priority_contains(start, end, "npm");
    bool prefer_dotnet = object_priority_contains(start, end, "dotnet") || object_priority_contains(start, end, "dotnettool") || object_priority_contains(start, end, "dotnet-tool");
    bool prefer_psgallery = object_priority_contains(start, end, "psgallery") || object_priority_contains(start, end, "powershellgallery") || object_priority_contains(start, end, "powershell-gallery");
    if (prefer_winget || prefer_scoop || prefer_choco || prefer_pip || prefer_npm || prefer_dotnet || prefer_psgallery) {
        if (!prefer_winget) sources->winget[0] = '\0';
        if (!prefer_scoop) sources->scoop[0] = '\0';
        if (!prefer_choco) sources->choco[0] = '\0';
        if (!prefer_pip) sources->pip[0] = '\0';
        if (!prefer_npm) sources->npm[0] = '\0';
        if (!prefer_dotnet) sources->dotnet[0] = '\0';
        if (!prefer_psgallery) sources->psgallery[0] = '\0';
    }
}

static bool find_catalog_app(const char *display, package_sources *sources, ph_error *err) {
    char *txt = NULL;
    const char *start = NULL, *end = NULL;
    if (!load_object_by_id("catalog.apps.json", "displayName", display ? display : "", &txt, &start, &end, err)) {
        ph_error_set(err, 1, "store app not found in catalog: %s", display ? display : "");
        return false;
    }
    memset(sources, 0, sizeof *sources);
    ph_json_string_field(start, end, "wingetId", sources->winget, sizeof sources->winget);
    ph_json_string_field(start, end, "scoopId", sources->scoop, sizeof sources->scoop);
    ph_json_string_field(start, end, "chocoId", sources->choco, sizeof sources->choco);
    ph_json_string_field(start, end, "pipId", sources->pip, sizeof sources->pip);
    ph_json_string_field(start, end, "npmId", sources->npm, sizeof sources->npm);
    ph_json_string_field(start, end, "dotNetToolId", sources->dotnet, sizeof sources->dotnet);
    ph_json_string_field(start, end, "powerShellGalleryId", sources->psgallery, sizeof sources->psgallery);
    sources->manual_only = ph_json_bool_field(start, end, "manualOnly");
    clear_unpreferred_sources(start, end, sources);
    free(txt);
    return true;
}

static void build_store_script(char *script, size_t script_len, const char *manager, const char *package_id, const char *action) {
    if (ph_streqi(manager, "winget")) {
        if (ph_streqi(action, "install")) {
            snprintf(script, script_len, "winget install --id \"%s\" --exact --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity", package_id);
        } else if (ph_streqi(action, "uninstall")) {
            snprintf(script, script_len, "winget uninstall --id \"%s\" --exact --source winget --accept-source-agreements --disable-interactivity", package_id);
        } else {
            snprintf(script, script_len, "winget upgrade --id \"%s\" --exact --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity", package_id);
        }
    } else if (ph_streqi(manager, "scoop")) {
        snprintf(script, script_len, "scoop %s \"%s\"", ph_streqi(action, "upgrade") ? "update" : action, package_id);
    } else if (ph_streqi(manager, "choco")) {
        snprintf(script, script_len, "choco %s \"%s\" -y --no-progress", ph_streqi(action, "upgrade") ? "upgrade" : action, package_id);
    } else if (ph_streqi(manager, "pip")) {
        if (ph_streqi(action, "uninstall")) snprintf(script, script_len, "pip uninstall -y \"%s\"", package_id);
        else snprintf(script, script_len, "pip install --upgrade \"%s\"", package_id);
    } else if (ph_streqi(manager, "npm")) {
        snprintf(script, script_len, "npm %s -g \"%s\"", ph_streqi(action, "upgrade") ? "update" : action, package_id);
    } else if (ph_streqi(manager, "dotnet")) {
        snprintf(script, script_len, "dotnet tool %s --global \"%s\"", ph_streqi(action, "upgrade") ? "update" : action, package_id);
    } else {
        char quoted[180];
        ps_single_quote(package_id, quoted, sizeof quoted);
        if (ph_streqi(action, "install")) snprintf(script, script_len, "pwsh -NoProfile -NonInteractive -Command \"Install-Module -Name %s -Scope CurrentUser -Force -AllowClobber\"", quoted);
        else if (ph_streqi(action, "uninstall")) snprintf(script, script_len, "pwsh -NoProfile -NonInteractive -Command \"Uninstall-Module -Name %s -AllVersions -Force\"", quoted);
        else snprintf(script, script_len, "pwsh -NoProfile -NonInteractive -Command \"Update-Module -Name %s -Force\"", quoted);
    }
}

static void build_store_discovery_script(char *script, size_t script_len, const char *manager, const char *package_id) {
    if (ph_streqi(manager, "winget")) snprintf(script, script_len, "winget search --id \"%s\" --exact --source winget --disable-interactivity", package_id);
    else if (ph_streqi(manager, "scoop")) snprintf(script, script_len, "scoop search \"%s\"", package_id);
    else if (ph_streqi(manager, "choco")) snprintf(script, script_len, "choco search \"%s\" --exact --no-color", package_id);
    else if (ph_streqi(manager, "pip")) snprintf(script, script_len, "pip index versions \"%s\"", package_id);
    else if (ph_streqi(manager, "npm")) snprintf(script, script_len, "npm view \"%s\" version", package_id);
    else if (ph_streqi(manager, "dotnet")) snprintf(script, script_len, "dotnet tool search \"%s\"", package_id);
    else { char quoted[180]; ps_single_quote(package_id, quoted, sizeof quoted); snprintf(script, script_len, "pwsh -NoProfile -NonInteractive -Command \"Find-Module -Name %s\"", quoted); }
}

static void build_store_status_script(char *script, size_t script_len, const char *manager, const char *package_id) {
    if (ph_streqi(manager, "winget")) snprintf(script, script_len, "winget list --id \"%s\" --exact --source winget --disable-interactivity", package_id);
    else if (ph_streqi(manager, "scoop")) snprintf(script, script_len, "scoop list \"%s\"", package_id);
    else if (ph_streqi(manager, "choco")) snprintf(script, script_len, "choco list --local-only --exact \"%s\" --limit-output --no-color", package_id);
    else if (ph_streqi(manager, "pip")) snprintf(script, script_len, "pip show \"%s\"", package_id);
    else if (ph_streqi(manager, "npm")) snprintf(script, script_len, "npm list -g \"%s\" --depth=0", package_id);
    else if (ph_streqi(manager, "dotnet")) snprintf(script, script_len, "dotnet tool list --global");
    else { char quoted[180]; ps_single_quote(package_id, quoted, sizeof quoted); snprintf(script, script_len, "pwsh -NoProfile -NonInteractive -Command \"Get-InstalledModule -Name %s -ErrorAction Stop\"", quoted); }
}

static bool manager_available(const ph_store_manager_availability *availability, const char *manager) {
    if (!availability) return true;
    if (ph_streqi(manager, "winget")) return availability->winget.available;
    if (ph_streqi(manager, "scoop")) return availability->scoop.available;
    if (ph_streqi(manager, "choco")) return availability->chocolatey.available;
    if (ph_streqi(manager, "pip")) return availability->pip.available;
    if (ph_streqi(manager, "npm")) return availability->npm.available;
    if (ph_streqi(manager, "dotnet")) return availability->dotnet_tool.available;
    if (ph_streqi(manager, "psgallery")) return availability->powershell_gallery.available;
    return false;
}

static bool select_store_source(const package_sources *sources, const ph_store_manager_availability *availability, char *manager, size_t manager_len, char *package_id, size_t package_len) {
    const struct { const char *name; const char *id; } ordered[] = {
        {"winget", sources->winget}, {"scoop", sources->scoop}, {"choco", sources->choco},
        {"pip", sources->pip}, {"npm", sources->npm}, {"dotnet", sources->dotnet}, {"psgallery", sources->psgallery}
    };
    for (size_t i = 0; i < PH_ARRAY_LEN(ordered); i++) {
        if (ordered[i].id[0] && manager_available(availability, ordered[i].name)) {
            snprintf(manager, manager_len, "%s", ordered[i].name);
            snprintf(package_id, package_len, "%s", ordered[i].id);
            return true;
        }
    }
    return false;
}

static bool make_store_query_operation(const char *display, const char *kind, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    char id[96];
    sanitize_store_id(display, id, sizeof id);
    snprintf(op->id, sizeof op->id, "store.%s.%s", kind, id);
    snprintf(op->title, sizeof op->title, "%s %s", kind, display ? display : "");
    op->risk = PH_RISK_BASIC;
    package_sources sources;
    if (!find_catalog_app(display, &sources, err)) return false;
    if (sources.manual_only) { ph_error_set(err, 1, "Manual-only entry has no package manager discovery target: %s", display ? display : ""); return false; }
    char manager[32], package_id[128], script[512];
    if (!select_store_source(&sources, availability, manager, sizeof manager, package_id, sizeof package_id)) {
        ph_error_set(err, 1, "No configured package manager is available for this package: %s", display ? display : "");
        return false;
    }
    if (ph_streqi(kind, "discover")) build_store_discovery_script(script, sizeof script, manager, package_id);
    else build_store_status_script(script, sizeof script, manager, package_id);
    step(&op->run[op->run_count++], manager, script, true);
    return true;
}

bool ph_make_store_operation_with_availability(const char *display, const char *action, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    char id[96];
    sanitize_store_id(display, id, sizeof id);
    snprintf(op->id, sizeof op->id, "store.%s.%s", action, id);
    snprintf(op->title, sizeof op->title, "%s %s", action, display ? display : "");
    op->risk = PH_RISK_ADVANCED;

    package_sources sources;
    if (!find_catalog_app(display, &sources, err)) return false;
    if (sources.manual_only) {
        ph_error_set(err, 1, "Manual-only entry has no automatic package source: %s", display ? display : "");
        return false;
    }

    char manager[32], package_id[128], script[512];
    if (!select_store_source(&sources, availability, manager, sizeof manager, package_id, sizeof package_id)) {
        ph_error_set(err, 1, "No configured package manager is available for this package: %s", display ? display : "");
        return false;
    }
    build_store_script(script, sizeof script, manager, package_id, action);
    step(&op->run[op->run_count++], action, script, !ph_streqi(action, "uninstall"));
    return true;
}

bool ph_make_store_operation(const char *display, const char *action, ph_operation *op, ph_error *err) {
    return ph_make_store_operation_with_availability(display, action, NULL, op, err);
}

bool ph_make_store_discovery_operation(const char *display, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err) {
    return make_store_query_operation(display, "discover", availability, op, err);
}

bool ph_make_store_status_operation(const char *display, const ph_store_manager_availability *availability, ph_operation *op, ph_error *err) {
    return make_store_query_operation(display, "status", availability, op, err);
}

/* ------------------------------------------------------------------ */
/* Windows Update mode operations.                                     */
/* Scripts are ports of the C# UpdateModeOperationFactory.             */
/* ------------------------------------------------------------------ */

#define PS_REMOVE_REGISTRY_HELPERS \
    "function Remove-RegistryValue64([string]$subKey,[string]$name) {\n" \
    "  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)\n" \
    "  try {\n" \
    "    $key=$base.OpenSubKey($subKey,$true)\n" \
    "    if($null -eq $key){ return }\n" \
    "    try { if($null -ne $key.GetValue($name,$null)){ $key.DeleteValue($name,$false) } } finally { $key.Dispose() }\n" \
    "  } finally { $base.Dispose() }\n" \
    "}\n" \
    "function Remove-RegistrySubKeyIfEmpty64([string]$subKey) {\n" \
    "  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)\n" \
    "  try {\n" \
    "    $key=$base.OpenSubKey($subKey,$false)\n" \
    "    if($null -eq $key){ return }\n" \
    "    try {\n" \
    "      $valueNames=$key.GetValueNames() | Where-Object { $_ -ne '' }\n" \
    "      if($key.SubKeyCount -eq 0 -and $valueNames.Count -eq 0){ $base.DeleteSubKey($subKey,$false) }\n" \
    "    } finally { $key.Dispose() }\n" \
    "  } finally { $base.Dispose() }\n" \
    "}\n"

#define PS_SET_REGISTRY_HELPER \
    "function Set-RegistryDword64([string]$subKey,[string]$name,[int]$value) {\n" \
    "  $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)\n" \
    "  try {\n" \
    "    $key=$base.CreateSubKey($subKey)\n" \
    "    if($null -eq $key){ throw \"Unable to open HKLM:\\$subKey\" }\n" \
    "    try { $key.SetValue($name,$value,[Microsoft.Win32.RegistryValueKind]::DWord) } finally { $key.Dispose() }\n" \
    "  } finally { $base.Dispose() }\n" \
    "}\n"

static const char UPDATE_SECURITY_RUN[] =
    "$ErrorActionPreference='Stop'\n"
    "$wuSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate'\n"
    "$auSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'\n"
    PS_SET_REGISTRY_HELPER
    "Set-RegistryDword64 -subKey $wuSubKey -name 'DeferFeatureUpdatesPeriodInDays' -value 365\n"
    "Set-RegistryDword64 -subKey $wuSubKey -name 'DeferQualityUpdatesPeriodInDays' -value 4\n"
    "Set-RegistryDword64 -subKey $auSubKey -name 'NoAutoUpdate' -value 0\n";

static const char UPDATE_SECURITY_UNDO[] =
    "$ErrorActionPreference='Stop'\n"
    "$wuSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate'\n"
    "$auSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'\n"
    PS_REMOVE_REGISTRY_HELPERS
    "Remove-RegistryValue64 -subKey $auSubKey -name 'NoAutoUpdate'\n"
    "Remove-RegistryValue64 -subKey $wuSubKey -name 'DeferFeatureUpdatesPeriodInDays'\n"
    "Remove-RegistryValue64 -subKey $wuSubKey -name 'DeferQualityUpdatesPeriodInDays'\n"
    "Remove-RegistrySubKeyIfEmpty64 -subKey $auSubKey\n"
    "Remove-RegistrySubKeyIfEmpty64 -subKey $wuSubKey\n";

static const char UPDATE_DISABLE_ALL_RUN[] =
    "$ErrorActionPreference='Stop'\n"
    "$auSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'\n"
    "$stateDir=Join-Path $env:ProgramData 'Phantom\\state'\n"
    "$statePath=Join-Path $stateDir 'windows-update-service-modes.json'\n"
    PS_SET_REGISTRY_HELPER
    "function Get-ServiceStartMode([string]$serviceName) {\n"
    "  $safeName=$serviceName.Replace(\"'\",\"''\")\n"
    "  return (Get-CimInstance Win32_Service -Filter \"Name='$safeName'\" -ErrorAction Stop).StartMode\n"
    "}\n"
    "New-Item -Path $stateDir -ItemType Directory -Force -ErrorAction Stop | Out-Null\n"
    "@{ WuauservStartMode = Get-ServiceStartMode 'wuauserv'; BitsStartMode = Get-ServiceStartMode 'bits' } | ConvertTo-Json -Compress | Set-Content -Path $statePath -Encoding UTF8 -Force -ErrorAction Stop\n"
    "Set-RegistryDword64 -subKey $auSubKey -name 'NoAutoUpdate' -value 1\n"
    "Stop-Service -Name wuauserv -Force -ErrorAction Stop\n"
    "Stop-Service -Name bits -Force -ErrorAction Stop\n"
    "Set-Service -Name wuauserv -StartupType Disabled -ErrorAction Stop\n"
    "Set-Service -Name bits -StartupType Disabled -ErrorAction Stop\n";

static const char UPDATE_DEFAULT_RESTORE[] =
    "$ErrorActionPreference='Stop'\n"
    "$wuSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate'\n"
    "$auSubKey='SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'\n"
    "$statePath=Join-Path (Join-Path $env:ProgramData 'Phantom\\state') 'windows-update-service-modes.json'\n"
    PS_REMOVE_REGISTRY_HELPERS
    "function Resolve-ServiceStartupType([string]$mode) {\n"
    "  switch ($mode.ToLowerInvariant()) {\n"
    "    'auto' { return 'Automatic' }\n"
    "    'automatic' { return 'Automatic' }\n"
    "    'manual' { return 'Manual' }\n"
    "    'disabled' { return 'Disabled' }\n"
    "    default { Write-Warning \"Unknown service mode '$mode', defaulting to Manual\"; return 'Manual' }\n"
    "  }\n"
    "}\n"
    "$wuMode='Manual'\n"
    "$bitsMode='Manual'\n"
    "if(Test-Path $statePath){\n"
    "  try {\n"
    "    $state=Get-Content -Path $statePath -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop\n"
    "    if($null -ne $state -and -not [string]::IsNullOrWhiteSpace($state.WuauservStartMode)){ $wuMode=[string]$state.WuauservStartMode }\n"
    "    if($null -ne $state -and -not [string]::IsNullOrWhiteSpace($state.BitsStartMode)){ $bitsMode=[string]$state.BitsStartMode }\n"
    "  } catch { }\n"
    "}\n"
    "$wuStartup=Resolve-ServiceStartupType $wuMode\n"
    "$bitsStartup=Resolve-ServiceStartupType $bitsMode\n"
    "Set-Service -Name wuauserv -StartupType $wuStartup -ErrorAction Stop\n"
    "Set-Service -Name bits -StartupType $bitsStartup -ErrorAction Stop\n"
    "if($wuStartup -ne 'Disabled'){ Start-Service -Name wuauserv -ErrorAction Stop }\n"
    "if($bitsStartup -ne 'Disabled'){ Start-Service -Name bits -ErrorAction Stop }\n"
    "Remove-RegistryValue64 -subKey $auSubKey -name 'NoAutoUpdate'\n"
    "Remove-RegistryValue64 -subKey $wuSubKey -name 'DeferFeatureUpdatesPeriodInDays'\n"
    "Remove-RegistryValue64 -subKey $wuSubKey -name 'DeferQualityUpdatesPeriodInDays'\n"
    "Remove-RegistrySubKeyIfEmpty64 -subKey $auSubKey\n"
    "Remove-RegistrySubKeyIfEmpty64 -subKey $wuSubKey\n"
    "if(Test-Path $statePath){ Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $statePath }\n";

static const char UPDATE_DEFAULT_DETECT[] =
    "$au='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'; $noAuto=$null; if(Test-Path $au){ try { $noAuto=(Get-ItemProperty -Path $au -Name NoAutoUpdate -ErrorAction Stop).NoAutoUpdate } catch { $noAuto=$null } }; $wu=(Get-Service wuauserv -ErrorAction Stop).StartType; $bits=(Get-Service bits -ErrorAction Stop).StartType; if(($noAuto -ne 1) -and $wu -ne 'Disabled' -and $bits -ne 'Disabled'){'PHANTOM_STATUS=Applied'} else {'PHANTOM_STATUS=NotApplied'}";

static const char UPDATE_SECURITY_DETECT[] =
    "$wu='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate'; $au='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'; if((Test-Path $wu) -and (Test-Path $au)){ $p=Get-ItemProperty -Path $wu -ErrorAction Stop; $a=Get-ItemProperty -Path $au -ErrorAction Stop; if($p.DeferFeatureUpdatesPeriodInDays -eq 365 -and $p.DeferQualityUpdatesPeriodInDays -eq 4 -and $a.NoAutoUpdate -eq 0){'PHANTOM_STATUS=Applied'} else {'PHANTOM_STATUS=NotApplied'} } else {'PHANTOM_STATUS=NotApplied'}";

static const char UPDATE_DISABLE_ALL_DETECT[] =
    "$au='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU'; $noAuto=$null; if(Test-Path $au){ try { $noAuto=(Get-ItemProperty -Path $au -Name NoAutoUpdate -ErrorAction Stop).NoAutoUpdate } catch { $noAuto=$null } }; $wu=(Get-Service wuauserv -ErrorAction Stop).StartType; $bits=(Get-Service bits -ErrorAction Stop).StartType; if($noAuto -eq 1 -and $wu -eq 'Disabled' -and $bits -eq 'Disabled'){'PHANTOM_STATUS=Applied'} else {'PHANTOM_STATUS=NotApplied'}";

bool ph_make_update_operation(const char *mode, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    if (ph_streqi(mode, "Default")) {
        snprintf(op->id, sizeof op->id, "updates.mode.default");
        snprintf(op->title, sizeof op->title, "Windows Update: default policy");
        op->risk = PH_RISK_BASIC;
        op->reversible = true;
        step(&op->run[op->run_count++], "restore-default", UPDATE_DEFAULT_RESTORE, false);
        step(&op->detect, "detect", UPDATE_DEFAULT_DETECT, false);
        step(&op->undo[op->undo_count++], "undo-to-security", UPDATE_SECURITY_RUN, false);
        return true;
    }
    if (ph_streqi(mode, "Security")) {
        snprintf(op->id, sizeof op->id, "updates.mode.security");
        snprintf(op->title, sizeof op->title, "Windows Update: security-only deferral");
        op->risk = PH_RISK_BASIC;
        op->reversible = true;
        step(&op->run[op->run_count++], "apply-security", UPDATE_SECURITY_RUN, false);
        step(&op->detect, "detect", UPDATE_SECURITY_DETECT, false);
        step(&op->undo[op->undo_count++], "undo-default", UPDATE_SECURITY_UNDO, false);
        return true;
    }
    if (ph_streqi(mode, "Disable All") || ph_streqi(mode, "DisableAll")) {
        snprintf(op->id, sizeof op->id, "updates.mode.disableall");
        snprintf(op->title, sizeof op->title, "Windows Update: disable all updates");
        op->risk = PH_RISK_DANGEROUS;
        op->reversible = true;
        op->destructive = true;
        step(&op->run[op->run_count++], "disable-updates", UPDATE_DISABLE_ALL_RUN, false);
        step(&op->detect, "detect", UPDATE_DISABLE_ALL_DETECT, false);
        step(&op->undo[op->undo_count++], "undo-default", UPDATE_DEFAULT_RESTORE, false);
        return true;
    }
    ph_error_set(err, 1, "unknown update mode");
    return false;
}

/* ------------------------------------------------------------------ */
/* Catalog-backed operations: tweaks, features, fixes, panels          */
/* ------------------------------------------------------------------ */

bool ph_make_tweak_operation(const char *id, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    snprintf(op->id, sizeof op->id, "tweak.%s", id ? id : "");
    char *txt = NULL;
    const char *start = NULL, *end = NULL;
    if (!load_object_by_id("tweaks.json", "id", id ? id : "", &txt, &start, &end, err)) return false;
    char title[256] = {0}, risk[64] = {0}, detect[4096] = {0}, apply[4096] = {0}, undo[4096] = {0};
    ph_json_string_field(start, end, "name", title, sizeof title);
    ph_json_string_field(start, end, "riskTier", risk, sizeof risk);
    ph_json_string_field(start, end, "detectScript", detect, sizeof detect);
    ph_json_string_field(start, end, "applyScript", apply, sizeof apply);
    ph_json_string_field(start, end, "undoScript", undo, sizeof undo);
    snprintf(op->title, sizeof op->title, "%s", title[0] ? title : "Tweak");
    op->risk = risk_from_tier(risk);
    op->reversible = ph_json_bool_field(start, end, "reversible");
    op->destructive = ph_json_bool_field(start, end, "destructive") || op->risk == PH_RISK_DANGEROUS;
    if (detect[0]) {
        step(&op->detect, "detect", detect, false);
        /* Capture pre-apply state by recording the detect output. */
        step(&op->capture[op->capture_count++], "capture", detect, false);
    }
    if (apply[0]) step(&op->run[op->run_count++], "apply", apply, false);
    if (undo[0]) step(&op->undo[op->undo_count++], "undo", undo, false);
    free(txt);
    return true;
}

bool ph_make_feature_operation(const char *id, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    char *txt = NULL;
    const char *start = NULL, *end = NULL;
    if (!load_object_by_id("features.json", "id", id ? id : "", &txt, &start, &end, err)) return false;
    char name[256] = {0}, feature[256] = {0};
    ph_json_string_field(start, end, "name", name, sizeof name);
    ph_json_string_field(start, end, "featureName", feature, sizeof feature);
    snprintf(op->id, sizeof op->id, "feature.%s", id ? id : "");
    snprintf(op->title, sizeof op->title, "%s", name[0] ? name : "Feature");
    op->risk = PH_RISK_ADVANCED;
    op->reversible = true;
    const char *fname = feature[0] ? feature : (id ? id : "");
    char script[1024];
    snprintf(script, sizeof script, "Enable-WindowsOptionalFeature -Online -FeatureName '%s' -All -NoRestart", fname);
    step(&op->run[op->run_count++], "enable", script, false);
    snprintf(script, sizeof script, "Disable-WindowsOptionalFeature -Online -FeatureName '%s' -NoRestart", fname);
    step(&op->undo[op->undo_count++], "disable", script, false);
    snprintf(script, sizeof script, "if((Get-WindowsOptionalFeature -Online -FeatureName '%s').State -eq 'Enabled'){'Applied'} else {'Not Applied'}", fname);
    step(&op->detect, "detect", script, false);
    free(txt);
    return true;
}

bool ph_make_fix_operation(const char *id, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    char *txt = NULL;
    const char *start = NULL, *end = NULL;
    if (!load_object_by_id("fixes.json", "id", id ? id : "", &txt, &start, &end, err)) return false;
    char name[256] = {0}, risk[64] = {0}, apply[4096] = {0}, undo[4096] = {0};
    ph_json_string_field(start, end, "name", name, sizeof name);
    ph_json_string_field(start, end, "riskTier", risk, sizeof risk);
    ph_json_string_field(start, end, "applyScript", apply, sizeof apply);
    ph_json_string_field(start, end, "undoScript", undo, sizeof undo);
    snprintf(op->id, sizeof op->id, "fix.%s", id ? id : "");
    snprintf(op->title, sizeof op->title, "%s", name[0] ? name : "Fix");
    op->risk = risk_from_tier(risk);
    op->reversible = ph_json_bool_field(start, end, "reversible");
    op->destructive = ph_json_bool_field(start, end, "destructive") || op->risk == PH_RISK_DANGEROUS;
    step(&op->run[op->run_count++], "apply", apply, false);
    if (undo[0]) step(&op->undo[op->undo_count++], "undo", undo, false);
    free(txt);
    return true;
}

bool ph_make_panel_operation(const char *id, ph_operation *op, ph_error *err) {
    memset(op, 0, sizeof *op);
    char *txt = NULL;
    const char *start = NULL, *end = NULL;
    if (!load_object_by_id("legacy-panels.json", "id", id ? id : "", &txt, &start, &end, err)) return false;
    char name[256] = {0}, launch[2048] = {0};
    ph_json_string_field(start, end, "name", name, sizeof name);
    ph_json_string_field(start, end, "launchScript", launch, sizeof launch);
    snprintf(op->id, sizeof op->id, "panel.%s", id ? id : "");
    snprintf(op->title, sizeof op->title, "%s", name[0] ? name : "Panel");
    op->risk = PH_RISK_BASIC;
    step(&op->run[op->run_count++], "launch", launch, false);
    free(txt);
    return true;
}

/* ------------------------------------------------------------------ */
/* Operation engine                                                    */
/* ------------------------------------------------------------------ */

bool ph_operation_engine_run(ph_operation *ops, int count, ph_operation_request req, ph_runner_fn runner, void *ctx, ph_operation_result *res, int *rc, ph_error *err) {
    int outc = 0;
    for (int i = 0; i < count; i++) {
        ph_operation *op = &ops[i];
        ph_operation_result *r = &res[outc++];
        memset(r, 0, sizeof *r);
        snprintf(r->operation_id, sizeof r->operation_id, "%s", op->id);
        bool gated = op->risk == PH_RISK_DANGEROUS || op->destructive;
        if (gated && !req.enable_destructive) {
            snprintf(r->message, sizeof r->message, "dangerous operations disabled");
            *rc = outc;
            return false;
        }
        if (gated && (!req.force_dangerous || !req.confirm_dangerous)) {
            r->cancelled = true;
            snprintf(r->message, sizeof r->message, "dangerous operation rejected");
            *rc = outc;
            return false;
        }
        if (gated && req.create_restore_point) {
            ph_operation rp = {0};
            snprintf(rp.id, sizeof rp.id, "safety.restore-point");
            char desc[PH_RESTORE_POINT_DESCRIPTION_MAX];
            if (!ph_restore_point_description(op->id, desc, sizeof desc, err)) {
                snprintf(r->message, sizeof r->message, "restore point description failed");
                *rc = outc;
                return false;
            }
            ph_step st;
            step(&st, op->id, desc, false);
            char o[256];
            if (!runner(&rp, &st, req.dry_run, o, sizeof o, ctx)) {
                snprintf(r->message, sizeof r->message, "restore point creation failed: %s", o);
                *rc = outc;
                return false;
            }
        }
        if (req.dry_run) {
            r->success = true;
            snprintf(r->message, sizeof r->message, "Dry-run");
            continue;
        }
        char output[1024];
        for (int c = 0; c < op->capture_count; c++) {
            if (!runner(op, &op->capture[c], false, output, sizeof output, ctx)) {
                r->capture_failed = true;
                if (!req.skip_capture_check) {
                    snprintf(r->message, sizeof r->message, "state capture failed");
                    *rc = outc;
                    return false;
                }
            }
        }
        bool ok = true;
        for (int s = 0; s < op->run_count; s++) {
            if (!runner(op, &op->run[s], false, output, sizeof output, ctx)) { ok = false; break; }
        }
        if (!ok) {
            snprintf(r->message, sizeof r->message, "operation failed");
            for (int j = i - 1; j >= 0; j--) {
                ph_operation_result *rr = &res[outc++];
                memset(rr, 0, sizeof *rr);
                snprintf(rr->operation_id, sizeof rr->operation_id, "%.118s.rollback", ops[j].id);
                for (int u = 0; u < ops[j].undo_count; u++) runner(&ops[j], &ops[j].undo[u], false, output, sizeof output, ctx);
                rr->success = true;
            }
            *rc = outc;
            return false;
        }
        if (op->detect.script[0]) {
            r->verification_attempted = true;
            runner(op, &op->detect, false, output, sizeof output, ctx);
            r->verification_passed = ph_status_is_applied(output);
        }
        r->success = true;
        snprintf(r->message, sizeof r->message, "success");
    }
    *rc = outc;
    (void)err;
    return true;
}
