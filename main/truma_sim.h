// Truma Combi D LIN slave simulator.
//
// Emulates the Truma boiler side of the LIN bus described in
// .claude/skills/truma-protocol/SKILL.md.  Speaks 9600 8N1 on a single
// UART, half-duplex.  The master (a CP Plus D / TruMinus board) drives
// BREAK + SYNC + PID; we publish 0x21 / 0x22 / 0x3D and consume the
// master writes (0x20, 0x3C, ...).

#pragma once

#include <stdint.h>
#include <stdbool.h>

void truma_sim_init(int gpio_tx, int gpio_rx);

void truma_sim_set_room_temp(float c);
void truma_sim_set_water_temp(float c);
void truma_sim_set_burner(bool heating);
void truma_sim_set_error(uint8_t err_class, uint8_t err_code, uint8_t err_short);

void truma_sim_print_status(void);

// Force-transmit a 0x21 publisher frame, ignoring the master.  Useful
// for verifying the TX wiring with a sniffer or multimeter.
void truma_sim_ping(void);
