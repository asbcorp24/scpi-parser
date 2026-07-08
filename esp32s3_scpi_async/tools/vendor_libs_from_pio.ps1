# Copies already downloaded PlatformIO libraries from .pio/libdeps into project-local lib/.
# Run from esp32s3_scpi_async folder:
#   powershell -ExecutionPolicy Bypass -File tools/vendor_libs_from_pio.ps1

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$EnvName = "esp32s3_n8r2"
$SrcRoot = Join-Path $ProjectRoot ".pio\libdeps\$EnvName"
$DstRoot = Join-Path $ProjectRoot "lib"

$Libraries = @(
    "Ethernet",
    "Adafruit MCP23017 Arduino Library",
    "Adafruit SSD1306",
    "Adafruit GFX Library",
    "Adafruit BusIO",
    "Vrekrer SCPI parser"
)

Write-Host "Project: $ProjectRoot"
Write-Host "Source : $SrcRoot"
Write-Host "Target : $DstRoot"

if (!(Test-Path $SrcRoot)) {
    Write-Host ""
    Write-Host "ERROR: Source folder not found: $SrcRoot" -ForegroundColor Red
    Write-Host "First run once with old lib_deps or copy libraries manually into lib/." -ForegroundColor Yellow
    exit 1
}

if (!(Test-Path $DstRoot)) {
    New-Item -ItemType Directory -Path $DstRoot | Out-Null
}

foreach ($lib in $Libraries) {
    $src = Join-Path $SrcRoot $lib
    $dst = Join-Path $DstRoot $lib

    if (Test-Path $src) {
        if (Test-Path $dst) {
            Write-Host "Remove old: $dst"
            Remove-Item -Recurse -Force $dst
        }

        Write-Host "Copy: $lib"
        Copy-Item -Recurse -Force $src $dst
    } else {
        Write-Host "Skip, not found: $lib" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Done. Now local libraries are in lib/." -ForegroundColor Green
Write-Host "Commit lib/ folders to Git if you need full offline builds on another PC." -ForegroundColor Green
