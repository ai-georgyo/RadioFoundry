// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Connect Systems CS7000 M17 PLUS: board descriptor, clock, start-up order, key map
// (docs/spec/hardware-cs7000p.md). Written from the maker's schematic: nothing here has run on a radio
// (docs/testing/cs7000p-bringup.md).
//
// What happens before board_early_init() is in exp_cs7000p.c: the reset hook that takes every pin back and sets the
// output expander, and board_rf_safe().
#include "board.h"
#include "config.h"
#include "cs7000p.h"
#include "pins.h"
#include "romentry_cs7000p.h"
#include "hrc/dmr_glue.h"
#include "gps_uart/gps_uart.h"
#include "spi_flash/spi_flash.h"
#include "periph/dac.h"
#include "periph/pwm.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "ui/fmt.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif
// The channel selector's knob, CS7000P_CHSW_VERIFIED: its one default is with the selector's table
#include "keys_cs7000p.h"
#include "subpwm_cs7000p.h"	// PC8's period and idle duty, the sub-audio generator's
#include "rfplan_cs7000p.h"	// the band edges
// 1: allow the software entry to the ROM bootloader under the rule of romentry_cs7000p.h (board_rom_entry_ok below),
// so an update needs no key held at power-on. 0 refuses it always. On since a radio's owner asked for it with the
// rule's refusal kept (docs/testing/cs7000p-bringup.md)
#ifndef CS7000P_SOFT_ROM_ENTRY
#define CS7000P_SOFT_ROM_ENTRY	1
#endif

// One band. Receive 400-527 MHz: the maker's manual ("the frequency of this radio is 400-527 MHz"). Transmit only
// where the FCC grant has it, 400.0-406.0 and 406.1-512.0 MHz: the 100 kHz between the two rows is the distress
// beacon band, receive only, as the grant leaves it. Those rows bound the emission and a band is a range of
// carriers, so the inner edges are half a 25 kHz channel inside them. The edges are rfplan_cs7000p.h's, which the
// transmit check uses (tests/cs7000pRfTest.c), so the core's band check and the board's can't differ. Whether the
// synthesizer locks over all of it is a bring-up measurement
static const band_t bands[] = {
	{ CS7000P_TX_MIN_HZ, CS7000P_TX_GAP_LO_HZ, true },
	{ CS7000P_TX_GAP_LO_HZ, CS7000P_TX_GAP_HI_HZ, false },
	{ CS7000P_TX_GAP_HI_HZ, CS7000P_TX_MAX_HZ, true },
	{ CS7000P_TX_MAX_HZ, CS7000P_RX_MAX_HZ, false },
};
_Static_assert(CS7000P_RX_MIN_HZ == CS7000P_TX_MIN_HZ, "the band table starts where receive does");

// The certified figures (FCC Part 90 report: low 1 W, high 4 W). The maker's manual says 1 W and 5 W; the drives
// are set to the names on a wattmeter (docs/testing/power-calibration.md), and until then nothing transmits
static const char *const power_names[] = { "1W", "4W" };

// docs/ux/information-architecture.md 7.1. The matrix positions of the six function switches were read on a radio
// (keys_cs7000p.h): the LCD board's drawing has no legend for them
static const keymap_entry_t keymap[] = {
	{ KEY_OK, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_BACK, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, '#' },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_F1, ACT_USER_1, ACT_USER_1, false, false, 0 },
	{ KEY_F2, ACT_USER_4, ACT_USER_4, false, false, 0 },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_SK3, ACT_USER_3, ACT_USER_3, false, false, 0 },
	{ KEY_TOP, ACT_NONE, ACT_EMERGENCY, false, false, 0 },	// the emergency key: only a long press
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },		// the side PTT, or the accessory's
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom, the two
// function keys, the top key
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2, KEY_SK3, KEY_F1, KEY_F2, KEY_TOP };
static const char *const side_key_names[] = { "SK1", "SK2", "SK3", "F1", "F2", "TK" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 7, the single-band 160x128 STM32 mapping
static uint32_t cps_radio_type(void)
{
	return 7;
}

// No security_read: the data flash's security registers are the maker's, and nothing of this radio's is read there
static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 2u * 1024u * 1024u,
};
#endif

