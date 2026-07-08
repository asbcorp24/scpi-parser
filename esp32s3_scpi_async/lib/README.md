# Local libraries

This project is configured for offline/local-library builds.

`platformio.ini` intentionally does not use `lib_deps`, so PlatformIO will not download Arduino libraries during build.

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

If the libraries were previously downloaded by PlatformIO, run from `esp32s3_scpi_async/`:

```powershell
powershell -ExecutionPolicy Bypass -File tools/vendor_libs_from_pio.ps1
```

Then commit the copied folders:

```bash
git add lib tools/vendor_libs_from_pio.ps1 platformio.ini
git commit -m "Vendor PlatformIO libraries locally"
git push
```

## Build

```bash
pio run
```

If build says that a header is missing, copy the missing library folder into `lib/` too.
