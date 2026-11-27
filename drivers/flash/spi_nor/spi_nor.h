// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic SPI NOR flash (W25Q / GD25Q class, 3-byte addresses). The board supplies the bus (chip select and a
// full-duplex transfer) and any locking; the driver only builds the command sequences.
//
// SPI_NOR_ADDR4: a board whose part is larger than 16 MB adds -DSPI_NOR_ADDR4=1 to MCU_DEFS. The driver then also
// knows parts with 4-byte addresses and resets the part at the probe; for a part of 16 MB or less it sends the same
// array commands as without the define. The probe then reads status registers 1 and 2 before it knows the part, and
// takes the suspend and address mode bits to be where this class has them. Without the define nothing of it is
// compiled.
#ifndef SPI_NOR_H
#define SPI_NOR_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/spi_bus.h"

#ifndef SPI_NOR_ADDR4
#define SPI_NOR_ADDR4	0
#endif

typedef struct {
	const spi_bus_t *bus;
	uint8_t manufacturer;	// JEDEC manufacturer ID (0xEF Winbond, 0xC8 GigaDevice, ...)
	uint16_t device;	// memory type << 8 | capacity code
	uint32_t size;		// bytes, from the capacity code (0 if unknown)
#if SPI_NOR_ADDR4
	uint8_t addr_len;	// address bytes of the array commands: 3, or 4 on a part larger than 16 MB. Set by the probe
	// The part is in 4-byte address mode after the probe's reset (it powers up in it: other firmware set the
	// non-volatile ADP bit). Only the security register and unique ID reads depend on it
	bool ads;
	// The probe found an operation other firmware left suspended (the ID is filled in, size 0) or still running after
	// the wait (the probe fails). The part got no reset and is not used: a power cycle clears a suspended operation
	bool unsettled;
#endif
} spi_nor_t;

#define SPI_NOR_PAGE_SIZE	256u
#define SPI_NOR_SECTOR_SIZE	4096u
#if SPI_NOR_ADDR4
#define SPI_NOR_MAX_SIZE	0x10000000u	// 256 MB: what the capacity codes the probe knows reach
// Where 3-byte addresses end. A command that addressed a byte from here on leaves the extended address register set
#define SPI_NOR_ADDR3_END	0x1000000u
#else
#define SPI_NOR_MAX_SIZE	0x1000000u	// what 3-byte addresses reach: a larger part is used as 16 MB
#endif

// Read the JEDEC ID (0x9F) and fill in manufacturer, device and size (at most SPI_NOR_MAX_SIZE). False if the bus fails
// or nothing answers.
// With SPI_NOR_ADDR4 the part first gets a software reset (0x66, 0x99), which returns its address mode and its
// extended address register to their power-up state: it has no reset pin and keeps both through a reset of the MCU.
// A busy part is waited for (the 4 KB erase limit). A part with a suspended operation, or still busy after the wait,
// gets no reset (a reset can corrupt the data of that operation): unsettled is set and size stays 0. A part larger
// than 16 MB gets its real size and addr_len 4
bool spi_nor_probe(spi_nor_t *nor);
bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// Program already erased bytes; split at page boundaries, waits for each page
bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len);
// Erase the 4 KB sector holding addr; waits until done
bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr);
// Read security register bytes (0x48). addr is the register address as the chip defines it (W25Q: reg << 12 | offset)
bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
// The chip's unique ID (0x4B, four dummy bytes): 128 bits on the GD25Q and Puya PY25Q parts; a part with a 64-bit ID
// (W25Q) sends it in the first 8 bytes
#define SPI_NOR_UID_LEN		16u
bool spi_nor_read_uid(const spi_nor_t *nor, uint8_t id[SPI_NOR_UID_LEN]);
// Status register 1..3 (0x05 / 0x35 / 0x15)
bool spi_nor_status(const spi_nor_t *nor, uint8_t which, uint8_t *out);
#if SPI_NOR_ADDR4
// Write 0 to the extended address register (write enable, 0xC5 0x00, write disable). Every command with four address
// bytes replaces that register, the part has no reset pin, and a command with three address bytes uses it as the
// fourth: other code that talks to the part later (after a reset of the MCU, with 3-byte commands) would read and
// write 16 MB higher than it means to. The caller parks the part at the end of every program or erase transaction
// and after a read that reached SPI_NOR_ADDR3_END, under the lock that covered the transaction. True and no bus
// traffic on a part with 3-byte addresses
bool spi_nor_park(const spi_nor_t *nor);
// A read, program or erase that ends parked as that rule says: the read when it reached SPI_NOR_ADDR3_END, the
// program and the erase always, a failed one too. False when the transaction or the park failed. The caller holds
// its lock around the call
bool spi_nor_read_parked(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len);
bool spi_nor_program_parked(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len);
bool spi_nor_erase_sector_parked(const spi_nor_t *nor, uint32_t addr);
#endif

#endif
