// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The data flash of an STM32 radio: a SPI NOR chip (drivers/flash/spi_nor) on one of the SPI blocks, as hal_flash_t,
// with its security registers. One mutex serializes every transaction once the scheduler runs. A board adds
// platform/stm32/spi_flash and drivers/flash/spi_nor to BOARD_DIRS, calls stm32_spi_flash_init() from board_init
// (spi_flash_board.h) and points board_t.flash at stm32_spi_flash. This header has no STM32 types, so the board's host
// tests can include it.
#ifndef STM32_SPI_FLASH_H
#define STM32_SPI_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

extern hal_flash_t stm32_spi_flash;

// Security registers 1..3, 256 bytes each
bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);
// The open-firmware CPS's area 10 (flash security registers), by the chip's register address (reg << 12 | offset)
bool stm32_spi_flash_cps_security_read(uint32_t addr, void *buf, uint32_t len);
// JEDEC memory type and capacity, 0 = none
uint16_t stm32_spi_flash_device(void);
#if STORAGE_FACTORY_KEEP
// Erases and programs refused because they fell in an erase unit holding factory bytes of the board's map or its
// small map (hal_flash_map_t.factory, docs/spec/settings-store.md 4.1) outside the unit core/storage granted
// (hal_flash_t.factory_grant): nothing was sent to the chip for them. With SPI_NOR_ADDR4 also an address that 3-byte
// addressing would take there (the address modulo 16 MB). The STM32 counterpart of the HR_C7000 platform's write
// floor
uint32_t stm32_spi_flash_factory_refusals(void);
#endif
// Leave the chip as other code expects to find it, before the radio powers off or goes to a boot loader: a part
// larger than 16 MB (a board built with SPI_NOR_ADDR4) gets its extended address register written to 0, which every
// write, erase and high read already does at its end. From a task it waits for the bus lock. With interrupts masked
// (the hook before a reset, PRIMASK or BASEPRI) or the scheduler suspended it can't wait: it parks only if no
// transaction holds the bus, leaves the caller's mask as it was, and does nothing if a board's own lock (bus_take)
// guards it. Does nothing from an interrupt or a fault handler, before the chip is probed, and on every other board
void stm32_spi_flash_quiesce(void);

#endif
