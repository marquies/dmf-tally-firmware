// Sequence numbers are 64-bit on the wire; the ESP32 is a 32-bit target.
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

#include "tally.h"

namespace tally {

namespace {

bool readString(JsonVariantConst v, std::string& out) {
  const char* s = v.as<const char*>();
  if (!v.is<const char*>() || s == nullptr || *s == '\0') return false;
  out = s;
  return true;
}

}  // namespace

Message parse(const char* json, size_t len) {
  Message m;
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return m;
  if (!doc.is<JsonObjectConst>()) return m;
  if (!doc["version"].is<int>() || doc["version"].as<int>() != 1) return m;
  const char* type = doc["type"].as<const char*>();
  if (type == nullptr) return m;

  std::string t = type;
  if (t == "registered") {
    if (!readString(doc["sourceId"], m.sourceId) || !readString(doc["session"], m.session)) {
      return m;
    }
    if (doc["timeoutMs"].is<uint32_t>()) m.timeoutMs = doc["timeoutMs"].as<uint32_t>();
    m.type = MessageType::Registered;
  } else if (t == "tally") {
    if (!readString(doc["sourceId"], m.sourceId) || !readString(doc["session"], m.session) ||
        !doc["sequence"].is<uint64_t>()) {
      return m;
    }
    m.sequence = doc["sequence"].as<uint64_t>();
    // A missing flag reads as false, which errs towards LEDs off.
    m.program = doc["program"].is<bool>() && doc["program"].as<bool>();
    m.preview = doc["preview"].is<bool>() && doc["preview"].as<bool>();
    m.valid = doc["valid"].is<bool>() && doc["valid"].as<bool>();
    m.type = MessageType::Tally;
  } else if (t == "heartbeat") {
    m.type = MessageType::Heartbeat;
  } else if (t == "error") {
    m.type = MessageType::Error;
  } else {
    m.type = MessageType::Unknown;
  }
  return m;
}

std::string registerMessage(const std::string& deviceId, const std::string& sourceId,
                            const std::string& firmware) {
  JsonDocument doc;
  doc["version"] = 1;
  doc["type"] = "register";
  doc["deviceId"] = deviceId.c_str();
  doc["sourceId"] = sourceId.c_str();
  doc["firmware"] = firmware.c_str();
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool validDeviceId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string defaultDeviceId(const uint8_t mac[6]) {
  static const char hex[] = "0123456789abcdef";
  std::string id = "tally-";
  for (int i = 3; i < 6; i++) {
    id += hex[mac[i] >> 4];
    id += hex[mac[i] & 0x0f];
  }
  return id;
}

Leds decide(bool program, bool preview) {
  Leds l;
  l.red = program;
  l.green = preview && !program;
  return l;
}

uint32_t Client::timeoutMs() const {
  if (configTimeoutMs_ > 0) return configTimeoutMs_;
  if (gatewayTimeoutMs_ > 0) return gatewayTimeoutMs_;
  return kDefaultTimeoutMs;
}

void Client::onConnected(uint32_t nowMs) {
  connected_ = true;
  registered_ = false;
  lastRxMs_ = nowMs;
  program_ = preview_ = valid_ = false;
}

void Client::onDisconnected() {
  connected_ = false;
  registered_ = false;
  program_ = preview_ = valid_ = false;
}

void Client::onText(const char* json, size_t len, uint32_t nowMs) {
  Message m = parse(json, len);
  switch (m.type) {
    case MessageType::Registered:
      registered_ = true;
      lastRxMs_ = nowMs;
      gatewayTimeoutMs_ = m.timeoutMs;
      sourceId_ = m.sourceId;
      if (m.session != session_) {
        session_ = m.session;
        haveSequence_ = false;
      }
      // The state shown so far may belong to the previous source; the gateway
      // follows every registered with a tally for the current one.
      program_ = preview_ = valid_ = false;
      break;
    case MessageType::Tally:
      if (!registered_) return;
      lastRxMs_ = nowMs;
      if (m.sourceId != sourceId_) return;
      if (m.session == session_) {
        if (haveSequence_ && m.sequence <= lastSequence_) return;
      } else {
        session_ = m.session;
      }
      haveSequence_ = true;
      lastSequence_ = m.sequence;
      program_ = m.program;
      preview_ = m.preview;
      valid_ = m.valid;
      break;
    case MessageType::Heartbeat:
      if (registered_) lastRxMs_ = nowMs;
      break;
    default:
      break;
  }
}

bool Client::timedOut(uint32_t nowMs) const {
  return connected_ && nowMs - lastRxMs_ > timeoutMs();
}

Leds Client::leds(uint32_t nowMs) const {
  if (!connected_ || !registered_ || !valid_ || timedOut(nowMs)) return Leds();
  return decide(program_, preview_);
}

}  // namespace tally