// The support info (docs/spec/support-info.md 2): the parts this board's drivers run, and what differs between
// units of the processor: its silicon revision and the version of its ROM bootloader, which the install route and
// two errata depend on
static bool hw_line(uint8_t i, uint8_t *role, char *buf, uint8_t len)
{
	sbuf_t b;

	switch (i) {
	case 0:
		*role = BOARD_HW_DMR;
		str_copy(buf, len, "HR_C6000");
		return true;
	case 1:
		*role = BOARD_HW_RF;
		str_copy(buf, len, "SKY72310 + AK2365A");
		return true;
	case 2: {
		const uint16_t rev = stm32h7_rev_id();
		const uint8_t loader = stm32h7_rom_loader_id();
		*role = BOARD_HW_BOARD;
		sb_init(&b, buf, len);
		sb_str(&b, "rev ");
		if (rev == 0x1003u || rev == 0x2001u || rev == 0x2003u) {
			sb_chr(&b, rev == 0x1003u ? 'Y' : rev == 0x2001u ? 'X' : 'V');
		} else {
			sb_hex(&b, rev, 4);
		}
		sb_str(&b, ", loader ");
		sb_u32(&b, loader >> 4, 1);	// the ROM's version byte: 0x91 is 9.1 (AN2606)
		sb_chr(&b, '.');
		sb_u32(&b, loader & 0xFu, 1);
		return true;
	}
	default:
		return false;
	}
}

