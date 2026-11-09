// ESP32 PC Wake v4 (ESP32-WROOM-32, Arduino-ESP32 core 3.x, ArduinoJson 7.x)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h> // TELEGRAM_CERTIFICATE_ROOT
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <time.h>
#include "config.h"
#include "secrets.h"

constexpr uint32_t LONG_POLL_SEC = 20;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t WDT_TIMEOUT_MS = 60000;
constexpr uint32_t RESTART_AFTER_MS = 300000;
constexpr uint32_t WAKE_COOLDOWN_MS = 30000;
constexpr uint32_t BOOT_CHECK_INTERVAL_MS = 5000;
constexpr uint32_t BOOT_CHECK_MAX_MS = 90000;
constexpr uint32_t HEALTH_LOG_MS = 60000;
constexpr uint32_t OUTBOX_RETRY_MS = 10000;
constexpr uint32_t OUTBOX_MAX_AGE_MS = 180000;
constexpr int64_t MAX_COMMAND_AGE_SEC = 90;

const int64_t OWNER_ID = strtoll(AUTHORIZED_USER_ID, nullptr, 10);
const String API_URL = String("https://api.telegram.org/bot") + BOT_TOKEN + "/";

WiFiClientSecure tls;
HTTPClient http;
WiFiUDP udp;
Preferences prefs;
JsonDocument updateFilter;
IPAddress pcIp;
bool pcCheckEnabled = false;

uint64_t nextOffset = 0;
uint32_t nextPollAt = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastHealthLog = 0;
uint32_t failingSince = 0;
uint32_t failStreak = 0;
uint32_t failTotal = 0;
uint32_t lastFailAt = 0;
uint32_t lastWakeAt = 0;
uint32_t bootCheckStartedAt = 0;
uint32_t lastBootCheckAt = 0;
bool wifiUp = false;
bool wakeSent = false;
bool bootCheckActive = false;
String outbox;
uint32_t outboxSince = 0;
uint32_t outboxNextTry = 0;

bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

bool clockValid() { return time(nullptr) >= 1700000000; }

String durationText(uint32_t s) {
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h " + String(s / 60 % 60) + "m";
  return String(s / 86400) + "d " + String(s / 3600 % 24) + "h";
}

String uptimeText() { return durationText(esp_timer_get_time() / 1000000ULL); }

bool rdpReachable() {
  if (!pcCheckEnabled || WiFi.status() != WL_CONNECTED) return false;
  WiFiClient tcp;
  bool ok = tcp.connect(pcIp, PC_RDP_PORT, 1000);
  tcp.stop();
  return ok;
}

bool sendWol() {
  if (WiFi.status() != WL_CONNECTED) return false;
  uint8_t packet[102];
  memset(packet, 0xff, 6);
  for (int i = 0; i < 16; i++) memcpy(packet + 6 + i * 6, PC_MAC, 6);
  IPAddress broadcast = WiFi.broadcastIP();
  bool sent = udp.beginPacket(broadcast, WOL_PORT) &&
              udp.write(packet, sizeof(packet)) == sizeof(packet) &&
              udp.endPacket();
  Serial.printf("WoL %s, broadcast=%s\n", sent ? "sent" : "failed", broadcast.toString().c_str());
  return sent;
}

// telegram sends Content-Length, but a chunked reply still works through getString
DeserializationError readUpdates(JsonDocument &reply) {
  auto filter = DeserializationOption::Filter(updateFilter);
  if (http.getSize() >= 0) return deserializeJson(reply, http.getStream(), filter);
  String raw = http.getString();
  return deserializeJson(reply, raw, filter);
}

// the TLS connection is kept open between calls, so most calls skip the handshake.
// network errors are retried a few times, because the kept connection can be dropped anytime.
bool callFailed() {
  failTotal++;
  lastFailAt = millis();
  return false;
}

