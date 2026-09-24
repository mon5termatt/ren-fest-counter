# Ren Fest Counter

ESP32-WROOM-32 scoreboards for Renaissance festival games: a name on **8×** MAX7219 8×8 panels and a count on **4×** panels below, with local +/− buttons, a WiZ mote (ESP-NOW), and a SoftAP web UI.

```
  [======== NAME ========]   ← 8 modules (64×8)
  [==== NUMBER ====]         ← 4 modules (32×8), centered
        [ - ]  [ + ]
```

## Features

- Up to **8** scoreboards (compile-time `MAX_COUNTERS`), enable/disable in the web UI
- Each board: 12 FC-16 MAX7219 modules on one SPI chain (zones: name + number)
- Per-board physical **− / +** (debounce + hold-to-repeat); no hardware reset
- **WiZ mote** ESP-NOW remote with web-assignable key → action map
- SoftAP setup UI for names, counts, intensity, remote link, and keymap
- Settings persisted in NVS

## Hardware

### SPI / matrices

Shared `DIN` / `CLK` / **one CS** for the whole name+number daisy-chain. The display always shows the **active** (selected) counter; WiZ 1–4 switches which logical counter is shown.

| Signal | GPIO (default) |
|--------|----------------|
| DIN    | 19 |
| CLK    | 18 |
| CS     | 5 |

Power LED matrices from a solid **5 V** supply (not the ESP32 3V3 pin); share ground with the ESP32.

### Physical buttons

One − / + pair on the board adjusts the **active** counter:

| Button | GPIO |
|--------|------|
| − | 26 |
| + | 14 |

Edit all pins in [`include/config.h`](include/config.h). Set unused CS/button entries to `-1`.

For more than ~4 boards you may run out of GPIOs; an I2C expander (e.g. MCP23017) is a later option.

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                    # build
pio run -e ota -t upload   # HTTP OTA over SoftAP Wi‑Fi (PC must be 192.168.4.x)
pio device monitor         # serial @ 115200
```

OTA uses **Wi‑Fi only** (`ota_host_ip.py` / SoftAP). Upload is an HTTP push to `http://192.168.4.1/update` (also available as a form on the web UI). Password for SoftAP: `renfest123`.

If the device doesn’t have `/update` yet (older build), one USB flash bootstraps it; after that use `-e ota` only.

## Web UI

On boot the ESP32 opens SoftAP:

- SSID: `RenFest-Counter`
- Password: `renfest123`
- URL: http://192.168.4.1/

There you can:

- Edit counter names / counts, enable boards, set LED intensity, blank/unblank
- **Reset counts** (only place that zeros a count — not on the remote or local buttons)
- Link a WiZ remote MAC (or clear to accept any)
- Assign each WiZ key to an action: `none`, `select` (counter N), `inc_active`, `dec_active`, `blank`, `unblank`

## WiZ mote (factory defaults)

Protocol matches WLED’s WiZ remote handler (`WizMoteMessageStructure`).

| Button | Default action |
|--------|----------------|
| 1–4 | Select counter 1–4 |
| Bright + | Increment active |
| Bright − | Decrement active (floor 0) |
| ON | Unblank |
| OFF | Blank (counts kept) |
| NIGHT | Ignored |

Press a key on the remote once so it appears under “Last seen”, then **Link last seen** if you want to lock to that remote.

## Project layout

```
include/config.h       pins, MAX_COUNTERS, AP credentials
src/main.cpp
src/counters.*         state + NVS
src/display.*          MD_Parola zones
src/local_buttons.*    physical +/- 
src/wiz_remote.*       ESP-NOW receive
src/remote_map.*       key → action map
src/web_ui.*           SoftAP HTTP UI
```

## Notes

- ESP-NOW shares the SoftAP Wi-Fi channel (AP starts before ESP-NOW init).
- Module order: if text is mirrored or zones look swapped, check daisy-chain direction / `FC16_HW` vs your PCB variant in `display.cpp`.
- Count range: 0–9999.
