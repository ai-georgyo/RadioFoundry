# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# STM32H7 feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The fault trace needs a host UART the fault handler can drive polled: the platform has no host UART driver yet
CONFIG_FAULT_TRACE ?= n
# What no radio on the platform has yet, one switch per line with the reason (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): ci's maximal configuration leaves these off
CONFIG_STUB_ENTRY ?= n
BOARD_NOT_WRITTEN += STUB_ENTRY	# no RAM flash stub for this SoC yet: updates go through the ROM bootloader
# Link time optimization on our sources, as on the other STM32 platform: the shared code then runs as it was verified
# on that platform's radios. LTO=0 builds the plain way
LTO ?= 1
