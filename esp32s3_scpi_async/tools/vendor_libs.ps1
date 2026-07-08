# Copies already downloaded PlatformIO libraries into the project lib/ folder.
# Run once after PlatformIO has downloaded dependencies at least one time.
#
# Usage from PowerShell:
#   cd C:\dev\scpi-parser\esp32s3_scpi_async
#   powershell -ExecutionPolicy Bypass -File .\tools\vendor_libs.ps1

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$LibDepsRoot = Join-Path $ProjectRoot ".pio\libdeps\esp32s3_n8r2"
$LibRoot = Join-Path $ProjectRoot "lib"

$RequiredLibs = @(
    "Ethernet",
    "Adafruit MCP23017 Arduino Library",
    "Adafruit SSD1306",
    "Adafruit GFX Library",
    "Adafruit BusIO",
    "Vrekrer SCPI parser"
)

Write-Host "Project: $ProjectRoot"
Write-Host "Source:  $LibDepsRoot"
Write-Host "Target:  $LibRoot"

if (!(Test-Path $LibDepsRoot)) {
    Write-Host ""
    Write-Host "ERROR: .pio/libdeps/esp32s3_n8r2 not found." -ForegroundColor Red
    Write-Host "Run once with temporary lib_deps enabled, or copy libraries manually into lib/."
    exit 1
}

if (!(Test-Path $LibRoot)) {
    New-Item -ItemType Directory -Path $LibRoot | Out-Null
}

foreach ($LibName in $RequiredLibs) {
    $Source = Join-Path $LibDepsRoot $LibName
    $Target = Join-Path $LibRoot $LibName

    if (!(Test-Path $Source)) {
        Write-Host "SKIP: $LibName was not found in .pio/libdeps" -ForegroundColor Yellow
        continue
    }

    if (Test-Path $Target) {
        Write-Host "REMOVE OLD: lib/$LibName"
        Remove-Item -Recurse -Force $Target
    }

    Write-Host "COPY: $LibName"
    Copy-Item -Recurse -Force $Source $Target
}

Write-Host ""
Write-Host "Done. Local libraries are now in esp32s3_scpi_async/lib/." -ForegroundColor Green
Write-Host "Now build without downloading dependencies:"
Write-Host "  pio run"
