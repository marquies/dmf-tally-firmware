# Changelog

## [0.2.0](https://github.com/marquies/dmf-tally-firmware/compare/v0.1.0...v0.2.0) (2026-11-03)


### Features

* **firmware:** point the simulator build at any gateway ([a3ffea1](https://github.com/marquies/dmf-tally-firmware/commit/a3ffea1c01befad73b50f190864c589715adc6c4))
* run on the ESP32-C3 Super Mini ([#3](https://github.com/marquies/dmf-tally-firmware/issues/3)) ([5a4e39e](https://github.com/marquies/dmf-tally-firmware/commit/5a4e39e31c3970e1515b7cf8d93fc628d7f0b213))
* serve the settings on the LAN once Wi-Fi is up ([e595abd](https://github.com/marquies/dmf-tally-firmware/commit/e595abdfa90364b165541a3e8ee32d472d92cef5))


### Bug Fixes

* answer 404 for the portal's maintenance routes ([9e574d9](https://github.com/marquies/dmf-tally-firmware/commit/9e574d933791b69d0dc2404b3135811a2c93ba2b))

## 0.1.0 (2026-11-01)


### Features

* **firmware:** add a Wokwi simulation build ([dc8a833](https://github.com/marquies/dmf-tally-firmware/commit/dc8a8333086b3e4b252c78567ea98e80c896ee5c))
* **firmware:** drive the tally LEDs from the gateway on an ESP32 ([2819157](https://github.com/marquies/dmf-tally-firmware/commit/281915738d051598f19abeeb2ad03b40ef523173))
* **tally:** add the protocol client logic with native tests ([dc1f002](https://github.com/marquies/dmf-tally-firmware/commit/dc1f002f722500e6facbc4576a044f5688514777))


### Bug Fixes

* **firmware:** default the gateway port to 8080 ([8a024f4](https://github.com/marquies/dmf-tally-firmware/commit/8a024f4a7e0d5e38a1a04bdf3625d753b0d372b4))
* **firmware:** stop sending an Origin header the gateway rejects ([160fb76](https://github.com/marquies/dmf-tally-firmware/commit/160fb76e4c17e589d8cf2328269464d2af3b674f))


### Continuous Integration

* open release pull requests with the release-please token ([d159b3b](https://github.com/marquies/dmf-tally-firmware/commit/d159b3b404d9b9b7295a69706fa5eae3aed63cd6))
* test, build and release the firmware ([e082cae](https://github.com/marquies/dmf-tally-firmware/commit/e082cae45af29186f78df27ed2a056b467cc171d))
