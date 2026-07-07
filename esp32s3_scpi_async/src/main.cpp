#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Ethernet.h>
#include <Preferences.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_heap_caps.h"
#include "config.h"

#define MAX_SCPI_PARAMS 32
#define OLED_CONSOLE_LINES 7
#define OLED_CONSOLE_COLS 22
#define MENU_ITEMS 8

class EthernetServerCompat : public EthernetServer {
public:
  explicit EthernetServerCompat(uint16_t port) : EthernetServer(port) {}
  void begin(uint16_t port = 0) override { (void)port; EthernetServer::begin(); }
};

struct LineReceiver { char *buf = nullptr; size_t cap = 0; size_t len = 0; bool overflow = false; };
struct ParamList { char *v[MAX_SCPI_PARAMS]; int count = 0; };
struct McpPinRef { int globalPin = -1; int chip = -1; int localPin = -1; };

enum UiMode { UI_CONSOLE, UI_MENU, UI_EDIT_IP, UI_EDIT_GW, UI_EDIT_MASK, UI_EDIT_DNS, UI_EDIT_DHCP, UI_EDIT_PORT };

Adafruit_MCP23X17 mcp[MCP23017_COUNT];
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);
EthernetServerCompat *scpiServer = nullptr;
EthernetClient ethClients[MAX_ETH_CLIENTS];
Preferences prefs;

LineReceiver serialRx;
LineReceiver ethRx[MAX_ETH_CLIENTS];
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t stateMutex;

const uint8_t mcpAddr[MCP23017_COUNT] = { MCP23017_ADDR0, MCP23017_ADDR1, MCP23017_ADDR2, MCP23017_ADDR3 };
bool mcpReady[MCP23017_COUNT] = {false, false, false, false};
bool oledReady = false;
bool ethReady = false;
bool psramReady = false;
bool netRestartRequest = false;

IPAddress currentIp(0, 0, 0, 0);
IPAddress cfgIp(DEFAULT_IP_A, DEFAULT_IP_B, DEFAULT_IP_C, DEFAULT_IP_D);
IPAddress cfgGw(DEFAULT_GW_A, DEFAULT_GW_B, DEFAULT_GW_C, DEFAULT_GW_D);
IPAddress cfgMask(DEFAULT_MASK_A, DEFAULT_MASK_B, DEFAULT_MASK_C, DEFAULT_MASK_D);
IPAddress cfgDns(DEFAULT_DNS_A, DEFAULT_DNS_B, DEFAULT_DNS_C, DEFAULT_DNS_D);
bool cfgUseDhcp = DEFAULT_USE_DHCP;
uint16_t cfgPort = SCPI_TCP_PORT;

char lastError[96] = "0,\"No error\"";
char lastSource[8] = "boot";
char oledConsole[OLED_CONSOLE_LINES][OLED_CONSOLE_COLS];
uint32_t serialCount = 0;
uint32_t ethCount = 0;
uint32_t overflowCount = 0;

UiMode uiMode = UI_CONSOLE;
int menuIndex = 0;
int editOctet = 0;
uint32_t lastButtonMs = 0;

byte macAddress[] = {0x02, 0xA5, 0xB0, 0xC0, 0x00, 0x01};

static void safeCopy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = 0;
}

static void ipToText(const IPAddress &ip, char *out, size_t n) {
  snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static int mcpReadyCount() { int c = 0; for (int i = 0; i < MCP23017_COUNT; i++) if (mcpReady[i]) c++; return c; }
static uint8_t mcpReadyMask() { uint8_t m = 0; for (int i = 0; i < MCP23017_COUNT; i++) if (mcpReady[i]) m |= (1 << i); return m; }

static void oledConsoleClear() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < OLED_CONSOLE_LINES; i++) oledConsole[i][0] = 0;
  xSemaphoreGive(stateMutex);
}

static void oledConsolePushRaw(const char *text) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (int i = 0; i < OLED_CONSOLE_LINES - 1; i++) safeCopy(oledConsole[i], sizeof(oledConsole[i]), oledConsole[i + 1]);
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

static void upperAscii(char *s) { while (s && *s) { *s = (char)toupper((unsigned char)*s); s++; } }

