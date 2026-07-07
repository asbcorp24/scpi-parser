#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Ethernet.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_heap_caps.h"
#include "config.h"

#define MAX_SCPI_PARAMS 24
#define OLED_CONSOLE_LINES 7
#define OLED_CONSOLE_COLS 22

class EthernetServerCompat : public EthernetServer {
public:
  explicit EthernetServerCompat(uint16_t port) : EthernetServer(port) {}
  void begin(uint16_t port = 0) override {
    (void)port;
    EthernetServer::begin();
  }
};

struct LineReceiver {
  char *buf = nullptr;
  size_t cap = 0;
  size_t len = 0;
  bool overflow = false;
};

struct ParamList {
  char *v[MAX_SCPI_PARAMS];
  int count;
};

Adafruit_MCP23X17 mcp;
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
EthernetServerCompat scpiServer(SCPI_TCP_PORT);
EthernetClient ethClients[MAX_ETH_CLIENTS];

LineReceiver serialRx;
LineReceiver ethRx[MAX_ETH_CLIENTS];

SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t stateMutex;

bool mcpReady = false;
bool oledReady = false;
bool ethReady = false;
bool psramReady = false;
IPAddress currentIp(0, 0, 0, 0);
char lastError[96] = "0,\"No error\"";
char lastSource[8] = "boot";
char oledConsole[OLED_CONSOLE_LINES][OLED_CONSOLE_COLS];
uint32_t serialCount = 0;
uint32_t ethCount = 0;
uint32_t overflowCount = 0;

byte macAddress[] = {0x02, 0xA5, 0xB0, 0xC0, 0x00, 0x01};
IPAddress fallbackIp(192, 168, 1, 77);
IPAddress fallbackDns(192, 168, 1, 1);
IPAddress fallbackGw(192, 168, 1, 1);
IPAddress fallbackMask(255, 255, 255, 0);

static void safeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = 0;
}

static void oledConsoleClear() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < OLED_CONSOLE_LINES; i++) {
    oledConsole[i][0] = 0;
  }
  xSemaphoreGive(stateMutex);
}

static void oledConsolePushRaw(const char *text) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < OLED_CONSOLE_LINES - 1; i++) {
    safeCopy(oledConsole[i], sizeof(oledConsole[i]), oledConsole[i + 1]);
  }
  safeCopy(oledConsole[OLED_CONSOLE_LINES - 1], sizeof(oledConsole[OLED_CONSOLE_LINES - 1]), text);
  xSemaphoreGive(stateMutex);
}

static void oledConsolePush(const char *prefix, const char *text) {
  char line[OLED_CONSOLE_COLS];
  snprintf(line, sizeof(line), "%s%s", prefix ? prefix : "", text ? text : "");
  oledConsolePushRaw(line);
}

static void setErr(const char *e) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  safeCopy(lastError, sizeof(lastError), e);
  xSemaphoreGive(stateMutex);
}

static void replyErr(Stream &io, const char *text) {
  char e[96];
  snprintf(e, sizeof(e), "-100,\"%s\"", text);
  setErr(e);
  oledConsolePush("<", e);
  io.println(e);
}

static char *trim(char *s) {
  if (!s) return s;
  while (*s && isspace((unsigned char)*s)) s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)*(end - 1))) end--;
  *end = 0;
  return s;
}

static void upperAscii(char *s) {
  while (s && *s) {
    *s = (char)toupper((unsigned char)*s);
    s++;
  }
}

static void stripLiteralEscapesAtEnd(char *s) {
  if (!s) return;
  s = trim(s);
  bool changed = true;
  while (changed) {
    changed = false;
    size_t n = strlen(s);
    if (n >= 2 && s[n - 2] == '\\' && (s[n - 1] == 'n' || s[n - 1] == 'N' || s[n - 1] == 'r' || s[n - 1] == 'R')) {
      s[n - 2] = 0;
      s = trim(s);
      changed = true;
    }
    n = strlen(s);
    if (n > 0 && s[n - 1] == ')') {
      s[n - 1] = 0;
      s = trim(s);
      changed = true;
    }
  }
}

static bool parseIntStrict(const char *s, int &out) {
  if (!s) return false;
  while (*s && isspace((unsigned char)*s)) s++;
  char *endPtr = nullptr;
  long v = strtol(s, &endPtr, 10);
  while (endPtr && isspace((unsigned char)*endPtr)) endPtr++;
  if (!endPtr || *endPtr != 0) return false;
  out = (int)v;
  return true;
}

