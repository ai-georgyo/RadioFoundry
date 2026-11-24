# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-1701 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9,
# 0x0800C000 to 0x080BFFFF; the codec runs in place in sectors 10 and 11, which the app never links into
# (boards/dm1701/memory.ld, docs/spec/codec-dm1701.md). docs/feature-matrix.md has the image's size
# 160x120 display. The fault trace, the large font and LTO follow the platform (platform/stm32f4/config.mk)
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
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
# its screens (docs/ux/m17.md) and its debug link sections (docs/spec/debuglink.md) the mode alone takes 46,204 B of
# flash and 1,104 B of RAM (measured 2026-11-23, each part on top of it)
# flash: M17 alone is 13,536 B over the app region, and 20,892 B over with its packets and heard list (DMR AES, on by
# default, takes 4,656 B of the room). The design's
# fallback: off until the owner names a trade; docs/feature-matrix.md, "M17 trades", has the measured
# candidates
CONFIG_M17 ?= n
# The M17 reflector client (NET_M17) stays off with M17. On top of M17 with its packets and heard list,
# SATELLITE_TLE=n and LANG_ES=n it takes 9,120 B of flash and 1,272 B of RAM: 27,720 B and 1,188 B stay free, under the
# 1.5 KB of RAM a default keeps (2026-11-23)
CONFIG_NET_M17 ?= n
# flash: P-256 signatures take about 19.1 KB (an STM32 radio's figure)
CONFIG_M17_SIG ?= n
# flash: AES and the random pool take about 2.6 KB (an STM32 radio's figure; the cipher itself is DMR AES's too)
CONFIG_M17_AES ?= n
# flash: the scrambler takes 1,352 B
CONFIG_M17_SCRAMBLER ?= n
# flash: the BER test takes 3,188 B
CONFIG_M17_BERT ?= n
# flash: the maximal build keeps satellite tracking and the rest. It turns every switch on, M17 among them, though M17
# is off by default here until the owner names a trade (above). So it leaves out the M17 parts this radio leaves out,
# and M17 itself through MAXIMAL_VARS: with the mode alone (its packets and heard list too) the maximal image is
# 51,448 B over the app region (2026-11-23). An STM32 radio with more room builds M17 with every part in its maximal
# configuration
MAXIMAL_SKIP += M17_SIG M17_AES M17_SCRAMBLER M17_BERT
MAXIMAL_VARS += CONFIG_M17=n
# flash: the web pages stored without gzip are about 19 KB larger. With M17 off (the default) that image fits, so ci
# builds it; beside M17 alone it is 33,000 B over the app region (2026-11-23): turning M17 on by default here also needs
# BOARD_REQUIRED += NET_HTTP_GZIP, measured again

# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x120 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += FONT_BIG	# the 9x18 font is drawn only on panels 220 px and wider; this one is 160x120
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-dm1701.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the DM-1701 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the GPS line
# GPS stays on: no module from the factory, but owners fit one on PA10 / PA9 (hardware-dm1701.md 14.2). Detected at run
# time in the background: a radio without one stops listening after 5 s of silence

# Chinese (CONFIG_LANG_ZH, docs/build-config.md): a line of it needs 16 px, and this class draws its words in the 6x12
# font, so core/ui/ui_screens.c moves lists, labels, the strip, the hints and the text pages up to the 8x16 font while a
# wide language is in use; Home then has no room for the zone line and a list shows 5 rows instead of 7 on the 160x120
# panel (docs/spec/ui.md 2.5). Off by default like on every radio. Its glyph table (about 27 KB, docs/feature-matrix.md)
# is far smaller than the old GB2312 one: the `chinese` CI extra builds it beside the default build
#
#	make BOARD=dm1701 CONFIG_LANG_ZH=y CONFIG_LANG_DEFAULT_ZH=y
#
# board.toml's "chinese" ci extra builds exactly that. The web flasher offers Chinese to a browser that asks for it
# (docs/user/02-installing.md); the build fits without dropping Spanish (docs/user/23-radios.md)
