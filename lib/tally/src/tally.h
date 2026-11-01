// Client side of the tally WebSocket protocol v1, free of any Arduino
// dependency so it runs in the native test environment. The firmware feeds
// it connection events, received frames and the current millisecond clock,
// and reads back which LEDs to light.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace tally {

const uint32_t kDefaultTimeoutMs = 30000;

enum class MessageType { Invalid, Registered, Tally, Heartbeat, Error, Unknown };

struct Message {
  MessageType type = MessageType::Invalid;
  std::string sourceId;
  std::string session;
  bool program = false;
  bool preview = false;
  bool valid = false;
  uint64_t sequence = 0;
  uint32_t timeoutMs = 0;
};

// Parses one text frame. Anything that is not a JSON object carrying
// "version": 1, or that lacks a field its type needs, comes back Invalid.
Message parse(const char* json, size_t len);

std::string registerMessage(const std::string& deviceId, const std::string& sourceId,
                            const std::string& firmware);

// deviceId as the gateway accepts it: [A-Za-z0-9._-]{1,64}.
bool validDeviceId(const std::string& id);

// "tally-" followed by the last three MAC bytes in lower-case hex.
std::string defaultDeviceId(const uint8_t mac[6]);

struct Leds {
  bool red = false;
  bool green = false;
};

// Program wins over preview, so both lit shows red only.
Leds decide(bool program, bool preview);

class Client {
 public:
  // configTimeoutMs of 0 defers to the gateway's timeoutMs, then to 30 s.
  explicit Client(uint32_t configTimeoutMs) : configTimeoutMs_(configTimeoutMs) {}

  void onConnected(uint32_t nowMs);
  void onDisconnected();
  void onText(const char* json, size_t len, uint32_t nowMs);

  // True once nothing arrived for longer than the timeout on an open socket;
  // the caller drops the connection, which reconnects and re-registers.
  bool timedOut(uint32_t nowMs) const;
  Leds leds(uint32_t nowMs) const;

  bool registered() const { return registered_; }
  const std::string& sourceId() const { return sourceId_; }
  uint32_t timeoutMs() const;

 private:
  uint32_t configTimeoutMs_;
  uint32_t gatewayTimeoutMs_ = 0;
  bool connected_ = false;
  bool registered_ = false;
  uint32_t lastRxMs_ = 0;
  std::string sourceId_;
  std::string session_;
  bool haveSequence_ = false;
  uint64_t lastSequence_ = 0;
  bool program_ = false;
  bool preview_ = false;
  bool valid_ = false;
};

}  // namespace tally
