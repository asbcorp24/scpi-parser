#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Ethernet.h>
#include <Vrekrer_scpi_parser.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

SCPI_Parser scpiSerial;
SCPI_Parser scpiEth;

Adafruit_MCP23X17 mcp;
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
EthernetServer scpiServer(SCPI_TCP_PORT);
EthernetClient ethClients[MAX_ETH_CLIENTS];

SemaphoreHandle_t scpiMutex;
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t stateMutex;

bool mcpReady = false;
bool oledReady = false;
bool ethReady = false;
IPAddress currentIp(0, 0, 0, 0);
String lastError = "0,\"No error\"";
String lastSource = "boot";
uint32_t serialCount = 0;
uint32_t ethCount = 0;

byte macAddress[] = {0x02, 0xA5, 0xB0, 0xC0, 0x00, 0x01};
IPAddress fallbackIp(192, 168, 1, 77);
IPAddress fallbackDns(192, 168, 1, 1);
IPAddress fallbackGw(192, 168, 1, 1);
IPAddress fallbackMask(255, 255, 255, 0);

static String uptrim(const char *s) {
  String v = s ? String(s) : String("");
  v.trim();
  v.toUpperCase();
  return v;
}

static void setErr(const String &e) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  lastError = e;
  xSemaphoreGive(stateMutex);
}

static void replyErr(Stream &io, const String &text) {
  String e = "-100,\"" + text + "\"";
  setErr(e);
  io.println(e);
}

static String takeErr() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  String e = lastError;
  lastError = "0,\"No error\"";
  xSemaphoreGive(stateMutex);
  return e;
}

static bool parseIntStrict(const char *s, int &out) {
  if (!s) return false;
  char *endPtr = nullptr;
  long v = strtol(s, &endPtr, 10);
  while (endPtr && isspace((unsigned char)*endPtr)) endPtr++;
  if (!endPtr || *endPtr != '\0') return false;
  out = (int)v;
  return true;
}

static bool parseDigital(const char *s, int &out) {
  String v = uptrim(s);
  if (v == "1" || v == "ON" || v == "HIGH") { out = HIGH; return true; }
  if (v == "0" || v == "OFF" || v == "LOW") { out = LOW; return true; }
  return false;
}

static bool isEspPinAllowed(int pin) {
  if (pin < 0 || pin > 48) return false;
  if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return false;
  if (pin == 19 || pin == 20) return false;
  if (pin >= 26 && pin <= 37) return false;
  if (pin == I2C_SDA_PIN || pin == I2C_SCL_PIN) return false;
  if (pin == W5500_SCK_PIN || pin == W5500_MISO_PIN || pin == W5500_MOSI_PIN) return false;
  if (pin == W5500_CS_PIN || pin == W5500_RST_PIN) return false;
  return true;
}

static bool parseEspPin(const char *s, int &pin) {
  int p = -1;
  if (!parseIntStrict(s, p)) return false;
  if (!isEspPinAllowed(p)) return false;
  pin = p;
  return true;
}

static bool parseMcpPin(const char *s, int &pin) {
  int p = -1;
  if (!parseIntStrict(s, p)) return false;
  if (p < 0 || p > 15) return false;
  pin = p;
  return true;
}

static void incCounter(bool eth, const char *src) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (eth) ethCount++; else serialCount++;
  lastSource = src;
  xSemaphoreGive(stateMutex);
}

static void setEspMode(int pin, const String &mode, Stream &io) {
  if (mode == "OUT" || mode == "OUTPUT") pinMode(pin, OUTPUT);
  else if (mode == "IN" || mode == "INPUT") pinMode(pin, INPUT);
  else if (mode == "INPULLUP" || mode == "INPUT_PULLUP" || mode == "PULLUP") pinMode(pin, INPUT_PULLUP);
  else if (mode == "INPULLDOWN" || mode == "INPUT_PULLDOWN" || mode == "PULLDOWN") pinMode(pin, INPUT_PULLDOWN);
  else { replyErr(io, "unknown ESP GPIO mode"); return; }
  io.println(F("OK"));
}