static void stripLiteralEscapesAtEnd(char *s) {
  if (!s) return;
  s = trim(s);
  bool changed = true;
  while (changed) {
    changed = false;
    size_t n = strlen(s);
    if (n >= 2 && s[n - 2] == '\\' && (s[n - 1] == 'n' || s[n - 1] == 'N' || s[n - 1] == 'r' || s[n - 1] == 'R')) { s[n - 2] = 0; s = trim(s); changed = true; }
    n = strlen(s);
    if (n > 0 && s[n - 1] == ')') { s[n - 1] = 0; s = trim(s); changed = true; }
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
  char tmp[16]; safeCopy(tmp, sizeof(tmp), s);
  char *v = trim(tmp); upperAscii(v);
  if (!strcmp(v, "1") || !strcmp(v, "ON") || !strcmp(v, "HIGH") || !strcmp(v, "TRUE")) { out = HIGH; return true; }
  if (!strcmp(v, "0") || !strcmp(v, "OFF") || !strcmp(v, "LOW") || !strcmp(v, "FALSE")) { out = LOW; return true; }
  return false;
}

static bool isDelimiter(char c) { return c == ',' || isspace((unsigned char)c); }

static int parseParams(char *params, ParamList &pl) {
  pl.count = 0;
  for (int i = 0; i < MAX_SCPI_PARAMS; i++) pl.v[i] = nullptr;
  if (!params) return 0;
  char *p = params;
  while (*p && pl.count < MAX_SCPI_PARAMS) {
    while (*p && isDelimiter(*p)) p++;
    if (!*p) break;
    char *start = nullptr;
    if (*p == '"' || *p == '\'') {
      char quote = *p++;
      start = p;
      while (*p && *p != quote) p++;
      if (*p == quote) *p++ = 0;
      while (*p && !isDelimiter(*p)) p++;
      if (*p) *p++ = 0;
    } else {
      start = p;
      while (*p && !isDelimiter(*p)) p++;
      if (*p) *p++ = 0;
    }
    start = trim(start);
    stripLiteralEscapesAtEnd(start);
    if (*start) pl.v[pl.count++] = start;
  }
  return pl.count;
}

static const char *paramAt(const ParamList &pl, int index) { return (index >= 0 && index < pl.count) ? pl.v[index] : nullptr; }

static bool parseIpParams(const ParamList &pl, IPAddress &ip) {
  if (pl.count < 4) return false;
  int a[4];
  for (int i = 0; i < 4; i++) {
    if (!parseIntStrict(paramAt(pl, i), a[i])) return false;
    if (a[i] < 0 || a[i] > 255) return false;
  }
  ip = IPAddress(a[0], a[1], a[2], a[3]);
  return true;
}

static void printParseResult(const ParamList &pl, Stream &io) {
  io.print(F("PARAMS=")); io.print(pl.count);
  for (int i = 0; i < pl.count; i++) { io.print(F(",P")); io.print(i); io.print(F("=\"")); io.print(pl.v[i] ? pl.v[i] : ""); io.print(F("\"")); }
  io.println();
}

static void loadNetPrefs() {
  prefs.begin("net", true);
  cfgUseDhcp = prefs.getBool("dhcp", DEFAULT_USE_DHCP);
  cfgPort = prefs.getUShort("port", SCPI_TCP_PORT);
  if (cfgPort == 0) cfgPort = SCPI_TCP_PORT;
  cfgIp = IPAddress(prefs.getUChar("ip0", DEFAULT_IP_A), prefs.getUChar("ip1", DEFAULT_IP_B), prefs.getUChar("ip2", DEFAULT_IP_C), prefs.getUChar("ip3", DEFAULT_IP_D));
  cfgGw = IPAddress(prefs.getUChar("gw0", DEFAULT_GW_A), prefs.getUChar("gw1", DEFAULT_GW_B), prefs.getUChar("gw2", DEFAULT_GW_C), prefs.getUChar("gw3", DEFAULT_GW_D));
  cfgMask = IPAddress(prefs.getUChar("ms0", DEFAULT_MASK_A), prefs.getUChar("ms1", DEFAULT_MASK_B), prefs.getUChar("ms2", DEFAULT_MASK_C), prefs.getUChar("ms3", DEFAULT_MASK_D));
  cfgDns = IPAddress(prefs.getUChar("dn0", DEFAULT_DNS_A), prefs.getUChar("dn1", DEFAULT_DNS_B), prefs.getUChar("dn2", DEFAULT_DNS_C), prefs.getUChar("dn3", DEFAULT_DNS_D));
  prefs.end();
}

static void saveNetPrefs() {
  prefs.begin("net", false);
  prefs.putBool("dhcp", cfgUseDhcp);
  prefs.putUShort("port", cfgPort);
  for (int i = 0; i < 4; i++) {
    char k[4];
    snprintf(k, sizeof(k), "ip%d", i); prefs.putUChar(k, cfgIp[i]);
    snprintf(k, sizeof(k), "gw%d", i); prefs.putUChar(k, cfgGw[i]);
    snprintf(k, sizeof(k), "ms%d", i); prefs.putUChar(k, cfgMask[i]);
    snprintf(k, sizeof(k), "dn%d", i); prefs.putUChar(k, cfgDns[i]);
  }
  prefs.end();
}

static bool isEspPinAllowed(int pin) {
  if (pin < 0 || pin > 48) return false;
  if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return false;
  if (pin == 19 || pin == 20) return false;
  if (pin >= 26 && pin <= 37) return false;
  if (pin == I2C_SDA_PIN || pin == I2C_SCL_PIN) return false;
  if (pin == W5500_SCK_PIN || pin == W5500_MISO_PIN || pin == W5500_MOSI_PIN) return false;
  if (pin == W5500_CS_PIN || pin == W5500_RST_PIN) return false;
  if (pin == ENC_CLK_PIN || pin == ENC_DT_PIN || pin == ENC_SW_PIN) return false;
  return true;
}

static bool parseEspPin(const char *s, int &pin) { int p = -1; if (!parseIntStrict(s, p)) return false; if (!isEspPinAllowed(p)) return false; pin = p; return true; }

static bool parseMcpPinRef(const char *s, McpPinRef &ref) {
  int p = -1;
  if (!parseIntStrict(s, p)) return false;
  if (p < 0 || p >= MCP_TOTAL_PINS) return false;
  ref.globalPin = p; ref.chip = p / 16; ref.localPin = p % 16;
  if (ref.chip < 0 || ref.chip >= MCP23017_COUNT) return false;
  if (!mcpReady[ref.chip]) return false;
  return true;
}

static void incCounter(bool eth, const char *src) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (eth) ethCount++; else serialCount++;
  safeCopy(lastSource, sizeof(lastSource), src);
  xSemaphoreGive(stateMutex);
}

