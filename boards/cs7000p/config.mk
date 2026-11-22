# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# CS7000 M17 PLUS feature defaults (config/features.mk lists them all). 1 MB for code and read-only data: flash bank 1
# (boards/cs7000p/memory.ld). 160x128 display, insets 0 until they are measured on a radio. The fault trace, the stub
# entry and LTO follow the platform (platform/stm32h7/config.mk). What is on by default is in neither list below: its
# test state is in the bring-up records (docs/testing/cs7000p-bringup.md), never here.
# No line for the vibration alert: a motor driver and socket are on the board, and whether a motor is fitted is in no
# document (board.toml [hardware] unconfirmed). CONFIG_VIBRATE stays at its default, off
#
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += FONT_BIG	# drawn only on panels 220 px and wider
BOARD_UNSUPPORTED += LANG_ZH	# a Chinese line needs 16 px; this layout draws its words in a smaller font
BOARD_UNSUPPORTED += LANG_DEFAULT_ZH	# needs LANG_ZH
BOARD_UNSUPPORTED += AM_RX	# no AM receiver: an FM IF chain
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a SPI NOR
# What the hardware can do but this radio's driver or glue doesn't yet, the same way: off by default, and off in ci's
# maximal configuration too
BOARD_NOT_WRITTEN += FAULT_TRACE	# the accessory connector carries a UART (UART4) that could take it; no driver yet
BOARD_NOT_WRITTEN += RX_EXT	# no receive range in its chip documents (a PLL and an IF chain, no single receiver chip)
BOARD_NOT_WRITTEN += SFR	# the between-burst receive retune isn't written for this radio's key-up path
BOARD_NOT_WRITTEN += SFR_DUALPATH_RX	# needs SFR
BOARD_NOT_WRITTEN += SFR_CWID	# needs SFR
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# the split duplex outbound keying isn't written for this radio
BOARD_NOT_WRITTEN += P25	# no air interface (board.air) is wired up for this radio
BOARD_NOT_WRITTEN += P25_TRUNK	# needs P25
BOARD_NOT_WRITTEN += NXDN	# no air interface (board.air) is wired up for this radio
BOARD_NOT_WRITTEN += NXDN_SCRAMBLER	# needs NXDN
BOARD_NOT_WRITTEN += FM_TX_TONES	# the HR_C6000 TX tone path isn't written for this radio
BOARD_NOT_WRITTEN += DTMF	# needs the same path
BOARD_NOT_WRITTEN += APRS_AFSK	# needs the TX tone path and an AFSK source
BOARD_NOT_WRITTEN += NET_KISS	# needs APRS_AFSK
BOARD_NOT_WRITTEN += NET_APRSIS	# needs APRS_AFSK
BOARD_NOT_WRITTEN += APRS_RX	# the receive audio tap (an ADC input) isn't fed to the demodulator yet
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
BOARD_NOT_WRITTEN += VOX	# the level detector's amplifier and microphone switch are on only while transmitting
# APRS goes over DMR only here (no AFSK path yet): with DMR or APRS_DMR off there is no backend left, so APRS follows
# them. Read while this file is included: only a command-line or variant value is set by now. It must be := : a ?=
# that names CONFIG_APRS_DMR is recursive (that switch's default names CONFIG_APRS) and fails
ifeq ($(origin CONFIG_APRS),undefined)
CONFIG_APRS := $(if $(filter n,$(CONFIG_DMR) $(CONFIG_APRS_DMR)),n,y)
endif
# Without a codec route FM is this radio's only voice mode and the path its receiver is brought up on: the matrix has
# no FM-less configuration
BOARD_REQUIRED := FM