static void setMcpMode(int pin, const String &mode, Stream &io) {
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  if (mode == "OUT" || mode == "OUTPUT") mcp.pinMode(pin, OUTPUT);
  else if (mode == "IN" || mode == "INPUT") mcp.pinMode(pin, INPUT);
  else if (mode == "INPULLUP" || mode == "INPUT_PULLUP" || mode == "PULLUP") mcp.pinMode(pin, INPUT_PULLUP);
  else {
    xSemaphoreGive(i2cMutex);
    replyErr(io, "unknown MCP mode");
    return;
  }
  xSemaphoreGive(i2cMutex);
  io.println(F("OK"));
}

void cmdIDN(SCPI_C, SCPI_P, Stream &io) {
  io.print(DEVICE_MANUFACTURER); io.print(',');
  io.print(DEVICE_MODEL); io.print(',');
  io.print(DEVICE_SERIAL); io.print(',');
  io.println(DEVICE_VERSION);
}

void cmdRST(SCPI_C, SCPI_P, Stream &io) {
  setErr("0,\"No error\"");
  io.println(F("OK"));
}

void cmdHelp(SCPI_C, SCPI_P, Stream &io) {
  io.println(F("*IDN?"));
  io.println(F("*RST"));
  io.println(F("HELP?"));
  io.println(F("SYST:ERR?"));
  io.println(F("SYST:STAT?"));
  io.println(F("ETH:IP?"));
  io.println(F("GPIO:MODE pin,OUT|IN|INPULLUP|INPULLDOWN"));
  io.println(F("GPIO:WRITE pin,0|1|ON|OFF|HIGH|LOW"));
  io.println(F("GPIO:READ? pin"));
  io.println(F("GPIO:TOGGLE pin"));
  io.println(F("MCP:MODE 0..15,OUT|IN|INPULLUP"));
  io.println(F("MCP:WRITE 0..15,0|1|ON|OFF|HIGH|LOW"));
  io.println(F("MCP:READ? 0..15"));
  io.println(F("MCP:TOGGLE 0..15"));
}

void cmdErr(SCPI_C, SCPI_P, Stream &io) {
  io.println(takeErr());
}

void cmdStat(SCPI_C, SCPI_P, Stream &io) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool er = ethReady, mr = mcpReady, orr = oledReady;
  IPAddress ip = currentIp;
  uint32_t sc = serialCount, ec = ethCount;
  String src = lastSource;
  xSemaphoreGive(stateMutex);
  io.print(F("ETH=")); io.print(er ? F("1") : F("0"));
  io.print(F(",IP=")); io.print(ip);
  io.print(F(",MCP=")); io.print(mr ? F("1") : F("0"));
  io.print(F(",OLED=")); io.print(orr ? F("1") : F("0"));
  io.print(F(",SERIAL_CMDS=")); io.print(sc);
  io.print(F(",ETH_CMDS=")); io.print(ec);
  io.print(F(",LAST=")); io.println(src);
}

void cmdEthIp(SCPI_C, SCPI_P, Stream &io) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  IPAddress ip = currentIp;
  xSemaphoreGive(stateMutex);
  io.println(ip);
}

