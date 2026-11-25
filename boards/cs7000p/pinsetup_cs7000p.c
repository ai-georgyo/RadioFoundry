// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS: every pin's idle state, set once from board_early_init() (docs/spec/hardware-cs7000p.md 3).
// Host-clean: only platform.h's pin functions, so tests/cs7000pPinsTest.c runs the table against the list of lines
// that must never be driven. What has run on a radio is the bring-up page's First radio
// (docs/testing/cs7000p-bringup.md).
//
// Every pin arrives in analog mode (board_reset_safe, exp_cs7000p.c). A pin that a driver takes over later (an
// alternate function, an interrupt line) gets the state here that is safe until then. Not in the table, and never
// set up anywhere else: PE7-PE9, the expander's three lines, which the reset hook alone makes outputs, and PA13 /
// PA14, the debug port.
#include "cs7000p.h"
#include "pins.h"

typedef enum {
	IDLE_ANALOG,		// not read as a level: an ADC or DAC line, or a line nothing defines
	IDLE_IN,		// input; the board has the pull resistor, or the far end always drives
	IDLE_IN_UP,		// input with the pull-up
	IDLE_IN_DOWN,		// input with the pull-down
	IDLE_LOW,		// push-pull output, low
	IDLE_HIGH,		// push-pull output, high
} idle_t;

