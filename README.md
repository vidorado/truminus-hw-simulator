# TruMinus-BLESim

ESP32-C3 SuperMini firmware that impersonates a **Victron SmartSolar MPPT**,
an **Ultimatron LiFePO4 BMS** and a **fresh-water tank level sensor** over
BLE on a single device. Useful for developing and testing the
[TruMinus](https://github.com/) P4 firmware without the real hardware.

## What it does

| Role | Transport | Details |
|------|-----------|---------|
| Victron SmartSolar | BLE advertising (ADV_IND) | Company ID `0x02E1`, Instant Readout marker `0x10`, AES-128-CTR encrypted payload. |
| Ultimatron BMS | BLE GATT | Service `0xFF00`, characteristic `0xFF01` (notify) / `0xFF02` (write). Responds to the JBD `DD A5 03 00 FF FD 77` query. |
| Fresh-water tank | BLE advertising (BTHome v2) | Service Data UUID `0xFCD2`, *Moisture* tag `0x2F` (uint8 0..100 %). Bench value oscillates 25..95 % over ~10 min via `sinf()`. |

All roles share one BLE identity and one MAC. The Victron mfr-data adv
and the BTHome service-data adv alternate every 2 s on the same legacy
advertising channel; in any 5 s P4 scan window both are seen at least
once. When a central connects to read Ultimatron, legacy advertising
pauses and resumes on disconnect — the P4 polls Ultimatron once every
~30 s, so the gap is harmless.

## Hardware

- ESP32-C3 SuperMini (native USB-CDC, no UART bridge).
- USB-C cable. No external wiring needed.

## Build & flash

Requires ESP-IDF 6.0.x in `~/esp/esp-idf`.

```bash
. ~/esp/esp-idf/export.sh
make build
make flash          # PORT=/dev/ttyACM0 by default
make monitor        # Ctrl+] to exit
make flash-monitor
make clean
```

If flashing fails with "port busy", stop ModemManager:

```bash
sudo systemctl stop ModemManager
```

## Pairing with TruMinus P4

On first boot, read the MAC printed by the simulator:

```
I (xxx) blesim: Device MAC: AC:A7:04:D1:C1:8A  type=0
```

In the P4 settings UI (**Monitorización**), enter:

| P4 NVS key   | Value                                  |
|--------------|----------------------------------------|
| `solar/addr` | the MAC above, hex without colons      |
| `solar/key`  | `00112233445566778899AABBCCDDEEFF`     |
| `batt/addr`  | same MAC as `solar/addr`               |
| `tank/addr`  | same MAC as `solar/addr`               |

The same MAC powers all three roles. Or from the P4 serial REPL:

```
victron AABBCCDDEEFF 00112233445566778899AABBCCDDEEFF
ultimatron AABBCCDDEEFF
tank AABBCCDDEEFF
show
```

The AES key matches `s_aes_key[]` in `main/main.c`. Change it there and in
NVS together if you want a different one.

## Serial REPL

A small command interpreter runs on the USB-CDC console. Open it with
`make monitor` (or any terminal at `/dev/ttyACM0`) and type:

| Command         | Effect                                                  |
|-----------------|---------------------------------------------------------|
| `?`, `help`     | List commands                                           |
| `status`        | Print current values                                    |
| `v <V>`         | Battery voltage                                         |
| `a <A>`         | Battery current (+ charging / − discharging)            |
| `p <W>`         | PV power                                                |
| `k <kWh>`       | Yield today                                             |
| `s <state>`     | Victron state (0=Off, 2=Fault, 3=Bulk, 4=Abs, 5=Float, 7=Equalize) |
| `soc <%>`       | BMS state of charge                                     |
| `t <degC>`      | BMS NTC1 temperature                                    |
| `tank <pct>`    | Pin BTHome tank level (0..100 %). Disables autocycle.   |
| `auto on\|off`  | Toggle the `sinf()` auto-cycler                         |

Set `auto off` right after your first manual command so the cycler stops
overwriting values every 2 s.

## Source layout

- `main/main.c` — everything: NimBLE init, GAP/GATT, AES-CTR encoder, GATT
  callbacks, value updater task, USB-CDC REPL.
- `main/CMakeLists.txt` — component registration.
- `CMakeLists.txt` — top-level IDF project.
- `sdkconfig.defaults` — `IDF_TARGET=esp32c3`, NimBLE peripheral-only, MTU 247.
- `Makefile` — thin wrapper around `idf.py`.
- `.claude/skills/victronble/` and `.claude/skills/ultimatronble/` —
  byte-level reference for both protocols.

## Two MACs instead of one?

Out of scope for now. The current single-identity setup is what's used
during P4 debugging. If you need two MACs visible at the same time, switch
to BLE 5 extended advertising with two `ble_gap_ext_adv_*` sets — see the
note in `CLAUDE.md`.

## Gotchas

- Victron `0x10` marker lives in the SCAN_RSP, not the ADV_IND. The P4
  scanner uses active scan so it picks it up.
- BMS notification is a single 32-byte frame; the P4 already accumulates,
  so single or split delivery both work.
- AES-CTR nonce wraps every 65536 packets (~36 h at 2 s cadence). Reset
  the C3 to refresh nonce space — not a security issue, this is a sim.