const board_t board = {
	.name = "CS7000 M17 PLUS",
	// CAP_VOLUME_POT: the knob sets the volume in the analog path, the MCU only sees its position. CAP_ENCODER with
	// the channel selector's code table, which was read on a radio (keys_cs7000p.h)
	.caps = CAP_KEYPAD | CAP_VOLUME_POT | (CS7000P_CHSW_VERIFIED ? CAP_ENCODER : 0) | (CONFIG_GPS ? CAP_GPS : 0) |
		(CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,	// 1 W
	.display = &cs7000p_display,
	.input = &cs7000p_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.radio = &cs7000p_radio,
	.hw_line = hw_line,
	.power = &cs7000p_power,
	.flash = &stm32_spi_flash,
#if CONFIG_DIAG
	.diag_line = cs7000p_diag_line,
	.diag_reset = cs7000p_diag_reset,
#endif
#if CONFIG_DEBUGLINK
	.view_line = cs7000p_view_line,
	.view_split = CS7000P_VIEW_SPLIT,	// the maker's table and its copy in a second section, board2
	.chip_reg = cs7000p_chip_reg,	// the bench's way to the chips, a debug link request
#endif
#if CONFIG_DMR
	.dmr = &stm32_hrc_dmr,
	// The PCM stream is DMR-only code, so the beeps are too: without DMR, FM audio leaves by the modem's line out
	// and the radio is silent otherwise. No vocoder: no codec route is known for this radio, so DMR voice is refused
	// by the core and DMR data, the hotspot and FM are what runs
	.audio = &cs7000p_audio,
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// PPP on the USB serial port, the network adapter under it (board_init)
#elif CONFIG_USB_NETWORK
	.netif = &usb_ncm_netif,
#endif
#endif
	.system = &stm32_system,
	// A 2-cell pack, as the other 7.4 V radios: low 6.2 V, critical 6.0 V (our choice). The reading's scale is a
	// model (battery_cs7000p.h)
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_GPS
	.gps = &stm32_gps,
#endif
#if CONFIG_FACTORY_COPY
	// The maker's factory table (the map's first factory range) and its copy in the board sector, 0x01B000
	// (radio_cs7000p.c, hardware-cs7000p.md 10.3)
	.factory_copy = &cs7000p_factory_copy,
#endif
};

// The 25 MHz crystal through PLL1: a 1 MHz input, the VCO at 384 MHz, 192 MHz system clock, 96 MHz for the bus
// matrix and the timers, 48 MHz on every peripheral bus and, from the Q output, for USB and the SPI1-3 kernel clock.
// All inside the limits of the voltage scale a reset leaves (VOS3), which the platform never changes
// (platform/stm32h7/clock_math.h). If the crystal does not start the platform runs the same rates from the internal
// oscillator, and the radio says so (board.clock)
#define CLOCK_PLL_M		25u
#define CLOCK_PLL_N		384u
#define CLOCK_PLL_P		2u
#define CLOCK_PLL_Q		8u
#define CLOCK_AHB_DIV		2u
#define CLOCK_APB_DIV		2u
#define CLOCK_FLASH_WS		2u
#define CLOCK_VCO_HZ		(HSE_VALUE / CLOCK_PLL_M * CLOCK_PLL_N)
#define CLOCK_HCLK_HZ		(CLOCK_VCO_HZ / CLOCK_PLL_P / CLOCK_AHB_DIV)
// The platform refuses a table outside its limits at run time and stays on the bare internal oscillator (USB from
// HSI48, no SPI1-3 kernel clock). The same limits here (stm32h7_clock_check() in clock_math.h, which
// tests/host/stm32h7ClockTest.c runs on these numbers), so that such a table never builds
_Static_assert(HSE_VALUE % CLOCK_PLL_M == 0 && CLOCK_PLL_M <= 63u && HSE_VALUE / CLOCK_PLL_M >= STM32H7_PLL_IN_MIN_HZ &&
	       HSE_VALUE / CLOCK_PLL_M <= STM32H7_PLL_IN_SPLIT_HZ, "PLL1 input: 1 to 2 MHz with the medium VCO");
_Static_assert(CLOCK_PLL_N >= 4u && CLOCK_PLL_N <= 512u && CLOCK_VCO_HZ >= STM32H7_VCO_MEDIUM_MIN_HZ &&
	       CLOCK_VCO_HZ <= STM32H7_VCO_MEDIUM_MAX_HZ, "PLL1 VCO inside the medium range");
_Static_assert((CLOCK_PLL_P == 1u || CLOCK_PLL_P % 2u == 0) && CLOCK_PLL_P <= 128u && CLOCK_PLL_Q <= 128u &&
	       CLOCK_VCO_HZ % CLOCK_PLL_Q == 0, "PLL1 output dividers");
_Static_assert(CLOCK_VCO_HZ / CLOCK_PLL_P <= STM32H7_CPU_MAX_HZ && CLOCK_VCO_HZ / CLOCK_PLL_Q == STM32H7_USB_HZ,
	       "192 MHz system clock at most 200 MHz, and exactly 48 MHz for USB");
_Static_assert(CLOCK_AHB_DIV == 2u && CLOCK_APB_DIV == 2u && CLOCK_HCLK_HZ <= STM32H7_HCLK_MAX_HZ &&
	       CLOCK_HCLK_HZ / CLOCK_APB_DIV <= STM32H7_PCLK_MAX_HZ &&
	       2u * (CLOCK_HCLK_HZ / CLOCK_APB_DIV) <= STM32H7_TIMER_MAX_HZ, "bus and timer clocks inside the VOS3 limits");
_Static_assert(CLOCK_HCLK_HZ > 90000000u && CLOCK_HCLK_HZ <= 135000000u && CLOCK_FLASH_WS == 2u,
	       "two flash wait states are the row for this bus clock (RM0433 Table 17)");
// And the tree without the crystal exists: the same VCO from the internal oscillator's 3.2 MHz, in the wide range
_Static_assert(CLOCK_VCO_HZ % (STM32H7_HSI_HZ / STM32H7_FALLBACK_M) == 0 && CLOCK_VCO_HZ >= STM32H7_VCO_WIDE_MIN_HZ &&
	       CLOCK_VCO_HZ <= STM32H7_VCO_WIDE_MAX_HZ, "the same rates can come from the internal oscillator");

static const stm32_clock_t clock_crystal = {
	.hse_hz = HSE_VALUE,		// X603, a crystal: no bypass
	.hse_bypass = false,
	.pll_m = CLOCK_PLL_M,
	.pll_n = CLOCK_PLL_N,
	.pll_p = CLOCK_PLL_P,
	.pll_q = CLOCK_PLL_Q,
	.vco_medium = true,
	.cpu_div = 1,
	.ahb_div = CLOCK_AHB_DIV,
	.apb_div = CLOCK_APB_DIV,
	.flash_latency = CLOCK_FLASH_WS,
};

// PC8, sub-audio out: TIM3 channel 3 at 96 MHz / 1024 = 93.75 kHz. Its mean level is part of the reference
// oscillator's control voltage (hardware-cs7000p.md 7.6), so the line carries 50 % from here on, tone or not, and
// the sub-audio generator moves the duty around it (subtone_cs7000p.c)
#define PC8_PWM_TIMER		TIM3
#define PC8_PWM_CHANNEL		3u
#define PC8_PWM_PERIOD		CS7000P_SUBPWM_PERIOD
#define PC8_PWM_IDLE		CS7000P_SUBPWM_IDLE

static SemaphoreHandle_t lcdbus;

// The LCD board's data lines are the display bus and the key matrix: one lock for both users. Nothing before the
// scheduler runs
void cs7000p_lcdbus_lock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreTakeRecursive(lcdbus, portMAX_DELAY);
	}
}

