#include <Arduino.h>
#include <esp_system.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include "VideoFrame.h"

// Ajustar estos pines al cableado real. TTP223 entrega HIGH al tocar.
constexpr uint8_t OLED_SDA = 21;
constexpr uint8_t OLED_SCL = 22;
constexpr uint8_t TOUCH_PIN = 13;
constexpr uint8_t OLED_RESET = -1;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint16_t SCREEN_W = 128, SCREEN_H = 64;
constexpr uint32_t GREETING_WINDOW_MS = 1500;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 45;
constexpr uint8_t TRIPLE_TAP_COUNT = 3;
constexpr char AP_SSID[] = "CajitaMSG";
constexpr char AP_PASSWORD[] = ""; // Red abierta; asigna una clave si deseas protegerla.

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);
Preferences prefs;
WebServer server(80);
String messages[40];
size_t messageCount = 0;
size_t messageOrder[40];
size_t messageOrderCursor = 0;
int lastMessageIndex = -1;
bool touchWasDown = false;
uint32_t lastTouchMs = 0;
uint32_t tapWindowStart = 0;
uint8_t tapCount = 0;
bool configWindow = false;
bool configMode = false;

void shuffleMessageOrder() {
  for (size_t i = 0; i < messageCount; ++i) messageOrder[i] = i;
  for (size_t i = messageCount; i > 1; --i) {
    size_t j = static_cast<size_t>(random(i));
    size_t temp = messageOrder[i - 1];
    messageOrder[i - 1] = messageOrder[j];
    messageOrder[j] = temp;
  }
  // Evita que el ultimo mensaje de un ciclo se repita al comenzar el siguiente.
  if (messageCount > 1 && lastMessageIndex >= 0 && messageOrder[0] == static_cast<size_t>(lastMessageIndex)) {
    size_t j = 1 + static_cast<size_t>(random(messageCount - 1));
    size_t temp = messageOrder[0];
    messageOrder[0] = messageOrder[j];
    messageOrder[j] = temp;
  }
  messageOrderCursor = 0;
}

size_t nextMessageIndex() {
  if (messageOrderCursor >= messageCount) shuffleMessageOrder();
  size_t index = messageOrder[messageOrderCursor++];
  lastMessageIndex = static_cast<int>(index);
  return index;
}
String oledCompatibleText(const String &input) {
  String out;
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    if (c == 0xC3 && i + 1 < input.length()) {
      uint8_t next = static_cast<uint8_t>(input[i + 1]);
      if (next == 0xA1 || next == 0xA9 || next == 0xAD || next == 0xB3 || next == 0xBA) {
        out += (next == 0xA1) ? 'a' : (next == 0xA9) ? 'e' : (next == 0xAD) ? 'i' : (next == 0xB3) ? 'o' : 'u';
        ++i; continue;
      }
      if (next == 0x81 || next == 0x89 || next == 0x8D || next == 0x93 || next == 0x9A) {
        out += (next == 0x81) ? 'A' : (next == 0x89) ? 'E' : (next == 0x8D) ? 'I' : (next == 0x93) ? 'O' : 'U';
        ++i; continue;
      }
      if (next == 0xB1) { out += 'n'; ++i; continue; }
      if (next == 0x91) { out += 'N'; ++i; continue; }
    }
    out += static_cast<char>(c);
  }
  return out;
}

uint16_t textPixelWidth(const String &text, uint8_t size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w;
}

bool wrapMessage(const String &text, uint8_t size, String lines[], uint8_t &lineCount) {
  lineCount = 0;
  String current;
  const uint16_t maxWidth = SCREEN_W - 4;
  const uint8_t maxChars = max<uint8_t>(1, maxWidth / (6 * size));
  int pos = 0;
  while (pos < text.length()) {
    while (pos < text.length() && text[pos] == ' ') ++pos;
    if (pos >= text.length()) break;
    int end = text.indexOf(' ', pos);
    if (end < 0) end = text.length();
    String word = text.substring(pos, end);
    pos = end + 1;
    while (word.length()) {
      String part = word;
      if (part.length() > maxChars) part = word.substring(0, maxChars);
      if (current.isEmpty()) {
        current = part;
      } else if (textPixelWidth(current + " " + part, size) <= maxWidth) {
        current += " " + part;
      } else {
        if (lineCount >= 16) return false;
        lines[lineCount++] = current;
        current = part;
      }
      word.remove(0, part.length());
      if (word.length()) {
        if (lineCount >= 16) return false;
        lines[lineCount++] = current;
        current = "";
      }
    }
  }
  if (!current.isEmpty()) {
    if (lineCount >= 16) return false;
    lines[lineCount++] = current;
  }
  return lineCount > 0 && lineCount * 8 * size <= SCREEN_H - 4;
}