bool telegramCall(const char *method, const JsonDocument &body, uint32_t timeoutMs,
                  JsonDocument *reply = nullptr) {
  String payload;
  serializeJson(body, payload);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) delay(attempt * 1000);
    if (WiFi.status() != WL_CONNECTED) return false;
    esp_task_wdt_reset();
    if (!http.begin(tls, API_URL + method)) return callFailed();
    http.setTimeout(timeoutMs);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    bool ok = code == 200;
    if (ok && reply) {
      DeserializationError err = readUpdates(*reply);
      ok = !err && (*reply)["ok"] == true;
    } else if (ok) {
      http.getString(); // read the body so the connection is clean for the next call
    }
    http.end();
    if (ok) return true;
    Serial.printf("telegram %s failed, http=%d\n", method, code);
    tls.stop();
    if (code > 0) break;
  }
  return callFailed();
}

bool postMessage(const String &text) {
  JsonDocument body;
  body["chat_id"] = OWNER_ID;
  body["text"] = text;
  return telegramCall("sendMessage", body, HTTP_TIMEOUT_MS);
}

// a message that can't be sent goes to the outbox and is retried later, so replies aren't lost
void sendMessage(const String &text) {
  if (outbox.length()) {
    outbox += "\n\n" + text; // keep the order behind the waiting message
    return;
  }
  if (postMessage(text)) return;
  outbox = text;
  outboxSince = millis();
  outboxNextTry = millis() + OUTBOX_RETRY_MS;
}

void flushOutbox() {
  if (!outbox.length() || !wifiUp || !due(outboxNextTry)) return;
  if (postMessage(outbox)) {
    outbox = "";
  } else if (millis() - outboxSince >= OUTBOX_MAX_AGE_MS) {
    Serial.println("outbox dropped after 3m");
    outbox = "";
  } else {
    outboxNextTry = millis() + OUTBOX_RETRY_MS;
  }
}

void startBootCheck() {
  bootCheckActive = true;
  bootCheckStartedAt = lastBootCheckAt = millis();
}

void wakeCommand() {
  if (rdpReachable()) {
    sendMessage("PC already online (RDP reachable).");
    return;
  }
  if (wakeSent && millis() - lastWakeAt < WAKE_COOLDOWN_MS) {
    sendMessage("Wake recently requested. Please try again shortly.");
    return;
  }
  if (!sendWol()) {
    sendMessage("Failed to send Wake-on-LAN packet.");
    return;
  }
  wakeSent = true;
  lastWakeAt = millis();
  sendMessage("Wake-on-LAN packet sent. Boot is not yet confirmed.");
  if (pcCheckEnabled) startBootCheck();
}

String statusText() {
  String s = "ESP32 online\n";
  s += "Wi-Fi: " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() + "\n";
  s += "Uptime: " + uptimeText() + "\n";
  s += "Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  if (failTotal) s += "Telegram: " + String(failTotal) + " failed calls, last " + durationText((millis() - lastFailAt) / 1000) + " ago\n";
  else s += "Telegram: no failed calls\n";
  if (!pcCheckEnabled) s += "PC: RDP check not configured";
  else s += rdpReachable() ? "PC: RDP reachable" : "PC: RDP unreachable (power unknown)";
  return s;
}

void handleCommand(const String &cmd) {
  if (cmd == "/wake") wakeCommand();
  else if (cmd == "/status") sendMessage(statusText());
  else sendMessage("/wake - Send WoL packet\n/status - Check ESP32 and PC\n/help - Commands");
}

void pollFailed(const char *reason) {
  failStreak++;
  if (!failingSince) failingSince = millis();
  Serial.printf("poll failed (%s), streak=%lu\n", reason, (unsigned long)failStreak);
  if (millis() - failingSince >= RESTART_AFTER_MS) {
    Serial.println("no telegram for 5m, restarting");
    delay(100);
    ESP.restart();
  }
  nextPollAt = millis() + (1000UL << (failStreak < 4 ? failStreak : 4));
}

