// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC1 scanning up to 16 inputs into a circular DMA buffer (adc_scan.h), one scan per TIM6 update; the supply from
// the internal reference on ADC3, once, before the scan starts.
//
// Clock: the synchronous mode, CKMODE 11 (RM0433 25.4.3). The converter has no clock source of its own to lose: it
// runs whenever the CPU does, on every clock tree, and a timer trigger starts a conversion with no resynchronization
// jitter. Silicon revision Y: the bus clock / 4. Revision V: sys_ck (HPRE 1) or sys_ck / 2 (any other HPRE), / 4,
// and / 2 again at the analog block (stm32h7_adc_sync_hz_v). Either way 12 to 24 MHz from a 192 MHz sys_ck and a
// 96 MHz bus. ST's module sets BOOST from the bus clock, which is right on revision Y only: revision V's is set again
// here from the clock the converter gets.
//
// The set-up goes through ST's ADC module, which hides what differs between the silicon revisions (the width of the
// BOOST field, the resolution codes). The start-up after it is done here, on the registers and ST's inline helpers,
// because every wait in it needs a bound: ST's calibration call would wait for tens of seconds on a converter
// without a clock, and a board reads its power switch from this scan.
//
// Trigger: TIM6's update as TRGO (regular trigger 13), so the scan rate is exact. DMA2 Stream0 takes the samples
// (request 9 through DMAMUX1 channel 8), 16 bits each, circular, with an interrupt at half and at the end of the
// buffer: a batch per rank goes to the rank's callback from there. TIM6's own update interrupt is the per-scan tick
// and is on only while a tick callback is set.
//
// Overrun: when the DMA does not take a sample in time the converter stops handing samples over until the flag is
// cleared (RM0433 25.4.27), and the sample that was lost would shift every later one into its neighbor's column. So
// the overrun interrupt stops the conversions and starts stream and sequence again from the first rank. A second
// overrun before the buffer has filled once, or a DMA bus error, ends the scan: it then says so
// (stm32_adc_scan_running) and its averages are 0, never stale.
//
// Silicon revision Y, ES0392: 2.9.7, conversions of two ADCs, or of an ADC and the DAC, that overlap may cost
// accuracy: the reference is measured on ADC3 before ADC1 starts. 2.9.10: 12 bits are not guaranteed with VDDA above
// 3.3 V.
#include <stddef.h>
#include <string.h>
#include "periph/adc_scan.h"
#include "periph/pwm.h"
#include "platform.h"
#include "hal_time.h"

#define SCAN_IRQ_PRIORITY	6	// numerically >= 4: the callbacks may call the RTOS's FromISR functions
#define RANKS_MAX		16u

#define REGULATOR_US		20u	// the converter's regulator: 10 us at most (DS12110, tADCREG_STUP)
// A calibration with linearity is 165,010 converter clocks (DS12110, tCAL): 7 ms at 24 MHz, and 41 ms at 4 MHz, a
// revision V part's converter clock on a 32 MHz bus. Under 3.3 MHz the start fails. An offset calibration is 1,280
#define CAL_LIMIT_US		50000u
// Four converter clocks must pass between a calibration's end and the enable (ST's ADC header,
// LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES): 1 us at 4 MHz
#define CAL_TO_ENABLE_US	5u
#define ENABLE_LIMIT_US		2000u
#define STOP_LIMIT_US		2000u	// ADSTP, ADDIS: a few converter clocks
#define DMA_OFF_SPINS		10000u	// the stream's enable bit reads back clear once its transfer is over
#define FILL_LIMIT_MS		100u	// the longest buffer stm32_adc_scan_start() waits for
// The reference: sampled for 810.5 clocks (at least 4.3 us, DS12110 Table 18: 34 us at 24 MHz), then 6.5 to convert
#define VREF_CONV_LIMIT_US	5000u
#define VREF_READS		8u
// A supply outside the part's range is a reading that went wrong (DS12110: VDDA 1.62 to 3.6 V)
#define VDDA_MIN_MV		1600u
#define VDDA_MAX_MV		3700u
// Converter clocks a rank takes: 64.5 to sample and 6.5 to convert at 12 bits (RM0433 Table 214), with a margin
#define RANK_CLOCKS		80u
// The slowest the converter runs at for a given bus clock: / 4, and / 2 again on revision V (whose clock is sys_ck or
// sys_ck / 2, never below the bus clock)
#define CLOCK_DIV_MAX		8u
#define REV_V			0x2000u	// DBGMCU REV_ID from here on: revision V's converter (Y: 0x1003)

