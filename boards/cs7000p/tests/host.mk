# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host).

CS7000P   := $(ROOT)/boards/cs7000p
CS7000P_T := $(CS7000P)/tests
# Board code reaches platform.h and FreeRTOS.h through stub/. The platform's own host-clean headers (soc.h,
# periph/adc_scan.h, which cs7000p.h includes, and periph/dac.h on the stub's registers) are the real ones
CS7000P_INC := -I$(CS7000P_T)/stub -I$(CS7000P) -I$(ROOT)/hal -I$(ROOT)/platform/stm32h7
CS7000P_H   := $(wildcard $(CS7000P)/*.h $(CS7000P_T)/stub/*.h) $(ROOT)/platform/stm32h7/soc.h \
	$(wildcard $(ROOT)/platform/stm32h7/periph/*.h)

# The output expander and the shared DAC's interlock: the header's words and rules, and the one writer
# (exp_cs7000p.c, with the real pins.h) on a model of the three chips behind PE7-PE9 and a fake DAC, the interrupt
# mask its own (an "interrupt" can come just before it)
CS7000P_EXP := $(CS7000P_T)/cs7000pExpTest.c $(CS7000P)/exp_cs7000p.c $(ROOT)/drivers/io/hc595/hc595.c
TESTS   += cs7000pExpTest cs7000pExpTestFloor
cs7000pExpTest: $(CS7000P_EXP) $(CS7000P_H) $(ROOT)/drivers/io/hc595/hc595.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -DCS7000P_STUB_MASK_HOOK -o $@ $(CS7000P_EXP)
# Again as a radio with a measured PA floor builds it: values at and under the floor
cs7000pExpTestFloor: $(CS7000P_EXP) $(CS7000P_H) $(ROOT)/drivers/io/hc595/hc595.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -DCS7000P_STUB_MASK_HOOK -DCS7000P_APC_FLOOR=300 -o $@ $(CS7000P_EXP)
run-boards:: cs7000pExpTest cs7000pExpTestFloor
	./cs7000pExpTest
	./cs7000pExpTestFloor

# Every pin's idle state (pinsetup_cs7000p.c with the real pins.h) against the lines that must never be driven
CS7000P_PINS := $(CS7000P_T)/cs7000pPinsTest.c $(CS7000P)/pinsetup_cs7000p.c
TESTS   += cs7000pPinsTest
cs7000pPinsTest: $(CS7000P_PINS) $(CS7000P_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_PINS)
run-boards:: cs7000pPinsTest
	./cs7000pPinsTest

# The battery model, the rule for holding the supply and the knob watch after a power-off (battery_cs7000p.h)
CS7000P_BATTERY := $(CS7000P_T)/cs7000pBatteryTest.c $(CORE)/radio/power_switch.c
TESTS   += cs7000pBatteryTest
cs7000pBatteryTest: $(CS7000P_BATTERY) $(CS7000P_H) $(CORE)/radio/power_switch.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_BATTERY)
run-boards:: cs7000pBatteryTest
	./cs7000pBatteryTest

# The power file itself (power_cs7000p.c, included by the test) on a fake scan, expander, timer and clock: the hold
# only on a knob read on, the order of a power-off, the scan that stops, a sagging pack, the off state's LEDs (the
# interrupt mask the test's own: a power-off can come just before it)
CS7000P_POWER := $(CS7000P_T)/cs7000pPowerTest.c $(CORE)/radio/power_switch.c
TESTS   += cs7000pPowerTest
cs7000pPowerTest: $(CS7000P_POWER) $(CS7000P)/power_cs7000p.c $(CS7000P_H) $(CORE)/radio/power_switch.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -I$(ROOT)/platform/stm32 -Wshadow -DCS7000P_STUB_MASK_HOOK -o $@ $(CS7000P_POWER)
run-boards:: cs7000pPowerTest
	./cs7000pPowerTest

# The data flash map under core/storage and core/settings as the board builds them (4-byte addresses, claimed before
# the first write, the maker's bytes kept), on spiNorTest.c's model of the 32 MB part; with the codeplug and the
# open-firmware CPS shim for the writers of the sector that holds the maker's table
CS7000P_STORE := $(CS7000P_T)/cs7000pStoreTest.c $(ROOT)/drivers/flash/spi_nor/spi_nor.c $(CORE)/storage/storage.c \
	$(CORE)/settings/settings.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/codeplug_kv.c \
	$(CORE)/codeplug/codeplug_tag.c $(CORE)/cps/cps_openfw.c $(CS7000P)/store_cs7000p.c
TESTS   += cs7000pStoreTest
cs7000pStoreTest: $(CS7000P_STORE) spiNorTest.c $(ROOT)/drivers/flash/spi_nor/spi_nor.h $(CS7000P)/store_cs7000p.h \
                  $(ROOT)/hal/hal_flash.h $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h $(CORE)/codeplug/*.h \
                  $(CORE)/cps/*.h)
	$(CC) $(CFLAGS) -DSPI_NOR_ADDR4=1 -DSTORAGE_CLAIM_FIRST=1 -DSTORAGE_FACTORY_KEEP=1 -I$(ROOT)/hal -I. -I$(CS7000P) \
		-Wshadow -o $@ $(CS7000P_STORE)
run-boards:: cs7000pStoreTest
	./cs7000pStoreTest

# The key matrix as drawn, the provisional order of the function switches and the selector's table
# (keys_cs7000p.h)
CS7000P_KEYS := $(CS7000P_T)/cs7000pKeysTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c \
	$(ROOT)/drivers/input/chswitch/chswitch.c
TESTS   += cs7000pKeysTest
cs7000pKeysTest: $(CS7000P_KEYS) $(CS7000P_H) $(ROOT)/drivers/input/keymatrix/keymatrix.h \
                 $(ROOT)/drivers/input/chswitch/chswitch.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_KEYS)
run-boards:: cs7000pKeysTest
	./cs7000pKeysTest

# The LCD board check's rule and the panel choice from the controller's ID (panel_cs7000p.h), the display driver's
# init on a recording bus, and the board's own display_cs7000p.c (included by the test) and input_cs7000p.c on a
# fake bus, key scan, timer and pins: nothing is driven before the board check has passed, and keys held at power-on
# reach the core's gesture engine as held at start-up
CS7000P_PANEL := $(CS7000P_T)/cs7000pPanelTest.c $(CS7000P)/input_cs7000p.c \
	$(ROOT)/drivers/display/st7735/st7735.c $(ROOT)/drivers/display/st7735/id/st7735_id.c \
	$(ROOT)/drivers/input/keymatrix/keymatrix.c $(ROOT)/drivers/input/chswitch/chswitch.c \
	$(ROOT)/drivers/input/plugptt/plug_ptt.c $(CORE)/input/input_engine.c
TESTS   += cs7000pPanelTest
cs7000pPanelTest: $(CS7000P_PANEL) $(CS7000P)/display_cs7000p.c $(CS7000P_H) \
                  $(ROOT)/drivers/display/st7735/st7735.h $(ROOT)/drivers/display/st7735/id/st7735_id.h \
                  $(ROOT)/platform/stm32/lcd8080_gpio/lcd8080_gpio.h $(ROOT)/platform/stm32/gpio_keypad/gpio_keypad.h \
                  $(wildcard $(ROOT)/drivers/input/*/*.h) $(CORE)/input/input_engine.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -I$(ROOT)/platform/stm32 -Wshadow -o $@ $(CS7000P_PANEL)
