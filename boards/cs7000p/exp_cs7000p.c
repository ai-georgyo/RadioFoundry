// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS: the one writer of the output expander and of the shared DAC, and what the board does around a
// reset (exp_cs7000p.h has the bits and the rule; docs/spec/hardware-cs7000p.md 4). Nothing here has run on a radio.
//
// The expander's 24 outputs carry every rail, the PA loop, the chip selects and the supply hold. They are random
// from power-up and keep their state through an MCU reset, so they get their word three times over: in the reset
// handler before .data and .bss exist (board_reset_safe), first thing in main() (board_rf_safe), and before every
// reset or jump out of the image (board_reset_prepare, board_sysboot_prepare). None of those words holds the supply:
// the hold is set only after the knob has been read on (power_cs7000p.c).
//
// DAC channel 1 on PA4 is the PA's power reference while transmitting and the front end's tuning voltage while
// receiving. Every write to it and to the chain goes through here, check and write in one interrupt-masked section,
// so the interlock of exp_cs7000p.h holds whoever calls: the radio task, the burst timer's interrupt, a bench write
// over the debug link.
//
// tests/cs7000pExpTest.c runs this file on a model of the three chips.
#include "cs7000p.h"
#include "exp_cs7000p.h"
#include "pins.h"
#include "io/hc595/hc595.h"
#include "periph/dac.h"
#include "romentry_cs7000p.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"

#define APC_DAC			1u	// DAC1 channel 1, PA4
#define APC_DAC_MAX		4095u
// Code 0 is written with the output buffer still on, which takes the pin to its minimum at once (0.2 V at most,
// DS12110); the channel goes off this long after, and the 39k to ground takes the reference the rest of the way
#define APC_ZERO_SETTLE_NS	20000u

static void chain_clk(bool level)
{
	PIN_WRITE(PIN_EXP_CLK, level);
}

static void chain_data(bool level)
{
	PIN_WRITE(PIN_EXP_DATA, level);
}

static void chain_latch(bool level)
{
	PIN_WRITE(PIN_EXP_LATCH, level);
}

// One half period as a fixed count (exp_cs7000p.h, EXP_DELAY_PASSES): the reset hook runs before the cycle counter
// and SystemCoreClock exist
static void chain_delay(void)
{
	for (uint32_t i = EXP_DELAY_PASSES; i != 0u; i--) {
		__NOP();
	}
}

static uint32_t chain_crit_enter(void)
{
	return portSET_INTERRUPT_MASK_FROM_ISR();
}

static void chain_crit_exit(uint32_t saved)
{
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
}

// const, and its functions write registers only: the reset hook uses it before .data and .bss exist
static const hc595_ops_t chain_ops = {
	.clk = chain_clk,
	.data = chain_data,
	.latch = chain_latch,
	.delay = chain_delay,
	.crit_enter = chain_crit_enter,
	.crit_exit = chain_crit_exit,
};

// Initialized data, not hc595_init(): the reset hook has shifted EXP_RESET_WORD before .data was copied, so the
// shadow is what the outputs hold from the first statement of main(), and no state without a shadow exists
static hc595_t chain = { .ops = &chain_ops, .bits = EXP_BITS, .shadow = EXP_RESET_WORD };
// What DAC channel 1 holds. 0: the channel is off (the pin high impedance, the reference at a true 0 V)
static volatile uint16_t dac_value;

// Every pin of ports A to E in analog mode, as a reset leaves most of them, but for PA13 and PA14: they stay the
// debug port (alternate function 0) and are never touched again. PA15, PB3 and PB4 are JTAG lines after a reset and
// analog here: the board has no JTAG, and they are the modem's I2S frame line and voice port
#define MODER_ANALOG		0xFFFFFFFFu
#define MODER_A_ANALOG_SWD	0xEBFFFFFFu
#define OSPEEDR_A_RESET		0x0C000000u	// PA13 very high, as after a reset
#define PUPDR_A_SWD		0x24000000u	// PA13 pull-up, PA14 pull-down, as after a reset