void cs7000p_lcdbus_unlock(void)
{
	if (lcdbus != NULL) {
		xSemaphoreGiveRecursive(lcdbus);
	}
}

// Whether a REBOOT to the ROM bootloader may go ahead (platform/stm32/sysboot.c, which the debug link asks before it
// answers). The rule and why are in romentry_cs7000p.h. Both reads are side-effect free, so they are made with the
// knob off too: the rule stays in one tested place
bool board_rom_entry_ok(void)
{
	return cs7000p_rom_entry_ok(CS7000P_SOFT_ROM_ENTRY != 0, stm32h7_rom_loader_id(), PIN_READ(PIN_CHSW_2));
}

// The expander holds its reset word and every pin is in analog mode (the reset hook). Its shadow needs no set-up:
// it is initialized data (exp_cs7000p.c)
void board_early_init(void)
{
	cs7000p_reset_levels();
	// False when the crystal or its PLL did not start: the platform has then gone on by itself, to the same rates
	// from the internal oscillator or, failing that too, to the bare oscillator (USB from HSI48, no SPI1-3 kernel
	// clock)
	(void)stm32_clock_init(&clock_crystal);
	// The shared DAC set up and left off: PA4 high impedance, the PA's reference at 0 V through the board's 39k,
	// before any rail comes up. From here on exp_cs7000p.c is its only writer
	stm32_dac_init(1, 0);
	stm32_dac_off(1);
	stm32_pwm_init(PC8_PWM_TIMER, PC8_PWM_CHANNEL, PIN_PORT(PIN_SUBAUDIO_PWM), PIN_MASK(PIN_SUBAUDIO_PWM),
		       GPIO_AF2_TIM3, PC8_PWM_PERIOD, PC8_PWM_IDLE);
	// PC9 to its timer, dark, here and not in the display's init: port C's pin modes have writers in the GPS and
	// audio tasks later, and no task exists yet (display_cs7000p.c)
	cs7000p_backlight_init();
}

void board_init(void)
{
	cs7000p_power_start();	// first: the knob is read and, if it is on, the supply held
	lcdbus = xSemaphoreCreateRecursiveMutex();
	cs7000p_flash_init();
#if CONFIG_NET_PPP && CONFIG_USB_NETWORK
	hostlink_ppp_set_usb(&usb_ncm_netif);
#endif
#if CONFIG_GPS
	cs7000p_gps_init();
#endif
}
