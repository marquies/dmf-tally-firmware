# dmf-tally-firmware

PlatformIO project, Arduino framework. `esp32dev` is the device image;
`esp32dev-wokwi` is a simulator build only and is never flashed to a board.

- `lib/tally/` holds every protocol and LED decision and must not include
  Arduino headers: it is what `pio test -e native` covers. `src/main.cpp` is
  glue between it, WiFiManager, NVS and the WebSocket client; logic that can
  be tested goes into `lib/tally/`, with a test in `test/test_tally/`.
- The client side follows the gateway's protocol v1 in
  `dmf-app-tally-gateway/docs/protocol.md`. Raise a disagreement with that
  document instead of diverging from it.
- Fail-safe is LEDs off. A change must not light red or green steadily for any
  reason other than a valid tally for the assigned source.
- Never build credentials, hosts or tokens into the device image;
  configuration lives in NVS. The simulator build compiles in the simulator's
  open network and a gateway address, and nothing else.
- Platform, library and tool versions are pinned in `platformio.ini`, and CI
  keys its cache on that file.
- `src/version.h` is bumped by release-please through the
  `x-release-please-version` marker; do not edit the version by hand.
- Verify with `pio test -e native`, `pio run -e esp32dev` and
  `pio run -e esp32dev-wokwi`. None of them covers behaviour on a real board.
