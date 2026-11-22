// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Generic SPI NOR flash: command set from the Winbond W25Q / GigaDevice GD25Q datasheets
// (docs/spec/hardware-dm1701.md 13.1)
#include <stddef.h>
#include "spi_nor.h"
#include "hal_time.h"

enum {
	CMD_READ		= 0x03,
	CMD_PAGE_PROGRAM	= 0x02,
	CMD_SECTOR_ERASE	= 0x20,
	CMD_WRITE_ENABLE	= 0x06,
	CMD_STATUS1		= 0x05,
	CMD_STATUS2		= 0x35,
	CMD_STATUS3		= 0x15,
	CMD_JEDEC_ID		= 0x9F,
	CMD_READ_SECURITY	= 0x48,
	CMD_READ_UID		= 0x4B,
#if SPI_NOR_ADDR4
	// The forms that take four address bytes in either address mode (Winbond W25Q256JV datasheet 8.1.4). A part
	// with 4-byte addresses gets only these: nothing then depends on the mode other firmware left it in, and the
	// driver never changes that mode (0xB7 / 0xE9)
	CMD_READ4		= 0x13,
	CMD_PAGE_PROGRAM4	= 0x12,
	CMD_SECTOR_ERASE4	= 0x21,
	CMD_WRITE_DISABLE	= 0x04,
	CMD_ENABLE_RESET	= 0x66,
	CMD_RESET		= 0x99,
	CMD_WRITE_EXT_ADDR	= 0xC5,
#endif
};

#define STATUS1_BUSY	0x01u
#define STATUS1_WEL	0x02u
#if SPI_NOR_ADDR4
// Where the W25Q / GD25Q class has them (W25Q256JV 7.1)
#define STATUS2_SUS	0x80u	// an erase or program is suspended
#define STATUS3_ADS	0x01u	// the address mode now: 1 = 4-byte
// After the reset no command is accepted for tRST, 30 us (W25Q256JV 9.6)
#define RESET_WAIT_US	100u
#endif

// Busy limits in 1 ms polls (datasheet maximums: page program 3 ms, 4 KB erase 400 ms)
#define PROGRAM_POLLS	20u
#define ERASE_POLLS	1000u

static void addr_bytes(uint8_t *p, uint8_t cmd, uint32_t addr)
{
	p[0] = cmd;
	p[1] = (uint8_t)(addr >> 16);
	p[2] = (uint8_t)(addr >> 8);
	p[3] = (uint8_t)addr;
}

#if SPI_NOR_ADDR4
static void addr4_bytes(uint8_t *p, uint8_t cmd, uint32_t addr)
{
	p[0] = cmd;
	p[1] = (uint8_t)(addr >> 24);
	p[2] = (uint8_t)(addr >> 16);
	p[3] = (uint8_t)(addr >> 8);
	p[4] = (uint8_t)addr;
}

// The header of an array command into p (5 bytes of room): cmd with three address bytes, or cmd4 with four on a part
// that has them. Returns its length
static uint32_t array_hdr(const spi_nor_t *nor, uint8_t *p, uint8_t cmd, uint8_t cmd4, uint32_t addr)
{
	if (nor->addr_len == 4) {
		addr4_bytes(p, cmd4, addr);
		return 5;
	}
	addr_bytes(p, cmd, addr);
	return 4;
}
#endif

// One command: a header clocked out, then len bytes read (rx) or written (tx) in the same chip select
static bool command(const spi_nor_t *nor, const uint8_t *hdr, uint32_t hdr_len,
		    const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	const spi_bus_t *bus = nor->bus;
	bus->select(true);
	bool ok = bus->xfer(hdr, NULL, hdr_len);
	if (ok && len > 0) {
		ok = bus->xfer(tx, rx, len);
	}
	bus->select(false);
	return ok;
}

bool spi_nor_status(const spi_nor_t *nor, uint8_t which, uint8_t *out)
{
	static const uint8_t cmds[] = { CMD_STATUS1, CMD_STATUS2, CMD_STATUS3 };
	if (which < 1 || which > 3) {
		return false;
	}
	return command(nor, &cmds[which - 1], 1, NULL, out, 1);
}

static bool wait_ready(const spi_nor_t *nor, uint32_t polls)
{
	for (uint32_t i = 0; i <= polls; i++) {
		uint8_t st;
		if (!spi_nor_status(nor, 1, &st)) {
			return false;
		}
		if ((st & STATUS1_BUSY) == 0) {
			return true;
		}
		hal_delay_ms(1);
	}
	return false;
}

static bool write_enable(const spi_nor_t *nor)
{
	static const uint8_t cmd = CMD_WRITE_ENABLE;
	uint8_t st;
	return command(nor, &cmd, 1, NULL, NULL, 0) && spi_nor_status(nor, 1, &st) && (st & STATUS1_WEL) != 0;
}

