// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Connect Systems CS7000 M17 PLUS pin map: the STM32H743VI in LQFP100 (docs/spec/hardware-cs7000p.md 2). Every line
// is from the maker's schematic and none has been checked on a radio (docs/testing/cs7000p-bringup.md). A comment
// starts with the package pin and the schematic's net name.
//
// The LCD board (display bus, key matrix, backlight driver) is not in that schematic: the OEM's 2018 LCD board is the
// working assumption for every PIN_LCD_* and PIN_KEY_K9 line. The bus, select, strobe, reset and matrix lines
// (PIN_LCD_DC, _WR, _RD, _RES, _CS, PD0-PD7, PIN_KEY_K9) stay inputs with pull-ups until the board check has passed
// (display_cs7000p.c). PIN_LCD_BACKLIGHT is driven low (off) from the pin set-up on, and so is the key lights' LAMP
// (EXP_LAMP) from the expander's reset word.
//
// Never driven push-pull, because a chip's output or a contact to ground is on the net: PC0, PC2, PC3, PB14, PB4, PC12,
// PA2, PC1, PA10, PC7, PA1, PE14, PE15, PB10, PB11, PE10, PE11, PA8, PD15, PE12, PE13, PC15, PB8, PB9, PE5, PB0, PB1,
// PD11; PA15 and PC10 until the HR_C6000 is known to be the master of its I2S port. tests/cs7000pPinsTest.c holds the
// same list.
//
// No define for PA13 (pin 72: SWDIO, and PHONE_DET behind 10k) and PA14 (pin 76: SWCLK, and BLUE-DET, the Bluetooth
// module's supply behind 10k): they are never reconfigured, and both nets are read through GPIOA->IDR.
//
// The supply hold (POW_C) is not a pin: it is bit 20 of the output expander, EXP_POWER_HOLD in exp_cs7000p.h. So are
// the rails, the AK2365A's select and power-down, the HR_C6000's sleep line, the LEDs and the audio switches.
#ifndef CS7000P_PINS_H
#define CS7000P_PINS_H

#include "platform.h"

// Data flash, a W25Q256JV on SPI4 (AF5). The select is a GPIO and has no pull-up on the board: it is driven high in
// the reset hook. SCK and MOSI run on the medium slope (12 MHz is the low slope's limit)
#define PIN_FLASH_SCK		GPIOE, GPIO_PIN_2	// 1, FLASH_SCK
#define PIN_FLASH_CS		GPIOE, GPIO_PIN_4	// 3, FLASH_CS
#define PIN_FLASH_MISO		GPIOE, GPIO_PIN_5	// 4, FLASH_SDO: the chip's output
#define PIN_FLASH_MOSI		GPIOE, GPIO_PIN_6	// 5, FLASH_SDI

// AK2365A IF receiver: a bit-banged three-wire bus at 250 kHz, 1k in series with each line. Its select (A-CSN) and
// power-down (A-PDN) are expander bits. PC13 and PC14 are backup-domain pads: RM0433 6.4.4 allows one of PC13-PC15
// as an output at a time, at 2 MHz into 30 pF, and the maker wired two. So PC14 is static while PC13 moves, and PC13
// is an output only inside a frame (an input with a pull-down otherwise). A known deviation (hardware-cs7000p.md)
#define PIN_AK_SCLK		GPIOE, GPIO_PIN_3	// 2, A-SCLK
#define PIN_AK_SDATA		GPIOC, GPIO_PIN_13	// 7, A-SDATA: both ways
#define PIN_AK_RSTN		GPIOC, GPIO_PIN_14	// 8, A-RSTN

// Accelerometer (not used): its data line; its clock is an expander bit. Pulled up on the board: an input
#define PIN_ACC_SDA		GPIOC, GPIO_PIN_15	// 9, MD-SDA

// HR_C6000 interrupt lines, the chip's outputs. PC2 and PC3 are the package's PC2_C / PC3_C pads: the analog
// switches behind them (SYSCFG_PMCR PC2SO, PC3SO) stay closed, 0
#define PIN_DMR_INT_TS		GPIOC, GPIO_PIN_0	// 15, TIME_SLOT_INTER: EXTI0
#define PIN_DMR_INT_RFTX	GPIOC, GPIO_PIN_2	// 17, RF_TX_INTER: EXTI2
#define PIN_DMR_INT_SYS		GPIOC, GPIO_PIN_3	// 18, SYS_INTER: EXTI3

