// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PCM audio over I2S3 to an HR_C6000 / HR_C5000 codec (i2s_pcm.h; docs/spec/hardware-dm1701.md 3.2, 4.4, 8,
// hardware-md380.md 6.2, hardware-md9600.md 4.3).
//
// The chip drives CK (PC10) and WS (PA15); the I2S block receives the microphone and sends speaker PCM on PC12 and PC11
// (the family's half, hrc/i2s_pcm_soc.h, has the block: on the F4 SPI3 receives on PC12, I2S3_SD, and I2S3ext sends on
// PC11, I2S3ext_SD). Philips standard, 16-bit data in 32-bit channels, 8 kHz, so every sample period has a left and a
// right halfword. Both directions run circular DMA over two 20 ms halves (DMA1 S0 RX, DMA1 S5 TX); they share
// the bit clock and start together, so the RX stream's half / full interrupts pace both. The interrupt only wakes the
// audio task, which calls the block callback (never from the ISR) and fills the TX half that has just been sent, one
// block (20 ms) of latency.
//
// Channels: the microphone is taken from the left channel (unverified which one the chip fills); speaker samples are
// written to both.
//
// The prompt FIFO (cfg->prompt_fifo, CONFIG_AIR or CONFIG_BEEP): in FM mode I2S PCM doesn't reach the chip's line out
// (modem-hrc6000.md 8). While the FM copy holds the chip (a profile the correlator can't take, such as NXDN), and in FM
// receive while the PCM route is on (beeps; the board reports the chip, stm32_i2s_pcm_fm_chip), every speaker block
// also goes into a ring that a feeder task keeps the chip's 64-sample (8 ms) prompt FIFO topped up from. The I2S stream and the FIFO run
// on the chip's clock, so they never drift. The feeder runs at the top priority (with the RTOS timer task): the DMR
// glue's copy interrupt reads 512 bytes with interrupts masked every 32 ms, and a feeder below the audio task missed
// its turn there (8 ms gaps).
#include "hrc/i2s_pcm.h"

#if CONFIG_DMR	// the whole file: DMR only
#include <string.h>
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "debuglink/dlog.h"
#include "hrc/dmr_glue.h"
#include "hrc/pcm_ring.h"

// The two-argument pin form the boards' pins.h use
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))
// A DMA stream's memory address must be in RAM the DMA controllers reach: never the CPU-only RAM (stm32_family.h)
#define DMA_RAM_OK(p)		configASSERT(STM32_DMA_REACHABLE(p))

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1280	// words: the vocoder in the callback needs about 4 KB (stock_vocoder.c)
#define AUDIO_IRQ_PRIORITY	6	// numerically >= 4 for FreeRTOS API calls

#define PIN_I2S_WS		GPIOA, GPIO_PIN_15	// AF6
#define PIN_I2S_CK		GPIOC, GPIO_PIN_10	// AF6
#define PIN_I2S_SD		GPIOC, GPIO_PIN_12	// chip -> MCU on the F4 (AF6)
#define PIN_I2S_EXT_SD		GPIOC, GPIO_PIN_11	// MCU -> chip on the F4 (AF5, I2S3ext)

#define HALF_WORDS		(AUDIO_BLOCK_SAMPLES * 2)	// one 20 ms block, left + right
#define WS_TIMEOUT_MS		20u			// WS edges come every 62.5 us; none for 20 ms: no clock

// DMA buffers in main SRAM (the CPU-only RAM is not reachable by DMA)
static int16_t rx_buf[2 * HALF_WORDS] __attribute__((aligned(4)));
static int16_t tx_buf[2 * HALF_WORDS] __attribute__((aligned(4)));
static int16_t mic[AUDIO_BLOCK_SAMPLES];
static int16_t spk[AUDIO_BLOCK_SAMPLES];

static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static volatile uint8_t pending;	// bit 0: half 0 received, bit 1: half 1
static volatile uint32_t overruns;	// blocks the task did not get to in time
static volatile uint32_t blocks;	// blocks processed (one per 20 ms while streaming)
static volatile uint32_t dropped;	// blocks skipped: both halves pending, the older one overwritten
static const stm32_i2s_pcm_cfg_t *cfg;

// The FIFO path: the FM copy (an air interface) and beeps in FM receive use it
#define I2S_FIFO	(CONFIG_AIR || CONFIG_BEEP)