#if SPI_NOR_ADDR4
// The part has no reset pin: its address mode, its extended address register and an operation other firmware
// suspended all outlive a reset of the MCU. The software reset returns it to its power-up state, but only a part
// that is neither busy nor suspended gets one: the reset can corrupt the data of an operation in flight (W25Q256JV
// 8.2.51). *reset: it was sent (not to a part with a suspended operation). False: the bus failed, or the part is
// still busy after the longest operation the driver knows (unsettled set: until it is done it answers its status
// and nothing else, so there is no ID to read)
static bool settle(spi_nor_t *nor, bool *reset)
{
	static const uint8_t enable = CMD_ENABLE_RESET, go = CMD_RESET;
	uint8_t st1, st2;
	*reset = false;
	if (!spi_nor_status(nor, 1, &st1) || !spi_nor_status(nor, 2, &st2)) {
		return false;
	}
	if ((st2 & STATUS2_SUS) != 0) {
		return true;	// (a bus nothing drives reads like this too: no wait for a part that isn't there)
	}
	if ((st1 & STATUS1_BUSY) != 0 && !wait_ready(nor, ERASE_POLLS)) {
		nor->unsettled = true;
		return false;
	}
	if (!command(nor, &enable, 1, NULL, NULL, 0) || !command(nor, &go, 1, NULL, NULL, 0)) {
		return false;
	}
	hal_delay_us(RESET_WAIT_US);
	*reset = true;
	return true;
}
#endif

bool spi_nor_probe(spi_nor_t *nor)
{
	static const uint8_t cmd = CMD_JEDEC_ID;
	uint8_t id[3];
	nor->manufacturer = 0;
	nor->device = 0;
	nor->size = 0;
#if SPI_NOR_ADDR4
	bool reset;
	nor->addr_len = 3;
	nor->ads = false;
	nor->unsettled = false;
	if (!settle(nor, &reset)) {
		return false;
	}
#endif
	if (!command(nor, &cmd, 1, NULL, id, 3)) {
		return false;
	}
	if ((id[0] == 0x00 && id[1] == 0x00) || (id[0] == 0xFF && id[1] == 0xFF)) {
		return false;	// bus stuck low or nothing driving MISO
	}
#if SPI_NOR_ADDR4
	// The address mode the reset left: the part's power-up mode (the non-volatile ADP bit), which the driver never
	// changes. Only a part with 4-byte addresses has the bit. Read before anything is filled in: a part whose mode
	// isn't known is not used
	bool addr4 = reset && id[2] >= 0x19 && id[2] <= 0x22;
	uint8_t st3 = 0;
	if (addr4 && !spi_nor_status(nor, 3, &st3)) {
		return false;
	}
#endif
	nor->manufacturer = id[0];
	nor->device = (uint16_t)((id[1] << 8) | id[2]);
#if SPI_NOR_ADDR4
	if (!reset) {
		nor->unsettled = true;
		return true;	// known by its ID, size 0
	}
#endif
	if (id[2] >= 0x10 && id[2] <= 0x18) {
		nor->size = 1u << id[2];	// W25Q / GD25Q capacity code: 2^n bytes (0x18 = 16 MB)
	} else if (id[2] >= 0x19 && id[2] <= 0x22) {
#if SPI_NOR_ADDR4
		// 0x19 is 32 MB. Winbond goes on at 0x20 (64 MB, 0x21 128 MB, 0x22 256 MB), others at 0x1A: 2^n bytes up to
		// 0x1C. A code past that is used as the largest size
		uint8_t n = id[2] >= 0x20 ? (uint8_t)(id[2] - 6u) : id[2];
		nor->size = n <= 28u ? 1u << n : SPI_NOR_MAX_SIZE;
		nor->addr_len = 4;
		nor->ads = (st3 & STATUS3_ADS) != 0;
#else
		// Larger parts (0x19 32 MB; Winbond goes on 0x20 64 MB, 0x21 128 MB, others 0x1A...): 3-byte addresses
		// reach only the first 16 MB, and an address above that would wrap to the bottom of the chip
		nor->size = SPI_NOR_MAX_SIZE;
#endif
	}
	return true;
}

#if SPI_NOR_ADDR4
bool spi_nor_park(const spi_nor_t *nor)
{
	static const uint8_t ext[2] = { CMD_WRITE_EXT_ADDR, 0x00 }, disable = CMD_WRITE_DISABLE;
	if (nor->addr_len != 4) {
		return true;
	}
	// The datasheet's list of commands that clear the write enable latch (7.1.2) does not name this one, so the
	// latch is cleared by command: no later transfer finds the part write enabled
	bool ok = write_enable(nor) && command(nor, ext, sizeof(ext), NULL, NULL, 0);
	return command(nor, &disable, 1, NULL, NULL, 0) && ok;
}
#endif

