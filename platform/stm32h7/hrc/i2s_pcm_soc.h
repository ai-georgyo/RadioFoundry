// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32H7 half of i2s_pcm.c, included there once
// --- I2S and DMA ---
//
// SPI3 / I2S3 as one full-duplex slave (I2SCFG 100): Philips standard, 16-bit data in 32-bit channels, the clock
// edge from the board's ckpol. One block does both directions here, where the other family needs a second one. The
// four pads are the shared file's: WS PA15, CK PC10, and the data pads PC12 (SDO while IOSWP is 0) and PC11 (SDI),
// all AF6. DMA1 Stream0 receives and DMA1 Stream5 sends, circular, through DMAMUX1 (requests 61 and 62); the
// receive stream's half and full interrupts pace the shared file's task, as on the other family.
//
// AFCNTR stays 0: it must not be set in slave mode (RM0433 50.9.6), and with it clear the block leaves its pads alone
// while it is disabled.
//
// Which data pad the slave drives is not settled by the reference manual (hrc/i2s_dir.h has the two passages), and a
// wrong guess would drive a pad against the codec's own output. So the first start that finds the codec's clock
// running proves it, on the pad that is safe to drive:
//   1. hw_setup(): WS and CK to the block (inputs: a slave). Both data pads stay GPIO inputs, the transmit pad (the
//      one on the codec's data input) with a pull-down, the receive pad without a pull.
//   2. The transmit pad must follow a pull-up and a pull-down, or something else drives it: BUSY, nothing more.
//   3. Twice, Table 426's sense first: IOSWP set for the pass, only the transmit pad given to the block, two samples
//      in the transmit FIFO, the block started, the pad's level changes counted for 4 ms (after the two samples the
//      block repeats the last one, RM0433 50.9.12, so the pad keeps moving while the codec clocks it), the block
//      off, the pad a GPIO input again. In the sense that is not the block's, the pad is the block's input and the
//      block's output is the receive pad, which is not in alternate function mode and so is not driven.
//   4. One pass moved the pad and the other did not: that pass's sense is the block's (OK, or SWAPPED when it is not
//      the sense Table 426 gives). Neither: NONE. Both: BUSY. The verdict is kept until the next reset. If the clock
//      is gone after the passes there is no verdict, and the next start probes again.
//   5. Only with OK or SWAPPED, in hw_start(): the proven sense into the block, the transmit pad and then the receive
//      pad to the block, the stream. With any other verdict hw_start() fails and no data pad is the block's.
//
// Stream errors latch on this block: after an underrun the transmitter repeats its last sample, and after an overrun
// the receiver stops filling its FIFO, each until software clears the flag (RM0433 50.9.12, 50.9.13). The shared
// task has no flag handling, so the block's own interrupt clears and counts them. The two streams may then be a few
// samples out of step until the next start. An overrun and an early frame within one frame need the block switched
// off and on (50.9.14): the next stop and start do that.
#include "hrc/i2s_dir.h"

#ifndef STM32_I2S_TX_ON_SDI
#define STM32_I2S_TX_ON_SDI	0	// the board's transmit line is on the pad that is SDI by default (MCU_DEFS)
#endif

// The data pads by what the board has on them. PIN_I2S_SD is the block's SDO pad, PIN_I2S_EXT_SD its SDI pad
#if STM32_I2S_TX_ON_SDI
#define PAD_TX		PIN_I2S_EXT_SD	// to the codec's data input: safe to drive
#define PAD_RX		PIN_I2S_SD	// the codec's data output is on it: never driven
#else
#define PAD_TX		PIN_I2S_SD
#define PAD_RX		PIN_I2S_EXT_SD
#endif

#define I2S_FLAGS	(SPI_IFCR_SUSPC | SPI_IFCR_TIFREC | SPI_IFCR_OVRC | SPI_IFCR_UDRC)
#define I2S_ERRORS	(SPI_SR_UDR | SPI_SR_OVR | SPI_SR_TIFRE)
_Static_assert(SPI_IFCR_UDRC == SPI_SR_UDR && SPI_IFCR_OVRC == SPI_SR_OVR && SPI_IFCR_TIFREC == SPI_SR_TIFRE,
	       "the error flags' clear bits are at the flags' positions");
#define I2S_SPINS	10000u		// register handshakes that take a few bus cycles
#define I2S_PROBE_SAMPLE	0xA5A5u	// a pattern with an edge in almost every bit

static uint8_t i2s_dir;			// stm32h7_i2s_dir_t: the probe's verdict, kept until the next reset
static bool i2s_ioswp;			// the proven sense (with OK or SWAPPED)
static uint16_t i2s_edges[2];
static volatile bool i2s_on;
static volatile uint32_t i2s_underruns, i2s_overruns, i2s_frame_errors;

