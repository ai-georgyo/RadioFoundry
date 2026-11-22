// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The data flash of an STM32 radio (spi_flash.h)
#include "spi_flash_board.h"
#include "flash/spi_nor/spi_nor.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#if SPI_NOR_ADDR4
#include "debuglink/dlog.h"
#endif

#define SECURITY_REG_SIZE	256u

static const stm32_spi_flash_cfg_t *cfg;
static SemaphoreHandle_t lock;
static bool ready;
#if SPI_NOR_ADDR4
// A transaction holds the bus: set once take() has it, cleared before give() lets it go. What a masked caller reads
// instead of the lock (stm32_spi_flash_quiesce)
static volatile bool busy;
#endif

static void chip_select(bool active)
{
	stm32_pin_write(cfg->cs_port, cfg->cs_pin, !active);
}

// Byte at a time on the registers (the platform's spi.c)
static bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	return stm32_spi_xfer(cfg->spi, tx, rx, len);
}

static const spi_bus_t bus = { .select = chip_select, .xfer = xfer };
static spi_nor_t nor = { .bus = &bus };

static void take(void)
{
	if (cfg->bus_take != NULL) {
		cfg->bus_take();
	} else if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreTake(lock, portMAX_DELAY);
	}
#if SPI_NOR_ADDR4
	busy = true;
#endif
}

static void give(void)
{
#if SPI_NOR_ADDR4
	busy = false;
#endif
	if (cfg->bus_give != NULL) {
		cfg->bus_give();
	} else if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		xSemaphoreGive(lock);
	}
}

static bool init(void)
{
	if (cfg == NULL) {
		return false;
	}
	if (lock == NULL && cfg->bus_take == NULL) {
		lock = xSemaphoreCreateMutex();
	}
	stm32_pin_out(cfg->cs_port, cfg->cs_pin, true, false);
	stm32_pin_af(cfg->bus_port, cfg->bus_pins, cfg->af, false, GPIO_NOPULL);
	stm32_spi_master_init(cfg->spi, cfg->cpol, cfg->cpha, cfg->br);
	take();
	bool ok = spi_nor_probe(&nor) && (nor.device & cfg->device_mask) == cfg->device && nor.size != 0;
	give();
#if SPI_NOR_ADDR4
	if (nor.unsettled) {
		// Other firmware left the chip in the middle of an erase or a program. It is not reset over that and
		// nothing is read from it or written to it; it reports as a chip that didn't start
		dlog("flash: an erase or program is suspended or running: chip not used");
	}
#endif
	ready = ok;
	stm32_spi_flash.size = ok ? nor.size : 0;
	// The map the chip holds (the board's, else its small one), none when it holds neither; a chip that didn't answer
	// keeps the board's with size 0, as on HR_C7000 (the debug link tells "didn't start" from "too small" by it)
	stm32_spi_flash.map = !ok ? cfg->map : hal_flash_map_for(cfg->map, cfg->small_map, nor.size, SPI_NOR_SECTOR_SIZE);
	return ok;
}

bool stm32_spi_flash_init(const stm32_spi_flash_cfg_t *c)
{
	cfg = c;
	return init();
}

static bool in_range(uint32_t addr, uint32_t len)
{
	return ready && addr < nor.size && len <= nor.size - addr;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	if (!in_range(addr, len)) {
		return false;
	}
	take();
#if SPI_NOR_ADDR4
	bool ok = spi_nor_read_parked(&nor, addr, buf, len);	// the extended address register back to 0
#else
	bool ok = spi_nor_read(&nor, addr, buf, len);
#endif
	give();
	return ok;
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	if (!in_range(addr, len)) {
		return false;
	}
	take();
#if SPI_NOR_ADDR4
	bool ok = spi_nor_program_parked(&nor, addr, buf, len);
#else
	bool ok = spi_nor_program(&nor, addr, buf, len);
#endif
	give();
	return ok;
}

static bool er(uint32_t addr)
{
	if (!in_range(addr, 1)) {
		return false;
	}
	take();
#if SPI_NOR_ADDR4
	bool ok = spi_nor_erase_sector_parked(&nor, addr);
#else
	bool ok = spi_nor_erase_sector(&nor, addr);
#endif
	give();
	return ok;
}

static uint32_t jedec(void)
{
	return ready ? (uint32_t)nor.manufacturer << 16 | nor.device : 0u;
}

hal_flash_t stm32_spi_flash = {
	.init = init,
	.size = 0,
	.erase_size = SPI_NOR_SECTOR_SIZE,
	.read = rd,
	.write = wr,
	.erase = er,
	.map = NULL,	// set by init
	.jedec = jedec,
};

bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!ready || reg < 1 || reg > 3 || offset >= SECURITY_REG_SIZE || len > SECURITY_REG_SIZE - offset) {
		return false;
	}
	take();
	bool ok = spi_nor_read_security(&nor, ((uint32_t)reg << 12) | offset, buf, len);
	give();
	return ok;
}

bool stm32_spi_flash_cps_security_read(uint32_t addr, void *buf, uint32_t len)
{
	return stm32_spi_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}

uint16_t stm32_spi_flash_device(void)
{
	return ready ? nor.device : 0;
}

void stm32_spi_flash_quiesce(void)
{
#if SPI_NOR_ADDR4
	// Every transaction already ends parked. This covers a park that failed. Not from an interrupt or a fault
	// handler: the bus may be in the middle of a task's transaction there, and the lock can't be taken
	if (!ready || xPortIsInsideInterrupt()) {
		return;
	}
	// A task that can wait for the bus does (power off: a write another task has in flight ends first). Before the
	// scheduler starts nothing else is on the bus
	BaseType_t sched = xTaskGetSchedulerState();
	if (sched == taskSCHEDULER_NOT_STARTED ||
	    (sched == taskSCHEDULER_RUNNING && __get_PRIMASK() == 0 && __get_BASEPRI() == 0)) {
		take();
		(void)spi_nor_park(&nor);
		give();
		return;
	}
	// Interrupts masked (the hook before a reset) or the scheduler suspended: a wait for the lock would never end
	// there. The park goes ahead only if no transaction holds the bus; one that does parks at its own end. The lock
	// itself is not touched: its calls open and close a critical section, whose end sets BASEPRI to 0 and would
	// unmask a caller that masked with BASEPRI. No other task runs meanwhile and no interrupt uses the chip. A
	// board's own lock can't be asked without waiting
	if (cfg->bus_take != NULL || busy) {
		return;
	}
	(void)spi_nor_park(&nor);
#endif
}