static void port_reset(GPIO_TypeDef *port, uint32_t moder, uint32_t ospeedr, uint32_t pupdr)
{
	port->MODER = moder;	// first: no pin drives from here on
	port->OTYPER = 0;
	port->OSPEEDR = ospeedr;
	port->PUPDR = pupdr;	// (no pull acts on a pin in analog mode, RM0433 11.3.13)
}

// From the reset handler, after the supply configuration and ST's SystemInit, before .data and .bss (the contract is
// in platform/stm32h7/soc.h): registers and const data only. The image may have been started by the ROM bootloader's
// Leave, which enters it with PB14, PB10, PA2, PA9, PE13, PA6 and PC11 still driven push-pull (AN2606 Table 145),
// three of them onto nets a chip or a switch contact drives: every line is taken back first. Then the chain's three
// lines, the data flash's select (no pull-up on the board), and one word that sets all 24 outputs on one latch edge.
// PA4 is left alone: analog mode, so the 39k to ground holds the PA's reference at 0 V whatever the chain held
void board_reset_safe(void)
{
	RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIODEN |
			RCC_AHB4ENR_GPIOEEN;
	(void)RCC->AHB4ENR;	// a read back before the first access to a port, as ST's clock enable macros do
	port_reset(GPIOA, MODER_A_ANALOG_SWD, OSPEEDR_A_RESET, PUPDR_A_SWD);
	port_reset(GPIOB, MODER_ANALOG, 0, 0);
	port_reset(GPIOC, MODER_ANALOG, 0, 0);
	port_reset(GPIOD, MODER_ANALOG, 0, 0);
	port_reset(GPIOE, MODER_ANALOG, 0, 0);
	// The only place these three pins are set up; their slope stays the reset value, low
	stm32_pin_force(PIN_PORT(PIN_EXP_CLK),
			PIN_MASK(PIN_EXP_CLK) | PIN_MASK(PIN_EXP_LATCH) | PIN_MASK(PIN_EXP_DATA), false);
	stm32_pin_force(PIN_PORT(PIN_FLASH_CS), PIN_MASK(PIN_FLASH_CS), true);
	hc595_shift(&chain_ops, EXP_RESET_WORD, EXP_BITS);
}

// The chain in its reset state and the modem's configuration port deselected, for code that writes neither: the ROM
// bootloader. It runs after any reset with SK1 held, drives PB14 (the modem's data output, AN2606 Table 145) and
// never writes the chain; with its select high the modem is expected to release that line. Stateless: no shadow is
// read or written, because nothing of this image runs afterwards
static void outputs_to_reset(void)
{
	hc595_shift(&chain_ops, EXP_RESET_WORD, EXP_BITS);
	stm32_pin_force(PIN_PORT(PIN_DMR_CS), PIN_MASK(PIN_DMR_CS), true);
}

// About a millisecond on the cycle counter. The pass count is capped by the same number, so the wait also ends when
// the counter stands still (a fault before the time base was started): a pass is several cycles, so a step is then
// several milliseconds, and the bound several minutes
static void wait_about_1ms(void)
{
	const uint32_t cycles = SystemCoreClock / 1000u;
	const uint32_t start = DWT->CYCCNT;
	for (uint32_t pass = 0; pass < cycles && DWT->CYCCNT - start < cycles; pass++) {
	}
}

// SK1 read as a plain input, by register: before the pin set-up port D is in analog mode, where its IDR reads 0,
// "down". The board's 47k pull-up holds the line; interrupts are off, so the mode write can't meet another
static bool sk1_down(void)
{
	return (PIN_PORT(PIN_KEY_SK1)->IDR & PIN_MASK(PIN_KEY_SK1)) == 0u;
}

// The wait for SK1's release (romentry_cs7000p.h). The independent watchdog can't be stopped and may be running: fed
// at each step, so it does not take the reset into the loader itself. noinline: inlined, its loop's registers would
// sit in the frame of reboot(), under board_rf_safe() too, at the end of every task's deepest path (an assert)
static void __attribute__((noinline)) sk1_wait(void)
{
	_Static_assert(PIN_MASK(PIN_KEY_SK1) == GPIO_PIN_15, "SK1 is pin 15 of its port");
	PIN_PORT(PIN_KEY_SK1)->MODER &= ~(3u << (2u * 15u));	// 00: an input
	if (!sk1_down()) {
		return;
	}
	cs7000p_sk1_wait_t w = { 0 };
	do {
		wait_about_1ms();
#if CONFIG_WATCHDOG
		hal_watchdog_feed();
#endif
	} while (cs7000p_sk1_wait_more(&w, sk1_down()));
}