#if I2S_FIFO
#define FIFO_TASK_PRIORITY	(configMAX_PRIORITIES - 1)
#define FIFO_TASK_STACK		176	// words: 704 B, the stack check's 336 deepest + 208 port + 128 margin, and a few
_Static_assert(AUDIO_BLOCK_SAMPLES % PCM_RING_CHUNK == 0, "a block is whole chunks");
_Static_assert(PCM_RING_CHUNK == HRC_PROMPT_CHUNK, "a ring chunk is one FIFO write");
static pcm_ring_t *ring;		// from the heap with the feeder, on the first FIFO use
static TaskHandle_t feeder;
static hrc_t *volatile fifo_chip;	// the chip whose FIFO the speaker blocks go to, NULL: none
static hrc_t *volatile fm_chip;		// the board's chip in FM receive (stm32_i2s_pcm_fm_chip), NULL: not
static volatile bool fifo_spk;		// the speaker's PCM route is on (stm32_i2s_pcm_speaker)
#define FIFO_PRIME	(2u * AUDIO_BLOCK_SAMPLES)	// in the ring before the first write: 40 ms in hand
_Static_assert(FIFO_PRIME + AUDIO_BLOCK_SAMPLES <= PCM_RING_SAMPLES, "a block fits on top of the priming");
static volatile uint32_t fifo_chunks, fifo_gaps, fifo_full;
// Writes a poll may make: the FIFO's depth in chunks, plus one for what drains meanwhile. A bound, so a chip that never
// reports full (reset, or the bit read wrong) can't keep the top-priority feeder in a loop that starves every task
#define FIFO_POLL_CHUNKS	((uint32_t)(HRC_PROMPT_DEPTH / HRC_PROMPT_CHUNK) + 1u)
#endif

// --- I2S and DMA ---
//
// The family's half, included once: hw_setup() (clocks, pins, the I2S block, the DMA interrupt), hw_start() (both
// streams running, lined up on a WS edge; false without a clock from the chip), hw_stop(), hw_rx_left() (the receive
// stream's words still to go) and the receive stream's interrupt handler, which sets the finished half's bit in pending
// (counting an overrun when it is still set) and notifies the task. It uses cfg, rx_buf, tx_buf, HALF_WORDS, pending,
// overruns, task, the pins and their macros, DMA_RAM_OK, AUDIO_IRQ_PRIORITY, WS_TIMEOUT_MS and dlog from above
#include "hrc/i2s_pcm_soc.h"

static void process(uint8_t half)
{
	const int16_t *in = &rx_buf[half * HALF_WORDS];
	int16_t *out = &tx_buf[half * HALF_WORDS];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = in[2 * i];
	}
	if (cfg->raw_block != NULL) {
		cfg->raw_block(in);	// both channels, before the callback looks at them
	}
	audio_block_cb_t cb = block_cb;
	if (cb != NULL) {
		cb(mic, spk, block_ctx);
	} else {
		memset(spk, 0, sizeof spk);
	}
#if I2S_FIFO
	// Silence while the PCM route is off: the FIFO goes past the switch that keeps the stream off the amplifier, which
	// stays on for the hang after a call, so what the stream holds then (not a call) would play
	if (fifo_chip != NULL && !pcm_ring_push(ring, fifo_spk ? spk : NULL, AUDIO_BLOCK_SAMPLES)) {
		fifo_full++;
	}
#endif
	// This half has just gone out (TX runs in step with RX), so it is free until the DMA comes back in 20 ms
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		out[2 * i] = spk[i];
		out[2 * i + 1] = spk[i];
	}
}

#if I2S_FIFO
// The chip in a mode whose line out the FIFO reaches: the FM copy's, else the board's in FM receive. The feeder writes
// page 3 only while this is still its chip: back in DMR, page 3 is the vocoder's
static hrc_t *fifo_mode_chip(void)
{
	hrc_t *copy = stm32_hrc_copy_chip();
	return copy != NULL ? copy : fm_chip;
}

// Waits for FIFO_PRIME in the ring before the first write, so the FIFO starts with 40 ms in hand (20 ms left a few gaps a
// call); after that a chunk the ring hasn't got is a gap (silence written)
static void feeder_task(void *arg)
{
	(void)arg;
	int16_t buf[PCM_RING_CHUNK];
	bool primed = false;
	for (;;) {
		hrc_t *chip = fifo_chip;
		if (chip == NULL) {
			primed = false;
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
			continue;
		}
		if (!primed) {
			primed = pcm_ring_level(ring) >= FIFO_PRIME;
		}
		// Stops as soon as the chip leaves the mode too: the prompt bit stays on until the audio task's next block
		for (uint32_t n = 0; n < FIFO_POLL_CHUNKS && primed && fifo_chip == chip && fifo_mode_chip() == chip
				     && hrc_prompt_room(chip); n++) {
			if (!pcm_ring_pop(ring, buf)) {
				fifo_gaps++;
			}
			(void)hrc_prompt_write(chip, buf);
			fifo_chunks++;
		}
		vTaskDelay(1);	// the FIFO holds 8 ms: polled every 1 ms, as the chip's manual does
	}
}

// The FIFO off. fifo_chip first: the feeder runs above any caller, and one let in between would write page 3 with the
// prompt bit already clear
static void fifo_release(void)
{
	hrc_t *chip = fifo_chip;
	fifo_chip = NULL;
	(void)hrc_prompt_on(chip, false);
}

