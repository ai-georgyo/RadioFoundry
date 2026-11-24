# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Connect Systems CS7000 M17 PLUS: STM32H743VI + HR_C6000 + SKY72310 synthesizer + AK2365A IF receiver, one band
# (400-527 MHz), the rails and switches on a 24-output 74HC595 chain, a 32 MB SPI NOR (docs/spec/hardware-cs7000p.md).
# Written from the maker's schematic: nothing here has run on a radio (docs/testing/cs7000p-bringup.md)
PLATFORM   := stm32h7
MCU        := stm32h743
# The 25 MHz crystal, X603 (hardware-cs7000p.md 2)
HSE_VALUE  := 25000000U
# The flash base: the image is installed through the ROM bootloader, and no loader is kept below it
APP_BASE   := 0x08000000
BOARD_DIRS := boards/cs7000p platform/stm32/hrc platform/stm32/spi_flash platform/stm32/gps_uart \
	platform/stm32/lcd8080_gpio platform/stm32/gpio_keypad platform/stm32/dmr_keyup platform/stm32h7/periph \
	drivers/io/hc595 drivers/bus/wire3_gpio drivers/rf/chainseq drivers/rf/sky72310 drivers/rf/ak2365a \
	drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc6000/if_host drivers/modem/hrc_common \
	drivers/display/st7735 drivers/display/st7735/id drivers/flash/spi_nor \
	drivers/input/keymatrix drivers/input/chswitch drivers/input/plugptt drivers/gps/nmea_line drivers/gps/gps_rx
# The 160x128 panel in 16 colors, as the other radios of this class: a 4 bpp frame buffer of 10240 B and 128 rows
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
# The data flash is a 32 MB part: 4-byte addresses (drivers/flash/spi_nor)
MCU_DEFS   += -DSPI_NOR_ADDR4=1
# It held another firmware's data and nothing says where: the settings region is foreign until the owner claims it
# (core/storage, docs/spec/settings-store.md 5)
MCU_DEFS   += -DSTORAGE_CLAIM_FIRST=1
# The maker's factory table and records lie inside the map (docs/spec/calibration-cs7000p.md 2): the map lists them
# (store_cs7000p.c .factory), and every rewrite of their sectors keeps their bytes (core/storage,
# docs/spec/settings-store.md 4.1)
MCU_DEFS   += -DSTORAGE_FACTORY_KEEP=1
# The modem's interrupt lines: timeslot PC0, system PC3, RF TX PC2 (platform/stm32/hrc/hrc_lines.h)
MCU_DEFS   += -DSTM32_HRC_LINE_SYS=3 -DSTM32_HRC_LINE_RFTX=2
# Its I2S data input is on the SPI3 pad that is SDI by default (hardware-cs7000p.md 6): the half file proves the
# direction before it drives anything (platform/stm32h7/hrc/i2s_pcm_soc.h)
MCU_DEFS   += -DSTM32_I2S_TX_ON_SDI=1
# Bench knobs (docs/testing/cs7000p-bringup.md has a row for each), e.g. CS7000P_DEFS="-DCS7000P_LCD_FORCE=1"
CS7000P_DEFS ?=
MCU_DEFS   += $(CS7000P_DEFS)