static bool parseDigital(const char *s, int &out) {
  if (!s) return false;
  char tmp[16];
  safeCopy(tmp, sizeof(tmp), s);
  char *v = trim(tmp);
  upperAscii(v);
  if (!strcmp(v, "1") || !strcmp(v, "ON") || !strcmp(v, "HIGH") || !strcmp(v, "TRUE")) {
    out = HIGH;
    return true;
  }
  if (!strcmp(v, "0") || !strcmp(v, "OFF") || !strcmp(v, "LOW") || !strcmp(v, "FALSE")) {
    out = LOW;
    return true;
  }
  return false;
}

static bool parseFloatStrict(const char *s, float &out) {
  if (!s) return false;
  while (*s && isspace((unsigned char)*s)) s++;
  char *endPtr = nullptr;
  float v = strtof(s, &endPtr);
  while (endPtr && isspace((unsigned char)*endPtr)) endPtr++;
  if (!endPtr || *endPtr != 0) return false;
  out = v;
  return true;
}

static bool isDelimiter(char c) {
  return c == ',' || isspace((unsigned char)c);
}

static void initParams(ParamList &pl) {
  pl.count = 0;
  for (int i = 0; i < MAX_SCPI_PARAMS; i++) pl.v[i] = nullptr;
}

static int parseParams(char *params, ParamList &pl) {
  initParams(pl);
  if (!params) return 0;

  char *p = params;
  while (*p && pl.count < MAX_SCPI_PARAMS) {
    while (*p && isDelimiter(*p)) p++;
    if (!*p) break;

    char *start = nullptr;

    if (*p == '"' || *p == '\'') {
      char quote = *p;
      p++;
      start = p;
      while (*p && *p != quote) p++;
      if (*p == quote) {
        *p = 0;
        p++;
      }
      while (*p && !isDelimiter(*p)) p++;
      if (*p) {
        *p = 0;
        p++;
      }
    } else {
      start = p;
      while (*p && !isDelimiter(*p)) p++;
      if (*p) {
        *p = 0;
        p++;
      }
    }

    start = trim(start);
    stripLiteralEscapesAtEnd(start);
    if (*start) {
      pl.v[pl.count++] = start;
    }
  }

  return pl.count;
}

static const char *paramAt(const ParamList &pl, int index) {
  if (index < 0 || index >= pl.count) return nullptr;
  return pl.v[index];
}

static void printParseResult(const ParamList &pl, Stream &io) {
  io.print(F("PARAMS="));
  io.print(pl.count);
  for (int i = 0; i < pl.count; i++) {
    io.print(F(",P"));
    io.print(i);
    io.print(F("=\""));
    io.print(pl.v[i] ? pl.v[i] : "");
    io.print(F("\""));
  }
  io.println();
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
  if (eth) ethCount++;
  else serialCount++;
  safeCopy(lastSource, sizeof(lastSource), src);
  xSemaphoreGive(stateMutex);
}

static bool allocLineReceiver(LineReceiver &rx) {
  rx.cap = SCPI_LINE_LENGTH;
  rx.len = 0;
  rx.overflow = false;

  rx.buf = (char *)heap_caps_malloc(rx.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx.buf) {
    rx.buf = (char *)heap_caps_malloc(rx.cap, MALLOC_CAP_8BIT);
  }
  if (!rx.buf) return false;
  rx.buf[0] = 0;
  return true;
}

static void resetLine(LineReceiver &rx) {
  rx.len = 0;
  rx.overflow = false;
  if (rx.buf && rx.cap) rx.buf[0] = 0;
}

static bool readLine(Stream &io, LineReceiver &rx, Stream &replyTo) {
  while (io.available()) {
    int c = io.read();
    if (c < 0) break;

    if (c == '\r') continue;

    if (c == '\n') {
      if (rx.overflow) {
        resetLine(rx);
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        overflowCount++;
        xSemaphoreGive(stateMutex);
        replyErr(replyTo, "SCPI line too long");
        return false;
      }
      if (!rx.buf) return false;
      rx.buf[rx.len] = 0;
      return rx.len > 0;
    }

    if (!rx.buf || rx.cap < 2) continue;

    if (rx.len + 1 >= rx.cap) {
      rx.overflow = true;
      continue;
    }

    rx.buf[rx.len++] = (char)c;
  }
  return false;
}

