// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USB port glue for TinyUSB on the STM32H7's full-speed OTG core (USB2 OTG_FS, Synopsys DWC2; TinyUSB's port 0):
// device only, embedded PHY on PA11 (DM) / PA12 (DP), OTG_FS interrupt at priority 6. VBUS sensing is off: the stack
// forces the B session valid when it is (drivers/usb/usb_device.c leaves vbus_sensing false), so PA9 stays free for a
// board. Slave mode: no DMA, and so no cache or buffer placement question.
// The 48 MHz clock is set up with the system clock (clock.c): pll1_q on the crystal's tree, HSI48 on either HSI tree,
// so the core is clocked whichever tree the start-up reached. TinyUSB's core reset waits for a clocked core without a
// bound.
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"
#include "tusb.h"

#define OTG_FS_PRIORITY		6	// at or below the RTOS max syscall level
#define USB33_SPINS		100000u	// the supply detector answers within microseconds

void usb_port_init(void)
{
	stm32_pin_af(GPIOA, GPIO_PIN_11 | GPIO_PIN_12, GPIO_AF10_OTG2_FS, false, GPIO_NOPULL);

	// The transceiver's supply detector (RM0433 6.4.7): without it the PHY stays off. USB33DEN is above the byte of
	// PWR_CR3 that takes one write after power-on, and the byte below reads back as it is. The wait is bounded and
	// the result not used: without the supply nothing enumerates, and nothing else can be done about it here
	PWR->CR3 |= PWR_CR3_USB33DEN;
	for (uint32_t i = 0; i < USB33_SPINS && (PWR->CR3 & PWR_CR3_USB33RDY) == 0; i++) {
	}

	RCC->AHB1ENR |= RCC_AHB1ENR_USB2OTGFSEN;
	(void)RCC->AHB1ENR;

	// The ROM bootloader may have left the core running: start from reset values
	RCC->AHB1RSTR |= RCC_AHB1RSTR_USB2OTGFSRST;
	RCC->AHB1RSTR &= ~RCC_AHB1RSTR_USB2OTGFSRST;

	HAL_NVIC_SetPriority(OTG_FS_IRQn, OTG_FS_PRIORITY, 0);
	// TinyUSB enables the interrupt itself (dcd_int_enable) once the core is set up
}

void usb_port_soft_disconnect(bool disconnect)
{
	if (disconnect) {
		tud_disconnect();
	} else {
		tud_connect();
	}
}

void OTG_FS_IRQHandler(void)
{
	tud_int_handler(0);
}

void stm32_usb_serial_number(char *buf, uint8_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)UID_BASE;	// 96-bit unique device ID
	uint8_t n = 0;

	for (uint8_t i = 0; (i < 12) && (n + 2 < len); i++) {
		buf[n++] = hex[uid[i] >> 4];
		buf[n++] = hex[uid[i] & 0x0F];
	}
	buf[n] = '\0';
}

#endif	// CONFIG_USB