// Analog inputs (ADC1; ADC_CH_* is the pin's input number, INPn). PA2 is an op-amp's output: never an output here
#define PIN_TONE_IN		GPIOC, GPIO_PIN_1	// 16, 2T/5T: INP11, not scanned yet
#define PIN_SUBAUDIO_IN		GPIOA, GPIO_PIN_2	// 24, QT_DQT_IN: INP14, the sub-audio filter's output
#define PIN_RX_AUDIO		GPIOA, GPIO_PIN_3	// 25, RX-A: INP15, not scanned yet
#define PIN_POW_DET		GPIOA, GPIO_PIN_6	// 30, POW_DET: INP3, the battery behind the knob's switch
#define PIN_MIC_AUDIO		GPIOA, GPIO_PIN_7	// 31, TX-A: INP7, not scanned yet
#define PIN_VOX			GPIOC, GPIO_PIN_4	// 32, VOX: INP4, the microphone level detector
#define PIN_VOL_DET		GPIOC, GPIO_PIN_5	// 33, VOL-DET: INP8, the volume pot's wiper
#define PIN_RSSI		GPIOB, GPIO_PIN_0	// 34, RSSI: INP9, from the AK2365A
#define PIN_NOISE		GPIOB, GPIO_PIN_1	// 35, BUSY: INP5, the AK2365A's noise level
#define ADC_CH_TONE_IN		11
#define ADC_CH_SUBAUDIO		14
#define ADC_CH_RX_AUDIO		15
#define ADC_CH_POW_DET		3
#define ADC_CH_MIC_AUDIO	7
#define ADC_CH_VOX		4
#define ADC_CH_VOL_DET		8
#define ADC_CH_RSSI		9
#define ADC_CH_NOISE		5
// Analog mode, not read: the pad has no ADC input, and what the battery pack puts on the line is open
#define PIN_BAT_DET		GPIOB, GPIO_PIN_2	// 36, BAT_DET

// The DAC. Channel 1 is shared: the PA's power reference while transmitting, the front end's tuning voltage while
// receiving (exp_cs7000p.h has the interlock). 39k to ground keeps the PA off whenever the pin is not driven
#define PIN_APC_TV		GPIOA, GPIO_PIN_4	// 28, APC/TV: DAC1 channel 1
#define PIN_BEEP		GPIOA, GPIO_PIN_5	// 29, BEEP: DAC1 channel 2, off (not used: analog mode)

// The accessory connector's UART (UART4, not used) and the Bluetooth module's (unpowered, not used): analog mode,
// the reset state. Nothing defines the level of the accessory lines; PA1 and PA10 are the far ends' outputs
#define PIN_ACC_UART_TX		GPIOA, GPIO_PIN_0	// 22, PHONE-TXD
#define PIN_ACC_UART_RX		GPIOA, GPIO_PIN_1	// 23, PHONE-RXD
#define PIN_BT_TX		GPIOA, GPIO_PIN_9	// 68, BLUE-RXD
#define PIN_BT_RX		GPIOA, GPIO_PIN_10	// 69, BLUE-TXD

// The output expander, three 74HC595 in a chain (exp_cs7000p.h). Only ever set up by stm32_pin_force() in the reset
// hook: the chips keep their outputs through an MCU reset and power up with random ones
#define PIN_EXP_CLK		GPIOE, GPIO_PIN_7	// 37, CLK: SRCLK of all three
#define PIN_EXP_LATCH		GPIOE, GPIO_PIN_8	// 38, CS: RCLK of all three
#define PIN_EXP_DATA		GPIOE, GPIO_PIN_9	// 39, DS: SER of the first chip

// Keys wired to the MCU, each with a pull-up on the board: low = pressed
#define PIN_KEY_TOP		GPIOE, GPIO_PIN_10	// 40, ALARM_KEY: the top key
#define PIN_EXT_PTT		GPIOE, GPIO_PIN_11	// 41, EXT_PTT: the accessory connector's PTT
#define PIN_KEY_SK2		GPIOE, GPIO_PIN_12	// 42, PF2
#define PIN_KEY_SK3		GPIOE, GPIO_PIN_13	// 43, PF3
#define PIN_KEY_SK1		GPIOD, GPIO_PIN_15	// 62, PF1: the same key pulls BOOT0 high
#define PIN_PTT			GPIOA, GPIO_PIN_8	// 67, PTT_KEY

// The channel selector's four code lines, pulled up on the board: a closed contact reads 0. Never outputs: the
// switch connects them to ground
#define PIN_CHSW_0		GPIOE, GPIO_PIN_14	// 44, ECN0
#define PIN_CHSW_1		GPIOE, GPIO_PIN_15	// 45, ECN1
#define PIN_CHSW_2		GPIOB, GPIO_PIN_10	// 46, ECN2
#define PIN_CHSW_3		GPIOB, GPIO_PIN_11	// 47, ECN3

