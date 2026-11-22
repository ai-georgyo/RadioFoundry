// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for platform/stm32/platform.h, for the tests of the shared STM32 glue that works on GPIO registers
// (lcd8080GpioTest, gpioKeypadTest) or only needs the port type (dmrKeyupTest). The ports are fake registers with the
// real layout's names; GPIO_PIN_n and the pull codes are the real values.
//
// The glue writes some registers itself (a port's BSRR for a data byte, MODER and PUPDR to turn lines round) and
// calls platform.h for the rest. A plain struct can't see a write when it happens, so the fake looks at the registers
// at every call the glue makes into this file (stm32_pin_write, _read, _out, _in, stm32_wait_ns): a pending BSRR
// write is applied, the pins' levels are worked out, and the test's watcher sees them. That is the order the pins
// change in, as long as the code under test does not write one port's BSRR twice with none of those calls between
// (the fake would see the second write only).
//
// A test that wants the fake defines STM32GPIO_FAKE_MODEL before the include, in its one source file: the model below
// is then compiled there.
#ifndef STM32GPIO_STUB_PLATFORM_H
#define STM32GPIO_STUB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR;
} GPIO_TypeDef;

#define FAKE_PORTS		5
extern GPIO_TypeDef fake_ports[FAKE_PORTS];
#define GPIOA			(&fake_ports[0])
#define GPIOB			(&fake_ports[1])
#define GPIOC			(&fake_ports[2])
#define GPIOD			(&fake_ports[3])
#define GPIOE			(&fake_ports[4])

#define GPIO_PIN_0		((uint16_t)(1u << 0))
#define GPIO_PIN_1		((uint16_t)(1u << 1))
#define GPIO_PIN_2		((uint16_t)(1u << 2))
#define GPIO_PIN_3		((uint16_t)(1u << 3))
#define GPIO_PIN_4		((uint16_t)(1u << 4))
#define GPIO_PIN_5		((uint16_t)(1u << 5))
#define GPIO_PIN_6		((uint16_t)(1u << 6))
#define GPIO_PIN_7		((uint16_t)(1u << 7))
#define GPIO_PIN_8		((uint16_t)(1u << 8))
#define GPIO_PIN_9		((uint16_t)(1u << 9))
#define GPIO_PIN_10		((uint16_t)(1u << 10))
#define GPIO_PIN_11		((uint16_t)(1u << 11))
#define GPIO_PIN_12		((uint16_t)(1u << 12))
#define GPIO_PIN_13		((uint16_t)(1u << 13))
#define GPIO_PIN_14		((uint16_t)(1u << 14))
#define GPIO_PIN_15		((uint16_t)(1u << 15))

#define GPIO_NOPULL		0x00000000u
#define GPIO_PULLUP		0x00000001u
#define GPIO_PULLDOWN		0x00000002u
#define STM32_GPIO_SPEED	0x00000001u	// a family's slope code: any value the test can find in OSPEEDR again

// The core's interrupt switch, as the glue uses it around a register's read-modify-write
extern int fake_primask;
static inline uint32_t __get_PRIMASK(void) { return (uint32_t)fake_primask; }
static inline void __disable_irq(void) { fake_primask = 1; }
static inline void __set_PRIMASK(uint32_t primask) { fake_primask = (int)primask; }

// platform.h's own, each a point where the fake looks at the registers
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level);
bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin);
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain);
void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull);
void stm32_pin_speed(GPIO_TypeDef *port, uint16_t pins, uint32_t speed);
void stm32_wait_ns(uint32_t ns);

#ifdef STM32GPIO_FAKE_MODEL

// A pin's level: an output's is its ODR bit. An input's is what the world outside drives (fake_drive), else its
// pull, else nothing: a floating input reads a level that changes from look to look, so a read that depends on one
// does not pass by luck. With fake_hold set, a floating input keeps the level it had at the last look instead, as a
// line with only its capacitance does for microseconds
GPIO_TypeDef fake_ports[FAKE_PORTS];
int fake_primask;
bool fake_hold;
uint64_t fake_ns;			// the waits so far
uint32_t fake_fights;			// looks that found an output against the world's other level on its pin
uint32_t fake_float_reads;		// stm32_pin_read of an input with no driver and no pull
void (*fake_watch)(void);		// the test's watcher, after every look; it may call fake_drive / fake_release
static uint32_t fake_ext_mask[FAKE_PORTS], fake_ext_level[FAKE_PORTS];
static uint32_t fake_noise = 0x2545F491u;

static inline uint32_t fake_field2(uint32_t reg, uint32_t n)
{
	return (reg >> (2u * n)) & 3u;
}

static inline uint32_t fake_set2(uint32_t reg, uint16_t pins, uint32_t value)
{
	for (uint32_t n = 0; n < 16u; n++) {
		if (pins & (1u << n)) {
			reg = (reg & ~(3u << (2u * n))) | (value << (2u * n));
		}
	}
	return reg;
}

// The pins of port that are outputs, as a mask
static inline uint16_t fake_outputs(const GPIO_TypeDef *port)
{
	uint16_t out = 0;
	for (uint32_t n = 0; n < 16u; n++) {
		if (fake_field2(port->MODER, n) == 1u) {
			out |= (uint16_t)(1u << n);
		}
	}
	return out;
}

