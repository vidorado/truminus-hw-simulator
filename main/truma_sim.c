// Truma Combi D LIN slave simulator — implementation.
//
// LIN frame on the wire:
//   BREAK (>=13 dominant bits) | SYNC 0x55 | PID | data[N] | CHK
// PID parity per LIN 2.x; enhanced checksum (includes PID) for all
// frames except 0x3C/0x3D which use the classic checksum (data only).
//
// We act as a passive slave.  Instead of relying on the ESP-IDF UART
// BREAK event (the default detection threshold is ~23 bit times and
// often misses LIN's 13-bit breaks), we sync on the 0x55 SYNC byte and
// validate the next byte by recomputing the PID parity.  False sync on
// random 0x55s in payload data is unlikely because the PID parity check
// rejects most of them, and a wrong frame is harmless (we either send
// no response or fail the checksum).

#include "truma_sim.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static const char* TAG = "truma";

#define LIN_UART        UART_NUM_1
#define LIN_BAUD        9600
#define RX_BUF_SIZE     512
#define TX_BUF_SIZE     256

// ── Slave state ──────────────────────────────────────────────────────────
static float   s_room_temp_c   = 21.5f;
static float   s_water_temp_c  = 48.0f;
static bool    s_burner        = false;

static uint8_t s_room_sp_lo    = 0xAA;
static uint8_t s_room_sp_hi    = 0xAA;
static uint8_t s_water_sp      = 0xAA;
static uint8_t s_modes         = 0x00;

static uint8_t s_pending_sid       = 0;
static uint8_t s_pending_req_state = 1;
static uint8_t s_req_state         = 1;
static uint8_t s_cur_state         = 1;
static uint8_t s_err_class         = 0;
static uint8_t s_err_code          = 0;
static uint8_t s_err_short         = 0;

// Diagnostic counters (visible from the REPL via `tstatus`).
static uint32_t s_evt_bytes   = 0;
static uint32_t s_evt_syncs   = 0;
static uint32_t s_evt_frames  = 0;
static uint32_t s_evt_sent    = 0;

// Cached pins so ttoggle can re-arm the UART driver after detaching it.
int s_lin_tx_gpio = -1;
int s_lin_rx_gpio = -1;

