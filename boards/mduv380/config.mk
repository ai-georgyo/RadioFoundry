# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-UV380 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9;
# the codec sectors 10 and 11 are never linked into (boards/mduv380/memory.ld, docs/spec/codec-placement.md).
# 160x128 display, every row visible. The fault trace, the large font and LTO follow the platform
# (platform/stm32f4/config.mk)
# Safe area: the TYT housing's edge crowds the top row and the two left columns; unconfirmed on this radio
# (hardware-mduv380.md 4)
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= 1
CONFIG_DISPLAY_INSET_LEFT ?= 2
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above); French, off by
# default, so that the maximal build keeps satellite tracking and still fits the app region; Chinese stays out too
# (its glyph table does not fit beside the rest)
MAXIMAL_SKIP := NET_PPP APRS_IGATE LANG_FR LANG_ZH LANG_DEFAULT_ZH
# M17 (docs/spec/m17.md, the memory section): off by default here, and its parts with it, in the trade orders. With
# its screens (docs/ux/m17.md) and its debug link sections (docs/spec/debuglink.md) the mode alone takes 46,232 B of
# flash and 1,104 B of RAM (measured 2026-11-23, each part on top of it)
# flash: M17 alone is 14,524 B over the app region, 16,872 B over with the heard list (DMR AES, on by default, takes
# 4,624 B of the room). The design's fallback: off until
# the owner names a trade; docs/feature-matrix.md, "M17 trades", has the measured candidates
CONFIG_M17 ?= n
# The M17 reflector client (NET_M17) stays off with M17: about 9.1 KB of flash and 1.3 KB of RAM on top of it (an
# STM32 radio's figure, 2026-11-23)
CONFIG_NET_M17 ?= n
# flash: P-256 signatures take about 19.1 KB (an STM32 radio's figure)
CONFIG_M17_SIG ?= n
# flash: AES and the random pool take about 2.6 KB (an STM32 radio's figure; the cipher itself is DMR AES's too)
CONFIG_M17_AES ?= n
# flash: the scrambler takes 1,336 B
CONFIG_M17_SCRAMBLER ?= n
# flash: the BER test takes 3,168 B
CONFIG_M17_BERT ?= n
# RAM: the SMS inbox and its send take 856 B (and 4,980 B of flash): with them and the mode 972 B of RAM is left,
# under 1.5 KB
CONFIG_M17_PACKET ?= n
# flash: the maximal build keeps satellite tracking and the rest. It turns every switch on, M17 among them, though M17
# is off by default here until the owner names a trade (above). So it leaves out the M17 parts this radio leaves out,
# and M17 itself through MAXIMAL_VARS: with the mode alone (its packets and heard list too) the maximal image is
# 47,468 B over the app region (2026-11-23). An STM32 radio with more room builds M17 with every part in its maximal
# configuration
MAXIMAL_SKIP += M17_SIG M17_AES M17_SCRAMBLER M17_BERT
MAXIMAL_VARS += CONFIG_M17=n
# flash: the web pages stored without gzip are about 19 KB larger. With M17 off (the default) that image fits, so ci
# builds it; beside M17 alone it is 33,132 B over the app region (2026-11-23): turning M17 on by default here also needs
# BOARD_REQUIRED += NET_HTTP_GZIP, measured again

# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += FONT_BIG	# the 9x18 font is drawn only on panels 220 px and wider; this one is 160x128
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-mduv380.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the MD-UV380 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the G models' GPS line

# Chinese (CONFIG_LANG_ZH): a 160x128 panel is the 160 px class, whose layout moves its words up to the 8x16 font while
# a wide language is in use (core/ui/ui_screens.c, docs/spec/ui.md 2.5), so a 16 px CJK glyph fits; Home then has no
# zone line and a list shows 5 rows instead of 7. Off by default like on every radio. Its glyph table (about 27 KB,
# docs/feature-matrix.md) is far smaller than the old GB2312 one: the `chinese` CI extra builds it beside the default build
