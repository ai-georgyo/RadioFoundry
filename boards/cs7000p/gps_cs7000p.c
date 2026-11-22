// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS GPS: the module's NMEA output on USART6 RX (PC7, AF7), through the shared STM32 GPS receiver
// (platform/stm32/gps_uart; docs/spec/hardware-cs7000p.md 11). The module is always powered: no power switch, no
// reset line and no 1PPS is wired, so the board gives no hooks, and Settings > Location > GPS off only stops the
// USART. Nothing is sent to the module: PC6, its input, stays an input (pinsetup_cs7000p.c). Unverified on a radio.
#include "cs7000p.h"
#include "pins.h"
#include "gps_uart/gps_uart.h"

#if CONFIG_GPS	// off: the driver and its interrupt handler are left out, and the radio has no GPS

// The module's own rate is in no document. 9600 first, the documented default of its maker's sibling part, then
// 115200: the shared receiver listens at each in turn until a checksum-valid sentence comes
static const uint32_t rates[] = { 9600, 115200 };

static const stm32_gps_cfg_t gps = {
	.uart = USART6, .irq = USART6_IRQn,
	.rx_port = PIN_PORT(PIN_GPS_RX), .rx_pin = PIN_MASK(PIN_GPS_RX), .rx_af = GPIO_AF7_USART6,
	.rates = rates, .n_rates = sizeof(rates) / sizeof(rates[0]), .listen_ms = 3000,
};

void USART6_IRQHandler(void)
{
	stm32_gps_irq();
}

void cs7000p_gps_init(void)
{
	stm32_gps_init(&gps);
}
#endif
