#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "tally.h"
#include "version.h"

namespace {

const char* kPath = "/ws/v1";
// Holding GPIO 0 low through reset enters the ROM download mode, so the
// provisioning button is read during a window after boot instead.
const uint32_t kButtonWindowMs = 3000;
const uint32_t kButtonHoldMs = 1000;

struct Config {
  String host;
  uint16_t port = 80;
  bool tls = false;
  String token;
  String deviceId;
  String sourceId;
  uint32_t timeoutS = 30;
  uint8_t pinRed = 25;
  uint8_t pinGreen = 26;
  uint8_t pinButton = 0;
};

Config cfg;
Preferences prefs;
WebSocketsClient ws;
tally::Client* client;
bool wifiUp = false;
bool shouldSave = false;

void loadConfig() {
  prefs.begin("tally", true);
  cfg.host = prefs.getString("host", "");
  cfg.port = prefs.getUShort("port", cfg.port);
  cfg.tls = prefs.getBool("tls", cfg.tls);
  cfg.token = prefs.getString("token", "");
  cfg.deviceId = prefs.getString("device", "");
  cfg.sourceId = prefs.getString("source", "");
  cfg.timeoutS = prefs.getUInt("timeout", cfg.timeoutS);
  cfg.pinRed = prefs.getUChar("pin_red", cfg.pinRed);
  cfg.pinGreen = prefs.getUChar("pin_green", cfg.pinGreen);
  cfg.pinButton = prefs.getUChar("pin_button", cfg.pinButton);
  prefs.end();

  if (cfg.deviceId.isEmpty()) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    cfg.deviceId = tally::defaultDeviceId(mac).c_str();
  }
}

void setLeds(const tally::Leds& l) {
  digitalWrite(cfg.pinRed, l.red ? HIGH : LOW);
  digitalWrite(cfg.pinGreen, l.green ? HIGH : LOW);
}

bool buttonHeld() {
  pinMode(cfg.pinButton, INPUT_PULLUP);
  uint32_t start = millis();
  uint32_t pressedSince = 0;
  bool pressed = false;
  while (millis() - start < kButtonWindowMs) {
    if (digitalRead(cfg.pinButton) == LOW) {
      if (!pressed) pressedSince = millis();
      pressed = true;
      if (millis() - pressedSince >= kButtonHoldMs) return true;
    } else {
      pressed = false;
    }
    delay(10);
  }
  return false;
}

uint8_t pinOr(const char* value, uint8_t fallback, bool output) {
  long v = String(value).toInt();
  if (v < 0 || v > 39) return fallback;
  bool ok = output ? GPIO_IS_VALID_OUTPUT_GPIO(v) : GPIO_IS_VALID_GPIO(v);
  return ok ? v : fallback;
}

