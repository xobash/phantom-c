#ifndef PHANTOM_PACKAGE_MANAGER_H
#define PHANTOM_PACKAGE_MANAGER_H
#include "phantom/common.h"

typedef enum {
    PH_MANAGER_WINGET = 0,
    PH_MANAGER_SCOOP,
    PH_MANAGER_CHOCO,
    PH_MANAGER_PIP,
    PH_MANAGER_NPM,
    PH_MANAGER_DOTNET_TOOL,
    PH_MANAGER_POWERSHELL_GALLERY
} ph_package_manager;

typedef const char *(*ph_env_probe)(void *ctx, const char *name);
typedef bool (*ph_file_exists_probe)(void *ctx, const char *path);
typedef bool (*ph_path_resolve_probe)(void *ctx, const char *exe, char *out, size_t out_size);

typedef struct {
    ph_env_probe env;
    ph_file_exists_probe file_exists;
    ph_path_resolve_probe path_resolve;
    void *ctx;
} ph_package_manager_probes;

typedef struct {
    bool available;
    char executable_path[512];
    char source[128];
    char message[256];
} ph_package_manager_resolution;

typedef struct {
    ph_package_manager_resolution winget;
    ph_package_manager_resolution scoop;
    ph_package_manager_resolution chocolatey;
    ph_package_manager_resolution pip;
    ph_package_manager_resolution npm;
    ph_package_manager_resolution dotnet_tool;
    ph_package_manager_resolution powershell_gallery;
} ph_store_manager_availability;

bool ph_package_manager_resolve(ph_package_manager manager, const ph_package_manager_probes *probes, ph_package_manager_resolution *out);
bool ph_store_manager_availability_get(const ph_package_manager_probes *probes, ph_store_manager_availability *out);

#endif
