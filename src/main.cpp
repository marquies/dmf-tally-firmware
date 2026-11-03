#include <Arduino.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <WebSocketsClient.h>
#include <ESPmDNS.h>
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
  uint16_t port = 8080;
  bool tls = false;
  String token;
  String deviceId;
  String sourceId;
  uint32_t timeoutS = 30;
#ifdef CONFIG_IDF_TARGET_ESP32C3
  // C3 Super Mini: LEDs on GPIO 0/1, BOOT button on GPIO 9.
  uint8_t pinRed = 0;
  uint8_t pinGreen = 1;
  uint8_t pinButton = 9;
#else
  uint8_t pinRed = 25;
  uint8_t pinGreen = 26;
  uint8_t pinButton = 0;
#endif
};

Config cfg;
Preferences prefs;
WebSocketsClient ws;
tally::Client* client;
bool wifiUp = false;
// Gateway host and source ID are both set; without them only Wi-Fi and the
// settings page run, and the LEDs stay off.
bool gatewaySet = false;
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

#ifdef CONFIG_IDF_TARGET_ESP32C3
// C3 Super Mini: the mismatched antenna distorts at full TX power (the AP
// becomes invisible), and modem sleep delays the auth handshake on some
// routers until it expires (FritzBox: AUTH_EXPIRE). The country follows the
// AP's beacons so channels 12/13 are usable where allowed.
void tuneWifi() {
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_country_code("01", true);
}
#endif

#ifdef SIM_WOKWI
// Simulator defaults: a gateway on the host reached through wokwigw.
#ifndef SIM_WOKWI_HOST
#define SIM_WOKWI_HOST "host.wokwi.internal"
#endif
#ifndef SIM_WOKWI_PORT
#define SIM_WOKWI_PORT 8081
#endif
#ifndef SIM_WOKWI_TLS
#define SIM_WOKWI_TLS 0
#endif
#ifndef SIM_WOKWI_SOURCE
#define SIM_WOKWI_SOURCE "sim-source"
#endif
// LED pins of the simulated board (ESP32-C3 in diagram.json).
#ifndef SIM_WOKWI_PIN_RED
#define SIM_WOKWI_PIN_RED 0
#endif
#ifndef SIM_WOKWI_PIN_GREEN
#define SIM_WOKWI_PIN_GREEN 1
#endif
#endif  // SIM_WOKWI

#ifndef SIM_WOKWI
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

// WiFiManager registers firmware upload, erase, restart, exit and info routes
// whatever the menu shows. The web server answers with the first handler that
// matches, so claiming them before the library registers its own turns them
// into 404s.
void blockMaintenanceRoutes(WiFiManager& wm) {
  wm.setWebServerCallback([&wm] {
    for (const char* uri : {"/update", "/u", "/erase", "/restart", "/exit", "/close", "/info"}) {
      wm.server->on(uri, [&wm] { wm.server->send(404, "text/plain", "Not found"); });
    }
  });
}

// The settings shared by the provisioning portal and the station-mode page.
// Not prefilled with the token: the pages are served without authentication.
struct SettingsForm {
  String port, timeout, red, green, button;
  WiFiManagerParameter pHost, pPort, pTls, pToken, pDevice, pSource, pTimeout, pRed, pGreen,
      pButton;

  SettingsForm()
      : port(cfg.port),
        timeout(cfg.timeoutS),
        red(cfg.pinRed),
        green(cfg.pinGreen),
        button(cfg.pinButton),
        pHost("host", "Gateway host", cfg.host.c_str(), 128),
        pPort("port", "Gateway port", port.c_str(), 5),
        pTls("tls", "TLS (1 = on, 0 = off)", cfg.tls ? "1" : "0", 1),
        pToken("token", "Auth token (blank keeps the stored one)", "", 256),
        pDevice("device", "Device ID", cfg.deviceId.c_str(), 64),
        pSource("source", "Source ID", cfg.sourceId.c_str(), 128),
        pTimeout("timeout", "Timeout seconds (0 = gateway value)", timeout.c_str(), 5),
        pRed("pin_red", "Red (program) GPIO", red.c_str(), 2),
        pGreen("pin_green", "Green (preview) GPIO", green.c_str(), 2),
        pButton("pin_button", "Provisioning button GPIO", button.c_str(), 2) {}

  void attach(WiFiManager& wm) {
    WiFiManagerParameter* params[] = {&pHost,   &pPort,    &pTls, &pToken,   &pDevice,
                                      &pSource, &pTimeout, &pRed, &pGreen,   &pButton};
    for (auto* p : params) wm.addParameter(p);
  }

