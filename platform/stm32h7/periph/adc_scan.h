// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// ADC1 scanning a board's analog inputs at an exact rate into a circular DMA buffer (adc_scan.c): the buffer keeps
// the last scans of every rank, so a reading is a short average with no locking, and a rank that needs every sample
// (a sub-audio detector) gets them in batches. The supply the readings are a fraction of is measured once, from the
// chip's internal reference. A board lists platform/stm32h7/periph in BOARD_DIRS and sets its analog pins to analog
// mode itself.
//
// No platform or chip header here: a board's host tests include this file through the board's own header.
#ifndef STM32H7_ADC_SCAN_H
#define STM32H7_ADC_SCAN_H

#include <stdint.h>
#include <stdbool.h>

// A batch of one rank's samples: n of them, the first at *first, each next one stride entries on. Half the buffer at
// a time (n = n_scans / 2), oldest first, right after its last sample was written. Raw, 12 bits. Called from the
// stream's interrupt
typedef void (*stm32_adc_rank_cb_t)(const volatile uint16_t *first, uint16_t n, uint16_t stride);	// DMA interrupt, priority 6

// channels: ADC1 input numbers, one per rank. buffer: n * n_scans samples in DMA-reachable RAM (STM32_DMA_REACHABLE).
// False: the ADC did not come up within its bounds; every reading is then absent
//
// n is 1 to 16, n_scans is even and at least 2, the buffer is at most 65535 samples and must fill within 100 ms
// (n_scans / scan_hz): the call returns when it has filled once, so an average is over real samples from the first
// one on. It takes some tens of milliseconds (two calibrations, then the buffer). From a task or before the
// scheduler; calling it again stops the scan and starts it with the new arguments
bool stm32_adc_scan_start(const uint8_t *channels, uint8_t n, volatile uint16_t *buffer, uint16_t n_scans, uint32_t scan_hz);
// False before the first start, after a start that failed, and after a fault the scan did not recover from (a second
// overrun right behind the first, a DMA bus error). The buffer is then stale and stm32_adc_scan_avg() gives 0, never
// an old reading: a caller that switches something on a reading must not keep acting on the last one
bool stm32_adc_scan_running(void);
void stm32_adc_scan_rank_cb(uint8_t rank, stm32_adc_rank_cb_t cb);	// a batch of one rank's samples; any time; NULL: none
void stm32_adc_scan_tick_cb(void (*fn)(void));		// once per scan (TIM6 update interrupt, priority 6); any time; NULL: none
uint16_t stm32_adc_scan_avg(uint8_t rank);		// raw 12-bit, over the buffer
uint16_t stm32_adc_vdda_mv(void);			// 0 until measured
// How often the scan was started again from its first rank after an overrun (the DMA did not take a sample in time,
// and the converter then stops handing samples over). For a board's view; 0 is the expected value
uint32_t stm32_adc_scan_restarts(void);

#endif
