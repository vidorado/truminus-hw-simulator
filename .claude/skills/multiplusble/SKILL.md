---
name: multiplusble
description: Victron Multiplus / VE.Bus BLE Instant Readout — byte-level protocol reference for the VE.Bus Smart dongle adv. Reads inverter/charger state + power flows from advertising data. No GATT (read-only).
---

# Victron VE.Bus — Multiplus Instant Readout

Byte-level reference for parsing the BLE *Instant Readout* advertisement that
the **VE.Bus Smart dongle** broadcasts on behalf of a Multiplus / Multiplus-II
/ Quattro / MultiGrid / Phoenix Inverter (with VE.Bus port).

Reverse-engineered from `keshavdv/victron-ble` (Python) and cross-checked
against `Fabian-Schmidt/esphome-victron_ble` (C++).  Same outer envelope as
the SolarCharger record (see `victronble/SKILL.md`) but a different record
type byte and a packed-bitstream plaintext layout.

Implementation: `main/multiplusble.cpp` (parser, NVS, snapshot).  Hooked
into the shared `VictronScanCb::onResult` in `main/victronble.cpp` so we
don't fight NimBLE for the single global scan callback slot.

---

## Envelope (identical to other Victron Instant Readout records)

26-byte manufacturer-data field as returned by NimBLE
`NimBLEAdvertisedDevice::getManufacturerData()` (company ID INCLUDED):

```
mfr[0..1]  = E1 02       Victron company ID 0x02E1, LE
mfr[2]     = 0x10        Manufacturer record type = PRODUCT_ADVERTISEMENT
mfr[3]     = record length
mfr[4..5]  = product_id (uint16 LE)
mfr[6]     = readout_type   ← 0x0C = VE.Bus  (0x01 = Solar)
mfr[7..8]  = data counter / IV (uint16 LE) — AES-CTR nonce[0..1]
mfr[9]     = key-check = advertisement_key[0]
mfr[10..]  = AES-128-CTR ciphertext (16 bytes consumed)
```

`mfr[6]` is the **only** field that distinguishes VE.Bus from Solar — the
rest of the cipher envelope is the same.  The Solar parser in
`main/victronble.cpp` gates on `mfr[6] == 0x01` to avoid mis-decrypting
VE.Bus traffic with the solar key.

### AES-128-CTR nonce

```
nonce[0]     = mfr[7]   (IV low byte)
nonce[1]     = mfr[8]   (IV high byte)
nonce[2..15] = 0
```

The `mbedtls` / `esp_aes_crypt_ctr` API increments the nonce internally as
it consumes blocks.  We only ever decrypt 16 bytes (one block) so the
counter never advances visibly.

### Bind key

16 bytes, retrieved from VictronConnect: connect to the dongle → ⚙ ️→
*Product info* → *Show* the bind key (32 hex chars).  Each VE.Bus dongle
has its **own** key, distinct from any solar charger.  Stored in
NVS namespace `multiplus` as keys `addr` (12-hex MAC) and `key` (32-hex
bind key).

### IV deduplication

The dongle re-emits the same ciphertext several times within a few
seconds (BLE advertising is unreliable so the protocol relies on
repetition).  We dedup by IV in `multiplusBleHandleAd()` so the WS
broadcast diff filter sees one fresh sample per emission cycle, not 4–6
duplicates.

---

## Plaintext bit-stream (after AES decryption, 102 bits used of 128)

LSB-first packed bit stream.  Fields are listed in emission order:

