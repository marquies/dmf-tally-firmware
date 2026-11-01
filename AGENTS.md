# dmf-tally-firmware

PlatformIO project, Arduino framework, one target `esp32dev`.

- `lib/tally/` holds every protocol and LED decision and must not include
  Arduino headers: it is what `pio test -e native` covers. `src/main.cpp` is
  glue between it, WiFiManager, NVS and the WebSocket client; logic that can
  be tested goes into `lib/tally/`, with a test in `test/test_tally/`.
- The client side follows the gateway's protocol v1 in
  `dmf-app-tally-gateway/docs/protocol.md`. Raise a disagreement with that
  document instead of diverging from it.
- Fail-safe is LEDs off. A change must not light red or green steadily for any
  reason other than a valid tally for the assigned source.
- Never build credentials, hosts or tokens into the image; configuration lives
  in NVS.
- Platform, library and tool versions are pinned in `platformio.ini`, and CI
  keys its cache on that file.
- `src/version.h` is bumped by release-please through the
  `x-release-please-version` marker; do not edit the version by hand.
- Verify with `pio test -e native` and `pio run -e esp32dev`. Behaviour on a
  real board is not covered by either.
