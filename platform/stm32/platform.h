// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// STM32 platform services for boards built on one of the STM32 platforms. The code behind them is shared
// (platform/stm32) or the family's own (platform/<family>: clock.c, spi.c, usb.c); what differs between the families
// inside the shared code is in the family's stm32_family.h: its ST HAL, the clock table (stm32_clock_t) and the
// STM32_* names
#ifndef PLATFORM_STM32_H
#define PLATFORM_STM32_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32_family.h"
#include "hal_system.h"

// The board's clock tree from its table (the family's clock.c; the table's fields are the family's). Switch to HSI
// first (the bootloader may leave HSE/PLL running, and HSE can't be changed while it is in use), then bring up HSE and
// the PLL from the table. Returns false and stays on HSI if the HSE or PLL never locks
bool stm32_clock_init(const stm32_clock_t *c);

// System services for board.system (sysboot.c): chip info, memory reads, reboot incl. the ROM DFU entry
extern const hal_system_t stm32_system;
// First thing in main(): enters the ROM bootloader when a REBOOT ROM-DFU request armed it (does not return then)
void stm32_sysboot_check(void);
// The main stack, the interrupt handlers' once the scheduler runs (irq_stack.c): fill its free part (first thing in
// main()), check its lowest words (a stack overflow crash, task "irq"; every tick), and its bytes never written since
// the fill (hal_system_t.irq_stack_free)
void stm32_irq_stack_fill(void);
void stm32_irq_stack_check(void);
uint32_t stm32_irq_stack_free(void);
// Optional board hook, called before the ROM bootloader entry: the board's reset-time pin levels (power latch high,
// transmitter off). Weak default does nothing
void board_sysboot_prepare(void);

// Runtime flash-stub entry (CONFIG_STUB_ENTRY, stub_entry.c): the staging buffer the debug link loads the image into,
// and the quiesce, copy-to-SRAM-base and jump. The transmitter goes off first (board_rf_safe); the power latch, display
// and USB pins stay
uint8_t *stm32_stub_region(uint32_t *size);
bool stm32_enter_stub(uint32_t entry);
// With CONFIG_WATCHDOG and the IWDG running: true only for an image whose header (stub/stm32f4/f4stub_header.h) says
// the stub feeds the IWDG. Otherwise true
bool stm32_stub_image_ok(const uint8_t *img, uint32_t len);
// CONFIG_WATCHDOG (watchdog.c): the IWDG can't be stopped, so before the stub entry it gets its longest timeout (about
// 32 s) and a feed. The stub feeds it from then on; RadioFoundry refuses a stub that doesn't
void stm32_watchdog_stretch(void);

// Enable the clock of a GPIO port by its base address
void stm32_gpio_clock(GPIO_TypeDef *port);

// Simple pin set-up helpers. A pin nobody configured is a floating input on the F4; on the H7 it is in analog mode
// after reset and reads 0. The outputs' slope is the family's (STM32_GPIO_SPEED)
// For board_rf_safe: the port's clock on, the level, then push-pull output, with plain register writes (no HAL). Safe
// in a fault handler and before the clocks are set up
void stm32_pin_force(GPIO_TypeDef *port, uint16_t pins, bool level);
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain);
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull);
void stm32_pin_af(GPIO_TypeDef *port, uint16_t pins, uint8_t af, bool open_drain, uint32_t pull);
void stm32_pin_analog(GPIO_TypeDef *port, uint16_t pins);

// SPI master on the registers (spi.c): mode from cpol / cpha, clock = the block's clock / 2^(br + 1) (PCLK on the F4,
// the SPI kernel clock on the H7), software chip select
void stm32_spi_master_init(SPI_TypeDef *spi, bool cpol, bool cpha, uint8_t br);
// Byte at a time, polled: tx NULL sends 0xFF, rx NULL discards. False on a stuck peripheral
bool stm32_spi_xfer(SPI_TypeDef *spi, const uint8_t *tx, uint8_t *rx, uint32_t len);

// The FSMC data lines D0-D3 on port D and D4-D7 on port E (the 8-bit LCD bus; a keypad may share them, fsmc_keypad)
#define STM32_FSMC_D0_D3	(GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_0 | GPIO_PIN_1)
#define STM32_FSMC_D4_D7	(GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10)

// USB identity (a board's board_usb_ids): the IDs the aftermarket firmware enumerates with, so existing hosts find the
// radio: qdmr and the open-firmware CPS look for this pair, MMDVMHost and the SDR bench tools only need the serial port.
// 0x1FC9 is NXP's vendor ID, not ours: a release should move to an ID of its own (for example a pid.codes PID under
// 0x1209) once the host tools know it
#define STM32_USB_VID		0x1FC9
#define STM32_USB_PID_SERIAL	0x0094
#define STM32_USB_PID_NETWORK	0x0096
// usb_ids_t.serial_number: the chip's 96-bit unique ID in hex (usb.c, CONFIG_USB)
void stm32_usb_serial_number(char *buf, uint8_t len);

static inline void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	port->BSRR = level ? pins : ((uint32_t)pins << 16);
}

static inline bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	return (port->IDR & pin) != 0;
}

// OSPEEDR of pins (the register is the same on both families): for a bus faster than the family's default slope.
// speed is a GPIO_SPEED_FREQ_* value. A read-modify-write of the port's register: from the code that sets the port up
static inline void stm32_pin_speed(GPIO_TypeDef *port, uint16_t pins, uint32_t speed)
{
	uint32_t ospeedr = port->OSPEEDR;
	for (uint32_t n = 0; n < 16u; n++) {
		if (pins & (1u << n)) {
			ospeedr = (ospeedr & ~(3u << (2u * n))) | (speed << (2u * n));
		}
	}
	port->OSPEEDR = ospeedr;
}

// Busy wait of at least ns on the CPU cycle counter (time.c starts it: valid after stm32_time_init()). For bit-banged
// bus timing: a counted loop is 1 to 3 cycles a pass depending on the core, a cycle count is not. Up to 5 ms (ns
// times the core clock in MHz must fit 32 bits: 8.9 ms at 480 MHz). Both divisions round up: never shorter than ns
static inline void stm32_wait_ns(uint32_t ns)
{
	uint32_t cycles = (ns * ((SystemCoreClock + 999999u) / 1000000u) + 999u) / 1000u;
	uint32_t start = DWT->CYCCNT;
	while (DWT->CYCCNT - start < cycles) {
	}
}

#endif
