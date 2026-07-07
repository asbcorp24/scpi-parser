# ESP32-S3 N16R2 Async SCPI GPIO

PlatformIO Arduino project for ESP32-S3 N16R2.

Features:

- SCPI via USB Serial
- SCPI via Ethernet W5500 TCP port 5025
- Native ESP32-S3 GPIO commands
- MCP23017 16-bit GPIO expander commands
- GM009605 / SSD1306 0.96 inch 128x64 I2C OLED status screen
- FreeRTOS tasks for Serial, Ethernet, and OLED

## Default wiring

Change pins in `include/config.h` if needed.

### I2C bus

| Device | ESP32-S3 |
|---|---|
| SDA | GPIO17 |
| SCL | GPIO18 |
| VCC | 3.3V |
| GND | GND |

OLED address: `0x3C`  
MCP23017 address: `0x20`

### W5500 SPI

| W5500 | ESP32-S3 |
|---|---|
| SCK | GPIO12 |
| MISO | GPIO13 |
| MOSI | GPIO11 |
| CS | GPIO10 |
| RST | GPIO9 |
| 3.3V | 3.3V |
| GND | GND |

Use 3.3V logic only.

## Build and upload

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Serial commands

```text
*IDN?
HELP?
SYST:STAT?
GPIO:MODE 5,OUT
GPIO:WRITE 5,1
GPIO:READ? 5
GPIO:TOGGLE 5
MCP:MODE 0,OUT
MCP:WRITE 0,1
MCP:READ? 0
ETH:IP?
SYST:ERR?
```

## Ethernet test

Default TCP port: `5025`.

Linux/macOS:

```bash
nc 192.168.1.77 5025
```

Windows PowerShell:

```powershell
Test-NetConnection 192.168.1.77 -Port 5025
```

For interactive TCP terminal on Windows use PuTTY in Raw mode or Hercules TCP Client.

If DHCP works, read the real IP from Serial Monitor or send `ETH:IP?` over Serial.
If DHCP fails, fallback IP is `192.168.1.77`.

## Protected ESP32-S3 GPIO

The firmware blocks these pins by default:

- GPIO0, GPIO3, GPIO45, GPIO46: strapping pins
- GPIO19, GPIO20: native USB D-/D+
- GPIO26..37: often used by Flash/PSRAM on ESP32-S3 modules
- project bus pins from `config.h`: I2C and W5500 SPI pins

MCP23017 pins are addressed as `0..15`.

## Notes

The SCPI parser is protected by a mutex because USB and Ethernet run in separate FreeRTOS tasks.
I2C devices are also protected by a mutex.