// Runs the captive portal until Wi-Fi and the gateway settings are saved,
// then restarts so the device boots straight into the saved configuration.
void provision() {
  Serial.println("provisioning: starting captive portal " + cfg.deviceId);
  WiFiManager wm;
  String port(cfg.port), timeout(cfg.timeoutS), red(cfg.pinRed), green(cfg.pinGreen),
      button(cfg.pinButton);
  WiFiManagerParameter pHost("host", "Gateway host", cfg.host.c_str(), 128);
  WiFiManagerParameter pPort("port", "Gateway port", port.c_str(), 5);
  WiFiManagerParameter pTls("tls", "TLS (1 = on, 0 = off)", cfg.tls ? "1" : "0", 1);
  // Not prefilled: the portal is an open access point.
  WiFiManagerParameter pToken("token", "Auth token (blank keeps the stored one)", "", 256);
  WiFiManagerParameter pDevice("device", "Device ID", cfg.deviceId.c_str(), 64);
  WiFiManagerParameter pSource("source", "Source ID", cfg.sourceId.c_str(), 128);
  WiFiManagerParameter pTimeout("timeout", "Timeout seconds (0 = gateway value)",
                                timeout.c_str(), 5);
  WiFiManagerParameter pRed("pin_red", "Red (program) GPIO", red.c_str(), 2);
  WiFiManagerParameter pGreen("pin_green", "Green (preview) GPIO", green.c_str(), 2);
  WiFiManagerParameter pButton("pin_button", "Provisioning button GPIO", button.c_str(), 2);
  WiFiManagerParameter* params[] = {&pHost,   &pPort,    &pTls, &pToken,   &pDevice,
                                    &pSource, &pTimeout, &pRed, &pGreen, &pButton};
  for (auto* p : params) wm.addParameter(p);
  wm.setSaveConfigCallback([] { shouldSave = true; });
  wm.setBreakAfterConfig(true);
  wm.startConfigPortal(cfg.deviceId.c_str());

  if (shouldSave) {
    prefs.begin("tally", false);
    prefs.putString("host", pHost.getValue());
    long p = String(pPort.getValue()).toInt();
    if (p > 0 && p <= 65535) prefs.putUShort("port", p);
    prefs.putBool("tls", String(pTls.getValue()) == "1");
    if (strlen(pToken.getValue()) > 0) prefs.putString("token", pToken.getValue());
    if (tally::validDeviceId(pDevice.getValue())) prefs.putString("device", pDevice.getValue());
    prefs.putString("source", pSource.getValue());
    long t = String(pTimeout.getValue()).toInt();
    if (t >= 0) prefs.putUInt("timeout", t);
    prefs.putUChar("pin_red", pinOr(pRed.getValue(), cfg.pinRed, true));
    prefs.putUChar("pin_green", pinOr(pGreen.getValue(), cfg.pinGreen, true));
    prefs.putUChar("pin_button", pinOr(pButton.getValue(), cfg.pinButton, false));
    prefs.end();
  }
  ESP.restart();
}

void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.println("ws: connected, registering");
      client->onConnected(millis());
      std::string reg = tally::registerMessage(cfg.deviceId.c_str(), cfg.sourceId.c_str(),
                                               FIRMWARE_VERSION);
      ws.sendTXT(reg.c_str(), reg.size());
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("ws: disconnected");
      client->onDisconnected();
      break;
    case WStype_TEXT: {
      std::string before = client->sourceId();
      client->onText(reinterpret_cast<const char*>(payload), length, millis());
      if (client->sourceId() != before) {
        Serial.printf("ws: assigned source %s\n", client->sourceId().c_str());
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.printf("dmf-tally-firmware %s\n", FIRMWARE_VERSION);
  loadConfig();
  pinMode(cfg.pinRed, OUTPUT);
  pinMode(cfg.pinGreen, OUTPUT);
  setLeds(tally::Leds());

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  if (buttonHeld() || cfg.host.isEmpty() || cfg.sourceId.isEmpty() || !wm.getWiFiIsSaved()) {
    provision();
  }

  client = new tally::Client(cfg.timeoutS * 1000);
  WiFi.setAutoReconnect(true);
  WiFi.begin();

  if (!cfg.token.isEmpty()) ws.setExtraHeaders(("Authorization: Bearer " + cfg.token).c_str());
  if (cfg.tls) {
    ws.beginSSL(cfg.host.c_str(), cfg.port, kPath);
  } else {
    ws.begin(cfg.host.c_str(), cfg.port, kPath);
  }
  ws.onEvent(onEvent);
  ws.setReconnectInterval(2000);
  Serial.printf("device %s, source %s, gateway %s:%u\n", cfg.deviceId.c_str(),
                cfg.sourceId.c_str(), cfg.host.c_str(), cfg.port);
}

void loop() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wifiUp) {
    Serial.println(up ? "wifi: connected" : "wifi: lost");
    wifiUp = up;
    if (!up) {
      ws.disconnect();
      client->onDisconnected();
    }
  }
  if (up) {
    ws.loop();
    if (client->timedOut(millis())) {
      Serial.println("ws: timeout, reconnecting");
      ws.disconnect();
      client->onDisconnected();
    }
  }
  setLeds(client->leds(millis()));
}