  void save() {
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
};

// Runs the captive portal until the settings are saved, then restarts so the
// device boots straight into the saved configuration.
void provision() {
  Serial.println("provisioning: starting captive portal " + cfg.deviceId);
  WiFiManager wm;
  SettingsForm form;
  form.attach(wm);
  blockMaintenanceRoutes(wm);
  // The credentials are read inside the callback: after startConfigPortal
  // returns the library has already disabled STA and cleared them. They come
  // from the STA config (true): the non-persistent getter reports the AP the
  // station is connected to, which is empty when the in-portal attempt failed.
  static String ssid, pass;
  wm.setSaveConfigCallback([&wm] {
    shouldSave = true;
    ssid = wm.getWiFiSSID(true);
    pass = wm.getWiFiPass(true);
  });
  wm.setBreakAfterConfig(true);
#ifdef CONFIG_IDF_TARGET_ESP32C3
  wm.setAPCallback([](WiFiManager*) { tuneWifi(); });
  // The in-portal attempt rarely succeeds on one radio; keep it short, the
  // clean STA-only attempt below is the one that counts.
  wm.setConnectTimeout(10);
  wm.setConnectRetries(1);
#endif
  wm.startConfigPortal(cfg.deviceId.c_str());

  if (shouldSave) {
    form.save();
    // The portal's own connect shares the single radio with the still-running
    // AP and the handshake starves (AUTH_EXPIRE/TIMEOUT). With the portal
    // gone, retry STA-only; begin() persists the credentials either way.
    WiFi.mode(WIFI_STA);
#ifdef CONFIG_IDF_TARGET_ESP32C3
    tuneWifi();
#endif
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    bool ok = WiFi.waitForConnectResult(30000) == WL_CONNECTED;
    Serial.println(ok ? "provisioning: wifi connected" : "provisioning: wifi failed");
  }
  ESP.restart();
}

// Serves the same settings on the LAN address once Wi-Fi is up. Saving
// persists them and schedules a restart; the delay lets the response page
// reach the browser first.
const uint32_t kRestartDelayMs = 1500;
WiFiManager* webWm;
SettingsForm* webForm;
uint32_t restartAt = 0;

void scheduleRestart() {
  setLeds(tally::Leds());
  restartAt = millis() + kRestartDelayMs;
}

// DNS labels allow neither '.' nor '_', both of which a device ID may hold.
String mdnsName() {
  String name = cfg.deviceId;
  name.replace('.', '-');
  name.replace('_', '-');
  return name;
}

void startSettingsPage() {
  webWm = new WiFiManager();
  webForm = new SettingsForm();
  webForm->attach(*webWm);
  blockMaintenanceRoutes(*webWm);
  // Parameters get their own page; the Wi-Fi page scans, which blocks.
  static const char* menu[] = {"param", "wifi"};
  webWm->setMenu(menu, 2);
  webWm->setSaveParamsCallback([] {
    webForm->save();
    scheduleRestart();
  });
  webWm->setSaveConfigCallback(scheduleRestart);
  webWm->startWebPortal();
  String name = mdnsName();
  if (MDNS.begin(name.c_str())) MDNS.addService("http", "tcp", 80);
  Serial.printf("settings: http://%s/ and http://%s.local/\n",
                WiFi.localIP().toString().c_str(), name.c_str());
}
#endif  // SIM_WOKWI

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
      Serial.printf("ws: disconnected: %.*s\n", static_cast<int>(length),
                    reinterpret_cast<const char*>(payload));
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
#ifdef SIM_WOKWI
  cfg.pinRed = SIM_WOKWI_PIN_RED;
  cfg.pinGreen = SIM_WOKWI_PIN_GREEN;
#endif
  pinMode(cfg.pinRed, OUTPUT);
  pinMode(cfg.pinGreen, OUTPUT);
  setLeds(tally::Leds());

  // Boot self-test: each LED lights for one second, red then green, so a
  // wiring or pin-config problem is visible without a gateway.
  digitalWrite(cfg.pinRed, HIGH);
  delay(1000);
  digitalWrite(cfg.pinRed, LOW);
  digitalWrite(cfg.pinGreen, HIGH);
  delay(1000);
  digitalWrite(cfg.pinGreen, LOW);

  WiFi.mode(WIFI_STA);
#ifdef CONFIG_IDF_TARGET_ESP32C3
  tuneWifi();
#endif
#ifdef SIM_WOKWI
  // Simulator build: no captive portal, fixed Wokwi network and gateway.
  cfg.host = SIM_WOKWI_HOST;
  cfg.port = SIM_WOKWI_PORT;
  cfg.tls = SIM_WOKWI_TLS;
  cfg.sourceId = SIM_WOKWI_SOURCE;
  cfg.token = SIM_WOKWI_TOKEN;
#else
  // A missing gateway setting does not reopen the portal: once Wi-Fi is
  // saved, the settings page on the LAN is where it gets filled in.
  WiFiManager wm;
  if (buttonHeld() || !wm.getWiFiIsSaved()) {
    provision();
  }
#endif
  gatewaySet = !cfg.host.isEmpty() && !cfg.sourceId.isEmpty();

  client = new tally::Client(cfg.timeoutS * 1000);
  WiFi.setAutoReconnect(true);
#ifdef SIM_WOKWI
  WiFi.begin("Wokwi-GUEST", "", 6);
#else
  WiFi.begin();
#endif

  if (!gatewaySet) {
    Serial.println("gateway: host or source ID missing, settings page only");
    return;
  }

  // Always set: the library's default "Origin: file://" is rejected by the
  // gateway with 403 because the origin has no host.
  ws.setExtraHeaders(cfg.token.isEmpty() ? "" : ("Authorization: Bearer " + cfg.token).c_str());
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
#ifndef SIM_WOKWI
    if (up && !webWm) startSettingsPage();
#endif
    if (!up) {
      ws.disconnect();
      client->onDisconnected();
    }
  }
  if (up && gatewaySet) {
    ws.loop();
    if (client->timedOut(millis())) {
      Serial.println("ws: timeout, reconnecting");
      ws.disconnect();
      client->onDisconnected();
    }
  }
#ifndef SIM_WOKWI
  if (webWm) webWm->process();
  if (restartAt) {
    if (static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();
    setLeds(tally::Leds());
    return;
  }
#endif
  setLeds(client->leds(millis()));
}