static bool allocLineReceiver(LineReceiver &rx) {
  rx.cap = SCPI_LINE_LENGTH; rx.len = 0; rx.overflow = false;
  rx.buf = (char *)heap_caps_malloc(rx.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx.buf) rx.buf = (char *)heap_caps_malloc(rx.cap, MALLOC_CAP_8BIT);
  if (!rx.buf) return false;
  rx.buf[0] = 0;
  return true;
}

static void resetLine(LineReceiver &rx) { rx.len = 0; rx.overflow = false; if (rx.buf && rx.cap) rx.buf[0] = 0; }

static bool readLine(Stream &io, LineReceiver &rx, Stream &replyTo) {
  while (io.available()) {
    int c = io.read(); if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (rx.overflow) { resetLine(rx); xSemaphoreTake(stateMutex, portMAX_DELAY); overflowCount++; xSemaphoreGive(stateMutex); replyErr(replyTo, "SCPI line too long"); return false; }
      if (!rx.buf) return false;
      rx.buf[rx.len] = 0;
      return rx.len > 0;
    }
    if (!rx.buf || rx.cap < 2) continue;
    if (rx.len + 1 >= rx.cap) { rx.overflow = true; continue; }
    rx.buf[rx.len++] = (char)c;
  }
  return false;
}

static void cmdMem(Stream &io) {
  io.print(F("HEAP_FREE=")); io.print(ESP.getFreeHeap());
  io.print(F(",HEAP_MIN=")); io.print(ESP.getMinFreeHeap());
  io.print(F(",HEAP_LARGEST=")); io.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  io.print(F(",PSRAM_SIZE=")); io.print(ESP.getPsramSize());
  io.print(F(",PSRAM_FREE=")); io.print(ESP.getFreePsram());
  io.print(F(",PSRAM_LARGEST=")); io.print(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  io.print(F(",SCPI_LINE=")); io.print(SCPI_LINE_LENGTH);
  io.print(F(",PSRAM_RX=")); io.println(psramReady ? F("1") : F("0"));
}

static void cmdMcpStat(Stream &io) {
  io.print(F("MCP_COUNT=")); io.print(MCP23017_COUNT);
  io.print(F(",MCP_READY=")); io.print(mcpReadyCount());
  io.print(F(",MCP_MASK=0x")); io.print(mcpReadyMask(), HEX);
  for (int i = 0; i < MCP23017_COUNT; i++) { io.print(F(",MCP")); io.print(i); io.print(F("=0x")); io.print(mcpAddr[i], HEX); io.print(mcpReady[i] ? F("/OK") : F("/NO")); }
  io.println();
}

static void cmdNetStat(Stream &io) {
  char ip[18], gw[18], mask[18], dns[18], local[18];
  ipToText(cfgIp, ip, sizeof(ip)); ipToText(cfgGw, gw, sizeof(gw)); ipToText(cfgMask, mask, sizeof(mask)); ipToText(cfgDns, dns, sizeof(dns)); ipToText(currentIp, local, sizeof(local));
  io.print(F("DHCP=")); io.print(cfgUseDhcp ? F("1") : F("0"));
  io.print(F(",PORT=")); io.print(cfgPort);
  io.print(F(",IP=")); io.print(ip);
  io.print(F(",GW=")); io.print(gw);
  io.print(F(",MASK=")); io.print(mask);
  io.print(F(",DNS=")); io.print(dns);
  io.print(F(",LOCAL=")); io.println(local);
}

static void cmdHelp(Stream &io) {
  io.println(F("*IDN?")); io.println(F("*RST")); io.println(F("HELP?")); io.println(F("MEM?")); io.println(F("PARSE? p0,p1,p2"));
  io.println(F("SYST:ERR?")); io.println(F("SYST:STAT?")); io.println(F("ETH:IP?"));
  io.println(F("NET:STAT?")); io.println(F("NET:DHCP 0|1")); io.println(F("NET:PORT 1..65535")); io.println(F("NET:IP a,b,c,d")); io.println(F("NET:GW a,b,c,d")); io.println(F("NET:MASK a,b,c,d")); io.println(F("NET:DNS a,b,c,d")); io.println(F("NET:SAVE")); io.println(F("NET:RESTART"));
  io.println(F("MCP:STAT?"));
  io.println(F("GPIO:MODE pin,OUT|IN|INPULLUP|INPULLDOWN")); io.println(F("GPIO:WRITE pin,0|1|ON|OFF|HIGH|LOW")); io.println(F("GPIO:READ? pin")); io.println(F("GPIO:TOGGLE pin"));
  io.println(F("MCP pins are global 0..63 for 4x MCP23017")); io.println(F("MCP0 0..15, MCP1 16..31, MCP2 32..47, MCP3 48..63"));
  io.println(F("MCP:MODE 0..63,OUT|IN|INPULLUP")); io.println(F("MCP:WRITE 0..63,0|1|ON|OFF|HIGH|LOW")); io.println(F("MCP:READ? 0..63")); io.println(F("MCP:TOGGLE 0..63"));
}

static void cmdStat(Stream &io) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool er = ethReady, orr = oledReady; IPAddress ip = currentIp; uint32_t sc = serialCount, ec = ethCount, ov = overflowCount; char src[8]; safeCopy(src, sizeof(src), lastSource);
  xSemaphoreGive(stateMutex);
  io.print(F("ETH=")); io.print(er ? F("1") : F("0")); io.print(F(",IP=")); io.print(ip); io.print(F(",PORT=")); io.print(cfgPort);
  io.print(F(",MCP_READY=")); io.print(mcpReadyCount()); io.print('/'); io.print(MCP23017_COUNT); io.print(F(",MCP_MASK=0x")); io.print(mcpReadyMask(), HEX);
  io.print(F(",OLED=")); io.print(orr ? F("1") : F("0")); io.print(F(",SERIAL_CMDS=")); io.print(sc); io.print(F(",ETH_CMDS=")); io.print(ec); io.print(F(",OVERFLOW=")); io.print(ov); io.print(F(",LAST=")); io.println(src);
}

