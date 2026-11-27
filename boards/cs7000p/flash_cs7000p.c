// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS data flash: a Winbond W25Q256JV, 32 MB, on SPI4 (PE2 SCK, PE5 MISO, PE6 MOSI, AF5), mode 0, chip
// select PE4 (docs/spec/hardware-cs7000p.md 10), through the shared driver (platform/stm32/spi_flash). The board
// builds that driver with 4-byte addresses and core/storage as a flash that must be claimed before its first write and
// whose maker's bytes every rewrite keeps (board.mk); the map, its factory ranges and the board sector are in
// store_cs7000p.{h,c}. What has run on a radio is the bring-up page's First radio (docs/testing/cs7000p-bringup.md).
#include "cs7000p.h"
#include "pins.h"
#include "store_cs7000p.h"
#include "spi_flash/spi_flash_board.h"

static const stm32_spi_flash_cfg_t cfg = {
	.spi = SPI4,
	.bus_port = PIN_PORT(PIN_FLASH_SCK),	// SCK, MISO and MOSI are on one port (pins.h)
	.bus_pins = PIN_MASK(PIN_FLASH_SCK) | PIN_MASK(PIN_FLASH_MISO) | PIN_MASK(PIN_FLASH_MOSI),
	.af = GPIO_AF5_SPI4,
	.cs_port = PIN_PORT(PIN_FLASH_CS),
	.cs_pin = PIN_MASK(PIN_FLASH_CS),
	.cpol = false, .cpha = false,		// mode 0
	.br = 1,				// SPI4 runs from its bus clock: 48 MHz / 4 = 12 MHz
	.device = 0x4000u, .device_mask = 0xFF00u,	// the W25Q family (JEDEC memory type 0x40); this part is 0x4019
	.map = &cs7000p_flash_map,		// no small_map: one part size
};

void cs7000p_flash_init(void)
{
	// False: no accepted part, or one left busy or with a suspended operation (the driver logs it). The size is then
	// 0 and nothing is stored; board.flash in the view shows it
	(void)stm32_spi_flash_init(&cfg);
	// SCK and MOSI on the medium slope: 12 MHz is the limit of the low one, the family's default, which the
	// driver's pin set-up has just given them. After that set-up, so the probe itself ran at the limit
	stm32_pin_speed(PIN_PORT(PIN_FLASH_SCK), PIN_MASK(PIN_FLASH_SCK) | PIN_MASK(PIN_FLASH_MOSI),
			GPIO_SPEED_FREQ_MEDIUM);
}
