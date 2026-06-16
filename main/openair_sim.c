// OpenAir PLUS (Bergstrom/Dirna A/C) BLE peripheral simulator — see header.

#include "openair_sim.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"

static const char* TAG = "openair";

// ── GATT UUIDs (128-bit, declared little-endian as NimBLE expects) ────────
// e43ff2c2-8602-48f6-82d0-72cd56fb06f2  (service)
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0xf2, 0x06, 0xfb, 0x56, 0xcd, 0x72, 0xd0, 0x82,
    0xf6, 0x48, 0x02, 0x86, 0xc2, 0xf2, 0x3f, 0xe4);
// 9d667ea8-9c95-4dd0-b952-92031c4f5375  (write — commands from the app)
// The app calls writeId()/writeToCharacteristic() on this UUID.
static const ble_uuid128_t UUID_WRITE = BLE_UUID128_INIT(
    0x75, 0x53, 0x4f, 0x1c, 0x03, 0x92, 0x52, 0xb9,
    0xd0, 0x4d, 0x95, 0x9c, 0xa8, 0x7e, 0x66, 0x9d);
// 4a01b4dd-350d-4afc-9a9f-27164f2b6b56  (notify — state to the app)
// The app calls setNotifyValue()/onValueReceived() on this UUID.
static const ble_uuid128_t UUID_NOTIFY = BLE_UUID128_INIT(
    0x56, 0x6b, 0x2b, 0x4f, 0x16, 0x27, 0x9f, 0x9a,
    0xfc, 0x4a, 0x0d, 0x35, 0xdd, 0xb4, 0x01, 0x4a);

// ── Frame model ───────────────────────────────────────────────────────────
// 28 fields, canonical order recovered from Mensaje.dart.  Indices 0..12 are
// the writable command fields; 13..27 are read-only telemetry.
#define OA_NFIELDS   28
#define OA_NWRITE    13

static const char* const FIELD[OA_NFIELDS] = {
    "RealTimeClock", "BatteryType", "Power", "TempScale", "PowerState",
    "Mode", "Temp", "BlowerSpeed", "LedBright", "LedColor", "ScheduledTime",
    "Flaps1Mode", "Flaps2Mode",
    "Errors", "BatteryValue", "ElectroSpeed", "CompressorSpeed",
    "BlowerSpeedPer", "ElectroSpeedPer", "CompressorSpeedRPM", "TiltX",
    "TiltY", "Sonda1F", "Sonda1C", "Sonda2F", "Sonda2C",
    "VersionBase", "VersionFrontal",
};

// Telemetry field values + per-field byte width.  Width is unknown for the
// multi-byte time fields (RealTimeClock / ScheduledTime); default everything
// to 1 byte and grow with `oaw <idx> <bytes>` until the app stops logging the
// "datos recibidos no coinciden en longitud" length error.  Values are raw
// device bytes (no scaling known yet) — set them with `oaset` and watch how
// the app renders them to calibrate the read path.
static uint32_t s_val[OA_NFIELDS];
static uint8_t  s_width[OA_NFIELDS];

static uint16_t s_conn   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_notify = 0;       // notify char value handle
static bool     s_subscribed = false; // central enabled notifications
static bool     s_auto   = true;      // periodic telemetry resend

static void defaults(void) {
    for (int i = 0; i < OA_NFIELDS; i++) { s_val[i] = 0; s_width[i] = 1; }
    // A few plausible non-zero starting values so the app shows "something".
    s_val[2]  = 1;     // Power (available/enabled)
    s_val[4]  = 1;     // PowerState (1 = on)
    s_val[5]  = 0;     // Mode (0 = AUTO, 2 = MAN)
    s_val[6]  = 210;   // Temp = 21.0 C (scale x10)
    s_val[7]  = 1;     // BlowerSpeed
}

