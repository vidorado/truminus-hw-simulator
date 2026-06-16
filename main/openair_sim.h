// OpenAir PLUS (Bergstrom/Dirna A/C) BLE peripheral simulator.
//
// Impersonates the A/C unit so the official "OpenAir PLUS" Android app
// connects to *us*.  Every command the app writes to the control
// characteristic is logged (hex + ASCII + tentative field decode) — that
// is how we recover the command-side protocol a priori, without the real
// unit and without an external sniffer.  We also push crafted telemetry
// notifications so the app's UI populates, which validates the read-side
// frame decode reverse-engineered from libapp.so.
//
// Protocol (from .claude/skills/openair-plus or the truminus repo skill):
//   Service           UUID  e43ff2c2-8602-48f6-82d0-72cd56fb06f2
//   Write (commands)  UUID  9d667ea8-9c95-4dd0-b952-92031c4f5375
//   Notify (state)    UUID  4a01b4dd-350d-4afc-9a9f-27164f2b6b56  (+ CCCD)
//   Payload: ASCII-hex text, 2 hex chars per byte/field, 28-field record.
//   Write frame = first 13 fields; fields 13..27 are read-only telemetry.

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct ble_gap_event;  // fwd (NimBLE)

// The app's scan screen filters results by advName equal to this exact
// string, so the peripheral must advertise it to appear in the device list.
#define OPENAIR_DEVICE_NAME "My OpenAir PLUS"

// Register the GATT service.  Call after ble_svc_gatt_init() and before
// the host starts (same place the Ultimatron service is registered).
void openair_sim_register_gatt(void);

// Start undirected connectable advertising as "OpenAir PLUS".  `gap_cb` is
// the same GAP event handler main.c uses; events are forwarded back here
// via openair_sim_on_gap_event().
void openair_sim_advertise(uint8_t own_addr_type,
                           int (*gap_cb)(struct ble_gap_event*, void*));

// Forward GAP connect/disconnect/subscribe events so we can track the
// connection + notify subscription state.  Returns true if handled.
bool openair_sim_on_gap_event(struct ble_gap_event* event);

// Send one telemetry notification now (built from the current field table).
void openair_sim_send_telemetry(void);

// Periodic tick (call ~every 2 s): re-sends telemetry while connected and
// auto-send is on, so the app stays populated.
void openair_sim_tick(void);

// REPL hook.  Returns true if the command was an OpenAir command.
// `arg`/`arg2` may be NULL.
bool openair_sim_console(const char* cmd, const char* arg, const char* arg2);