static void cmdMem(Stream &io) {
  io.print(F("HEAP_FREE="));
  io.print(ESP.getFreeHeap());
  io.print(F(",HEAP_MIN="));
  io.print(ESP.getMinFreeHeap());
  io.print(F(",HEAP_LARGEST="));
  io.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  io.print(F(",PSRAM_SIZE="));
  io.print(ESP.getPsramSize());
  io.print(F(",PSRAM_FREE="));
  io.print(ESP.getFreePsram());
  io.print(F(",PSRAM_LARGEST="));
  io.print(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  io.print(F(",SCPI_LINE="));
  io.print(SCPI_LINE_LENGTH);
  io.print(F(",PSRAM_RX="));
  io.println(psramReady ? F("1") : F("0"));
}

static void cmdHelp(Stream &io) {
  io.println(F("*IDN?"));
  io.println(F("*RST"));
  io.println(F("HELP?"));
  io.println(F("MEM?"));
  io.println(F("PARSE? p0,p1,p2"));
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
  io.println(F("Accepted params: comma or spaces, any case, extra spaces, quoted text"));
  io.println(F("Example: gpio:write  5 , on ; mcp:write 0 off ; mem?"));
}

static void cmdStat(Stream &io) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool er = ethReady;
  bool mr = mcpReady;
  bool orr = oledReady;
  IPAddress ip = currentIp;
  uint32_t sc = serialCount;
  uint32_t ec = ethCount;
  uint32_t ov = overflowCount;
  char src[8];
  safeCopy(src, sizeof(src), lastSource);
  xSemaphoreGive(stateMutex);

  io.print(F("ETH=")); io.print(er ? F("1") : F("0"));
  io.print(F(",IP=")); io.print(ip);
  io.print(F(",MCP=")); io.print(mr ? F("1") : F("0"));
  io.print(F(",OLED=")); io.print(orr ? F("1") : F("0"));
  io.print(F(",SERIAL_CMDS=")); io.print(sc);
  io.print(F(",ETH_CMDS=")); io.print(ec);
  io.print(F(",OVERFLOW=")); io.print(ov);
  io.print(F(",LAST=")); io.println(src);
}

static void handleGpioMode(char *params, Stream &io) {
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 2) { replyErr(io, "GPIO:MODE needs pin,mode"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }

  char mode[24];
  safeCopy(mode, sizeof(mode), paramAt(pl, 1));
  upperAscii(mode);

  if (!strcmp(mode, "OUT") || !strcmp(mode, "OUTPUT")) pinMode(pin, OUTPUT);
  else if (!strcmp(mode, "IN") || !strcmp(mode, "INPUT")) pinMode(pin, INPUT);
  else if (!strcmp(mode, "INPULLUP") || !strcmp(mode, "INPUT_PULLUP") || !strcmp(mode, "PULLUP")) pinMode(pin, INPUT_PULLUP);
  else if (!strcmp(mode, "INPULLDOWN") || !strcmp(mode, "INPUT_PULLDOWN") || !strcmp(mode, "PULLDOWN")) pinMode(pin, INPUT_PULLDOWN);
  else { replyErr(io, "unknown ESP GPIO mode"); return; }
  oledConsolePush("<", "OK GPIO MODE");
  io.println(F("OK"));
}