// Per block: the speaker to the prompt FIFO too while the FM copy holds the chip, or while a beep plays in FM receive
// (pcm_fifo_chip). A beep's route stays on for its hold after the tone, so the ring has played the tone out by the
// time the route's end turns the FIFO off
static void fifo_follow(void)
{
	hrc_t *chip = cfg->prompt_fifo ? pcm_fifo_chip(stm32_hrc_copy_chip(), fm_chip, fifo_spk) : NULL;
	if (chip == fifo_chip) {
		return;
	}
	if (chip != NULL && feeder == NULL) {
		ring = pvPortMalloc(sizeof *ring);
		if (ring == NULL || xTaskCreate(feeder_task, "fifo", FIFO_TASK_STACK, NULL, FIFO_TASK_PRIORITY,
						&feeder) != pdPASS) {
			vPortFree(ring);
			ring = NULL;
			feeder = NULL;
			static bool told;	// tried again each block; said once
			if (!told) {
				told = true;
				dlog("i2s: prompt FIFO: no memory, speaker silent in the FM copy");
			}
			return;
		}
	}
	if (fifo_chip != NULL) {
		fifo_release();
		vTaskDelay(2);	// the feeder sees it stopped before the ring is reset below
	}
	if (chip != NULL) {
		pcm_ring_reset(ring);
		(void)hrc_prompt_on(chip, true);
		fifo_chip = chip;
		xTaskNotifyGive(feeder);
	}
}

void stm32_i2s_pcm_speaker(bool on)
{
	fifo_spk = on;
}

void stm32_i2s_pcm_fm_chip(hrc_t *chip)
{
	fm_chip = chip;
}

static void fifo_off(void)
{
	if (fifo_chip != NULL) {
		fifo_release();
		dlog("i2s: prompt FIFO off");
	}
}
#else
void stm32_i2s_pcm_speaker(bool on)
{
	(void)on;
}

void stm32_i2s_pcm_fm_chip(hrc_t *chip)
{
	(void)chip;
}

static void fifo_follow(void) {}
static void fifo_off(void) {}
#endif

static void audio_task(void *arg)
{
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		for (;;) {
			taskENTER_CRITICAL();
			uint8_t p = pending;
			uint8_t half;
			if (p == 3) {
				// Both pending: we are a block late and the DMA is already overwriting the older half. Take the
				// one it is not writing and drop the other
				half = (hw_rx_left() > HALF_WORDS) ? 1 : 0;
				pending = 0;
				dropped++;
			} else {
				half = (p & 1) ? 0 : 1;
				pending = 0;
			}
			taskEXIT_CRITICAL();
			if (p == 0 || !running) {
				break;
			}
			fifo_follow();
			process(half);
			blocks++;
		}
	}
}

// --- hal_audio_t ---

static void clock(bool on)
{
	if (cfg->clock != NULL) {
		cfg->clock(on);
	}
}

bool stm32_i2s_pcm_start(const stm32_i2s_pcm_cfg_t *c, audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		cfg = c;
		hw_setup();
		if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &task) != pdPASS) {
			task = NULL;
			return false;
		}
	}
	block_ctx = ctx;
	block_cb = cb;
	if (running) {
		return true;	// already streaming: just the new callback
	}
	running = true;
	clock(true);
	if (!hw_start()) {
		// No I2S clock from the chip (powered down, or its codec not running): blocks the DMA caught before the
		// clock check gave up must not reach the callback
		running = false;
		pending = 0;
		block_cb = NULL;
		clock(false);
		return false;
	}
	return true;
}

void stm32_i2s_pcm_stop(void)
{
	running = false;
	if (task == NULL) {
		return;		// never started: nothing set up
	}
	fifo_off();
	hw_stop();
	pending = 0;
	block_cb = NULL;
	clock(false);
}

bool stm32_i2s_pcm_running(void)
{
	return running;
}

void stm32_i2s_pcm_stats(stm32_i2s_pcm_stats_t *out)
{
	out->blocks = blocks;
	out->overruns = overruns;
	out->dropped = dropped;
}

void stm32_i2s_pcm_stream_stats(audio_stream_stats_t *out)
{
	out->blocks = blocks;
	out->late = overruns;
	out->dropped = dropped;
#if I2S_FIFO
	out->fifo_chunks = fifo_chunks;
	out->fifo_gaps = fifo_gaps;
	out->fifo_full = fifo_full;
	out->fifo_fm = fm_chip != NULL && (cfg == NULL || cfg->prompt_fifo);	// before the first start: the board's word
#else
	out->fifo_chunks = out->fifo_gaps = out->fifo_full = 0;
	out->fifo_fm = false;
#endif
}

void stm32_i2s_pcm_reset_stats(void)
{
	taskENTER_CRITICAL();
	blocks = 0;
	overruns = 0;
	dropped = 0;
#if I2S_FIFO
	fifo_chunks = 0;
	fifo_gaps = 0;
	fifo_full = 0;
#endif
	taskEXIT_CRITICAL();
}

#endif	// CONFIG_DMR