static bool i2s_proven(void)
{
	return i2s_dir == STM32H7_I2S_DIR_OK || i2s_dir == STM32H7_I2S_DIR_SWAPPED;
}

static void dma_stop(void)
{
	DMA1_Stream0->CR &= ~DMA_SxCR_EN;
	DMA1_Stream5->CR &= ~DMA_SxCR_EN;
	for (uint32_t i = 0; i < I2S_SPINS && ((DMA1_Stream0->CR | DMA1_Stream5->CR) & DMA_SxCR_EN) != 0; i++) {
	}
	DMA1->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
	DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 | DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
}

// The stream off: the error interrupts, the two DMA streams, then the block. The streams first, as RM0433 50.4.14
// asks: a disabled block shows its transmit FIFO as having room (ES0392 2.22.4), and with its request still up the
// stream would run on through tx_buf
static void block_off(void)
{
	SPI3->IER = 0;
	dma_stop();
	SPI3->CR1 = 0;		// a slave stops with SPE alone (RM0433 50.9.8)
	SPI3->CFG1 &= ~(SPI_CFG1_RXDMAEN | SPI_CFG1_TXDMAEN);
	SPI3->IFCR = I2S_FLAGS;
}

// The block off and configured, with the data pads' sense as given. In I2S mode every field not named here keeps its
// reset value (RM0433 Table 424): CFG1 with its data and CRC sizes, a FIFO threshold of one sample and no DMA
// request; CFG2 with nothing but IOSWP (a slave, AFCNTR 0)
static void block_setup(bool ioswp)
{
	SPI3->CR1 = 0;
	SPI3->IER = 0;
	SPI3->CR2 = 0;
	SPI3->CFG1 = (7u << SPI_CFG1_CRCSIZE_Pos) | (7u << SPI_CFG1_DSIZE_Pos);
	SPI3->CFG2 = ioswp ? SPI_CFG2_IOSWP : 0u;
	// I2S mode, slave full duplex (100), Philips (00), 16-bit data (00) right aligned in a 32-bit channel. FIXCH 0:
	// the channel length is the codec's, not checked. CKPOL is the board's: clear, the block changes its data on the
	// falling clock edge and reads on the rising one; set, the other way round
	SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_2 | SPI_I2SCFGR_CHLEN |
			(cfg->ckpol ? SPI_I2SCFGR_CKPOL : 0u);
	SPI3->IFCR = I2S_FLAGS;
}

// The method of the other family's half: a free line follows a pull-up and a pull-down. Left with the pull-down. The
// pin mode changes are masked, here and below: each reads, changes and writes the port's registers, and a change
// another task makes to the port in between would be undone by the write
static bool line_free(GPIO_TypeDef *port, uint16_t pin)
{
	taskENTER_CRITICAL();
	stm32_pin_in(port, pin, GPIO_PULLUP);
	taskEXIT_CRITICAL();
	hal_delay_us(50);
	bool up = (port->IDR & pin) != 0;
	taskENTER_CRITICAL();
	stm32_pin_in(port, pin, GPIO_PULLDOWN);
	taskEXIT_CRITICAL();
	hal_delay_us(50);
	bool down = (port->IDR & pin) != 0;
	return up && !down;
}