static void handleGpioMode(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int pin;
  if (pl.count < 2) { replyErr(io, "GPIO:MODE needs pin,mode"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  char mode[24]; safeCopy(mode, sizeof(mode), paramAt(pl, 1)); upperAscii(mode);
  if (!strcmp(mode, "OUT") || !strcmp(mode, "OUTPUT")) pinMode(pin, OUTPUT);
  else if (!strcmp(mode, "IN") || !strcmp(mode, "INPUT")) pinMode(pin, INPUT);
  else if (!strcmp(mode, "INPULLUP") || !strcmp(mode, "INPUT_PULLUP") || !strcmp(mode, "PULLUP")) pinMode(pin, INPUT_PULLUP);
  else if (!strcmp(mode, "INPULLDOWN") || !strcmp(mode, "INPUT_PULLDOWN") || !strcmp(mode, "PULLDOWN")) pinMode(pin, INPUT_PULLDOWN);
  else { replyErr(io, "unknown ESP GPIO mode"); return; }
  oledConsolePush("<", "OK GPIO MODE"); io.println(F("OK"));
}

static void handleGpioWrite(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int pin, value;
  if (pl.count < 2) { replyErr(io, "GPIO:WRITE needs pin,value"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  if (!parseDigital(paramAt(pl, 1), value)) { replyErr(io, "bad GPIO value"); return; }
  pinMode(pin, OUTPUT); digitalWrite(pin, value);
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "OK GPIO%d=%d", pin, value ? 1 : 0); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleGpioRead(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int pin;
  if (pl.count < 1) { replyErr(io, "GPIO:READ? needs pin"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  int v = digitalRead(pin) ? 1 : 0; char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "GPIO%d=%d", pin, v); oledConsolePush("<", res); io.println(v ? F("1") : F("0"));
}

static void handleGpioToggle(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int pin;
  if (pl.count < 1) { replyErr(io, "GPIO:TOGGLE needs pin"); return; }
  if (!parseEspPin(paramAt(pl, 0), pin)) { replyErr(io, "bad/protected ESP GPIO pin"); return; }
  pinMode(pin, OUTPUT); digitalWrite(pin, !digitalRead(pin)); int v = digitalRead(pin) ? 1 : 0;
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "OK GPIO%d=%d", pin, v); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleMcpMode(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); McpPinRef r;
  if (pl.count < 2) { replyErr(io, "MCP:MODE needs pin,mode"); return; }
  if (!parseMcpPinRef(paramAt(pl, 0), r)) { replyErr(io, "bad/offline MCP pin 0..63"); return; }
  char mode[24]; safeCopy(mode, sizeof(mode), paramAt(pl, 1)); upperAscii(mode);
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  if (!strcmp(mode, "OUT") || !strcmp(mode, "OUTPUT")) mcp[r.chip].pinMode(r.localPin, OUTPUT);
  else if (!strcmp(mode, "IN") || !strcmp(mode, "INPUT")) mcp[r.chip].pinMode(r.localPin, INPUT);
  else if (!strcmp(mode, "INPULLUP") || !strcmp(mode, "INPUT_PULLUP") || !strcmp(mode, "PULLUP")) mcp[r.chip].pinMode(r.localPin, INPUT_PULLUP);
  else { xSemaphoreGive(i2cMutex); replyErr(io, "unknown MCP mode"); return; }
  xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "OK M%d.%d MODE", r.chip, r.localPin); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleMcpWrite(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); McpPinRef r; int value;
  if (pl.count < 2) { replyErr(io, "MCP:WRITE needs pin,value"); return; }
  if (!parseMcpPinRef(paramAt(pl, 0), r)) { replyErr(io, "bad/offline MCP pin 0..63"); return; }
  if (!parseDigital(paramAt(pl, 1), value)) { replyErr(io, "bad MCP value"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY); mcp[r.chip].pinMode(r.localPin, OUTPUT); mcp[r.chip].digitalWrite(r.localPin, value); xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "OK M%d.%d=%d", r.chip, r.localPin, value ? 1 : 0); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleMcpRead(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); McpPinRef r;
  if (pl.count < 1) { replyErr(io, "MCP:READ? needs pin"); return; }
  if (!parseMcpPinRef(paramAt(pl, 0), r)) { replyErr(io, "bad/offline MCP pin 0..63"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY); int v = mcp[r.chip].digitalRead(r.localPin) ? 1 : 0; xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "M%d.%d=%d", r.chip, r.localPin, v); oledConsolePush("<", res); io.println(v ? F("1") : F("0"));
}

static void handleMcpToggle(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); McpPinRef r;
  if (pl.count < 1) { replyErr(io, "MCP:TOGGLE needs pin"); return; }
  if (!parseMcpPinRef(paramAt(pl, 0), r)) { replyErr(io, "bad/offline MCP pin 0..63"); return; }
  xSemaphoreTake(i2cMutex, portMAX_DELAY); mcp[r.chip].pinMode(r.localPin, OUTPUT); int v = mcp[r.chip].digitalRead(r.localPin); mcp[r.chip].digitalWrite(r.localPin, !v); int nv = mcp[r.chip].digitalRead(r.localPin) ? 1 : 0; xSemaphoreGive(i2cMutex);
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "OK M%d.%d=%d", r.chip, r.localPin, nv); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleNetIpSet(char *params, Stream &io, IPAddress &target, const char *label) {
  ParamList pl; parseParams(params, pl);
  if (!parseIpParams(pl, target)) { replyErr(io, "bad IP, use a,b,c,d"); return; }
  saveNetPrefs(); netRestartRequest = true;
  char ip[18], res[OLED_CONSOLE_COLS]; ipToText(target, ip, sizeof(ip)); snprintf(res, sizeof(res), "%s %s", label, ip); oledConsolePush("<", res); io.println(F("OK"));
}

static void handleNetDhcp(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int v;
  if (pl.count < 1 || !parseDigital(paramAt(pl, 0), v)) { replyErr(io, "NET:DHCP needs 0|1"); return; }
  cfgUseDhcp = v ? true : false; saveNetPrefs(); netRestartRequest = true; oledConsolePush("<", cfgUseDhcp ? "DHCP ON" : "DHCP OFF"); io.println(F("OK"));
}

static void handleNetPort(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); int p;
  if (pl.count < 1 || !parseIntStrict(paramAt(pl, 0), p) || p < 1 || p > 65535) { replyErr(io, "NET:PORT needs 1..65535"); return; }
  cfgPort = (uint16_t)p;
  saveNetPrefs();
  netRestartRequest = true;
  char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "PORT %u", cfgPort);
  oledConsolePush("<", res);
  io.println(F("OK"));
}

