#include <string.h>
#include <unity.h>

#include "tally.h"

using tally::Client;
using tally::Leds;

namespace {

void feed(Client& c, const char* json, uint32_t now) { c.onText(json, strlen(json), now); }

const char* kRegistered =
    R"({"version":1,"type":"registered","deviceId":"d","sourceId":"cam-1",)"
    R"("session":"s1","heartbeatIntervalMs":10000,"timeoutMs":30000})";

std::string tallyMsg(const char* source, const char* session, unsigned seq, bool program,
                     bool preview, bool valid = true) {
  char buf[256];
  snprintf(buf, sizeof buf,
           R"({"version":1,"type":"tally","sourceId":"%s","program":%s,"preview":%s,)"
           R"("state":"x","valid":%s,"session":"%s","sequence":%u,"timestamp":"t"})",
           source, program ? "true" : "false", preview ? "true" : "false",
           valid ? "true" : "false", session, seq);
  return buf;
}

void feed(Client& c, const std::string& json, uint32_t now) { feed(c, json.c_str(), now); }

// A client connected and registered for cam-1 in session s1 at t=0.
Client registeredClient(uint32_t configTimeoutMs = 0) {
  Client c(configTimeoutMs);
  c.onConnected(0);
  feed(c, kRegistered, 0);
  return c;
}

void assertLeds(const Leds& l, bool red, bool green) {
  TEST_ASSERT_EQUAL_MESSAGE(red, l.red, "red");
  TEST_ASSERT_EQUAL_MESSAGE(green, l.green, "green");
}

}  // namespace

// Red means on air; showing green as well would tell the operator the camera
// is only in preview.
void test_program_takes_precedence_over_preview() {
  assertLeds(tally::decide(true, true), true, false);
  assertLeds(tally::decide(true, false), true, false);
  assertLeds(tally::decide(false, true), false, true);
  assertLeds(tally::decide(false, false), false, false);
}

void test_tally_lights_leds() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 5, false, true), 10);
  assertLeds(c.leds(10), false, true);
  feed(c, tallyMsg("cam-1", "s1", 6, true, true), 20);
  assertLeds(c.leds(20), true, false);
}

// A delayed older tally must not overwrite newer state.
void test_stale_sequence_rejected() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 10, true, false), 10);
  feed(c, tallyMsg("cam-1", "s1", 9, false, true), 20);
  assertLeds(c.leds(20), true, false);
  feed(c, tallyMsg("cam-1", "s1", 10, false, false), 30);
  assertLeds(c.leds(30), true, false);
}

// A restarted gateway starts a new session with low sequence numbers, which
// must not be discarded against the old session's high ones.
void test_new_session_resets_sequence() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1000, true, false), 10);
  c.onDisconnected();
  c.onConnected(20);
  feed(c,
       R"({"version":1,"type":"registered","deviceId":"d","sourceId":"cam-1","session":"s2",)"
       R"("timeoutMs":30000})",
       20);
  feed(c, tallyMsg("cam-1", "s2", 1, false, true), 30);
  assertLeds(c.leds(30), false, true);
}

void test_session_change_on_tally_resets_sequence() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1000, true, false), 10);
  feed(c, tallyMsg("cam-1", "s9", 1, false, true), 20);
  assertLeds(c.leds(20), false, true);
}

void test_timeout_turns_leds_off() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 1000);
  assertLeds(c.leds(31000), true, false);
  TEST_ASSERT_FALSE(c.timedOut(31000));
  assertLeds(c.leds(31001), false, false);
  TEST_ASSERT_TRUE(c.timedOut(31001));
}

void test_heartbeat_extends_timeout() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 0);
  feed(c, R"({"version":1,"type":"heartbeat","session":"s1","sequence":2,"timestamp":"t"})",
       25000);
  assertLeds(c.leds(50000), true, false);
  assertLeds(c.leds(55001), false, false);
}

void test_config_timeout_overrides_gateway() {
  Client c = registeredClient(5000);
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 0);
  TEST_ASSERT_EQUAL_UINT32(5000, c.timeoutMs());
  assertLeds(c.leds(5001), false, false);
}

void test_gateway_timeout_used_without_config() {
  Client c(0);
  TEST_ASSERT_EQUAL_UINT32(30000, c.timeoutMs());
  c.onConnected(0);
  feed(c, R"({"version":1,"type":"registered","sourceId":"cam-1","session":"s1","timeoutMs":8000})",
       0);
  TEST_ASSERT_EQUAL_UINT32(8000, c.timeoutMs());
}

// Without any message the timer still runs from the connect, so a gateway
// that never answers the register is dropped and retried.
void test_timeout_without_registered() {
  Client c(0);
  c.onConnected(0);
  TEST_ASSERT_FALSE(c.timedOut(30000));
  TEST_ASSERT_TRUE(c.timedOut(30001));
}

// Contract: the gateway already sends program/preview false with valid=false;
// the device must still not trust the flags.
void test_invalid_turns_leds_off() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 10);
  feed(c, tallyMsg("cam-1", "s1", 2, true, false, false), 20);
  assertLeds(c.leds(20), false, false);
}

void test_disconnect_turns_leds_off() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 10);
  c.onDisconnected();
  assertLeds(c.leds(20), false, false);
  TEST_ASSERT_FALSE(c.timedOut(100000));
}