// Before every software reset (platform/stm32/sysboot.c reboot(), interrupts off, after board_rf_safe()) and on a
// crash restart from the fault handler: the supply is not held through the reset, so a radio whose knob is off goes
// off. With SK1 down the reset would start the ROM bootloader (romentry_cs7000p.h): the wait for its release comes
// after the chain's reset word, so nothing is on and nothing is held meanwhile
void board_reset_prepare(void)
{
	outputs_to_reset();
	sk1_wait();
}

// Before the jump to the ROM bootloader that a request armed (stm32_sysboot_check(), first thing after a reset)
void board_sysboot_prepare(void)
{
	outputs_to_reset();
}

// The DAC to a true 0 V. Inside a masked section. Nothing when the channel is off already: a write of 0 would
// switch it on (periph/dac.h)
static void dac_zero(void)
{
	if (dac_value == 0u) {
		return;
	}
	stm32_dac_write(APC_DAC, 0);
	stm32_wait_ns(APC_ZERO_SETTLE_NS);
	stm32_dac_off(APC_DAC);
	dac_value = 0;
}

// Transmitter off from any context (hal/board.h, docs/spec/crash-log.md 8): fault handlers, the reboot path, the
// watchdog's hang check, first thing in main(). Register writes only, no wait, no lock. The DAC first: it acts at
// once, the loop needs about a millisecond. Then the chain, the shadow changed before it is shifted: code that runs
// on after this call (a power-off the knob overrides) can't bring a transmit bit back with its next update. The word
// is shifted without the driver's state, so it also cuts into an update that a fault interrupted.
//
// These writes are the only ones: the radio's sequencer is told its lines moved and writes nothing itself
// (cs7000p_radio_rf_safe, chainseq_safed). So it is told inside the same masked section. From a task (latch(false)),
// a burst point the timer or the modem's interrupt ran between the unmask and the radio's safe flag would key the PA
// again, and nothing after it would take that back
void board_rf_safe(void)
{
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	stm32_dac_force_zero(APC_DAC);	// nothing while the DAC's clock is off: the pin is then in its reset state
	dac_value = 0;
	chain.shadow = exp_rf_safe_word(chain.shadow);
	hc595_shift(&chain_ops, chain.shadow, EXP_BITS);
	cs7000p_radio_rf_safe();	// flags and state only (nothing while the radio is not up)
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
}

bool cs7000p_exp_set(uint32_t set, uint32_t clear)
{
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	const bool ok = exp_set_ok(set, dac_value, CS7000P_APC_FLOOR);
	if (ok) {
		// A clear is never refused. One that would leave a single transmit bit under a reference above the floor
		// takes the reference away first, so the rule of exp_dac_ok holds after every write
		if (!exp_dac_ok((chain.shadow & ~clear) | set, dac_value, CS7000P_APC_FLOOR)) {
			dac_zero();
		}
		hc595_update(&chain, set, clear);
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

uint32_t cs7000p_exp_get(void)
{
	return hc595_get(&chain);
}

bool cs7000p_apc_dac(uint16_t value)
{
	const uint32_t saved = portSET_INTERRUPT_MASK_FROM_ISR();
	bool ok = true;
	if (value == 0u) {
		dac_zero();
	} else if (value > APC_DAC_MAX || (RCC->APB1LENR & RCC_APB1LENR_DAC12EN) == 0u ||
		   !exp_dac_ok(chain.shadow, value, CS7000P_APC_FLOOR)) {
		ok = false;	// out of range, the DAC not set up yet (board_early_init), or the interlock
	} else {
		stm32_dac_write(APC_DAC, value);	// the data register, and the channel on if it was off
		dac_value = value;
	}
	portCLEAR_INTERRUPT_MASK_FROM_ISR(saved);
	return ok;
}

uint16_t cs7000p_apc_dac_get(void)
{
	return dac_value;
}