void drawCentered(const String &text) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  String normalized = oledCompatibleText(text);
  String lines[16];
  uint8_t lineCount = 0, chosenSize = 1;
  for (int size = 3; size >= 1; --size) {
    if (wrapMessage(normalized, size, lines, lineCount)) {
      chosenSize = size;
      break;
    }
  }
  display.setTextSize(chosenSize);
  int16_t y = (SCREEN_H - lineCount * 8 * chosenSize) / 2;
  for (uint8_t i = 0; i < lineCount; ++i) {
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &h);
    display.setCursor(max<int>(0, (SCREEN_W - w) / 2), y);
    display.print(lines[i]);
    y += 8 * chosenSize;
  }
  display.display();
}
void drawGreetingProgress(uint8_t filled) {
  // Usa el mismo ajuste y centrado que el saludo normal para que no salte al tocar.
  drawCentered("Hola -custom name- <3");
  const int16_t firstX = 54;
  for (uint8_t i = 0; i < TRIPLE_TAP_COUNT; ++i) {
    int16_t x = firstX + i * 10;
    if (i < filled) display.fillCircle(x, 53, 3, SSD1306_WHITE);
    else display.drawCircle(x, 53, 3, SSD1306_WHITE);
  }
  display.display();
}
void drawConfigInfo() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 18); display.println("RED: CajitaMSG");
  display.setCursor(0, 34); display.println("IP: 192.168.4.1");
  display.display();
}

void saveMessages() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < messageCount; ++i) arr.add(messages[i]);
  String json; serializeJson(doc, json);
  prefs.putString("messages", json);
}

void loadMessages() {
  String json = prefs.getString("messages", "");
  if (json.isEmpty()) {
    messages[0] = "Te quiero mucho";
    messages[1] = "Eres mi persona favorita";
    messageCount = 2;
    saveMessages();
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (messageCount == 40) break;
    String value = v.as<String>(); value.trim();
    if (!value.isEmpty()) messages[messageCount++] = value;
  }
}

String htmlEscape(const String &s) {
  String out; out.reserve(s.length());
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

void handleRoot() {
  String page = F("<!doctype html><html lang='es'><meta name='viewport' content='width=device-width,initial-scale=1'><meta charset='utf-8'><title>TheBox</title><style>body{font:16px sans-serif;max-width:600px;margin:2em auto;padding:0 1em}input{padding:.7em;width:70%}button{padding:.7em}li{margin:.8em 0}</style><h1>Mensajes de TheBox</h1><form method='post' action='/add'><input name='message' maxlength='120' required placeholder='Escribe un mensaje'><button>Agregar</button></form><ol>");
  for (size_t i = 0; i < messageCount; ++i) {
    page += "<li>" + htmlEscape(messages[i]) + " <form style='display:inline' method='post' action='/delete'><input type='hidden' name='i' value='" + String(i) + "'><button>Eliminar</button></form></li>";
  }
  page += F("</ol><p>Los cambios se guardan en el ESP32. Reinicia la cajita para salir del modo configuración.</p></html>");
  server.send(200, "text/html; charset=utf-8", page);
}

void handleAdd() {
  String value = server.arg("message"); value.trim();
  if (!value.isEmpty() && messageCount < 40) messages[messageCount++] = value;
  saveMessages(); server.sendHeader("Location", "/"); server.send(303);
}

void handleDelete() {
  int i = server.arg("i").toInt();
  if (i >= 0 && static_cast<size_t>(i) < messageCount) {
    for (size_t j = i; j + 1 < messageCount; ++j) messages[j] = messages[j + 1];
    --messageCount;
    saveMessages();
  }
  server.sendHeader("Location", "/"); server.send(303);
}

void startConfigMode() {
  configMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/delete", HTTP_POST, handleDelete);
  server.begin();
  drawConfigInfo();
}

void playIntro() {
  constexpr size_t frameBytes = 1024;
  for (int i = 0; i < TOTAL_FRAMES; ++i) {
    display.clearDisplay();
    display.drawBitmap(0, 0, video_frames[i], SCREEN_W, SCREEN_H, SSD1306_WHITE);
    display.display();
    delay(FRAME_DELAY);
  }
}

bool touchPressed() {
  bool down = digitalRead(TOUCH_PIN) == HIGH;
  uint32_t now = millis();
  bool pressed = down && !touchWasDown && now - lastTouchMs >= TOUCH_DEBOUNCE_MS;
  if (pressed) lastTouchMs = now;
  touchWasDown = down;
  return pressed;
}

void setup() {
  pinMode(TOUCH_PIN, INPUT);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) while (true) delay(1000);
  prefs.begin("thebox", false);
  loadMessages();
  randomSeed(esp_random());
  shuffleMessageOrder();
  playIntro();
  drawCentered("para la más linda");
  delay(1500);
  drawCentered("Hola Sami <3");
  configWindow = true;
  tapWindowStart = millis();
}

void loop() {
  if (configMode) { server.handleClient(); return; }
  uint32_t now = millis();
  if (configWindow && now - tapWindowStart > GREETING_WINDOW_MS) {
    configWindow = false; tapCount = 0;
  }
  if (!touchPressed()) return;
  if (configWindow) {
    ++tapCount;
    drawGreetingProgress(tapCount);
    if (tapCount >= TRIPLE_TAP_COUNT && now - tapWindowStart <= GREETING_WINDOW_MS) {
      startConfigMode();
      return;
    }
    return;
  }
  if (messageCount) {
    drawCentered(messages[nextMessageIndex()]);
  }
}
