# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# STM32H7: arm-none-eabi GCC, Cortex-M7 hard float, ST HAL + CMSIS, FreeRTOS CM4F port
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

# Single precision on purpose, though the core's FPU has doubles: it selects the thumb/v7e-m+fp/hard multilib, the one
# CI's toolchain keeps (tools/install-arm-toolchain.sh; the core's default FPU would ask for v7e-m+dp), and the tree
# has no double (gotchas 13.12)
CPUFLAGS := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
# Bytes a task's stack holds beyond its call chain (make stack-check): interrupts run on the main stack (MSP), but the
# exception entry stacks its frame on the task's stack, 26 words with the FPU's lazy space plus 1 for alignment, and the
# port's context switch adds r4-r11, lr and s16-s31 (25 words): 52 words. The same frame and the same port as the
# STM32F4's
STACK_OVERHEAD := 208

HALDIR := third_party/stm32h7xx-hal-driver
CMSIS_DEV := third_party/cmsis-device-h7
CMSIS_CORE := third_party/cmsis-core
RTOS := third_party/FreeRTOS-Kernel
TUSB := third_party/tinyusb/src

# The board's part (board.mk MCU): its CMSIS device define and its vector table. ST's start-up file is built for the
# table only: its Reset_Handler is weak and loses to this platform's (reset.S)
ifeq ($(MCU),stm32h743)
MCU_DEFS += -DSTM32H743xx
MCU_STARTUP := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32h743xx.s
else
$(error boards/$(BOARD)/board.mk: MCU must name a supported part (stm32h743), not "$(MCU)")
endif
$(if $(HSE_VALUE),,$(error boards/$(BOARD)/board.mk: HSE_VALUE, the crystal in Hz, is missing))
$(if $(APP_BASE),,$(error boards/$(BOARD)/board.mk: APP_BASE, the image address, is missing))
# The vector table is the image's first word: SystemInit sets VTOR = FLASH_BANK1_BASE | VECT_TAB_OFFSET
# (FLASH_BANK1_BASE is the CMSIS device header's; - binds tighter than |). No parentheses or spaces: the flags file is
# written by a shell echo. strip: a board.mk line that ends in a comment keeps the blanks before it, and one between
# the address and its suffix would split the define in two. No USE_PWR_* define, ever: ST's ExitRun0Mode() then
# compiles empty and nobody calls it, because the supply configuration is written once, with a bounded wait, by this
# platform's Reset_Handler
MCU_DEFS += -DHSE_VALUE=$(strip $(HSE_VALUE)) -DUSER_VECT_TAB_ADDRESS \
	-DVECT_TAB_OFFSET=$(strip $(APP_BASE))u-FLASH_BANK1_BASE
# common.ld's CPU-only RAM (the DTCM here: no DMA): zeroed at start-up for RFOUNDRY_CPU_RAM, left alone for _NOINIT.
# The section names are the shared linker script's
MCU_DEFS += -DRFOUNDRY_CPU_RAM_SECTION='".ccmbss"' -DRFOUNDRY_CPU_RAM_NOINIT_SECTION='".ccmram"' \
	-DRFOUNDRY_CPU_DATA_SECTION='".ccmdata"'
LDSCRIPT ?= boards/$(BOARD)/memory.ld

# The USB device stack, built only with CONFIG_USB
BOARD_USB_DIRS ?= drivers/usb
# The ST HAL modules every board builds, in link order. No hal_flash and no hal_flash_ex: nothing in an image for this
# SoC programs the internal flash or writes an option byte (the HAL conf header takes the flash module's header only,
# for the latency macros). No optional module yet, so BOARD_HAL_MODULES has nothing to name
HAL_ORDER := hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_pwr hal_pwr_ex hal_dma hal_dma_ex hal_adc hal_adc_ex
$(foreach m,$(BOARD_HAL_MODULES),$(error BOARD_HAL_MODULES: $(m) is not a module this platform builds on request (it has none)))
HAL_MODULES := $(HAL_ORDER)

# TinyUSB (MIT), device stack on the OTG_FS core (Synopsys DWC2); configured by drivers/usb/tusb_config.h
MCU_DEFS += -DCFG_TUSB_MCU=OPT_MCU_STM32H7
TUSB_SRC := $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
	$(TUSB)/class/cdc/cdc_device.c $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c $(TUSB)/portable/synopsys/dwc2/dwc2_common.c \
	$(if $(filter y,$(CONFIG_USB_NETWORK)),$(TUSB)/class/net/ncm_device.c)

# platform/stm32 is what every STM32 platform shares; this directory has the STM32H7's own files and its halves of the
# shared ones (stm32_family.h, sysboot_soc.h, hrc/i2s_pcm_soc.h, gps_uart/gps_uart_soc.h)
PLATFORM_DIRS := platform/stm32 platform/stm32h7
# One source set in two directories, built in file-name order across both, as on the other STM32 platform: the link
# order steers LTO's code (gotchas 13.10). A file name must not exist in both
PLATFORM_SRC := $(foreach f,$(sort $(notdir $(wildcard $(addsuffix /*.c,$(PLATFORM_DIRS))))),$(wildcard $(addsuffix /$(f),$(PLATFORM_DIRS))))
# The shared linker script (common.ld), after this directory on the linker's search path
PLATFORM_LD_DIRS := platform/stm32
# The file with file-scope assembly, kept out of link time optimization (the Makefile's LTO_EXCLUDE)
LTO_EXCLUDE := platform/stm32/codec_call.c
# The FreeRTOS port is the Cortex-M4F one: the same core registers and FPU frame, and it refuses only the Cortex-M7
# revisions r0p0 and r0p1 (port.c), which this part's core (r1p1) is not
PLATFORM_INC := -Iplatform/stm32 -I$(HALDIR)/Inc -I$(CMSIS_DEV)/Include -I$(CMSIS_CORE)/Include -I$(RTOS)/include \
	-I$(RTOS)/portable/GCC/ARM_CM4F -I$(TUSB) -Idrivers/usb

PLATFORM_3P_SRC := $(MCU_STARTUP) \
	$(CMSIS_DEV)/Source/Templates/system_stm32h7xx.c \
	$(foreach m,$(HAL_MODULES),$(HALDIR)/Src/stm32h7xx_$(m).c) \
	$(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_5.c \
	$(if $(filter y,$(CONFIG_USB)),$(TUSB_SRC))