bool spi_nor_read(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
#if SPI_NOR_ADDR4
	uint8_t hdr[5];
#else
	uint8_t hdr[4];
#endif
	if (len == 0) {
		return true;
	}
#if SPI_NOR_ADDR4
	return command(nor, hdr, array_hdr(nor, hdr, CMD_READ, CMD_READ4, addr), NULL, buf, len);
#else
	addr_bytes(hdr, CMD_READ, addr);
	return command(nor, hdr, sizeof(hdr), NULL, buf, len);
#endif
}

bool spi_nor_read_security(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t hdr[5];
	if (len == 0) {
		return true;
	}
#if SPI_NOR_ADDR4
	if (nor->ads) {
		// A part in 4-byte address mode takes four address bytes here too (W25Q256JV 8.1.4)
		uint8_t hdr4[6];
		addr4_bytes(hdr4, CMD_READ_SECURITY, addr);
		hdr4[5] = 0xFF;	// dummy byte
		return command(nor, hdr4, sizeof(hdr4), NULL, buf, len);
	}
#endif
	addr_bytes(hdr, CMD_READ_SECURITY, addr);
	hdr[4] = 0xFF;	// dummy byte
	return command(nor, hdr, sizeof(hdr), NULL, buf, len);
}

bool spi_nor_read_uid(const spi_nor_t *nor, uint8_t id[SPI_NOR_UID_LEN])
{
	const uint8_t hdr[5] = { CMD_READ_UID, 0xFF, 0xFF, 0xFF, 0xFF };	// four dummy bytes
#if SPI_NOR_ADDR4
	if (nor->ads) {
		const uint8_t hdr4[6] = { CMD_READ_UID, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };	// five in 4-byte address mode
		return command(nor, hdr4, sizeof(hdr4), NULL, id, SPI_NOR_UID_LEN);
	}
#endif
	return command(nor, hdr, sizeof(hdr), NULL, id, SPI_NOR_UID_LEN);
}

bool spi_nor_program(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		uint32_t room = SPI_NOR_PAGE_SIZE - (addr % SPI_NOR_PAGE_SIZE);
		uint32_t n = len < room ? len : room;
#if SPI_NOR_ADDR4
		uint8_t hdr[5];
		uint32_t hdr_len = array_hdr(nor, hdr, CMD_PAGE_PROGRAM, CMD_PAGE_PROGRAM4, addr);
		if (!write_enable(nor) || !command(nor, hdr, hdr_len, p, NULL, n) || !wait_ready(nor, PROGRAM_POLLS)) {
			return false;
		}
#else
		uint8_t hdr[4];
		addr_bytes(hdr, CMD_PAGE_PROGRAM, addr);
		if (!write_enable(nor) || !command(nor, hdr, sizeof(hdr), p, NULL, n) || !wait_ready(nor, PROGRAM_POLLS)) {
			return false;
		}
#endif
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

bool spi_nor_erase_sector(const spi_nor_t *nor, uint32_t addr)
{
#if SPI_NOR_ADDR4
	uint8_t hdr[5];
	uint32_t hdr_len = array_hdr(nor, hdr, CMD_SECTOR_ERASE, CMD_SECTOR_ERASE4, addr & ~(SPI_NOR_SECTOR_SIZE - 1u));
	return write_enable(nor) && command(nor, hdr, hdr_len, NULL, NULL, 0) && wait_ready(nor, ERASE_POLLS);
#else
	uint8_t hdr[4];
	addr_bytes(hdr, CMD_SECTOR_ERASE, addr & ~(SPI_NOR_SECTOR_SIZE - 1u));
	return write_enable(nor) && command(nor, hdr, sizeof(hdr), NULL, NULL, 0) && wait_ready(nor, ERASE_POLLS);
#endif
}

#if SPI_NOR_ADDR4
bool spi_nor_read_parked(const spi_nor_t *nor, uint32_t addr, void *buf, uint32_t len)
{
	bool ok = spi_nor_read(nor, addr, buf, len);
	// Only a read that reached the top half can have left the register set: a read below it wrote it 0
	if (len > 0 && addr + (len - 1u) >= SPI_NOR_ADDR3_END) {
		ok = spi_nor_park(nor) && ok;
	}
	return ok;
}

bool spi_nor_program_parked(const spi_nor_t *nor, uint32_t addr, const void *buf, uint32_t len)
{
	bool ok = spi_nor_program(nor, addr, buf, len);
	return spi_nor_park(nor) && ok;		// also after a failed program: a page of it may have gone out
}

bool spi_nor_erase_sector_parked(const spi_nor_t *nor, uint32_t addr)
{
	bool ok = spi_nor_erase_sector(nor, addr);
	return spi_nor_park(nor) && ok;
}
#endif