#define DMA_FLAGS0	(DMA_LIFCR_CTCIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CFEIF0)

static const uint32_t rank_id[RANKS_MAX] = {
	ADC_REGULAR_RANK_1, ADC_REGULAR_RANK_2, ADC_REGULAR_RANK_3, ADC_REGULAR_RANK_4,
	ADC_REGULAR_RANK_5, ADC_REGULAR_RANK_6, ADC_REGULAR_RANK_7, ADC_REGULAR_RANK_8,
	ADC_REGULAR_RANK_9, ADC_REGULAR_RANK_10, ADC_REGULAR_RANK_11, ADC_REGULAR_RANK_12,
	ADC_REGULAR_RANK_13, ADC_REGULAR_RANK_14, ADC_REGULAR_RANK_15, ADC_REGULAR_RANK_16,
};

static ADC_HandleTypeDef hadc;		// ST's handle: ADC3 for the reference, then ADC1
static volatile uint16_t *buf;
static uint8_t ranks;
static uint16_t scans;
static volatile bool running;
static volatile bool wrapped;		// the stream has reached the end of the buffer at least once
static volatile bool fresh;		// started again after an overrun, and the buffer has not filled since
static volatile uint32_t restarts;
static stm32_adc_rank_cb_t volatile rank_cb[RANKS_MAX];
static void (*volatile tick_cb)(void);
static uint16_t vdda_mv;

// Waits until (*reg & mask) == want, at most limit_us. Task or interrupt
static bool wait_for(const volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t limit_us)
{
	const uint32_t t0 = hal_micros();
	while ((*reg & mask) != want) {
		if ((uint32_t)(hal_micros() - t0) > limit_us) {
			return (*reg & mask) == want;
		}
	}
	return true;
}

// Conversions stopped and the converter disabled, each with its bound. False: it did not obey
static bool adc_off(ADC_TypeDef *adc)
{
	if (LL_ADC_REG_IsConversionOngoing(adc) != 0) {
		LL_ADC_REG_StopConversion(adc);
		if (!wait_for(&adc->CR, ADC_CR_ADSTP | ADC_CR_ADSTART, 0, STOP_LIMIT_US)) {
			return false;
		}
	}
	if (LL_ADC_IsEnabled(adc) != 0) {
		LL_ADC_Disable(adc);
		if (!wait_for(&adc->CR, ADC_CR_ADDIS | ADC_CR_ADEN, 0, STOP_LIMIT_US)) {
			return false;
		}
	}
	return true;
}

