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
   `0x02E1`, readout type `0x01`), cycling values via `sinf()` every 2 s.
2. An **Ultimatron LiFePO4 BMS** — exposes a GATT server with service
   `0xFF00` and the JBD-protocol characteristics `0xFF01` (notify) /
   `0xFF02` (write).  Responds to the standard read-basic-info command
   `DD A5 03 00 FF FD 77` with canned voltage / current / SOC / temp.
3. A **fresh-water tank level sensor** — broadcasts a BTHome v2
   unencrypted Service Data frame (UUID `0xFCD2`) carrying the
   *Moisture* tag (`0x2F`, `uint8` 0..100 %).
4. A **Victron Multiplus II** (VE.Bus Smart dongle) — broadcasts
   *Instant Readout* BLE advertising with readout type `0x0C` and
   a distinct AES-128 bind key.  The 102-bit packed plaintext carries
   device state, error, battery V/A/T, AC in/out W, AC-in state,
   alarm, and SOC — identical to what a real VE.Bus Smart dongle emits.
   See `../TruMinus/.claude/skills/multiplusble/SKILL.md` for the
   bit-level layout.

The advertising rotates three payloads (solar → BTHome tank → multiplus)
every 2 s on the same legacy advertising channel.  In a 5 s P4 scan
window each payload appears at least once.

All roles live on a **single BLE identity / one MAC**.  The TruMinus P4
firmware (`../TruMinus/`, separate repo) is configured to point
`solar/addr`, `batt/addr` and `multiplus/addr` NVS keys at this MAC.
When the P4 connects for an Ultimatron GATT poll, the Victron
advertising pauses briefly and resumes on disconnect — fine because the
P4 polls Ultimatron once every ~30 s.

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

- `main/main.c` — NimBLE init, GAP/GATT, AES-CTR encryption of the
  Victron Solar + VE.Bus payloads, BTHome v2 service-data builder for
  the fresh-water tank role, GATT service definition, value updater
  task that rotates Solar / BTHome / Multiplus adv payloads every 2 s,
  USB-CDC REPL.  Calls `truma_sim_init()` at the end of `app_main`.
- `main/truma_sim.[ch]` — Truma Combi D LIN slave simulator on UART1.
  TX=GPIO20, RX=GPIO21, 9600 8N1, TTL-direct (no LIN transceiver).
  Publishes frames 0x21 (room/water temperatures) and 0x22 (burner
  state); accepts master writes on 0x20 and answers 0x3C/0x3D
  diagnostic transport for TOnOff (SID 0xB8) and TGetErrorInfo
  (SID 0xB2). See `.claude/skills/truma-protocol/SKILL.md` for the
  byte layout.
- `main/CMakeLists.txt` — registers `main.c` + `truma_sim.c`; requires
  `nvs_flash bt esp_timer mbedtls esp_driver_usb_serial_jtag
  esp_driver_uart`.
- `CMakeLists.txt` — top-level IDF project.
- `sdkconfig.defaults` — `IDF_TARGET=esp32c3`, NimBLE peripheral-only,
  max 1 connection.

## Truma LIN simulator — wiring & gotchas

Hard-won lessons from getting the C3 to actually talk to a real CYD-C5
running the TruMinus firmware over TTL UART (no LIN transceiver):