run-boards:: cs7000pPanelTest
	./cs7000pPanelTest

# The frequency plan (rfplan_cs7000p.c) on the synthesizer and IF receiver drivers
CS7000P_RF_DRV := $(ROOT)/drivers/rf/sky72310/sky72310.c $(ROOT)/drivers/rf/ak2365a/ak2365a.c
CS7000P_RF_H   := $(ROOT)/drivers/rf/sky72310/sky72310.h $(ROOT)/drivers/rf/ak2365a/ak2365a.h \
	$(ROOT)/drivers/bus/wire3.h
CS7000P_RF := $(CS7000P_T)/cs7000pRfTest.c $(CS7000P)/rfplan_cs7000p.c $(CS7000P_RF_DRV)
TESTS   += cs7000pRfTest
cs7000pRfTest: $(CS7000P_RF) $(CS7000P_H) $(CS7000P_RF_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_RF)
run-boards:: cs7000pRfTest
	./cs7000pRfTest

# The calibration tables as shipped (unset: no transmission) and on a synthetic table (rfcal_cs7000p.c)
CS7000P_CAL := $(CS7000P_T)/cs7000pCalTest.c $(CS7000P)/rfcal_cs7000p.c
TESTS   += cs7000pCalTest
cs7000pCalTest: $(CS7000P_CAL) $(CS7000P_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_CAL)
run-boards:: cs7000pCalTest
	./cs7000pCalTest