// ST's set-up of a disabled converter: out of deep power-down, regulator on, the synchronous clock, 12 bits, a
// sequence of n ranks started by trigger (or by software), the data to the DMA in circular mode or to the data
// register. False: the regulator did not come on (no bus clock)
static bool adc_setup(ADC_TypeDef *adc, uint8_t n, uint32_t trigger, uint32_t data)
{
	memset(&hadc, 0, sizeof hadc);
	hadc.Instance = adc;
	hadc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
	hadc.Init.Resolution = ADC_RESOLUTION_12B;
	hadc.Init.ScanConvMode = ADC_SCAN_ENABLE;
	hadc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc.Init.LowPowerAutoWait = DISABLE;
	hadc.Init.ContinuousConvMode = DISABLE;
	hadc.Init.NbrOfConversion = n;
	hadc.Init.DiscontinuousConvMode = DISABLE;
	hadc.Init.ExternalTrigConv = trigger;
	hadc.Init.ExternalTrigConvEdge = trigger == ADC_SOFTWARE_START ? ADC_EXTERNALTRIGCONVEDGE_NONE :
									 ADC_EXTERNALTRIGCONVEDGE_RISING;
	hadc.Init.ConversionDataManagement = data;
	hadc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
	hadc.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
	hadc.Init.OversamplingMode = DISABLE;
	if (HAL_ADC_Init(&hadc) != HAL_OK) {
		return false;
	}
	if (stm32h7_rev_id() >= REV_V) {
		// The converter is disabled here, so BOOST takes a write. HPRE 0xxx is 1, and only "1 or not" counts
		const uint16_t ahb_div = (RCC->D1CFGR & RCC_D1CFGR_HPRE_3) != 0 ? 2u : 1u;
		const uint32_t hz = stm32h7_adc_sync_hz_v(HAL_RCC_GetSysClockFreq(), ahb_div);
		MODIFY_REG(adc->CR, ADC_CR_BOOST, (uint32_t)stm32h7_adc_boost_v(hz) << ADC_CR_BOOST_Pos);
	}
	return true;
}

static bool adc_rank(uint8_t rank, uint32_t channel, uint32_t sample_time)
{
	ADC_ChannelConfTypeDef ch = {
		.Channel = channel,
		.Rank = rank_id[rank],
		.SamplingTime = sample_time,
		.SingleDiff = ADC_SINGLE_ENDED,
		.OffsetNumber = ADC_OFFSET_NONE,
	};
	return HAL_ADC_ConfigChannel(&hadc, &ch) == HAL_OK;
}

// A set-up converter calibrated and enabled (RM0433 25.4.6 to 25.4.9), every wait with its bound
static bool adc_on(ADC_TypeDef *adc, uint32_t calibration)
{
	hal_delay_us(REGULATOR_US);
	if (LL_ADC_IsInternalRegulatorEnabled(adc) == 0) {
		return false;
	}
	LL_ADC_StartCalibration(adc, calibration, LL_ADC_SINGLE_ENDED);
	if (!wait_for(&adc->CR, ADC_CR_ADCAL, 0, CAL_LIMIT_US)) {
		return false;
	}
	hal_delay_us(CAL_TO_ENABLE_US);
	adc->ISR = ADC_ISR_ADRDY;
	LL_ADC_Enable(adc);
	return wait_for(&adc->ISR, ADC_ISR_ADRDY, ADC_ISR_ADRDY, ENABLE_LIMIT_US);
}

// The supply from the internal reference: ADC3 (the only converter the reference is wired to, input 19) up, a few
// conversions started by software, ADC3 down again with its clock. vdda_mv stays 0 when any step fails or the result
// is not a supply the part can have
static void vdda_measure(void)
{
	vdda_mv = 0;
	__HAL_RCC_ADC3_CLK_ENABLE();
	(void)adc_off(ADC3);
	// The channel set-up also switches the reference's buffer onto the converter (ADC3_COMMON CCR VREFEN)
	if (adc_setup(ADC3, 1, ADC_SOFTWARE_START, ADC_CONVERSIONDATA_DR) &&
	    adc_rank(0, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_810CYCLES_5) && adc_on(ADC3, LL_ADC_CALIB_OFFSET)) {
		uint32_t sum = 0;
		unsigned got = 0;
		for (unsigned i = 0; i <= VREF_READS; i++) {
			ADC3->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
			LL_ADC_REG_StartConversion(ADC3);
			if (!wait_for(&ADC3->ISR, ADC_ISR_EOC, ADC_ISR_EOC, VREF_CONV_LIMIT_US)) {
				break;
			}
			const uint32_t v = ADC3->DR & 0xFFFu;
			if (i > 0) {	// the first one is left out: the buffer has just been switched on
				sum += v;
				got++;
			}
		}
		if (got == VREF_READS && sum >= VREF_READS) {
			// ST's factory value is a reading of the reference at 3.3 V on the converter's 16-bit scale
			// (DS12110 Table 19): the macro scales the 12-bit reading by 16 before it divides
			const uint32_t mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(sum / VREF_READS, LL_ADC_RESOLUTION_12B);
			if (mv >= VDDA_MIN_MV && mv <= VDDA_MAX_MV) {
				vdda_mv = (uint16_t)mv;
			}
		}
	}
	if (adc_off(ADC3)) {
		LL_ADC_SetCommonPathInternalCh(ADC3_COMMON, LL_ADC_PATH_INTERNAL_NONE);
		LL_ADC_DisableInternalRegulator(ADC3);
		LL_ADC_EnableDeepPowerDown(ADC3);
	}
	__HAL_RCC_ADC3_CLK_DISABLE();
}

