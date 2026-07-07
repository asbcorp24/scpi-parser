#pragma once

// ================= USER SETTINGS =================

// I2C bus for MCP23017 and OLED GM009605/SSD1306
#define I2C_SDA_PIN 17
#define I2C_SCL_PIN 18
#define I2C_FREQ_HZ 400000

// MCP23017 I2C address. Usually 0x20 when A0/A1/A2 are GND.
#define MCP23017_ADDR 0x20

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
#define MAX_ETH_CLIENTS 4

// Network fallback if DHCP fails
#define USE_STATIC_IP_IF_DHCP_FAIL 1

// Device identity
#define DEVICE_MANUFACTURER "ASBCORP"
#define DEVICE_MODEL "ESP32S3-SCPI-ASYNC-GPIO"
#define DEVICE_SERIAL "N8R2"
#define DEVICE_VERSION "0.2"

// Queue settings
#define SCPI_QUEUE_LENGTH 16
#define SCPI_LINE_LENGTH 160

// Task stack sizes
#define TASK_STACK_SERIAL 4096
#define TASK_STACK_ETH 8192
#define TASK_STACK_OLED 4096
