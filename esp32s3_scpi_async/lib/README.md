# Local libraries

This project is configured for offline/local-library builds.

`platformio.ini` intentionally does not use `lib_deps`, so PlatformIO will not download Arduino libraries during normal builds.

Required folders inside this `lib/` directory:

```text
lib/Ethernet
lib/Adafruit MCP23017 Arduino Library
lib/Adafruit SSD1306
lib/Adafruit GFX Library
lib/Adafruit BusIO
```

Optional, only if you return to Vrekrer SCPI parser experiments:

```text
lib/Vrekrer SCPI parser
```

## Copy from already downloaded PlatformIO libraries

If the libraries were previously downloaded by PlatformIO, run from Windows CMD:

```bat
cd C:\dev\scpi-parser\esp32s3_scpi_async
tools\vendor_libs.cmd
```

Or from PowerShell:

```powershell
cd C:\dev\scpi-parser\esp32s3_scpi_async
powershell -ExecutionPolicy Bypass -File .\tools\vendor_libs.ps1
```

The script copies libraries from:

```text
.pio/libdeps/esp32s3_n8r2/
```

to:

```text
lib/
```

Then commit the copied folders if you want the repository to be fully self-contained:

```bash
git add lib tools platformio.ini
git commit -m "Vendor PlatformIO libraries locally"
git push
```

## Build

```bash
pio run
```

If build says that a header is missing, copy the missing library folder into `lib/` too.