// The stream from the buffer's first entry and the sequence from its first rank, waiting for the next trigger. Task
// (the start) or the scan's interrupts (after an overrun). False: the converter or the stream did not stop
static bool stream_start(void)
{
	if (LL_ADC_REG_IsConversionOngoing(ADC1) != 0) {
		LL_ADC_REG_StopConversion(ADC1);	// the sequence starts at its first rank again after this
		if (!wait_for(&ADC1->CR, ADC_CR_ADSTP | ADC_CR_ADSTART, 0, STOP_LIMIT_US)) {
			return false;
		}
	}
	DMA2_Stream0->CR &= ~DMA_SxCR_EN;
	for (uint32_t i = 0; (DMA2_Stream0->CR & DMA_SxCR_EN) != 0; i++) {
		if (i >= DMA_OFF_SPINS) {
			return false;
		}
	}
	DMA2->LIFCR = DMA_FLAGS0;
	DMAMUX1_Channel8->CCR = DMA_REQUEST_ADC1;	// DMA2 Stream0 is the multiplexer's channel 8
	DMA2_Stream0->PAR = (uint32_t)&ADC1->DR;
	DMA2_Stream0->M0AR = (uint32_t)buf;
	DMA2_Stream0->NDTR = (uint32_t)ranks * scans;
	DMA2_Stream0->FCR = 0;	// direct mode
	// Peripheral to memory, 16 bits both sides, memory increment, circular, low priority; half, end and bus error
	DMA2_Stream0->CR = DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_HTIE |
			   DMA_SxCR_TCIE | DMA_SxCR_TEIE;
	DMA2_Stream0->CR |= DMA_SxCR_EN;
	ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_EOSMP;
	ADC1->IER = ADC_IER_OVRIE;
	LL_ADC_REG_StartConversion(ADC1);	// armed: every TIM6 update from here on is one scan
	return true;
}

// Everything of the scan off, as far as it obeys. Task or the scan's interrupts
static void scan_off(void)
{
	running = false;
	// The scan's three interrupts out first: a task may be here with the handlers live, and one of them between the
	// steps below would write the converter's control bits under this function. The next start lets them in again;
	// one left pending until then finds its flag gone and does nothing
	HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
	HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);
	HAL_NVIC_DisableIRQ(ADC_IRQn);
	TIM6->CR1 = 0;
	TIM6->DIER = 0;
	// The converter's interrupt enables take a write only while no conversion is armed (RM0433 25.4.10): after the
	// stop, not before it
	(void)adc_off(ADC1);
	ADC1->IER = 0;
	DMA2_Stream0->CR &= ~DMA_SxCR_EN;
	for (uint32_t i = 0; i < DMA_OFF_SPINS && (DMA2_Stream0->CR & DMA_SxCR_EN) != 0; i++) {
	}
	DMA2_Stream0->CR = 0;
	DMA2->LIFCR = DMA_FLAGS0;
	ADC1->ISR = ADC_ISR_OVR;
}

// From the scan's interrupts (one priority: none of them interrupts this). Once: a second overrun before the buffer
// has filled again means the DMA is not taking samples at all, and starting over for ever would only keep the CPU in
// this interrupt
static void restart(void)
{
	if (fresh || !running) {
		scan_off();
		return;
	}
	fresh = true;
	restarts++;
	if (!stream_start()) {
		scan_off();
	}
}

