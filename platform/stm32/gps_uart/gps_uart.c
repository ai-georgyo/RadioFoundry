// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS NMEA on a receive-only STM32 USART (gps_uart.h; docs/spec/hardware-md380.md 13, hardware-md9600.md 13)
#include <stddef.h>
#include "gps_uart/gps_uart.h"
#include "config.h"
#include "hal_time.h"
#include "gps/gps_rx/gps_rx.h"
#include "gps/gps_rx/gps_baud.h"
#include "gps/gps_rx/gps_power.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_GPS
#include "position/position.h"
#endif

#define GPS_TASK_STACK		384	// words: the position service's NMEA parser runs on it
#define GPS_TASK_PRIORITY	1
#define GPS_POLL_MS		20
#define GPS_IRQ_PRIORITY	7	// numerically >= 4: may call FreeRTOS, doesn't need to
#define NO_TIMEOUT		0xFFFFFFFFu	// gps_rx never gives up on its own: the rate hunt decides

static const stm32_gps_cfg_t *cfg;
static gps_rx_t rx;
static gps_baud_t hunt;
static volatile bool present;
static gps_power_t power;		// the probe, then the setting (gps_enable)
static volatile uint32_t baud;
static TaskHandle_t task;

// The family's half, included once: the USART's registers. USART_RX_ERRORS and USART_RX_READY (masks of the status
// word), usart_take() (the status word, with the data byte and the flags taken), clock_on() (the USART's clock gate),
// uart_rate() (off, the rate, then receiving with its interrupt on; registers only: rate_set() below keeps baud) and
// usart_off(). It uses cfg from above
#include "gps_uart/gps_uart_soc.h"

void stm32_gps_irq(void)
{
	USART_TypeDef *u = cfg->uart;
	uint8_t c;
	uint32_t sr = usart_take(u, &c);
	if (sr & USART_RX_ERRORS) {
		gps_rx_error(&rx);
		return;
	}
	if (sr & USART_RX_READY) {
		gps_rx_byte(&rx, c);
	}
}

// The USART listening at a rate of the hunt's list
static void rate_set(uint32_t rate)
{
	uart_rate(rate);
	baud = rate;
}

static void uart_on(bool on)
{
	if (on) {
		clock_on(true);
		stm32_pin_af(cfg->rx_port, cfg->rx_pin, cfg->rx_af, false, GPIO_PULLUP);
		rate_set(gps_baud_rate(&hunt));
		HAL_NVIC_SetPriority(cfg->irq, GPS_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(cfg->irq);
	} else {
		HAL_NVIC_DisableIRQ(cfg->irq);
		usart_off(cfg->uart);
		clock_on(false);
		stm32_pin_in(cfg->rx_port, cfg->rx_pin, GPIO_NOPULL);
		baud = 0;
	}
}

// Characters and flagged characters received so far: any of them means something drives the line
static uint32_t activity(void)
{
	return rx.bytes + rx.uart_errors;
}

static void sentence(const char *s, uint8_t len, uint32_t now_ms)
{
#if CONFIG_GPS
	position_nmea(s, len, true, now_ms);
#else
	(void)s;
	(void)len;
	(void)now_ms;
#endif
}

static void gps_task(void *arg)
{
	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
		if (power.on) {
			uint32_t now = hal_millis();
			gps_rx_state_t st = gps_rx_poll(&rx, now, sentence);
			switch (gps_baud_step(&hunt, st, now, activity())) {
			case GPS_BAUD_FOUND:
				if (!present) {
					present = true;
					gps_power_found(&power);
					if (cfg->found != NULL) {
						cfg->found();
					}
				}
				break;
			case GPS_BAUD_SWITCH:
				rate_set(gps_baud_rate(&hunt));
				break;
			case GPS_BAUD_ABSENT:
				uart_on(false);		// no module: nothing more to do
				gps_power_absent(&power);
				if (cfg->absent != NULL) {
					cfg->absent();
				}
				task = NULL;
				vTaskDelete(NULL);
				break;
			default:
				break;
			}
		}
		// Settings > Location > GPS, once the module is known: the supply first on the way up, last on the way down,
		// so the USART never listens to an unpowered line. Back on, it takes the rate the hunt found
		if (gps_power_step(&power)) {
			if (power.on && cfg->power != NULL) {
				cfg->power(true);
			}
			uart_on(power.on);
			if (!power.on && cfg->power != NULL) {
				cfg->power(false);
			}
		}
	}
}

void stm32_gps_init(const stm32_gps_cfg_t *c)
{
	cfg = c;
	uint32_t now = hal_millis();
	gps_rx_init(&rx, now, NO_TIMEOUT);
	gps_baud_init(&hunt, c->rates, c->n_rates, c->listen_ms, now, 0);
	present = false;
	gps_power_init(&power);
	uart_on(true);
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

static bool gps_present(void)
{
	return present;
}

static void gps_enable(bool on)
{
	gps_power_set(&power, on);
}

static bool gps_running(void)
{
	return power.on;
}

static void gps_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	gps_rx_counts(&rx, good, bad, bytes);
}

static uint32_t gps_baud(void)
{
	return baud;
}

const hal_gps_t stm32_gps = {
	.present = gps_present,
	.enable = gps_enable,
	.running = gps_running,
	.counts = gps_counts,
	.baud = gps_baud,
};