# The RF chain's sequences on this board's lines (rfops_cs7000p.c under drivers/rf/chainseq) with a fake expander
# and DAC
CS7000P_SEQ := $(CS7000P_T)/cs7000pRadioSeqTest.c $(CS7000P)/rfops_cs7000p.c $(CS7000P)/rfcal_cs7000p.c \
	$(CS7000P)/rfplan_cs7000p.c $(ROOT)/drivers/rf/chainseq/chainseq.c $(CS7000P_RF_DRV)
TESTS   += cs7000pRadioSeqTest
cs7000pRadioSeqTest: $(CS7000P_SEQ) $(CS7000P_H) $(CS7000P_RF_H) $(ROOT)/drivers/rf/chainseq/chainseq.h \
                     $(CORE)/radio/apc_ramp.h $(CORE)/radio/pa_keyup.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_SEQ)
run-boards:: cs7000pRadioSeqTest
	./cs7000pRadioSeqTest

# The speaker routing's truth table (speaker_cs7000p.h)
TESTS   += cs7000pSpeakerTest
cs7000pSpeakerTest: $(CS7000P_T)/cs7000pSpeakerTest.c $(CS7000P_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_T)/cs7000pSpeakerTest.c
run-boards:: cs7000pSpeakerTest
	./cs7000pSpeakerTest

# The sub-audio PWM's duty rule (subpwm_cs7000p.h)
TESTS   += cs7000pSubPwmTest
cs7000pSubPwmTest: $(CS7000P_T)/cs7000pSubPwmTest.c $(CS7000P_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_T)/cs7000pSubPwmTest.c
run-boards:: cs7000pSubPwmTest
	./cs7000pSubPwmTest

# The hal_radio_t (radio_cs7000p.c) over the real chain and its buses, with a fake expander, modem, burst runner and
# power state: the microphone for every transmission and off at every end, the power's off state (the interrupt
# mask the test's own: a power-off can come just before it). With the maker's factory table on core/storage and the
# factory copy as the board builds them (its map, kept bytes, claimed first) over the simulated NOR flash; again
# without the copy (CONFIG_FACTORY_COPY=0: the window alone)
CS7000P_RADIO := $(CS7000P_T)/cs7000pRadioTest.c $(CS7000P)/radio_cs7000p.c $(CS7000P)/rfops_cs7000p.c \
	$(CS7000P)/rfcal_cs7000p.c $(CS7000P)/rfplan_cs7000p.c $(ROOT)/drivers/rf/chainseq/chainseq.c \
	$(CS7000P_RF_DRV) $(ROOT)/drivers/bus/wire3_gpio/wire3_gpio.c $(CORE)/radio/tx_limit.c $(CORE)/radio/pa_keyup.c \
	$(CS7000P)/makercal_cs7000p.c $(CS7000P)/store_cs7000p.c $(CORE)/storage/storage.c \
	$(CORE)/storage/factory_copy.c $(CORE)/support/sha256.c $(CORE)/debuglink/dl_fmt.c sim/sim_flash.c