// HR_C6000 configuration port, bit-banged on port B (stm32_hrc_bus_cfg_t)
#define PIN_DMR_CS		GPIOB, GPIO_PIN_12	// 51, DMR_CS: no pull-up fitted
#define PIN_DMR_CLK		GPIOB, GPIO_PIN_13	// 52, DMR_SCLK
#define PIN_DMR_MISO		GPIOB, GPIO_PIN_14	// 53, DMR_SDO: the chip's output
#define PIN_DMR_MOSI		GPIOB, GPIO_PIN_15	// 54, DMR_SDI
// Its voice port on SPI1 (AF5), the select a GPIO
#define PIN_DMR_VCLK		GPIOB, GPIO_PIN_3	// 89, V_SCLK
#define PIN_DMR_VMISO		GPIOB, GPIO_PIN_4	// 90, V_SDO: the chip's output
#define PIN_DMR_VMOSI		GPIOB, GPIO_PIN_5	// 91, V_SDI
#define PIN_DMR_VCS		GPIOB, GPIO_PIN_6	// 92, V_CS
// Its I2S port on SPI3 (AF6), the MCU a slave. The two data pads are GPIO inputs until the direction probe of
// platform/stm32h7/hrc/i2s_pcm_soc.h has a verdict; nothing else configures them
#define PIN_I2S_WS		GPIOA, GPIO_PIN_15	// 77, I2S_FS
#define PIN_I2S_CK		GPIOC, GPIO_PIN_10	// 78, I2S_CK
#define PIN_I2S_TO_CHIP		GPIOC, GPIO_PIN_11	// 79, I2S_RX: the chip's data input
#define PIN_I2S_FROM_CHIP	GPIOC, GPIO_PIN_12	// 80, I2S_TX: the chip's data output

// SKY72310 synthesizer: a bit-banged three-wire bus, 100R in series with each line
#define PIN_PLL_DAT		GPIOD, GPIO_PIN_8	// 55, PLL_DAT
#define PIN_PLL_CS		GPIOD, GPIO_PIN_9	// 56, PLL_CS
#define PIN_PLL_CLK		GPIOD, GPIO_PIN_10	// 57, PLL_CLK
#define PIN_PLL_LD		GPIOD, GPIO_PIN_11	// 58, PLL_LD: input, high = locked

// The LCD board's connector (assumed board, above): an 8080 bus on GPIO whose data lines are also the key matrix.
// The main board's net names are from an older serial LCD and do not say what a line does on this one
#define PIN_LCD_DC		GPIOD, GPIO_PIN_12	// 59, LCD_RES: data / command
#define PIN_LCD_WR		GPIOD, GPIO_PIN_13	// 60, LCD_A0: write strobe
#define PIN_LCD_RD		GPIOD, GPIO_PIN_14	// 61, LCD_CS1: read strobe
#define PIN_LCD_RES		GPIOE, GPIO_PIN_0	// 97, LCD_DB7: reset
#define PIN_LCD_CS		GPIOE, GPIO_PIN_1	// 98, LCD_DB6: chip select
// D0..D7 are PD0..PD7 in order (pins 81-88, K1..K8): the display's data and the key matrix (K1-K4 sensed, K5-K8 driven)
#define PIN_LCD_D0		GPIOD, GPIO_PIN_0	// 81, K1
#define PIN_KEY_K9		GPIOB, GPIO_PIN_7	// 93, K9: the matrix's fifth driven line
#define PIN_LCD_BACKLIGHT	GPIOC, GPIO_PIN_9	// 66, LCD-LED: TIM8 CH4 (AF3)

// Sub-audio out: TIM3 CH3 (AF2). Its mean level is part of the reference oscillator's control voltage and the
// line has no pull resistor: once set up it always carries the PWM
#define PIN_SUBAUDIO_PWM	GPIOC, GPIO_PIN_8	// 65, CTC/DCS_OUT

// GPS module. Only its output is used (USART6 RX, AF7); PC6 stays an input, the module's RXD has a pull-up
#define PIN_GPS_TX		GPIOC, GPIO_PIN_6	// 63, GPS-RXD: the module's input
#define PIN_GPS_RX		GPIOC, GPIO_PIN_7	// 64, GPS-TXD: the module's output

// USB (OTG_FS, AF10) on the accessory connector
#define PIN_USB_DM		GPIOA, GPIO_PIN_11	// 70, USB_D-
#define PIN_USB_DP		GPIOA, GPIO_PIN_12	// 71, USB_D+

// An I2C bus to IC611, whose function is unknown (not used): inputs, pulled up on the board
#define PIN_I2C_SCL		GPIOB, GPIO_PIN_8	// 95, SCL
#define PIN_I2C_SDA		GPIOB, GPIO_PIN_9	// 96, SDA

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_PLL_CS, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))

#endif