static bool filled(void)
{
	// The flag as well: the stream's interrupt may be held off (a caller before the scheduler runs, or with
	// interrupts masked), and its handler sets wrapped before it clears the flag
	return wrapped || (DMA2->LISR & DMA_LISR_TCIF0) != 0;
}

bool stm32_adc_scan_start(const uint8_t *channels, uint8_t n, volatile uint16_t *buffer, uint16_t n_scans,
			  uint32_t scan_hz)
{
	__HAL_RCC_ADC12_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();
	__HAL_RCC_TIM6_CLK_ENABLE();
	// A second start: the old scan off, and its interrupts out until the new one is complete
	scan_off();
	wrapped = false;
	fresh = false;
	// The last test: the stream counts its samples in 16 bits
	if (channels == NULL || n == 0 || n > RANKS_MAX || n_scans < 2u || (n_scans & 1u) != 0 || scan_hz == 0 ||
	    !STM32_DMA_REACHABLE(buffer) || (uint32_t)n * n_scans > 0xFFFFu) {
		return false;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (channels[i] > 19u) {
			return false;
		}
	}
	// The trigger: counts of TIM6 per scan, the prescaler only as far as 16 bits need it
	const uint32_t timer_hz = stm32_pwm_timer_hz(TIM6);
	const uint32_t counts = (timer_hz + scan_hz / 2u) / scan_hz;
	const uint32_t psc = counts > 0 ? (counts - 1u) / 65536u : 0;
	// A scan must be over before the next trigger (one that comes during a sequence is ignored), with the slowest
	// converter clock the bus clock can give; and the buffer must fill within the wait below
	if (counts < 2u || psc > 0xFFFFu ||
	    (uint64_t)n * RANK_CLOCKS * CLOCK_DIV_MAX * scan_hz > HAL_RCC_GetHCLKFreq() ||
	    (uint64_t)n_scans * 1000u > (uint64_t)FILL_LIMIT_MS * scan_hz) {
		return false;
	}
	buf = buffer;
	ranks = n;
	scans = n_scans;
	memset((void *)buffer, 0, (size_t)n * n_scans * sizeof *buffer);

	// The reference first, alone (ES0392 2.9.7), with ADC1 off
	vdda_measure();

	// The trigger timer, stopped. The update event that loads the prescaler is also a trigger output: it is made
	// before the converter is armed, and with URS set it raises no interrupt flag
	TIM6->CR1 = TIM_CR1_URS;
	TIM6->PSC = psc;
	TIM6->ARR = counts / (psc + 1u) - 1u;
	TIM6->CR2 = TIM_CR2_MMS_1;	// TRGO: the update event
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = 0;
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, SCAN_IRQ_PRIORITY, 0);
	HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, SCAN_IRQ_PRIORITY, 0);
	HAL_NVIC_SetPriority(ADC_IRQn, SCAN_IRQ_PRIORITY, 0);

	// Every channel of the sequence must also be selected in PCSEL (RM0433 25.4.12): the channel set-up does it
	ADC1->PCSEL = 0;
	bool ok = adc_setup(ADC1, n, ADC_EXTERNALTRIG_T6_TRGO, ADC_CONVERSIONDATA_DMA_CIRCULAR);
	for (uint8_t i = 0; ok && i < n; i++) {
		ok = adc_rank(i, __LL_ADC_DECIMAL_NB_TO_CHANNEL(channels[i]), ADC_SAMPLETIME_64CYCLES_5);
	}
	ok = ok && adc_on(ADC1, LL_ADC_CALIB_OFFSET_LINEARITY) && stream_start();
	if (!ok) {
		scan_off();
		return false;
	}
	running = true;
	TIM6->DIER = tick_cb != NULL ? TIM_DIER_UIE : 0;
	HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
	HAL_NVIC_EnableIRQ(ADC_IRQn);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	TIM6->CR1 = TIM_CR1_URS | TIM_CR1_CEN;

	// The buffer once round: n_scans scans, two more for the start, and a millisecond
	const uint32_t limit_us = ((uint32_t)n_scans + 2u) * (1000000u / scan_hz + 1u) + 1000u;
	const uint32_t t0 = hal_micros();
	while (!filled() && running) {
		if ((uint32_t)(hal_micros() - t0) > limit_us) {
			break;
		}
	}
	if (!filled() || !running) {
		scan_off();
		return false;
	}
	return true;
}

