// TruMinus-BLESim — ESP32-C3 simulator for Victron SmartSolar + Ultimatron BMS.
//
// One BLE identity, one MAC.  Advertises Victron Instant Readout manuf-data
// (re-encrypted every 2 s with cycling values) and exposes the Ultimatron
// GATT service (0xFF00 / FF01 notify / FF02 write).
//
// On the TruMinus P4, set the same MAC for both `solar/addr` and `batt/addr`
// in NVS, and the AES key below (32-hex) as `solar/key`.

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "aes/esp_aes.h"

#include "driver/usb_serial_jtag.h"
#include <stdlib.h>
#include <ctype.h>

#include "truma_sim.h"

// LIN pins to the Truma consumer board (TTL direct, no transceiver).
#define TRUMA_LIN_TX_GPIO  21
#define TRUMA_LIN_RX_GPIO  20

static const char* TAG = "blesim";

// ── Configuration (match P4 NVS) ─────────────────────────────────────────
static const uint8_t s_aes_key[16] = {
    0x00,0x11,0x22,0x33, 0x44,0x55,0x66,0x77,
    0x88,0x99,0xAA,0xBB, 0xCC,0xDD,0xEE,0xFF,
};
#define DEVICE_NAME "TruMinus-BLESim"

// ── State ────────────────────────────────────────────────────────────────
static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_attr_ff01    = 0;
static uint16_t s_attr_ff02    = 0;
static uint16_t s_iv           = 0;

static volatile float   s_battV     = 13.20f;
static volatile float   s_battA     =  5.50f;
static volatile float   s_pvW       = 120.0f;
static volatile float   s_kWhToday  =   1.25f;
static volatile uint8_t s_state     = 3;
static volatile uint8_t s_soc       = 75;     // BMS SOC %
static volatile float   s_tempC     = 20.0f;  // BMS NTC1
static volatile uint8_t s_tankPct   = 60;     // BTHome tank level %, 0..100
static volatile uint8_t s_tankSeq   = 0;      // BTHome packet-id (dedup)
static volatile bool    s_autocycle = true;   // sinf() updater on/off

static void advertise(void);
static void update_adv_data(void);

// ── Victron Instant Readout payload ──────────────────────────────────────
static int build_victron_mfr(uint8_t* out /*26 bytes*/) {
    out[0] = 0xE1; out[1] = 0x02;          // manufacturer id = 0x02E1
    out[2] = 0x10;                          // product type marker
    out[3] = 0xA0; out[4] = 0xA0;          // model PID (anything)
    out[5] = 0x02; out[6] = 0x01;
    out[7] = (uint8_t)(s_iv & 0xFF);       // IV lo  → AES-CTR nonce[0]
    out[8] = (uint8_t)((s_iv >> 8) & 0xFF);// IV hi  → AES-CTR nonce[1]
    out[9] = s_aes_key[0];                  // P4 sanity check
    s_iv++;

    uint8_t plain[16];
    memset(plain, 0xFF, sizeof(plain));
    int16_t  rv = (int16_t)lroundf(s_battV    * 100.0f);
    int16_t  ra = (int16_t)lroundf(s_battA    *  10.0f);
    uint16_t rk = (uint16_t)lroundf(s_kWhToday * 100.0f);
    uint16_t rp = (uint16_t)lroundf(s_pvW);
    plain[0] = s_state;
    plain[1] = 0;                                          // error code
    plain[2] = rv & 0xFF; plain[3] = (rv >> 8) & 0xFF;     // battV LE
    plain[4] = ra & 0xFF; plain[5] = (ra >> 8) & 0xFF;     // battA LE
    plain[6] = rk & 0xFF; plain[7] = (rk >> 8) & 0xFF;     // kWh LE
    plain[8] = rp & 0xFF; plain[9] = (rp >> 8) & 0xFF;     // pvW LE

    uint8_t nonce[16]  = {0};
    uint8_t stream[16] = {0};
    size_t  nc_off     = 0;
    nonce[0] = out[7];
    nonce[1] = out[8];
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, s_aes_key, 128);
    esp_aes_crypt_ctr(&ctx, 16, &nc_off, nonce, stream, plain, out + 10);
    esp_aes_free(&ctx);
    return 26;
}

// ── Adv / scan-rsp data ──────────────────────────────────────────────────
// Put the Victron manufacturer payload in ADV_IND so passive scanners
// (and the P4 parser) see it directly. Name + Ultimatron service UUID
// go in the SCAN_RSP.
static void update_adv_data(void) {
    uint8_t mfr[26];
    build_victron_mfr(mfr);
    uint8_t adv[31];
    int p = 0;
    adv[p++] = 2;
    adv[p++] = 0x01;
    adv[p++] = 0x06;                       // LE general disc | BR/EDR not supported
    adv[p++] = 1 + 26;
    adv[p++] = 0xFF;                       // manufacturer specific
    memcpy(adv + p, mfr, 26); p += 26;
    int rc = ble_gap_adv_set_data(adv, p);
    if (rc) ESP_LOGW(TAG, "adv_set_data rc=%d", rc);
}