CS7000P_RADIO_H := $(CS7000P_H) $(CS7000P_RF_H) $(ROOT)/drivers/rf/chainseq/chainseq.h \
	$(ROOT)/drivers/bus/wire3_gpio/wire3_gpio.h $(ROOT)/platform/stm32/dmr_keyup/dmr_keyup.h \
	$(wildcard $(CORE)/storage/*.h) sim/sim_flash.h $(ROOT)/hal/hal_flash.h
CS7000P_RADIO_F := $(CS7000P_INC) -I$(ROOT)/platform/stm32 -I. -Wshadow -DCS7000P_STUB_MASK_HOOK \
	-DSTORAGE_CLAIM_FIRST=1 -DSTORAGE_FACTORY_KEEP=1
TESTS   += cs7000pRadioTest cs7000pRadioNoCopyTest
cs7000pRadioTest: $(CS7000P_RADIO) $(CS7000P_RADIO_H)
	$(CC) $(CFLAGS) $(CS7000P_RADIO_F) -o $@ $(CS7000P_RADIO)
cs7000pRadioNoCopyTest: $(CS7000P_RADIO) $(CS7000P_RADIO_H)
	$(CC) $(CFLAGS) $(CS7000P_RADIO_F) -DCONFIG_FACTORY_COPY=0 -o $@ $(CS7000P_RADIO)
run-boards:: cs7000pRadioTest cs7000pRadioNoCopyTest
	./cs7000pRadioTest
	./cs7000pRadioNoCopyTest

# The reader of the maker's factory calibration table (makercal_cs7000p.c) on invented tables: the maker's
# arithmetic, the integrity and use rules, the VCO switch windows, the settings, the trims and the RSSI line
CS7000P_MAKERCAL := $(CS7000P_T)/cs7000pMakerCalTest.c $(CS7000P)/makercal_cs7000p.c $(CS7000P)/rfplan_cs7000p.c \
	$(CS7000P_RF_DRV)
TESTS   += cs7000pMakerCalTest
cs7000pMakerCalTest: $(CS7000P_MAKERCAL) $(CS7000P_H) $(CS7000P_RF_H)
	$(CC) $(CFLAGS) $(CS7000P_INC) -Wshadow -o $@ $(CS7000P_MAKERCAL)
run-boards:: cs7000pMakerCalTest
	./cs7000pMakerCalTest

# The codec route (vocoder_cs7000p.c, included by the test as the firmware builds it) against sector 7 as
# tools/codec_extract.py merges a synthetic codec for this board: the descriptor and both windows pass the boot
# check's steps with the board's own profile, a changed byte anywhere in a window fails them, and a descriptor that
# names flash outside the sector is refused. The file may also be a whole merged image (its last 128 KB, as merged
# here too). Not run here: with "pinned" as its second argument the test takes an image merged from the maker's file,
# outside the repository, and also asks for the two CRC-32s the profile pins
TESTS   += cs7000pCodecTest
cs7000pCodecTest: $(CS7000P_T)/cs7000pCodecTest.c $(CS7000P)/vocoder_cs7000p.c $(ROOT)/platform/stm32/codec_desc.h \
                  $(ROOT)/platform/stm32/hrc/vocoder.h $(ROOT)/hal/hal_vocoder.h $(CORE)/config.h
	$(CC) $(CFLAGS) $(CS7000P_INC) -I$(ROOT)/platform/stm32 -DCONFIG_DMR=1 -Wshadow -o $@ $(CS7000P_T)/cs7000pCodecTest.c
run-boards:: cs7000pCodecTest
	python3 -I $(ROOT)/tools/codec_extract.py --test-desc codec_desc_cs7000p.bin --board cs7000p
	./cs7000pCodecTest codec_desc_cs7000p.bin
	python3 -I -c "import sys; d = open(sys.argv[1], 'rb').read(); open(sys.argv[2], 'wb').write(b'\xff' * (0x100000 - len(d)) + d)" \
		codec_desc_cs7000p.bin codec_merged_cs7000p.bin
	./cs7000pCodecTest codec_merged_cs7000p.bin