bool stm32_adc_scan_running(void)
{
	return running;
}

void stm32_adc_scan_rank_cb(uint8_t rank, stm32_adc_rank_cb_t cb)
{
	if (rank < RANKS_MAX) {
		rank_cb[rank] = cb;
	}
}

// The tick is TIM6's update interrupt, on only while there is somebody to call. Before the first start the timer has
// no clock and takes no write: the start sets the enable from tick_cb
void stm32_adc_scan_tick_cb(void (*fn)(void))
{
	if (fn == NULL) {
		TIM6->DIER = 0;
		tick_cb = NULL;
	} else {
		tick_cb = fn;
		if (running && (TIM6->DIER & TIM_DIER_UIE) == 0) {
			TIM6->SR = 0;	// set by a scan while nobody listened: not a tick, the first one is the next scan's
			TIM6->DIER = TIM_DIER_UIE;
		}
	}
}

uint16_t stm32_adc_scan_avg(uint8_t rank)
{
	if (!running || rank >= ranks) {
		return 0;	// a scan that has ended has only old samples: absent, not stale
	}
	const uint32_t total = (uint32_t)ranks * scans;
	uint32_t sum = 0;
	for (uint32_t i = rank; i < total; i += ranks) {
		sum += buf[i];
	}
	return (uint16_t)(sum / scans);
}

uint16_t stm32_adc_vdda_mv(void)
{
	return vdda_mv;
}

uint32_t stm32_adc_scan_restarts(void)
{
	return restarts;
}

// One half of the buffer to every rank that has a callback
static void batch(uint32_t half)
{
	const uint16_t n = scans / 2u;
	const volatile uint16_t *const first = buf + half * ((uint32_t)ranks * n);
	for (uint8_t r = 0; r < ranks; r++) {
		const stm32_adc_rank_cb_t cb = rank_cb[r];
		if (cb != NULL) {
			cb(first + r, n, ranks);
		}
	}
}

void DMA2_Stream0_IRQHandler(void)
{
	const uint32_t isr = DMA2->LISR;
	if ((isr & DMA_LISR_TCIF0) != 0) {
		wrapped = true;		// before the flag goes: filled() reads one or the other
		fresh = false;
	}
	DMA2->LIFCR = isr & DMA_FLAGS0;
	if ((isr & DMA_LISR_TEIF0) != 0) {
		scan_off();		// a bus error switches the stream off: nothing to start again with
		return;
	}
	if (!running) {
		return;
	}
	const bool half = (isr & DMA_LISR_HTIF0) != 0;
	const bool end = (isr & DMA_LISR_TCIF0) != 0;
	if (half && end) {
		// A whole buffer late. The half the stream is in now is being written again: it is the older one
		const uint32_t now = DMA2_Stream0->NDTR > (uint32_t)ranks * scans / 2u ? 0u : 1u;
		batch(now);
		batch(1u - now);
	} else if (half) {
		batch(0);
	} else if (end) {
		batch(1);
	}
}

// ADC1's and ADC2's vector: the overrun of ADC1 is the only interrupt enabled
void ADC_IRQHandler(void)
{
	if ((ADC1->ISR & ADC_ISR_OVR) != 0) {
		ADC1->ISR = ADC_ISR_OVR;
		restart();
	}
}

// TIM6's vector, which the DAC's underrun interrupt shares: that one is never enabled (periph/dac.c)
void TIM6_DAC_IRQHandler(void)
{
	if ((TIM6->SR & TIM_SR_UIF) == 0) {
		return;
	}
	TIM6->SR = 0;
	void (*const fn)(void) = tick_cb;
	if (fn != NULL) {
		fn();
	}
}