static void update_scan_rsp(void) {
    uint8_t sr[31];
    int p = 0;
    sr[p++] = 3;
    sr[p++] = 0x03;                        // complete list of 16-bit UUIDs
    sr[p++] = 0x00; sr[p++] = 0xFF;        // 0xFF00 (Ultimatron service)
    const char* name = DEVICE_NAME;
    int nlen = (int)strlen(name);
    if (nlen > 24) nlen = 24;
    sr[p++] = 1 + nlen;
    sr[p++] = 0x09;                        // complete local name
    memcpy(sr + p, name, nlen); p += nlen;
    int rc = ble_gap_adv_rsp_set_data(sr, p);
    if (rc) ESP_LOGW(TAG, "adv_rsp_set_data rc=%d", rc);
}

// BTHome v2 unencrypted service-data (UUID 0xFCD2).
// Layout in adv:
//   AD: 02 01 06                              Flags
//   AD: 03 03 D2 FC                           Complete list of 16-bit UUIDs (BTHome)
//   AD: 08 16 D2 FC 40 00 <seq> 2F <pct>      Service Data:
//        - UUID 0xFCD2 (BTHome) little-endian
//        - Device info 0x40 = v2, unencrypted, no trigger
//        - Tag 0x00 (Packet ID, uint8) + seq
//        - Tag 0x2F (Moisture, uint8 0..100%) + pct  ← tank level
// Total 16 bytes, well below the 31-byte legacy adv limit.
static void update_adv_data_bthome(void) {
    uint8_t adv[31];
    int p = 0;
    // Flags
    adv[p++] = 2;
    adv[p++] = 0x01;
    adv[p++] = 0x06;
    // 16-bit Service UUID list (helps generic BLE scanners — incl. HA — pick
    // the device up without having to parse Service Data first).
    adv[p++] = 3;
    adv[p++] = 0x03;
    adv[p++] = 0xD2; adv[p++] = 0xFC;
    // Service Data — BTHome v2 unencrypted
    adv[p++] = 8;
    adv[p++] = 0x16;
    adv[p++] = 0xD2; adv[p++] = 0xFC;          // UUID 0xFCD2 LE
    adv[p++] = 0x40;                            // device info: v2, unencrypted
    adv[p++] = 0x00; adv[p++] = s_tankSeq;     // packet ID tag + value
    adv[p++] = 0x2F; adv[p++] = s_tankPct;     // moisture (uint8 0..100%)
    int rc = ble_gap_adv_set_data(adv, p);
    if (rc) ESP_LOGW(TAG, "adv_set_data (bthome) rc=%d", rc);
}

// ── GATT 0xFF00 / FF01 (notify) / FF02 (write) ───────────────────────────
static void send_bms_response(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    // JBD frame: DD 03 00 <len> [data...] <chkH> <chkL> 77  → total = 4 + len + 3
    uint8_t pkt[32];
    pkt[0] = 0xDD; pkt[1] = 0x03; pkt[2] = 0x00;
    pkt[3] = 0x19;                                      // len = 25 data bytes
    uint16_t v = (uint16_t)lroundf(s_battV * 100.0f);
    int16_t  a = (int16_t)lroundf(s_battA * 100.0f);
    pkt[4] = (v >> 8) & 0xFF; pkt[5] = v & 0xFF;        // voltage *100 BE
    pkt[6] = (a >> 8) & 0xFF; pkt[7] = a & 0xFF;        // current *100 BE
    memset(pkt + 8, 0, 15);                              // 8..22 reserved
    pkt[23] = s_soc;                                     // residual SOC %
    pkt[24] = 0;                                         // status flags
    pkt[25] = 1;                                         // battery number
    pkt[26] = 1;                                         // NTC sensor count
    int16_t tk = (int16_t)lroundf(s_tempC * 10.0f + 2731.0f);
    pkt[27] = (tk >> 8) & 0xFF; pkt[28] = tk & 0xFF;    // tempC raw BE
    // 16-bit checksum: two's complement of sum of bytes [2..28]
    uint16_t sum = 0;
    for (int i = 2; i <= 28; i++) sum += pkt[i];
    uint16_t chk = (uint16_t)(0x10000 - sum);
    pkt[29] = (chk >> 8) & 0xFF; pkt[30] = chk & 0xFF;
    pkt[31] = 0x77;                                      // frame end
    struct os_mbuf* om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    if (om) ble_gatts_notify_custom(s_conn_handle, s_attr_ff01, om);
}

