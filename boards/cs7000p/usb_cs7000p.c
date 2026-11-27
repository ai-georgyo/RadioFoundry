// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS USB identity (docs/spec/hardware-cs7000p.md 11): the STM32 radios' IDs and serial number
// (platform.h). The port is on the accessory connector, through the maker's programming cable
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"

const usb_ids_t board_usb_ids = {
	.vid = STM32_USB_VID,
	.pid_serial = STM32_USB_PID_SERIAL,
	.pid_network = STM32_USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "RadioFoundry",
	.product = "CS7000 M17 PLUS",
	.serial_interface = "CS7000 M17 PLUS serial",
	.serial_number = stm32_usb_serial_number,
};

#endif	// CONFIG_USB
