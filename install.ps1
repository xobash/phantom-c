<#
.SYNOPSIS
    Installs Phantom (C edition) from the latest GitHub release.

.DESCRIPTION
    Downloads the latest release zip, verifies it arrived intact, extracts it
    to %LOCALAPPDATA%\Programs\PhantomC, and creates a Start Menu shortcut.

    Run:  irm https://raw.githubusercontent.com/xobash/phantom-c/main/install.ps1 | iex
#>

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = 'xobash/phantom-c'
$assetName = 'PhantomC-win-x64.zip'
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\PhantomC'

Write-Host ''
Write-Host '  Phantom (C edition) installer' -ForegroundColor Cyan
Write-Host '  -----------------------------'

# Locate the latest release asset.
Write-Host "  Fetching latest release of $repo..."
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'PhantomC-installer' }
$asset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1
if (-not $asset) {
    throw "Release $($release.tag_name) has no asset named $assetName."
}
Write-Host "  Found $($release.tag_name) ($([math]::Round($asset.size / 1MB, 1)) MB)"

# Download to a temp file.
$tmp = Join-Path $env:TEMP "PhantomC-$($release.tag_name).zip"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tmp -Headers @{ 'User-Agent' = 'PhantomC-installer' }
if ((Get-Item $tmp).Length -ne $asset.size) {
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    throw 'Downloaded file size does not match the release asset; aborting.'
}

# Extract.
Write-Host "  Installing to $installDir"
if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Expand-Archive -Path $tmp -DestinationPath $installDir -Force
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

$exe = Join-Path $installDir 'PhantomC.exe'
if (-not (Test-Path $exe)) {
    # The zip may contain a single top-level folder; flatten it.
    $inner = Get-ChildItem -Path $installDir -Directory | Select-Object -First 1
    if ($inner -and (Test-Path (Join-Path $inner.FullName 'PhantomC.exe'))) {
        Get-ChildItem -Path $inner.FullName | Move-Item -Destination $installDir -Force
        Remove-Item $inner.FullName -Recurse -Force
    }
}
if (-not (Test-Path $exe)) { throw 'PhantomC.exe was not found in the release archive.' }

# Start Menu shortcut.
$shortcutDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$shortcut = Join-Path $shortcutDir 'Phantom.lnk'
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $exe
$link.WorkingDirectory = $installDir
$link.Description = 'Phantom - Windows tweaks, apps, and fixes'
$link.Save()

Write-Host ''
Write-Host "  Installed $($release.tag_name) successfully." -ForegroundColor Green
Write-Host "  GUI:  $exe"
Write-Host "  CLI:  $(Join-Path $installDir 'phantom-cli.exe')"
Write-Host '  A Start Menu shortcut named "Phantom" was created.'
Write-Host ''

$answer = Read-Host '  Launch Phantom now? [Y/n]'
if ($answer -eq '' -or $answer -match '^[Yy]') {
    Start-Process -FilePath $exe -WorkingDirectory $installDir
}