// ── Telemetry builder ─────────────────────────────────────────────────────
// Serialise the read-frame as ASCII-hex (big-endian per field, 2 chars/byte).
// The app's parser (deAsciiAInt + Mensaje.cargarDesdeDispositivo) requires a
// frame of exactly 124 chars: it decodes 60 data bytes into fields and discards
// the last 4 chars (2 trailer bytes). Layout recovered by feeding an
// incremental probe and reading the field dump in logcat:
//
//   bytes  0-3   RealTimeClock (4B)      bytes 30-31  Errors
//   bytes  4-5   BatteryType             bytes 32-33  BatteryValue
//   bytes  6-7   Power                   bytes 34-35  ElectroSpeed
//   bytes  8-9   TempScale               bytes 36-37  CompressorSpeed
//   bytes 10-11  PowerState              bytes 38-39  BlowerSpeedPer
//   bytes 12-13  Mode                    bytes 40-41  ElectroSpeedPer
//   bytes 14-15  Temp                    bytes 42-43  CompressorSpeedRPM
//   bytes 16-17  BlowerSpeed             bytes 44-47  TiltX, TiltY
//   bytes 18-19  LedBright               bytes 48-55  Sonda1F/1C/2F/2C
//   bytes 20-21  (unnamed/reserved)      bytes 56-59  VersionBase/Frontal
//   bytes 22-23  LedColor                bytes 60-61  trailer (discarded)
//   bytes 24-29  ScheduledTime, Flaps1Mode, Flaps2Mode
//
// Every field is 2 bytes except RealTimeClock (4). The reserved slot at 20-21
// is not surfaced by the app; emit zeros.
static int build_frame(char* out, int cap) {
    int n = 0;
    #define OA_PUT(byte) do { \
        if (n + 2 < cap) n += sprintf(out + n, "%02x", (uint8_t)(byte)); } while (0)
    #define OA_PUTV(idx, w) do { \
        for (int b = (w) - 1; b >= 0; b--) OA_PUT((s_val[idx] >> (8 * b)) & 0xFF); } while (0)
    OA_PUTV(0, 4);                                   // RealTimeClock  (0-3)
    for (int i = 1; i <= 8; i++) OA_PUTV(i, 2);      // BatteryType..LedBright (4-19)
    OA_PUT(0); OA_PUT(0);                            // reserved (20-21)
    for (int i = 9; i <= 27; i++) OA_PUTV(i, 2);     // LedColor..VersionFrontal (22-59)
    OA_PUT(0); OA_PUT(0);                            // trailer (60-61, discarded)
    #undef OA_PUT
    #undef OA_PUTV
    out[n] = 0;
    return n;
}

static void notify_payload(const uint8_t* data, int len) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_h_notify == 0) {
        ESP_LOGW(TAG, "notify skipped (conn=%u h=%u)", s_conn, s_h_notify);
        return;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (!om) { ESP_LOGW(TAG, "mbuf alloc failed"); return; }
    int rc = ble_gatts_notify_custom(s_conn, s_h_notify, om);
    if (rc) ESP_LOGW(TAG, "notify rc=%d", rc);
}

static void send_telemetry(bool log) {
    char frame[2 * OA_NFIELDS * 4 + 1];
    int len = build_frame(frame, sizeof(frame));
    // Periodic keep-alive resends log at DEBUG so they don't bury the WRITE
    // captures in the monitor; user-initiated sends (oasend / subscribe seed)
    // log at INFO.
    if (log) ESP_LOGI(TAG, "TX telemetry (%d chars): %s", len, frame);
    else     ESP_LOGD(TAG, "TX telemetry (%d chars): %s", len, frame);
    notify_payload((const uint8_t*)frame, len);
}

void openair_sim_send_telemetry(void) { send_telemetry(true); }

void openair_sim_tick(void) {
    if (s_auto && s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed)
        send_telemetry(false);
}

// ── Write capture (the whole point) ───────────────────────────────────────
static int is_ascii_hex(const uint8_t* b, int n) {
    if (n == 0 || (n & 1)) return 0;
    for (int i = 0; i < n; i++) if (!isxdigit(b[i])) return 0;
    return 1;
}

