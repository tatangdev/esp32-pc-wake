#pragma once
#include <stdint.h>

// ethernet MAC of the windows PC (not the ESP32 MAC).
static const uint8_t PC_MAC[6] = {0x9C, 0x6B, 0x00, 0xFA, 0x74, 0x7C};

// home LAN IPv4 of the PC, not the 100.x tailscale IP.
// keep it 0.0.0.0 to disable the RDP check.
static const char PC_LAN_IP[] = "192.168.110.244";

static const uint16_t PC_RDP_PORT = 3389;
static const uint16_t WOL_PORT = 9;
