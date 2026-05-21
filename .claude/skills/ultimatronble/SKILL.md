# Ultimatron BLE — GATT protocol for LiFePO4 BMS

Technical reference for BLE GATT communication with the Ultimatron BMS.
Reference implementation: https://github.com/sergkh/node-ultimatron-battery

---

## Key difference vs Victron

| | Victron | Ultimatron |
|---|---|---|
| Type | Advertisement scan (active scan for SCAN_RSP) | GATT (active connection) |
| Encryption | AES-128, user-configured | None — data in the clear |
| Polling | Passive, always listening | Active: connect → write → read → disconnect |

---

## GATT service and characteristics

| UUID | Type | Purpose |
|------|------|---------|
| `ff00` | Service | BMS data |
| `ff01` | Characteristic (Notify) | BMS response — subscribe here |
| `ff02` | Characteristic (Write) | Send query command to BMS |

---

## Query protocol

### Command to write to `ff02`

```
DD A5 03 00 FF FD 77
```

- `DD` = frame start
- `A5 03` = read register 0x03 (basic info)
- `00` = no additional data
- `FF FD` = checksum (two's complement of sum)
- `77` = frame end

### Response on `ff01` (notification)

```
DD 03 00 <len> [data...] <chkH> <chkL> 77
```

- `DD 03 00` = response header for register 3
- `<len>` = data length (typically 0x1C = 28 bytes)
- `<chkH> <chkL>` = 16-bit checksum
- `77` = frame end
- Total length = 4 + len + 3 bytes

**Important:** the response arrives in **two separate BLE notifications**. The first starts with `DD 03` and the second carries the remainder. Accumulate until `4 + d[3] + 3` bytes total are received.

---

## Payload offsets (full packet, byte index from 0)

Verified against `battery.ts` → `processBatteryData()` and `getTemperatures()`.

| Offset | Type     | Scale | Field |
|--------|----------|-------|-------|
| [0]    | —        | —     | `0xDD` (header) |
| [1]    | —        | —     | `0x03` (register) |
| [2]    | —        | —     | `0x00` (status OK) |
| [3]    | —        | —     | data length |
| [4-5]  | uint16BE | /100 → V | pack voltage |
| [6-7]  | int16BE  | /100 → A | current (positive = charging, negative = discharging) |
| [8-9]  | uint16BE | /100 → Ah | remaining capacity |
| [10-11]| uint16BE | /100 → Ah | nominal capacity |
| [12-13]| uint16BE | — | cycle count |
| [14-15]| uint16BE | — | production date |
| [16-19]| — | — | (reserved / undocumented) |
| [20-21]| uint16BE | — | state protection flags |
| [22-23]| uint16BE | — | software version |
| **[23]** | **uint8** | **%** | **residual SOC (0–100%)** |
| [24]   | uint8 | bit0=charging, bit1=discharging | status flags |
| [25]   | uint8 | — | battery number |
| [26]   | uint8 | — | **number of NTC temperature sensors** |
| [27-28]| int16BE | (val-2731)/10 → °C | NTC temp 1 |
| [29-30]| int16BE | (val-2731)/10 → °C | NTC temp 2 (if sensors > 1) |

> [23] overlaps with the low byte of swVersion[22-23] — the BMS places SOC there directly.

### Current sign convention

The BMS uses signed int16 (two's complement): positive = charging, negative = discharging. Opposite of Victron.

```cpp
int16_t rawA = (int16_t)(((uint16_t)d[6] << 8) | d[7]);
float battA = rawA / 100.0f;
```

### Temperature

d[26] is the sensor count. First sensor starts at d[27]:

```cpp
// d[26] = number of NTC sensors; first sensor at d[27..28]
int16_t rawT = (int16_t)(((uint16_t)d[27] << 8) | d[28]);
float tempC = (rawT - 2731) / 10.0f;   // Kelvin×10 → °C
```

---

## TruMinus implementation (src/ultimatronble.cpp)

- `ultimatronBleInit()`: loads MAC from NVS, creates mutex + semaphore, launches `ultimatronTask`
- `ultimatronTask`: waits 22 s (WiFi/Victron settle), then calls `pollUltratron()` every 30 s
- `pollUltratron()`: suspends Victron scan → GATT connect → subscribe notify → write cmd → wait semaphore (4 s timeout) → disconnect → parse → resume Victron
- `notifyCb()`: accumulates bytes in buffer, gives semaphore when frame is complete
- NVS: namespace `"batt"`, key `"addr"` (12 uppercase hex chars, no colons)

### LCD integration

- `p4UpdateBatt(const P4BattData& d)` in `p4display.cpp` refreshes the SOC % label and the vertical battery icon fill (height proportional to SOC; colour: green ≥50%, amber 20–49%, red <20%). On the previous C5 board this was `cydUpdateBatt` in `cyddisplay.cpp`; treat that as a porting reference, not the current API.
- Data is broadcast to web clients every 10 s by `publishSolarBatt()` in `main.cpp`.
- The setup screen (formerly `runSolarSetup` in `wifisetup.cpp`; will be re-ported to the P4 settings flow) lets the user enter the battery MAC by hand; **it works even when BLE is not compiled in**.

### Simulated stubs (BLE disabled)

When `-DENABLE_BLE` is NOT defined, `ultimatronble.cpp` compiles to stubs:
- `ultimatronBleInit()` → no-op.
- `ultimatronBleGetData()` → returns an oscillating fake SOC via `sinf()`.
- This makes it possible to exercise the battery panel without real BLE hardware and without the ~60 KB heap footprint of the NimBLE stack.

---

## Device name

Format: Ultimatron serial number, e.g. `12100AE2100111`.
No specific manufacturer ID in the advertisement, so the setup scan shows all BLE devices found nearby.

---

## Pitfalls

**Connection timeout**
→ Verify the BMS is powered on and not already connected to another device (phone/app).
→ Try both BLE address types (PUBLIC / RANDOM); the module defaults to PUBLIC.

**No response to command (4 s timeout)**
→ Use `writeValue(cmd, sizeof(cmd), false)` — **Write Without Response** is required.  
  The BMS does not send an ATT write ACK; with `true` (Write Request) it may silently ignore the command.  
  Reference: `battery.ts` uses `writeChar.write(cmd, true)` where in noble `true` = `withoutResponse`.
→ Response arrives in **two separate BLE notifications** — first starts `DD 03`, second carries the rest.  
  Accumulate until `4 + d[3] + 3` total bytes before processing.
→ Verify UUIDs: some clones use different service UUIDs.

**RAM**: the GATT client uses more heap than passive scanning. Initialize before `WiFi.begin()` (same as Victron).