| Bits | Field | Type | Unit | Sentinel = no data |
|---|---|---|---|---|
| 0..7   | `device_state`    | uint8  | enum | `0xFF` |
| 8..15  | `error`           | uint8  | enum | `0xFF` |
| 16..31 | `battery_current` | int16  | 0.1 A | `0x7FFF` |
| 32..45 | `battery_voltage` | uint14 | 0.01 V | `0x3FFF` |
| 46..47 | `ac_in_state`     | uint2  | enum | `3` |
| 48..66 | `ac_in_power`     | int19  | 1 W  | (none documented) |
| 67..85 | `ac_out_power`    | int19  | 1 W  | (none documented) |
| 86..87 | `alarm`           | uint2  | enum | `3` |
| 88..94 | `battery_temp`    | uint7  | 1 °C, offset −40 | `0x7F` |
| 95..101| `soc`             | uint7  | 1 %  | `0x7F` |
| 102..127 | (random padding from AES block) | | | |

**Signed-int sign extension**: read N bits, then if bit (N−1) is set, OR
in `~((1<<N)−1)` to extend.  `BitReader::readS(int bits)` in
`main/multiplusble.cpp` does this.

### `device_state` enum (subset relevant to UI)

```
0   Off              7   Equalize          11  Power Supply
1   Low Power        8   Passthru          252 External Control
2   Fault            9   Inverting         0xFF  no data
3   Bulk            10   Power Assist
4   Absorption
5   Float
6   Storage
```

Anything not enumerated here just renders as `--` upstream; expand
`multiplusStateName()` (in `main/multiplusble.cpp`) when new states show
up on the wire.

### `ac_in_state` (shore-side connection)

```
0  AC In 1 available    2  Disconnected
1  AC In 2 available    3  no data
```

### `alarm`

```
0  No alarm    2  Alarm
1  Warning     3  no data
```

---

## Power flow conventions

- `ac_in_power`  : positive = the inverter is **drawing** from shore /
  generator.  Negative = exporting back (only meaningful on Multiplus-II
  in ESS / grid-tie configurations).
- `ac_out_power` : positive = **delivering** to the loads.
- `battery_current` : positive = **charging** the battery; negative =
  inverting (drawing from battery).
- Implicit DC-side W is `battery_voltage × battery_current` — we
  compute that in the UI rather than burning a field on the wire.

---

## What you cannot get from advertising

- **ON / OFF / Inverter-only / Charger-only mode switch** — the dongle
  publishes only telemetry over BLE adv.  Setting requires VE.Bus GATT,
  which Victron has not documented publicly and which no major open
  source project (`victron-ble`, `esphome-victron_ble`,
  `signalk-victron-ble`) has reverse-engineered.  In TruMinus the
  On/Off buttons render disabled until that work happens.
- **Per-phase AC voltage / current** — only aggregate W is in the adv.
  Full data requires VE.Bus protocol over the MK3 USB interface or a
  Cerbo GX / Venus OS device.
- **Detailed grid-feed metering** — same as above.

---

## Quick cheat-sheet for the P4

```
# Pair (REPL, USB-Serial-JTAG on /dev/ttyACM0):
multiplus AABBCCDDEEFF <32-hex-bind-key>
show

# Or via the LCD: ⚙ → Monitorización → Inversor Multiplus (VE.Bus).

# Wire frame check (monitor log):
[multi] st=9 err=0 acIn=0W acOut=420W V=12.86 A=-32.7 soc=87
#       └─inverting     └─drawing 32.7 A from battery at 12.86 V
```

The first valid frame typically arrives within one ~5 s scan window
after pairing.  If you see `[multi]` log lines but no UI updates, check
the key check byte — wrong bind key fails silently because
`aesCtrDecrypt` returns plausible-looking junk; the
`if (mfr[9] != s_aesKey[0])` gate in `multiplusBleHandleAd()` should
catch it.

---

## References

- `keshavdv/victron-ble/victron_ble/devices/vebus.py` — Python parser
  with the same bit layout.
- `Fabian-Schmidt/esphome-victron_ble/components/victron_ble/victron_ble.h`
  — C struct definitions for the envelope (confirmed our `mfr[6]`
  position is correct).
- VictronConnect → device → Product info → BLE bind key — the only
  supported way to retrieve the per-device AES key.