static int gatt_ff02_access(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[16];
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        ESP_LOGI(TAG, "FF02 write len=%u %02X %02X %02X %02X",
                 (unsigned)len,
                 len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0,
                 len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);
        // Standard JBD/Ultimatron read-basic-info: DD A5 03 00 FF FD 77
        if (len >= 4 && buf[0] == 0xDD && buf[1] == 0xA5 && buf[2] == 0x03) {
            send_bms_response();
        }
        // Optional auth: DD 5A 60 06 <6 password> <chk hi/lo> 77 — accept silently
    }
    return 0;
}

static int gatt_ff01_access(uint16_t conn, uint16_t attr,
                            struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFF00),
        .characteristics = (struct ble_gatt_chr_def[]) {{
            .uuid       = BLE_UUID16_DECLARE(0xFF01),
            .access_cb  = gatt_ff01_access,
            .val_handle = &s_attr_ff01,
            .flags      = BLE_GATT_CHR_F_NOTIFY,
        }, {
            .uuid       = BLE_UUID16_DECLARE(0xFF02),
            .access_cb  = gatt_ff02_access,
            .val_handle = &s_attr_ff02,
            .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        }, { 0 }},
    },
    { 0 },
};

// ── GAP event ────────────────────────────────────────────────────────────
static int gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "GAP connect ok, handle=%u", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect status=%d", event->connect.status);
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "GAP disconnect reason=0x%02x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    update_adv_data();
    update_scan_rsp();
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min  = 0x30;   // 30 ms
    p.itvl_max  = 0x60;   // 60 ms
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &p, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

// ── Host sync / reset ────────────────────────────────────────────────────
static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "Device MAC: %02X:%02X:%02X:%02X:%02X:%02X  type=%d",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             s_own_addr_type);
    ble_svc_gap_device_name_set(DEVICE_NAME);
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

