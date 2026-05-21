# Victron BLE — Instant Readout protocol

Technical reference for the Victron Solar Charger Instant Readout protocol over BLE.
Everything below was confirmed by capturing real packets from device `D8AC8D2C49FA` with key `67c338518c8cadb1d4c141a9771eb672`.

---

## Two advertisement types (same company ID 0x02E1)

| `full[2]` marker | PDU | Contents |
|---|---|---|
| `0x02` | ADV_IND (regular advertisement) | NOT Instant Readout; cannot be decrypted with the key |
| `0x10` | **SCAN_RSP** (scan response) | Encrypted Instant Readout — the useful payload |

**The `0x10` marker only ever arrives in the Scan Response → `setActiveScan(true)` is mandatory.**
Passive scan only sees `0x02` packets. Passive = no data.

---

## `0x10` packet layout (as returned by NimBLE `getManufacturerData()`, company ID INCLUDED)

```
full[0-1]  = E1 02       Victron company ID (LE 0x02E1)
full[2]    = 0x10        Instant Readout marker  ← FILTER HERE
full[3]    = prefix hi
full[4-5]  = model_id (LE)
full[6]    = readout_type  (0x01 = solar charger)
full[7-8]  = IV counter (little-endian 16-bit)
full[9]    = key-check = advertisement_key[0]  ← VERIFY BEFORE DECRYPTING
full[10+]  = ciphertext AES-128-CTR (16 bytes used)
```

**Python-vs-NimBLE offset gotcha:** the `victron-ble` library (bleak) receives bytes WITHOUT the company ID; its offsets are `-2` relative to NimBLE. Do not mix the two layouts.

---

## AES-128-CTR nonce

```
nonce[0]     = full[7]   (IV low byte)
nonce[1]     = full[8]   (IV high byte)
nonce[2..15] = 0x00
```

Equivalent to `Counter.new(128, initial_value=iv16, little_endian=True)` in pycryptodome.
Implemented in firmware with `mbedtls_aes_crypt_ctr()`.

---

## Decrypted payload — solar charger (offsets are into the 16 AES output bytes)

| Bytes  | Type     | Scale     | Field |
|--------|----------|-----------|-------|
| [0]    | uint8    | —         | device state (see table below) |
| [1]    | uint8    | —         | error code |
| [2-3]  | int16 LE | ÷100 → V  | battery voltage (sentinel `0x7FFF` = invalid) |
| [4-5]  | int16 LE | ÷10 → A   | battery current (sentinel `0x7FFF` = 0.0 A) |
| [6-7]  | uint16 LE | ×10 → Wh | yield today |
| [8-9]  | uint16 LE | 1 W       | PV power |

### Device states

| Value | State |
|-------|-------|
| 0     | Off |
| 2     | Fault |
| 3     | Bulk |
| 4     | Absorption |
| 5     | Float |
| 7     | Equalize |
| 245   | Starting |
| 247   | Auto-equalize |
| 252   | External control |

---

## Getting the encryption key

VictronConnect → device → ⚙ three dots → **Product info** → field **"Encryption key"** (32 hex chars).
- NOT the device pairing PIN.
- Changes whenever the user changes the PIN.

---

## Implementation in TruMinus (`main/victronble.cpp`)

- `victronBleInit()`: loads MAC + key from NVS namespace `"solar"`, brings NimBLE up, starts `bleTask`.
- `bleTask`: 3 s active scan every 5 s; `onResult` filters by company ID + marker + MAC + key-check.
- `aesCtrDecrypt()`: AES-128-CTR via mbedtls.
- `victronBleSuspend()` / `victronBleResume()`: pauses/resumes the scan task (required before an Ultimatron GATT connect).
- NVS: namespace `"solar"`, keys `"addr"` (12 uppercase hex chars) and `"key"` (32 uppercase hex chars).

### LCD integration

- `p4UpdateSolar(const P4SolarData& d)` in `p4display.cpp` refreshes the four solar data lines under the LVGL mutex. On the previous C5 board this was `cydUpdateSolar` in `cyddisplay.cpp`; treat that as a porting reference, not the current API.
- Data is broadcast to web clients every 10 s by `publishSolarBatt()` in `main.cpp`.
- The setup screen (formerly `runSolarSetup` in `wifisetup.cpp`; will be re-ported into the P4 settings flow) lets the user enter MAC + key by hand; **it works even when BLE is not compiled in**, so you can pre-provision the device.

### Simulated stubs (BLE disabled)

When `-DENABLE_BLE` is NOT defined, `victronble.cpp` compiles to stubs:
- `victronBleInit()` → no-op.
- `victronBleGetData()` → returns oscillating fake values (`sinf()`) for battV, battA, pvW, kWhToday.
- This lets you develop and test the solar/battery UI without real BLE hardware, and avoids the ~60 KB heap footprint of the NimBLE stack.

---

## Pitfalls and diagnostics

**Symptom: scan runs but no `0x10` data ever arrives**
→ Cause: `setActiveScan(false)` (passive scan). `0x10` only ever shows up in SCAN_RSP.
→ Fix: `s_bleScan->setActiveScan(true)`.

**Symptom: key-check mismatch (`pkt != key[0]`)**
→ Cause: the user changed the charger's PIN → the encryption key changed.
→ Fix: read the new key from VictronConnect → Product info.

**Symptom: voltage sentinel `0x7FFF`**
→ The charger hasn't populated data yet. Wait for the next scan cycles.

**Memory pressure when NimBLE + WiFi coexist**
→ ESP32-P4 has 32 MB PSRAM, so this is much less of a concern than on the old C5 board, but: keep `LV_MEM_SIZE` reasonable, initialise BLE before WiFi if possible, and prefer running NimBLE on the C6 co-processor where available rather than on the P4 itself.