void cmdGpioMode(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 2) { replyErr(io, "GPIO:MODE needs pin,mode"); return; }
  int pin;
  if (!parseEspPin(p[0], pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  setEspMode(pin, uptrim(p[1]), io);
}

void cmdGpioWrite(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 2) { replyErr(io, "GPIO:WRITE needs pin,value"); return; }
  int pin, value;
  if (!parseEspPin(p[0], pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  if (!parseDigital(p[1], value)) { replyErr(io, "bad GPIO value"); return; }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, value);
  io.println(F("OK"));
}

void cmdGpioRead(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 1) { replyErr(io, "GPIO:READ? needs pin"); return; }
  int pin;
  if (!parseEspPin(p[0], pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  io.println(digitalRead(pin) ? F("1") : F("0"));
}

void cmdGpioToggle(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 1) { replyErr(io, "GPIO:TOGGLE needs pin"); return; }
  int pin;
  if (!parseEspPin(p[0], pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, !digitalRead(pin));
  io.println(F("OK"));
}

void cmdMcpMode(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 2) { replyErr(io, "MCP:MODE needs pin,mode"); return; }
  int pin;
  if (!parseMcpPin(p[0], pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }
  setMcpMode(pin, uptrim(p[1]), io);
}

void cmdMcpWrite(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 2) { replyErr(io, "MCP:WRITE needs pin,value"); return; }
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  int pin, value;
  if (!parseMcpPin(p[0], pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }
  if (!parseDigital(p[1], value)) { replyErr(io, "bad MCP value"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  mcp.pinMode(pin, OUTPUT);
  mcp.digitalWrite(pin, value);
  xSemaphoreGive(i2cMutex);
  io.println(F("OK"));
}

void cmdMcpRead(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 1) { replyErr(io, "MCP:READ? needs pin"); return; }
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  int pin;
  if (!parseMcpPin(p[0], pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  int v = mcp.digitalRead(pin);
  xSemaphoreGive(i2cMutex);
  io.println(v ? F("1") : F("0"));
}

void cmdMcpToggle(SCPI_C, SCPI_P p, Stream &io) {
  if (p.Size() < 1) { replyErr(io, "MCP:TOGGLE needs pin"); return; }
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  int pin;
  if (!parseMcpPin(p[0], pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  mcp.pinMode(pin, OUTPUT);
  int v = mcp.digitalRead(pin);
  mcp.digitalWrite(pin, !v);
  xSemaphoreGive(i2cMutex);
  io.println(F("OK"));
}

void cmdUndefined(SCPI_C, SCPI_P, Stream &io) {
  setErr("-113,\"Undefined header\"");
  io.println(F("-113,\"Undefined header\""));
}

static void registerScpi(SCPI_Parser &p) {
  p.RegisterCommand(F("*IDN?"), &cmdIDN);
  p.RegisterCommand(F("*RST"), &cmdRST);
  p.RegisterCommand(F("HELP?"), &cmdHelp);
  p.RegisterCommand(F("SYSTem:ERRor?"), &cmdErr);
  p.RegisterCommand(F("SYSTem:STATus?"), &cmdStat);
  p.RegisterCommand(F("ETHernet:IP?"), &cmdEthIp);
  p.RegisterCommand(F("GPIO:MODe"), &cmdGpioMode);
  p.RegisterCommand(F("GPIO:WRITe"), &cmdGpioWrite);
  p.RegisterCommand(F("GPIO:READ?"), &cmdGpioRead);
  p.RegisterCommand(F("GPIO:TOGGle"), &cmdGpioToggle);
  p.RegisterCommand(F("MCP:MODe"), &cmdMcpMode);
  p.RegisterCommand(F("MCP:WRITe"), &cmdMcpWrite);
  p.RegisterCommand(F("MCP:READ?"), &cmdMcpRead);
  p.RegisterCommand(F("MCP:TOGGle"), &cmdMcpToggle);
  p.SetErrorHandler(&cmdUndefined);
  p.timeout = 10;
}

static void initI2c() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  mcpReady = mcp.begin_I2C(MCP23017_ADDR, &Wire);
  if (mcpReady) {
    for (int i = 0; i < 16; i++) mcp.pinMode(i, INPUT_PULLUP);
  }

  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledReady) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(F("SCPI GPIO boot"));
    oled.println(F("ESP32-S3 N16R2"));
    oled.display();
  }
  xSemaphoreGive(i2cMutex);
}

static void initW5500() {
  pinMode(W5500_RST_PIN, OUTPUT);
  digitalWrite(W5500_RST_PIN, LOW);
  delay(50);
  digitalWrite(W5500_RST_PIN, HIGH);
  delay(200);

  SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_PIN);
  Ethernet.init(W5500_CS_PIN);

  int dhcp = Ethernet.begin(macAddress, 5000, 1000);
#if USE_STATIC_IP_IF_DHCP_FAIL
  if (dhcp == 0) Ethernet.begin(macAddress, fallbackIp, fallbackDns, fallbackGw, fallbackMask);
#endif

  scpiServer.begin();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  currentIp = Ethernet.localIP();
  ethReady = Ethernet.hardwareStatus() != EthernetNoHardware && Ethernet.linkStatus() != LinkOFF;
  xSemaphoreGive(stateMutex);
}

void taskSerial(void *) {
  while (true) {
    if (Serial.available()) {
      incCounter(false, "USB");
      xSemaphoreTake(scpiMutex, portMAX_DELAY);
      scpiSerial.ProcessInput(Serial, "\n");
      xSemaphoreGive(scpiMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void acceptClient() {
  EthernetClient c = scpiServer.available();
  if (!c) return;
  for (int i = 0; i < MAX_ETH_CLIENTS; i++) {
    if (!ethClients[i] || !ethClients[i].connected()) {
      if (ethClients[i]) ethClients[i].stop();
      ethClients[i] = c;
      ethClients[i].println(F("ESP32-S3 SCPI TCP ready"));
      ethClients[i].println(F("Send HELP?"));
      return;
    }
  }
  c.println(F("ERR: too many clients"));
  c.stop();
}

void taskEthernet(void *) {
  while (true) {
    Ethernet.maintain();
    acceptClient();

    for (int i = 0; i < MAX_ETH_CLIENTS; i++) {
      if (ethClients[i] && ethClients[i].connected()) {
        if (ethClients[i].available()) {
          incCounter(true, "ETH");
          xSemaphoreTake(scpiMutex, portMAX_DELAY);
          scpiEth.ProcessInput(ethClients[i], "\n");
          xSemaphoreGive(scpiMutex);
        }
      } else if (ethClients[i]) {
        ethClients[i].stop();
      }
    }

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    currentIp = Ethernet.localIP();
    ethReady = Ethernet.hardwareStatus() != EthernetNoHardware && Ethernet.linkStatus() != LinkOFF;
    xSemaphoreGive(stateMutex);

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void taskOled(void *) {
  while (true) {
    if (oledReady) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      bool er = ethReady;
      bool mr = mcpReady;
      IPAddress ip = currentIp;
      uint32_t sc = serialCount;
      uint32_t ec = ethCount;
      String src = lastSource;
      xSemaphoreGive(stateMutex);

      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(0, 0);
      oled.println(F("ESP32-S3 SCPI"));
      oled.print(F("ETH: ")); oled.println(er ? F("OK") : F("NO"));
      oled.print(F("IP: ")); oled.println(ip);
      oled.print(F("MCP: ")); oled.println(mr ? F("OK") : F("NO"));
      oled.print(F("SER:")); oled.print(sc);
      oled.print(F(" ETH:")); oled.println(ec);
      oled.print(F("SRC: ")); oled.println(src);
      oled.display();
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  scpiMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();

  registerScpi(scpiSerial);
  registerScpi(scpiEth);

  initI2c();
  initW5500();

  Serial.println(F("ESP32-S3 async SCPI GPIO ready"));
  Serial.print(F("Ethernet IP: "));
  Serial.println(Ethernet.localIP());
  Serial.println(F("USB Serial and TCP/5025 enabled"));
  Serial.println(F("Send HELP?"));

  xTaskCreatePinnedToCore(taskSerial, "scpi_serial", TASK_STACK_SERIAL, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskEthernet, "scpi_eth", TASK_STACK_ETH, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(taskOled, "oled", TASK_STACK_OLED, nullptr, 1, nullptr, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
