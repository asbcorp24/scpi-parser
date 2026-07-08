# Copies already downloaded PlatformIO libraries into the project lib/ folder.
#
# Normal use after one-time fetch:
#   cd C:\dev\scpi-parser\esp32s3_scpi_async
#   pio run -e esp32s3_n8r2_fetch_libs
#   tools\vendor_libs.cmd
#   pio run -e esp32s3_n8r2

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$LibDepsBase = Join-Path $ProjectRoot ".pio\libdeps"
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
Write-Host "Source base: $LibDepsBase"
Write-Host "Target:  $LibRoot"

if (!(Test-Path $LibDepsBase)) {
    Write-Host ""
    Write-Host "ERROR: .pio/libdeps not found." -ForegroundColor Red
    Write-Host "Run first:"
    Write-Host "  pio run -e esp32s3_n8r2_fetch_libs"
    Write-Host "Then run again:"
    Write-Host "  tools\vendor_libs.cmd"
    exit 1
}

$LibDepDirs = Get-ChildItem -Path $LibDepsBase -Directory
if (!$LibDepDirs -or $LibDepDirs.Count -eq 0) {
    Write-Host ""
    Write-Host "ERROR: no environment folders inside .pio/libdeps." -ForegroundColor Red
    Write-Host "Run first:"
    Write-Host "  pio run -e esp32s3_n8r2_fetch_libs"
    exit 1
}

if (!(Test-Path $LibRoot)) {
    New-Item -ItemType Directory -Path $LibRoot | Out-Null
}

$CopiedAny = $false

foreach ($LibName in $RequiredLibs) {
    $Source = $null

    foreach ($EnvDir in $LibDepDirs) {
        $Candidate = Join-Path $EnvDir.FullName $LibName
        if (Test-Path $Candidate) {
            $Source = $Candidate
            break
        }
    }

    if ($null -eq $Source) {
        Write-Host "SKIP: $LibName was not found in any .pio/libdeps/* folder" -ForegroundColor Yellow
        continue
    }

    $Target = Join-Path $LibRoot $LibName

    if (Test-Path $Target) {
        Write-Host "REMOVE OLD: lib/$LibName"
        Remove-Item -Recurse -Force $Target
    }

    Write-Host "COPY: $LibName"
    Write-Host "  from: $Source"
    Copy-Item -Recurse -Force $Source $Target
    $CopiedAny = $true
}

Write-Host ""
if ($CopiedAny) {
    Write-Host "Done. Local libraries are now in esp32s3_scpi_async/lib/." -ForegroundColor Green
    Write-Host "Now build without downloading dependencies:"
    Write-Host "  pio run -e esp32s3_n8r2"
} else {
    Write-Host "ERROR: no libraries were copied." -ForegroundColor Red
    Write-Host "Run first:"
    Write-Host "  pio run -e esp32s3_n8r2_fetch_libs"
    exit 1
}
