#pragma once

// ================= USER SETTINGS =================

// I2C bus for MCP23017 and OLED GM009605/SSD1306
#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18
#define I2C_FREQ_HZ 400000

// Four MCP23017 expanders on one I2C bus.
// Address is selected by A0/A1/A2 pins:
// MCP0: 0x20 = A2=GND A1=GND A0=GND, global pins 0..15
// MCP1: 0x21 = A2=GND A1=GND A0=VCC, global pins 16..31
// MCP2: 0x22 = A2=GND A1=VCC A0=GND, global pins 32..47
// MCP3: 0x23 = A2=GND A1=VCC A0=VCC, global pins 48..63
#define MCP23017_COUNT 4
#define MCP23017_ADDR0 0x20
#define MCP23017_ADDR1 0x21
#define MCP23017_ADDR2 0x22
#define MCP23017_ADDR3 0x23
#define MCP_TOTAL_PINS (MCP23017_COUNT * 16)

// OLED GM009605 / SSD1306 settings
#define OLED_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_RESET_PIN -1

// W5500 SPI pins. Change to your wiring.
#define W5500_SCK_PIN 12
#define W5500_MISO_PIN 13
#define W5500_MOSI_PIN 11
#define W5500_CS_PIN 10
#define W5500_RST_PIN 9

// SCPI over Ethernet
#define SCPI_TCP_PORT 5025

// For long SCPI commands keep only one TCP client by default.
// Every client gets its own PSRAM RX line buffer.
#define MAX_ETH_CLIENTS 1

// Network fallback if DHCP fails
#define USE_STATIC_IP_IF_DHCP_FAIL 1

// Device identity
#define DEVICE_MANUFACTURER "ASBCORP"
#define DEVICE_MODEL "ESP32S3-SCPI-ASYNC-GPIO"
#define DEVICE_SERIAL "N8R2"
#define DEVICE_VERSION "0.4-4mcp-console"

// Long SCPI command line length.
// Allocated from PSRAM with heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT).
// Increase to 8192 if ESP.getFreePsram() is OK.
#define SCPI_LINE_LENGTH 4096

// Task stack sizes. Command text itself is not stored on task stack.
#define TASK_STACK_SERIAL 3072
#define TASK_STACK_ETH 6144
#define TASK_STACK_OLED 3072