// ── LIN helpers ──────────────────────────────────────────────────────────
static uint8_t lin_pid(uint8_t id) {
    uint8_t p0 = ((id) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
    uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1;
    return (id & 0x3F) | (p0 << 6) | (p1 << 7);
}

static bool lin_pid_ok(uint8_t pid) {
    return lin_pid(pid & 0x3F) == pid;
}

static uint8_t lin_chk(uint8_t pid, const uint8_t* data, int n, bool enhanced) {
    uint16_t s = enhanced ? pid : 0;
    for (int i = 0; i < n; i++) {
        s += data[i];
        if (s >= 0x100) s -= 0xFF;
    }
    return (uint8_t)(~s & 0xFF);
}

static bool id_uses_classic_chk(uint8_t id) {
    return id == 0x3C || id == 0x3D;
}

static int frame_len(uint8_t id) {
    switch (id) {
    case 0x21: case 0x22: case 0x20: case 0x3C: case 0x3D: return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: return 2;
    case 0x16: return 8;
    default: return -1;
    }
}

static uint16_t kelvin_x10(float c) {
    int v = (int)lroundf((c + 273.0f) * 10.0f);
    if (v < 0)      v = 0;
    if (v > 0x0FFF) v = 0x0FFF;
    return (uint16_t)v;
}

static void send_frame(uint8_t id, const uint8_t* data, int n) {
    // The C5's Lin_Interface::readFrame() state machine expects to see
    // 0x00 (BREAK byte) + 0x55 (SYNC) + PID in its RX before the data
    // bytes — that loopback happens naturally on a single-wire LIN bus,
    // but our wiring has separate TX/RX so we have to fake it.
    uint8_t pid = lin_pid(id);
    uint8_t buf[16];
    int p = 0;
    buf[p++] = 0x00;
    buf[p++] = 0x55;
    buf[p++] = pid;
    memcpy(buf + p, data, n); p += n;
    buf[p++] = lin_chk(pid, data, n, !id_uses_classic_chk(id));
    uart_write_bytes(LIN_UART, (const char*)buf, p);
    s_evt_sent++;
}

// ── Publishers ───────────────────────────────────────────────────────────
static void respond_0x21(void) {
    uint16_t r = kelvin_x10(s_room_temp_c);
    uint16_t w = kelvin_x10(s_water_temp_c);
    uint8_t d[8] = {0};
    d[0] = r & 0xFF;
    d[1] = ((r >> 8) & 0x0F) | ((w & 0x0F) << 4);
    d[2] = (w >> 4) & 0xFF;
    d[3] = 0xB8;
    send_frame(0x21, d, 8);
}

static void respond_0x22(void) {
    uint8_t d[8] = {0};
    d[0] = 0x81;
    d[1] = s_burner ? 0x40 : 0xD0;
    d[2] = s_burner ? 48 : 0x10;
    send_frame(0x22, d, 8);
}

static void respond_0x3D(void) {
    uint8_t d[8];
    memset(d, 0xFF, 8);
    if (s_pending_sid == 0xB8) {
        d[0] = 0x01; d[1] = 0x06; d[2] = 0xF8;
        d[3] = s_pending_req_state;
        d[4] = s_cur_state;
        d[5] = d[6] = d[7] = 0;
        s_cur_state = s_pending_req_state;
    } else if (s_pending_sid == 0xB2) {
        d[0] = 0x7F; d[1] = 0x06; d[2] = 0xF2;
        d[3] = 0;
        d[4] = s_err_class;
        d[5] = s_err_code;
        d[6] = s_err_short;
        d[7] = 0;
    } else {
        return;
    }
    send_frame(0x3D, d, 8);
    s_pending_sid = 0;
}

// ── Consumers ────────────────────────────────────────────────────────────
static void on_master_frame(uint8_t id, const uint8_t* d, int n) {
    if (id == 0x20 && n >= 8) {
        s_room_sp_lo = d[0];
        s_room_sp_hi = d[1];
        s_water_sp   = d[2];
        s_modes      = d[5];
    } else if (id == 0x3C && n >= 8) {
        uint8_t sid = d[2];
        if (sid == 0xB8) {
            s_pending_sid       = 0xB8;
            s_pending_req_state = d[5] ? 2 : 1;
            s_req_state         = s_pending_req_state;
        } else if (sid == 0xB2) {
            s_pending_sid       = 0xB2;
        }
    }
}

// ── Wire-side task ───────────────────────────────────────────────────────
static void lin_task(void* arg) {
    (void)arg;
    uint8_t b, pid, buf[16];

    while (1) {
        // Block until at least one byte arrives.
        if (uart_read_bytes(LIN_UART, &b, 1, portMAX_DELAY) != 1) continue;
        s_evt_bytes++;

        if (b != 0x55) continue;            // not a SYNC, keep scanning

        // Possible frame start: read what should be the PID.
        if (uart_read_bytes(LIN_UART, &pid, 1, pdMS_TO_TICKS(5)) != 1) continue;
        if (!lin_pid_ok(pid)) continue;     // random 0x55 in data → reject
        s_evt_syncs++;

        uint8_t id  = pid & 0x3F;
        int     len = frame_len(id);
        if (len < 0) continue;

        // Publish first so we hit the master's response window.
        if (id == 0x21) { respond_0x21(); s_evt_frames++; continue; }
        if (id == 0x22) { respond_0x22(); s_evt_frames++; continue; }
        if (id == 0x3D) { respond_0x3D(); s_evt_frames++; continue; }

        int got = uart_read_bytes(LIN_UART, buf, len + 1, pdMS_TO_TICKS(20));
        if (got < len + 1) continue;

        uint8_t want = lin_chk(pid, buf, len, !id_uses_classic_chk(id));
        if (buf[len] != want) {
            ESP_LOGD(TAG, "id=%02X chk bad got=%02X want=%02X",
                     id, buf[len], want);
            continue;
        }
        on_master_frame(id, buf, len);
        s_evt_frames++;
    }
}

// ── Public API ───────────────────────────────────────────────────────────
void truma_sim_init(int gpio_tx, int gpio_rx) {
    // GPIO20/21 are the ESP32-C3's default UART0 pins.  Reset them to
    // plain GPIO so UART0 stops driving GPIO21 and the GPIO matrix can
    // hand them to UART1 cleanly.
    gpio_reset_pin((gpio_num_t)gpio_tx);
    gpio_reset_pin((gpio_num_t)gpio_rx);
    s_lin_tx_gpio = gpio_tx;
    s_lin_rx_gpio = gpio_rx;

    uart_config_t cfg = {
        .baud_rate  = LIN_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LIN_UART, RX_BUF_SIZE, TX_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LIN_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LIN_UART, gpio_tx, gpio_rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(lin_task, "lin", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "Truma LIN sim ready: UART1 %d 8N1 TX=GPIO%d RX=GPIO%d",
             LIN_BAUD, gpio_tx, gpio_rx);
}

void truma_sim_ping(void) { respond_0x21(); }

void truma_sim_ttoggle(int gpio, int ms) {
    // Temporarily detach the UART driver so we can drive the pin
    // manually with gpio_set_level().  Reinstall afterwards so the
    // simulator keeps working without needing a reboot.
    uart_driver_delete(LIN_UART);
    gpio_reset_pin((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    int level = 0;
    while (esp_timer_get_time() < end) {
        gpio_set_level((gpio_num_t)gpio, level);
        level ^= 1;
        esp_rom_delay_us(500);
    }
    gpio_set_level((gpio_num_t)gpio, 1);

    // Re-arm UART1 with the same pin pair the sim was initialised with.
    // (We can recover the RX pin by reading the other macro from main.c
    // via a weak hint; safest is to just re-run the init with the same
    // TX/RX globals we cached at init time.)
    uart_config_t cfg = {
        .baud_rate  = LIN_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    extern int s_lin_tx_gpio, s_lin_rx_gpio;
    uart_driver_install(LIN_UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    uart_param_config(LIN_UART, &cfg);
    uart_set_pin(LIN_UART, s_lin_tx_gpio, s_lin_rx_gpio,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void truma_sim_set_room_temp(float c)  { s_room_temp_c  = c; }
void truma_sim_set_water_temp(float c) { s_water_temp_c = c; }
void truma_sim_set_burner(bool b)      { s_burner       = b; }

void truma_sim_set_error(uint8_t k, uint8_t c, uint8_t sh) {
    s_err_class = k;
    s_err_code  = c;
    s_err_short = sh;
}

void truma_sim_print_status(void) {
    printf("[truma] room=%.1fC water=%.1fC burner=%d "
           "req=%u cur=%u err=%02X/%02X/%02X "
           "sp_room=%02X%02X sp_water=%02X modes=%02X | "
           "rx_bytes=%lu syncs=%lu frames=%lu tx=%lu\r\n",
           (double)s_room_temp_c, (double)s_water_temp_c, (int)s_burner,
           s_req_state, s_cur_state,
           s_err_class, s_err_code, s_err_short,
           s_room_sp_hi, s_room_sp_lo, s_water_sp, s_modes,
           (unsigned long)s_evt_bytes, (unsigned long)s_evt_syncs,
           (unsigned long)s_evt_frames, (unsigned long)s_evt_sent);
}
