# dmf-tally-firmware

Reference tally light firmware for the ESP32. The device connects to a tally
gateway over WebSocket (protocol v1, `ws[s]://<host>:<port>/ws/v1`), registers
with its device and source IDs, and shows the program and preview state of its
assigned source on a red and a green LED. The protocol is specified in
[dmf-app-tally-gateway](https://github.com/marquies/dmf-app-tally-gateway) under
`docs/protocol.md`.

## Hardware

Any ESP32 board supported by the PlatformIO `esp32dev` target, two LEDs and two
resistors.

| Function          | Default GPIO | Notes                                  |
| ----------------- | ------------ | -------------------------------------- |
| Red LED, program  | 25           | active high                            |
| Green LED, preview| 26           | active high                            |
| Provisioning      | 0 (BOOT)     | active low, internal pull-up           |

Wire each LED from its GPIO through a series resistor to the anode, cathode to
GND. The GPIOs drive 3.3 V; 220 to 330 ohm limits a typical 5 mm LED to a few
mA. All three pins can be changed in the configuration. GPIOs 34 to 39 are
input only and have no internal pull-up, so they cannot drive an LED and need
an external pull-up when used for the button.

## LED states

| Condition                                      | Red | Green |
| ---------------------------------------------- | --- | ----- |
| program                                        | on  | off   |
| preview only                                   | off | on    |
| program and preview                            | on  | off   |
| neither                                        | off | off   |
| `valid=false` from the gateway                 | off | off   |
| no registered, tally or heartbeat for timeout  | off | off   |
| WebSocket closed, Wi-Fi lost, not registered   | off | off   |
| provisioning portal active                     | off | off   |

Program is `state` `program` or `key_fill` in the gateway's tally message.

## Configuration

Stored in NVS (namespace `tally`) and entered through the captive portal. Wi-Fi
SSID and password are stored by WiFiManager in the ESP32 Wi-Fi NVS.

| Setting                  | Default                 |
| ------------------------ | ----------------------- |
| Gateway host             | none, required          |
| Gateway port             | 8080                    |
| TLS (`1`/`0`)            | 0                       |
| Auth token               | none                    |
| Device ID                | `tally-` + last 3 bytes of the Wi-Fi MAC in hex |
| Source ID                | none, required          |
| Timeout seconds          | 30                      |
| Red, green, button GPIO  | 25, 26, 0               |

- The auth token is sent as `Authorization: Bearer <token>` on the upgrade
  request. The portal never shows it; leaving the field blank keeps the stored
  token. Erasing the flash (`pio run -e esp32dev -t erase`) clears it along with
  everything else.
- With TLS on, the server certificate is not verified.
- A device ID outside `[A-Za-z0-9._-]{1,64}` is not saved.
- Timeout `0` uses the `timeoutMs` the gateway sends in `registered`.
- The source ID is what the device registers with. The gateway's `registered`
  reply carries the authoritative source, and the device shows tally for that
  one, including when the gateway reassigns it at runtime.

## Provisioning

The device starts the portal when no Wi-Fi network is saved, when the gateway
host or source ID is empty, or when the provisioning button is held for one
second within three seconds after reset. Holding GPIO 0 low during the reset
itself enters the ESP32 ROM download mode instead, so press it after releasing
reset.

The portal is an open access point named after the device ID. Join it, open
the page the captive portal shows (or `http://192.168.4.1`), choose the Wi-Fi
network, fill in the settings and save. The device restarts with the new
configuration.

## Runtime behaviour

The device reconnects Wi-Fi automatically, reconnects the WebSocket every 2 s
while it is closed, and sends `register` on every connect. Any `registered`,
`tally` or `heartbeat` resets the fail-safe timer; when it expires, the device
turns the LEDs off, closes the connection and reconnects. A tally for another
source, or with a sequence at or below the last applied one in the same
session, is ignored. Serial output at 115200 baud logs connection state and
source assignments.

## Building and flashing

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/).

```sh
pio test -e native          # unit tests of lib/tally on the host
pio run -e esp32dev         # build
pio run -e esp32dev -t upload
pio device monitor
```

Each release attaches `firmware.bin`, the application image for offset
`0x10000`, and `factory.bin`, a merged image with bootloader, partition table
and application for a blank board:

```sh
esptool.py --chip esp32 write_flash 0x0 factory.bin
```