static void log_command(const uint8_t* b, int n) {
    ESP_LOGI(TAG, "──── WRITE from app: %d bytes ────", n);
    ESP_LOG_BUFFER_HEX(TAG, b, n);

    // Printable view (helps confirm raw-bytes vs ASCII-hex on the wire).
    char ascii[128];
    int a = 0;
    for (int i = 0; i < n && a < (int)sizeof(ascii) - 1; i++)
        ascii[a++] = (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
    ascii[a] = 0;
    ESP_LOGI(TAG, "ascii: \"%s\"", ascii);

    if (!is_ascii_hex(b, n)) {
        ESP_LOGI(TAG, "(not even-length ASCII-hex — likely raw bytes; "
                      "the %d bytes above ARE the frame)", n);
        return;
    }

    // Decode ASCII-hex → bytes, then map onto the command-frame layout (the
    // same big-endian layout as the read frame): RealTimeClock is 4 bytes, the
    // next 12 command fields are 2 bytes each, followed by a 2-byte trailer.
    // 30 bytes total. Temp is in tenths of a degree (e.g. 230 = 23.0 C).
    uint8_t dec[64];
    int dn = 0;
    for (int i = 0; i + 1 < n && dn < (int)sizeof(dec); i += 2) {
        char pair[3] = { (char)b[i], (char)b[i + 1], 0 };
        dec[dn++] = (uint8_t)strtol(pair, NULL, 16);
    }
    char line[256]; int p = 0;
    p += snprintf(line + p, sizeof(line) - p, "decoded %d bytes:", dn);
    for (int i = 0; i < dn; i++)
        p += snprintf(line + p, sizeof(line) - p, " [%d]=%u", i, dec[i]);
    ESP_LOGI(TAG, "%s", line);

    if (dn < 30) return;   // not the expected 13-field command frame
    // Command-frame layout (mirrors the read frame, including the reserved
    // 2-byte slot at offset 20): RealTimeClock is 4 bytes, the rest are 2-byte
    // big-endian. Temp is in tenths of a degree.
    #define OA_U16(o) ((dec[o] << 8) | dec[o + 1])
    uint32_t rtc = ((uint32_t)dec[0] << 24) | ((uint32_t)dec[1] << 16)
                 | ((uint32_t)dec[2] << 8)  | dec[3];
    int temp = OA_U16(14);
    ESP_LOGI(TAG, "RealTimeClock=%lu (%02lu:%02lu:%02lu)  BatteryType=%d  Power=%d  "
                  "TempScale=%d", (unsigned long)rtc,
             (unsigned long)(rtc / 3600), (unsigned long)((rtc / 60) % 60),
             (unsigned long)(rtc % 60), OA_U16(4), OA_U16(6), OA_U16(8));
    ESP_LOGI(TAG, "PowerState=%d  Mode=%d  Temp=%d (%d.%d C)  BlowerSpeed=%d",
             OA_U16(10), OA_U16(12), temp, temp / 10, temp % 10, OA_U16(16));
    ESP_LOGI(TAG, "LedBright=%d  LedColor=%d  ScheduledTime=%d  Flaps1Mode=%d  "
                  "Flaps2Mode=%d", OA_U16(18), OA_U16(22), OA_U16(24),
             OA_U16(26), OA_U16(28));
    #undef OA_U16
}

static int write_cb(uint16_t conn, uint16_t attr,
                    struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGI(TAG, "==== HANDSHAKE write (9d667ea8, writeId device-id) ====");
        log_command(buf, len);
    }
    return 0;
}

static int notify_cb(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn; (void)attr; (void)arg;
    // The app reads this characteristic right after discovery (before it
    // subscribes), so it must be readable — return the current frame.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char frame[2 * OA_NFIELDS * 4 + 1];
        int len = build_frame(frame, sizeof(frame));
        ESP_LOGI(TAG, "app READ state char -> %d chars", len);
        int rc = os_mbuf_append(ctxt->om, frame, len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    // Commands from the app (writeMessage) land here, not on the 9d667ea8 char.
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGI(TAG, "==== COMMAND write (4a01b4dd) ====");
        log_command(buf, len);
    }
    return 0;
}

static void gatt_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg) {
    (void)arg;
    char buf[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG, "reg svc %s handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG, "reg chr %s val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf), ctxt->chr.val_handle);
        break;
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGI(TAG, "reg dsc %s handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
        break;
    }
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {{
            .uuid       = &UUID_WRITE.u,
            .access_cb  = write_cb,
            .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        }, {
            // 4a01b4dd is bidirectional: the app subscribes here for telemetry
            // (NOTIFY/READ) and also writes commands here — discoverServices
            // caches this UUID as the write target (field_4b, "CARAC DE ESC?")
            // and writeMessage() writes to it. So it must also be writable.
            .uuid       = &UUID_NOTIFY.u,
            .access_cb  = notify_cb,
            .val_handle = &s_h_notify,
            .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ
                        | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        }, { 0 }},
    },
    { 0 },
};

void openair_sim_register_gatt(void) {
    defaults();
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
}

