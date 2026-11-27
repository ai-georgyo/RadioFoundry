// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32F4 half of i2s_pcm.c, included there once
// --- I2S and DMA ---

static void dma_stop(void)
{
	DMA1_Stream0->CR &= ~DMA_SxCR_EN;
	DMA1_Stream5->CR &= ~DMA_SxCR_EN;
	while ((DMA1_Stream0->CR & DMA_SxCR_EN) || (DMA1_Stream5->CR & DMA_SxCR_EN)) {
	}
	DMA1->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
	DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
}

// Bench knob (cfg->master_if_free): the STM32 generates CK and WS (SPI3 master receive, I2S3ext slave transmit), for a
// chip that doesn't drive its I2S. Only when nothing else drives the two lines: each must follow a pull-up and a pull-down
static bool i2s_master;

static bool line_free(GPIO_TypeDef *port, uint16_t pin)
{
	stm32_pin_in(port, pin, GPIO_PULLUP);
	hal_delay_us(50);
	bool up = (port->IDR & pin) != 0;
	stm32_pin_in(port, pin, GPIO_PULLDOWN);
	hal_delay_us(50);
	bool down = (port->IDR & pin) != 0;
	return up && !down;
}

// PLLI2S from the 2 MHz PLL input (HSE 8 MHz / M 4): N 128 = 256 MHz, R 5 = 51.2 MHz; with I2SDIV 50 and 32-bit
// channels Fs = 51.2 MHz / (64 x 100) = 8 kHz
static void plli2s_on(void)
{
	RCC->CR &= ~RCC_CR_PLLI2SON;
	while (RCC->CR & RCC_CR_PLLI2SRDY) {
	}
	RCC->PLLI2SCFGR = (5u << RCC_PLLI2SCFGR_PLLI2SR_Pos) | (128u << RCC_PLLI2SCFGR_PLLI2SN_Pos);
	RCC->CFGR &= ~RCC_CFGR_I2SSRC;
	RCC->CR |= RCC_CR_PLLI2SON;
	uint32_t t0 = hal_millis();
	while (!(RCC->CR & RCC_CR_PLLI2SRDY) && (uint32_t)(hal_millis() - t0) < 10u) {
	}
}

static void hw_setup(void)
{
	__HAL_RCC_SPI3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();
	if (cfg->master_if_free) {
		bool ck_free = line_free(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK));
		bool ws_free = line_free(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS));
		i2s_master = ck_free && ws_free;
		dlog("i2s: ck %s, ws %s: %s", ck_free ? "free" : "driven", ws_free ? "free" : "driven",
			i2s_master ? "STM32 master" : "slave (a line is driven)");
		if (i2s_master) {
			plli2s_on();
		}
	}
	stm32_pin_af(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_SD), PIN_MASK(PIN_I2S_SD), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_EXT_SD), PIN_MASK(PIN_I2S_EXT_SD), GPIO_AF5_I2S3ext, false, GPIO_NOPULL);

	// Philips, 16-bit data in a 32-bit channel, the board's clock polarity. Main: slave receive; ext: slave transmit
	const uint32_t fmt = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_CHLEN | (cfg->ckpol ? SPI_I2SCFGR_CKPOL : 0u);
	SPI3->I2SCFGR = 0;
	I2S3ext->I2SCFGR = 0;
	SPI3->I2SPR = 2;	// unused in slave mode; reset value
	SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_0;	// 01 = slave receive
	if (i2s_master) {
		SPI3->I2SPR = 50u;					// I2SDIV 50, ODD 0, MCKOE 0
		SPI3->I2SCFGR = fmt | SPI_I2SCFGR_I2SCFG_1 | SPI_I2SCFGR_I2SCFG_0;	// 11 = master receive
	}
	I2S3ext->I2SCFGR = fmt;				// 00 = slave transmit
	SPI3->CR2 = SPI_CR2_RXDMAEN;
	I2S3ext->CR2 = SPI_CR2_TXDMAEN;

	HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, AUDIO_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
}

static bool ws_wait(bool level)
{
	uint32_t t0 = hal_millis();	// TIM14 (priority 3) keeps counting inside RTOS critical sections
	while (PIN_READ(PIN_I2S_WS) != level) {
		if ((uint32_t)(hal_millis() - t0) > WS_TIMEOUT_MS) {
			return false;
		}
	}
	return true;
}

static bool hw_start(void)
{
	dma_stop();
	SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	I2S3ext->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	memset(tx_buf, 0, sizeof tx_buf);
	pending = 0;

	// RX: SPI3 DR -> rx_buf, half and full interrupts
	DMA1_Stream0->PAR = (uint32_t)&SPI3->DR;
	DMA_RAM_OK(rx_buf);
	DMA1_Stream0->M0AR = (uint32_t)rx_buf;
	DMA1_Stream0->NDTR = 2 * HALF_WORDS;
	DMA1_Stream0->FCR = 0;	// direct mode
	DMA1_Stream0->CR = (0u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
			   DMA_SxCR_CIRC | DMA_SxCR_HTIE | DMA_SxCR_TCIE;	// P->M, priority low
	// TX: tx_buf -> I2S3ext DR
	DMA1_Stream5->PAR = (uint32_t)&I2S3ext->DR;
	DMA_RAM_OK(tx_buf);
	DMA1_Stream5->M0AR = (uint32_t)tx_buf;
	DMA1_Stream5->NDTR = 2 * HALF_WORDS;
	DMA1_Stream5->FCR = 0;
	DMA1_Stream5->CR = (2u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC |
			   DMA_SxCR_CIRC | DMA_SxCR_DIR_0;	// M->P
	DMA1_Stream0->CR |= DMA_SxCR_EN;
	DMA1_Stream5->CR |= DMA_SxCR_EN;

	// Enable right after a WS rising edge (low, high, low, high) so both channels line up. Slave I2S in Philips mode
	// must be enabled while WS is high. Interrupts off for the short window between the edge and enable
	if (i2s_master) {
		I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;	// the slave transmitter first, then the master starts the clock
		SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
		return true;
	}
	if (!ws_wait(false) || !ws_wait(true) || !ws_wait(false)) {
		dma_stop();
		return false;
	}
	taskENTER_CRITICAL();
	bool ok = ws_wait(true);
	if (ok) {
		I2S3ext->I2SCFGR |= SPI_I2SCFGR_I2SE;
		SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
	}
	taskEXIT_CRITICAL();
	if (!ok) {
		dma_stop();
	}
	return ok;
}

static void hw_stop(void)
{
	SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	I2S3ext->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	dma_stop();
}

void DMA1_Stream0_IRQHandler(void)
{
	uint32_t isr = DMA1->LISR;
	DMA1->LIFCR = isr & (DMA_LISR_HTIF0 | DMA_LISR_TCIF0 | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0);
	uint8_t done = 0;
	if (isr & DMA_LISR_HTIF0) {
		done |= 1;
	}
	if (isr & DMA_LISR_TCIF0) {
		done |= 2;
	}
	if (done == 0 || task == NULL) {
		return;
	}
	if (pending & done) {
		overruns++;	// the task hasn't taken the previous one
	}
	pending |= done;
	BaseType_t woken = pdFALSE;
	vTaskNotifyGiveFromISR(task, &woken);
	portYIELD_FROM_ISR(woken);
}

// The receive stream's words still to go in its buffer (the audio task: which half the DMA is writing)
static inline uint32_t hw_rx_left(void)
{
	return DMA1_Stream0->NDTR;
}