- **Pins**: TX=GPIO21, RX=GPIO20. GPIO20/21 are the C3's default UART0
  pins. The IDF console MUST be moved to USB-CDC only
  (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_UART_NUM=-1`)
  or UART0 will keep driving GPIO21 and silently steal our TX line.
  `gpio_reset_pin()` alone is **not** enough — the IDF console
  reattaches the IO_MUX on every `ESP_LOGI()`.
- **Baud is 9600**, not the 19200 that `Lin_Interface.hpp` defaults to.
  `main.cpp` of the TruMinus C5 overrides it via `LinBus.baud = 9600;`.
  The BREAK byte is sent at half-baud (4800) inside an otherwise 9600
  stream — on the scope it looks like "a pulse twice as wide", which
  is a misleading sign that the C5 runs at 19200. It does not.
- **Slave responses must include a `0x00 0x55 PID` prefix** before the
  data + checksum. The C5's `Lin_Interface::readFrame()` state machine
  expects to see those three bytes in its RX before storing data. On a
  real LIN bus they appear naturally as the master's TX echo (single
  wire). With separate TX/RX wires the prefix has to be faked by us.
  Implemented in `send_frame()` in `truma_sim.c`.
- **1 kΩ pull-down on our TX (GPIO21 → GND)** is required. The C5's RX
  pin has an internal pull-up (~30 kΩ to 3.3 V) enabled implicitly by
  `Serial1.begin()` / `uart_set_pin()` in IDF. Our push-pull driver
  alone can't pull the line below ~2 V against it; the C5 then reads
  the line as "always high". With the 1k to GND the line swings 0–3.3
  V cleanly.
- **Sync on 0x55 + PID parity, not on UART BREAK events.** The default
  IDF break-detection threshold (~23 bit times) often misses LIN
  BREAKs (≥13 bits). Streaming the bytes and looking for `0x55`
  followed by a valid PID is more robust and avoids tuning HAL-private
  registers.

## Truma LIN sim REPL

In addition to the solar/BMS commands, the USB-CDC REPL exposes:

```
troom <C>                    set reported room temperature
twater <C>                   set reported water temperature
tburn 0|1                    burner state in frame 0x22
terr <class> [code [short]]  populate TGetErrorInfo reply
tstatus                      dump Truma sim state
```

And for the fresh-water tank role:

```
tank <pct 0..100>            pin the BTHome moisture value;
                             disables the sinusoidal autocycle so
                             the value sticks until rebooted or
                             `auto on` is re-issued
```

## Multiplus VE.Bus REPL

```
mstate <0-11>                device state (0=Off 3=Bulk 5=Float 9=Invert …)
merr <n>                     error code (0=none)
macin <W>                    AC input (shore) power in W
macout <W>                   AC output (load) power in W
mv <V>                       battery voltage
ma <A>                       battery current (neg = inverting)
msoc <%>                     state of charge
mtemp <°C>                   battery temperature
malarm <0-2>                 alarm (0=none 1=warning 2=alarm)
macst <0-2>                  AC input state (0=AC1 1=AC2 2=disconnected)
mstatus                      dump all multiplus state
```

All `m*` commands that set values cycling in autocycle also disable
autocycle so the value sticks.  Use `auto on` to resume cycling.

## BTHome v2 tank-level adv

Frame layout produced by `update_adv_data_bthome()` in `main.c`:

```
AD: 02 01 06                              Flags
AD: 03 03 D2 FC                           Complete list of 16-bit UUIDs (BTHome)
AD: 08 16 D2 FC 40 00 <seq> 2F <pct>      Service Data:
     - UUID 0xFCD2 (BTHome) little-endian
     - Device info 0x40 = v2, unencrypted, no trigger
     - Tag 0x00 (Packet ID, uint8) + seq    — used by receiver for dedup
     - Tag 0x2F (Moisture, uint8 0..100 %) + pct   ← the tank level
```

Sequence: `s_tankSeq` increments on every BTHome emission (8-bit
wrap).  The P4 (`main/tankble.cpp`) treats the same seq twice in a row
as a re-emit and skips it; that keeps the LCD/web from flickering when
the adv repeats inside one scan window.

No encryption: bit 0 of the device-info byte stays clear.  The MAC
allow-list on the P4 (`tank/addr` NVS key) is the only access control —
fine for a sensor that only carries one byte of household telemetry.
Match the receiver:  `.claude/skills/` on the P4 side does not have a
dedicated BTHome skill; the format is simple enough that the comment
block in `main/tankble.cpp` is the canonical reference.

## Protocol references

Two skills live under `.claude/skills/` — copied verbatim from the
TruMinus parent project so a fresh session has the full byte-level
spec without needing to read the parent repo:

- `victronble/SKILL.md` — Victron Instant Readout BLE advertising
  format, AES-CTR nonce layout, plaintext field offsets.
- `ultimatronble/SKILL.md` — Ultimatron / JBD BMS GATT protocol, the
  `DD A5 03 00 FF FD 77` query and the response packet layout.
- `truma-protocol/SKILL.md` — full Truma LIN frame reference (master/slave frames, byte layouts).

The encoder in `main.c::build_victron_mfr()` mirrors the *inverse* of
the parser in TruMinus's `main/victronble.cpp::parseMfrData()`.  The
encoder in `build_multiplus_mfr()` mirrors the *inverse* of the
`BitReader` in `main/multiplusble.cpp::multiplusBleHandleAd()`.  If
the field encoding ever drifts, update both sides — the P4 parsers are
authoritative because they read real Victron devices in production.

## Configuration shared with the P4

Hardcoded constants in `main/main.c` that the P4's NVS must match:

| Constant in `main.c`     | P4 NVS key                          | Value                                |
|--------------------------|--------------------------------------|--------------------------------------|
| `s_aes_key[16]`          | `solar/key` (hex str)                | `00112233445566778899AABBCCDDEEFF`   |
| `s_multi_key[16]`        | `multiplus/key` (hex str)            | `FFEEDDCCBBAA99887766554433221100`   |
| (device's own random MAC)| `solar/addr`, `batt/addr`, `multiplus/addr` | read from the C3 boot log     |

On first boot, monitor the C3:
```
I (xxx) blesim: Device MAC: AA:BB:CC:DD:EE:FF  type=1
I (xxx) blesim: Solar  key: 00112233445566778899AABBCCDDEEFF
I (xxx) blesim: Multi  key: FFEEDDCCBBAA99887766554433221100
```
Copy that MAC into the three NVS addr slots on the P4 and each key
into the corresponding key slot via the settings UI (⚙ → Monitorización).

## Common edits

- **Change cycled values** → `updater_task()` at the bottom of `main.c`.
- **Add an Ultimatron field** → extend `send_bms_response()`; consult
  the ultimatronble skill for the byte offsets the P4 reads.
- **Change Victron device state** (bulk / float / fault) → `s_state`.
- **Change Multiplus device state** → `s_mDevState` or REPL `mstate`.
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
