# Phantom

[![CI](https://github.com/xobash/phantom-c/actions/workflows/ci.yml/badge.svg)](https://github.com/xobash/phantom-c/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/xobash/phantom-c)](https://github.com/xobash/phantom-c/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A fast, native Windows utility for privacy tweaks, app installs, quick fixes,
optional features, legacy control panels, Windows Update policy, and unattended
automation — written in C11 with zero runtime dependencies. No .NET, no
bundled PowerShell modules, one small exe.

## Install

Paste into PowerShell:

```powershell
irm https://raw.githubusercontent.com/xobash/phantom-c/main/install.ps1 | iex
```

This installs the latest release to `%LOCALAPPDATA%\Programs\PhantomC` and adds
a Start Menu shortcut. Prefer manual? Grab `PhantomC-win-x64.zip` from the
[latest release](https://github.com/xobash/phantom-c/releases/latest), extract,
and run `PhantomC.exe`.

## What's inside

| Section | What it does |
| --- | --- |
| **Home** | Live system overview — uptime stopwatch, hardware, memory, catalog summary |
| **App store** | Install / uninstall / upgrade 375 curated apps via winget, scoop, choco, pip, npm, dotnet, or PowerShell Gallery |
| **Tweaks** | 19 privacy and system tweaks with apply / undo / status detection |
| **Windows features** | Toggle WSL, Hyper-V, Sandbox, and other optional features |
| **Legacy panels** | One-click shortcuts to the classic control panels Settings hides |
| **Quick fixes** | DNS flush, Windows Update reset, WinGet repair, and more |
| **Windows Update** | Default / security-focused / fully disabled update policy |
| **Automation** | Run a JSON config unattended — installs, tweaks, fixes, and update mode in one pass |

A console CLI (`phantom-cli.exe`) ships alongside the GUI for scripted and
CI use:

```powershell
phantom-cli --list
phantom-cli --validate-catalogs
phantom-cli -Config my-setup.json -Run --dry-run
phantom-cli -Config my-setup.json -Run -ForceDangerous -AcknowledgeDangerous I_UNDERSTAND_NO_ROLLBACK
```

## Safety model

Every operation — GUI or CLI — passes the same perimeter before anything runs:

- **Script validation.** PowerShell steps are rejected if they contain dynamic
  execution (`iex`, encoded commands, reflection loads) or download from
  untrusted hosts.
- **Stdin delivery.** Scripts reach PowerShell via stdin, so script content can
  never break out of the host invocation.
- **Dangerous-operation gates.** Destructive work requires explicit
  confirmation and can create a native System Restore point first
  (`SRSetRestorePointW` — no `Checkpoint-Computer` shell-out).
- **Catalog integrity.** All JSON catalogs are SHA-256 verified at startup with
  a built-in implementation.
- **Rollback.** Failed batch runs undo previously applied operations in
  reverse order.

## Build from source

Portable core and test suite run anywhere:

```sh
make test
```

Windows binaries cross-compile from macOS or Linux:

```sh
pip install ziglang
make release WINCC="python3 -m ziglang cc -target x86_64-windows-gnu"
# or, with mingw-w64 installed:
make release
```

`make release` produces a ready-to-zip `build/release/` (GUI + CLI + catalogs +
sample config). On Windows itself, compile the same sources with MSVC or
clang-cl and link against `comctl32 gdi32 user32 shell32 advapi32 comdlg32
srclient`.

## Project layout

```
Data/               integrity-checked JSON catalogs (apps, tweaks, features, fixes, panels)
include/phantom/    public headers
src/core/           operation factories + engine, JSON + SHA-256 helpers
src/ps/             PowerShell runner, safety validator, input sanitizer
src/system/         process runner, package-manager resolver, restore points
src/app/            CLI parsing and automation config
gui/                Win32 GUI (single translation unit)
tests/              behavioral test suite (runs on every push)
```

New catalog entries only require editing the JSON files and refreshing the
hashes in `src/catalog/catalog.c`. New Windows integrations belong in
`src/system/` — never bypass `operation_engine` or `ps_validator`.

## License

[GPL-3.0](LICENSE)