static void handleParseTest(char *params, Stream &io) {
  ParamList pl; parseParams(params, pl); char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "PARAMS=%d", pl.count); oledConsolePush("<", res); printParseResult(pl, io);
}

static void initW5500();

static void handleOneCommand(char *cmdLine, Stream &io) {
  char original[OLED_CONSOLE_COLS]; safeCopy(original, sizeof(original), trim(cmdLine)); oledConsolePush(">", original);
  char *line = trim(cmdLine); stripLiteralEscapesAtEnd(line); if (!line || !*line) return;
  char *params = line;
  while (*params && !isspace((unsigned char)*params) && *params != '(') params++;
  if (*params) { char ender = *params; *params = 0; params++; params = trim(params); if (ender == '(') stripLiteralEscapesAtEnd(params); } else params = nullptr;
  upperAscii(line);

  if (!strcmp(line, "*IDN?")) { io.print(DEVICE_MANUFACTURER); io.print(','); io.print(DEVICE_MODEL); io.print(','); io.print(DEVICE_SERIAL); io.print(','); io.println(DEVICE_VERSION); oledConsolePush("<", "IDN OK"); }
  else if (!strcmp(line, "*RST")) { setErr("0,\"No error\""); oledConsoleClear(); oledConsolePush("<", "RESET OK"); io.println(F("OK")); }
  else if (!strcmp(line, "HELP?")) { cmdHelp(io); oledConsolePush("<", "HELP OK"); }
  else if (!strcmp(line, "MEM?")) { cmdMem(io); oledConsolePush("<", "MEM OK"); }
  else if (!strcmp(line, "PARSE?") || !strcmp(line, "SYST:PARSE?")) handleParseTest(params, io);
  else if (!strcmp(line, "MCP:STAT?")) { cmdMcpStat(io); oledConsolePush("<", "MCP STAT OK"); }
  else if (!strcmp(line, "NET:STAT?")) { cmdNetStat(io); oledConsolePush("<", "NET STAT OK"); }
  else if (!strcmp(line, "NET:DHCP")) handleNetDhcp(params, io);
  else if (!strcmp(line, "NET:PORT")) handleNetPort(params, io);
  else if (!strcmp(line, "NET:IP")) handleNetIpSet(params, io, cfgIp, "IP");
  else if (!strcmp(line, "NET:GW")) handleNetIpSet(params, io, cfgGw, "GW");
  else if (!strcmp(line, "NET:MASK")) handleNetIpSet(params, io, cfgMask, "MASK");
  else if (!strcmp(line, "NET:DNS")) handleNetIpSet(params, io, cfgDns, "DNS");
  else if (!strcmp(line, "NET:SAVE")) { saveNetPrefs(); oledConsolePush("<", "NET SAVED"); io.println(F("OK")); }
  else if (!strcmp(line, "NET:RESTART")) { netRestartRequest = true; oledConsolePush("<", "NET RESTART"); io.println(F("OK")); }
  else if (!strcmp(line, "SYST:ERR?") || !strcmp(line, "SYSTEM:ERROR?")) { xSemaphoreTake(stateMutex, portMAX_DELAY); char e[96]; safeCopy(e, sizeof(e), lastError); safeCopy(lastError, sizeof(lastError), "0,\"No error\""); xSemaphoreGive(stateMutex); io.println(e); oledConsolePush("<", e); }
  else if (!strcmp(line, "SYST:STAT?") || !strcmp(line, "SYSTEM:STATUS?")) { cmdStat(io); oledConsolePush("<", "STAT OK"); }
  else if (!strcmp(line, "ETH:IP?") || !strcmp(line, "ETHERNET:IP?")) { xSemaphoreTake(stateMutex, portMAX_DELAY); IPAddress ip = currentIp; xSemaphoreGive(stateMutex); io.println(ip); char res[OLED_CONSOLE_COLS]; snprintf(res, sizeof(res), "IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); oledConsolePush("<", res); }
  else if (!strcmp(line, "GPIO:MODE")) handleGpioMode(params, io);
  else if (!strcmp(line, "GPIO:WRITE")) handleGpioWrite(params, io);
  else if (!strcmp(line, "GPIO:READ?")) handleGpioRead(params, io);
  else if (!strcmp(line, "GPIO:TOGGLE")) handleGpioToggle(params, io);
  else if (!strcmp(line, "MCP:MODE")) handleMcpMode(params, io);
  else if (!strcmp(line, "MCP:WRITE")) handleMcpWrite(params, io);
  else if (!strcmp(line, "MCP:READ?")) handleMcpRead(params, io);
  else if (!strcmp(line, "MCP:TOGGLE")) handleMcpToggle(params, io);
  else { setErr("-113,\"Undefined header\""); oledConsolePush("<", "-113 Undefined"); io.println(F("-113,\"Undefined header\"")); }
}

static void handleScpiLine(char *line, Stream &io, bool eth) {
  incCounter(eth, eth ? "ETH" : "USB");
  char *part = line;
  while (part && *part) { char *sep = strchr(part, ';'); if (sep) *sep = 0; handleOneCommand(part, io); if (!sep) break; part = sep + 1; }
}

static void initI2c() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  for (int chip = 0; chip < MCP23017_COUNT; chip++) {
    mcpReady[chip] = mcp[chip].begin_I2C(mcpAddr[chip], &Wire);
    if (mcpReady[chip]) for (int p = 0; p < 16; p++) mcp[chip].pinMode(p, INPUT_PULLUP);
  }
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledReady) { oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0); oled.println(F("SCPI GPIO boot")); oled.println(F("encoder IP menu")); oled.display(); }
  xSemaphoreGive(i2cMutex);
}

