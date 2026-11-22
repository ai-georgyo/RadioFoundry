// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPI master on the registers, byte at a time (platform.h), on the STM32H7's SPI block, SPI1 to SPI4: no HAL time
// base or DMA needed, so it works before the scheduler, and tx / rx may be NULL. For a board's SPI flash and display
// glue.
//
// The clock is the block's kernel clock / 2^(br + 1): pll1_q for SPI1-3, the bus clock for SPI4 (clock.c leaves both
// selections at reset; 48 MHz each with a board's usual table). On the bare HSI tree pll1_q is off and SPI1-3 do not
// run: a transfer there returns false at once, with nothing left in the transmit FIFO.
#include "platform.h"

#define XFER_SPINS	100000u	// per byte: a byte takes 0.3 us at 24 MHz and 43 us at the slowest divider
#define BLOCKS		4u

static bool up[BLOCKS];	// the block took its set-up: it is a master and enabled

static unsigned block(const SPI_TypeDef *spi)
{
	return spi == SPI1 ? 0u : spi == SPI2 ? 1u : spi == SPI3 ? 2u : spi == SPI4 ? 3u : BLOCKS;
}

void stm32_spi_master_init(SPI_TypeDef *spi, bool cpol, bool cpha, uint8_t br)
{
	const unsigned n = block(spi);
	if (n == 0u) {
		__HAL_RCC_SPI1_CLK_ENABLE();
	} else if (n == 1u) {
		__HAL_RCC_SPI2_CLK_ENABLE();
	} else if (n == 2u) {
		__HAL_RCC_SPI3_CLK_ENABLE();
	} else if (n == 3u) {
		__HAL_RCC_SPI4_CLK_ENABLE();
	} else {
		return;	// not a block this file knows the clock of
	}
	up[n] = false;
	// The order matters on this block. With software slave management the internal slave-select input is the SSI
	// bit, and a master whose slave-select reads low takes a mode fault: it loses MASTER and can't set SPE while
	// MODF is set (RM0433 50.5.2, "Mode fault"). So: off, a fault left from before cleared, SSI high, and only then
	// MASTER. The F4's block has the three bits in one register and takes them in one write
	spi->CR1 = 0;
	spi->IFCR = SPI_IFCR_MODFC;
	spi->CR1 = SPI_CR1_SSI;
	// 8-bit frames, an event per frame (FTHLV 0), the baud rate divider 2^(br + 1); the CRC length field keeps its
	// reset value (the CRC is off; 0 is not a value the field takes, and ES0392 2.22.5 wants it left alone)
	spi->CFG1 = (7u << SPI_CFG1_CRCSIZE_Pos) | (7u << SPI_CFG1_DSIZE_Pos) | ((uint32_t)(br & 7u) << SPI_CFG1_MBR_Pos);
	// Master, software chip select, MSB first, full duplex; AFCNTR: the block keeps its pins while it is disabled, so
	// the clock line never floats
	spi->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR | (cpol ? SPI_CFG2_CPOL : 0u) |
		    (cpha ? SPI_CFG2_CPHA : 0u);
	spi->CR2 = 0;	// TSIZE 0: one endless transaction (which also keeps clear of ES0392 2.22.2)
	spi->CR1 |= SPI_CR1_SPE;
	spi->CR1 |= SPI_CR1_CSTART;	// a master sends nothing before it
	up[n] = (spi->CFG2 & SPI_CFG2_MASTER) != 0 && (spi->CR1 & SPI_CR1_SPE) != 0;
}

// Whether the block has the clock its shifter runs on. SPI1-3 take pll1_q unless somebody selected another source
// for them: with PLL1 off (the bare HSI tree, clock.c) a byte written to the FIFO would wait there for a clock that
// may come later, when no chip is selected
static bool clocked(unsigned n)
{
	return n >= 3u || (RCC->D2CCIP1R & RCC_D2CCIP1R_SPI123SEL) != 0 || (RCC->CR & RCC_CR_PLL1RDY) != 0;
}

bool stm32_spi_xfer(SPI_TypeDef *r, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	const unsigned n = block(r);
	if (n >= BLOCKS || !up[n] || !clocked(n)) {
		return false;	// at once: not after a spin limit per byte
	}
	for (uint32_t i = 0; i < len; i++) {
		uint32_t spins = XFER_SPINS;
		while ((r->SR & SPI_SR_TXP) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		*(volatile uint8_t *)&r->TXDR = tx != NULL ? tx[i] : 0xFF;	// a byte access: one frame
		spins = XFER_SPINS;
		while ((r->SR & SPI_SR_RXP) == 0) {
			if (--spins == 0) {
				return false;
			}
		}
		uint8_t b = *(volatile uint8_t *)&r->RXDR;
		if (rx != NULL) {
			rx[i] = b;
		}
	}
	uint32_t spins = XFER_SPINS;
	while ((r->SR & SPI_SR_TXC) == 0) {	// the last bit is out and the clock is idle: the caller may lift the select
		if (--spins == 0) {
			return false;
		}
	}
	return true;
}