// The sourceId in registered is authoritative and may change at runtime.
void test_reassignment_via_registered() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 10);
  feed(c, R"({"version":1,"type":"registered","sourceId":"cam-2","session":"s1","timeoutMs":30000})",
       20);
  TEST_ASSERT_EQUAL_STRING("cam-2", c.sourceId().c_str());
  // cam-1's program state does not carry over to cam-2.
  assertLeds(c.leds(20), false, false);
  // A late tally for the old source is not shown.
  feed(c, tallyMsg("cam-1", "s1", 3, true, false), 30);
  assertLeds(c.leds(30), false, false);
  feed(c, tallyMsg("cam-2", "s1", 4, false, true), 40);
  assertLeds(c.leds(40), false, true);
}

void test_tally_before_registered_ignored() {
  Client c(0);
  c.onConnected(0);
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 10);
  assertLeds(c.leds(10), false, false);
}

void test_malformed_json_ignored() {
  Client c = registeredClient();
  feed(c, tallyMsg("cam-1", "s1", 1, true, false), 10);
  const char* bad[] = {
      "",
      "not json",
      "{\"version\":1,\"type\":\"tally\"",
      "[1,2,3]",
      R"({"type":"tally","sourceId":"cam-1","program":false,"valid":true,"session":"s1","sequence":2})",
      R"({"version":2,"type":"tally","sourceId":"cam-1","program":false,"valid":true,"session":"s1","sequence":2})",
      R"({"version":1,"type":"tally","sourceId":"cam-1","program":false,"valid":true,"session":"s1"})",
      R"({"version":1,"type":"tally","sourceId":"cam-1","program":false,"valid":true,"session":"s1","sequence":"2"})",
      R"({"version":1,"type":"registered","session":"s1"})",
      R"({"version":1,"type":"error","code":"invalid_message","message":"m"})",
      R"({"version":1,"type":"bogus"})",
  };
  for (const char* b : bad) feed(c, b, 20);
  assertLeds(c.leds(20), true, false);
  TEST_ASSERT_EQUAL_STRING("cam-1", c.sourceId().c_str());
  // None of them counted as a sign of life.
  TEST_ASSERT_TRUE(c.timedOut(30011));
}

void test_unknown_fields_ignored() {
  Client c = registeredClient();
  feed(c,
       R"({"version":1,"type":"tally","sourceId":"cam-1","program":true,"preview":false,)"
       R"("valid":true,"session":"s1","sequence":1,"extra":{"a":[1]}})",
       10);
  assertLeds(c.leds(10), true, false);
}

// Sequences are 64-bit on the wire.
void test_sequence_beyond_32_bits() {
  Client c = registeredClient();
  feed(c,
       R"({"version":1,"type":"tally","sourceId":"cam-1","program":true,"preview":false,)"
       R"("valid":true,"session":"s1","sequence":4294967297})",
       10);
  feed(c, tallyMsg("cam-1", "s1", 4294967295u, false, true), 20);
  assertLeds(c.leds(20), true, false);
}

void test_register_message() {
  TEST_ASSERT_EQUAL_STRING(
      R"({"version":1,"type":"register","deviceId":"tally-a1b2c3","sourceId":"cam-1","firmware":"0.1.0"})",
      tally::registerMessage("tally-a1b2c3", "cam-1", "0.1.0").c_str());
}

void test_default_device_id() {
  const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0xA1, 0x0b, 0xFF};
  TEST_ASSERT_EQUAL_STRING("tally-a10bff", tally::defaultDeviceId(mac).c_str());
  TEST_ASSERT_TRUE(tally::validDeviceId(tally::defaultDeviceId(mac)));
}

void test_valid_device_id() {
  TEST_ASSERT_TRUE(tally::validDeviceId("tally-esp32_001.a"));
  TEST_ASSERT_TRUE(tally::validDeviceId(std::string(64, 'a')));
  TEST_ASSERT_FALSE(tally::validDeviceId(""));
  TEST_ASSERT_FALSE(tally::validDeviceId(std::string(65, 'a')));
  TEST_ASSERT_FALSE(tally::validDeviceId("cam 1"));
  TEST_ASSERT_FALSE(tally::validDeviceId("cam/1"));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_program_takes_precedence_over_preview);
  RUN_TEST(test_tally_lights_leds);
  RUN_TEST(test_stale_sequence_rejected);
  RUN_TEST(test_new_session_resets_sequence);
  RUN_TEST(test_session_change_on_tally_resets_sequence);
  RUN_TEST(test_timeout_turns_leds_off);
  RUN_TEST(test_heartbeat_extends_timeout);
  RUN_TEST(test_config_timeout_overrides_gateway);
  RUN_TEST(test_gateway_timeout_used_without_config);
  RUN_TEST(test_timeout_without_registered);
  RUN_TEST(test_invalid_turns_leds_off);
  RUN_TEST(test_disconnect_turns_leds_off);
  RUN_TEST(test_reassignment_via_registered);
  RUN_TEST(test_tally_before_registered_ignored);
  RUN_TEST(test_malformed_json_ignored);
  RUN_TEST(test_unknown_fields_ignored);
  RUN_TEST(test_sequence_beyond_32_bits);
  RUN_TEST(test_register_message);
  RUN_TEST(test_default_device_id);
  RUN_TEST(test_valid_device_id);
  return UNITY_END();
}