static void initW5500() {
  for (int i = 0; i < MAX_ETH_CLIENTS; i++) if (ethClients[i]) ethClients[i].stop();
  if (scpiServer) { delete scpiServer; scpiServer = nullptr; }
  pinMode(W5500_RST_PIN, OUTPUT);
  digitalWrite(W5500_RST_PIN, LOW); delay(50); digitalWrite(W5500_RST_PIN, HIGH); delay(200);
  SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_PIN);
  Ethernet.init(W5500_CS_PIN);
  int dhcp = 0;
  if (cfgUseDhcp) dhcp = Ethernet.begin(macAddress, 5000, 1000);
  if (!cfgUseDhcp || dhcp == 0) Ethernet.begin(macAddress, cfgIp, cfgDns, cfgGw, cfgMask);
  scpiServer = new EthernetServerCompat(cfgPort);
  scpiServer->begin();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  currentIp = Ethernet.localIP();
  ethReady = Ethernet.hardwareStatus() != EthernetNoHardware && Ethernet.linkStatus() != LinkOFF;
  xSemaphoreGive(stateMutex);
  char txt[OLED_CONSOLE_COLS]; char ip[18]; ipToText(currentIp, ip, sizeof(ip)); snprintf(txt, sizeof(txt), "NET %s:%u", ip, cfgPort); oledConsolePush("<", txt);
}

static IPAddress *editIpPtr() {
  if (uiMode == UI_EDIT_IP) return &cfgIp;
  if (uiMode == UI_EDIT_GW) return &cfgGw;
  if (uiMode == UI_EDIT_MASK) return &cfgMask;
  if (uiMode == UI_EDIT_DNS) return &cfgDns;
  return nullptr;
}