static void handleGpioWrite(char *params, Stream &io) {
  ParamList pl;
  parseParams(params, pl);
  int pin, value;
  if (pl.count < 2) { replyErr(io, "GPIO:WRITE needs pin,value"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  if (!parseDigital(paramAt(pl, 1), value)) { replyErr(io, "bad GPIO value"); return; }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, value);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "OK GPIO%d=%d", pin, value ? 1 : 0);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleGpioRead(char *params, Stream &io) {
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 1) { replyErr(io, "GPIO:READ? needs pin"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  int v = digitalRead(pin) ? 1 : 0;
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "GPIO%d=%d", pin, v);
  oledConsolePush("<", res);
  io.println(v ? F("1") : F("0"));
}

static void handleGpioToggle(char *params, Stream &io) {
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 1) { replyErr(io, "GPIO:TOGGLE needs pin"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, !digitalRead(pin));
  int v = digitalRead(pin) ? 1 : 0;
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "OK GPIO%d=%d", pin, v);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleMcpMode(char *params, Stream &io) {
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 2) { replyErr(io, "MCP:MODE needs pin,mode"); return; }
  if (!parseMcpPin(paramAt(pl, 0), pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }

  char mode[24];
  safeCopy(mode, sizeof(mode), paramAt(pl, 1));
  upperAscii(mode);

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  if (!strcmp(mode, "OUT") || !strcmp(mode, "OUTPUT")) mcp.pinMode(pin, OUTPUT);
  else if (!strcmp(mode, "IN") || !strcmp(mode, "INPUT")) mcp.pinMode(pin, INPUT);
  else if (!strcmp(mode, "INPULLUP") || !strcmp(mode, "INPUT_PULLUP") || !strcmp(mode, "PULLUP")) mcp.pinMode(pin, INPUT_PULLUP);
  else {
    xSemaphoreGive(i2cMutex);
    replyErr(io, "unknown MCP mode");
    return;
  }
  xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "OK MCP%d MODE", pin);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleMcpWrite(char *params, Stream &io) {
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  ParamList pl;
  parseParams(params, pl);
  int pin, value;
  if (pl.count < 2) { replyErr(io, "MCP:WRITE needs pin,value"); return; }
  if (!parseMcpPin(paramAt(pl, 0), pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }
  if (!parseDigital(paramAt(pl, 1), value)) { replyErr(io, "bad MCP value"); return; }

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  mcp.pinMode(pin, OUTPUT);
  mcp.digitalWrite(pin, value);
  xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "OK MCP%d=%d", pin, value ? 1 : 0);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleMcpRead(char *params, Stream &io) {
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 1) { replyErr(io, "MCP:READ? needs pin"); return; }
  if (!parseMcpPin(paramAt(pl, 0), pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  int v = mcp.digitalRead(pin) ? 1 : 0;
  xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "MCP%d=%d", pin, v);
  oledConsolePush("<", res);
  io.println(v ? F("1") : F("0"));
}

static void handleMcpToggle(char *params, Stream &io) {
  if (!mcpReady) { replyErr(io, "MCP23017 not initialized"); return; }
  ParamList pl;
  parseParams(params, pl);
  int pin;
  if (pl.count < 1) { replyErr(io, "MCP:TOGGLE needs pin"); return; }
  if (!parseMcpPin(paramAt(pl, 0), pin)) { replyErr(io, "bad MCP pin, use 0..15"); return; }

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  mcp.pinMode(pin, OUTPUT);
  int v = mcp.digitalRead(pin);
  mcp.digitalWrite(pin, !v);
  int nv = mcp.digitalRead(pin) ? 1 : 0;
  xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "OK MCP%d=%d", pin, nv);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleParseTest(char *params, Stream &io) {
  ParamList pl;
  parseParams(params, pl);
  char res[OLED_CONSOLE_COLS];
  snprintf(res, sizeof(res), "PARAMS=%d", pl.count);
  oledConsolePush("<", res);
  printParseResult(pl, io);
}

static void handleOneCommand(char *cmdLine, Stream &io) {
  char original[OLED_CONSOLE_COLS];
  safeCopy(original, sizeof(original), trim(cmdLine));
  oledConsolePush(">", original);

  char *line = trim(cmdLine);
  stripLiteralEscapesAtEnd(line);
  if (!line || !*line) return;

  char *params = line;
  while (*params && !isspace((unsigned char)*params) && *params != '(') params++;
  if (*params) {
    char ender = *params;
    *params = 0;
    params++;
    params = trim(params);
    if (ender == '(') stripLiteralEscapesAtEnd(params);
  } else {
    params = nullptr;
  }

  upperAscii(line);

  if (!strcmp(line, "*IDN?")) {
    io.print(DEVICE_MANUFACTURER); io.print(',');
    io.print(DEVICE_MODEL); io.print(',');
    io.print(DEVICE_SERIAL); io.print(',');
    io.println(DEVICE_VERSION);
    oledConsolePush("<", "IDN OK");
  } else if (!strcmp(line, "*RST")) {
    setErr("0,\"No error\"");
    oledConsoleClear();
    oledConsolePush("<", "RESET OK");
    io.println(F("OK"));
  } else if (!strcmp(line, "HELP?")) {
    cmdHelp(io);
    oledConsolePush("<", "HELP OK");
  } else if (!strcmp(line, "MEM?")) {
    cmdMem(io);
    oledConsolePush("<", "MEM OK");
  } else if (!strcmp(line, "PARSE?") || !strcmp(line, "SYST:PARSE?")) {
    handleParseTest(params, io);
  } else if (!strcmp(line, "SYST:ERR?") || !strcmp(line, "SYSTEM:ERROR?")) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    char e[96];
    safeCopy(e, sizeof(e), lastError);
    safeCopy(lastError, sizeof(lastError), "0,\"No error\"");
    xSemaphoreGive(stateMutex);
    io.println(e);
    oledConsolePush("<", e);
  } else if (!strcmp(line, "SYST:STAT?") || !strcmp(line, "SYSTEM:STATUS?")) {
    cmdStat(io);
    oledConsolePush("<", "STAT OK");
  } else if (!strcmp(line, "ETH:IP?") || !strcmp(line, "ETHERNET:IP?")) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    IPAddress ip = currentIp;
    xSemaphoreGive(stateMutex);
    io.println(ip);
    char res[OLED_CONSOLE_COLS];
    snprintf(res, sizeof(res), "IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    oledConsolePush("<", res);
  } else if (!strcmp(line, "GPIO:MODE")) {
    handleGpioMode(params, io);
  } else if (!strcmp(line, "GPIO:WRITE")) {
    handleGpioWrite(params, io);
  } else if (!strcmp(line, "GPIO:READ?")) {
    handleGpioRead(params, io);
  } else if (!strcmp(line, "GPIO:TOGGLE")) {
    handleGpioToggle(params, io);
  } else if (!strcmp(line, "MCP:MODE")) {
    handleMcpMode(params, io);
  } else if (!strcmp(line, "MCP:WRITE")) {
    handleMcpWrite(params, io);
  } else if (!strcmp(line, "MCP:READ?")) {
    handleMcpRead(params, io);
  } else if (!strcmp(line, "MCP:TOGGLE")) {
    handleMcpToggle(params, io);
  } else {
    setErr("-113,\"Undefined header\"");
    oledConsolePush("<", "-113 Undefined");
    io.println(F("-113,\"Undefined header\""));
  }
}

static void handleScpiLine(char *line, Stream &io, bool eth) {
  incCounter(eth, eth ? "ETH" : "USB");

  char *part = line;
  while (part && *part) {
    char *sep = strchr(part, ';');
    if (sep) *sep = 0;
    handleOneCommand(part, io);
    if (!sep) break;
    part = sep + 1;
  }
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
    oled.println(F("console mode"));
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
    if (readLine(Serial, serialRx, Serial)) {
      handleScpiLine(serialRx.buf, Serial, false);
      resetLine(serialRx);
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
      resetLine(ethRx[i]);
      ethClients[i].println(F("ESP32-S3 SCPI TCP ready"));
      ethClients[i].println(F("Long commands use PSRAM buffer"));
      ethClients[i].println(F("Send HELP? MEM? PARSE?"));
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
        if (readLine(ethClients[i], ethRx[i], ethClients[i])) {
          handleScpiLine(ethRx[i].buf, ethClients[i], true);
          resetLine(ethRx[i]);
        }
      } else if (ethClients[i]) {
        ethClients[i].stop();
        resetLine(ethRx[i]);
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
      uint32_t sc = serialCount;
      uint32_t ec = ethCount;
      char lines[OLED_CONSOLE_LINES][OLED_CONSOLE_COLS];
      for (int i = 0; i < OLED_CONSOLE_LINES; i++) {
        safeCopy(lines[i], sizeof(lines[i]), oledConsole[i]);
      }
      xSemaphoreGive(stateMutex);

      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(0, 0);
      oled.print(F("E:")); oled.print(er ? F("OK") : F("NO"));
      oled.print(F(" M:")); oled.print(mr ? F("OK") : F("NO"));
      oled.print(F(" U:")); oled.print(sc);
      oled.print(F(" T:")); oled.println(ec);
      for (int i = 0; i < OLED_CONSOLE_LINES; i++) {
        oled.println(lines[i]);
      }
      oled.display();
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  i2cMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();
  oledConsoleClear();
  oledConsolePushRaw("console ready");

  psramReady = psramFound() && ESP.getPsramSize() > 0;

  bool ok = allocLineReceiver(serialRx);
  for (int i = 0; i < MAX_ETH_CLIENTS; i++) ok = allocLineReceiver(ethRx[i]) && ok;

  Serial.println();
  Serial.println(F("ESP32-S3 long SCPI GPIO ready"));
  Serial.print(F("SCPI line length: ")); Serial.println(SCPI_LINE_LENGTH);
  Serial.print(F("PSRAM size: ")); Serial.println(ESP.getPsramSize());
  Serial.print(F("PSRAM free: ")); Serial.println(ESP.getFreePsram());
  Serial.print(F("RX buffers allocated: ")); Serial.println(ok ? F("OK") : F("FAIL"));
  Serial.println(F("Robust parameter parser: comma/space/case/quotes OK"));
  Serial.println(F("OLED console mode enabled"));

  if (!ok) {
    setErr("-300,\"RX buffer allocation failed\"");
    oledConsolePush("<", "RX alloc fail");
  }

  initI2c();
  initW5500();

  Serial.print(F("Ethernet IP: "));
  Serial.println(Ethernet.localIP());
  Serial.println(F("USB Serial and TCP/5025 enabled"));
  Serial.println(F("Send HELP? MEM? PARSE?"));

  xTaskCreatePinnedToCore(taskSerial, "scpi_serial", TASK_STACK_SERIAL, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskEthernet, "scpi_eth", TASK_STACK_ETH, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(taskOled, "oled", TASK_STACK_OLED, nullptr, 1, nullptr, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