// ── Value updater ────────────────────────────────────────────────────────
// Alternates the legacy adv payload between Victron mfr-data and BTHome
// service-data every 2 s.  Both share the same SCAN_RSP (name + 0xFF00
// Ultimatron service), so the device's identity stays constant.  In a 5 s
// P4 scan window each payload is always present at least once.
static void updater_task(void* arg) {
    (void)arg;
    bool bthome_turn = false;
    while (1) {
        if (s_autocycle) {
            float t = (float)(esp_timer_get_time() / 1000000ULL);
            s_battV    = 13.0f  + 0.3f  * sinf(t * 0.6f);
            s_battA    =  4.0f  + 3.0f  * sinf(t * 0.4f);
            s_pvW      = 100.0f + 60.0f * sinf(t * 0.7f);
            s_kWhToday =   1.0f + (t / 3600.0f);
            // Tank level: oscillates 25..95 % over ~10 min so the P4 sees
            // visible changes without flooding the WS broadcast diff filter.
            float pctF = 60.0f + 35.0f * sinf(t * 0.01f);
            if (pctF < 0.0f)   pctF = 0.0f;
            if (pctF > 100.0f) pctF = 100.0f;
            s_tankPct = (uint8_t)lroundf(pctF);
        }
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            if (bthome_turn) {
                s_tankSeq++;          // bump BTHome packet ID each emission
                update_adv_data_bthome();
            } else {
                update_adv_data();
            }
            bthome_turn = !bthome_turn;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ── Serial REPL over USB-CDC ─────────────────────────────────────────────
static void print_status(void) {
    printf("V=%.2f A=%.2f W=%.1f kWh=%.3f state=%u SOC=%u T=%.1f tank=%u%% auto=%s\r\n",
           s_battV, s_battA, s_pvW, s_kWhToday,
           (unsigned)s_state, (unsigned)s_soc, s_tempC, (unsigned)s_tankPct,
           s_autocycle ? "on" : "off");
}

static void handle_line(char* line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;
    char* cmd = strtok(line, " \t");
    char* arg = strtok(NULL, " \t");
    if (!cmd) return;
    for (char* p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strcmp(cmd, "?") == 0 || strcmp(cmd, "help") == 0) {
        printf("solar/bms: v <V> | a <A> | p <W> | k <kWh> | s <state>"
               " | soc <%%> | t <degC> | auto on|off | status\r\n");
        printf("truma:     troom <C> | twater <C> | tburn 0|1 |"
               " terr <class> [code [short]] | tstatus\r\n");
        printf("tank:      tank <pct 0..100>     (BTHome moisture adv)\r\n");
        return;
    }
    if (strcmp(cmd, "status") == 0)  { print_status(); return; }
    if (strcmp(cmd, "tstatus") == 0) { truma_sim_print_status(); return; }
    if (strcmp(cmd, "tping") == 0)   { truma_sim_ping(); printf("tx 0x21\r\n"); return; }
    if (strcmp(cmd, "ttoggle") == 0) {
        // Drive TRUMA_LIN_TX_GPIO as plain GPIO @ ~1 kHz for 2 s to
        // verify rail-to-rail swing on the scope without UART involved.
        // Note: temporarily detaches the UART driver from the pin.
        extern void truma_sim_ttoggle(int gpio, int ms);
        truma_sim_ttoggle(TRUMA_LIN_TX_GPIO, 2000);
        printf("toggled GPIO%d for 2 s\r\n", TRUMA_LIN_TX_GPIO);
        return;
    }
    if (strcmp(cmd, "auto") == 0) {
        if (!arg) { printf("auto=%s\r\n", s_autocycle ? "on" : "off"); return; }
        s_autocycle = (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
        printf("auto=%s\r\n", s_autocycle ? "on" : "off");
        return;
    }
    if (!arg) { printf("missing value (try '?')\r\n"); return; }

    if      (strcmp(cmd, "v") == 0)   s_battV    = strtof(arg, NULL);
    else if (strcmp(cmd, "a") == 0)   s_battA    = strtof(arg, NULL);
    else if (strcmp(cmd, "p") == 0)   s_pvW      = strtof(arg, NULL);
    else if (strcmp(cmd, "k") == 0)   s_kWhToday = strtof(arg, NULL);
    else if (strcmp(cmd, "s") == 0)   s_state    = (uint8_t)strtol(arg, NULL, 0);
    else if (strcmp(cmd, "soc") == 0) s_soc      = (uint8_t)strtol(arg, NULL, 0);
    else if (strcmp(cmd, "t") == 0)   s_tempC    = strtof(arg, NULL);
    else if (strcmp(cmd, "tank") == 0) {
        long v = strtol(arg, NULL, 0);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        s_tankPct = (uint8_t)v;
        s_autocycle = false;          // freeze the sinusoid so the value sticks
        printf("tank=%u%% (autocycle off)\r\n", (unsigned)s_tankPct);
        return;
    }
    else if (strcmp(cmd, "troom") == 0) {
        truma_sim_set_room_temp(strtof(arg, NULL));
        truma_sim_print_status();
        return;
    }
    else if (strcmp(cmd, "twater") == 0) {
        truma_sim_set_water_temp(strtof(arg, NULL));
        truma_sim_print_status();
        return;
    }
    else if (strcmp(cmd, "tburn") == 0) {
        truma_sim_set_burner(strtol(arg, NULL, 0) != 0);
        truma_sim_print_status();
        return;
    }
    else if (strcmp(cmd, "terr") == 0) {
        char* a2 = strtok(NULL, " \t");
        char* a3 = strtok(NULL, " \t");
        uint8_t k = (uint8_t)strtol(arg, NULL, 0);
        uint8_t c = a2 ? (uint8_t)strtol(a2, NULL, 0) : 0;
        uint8_t s = a3 ? (uint8_t)strtol(a3, NULL, 0) : 0;
        truma_sim_set_error(k, c, s);
        truma_sim_print_status();
        return;
    }
    else { printf("unknown: %s (try '?')\r\n", cmd); return; }
    print_status();
}

static void console_task(void* arg) {
    (void)arg;
    char line[96];
    size_t pos = 0;
    printf("\r\nblesim REPL — type '?' for help\r\n");
    print_status();
    while (1) {
        uint8_t b;
        int n = usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue;
        if (b == '\r' || b == '\n') {
            if (pos > 0) {
                line[pos] = 0;
                printf("\r\n");
                handle_line(line);
                pos = 0;
            } else {
                printf("\r\n");
            }
        } else if ((b == 0x08 || b == 0x7F) && pos > 0) {
            pos--;
            printf("\b \b");
            fflush(stdout);
        } else if (b >= 0x20 && b < 0x7F && pos < sizeof(line) - 1) {
            line[pos++] = (char)b;
            usb_serial_jtag_write_bytes(&b, 1, 0);   // local echo
        }
    }
}

// ── Host task ────────────────────────────────────────────────────────────
static void ble_host_task(void* param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ── app_main ─────────────────────────────────────────────────────────────
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&ucfg);

    nimble_port_freertos_init(ble_host_task);
    xTaskCreate(updater_task, "upd", 4096, NULL, 2, NULL);
    xTaskCreate(console_task, "con", 4096, NULL, 3, NULL);

    truma_sim_init(TRUMA_LIN_TX_GPIO, TRUMA_LIN_RX_GPIO);
}