static void editOctetDelta(int delta) {
  IPAddress *ip = editIpPtr();
  if (!ip) return;
  int v = (*ip)[editOctet] + delta;
  while (v < 0) v += 256;
  while (v > 255) v -= 256;
  (*ip)[editOctet] = (uint8_t)v;
}

static void handleEncoderTurn(int delta) {
  if (uiMode == UI_MENU) {
    menuIndex += delta;
    if (menuIndex < 0) menuIndex = MENU_ITEMS - 1;
    if (menuIndex >= MENU_ITEMS) menuIndex = 0;
  } else if (uiMode == UI_EDIT_DHCP) {
    cfgUseDhcp = !cfgUseDhcp;
  } else if (uiMode == UI_EDIT_PORT) {
    int p = (int)cfgPort + delta;
    if (p < 1) p = 65535;
    if (p > 65535) p = 1;
    cfgPort = (uint16_t)p;
  } else if (uiMode != UI_CONSOLE) {
    editOctetDelta(delta);
  }
}

static void handleEncoderButton() {
  if (uiMode == UI_CONSOLE) { uiMode = UI_MENU; menuIndex = 0; return; }
  if (uiMode == UI_MENU) {
    editOctet = 0;
    if (menuIndex == 0) uiMode = UI_EDIT_IP;
    else if (menuIndex == 1) uiMode = UI_EDIT_GW;
    else if (menuIndex == 2) uiMode = UI_EDIT_MASK;
    else if (menuIndex == 3) uiMode = UI_EDIT_DNS;
    else if (menuIndex == 4) uiMode = UI_EDIT_DHCP;
    else if (menuIndex == 5) uiMode = UI_EDIT_PORT;
    else if (menuIndex == 6) { saveNetPrefs(); netRestartRequest = true; uiMode = UI_CONSOLE; oledConsolePush("<", "SAVE+NET RESTART"); }
    else uiMode = UI_CONSOLE;
    return;
  }
  if (uiMode == UI_EDIT_DHCP || uiMode == UI_EDIT_PORT) { saveNetPrefs(); netRestartRequest = true; uiMode = UI_MENU; return; }
  editOctet++;
  if (editOctet > 3) { saveNetPrefs(); netRestartRequest = true; uiMode = UI_MENU; editOctet = 0; }
}

static void drawMenu() {
  static const char *items[] = { "IP", "Gateway", "Mask", "DNS", "DHCP", "Port", "Save+Restart", "Exit" };
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
  oled.println(F("== NET MENU =="));
  int first = menuIndex - 2; if (first < 0) first = 0; if (first > MENU_ITEMS - 5) first = MENU_ITEMS - 5;
  for (int i = first; i < first + 5 && i < MENU_ITEMS; i++) { oled.print(i == menuIndex ? F(">") : F(" ")); oled.println(items[i]); }
  oled.println(F("Press=enter")); oled.display();
}

static void drawEditIp(const char *title, const IPAddress &ip) {
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
  oled.println(title);
  for (int i = 0; i < 4; i++) { if (i == editOctet) oled.print('['); oled.print(ip[i]); if (i == editOctet) oled.print(']'); if (i < 3) oled.print('.'); }
  oled.println(); oled.println(F("Turn=change")); oled.println(F("Press=next/save")); oled.display();
}

static void drawEditDhcp() {
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
  oled.println(F("DHCP mode")); oled.println(cfgUseDhcp ? F("ON") : F("OFF")); oled.println(F("Turn=toggle")); oled.println(F("Press=save")); oled.display();
}

static void drawEditPort() {
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
  oled.println(F("TCP SCPI Port")); oled.println(cfgPort); oled.println(F("Turn=change")); oled.println(F("Press=save")); oled.display();
}