static void hw_setup(void)
{
	__HAL_RCC_SPI3_CLK_ENABLE();
	__HAL_RCC_DMA1_CLK_ENABLE();
	if (cfg->master_if_free) {
		// The bench knob of the other family: it needs an audio PLL, which this platform does not set up
		dlog("i2s: master_if_free: not on this SoC, staying a slave");
	}
	block_setup(i2s_ioswp);
	taskENTER_CRITICAL();
	// The data pads are GPIO inputs until hw_start() has a verdict. Not again once it has one: this runs a second
	// time if the audio task could not be created the first time
	if (!i2s_proven()) {
		stm32_pin_in(PIN_PORT(PAD_TX), PIN_MASK(PAD_TX), GPIO_PULLDOWN);
		stm32_pin_in(PIN_PORT(PAD_RX), PIN_MASK(PAD_RX), GPIO_NOPULL);
	}
	stm32_pin_af(PIN_PORT(PIN_I2S_WS), PIN_MASK(PIN_I2S_WS), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	stm32_pin_af(PIN_PORT(PIN_I2S_CK), PIN_MASK(PIN_I2S_CK), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	taskEXIT_CRITICAL();
	// DMA1 Stream0 and Stream5 are the multiplexer's channels 0 and 5
	DMAMUX1_Channel0->CCR = DMA_REQUEST_SPI3_RX;
	DMAMUX1_Channel5->CCR = DMA_REQUEST_SPI3_TX;

	HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, AUDIO_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
	HAL_NVIC_SetPriority(SPI3_IRQn, AUDIO_IRQ_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(SPI3_IRQn);
}

static bool ws_wait(bool level)
{
	uint32_t t0 = hal_millis();	// the cycle counter's clock: it keeps counting inside RTOS critical sections
	while (PIN_READ(PIN_I2S_WS) != level) {
		if ((uint32_t)(hal_millis() - t0) > WS_TIMEOUT_MS) {
			return false;
		}
	}
	return true;
}

// One pass of the probe: the block in the given sense with only the transmit pad its own, and that pad's level
// changes while the codec clocks it
static uint16_t probe_pass(bool ioswp)
{
	GPIO_TypeDef *const port = PIN_PORT(PAD_TX);
	const uint16_t pin = PIN_MASK(PAD_TX);
	block_setup(ioswp);
	taskENTER_CRITICAL();
	stm32_pin_af(port, pin, GPIO_AF6_SPI3, false, GPIO_PULLDOWN);
	taskEXIT_CRITICAL();
	SPI3->CR1 |= SPI_CR1_SPE;
	// A left and a right sample by hand (no DMA request is on); 16-bit accesses, one sample each
	*(volatile uint16_t *)&SPI3->TXDR = I2S_PROBE_SAMPLE;
	*(volatile uint16_t *)&SPI3->TXDR = I2S_PROBE_SAMPLE;
	SPI3->CR1 |= SPI_CR1_CSTART;	// a slave needs it too: it then starts at the next frame (RM0433 50.9.7)

	const uint32_t cycles = (SystemCoreClock / 1000u) * STM32H7_I2S_PROBE_MS;
	const uint32_t t0 = DWT->CYCCNT;
	uint32_t last = port->IDR & pin;
	uint16_t edges = 0;
	while (DWT->CYCCNT - t0 < cycles) {
		const uint32_t now = port->IDR & pin;
		if (now != last) {
			last = now;
			if (edges < 0xFFFFu) {
				edges++;
			}
		}
	}

	SPI3->CR1 = 0;			// a slave stops with SPE alone (RM0433 50.9.8)
	taskENTER_CRITICAL();
	stm32_pin_in(port, pin, GPIO_PULLDOWN);
	taskEXIT_CRITICAL();
	SPI3->IFCR = I2S_FLAGS;
	return edges;
}

// Steps 2 to 4. The codec's clock is running; the block and both streams are off
static void probe(void)
{
	const bool is_free = line_free(PIN_PORT(PAD_TX), PIN_MASK(PAD_TX));
	uint16_t edges[2] = { 0, 0 };
	if (is_free) {
		for (uint8_t pass = 0; pass < 2u; pass++) {
			edges[pass] = probe_pass(stm32h7_i2s_try_ioswp(STM32_I2S_TX_ON_SDI != 0, pass));
		}
	}
	i2s_edges[0] = edges[0];
	i2s_edges[1] = edges[1];
	// A pass that saw no edge says something only if the codec was clocking the block: a clock that stopped under
	// the probe (the codec changing mode) leaves no verdict, and the next start probes again
	if (is_free && (!ws_wait(false) || !ws_wait(true))) {
		dlog("i2s: clock lost during the direction probe, edges %u / %u", (unsigned)edges[0], (unsigned)edges[1]);
		return;
	}
	const stm32h7_i2s_dir_t dir = stm32h7_i2s_verdict(is_free, edges[0], edges[1]);
	i2s_ioswp = stm32h7_i2s_try_ioswp(STM32_I2S_TX_ON_SDI != 0, dir == STM32H7_I2S_DIR_SWAPPED ? 1u : 0u);
	i2s_dir = (uint8_t)dir;
	dlog("i2s: direction %s, edges %u / %u, ioswp %u",
	     dir == STM32H7_I2S_DIR_OK ? "ok" : dir == STM32H7_I2S_DIR_SWAPPED ? "swapped" :
	     dir == STM32H7_I2S_DIR_NONE ? "none" : is_free ? "busy" : "busy (tx pad driven)",
	     (unsigned)edges[0], (unsigned)edges[1], (unsigned)i2s_ioswp);
}

static bool hw_start(void)
{
	i2s_on = false;
	block_off();
	memset(tx_buf, 0, sizeof tx_buf);
	pending = 0;

	if (i2s_dir == STM32H7_I2S_DIR_UNKNOWN) {
		// The probe counts edges the codec's clock makes: without the clock nothing is probed, there is no
		// verdict, and the next start tries again
		if (!ws_wait(false) || !ws_wait(true)) {
			return false;
		}
		probe();
	}
	if (!i2s_proven()) {
		return false;	// BUSY or NONE: both data pads stay GPIO inputs and the stream stays off
	}
	// Step 5, at every start (a mode change of the port lost to another task's is made good here). The block is
	// off and holds the proven sense before a pad becomes its own; the receive pad is its input in that sense
	block_setup(i2s_ioswp);
	taskENTER_CRITICAL();
	stm32_pin_af(PIN_PORT(PAD_TX), PIN_MASK(PAD_TX), GPIO_AF6_SPI3, false, GPIO_PULLDOWN);
	stm32_pin_af(PIN_PORT(PAD_RX), PIN_MASK(PAD_RX), GPIO_AF6_SPI3, false, GPIO_NOPULL);
	taskEXIT_CRITICAL();

	// RX: the block's receive register -> rx_buf, half and full interrupts
	DMA1_Stream0->PAR = (uint32_t)&SPI3->RXDR;
	DMA_RAM_OK(rx_buf);
	DMA1_Stream0->M0AR = (uint32_t)rx_buf;
	DMA1_Stream0->NDTR = 2 * HALF_WORDS;
	DMA1_Stream0->FCR = 0;	// direct mode
	DMA1_Stream0->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_HTIE |
			   DMA_SxCR_TCIE;	// P->M, 16 bits: one sample an access, priority low
	// TX: tx_buf -> the block's transmit register
	DMA1_Stream5->PAR = (uint32_t)&SPI3->TXDR;
	DMA_RAM_OK(tx_buf);
	DMA1_Stream5->M0AR = (uint32_t)tx_buf;
	DMA1_Stream5->NDTR = 2 * HALF_WORDS;
	DMA1_Stream5->FCR = 0;
	DMA1_Stream5->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
			   DMA_SxCR_DIR_0;	// M->P
	// The order of RM0433 50.4.14: the block's receive request, the two streams, the block's transmit request, SPE.
	// The transmit request is raised only right before SPE, below (the two request bits are the part of CFG1 that
	// takes a write at any time). A disabled block shows its transmit FIFO as having room (ES0392 2.22.4), and the
	// manual does not say that it asks for no data then: a request raised here could have the stream run on through
	// tx_buf during the line-up, and the two streams would start out of step
	SPI3->CFG1 |= SPI_CFG1_RXDMAEN;
	DMA1_Stream0->CR |= DMA_SxCR_EN;
	DMA1_Stream5->CR |= DMA_SxCR_EN;

	// The other family's line-up: enabled right after a WS rising edge (low, high, low, high), with interrupts off
	// for the short window between the edge and the enable. It is the clock check here, and makes the start the
	// same every time: this block finds the frame start itself (the next falling edge) and puts the first sample of
	// each stream in the left channel
	if (!ws_wait(false) || !ws_wait(true) || !ws_wait(false)) {
		block_off();
		return false;
	}
	taskENTER_CRITICAL();
	bool ok = ws_wait(true);
	if (ok) {
		SPI3->CFG1 |= SPI_CFG1_TXDMAEN;
		SPI3->CR1 |= SPI_CR1_SPE;	// the DMA fills the transmit FIFO from here
		for (uint32_t i = 0; i < I2S_SPINS && DMA1_Stream5->NDTR == 2 * HALF_WORDS; i++) {
		}
		SPI3->CR1 |= SPI_CR1_CSTART;
	}
	taskEXIT_CRITICAL();
	if (!ok) {
		block_off();
		return false;
	}
	SPI3->IER = SPI_IER_UDRIE | SPI_IER_OVRIE | SPI_IER_TIFREIE;
	i2s_on = true;
	return true;
}

static void hw_stop(void)
{
	i2s_on = false;
	block_off();
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

// The block's error flags: cleared, so the stream goes on, and counted
void SPI3_IRQHandler(void)
{
	const uint32_t sr = SPI3->SR & I2S_ERRORS;
	SPI3->IFCR = sr;
	if (sr & SPI_SR_UDR) {
		i2s_underruns++;
	}
	if (sr & SPI_SR_OVR) {
		i2s_overruns++;
	}
	if (sr & SPI_SR_TIFRE) {
		i2s_frame_errors++;
	}
}

// The receive stream's words still to go in its buffer (the audio task: which half the DMA is writing)
static inline uint32_t hw_rx_left(void)
{
	return DMA1_Stream0->NDTR;
}

void stm32h7_i2s_state(stm32h7_i2s_state_t *out)
{
	out->dir = i2s_dir;
	out->ioswp = i2s_ioswp;
	out->running = i2s_on;
	out->edges[0] = i2s_edges[0];
	out->edges[1] = i2s_edges[1];
	out->underruns = i2s_underruns;
	out->overruns = i2s_overruns;
	out->frame_errors = i2s_frame_errors;
}