static const struct {
	GPIO_TypeDef *port;
	uint16_t pin;
	uint8_t idle;
} idle[] = {
	// The accessory connector's UART and the Bluetooth module's: nothing defines the lines' level (the module is
	// unpowered), so no input stage is left on them
	{ PIN_ACC_UART_TX, IDLE_ANALOG },
	{ PIN_ACC_UART_RX, IDLE_ANALOG },
	{ PIN_BT_TX, IDLE_ANALOG },
	{ PIN_BT_RX, IDLE_ANALOG },
	// ADC inputs (the scan's six, and the three not scanned yet) and the pad without one
	{ PIN_SUBAUDIO_IN, IDLE_ANALOG },
	{ PIN_RX_AUDIO, IDLE_ANALOG },
	{ PIN_POW_DET, IDLE_ANALOG },
	{ PIN_MIC_AUDIO, IDLE_ANALOG },
	{ PIN_TONE_IN, IDLE_ANALOG },
	{ PIN_VOX, IDLE_ANALOG },
	{ PIN_VOL_DET, IDLE_ANALOG },
	{ PIN_RSSI, IDLE_ANALOG },
	{ PIN_NOISE, IDLE_ANALOG },
	{ PIN_BAT_DET, IDLE_ANALOG },
	// The DAC's two pins. PA4 stays high impedance until the first reference is written: 39k to ground holds the
	// PA off (exp_cs7000p.c)
	{ PIN_APC_TV, IDLE_ANALOG },
	{ PIN_BEEP, IDLE_ANALOG },
	// USB: the platform's driver takes both when it starts
	{ PIN_USB_DM, IDLE_ANALOG },
	{ PIN_USB_DP, IDLE_ANALOG },

	// Keys with their own line, low = pressed, pulled up on the board
	{ PIN_PTT, IDLE_IN },
	{ PIN_KEY_SK1, IDLE_IN },
	{ PIN_KEY_SK2, IDLE_IN },
	{ PIN_KEY_SK3, IDLE_IN },
	{ PIN_KEY_TOP, IDLE_IN },
	{ PIN_EXT_PTT, IDLE_IN },
	// The channel selector: its contacts go to ground, the board pulls the lines up
	{ PIN_CHSW_0, IDLE_IN },
	{ PIN_CHSW_1, IDLE_IN },
	{ PIN_CHSW_2, IDLE_IN },
	{ PIN_CHSW_3, IDLE_IN },

	// The LCD board's connector: inputs with pull-ups, all of them, until the display code has read the board as
	// the assumed one (display_cs7000p.c). The board is not in the maker's schematic
	{ PIN_LCD_D0, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_1, IDLE_IN_UP },	// D1..D7, K2..K8
	{ GPIOD, GPIO_PIN_2, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_3, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_4, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_5, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_6, IDLE_IN_UP },
	{ GPIOD, GPIO_PIN_7, IDLE_IN_UP },
	{ PIN_KEY_K9, IDLE_IN_UP },
	{ PIN_LCD_DC, IDLE_IN_UP },
	{ PIN_LCD_WR, IDLE_IN_UP },
	{ PIN_LCD_RD, IDLE_IN_UP },
	{ PIN_LCD_RES, IDLE_IN_UP },
	{ PIN_LCD_CS, IDLE_IN_UP },
	// The backlight's control line is on the main board: off
	{ PIN_LCD_BACKLIGHT, IDLE_LOW },

	// HR_C6000. Its configuration port: deselected, clock and data low; its data output is never driven. Its
	// select has no pull-up on the board
	{ PIN_DMR_CS, IDLE_HIGH },
	{ PIN_DMR_CLK, IDLE_LOW },
	{ PIN_DMR_MOSI, IDLE_LOW },
	{ PIN_DMR_MISO, IDLE_IN },
	// Its voice port, the same way
	{ PIN_DMR_VCS, IDLE_HIGH },
	{ PIN_DMR_VCLK, IDLE_LOW },
	{ PIN_DMR_VMOSI, IDLE_LOW },
	{ PIN_DMR_VMISO, IDLE_IN },
	// Its interrupt lines, the chip's outputs
	{ PIN_DMR_INT_TS, IDLE_IN },
	{ PIN_DMR_INT_RFTX, IDLE_IN },
	{ PIN_DMR_INT_SYS, IDLE_IN },
	// Its I2S port: the chip may be the master of the frame line and the clock, and which data pad the MCU drives
	// is not known before the platform's probe (platform/stm32h7/hrc/i2s_pcm_soc.h). Inputs, at a defined level
	// while the chip sleeps
	{ PIN_I2S_WS, IDLE_IN_DOWN },
	{ PIN_I2S_CK, IDLE_IN_DOWN },
	{ PIN_I2S_TO_CHIP, IDLE_IN_DOWN },
	{ PIN_I2S_FROM_CHIP, IDLE_IN_DOWN },

	// SKY72310: deselected, data and clock low; the lock line is the chip's output (10k to its supply)
	{ PIN_PLL_CS, IDLE_HIGH },
	{ PIN_PLL_DAT, IDLE_LOW },
	{ PIN_PLL_CLK, IDLE_LOW },
	{ PIN_PLL_LD, IDLE_IN },
	// AK2365A: held in reset, the clock low. The data line is an output only inside a frame: PC13 and PC14 are
	// backup-domain pads, and RM0433 6.4.4 wants one of them an output at a time (pins.h)
	{ PIN_AK_RSTN, IDLE_LOW },
	{ PIN_AK_SCLK, IDLE_LOW },
	{ PIN_AK_SDATA, IDLE_IN_DOWN },

	// Data flash: deselected (the reset hook did that already), clock and data low until the SPI block takes them;
	// its data output is never driven
	{ PIN_FLASH_CS, IDLE_HIGH },
	{ PIN_FLASH_SCK, IDLE_LOW },
	{ PIN_FLASH_MOSI, IDLE_LOW },
	{ PIN_FLASH_MISO, IDLE_IN_DOWN },

	// Sub-audio out: low until the PWM takes it a few statements on (board.c). It has no pull resistor and must
	// not float once the synthesizer's rail is on
	{ PIN_SUBAUDIO_PWM, IDLE_LOW },
	// GPS: the module's output, and its input, which has the module's own pull-up and is not used
	{ PIN_GPS_RX, IDLE_IN },
	{ PIN_GPS_TX, IDLE_IN },
	// Not used, pulled up on the board: the accelerometer's data line and the bus to IC611
	{ PIN_ACC_SDA, IDLE_IN },
	{ PIN_I2C_SCL, IDLE_IN },
	{ PIN_I2C_SDA, IDLE_IN },
};

void cs7000p_reset_levels(void)
{
	for (unsigned i = 0; i < sizeof(idle) / sizeof(idle[0]); i++) {
		switch (idle[i].idle) {
		case IDLE_ANALOG:
			stm32_pin_analog(idle[i].port, idle[i].pin);
			break;
		case IDLE_IN:
			stm32_pin_in(idle[i].port, idle[i].pin, GPIO_NOPULL);
			break;
		case IDLE_IN_UP:
			stm32_pin_in(idle[i].port, idle[i].pin, GPIO_PULLUP);
			break;
		case IDLE_IN_DOWN:
			stm32_pin_in(idle[i].port, idle[i].pin, GPIO_PULLDOWN);
			break;
		default:	// the level before the pin becomes an output
			stm32_pin_out(idle[i].port, idle[i].pin, idle[i].idle == IDLE_HIGH, false);
			break;
		}
	}
}