void taskSerial(void *) {
  while (true) {
    if (readLine(Serial, serialRx, Serial)) { handleScpiLine(serialRx.buf, Serial, false); resetLine(serialRx); }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void acceptClient() {
  if (!scpiServer) return;
  EthernetClient c = scpiServer->available(); if (!c) return;
  for (int i = 0; i < MAX_ETH_CLIENTS; i++) {
    if (!ethClients[i] || !ethClients[i].connected()) { if (ethClients[i]) ethClients[i].stop(); ethClients[i] = c; resetLine(ethRx[i]); ethClients[i].println(F("ESP32-S3 SCPI TCP ready")); ethClients[i].println(F("4x MCP23017, encoder IP/PORT menu")); ethClients[i].println(F("Send HELP? NET:STAT? MCP:STAT?")); return; }
  }
  c.println(F("ERR: too many clients")); c.stop();
}

void taskEthernet(void *) {
  while (true) {
    if (netRestartRequest) { netRestartRequest = false; initW5500(); }
    Ethernet.maintain(); acceptClient();
    for (int i = 0; i < MAX_ETH_CLIENTS; i++) {
      if (ethClients[i] && ethClients[i].connected()) { if (readLine(ethClients[i], ethRx[i], ethClients[i])) { handleScpiLine(ethRx[i].buf, ethClients[i], true); resetLine(ethRx[i]); } }
      else if (ethClients[i]) { ethClients[i].stop(); resetLine(ethRx[i]); }
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY); currentIp = Ethernet.localIP(); ethReady = Ethernet.hardwareStatus() != EthernetNoHardware && Ethernet.linkStatus() != LinkOFF; xSemaphoreGive(stateMutex);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void taskOled(void *) {
  while (true) {
    if (oledReady) {
      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      if (uiMode == UI_MENU) drawMenu();
      else if (uiMode == UI_EDIT_IP) drawEditIp("Edit IP", cfgIp);
      else if (uiMode == UI_EDIT_GW) drawEditIp("Edit Gateway", cfgGw);
      else if (uiMode == UI_EDIT_MASK) drawEditIp("Edit Mask", cfgMask);
      else if (uiMode == UI_EDIT_DNS) drawEditIp("Edit DNS", cfgDns);
      else if (uiMode == UI_EDIT_DHCP) drawEditDhcp();
      else if (uiMode == UI_EDIT_PORT) drawEditPort();
      else {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        bool er = ethReady; uint32_t sc = serialCount, ec = ethCount; char lines[OLED_CONSOLE_LINES][OLED_CONSOLE_COLS];
        for (int i = 0; i < OLED_CONSOLE_LINES; i++) safeCopy(lines[i], sizeof(lines[i]), oledConsole[i]);
        xSemaphoreGive(stateMutex);
        oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
        oled.print(F("E:")); oled.print(er ? F("OK") : F("NO")); oled.print(F(" M:")); oled.print(mcpReadyCount()); oled.print('/'); oled.print(MCP23017_COUNT); oled.print(F(" P:")); oled.println(cfgPort);
        for (int i = 0; i < OLED_CONSOLE_LINES; i++) oled.println(lines[i]);
        oled.display();
      }
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
  }
}

void taskEncoder(void *) {
  pinMode(ENC_CLK_PIN, INPUT_PULLUP); pinMode(ENC_DT_PIN, INPUT_PULLUP); pinMode(ENC_SW_PIN, INPUT_PULLUP);
  int lastClk = digitalRead(ENC_CLK_PIN);
  int lastSw = digitalRead(ENC_SW_PIN);
  while (true) {
    int clk = digitalRead(ENC_CLK_PIN);
    if (clk != lastClk) {
      if (clk == LOW) {
        int dt = digitalRead(ENC_DT_PIN);
        handleEncoderTurn(dt != clk ? 1 : -1);
      }
      lastClk = clk;
    }
    int sw = digitalRead(ENC_SW_PIN);
    if (lastSw == HIGH && sw == LOW && millis() - lastButtonMs > ENC_BUTTON_DEBOUNCE_MS) { lastButtonMs = millis(); handleEncoderButton(); }
    lastSw = sw;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200); delay(500);
  i2cMutex = xSemaphoreCreateMutex(); stateMutex = xSemaphoreCreateMutex();
  oledConsoleClear(); oledConsolePushRaw("console ready");
  loadNetPrefs();
  psramReady = psramFound() && ESP.getPsramSize() > 0;
  bool ok = allocLineReceiver(serialRx); for (int i = 0; i < MAX_ETH_CLIENTS; i++) ok = allocLineReceiver(ethRx[i]) && ok;
  Serial.println(); Serial.println(F("ESP32-S3 long SCPI GPIO ready"));
  Serial.print(F("SCPI line length: ")); Serial.println(SCPI_LINE_LENGTH);
  Serial.print(F("PSRAM size: ")); Serial.println(ESP.getPsramSize()); Serial.print(F("PSRAM free: ")); Serial.println(ESP.getFreePsram());
  Serial.print(F("RX buffers allocated: ")); Serial.println(ok ? F("OK") : F("FAIL"));
  Serial.println(F("OLED console + rotary encoder IP/PORT menu enabled"));
  if (!ok) { setErr("-300,\"RX buffer allocation failed\""); oledConsolePush("<", "RX alloc fail"); }
  initI2c();
  Serial.print(F("MCP ready: ")); Serial.print(mcpReadyCount()); Serial.print('/'); Serial.println(MCP23017_COUNT);
  Serial.print(F("MCP mask: 0x")); Serial.println(mcpReadyMask(), HEX);
  for (int i = 0; i < MCP23017_COUNT; i++) { Serial.print(F("MCP")); Serial.print(i); Serial.print(F(" addr 0x")); Serial.print(mcpAddr[i], HEX); Serial.println(mcpReady[i] ? F(" OK") : F(" NO")); }
  initW5500();
  Serial.print(F("Ethernet IP: ")); Serial.println(Ethernet.localIP());
  Serial.print(F("SCPI TCP port: ")); Serial.println(cfgPort);
  Serial.println(F("USB Serial and TCP enabled")); Serial.println(F("Send HELP? NET:STAT? MCP:STAT?"));
  xTaskCreatePinnedToCore(taskSerial, "scpi_serial", TASK_STACK_SERIAL, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskEthernet, "scpi_eth", TASK_STACK_ETH, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(taskOled, "oled", TASK_STACK_OLED, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(taskEncoder, "encoder", TASK_STACK_ENCODER, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