// The pins of port with this pull code (GPIO_PULLUP, ...), as a mask
static inline uint16_t fake_pulled(const GPIO_TypeDef *port, uint32_t pull)
{
	uint16_t m = 0;
	for (uint32_t n = 0; n < 16u; n++) {
		if (fake_field2(port->PUPDR, n) == pull) {
			m |= (uint16_t)(1u << n);
		}
	}
	return m;
}

// Inputs that nothing drives and nothing pulls
static inline uint16_t fake_floating(const GPIO_TypeDef *port)
{
	uint32_t i = (uint32_t)(port - fake_ports);
	return (uint16_t)(~fake_outputs(port) & ~fake_ext_mask[i] & fake_pulled(port, GPIO_NOPULL));
}

static inline void fake_levels(void)
{
	fake_noise ^= fake_noise << 13;
	fake_noise ^= fake_noise >> 17;
	fake_noise ^= fake_noise << 5;
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		GPIO_TypeDef *p = &fake_ports[i];
		uint32_t out = fake_outputs(p);
		uint32_t in = (fake_pulled(p, GPIO_PULLUP) & ~fake_ext_mask[i]) | (fake_ext_level[i] & fake_ext_mask[i]) |
			      (fake_floating(p) & (fake_hold ? p->IDR : fake_noise >> i));
		if ((out & fake_ext_mask[i] & (p->ODR ^ fake_ext_level[i])) != 0u) {
			fake_fights++;
		}
		p->IDR = ((p->ODR & out) | (in & ~out)) & 0xFFFFu;
	}
}

// A look at the registers: what the code under test wrote since the last one takes effect, in the order the real
// pins would see it
static inline void fake_sync(void)
{
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		GPIO_TypeDef *p = &fake_ports[i];
		uint32_t bsrr = p->BSRR;
		p->BSRR = 0;
		p->ODR = ((p->ODR & ~(bsrr >> 16)) | (bsrr & 0xFFFFu)) & 0xFFFFu;	// set wins over reset, as on the chip
	}
	fake_levels();
	if (fake_watch != NULL) {
		fake_watch();
		fake_levels();	// with what the watcher drives now
	}
}

// The world outside drives pins of port at levels, or lets them go
static inline void fake_drive(GPIO_TypeDef *port, uint16_t pins, uint16_t levels)
{
	uint32_t i = (uint32_t)(port - fake_ports);
	fake_ext_mask[i] |= pins;
	fake_ext_level[i] = (fake_ext_level[i] & ~(uint32_t)pins) | (levels & pins);
}

static inline void fake_release(GPIO_TypeDef *port, uint16_t pins)
{
	uint32_t i = (uint32_t)(port - fake_ports);
	fake_ext_mask[i] &= ~(uint32_t)pins;
}

// A pin's level as of the last look
static inline bool fake_level(const GPIO_TypeDef *port, uint16_t pin)
{
	return (port->IDR & pin) != 0u;
}

// Every port as a chip leaves reset in this model: inputs without pulls, nothing driven from outside
static inline void fake_reset(void)
{
	for (uint32_t i = 0; i < FAKE_PORTS; i++) {
		fake_ports[i] = (GPIO_TypeDef){ .MODER = 0 };
		fake_ext_mask[i] = 0;
		fake_ext_level[i] = 0;
	}
	fake_primask = 0;
	fake_hold = false;
	fake_ns = 0;
	fake_fights = 0;
	fake_float_reads = 0;
	fake_sync();
}

void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level)
{
	fake_sync();
	port->BSRR = level ? pins : (uint32_t)pins << 16;
	fake_sync();
}

bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin)
{
	fake_sync();
	if (fake_floating(port) & pin) {
		fake_float_reads++;
	}
	return (port->IDR & pin) != 0u;
}

// As the real ones: the level before the mode, so the pin never glitches; an output has no pull
void stm32_pin_out(GPIO_TypeDef *port, uint16_t pins, bool level, bool open_drain)
{
	stm32_pin_write(port, pins, level);
	port->OTYPER = open_drain ? (port->OTYPER | pins) : (port->OTYPER & ~(uint32_t)pins);
	port->OSPEEDR = fake_set2(port->OSPEEDR, pins, STM32_GPIO_SPEED);
	port->PUPDR = fake_set2(port->PUPDR, pins, GPIO_NOPULL);
	port->MODER = fake_set2(port->MODER, pins, 1u);
	fake_sync();
}

void stm32_pin_in(GPIO_TypeDef *port, uint16_t pins, uint32_t pull)
{
	fake_sync();
	port->PUPDR = fake_set2(port->PUPDR, pins, pull);
	port->MODER = fake_set2(port->MODER, pins, 0u);
	fake_sync();
}

void stm32_pin_speed(GPIO_TypeDef *port, uint16_t pins, uint32_t speed)
{
	port->OSPEEDR = fake_set2(port->OSPEEDR, pins, speed);
}

// The look comes first: what was written before the wait holds for all of it
void stm32_wait_ns(uint32_t ns)
{
	fake_sync();
	fake_ns += ns;
}

#endif

#endif