// ── Advertising ───────────────────────────────────────────────────────────
void openair_sim_advertise(uint8_t own_addr_type,
                           int (*gap_cb)(struct ble_gap_event*, void*)) {
    // ADV: flags + complete 128-bit service UUID (so name/UUID scanners and
    // flutter_blue_plus find us).  Name goes in the scan response (active
    // scan) since flags+128-bit UUID already fill most of the 31-byte adv.
    uint8_t adv[31]; int p = 0;
    adv[p++] = 2; adv[p++] = 0x01; adv[p++] = 0x06;           // flags
    adv[p++] = 1 + 16; adv[p++] = 0x07;                        // 128-bit UUID list
    memcpy(adv + p, UUID_SVC.value, 16); p += 16;
    // Service Data (16-bit UUID). The app's scan handler does
    // advertisementData.serviceData.entries.first, so the advertisement must
    // carry at least one service-data entry or its connect routine throws and
    // drops the link after a ~7 s timeout. The app uses the first entry
    // generically (no fixed UUID), so the UUID/value here are placeholders.
    adv[p++] = 1 + 2 + 4; adv[p++] = 0x16;                     // Service Data - 16-bit
    adv[p++] = 0x9A; adv[p++] = 0xFC;                          // UUID 0xFC9A (placeholder)
    adv[p++] = 0x01; adv[p++] = 0x02; adv[p++] = 0x03; adv[p++] = 0x04;
    int rc = ble_gap_adv_set_data(adv, p);
    if (rc) ESP_LOGW(TAG, "adv_set_data rc=%d", rc);

    uint8_t sr[31]; int q = 0;
    const char* name = OPENAIR_DEVICE_NAME;
    int nlen = (int)strlen(name);
    sr[q++] = 1 + nlen; sr[q++] = 0x09;                        // complete name
    memcpy(sr + q, name, nlen); q += nlen;
    rc = ble_gap_adv_rsp_set_data(sr, q);
    if (rc) ESP_LOGW(TAG, "adv_rsp_set_data rc=%d", rc);

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min  = 0x30;
    ap.itvl_max  = 0x60;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_cb, NULL);
    if (rc) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else    ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

bool openair_sim_on_gap_event(struct ble_gap_event* event) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            ESP_LOGI(TAG, "app connected (handle=%u)", s_conn);
        }
        return true;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "app disconnected (reason=0x%02x)",
                 event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        return true;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_h_notify) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notify %s by app",
                     s_subscribed ? "ENABLED" : "disabled");
            if (s_subscribed) openair_sim_send_telemetry();  // seed the UI
        }
        return true;
    default:
        return false;
    }
}

// ── REPL ──────────────────────────────────────────────────────────────────
static void print_status(void) {
    printf("openair: conn=%s notify=%s auto=%s\r\n",
           s_conn == BLE_HS_CONN_HANDLE_NONE ? "no" : "yes",
           s_subscribed ? "on" : "off", s_auto ? "on" : "off");
    printf("  write fields (sent by app -> captured in log):\r\n");
    for (int i = 0; i < OA_NWRITE; i++)
        printf("   [%2d] %-18s w=%u val=%lu\r\n",
               i, FIELD[i], s_width[i], (unsigned long)s_val[i]);
    printf("  telemetry-only fields [13..27]: Errors..VersionFrontal\r\n");
}

bool openair_sim_console(const char* cmd, const char* arg, const char* arg2) {
    if (strcmp(cmd, "oa") == 0 || strcmp(cmd, "oastatus") == 0) {
        print_status(); return true;
    }
    if (strcmp(cmd, "oahelp") == 0) {
        printf("openair: oa(status) | oasend | oaauto on|off |"
               " oaset <idx> <val> | oaw <idx> <bytes> | oastr <asciihex>\r\n"
               "  idx: 0 RealTimeClock 2 Power 4 PowerState 5 Mode 6 Temp"
               " 7 BlowerSpeed 11/12 Flaps  (full list: 'oa')\r\n");
        return true;
    }
    if (strcmp(cmd, "oasend") == 0) { openair_sim_send_telemetry(); return true; }
    if (strcmp(cmd, "oaauto") == 0) {
        if (arg) s_auto = (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
        printf("openair auto=%s\r\n", s_auto ? "on" : "off");
        return true;
    }
    if (strcmp(cmd, "oaset") == 0) {
        if (!arg || !arg2) { printf("usage: oaset <idx> <val>\r\n"); return true; }
        int i = atoi(arg);
        if (i < 0 || i >= OA_NFIELDS) { printf("idx 0..27\r\n"); return true; }
        s_val[i] = (uint32_t)strtoul(arg2, NULL, 0);
        printf("%s = %lu\r\n", FIELD[i], (unsigned long)s_val[i]);
        if (s_auto) openair_sim_send_telemetry();
        return true;
    }
    if (strcmp(cmd, "oaw") == 0) {
        if (!arg || !arg2) { printf("usage: oaw <idx> <bytes>\r\n"); return true; }
        int i = atoi(arg), w = atoi(arg2);
        if (i < 0 || i >= OA_NFIELDS || w < 1 || w > 4) {
            printf("idx 0..27, bytes 1..4\r\n"); return true;
        }
        s_width[i] = (uint8_t)w;
        printf("%s width = %d byte(s)\r\n", FIELD[i], w);
        if (s_auto) openair_sim_send_telemetry();
        return true;
    }
    if (strcmp(cmd, "oastr") == 0) {
        if (!arg) { printf("usage: oastr <literal-payload>\r\n"); return true; }
        ESP_LOGI(TAG, "TX literal (%d chars): %s", (int)strlen(arg), arg);
        notify_payload((const uint8_t*)arg, (int)strlen(arg));
        return true;
    }
    return false;
}