void pollTelegram() {
  if (!wifiUp || !due(nextPollAt)) return;
  if (!clockValid()) return pollFailed("clock not synced");

  // shorter long poll while waiting for the PC, so the boot check runs every few seconds
  uint32_t pollSec = bootCheckActive ? BOOT_CHECK_INTERVAL_MS / 1000 : LONG_POLL_SEC;
  JsonDocument body;
  body["offset"] = nextOffset;
  body["timeout"] = pollSec;
  body["limit"] = 1;
  body["allowed_updates"][0] = "message";
  JsonDocument reply;
  if (!telegramCall("getUpdates", body, HTTP_TIMEOUT_MS + pollSec * 1000, &reply)) return pollFailed("request");

  failStreak = 0;
  failingSince = 0;
  nextPollAt = millis();
  JsonObject update = reply["result"][0];
  if (update.isNull()) return;
  nextOffset = update["update_id"].as<uint64_t>() + 1;

  JsonObject msg = update["message"];
  if (msg["from"]["id"] != OWNER_ID || msg["chat"]["id"] != OWNER_ID) return;
  int64_t age = (int64_t)time(nullptr) - msg["date"].as<int64_t>();
  if (age < -10 || age > MAX_COMMAND_AGE_SEC) return;

  // saved before running the command, so a crash can't run it again after reboot
  prefs.putULong64("offset", nextOffset);
  String cmd = msg["text"] | "";
  cmd.trim();
  handleCommand(cmd);
}

void checkBootProgress() {
  if (!bootCheckActive || !wifiUp || millis() - lastBootCheckAt < BOOT_CHECK_INTERVAL_MS) return;
  lastBootCheckAt = millis();
  if (rdpReachable()) {
    bootCheckActive = false;
    sendMessage("PC ready: RDP reachable.");
  } else if (millis() - bootCheckStartedAt >= BOOT_CHECK_MAX_MS) {
    bootCheckActive = false;
    sendMessage("WoL sent, but RDP is still unreachable after 90 seconds.");
  }
}

void wifiMaintain() {
  bool online = WiFi.status() == WL_CONNECTED;
  if (online != wifiUp) {
    wifiUp = online;
    if (online) {
      Serial.printf("wifi connected: %s\n", WiFi.localIP().toString().c_str());
      nextPollAt = millis();
    } else {
      Serial.println("wifi lost");
      tls.stop();
    }
  }
  if (!online && millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setupWatchdog() {
  esp_task_wdt_config_t config = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  // the core usually starts the watchdog already, so reconfigure first
  esp_err_t status = esp_task_wdt_reconfigure(&config);
  if (status == ESP_ERR_INVALID_STATE) status = esp_task_wdt_init(&config);
  if (status != ESP_OK || esp_task_wdt_add(NULL) != ESP_OK) {
    Serial.println("failed to set up watchdog, restarting");
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nESP32 PC Wake v4 | reset reason=%d\n", (int)esp_reset_reason());
  setupWatchdog();

  prefs.begin("pcwake", false);
  nextOffset = prefs.getULong64("offset", 0);
  pcCheckEnabled = pcIp.fromString(PC_LAN_IP) && pcIp != IPAddress(0, 0, 0, 0);

  tls.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  tls.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);
  http.setReuse(true);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);

  updateFilter["ok"] = true;
  JsonObject f = updateFilter["result"][0].to<JsonObject>();
  f["update_id"] = true;
  f["message"]["from"]["id"] = true;
  f["message"]["chat"]["id"] = true;
  f["message"]["date"] = true;
  f["message"]["text"] = true;

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("wifi disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // modem sleep can delay or drop incoming packets
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void loop() {
  esp_task_wdt_reset();
  wifiMaintain();
  pollTelegram();
  flushOutbox();
  checkBootProgress();
  if (millis() - lastHealthLog >= HEALTH_LOG_MS) {
    lastHealthLog = millis();
    Serial.printf("[health] uptime=%s rssi=%d heap=%lu fails=%lu\n", uptimeText().c_str(),
      wifiUp ? WiFi.RSSI() : 0, (unsigned long)ESP.getFreeHeap(), (unsigned long)failTotal);
  }
  delay(20);
}
