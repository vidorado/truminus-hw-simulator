# CLAUDE.md

This file provides guidance to Claude Code when working in this repository.

## Language policy

**All code, comments, identifiers, commit messages, log strings, docs and PR
descriptions MUST be in English.** Conversation with the user can be in
Spanish.

## What this project is

TruMinus-BLESim is a small **ESP32-C3 SuperMini** firmware that runs on a
spare dev board and pretends to be:

1. A **Victron SmartSolar MPPT** — broadcasts the *Instant Readout* BLE
   advertising payload (AES-CTR encrypted manufacturer data, manuf id
   `0x02E1`), cycling values via `sinf()` every 2 s.
2. An **Ultimatron LiFePO4 BMS** — exposes a GATT server with service
   `0xFF00` and the JBD-protocol characteristics `0xFF01` (notify) /
   `0xFF02` (write).  Responds to the standard read-basic-info command
   `DD A5 03 00 FF FD 77` with canned voltage / current / SOC / temp.

Both roles live on a **single BLE identity / one MAC**.  The TruMinus P4
firmware (`../TruMinus/`, separate repo) is configured to point both
`solar/addr` and `batt/addr` NVS keys at this MAC.  When the P4 connects
for an Ultimatron GATT poll, the Victron advertising pauses briefly and
resumes on disconnect — fine because the P4 polls Ultimatron once every
~30 s.

If you need true two-device separation (distinct MACs simultaneously),
migrate to BLE 5 extended advertising with two adv sets.  That's a
~30-line change but keep it on a branch — the current design is what the
user prefers for debugging.

## Hardware

- **Board:** ESP32-C3 SuperMini (USB-CDC native, no USB-UART bridge).
- **Port:** `/dev/ttyACM0` by default.  ModemManager may grab it on plug
  in — `sudo systemctl stop ModemManager` if flash fails with "port
  busy".  Same trick documented in the TruMinus skill `pio-idf-p4`.
- **No external wiring needed.**  Power via USB-C; BLE is on the chip.

## Build system

Driven by **`idf.py`** (ESP-IDF 6.0.x) via a thin `Makefile`.  No
PlatformIO.

```bash
. ~/esp/esp-idf/export.sh   # once per terminal
make build                  # build (sets target esp32c3 first)
make flash                  # PORT=/dev/ttyACM0 by default
make monitor
make flash-monitor
make clean
```

The Makefile's `set-target` step is idempotent — safe to run on a
clean tree.

## Source layout

- `main/main.c` — everything: NimBLE init, GAP/GATT, AES-CTR encryption
  of the Victron payload, GATT service definition, value updater task.
  ~250 lines, single file.
- `main/CMakeLists.txt` — registers `main.c`; requires `nvs_flash bt
  esp_timer mbedtls`.
- `CMakeLists.txt` — top-level IDF project.
- `sdkconfig.defaults` — `IDF_TARGET=esp32c3`, NimBLE peripheral-only,
  max 1 connection.

## Protocol references

Two skills live under `.claude/skills/` — copied verbatim from the
TruMinus parent project so a fresh session has the full byte-level
spec without needing to read the parent repo:

- `victronble/SKILL.md` — Victron Instant Readout BLE advertising
  format, AES-CTR nonce layout, plaintext field offsets.
- `ultimatronble/SKILL.md` — Ultimatron / JBD BMS GATT protocol, the
  `DD A5 03 00 FF FD 77` query and the response packet layout.

The encoder in `main.c::build_victron_mfr()` mirrors the *inverse* of
the parser in TruMinus's `main/victronble.cpp::parseMfrData()`.  If
the field encoding ever drifts, update both sides — the P4 parser is
authoritative because it reads real Victron devices in production.

## Configuration shared with the P4

Hardcoded constants in `main/main.c` that the P4's NVS must match:

| Constant in `main.c`     | P4 NVS key              | Value                                |
|--------------------------|-------------------------|--------------------------------------|
| `s_aes_key[16]`          | `solar/key` (hex str)   | `00112233445566778899AABBCCDDEEFF`   |
| (device's own random MAC)| `solar/addr` AND `batt/addr` | read from the C3 boot log     |

On first boot, monitor the C3:
```
I (xxx) blesim: Device MAC: AA:BB:CC:DD:EE:FF  type=1
```
Copy that MAC into both NVS slots on the P4 via the settings UI.

## Common edits

- **Change cycled values** → `updater_task()` at the bottom of `main.c`.
- **Add an Ultimatron field** → extend `send_bms_response()`; consult
  the ultimatronble skill for the byte offsets the P4 reads.
- **Change Victron device state** (bulk / float / fault) → `s_state`.
- **Add password auth to Ultimatron** → already partially scaffolded
  in `gatt_ff02_access()`; just match `buf[0]==0xDD && buf[1]==0x5A`.

## Gotchas

- **Advertising pauses while connected.**  Legacy advertising stops
  when a central connects.  Victron scanners see a gap — harmless
  because the P4 polls Ultimatron briefly (<2 s) and resumes
  receiving Victron data after disconnect.
- **AES-CTR nonce reuse is fine here** because the simulator is not a
  security boundary.  The `s_iv` counter wraps every 65536 packets
  (~36 hours at 2 s cadence); reset the C3 if you need a fresh nonce
  space.
- **MTU.**  `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247` so the 33-byte BMS
  notification fits in one ATT_HANDLE_VALUE_NTF.  Don't drop below 64.
- **Build target.**  `make build` runs `idf.py set-target esp32c3`
  silently first.  If you flip to another chip, edit the `TARGET ?=`
  line in the Makefile *and* delete `build/` + `sdkconfig` to force a
  clean reconfigure.
