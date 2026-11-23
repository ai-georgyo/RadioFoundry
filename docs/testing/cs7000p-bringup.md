<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# CS7000 M17 PLUS bring-up checklist

The CS7000 M17 PLUS port (`make BOARD=cs7000p`) builds and passes its host tests. **One radio has run it** (2026-11-22):
it starts, and what was read on it is in [First radio](#first-radio) below. Everything else is from the maker's
schematic and the chip makers' documents ([hardware-cs7000p.md](../spec/hardware-cs7000p.md)). This page is the
order to try it in, every assumption the port makes with the check that settles it, and the knob to change when a
check fails. A row is untested unless First radio lists it: each says what a radio should show, not what one did.
The question numbers are those of the hardware page's section 16.

The stages are in order, and nothing that cannot be undone comes before the checks that gate it: the codeplug saved
and the build (0), the loader identified with a USB capture (1), the maker's recovery proven on this unit (2; it
erases and reloads the maker's programs itself), and only then the one-way Read Unprotect install (3). Nothing
transmits before stage 11, and never without the dummy load.

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio cs7000p`. `tools/bringup.py run --radio cs7000p` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). `boards/cs7000p/hil/test_cs7000p.py` checks the
view keys below on a radio and never transmits.

## Status

| Item | State |
|---|---|
| Build, host tests, stack check | pass |
| On a radio | one unit: starts; display, keys, the channel selector, beeps, USB, the debug link and settings storage work; FM receive with clear audio; DMR calls are received and decoded, without audio ([First radio](#first-radio)). Every transmission: not checked |
| Transmit | refused until the PA and the modulation are calibrated (stage 11) |
| DMR voice | off: no codec route is known for this radio |
| VOX | not written (`BOARD_NOT_WRITTEN`): the level detector has no supply while receiving |
| Records | `docs/testing/results/cs7000p/seed.json`, with no feature row |

## First radio

One unit, 2026-11-22, by its owner; readings through `rfoundry_debug.py view --section board` unless said otherwise.
This radio went straight to the install: stages 0 to 2 were not run on it as written.

| What | Seen | Settles |
|---|---|---|
| The first install | Through the ROM bootloader with the web installer; the image starts. `board.rdp=0xaa` afterwards | the route as a whole; the reset path, the power hold, the clock |
| `board.clock=hse`, `board.mcu_rev=V`, `board.rom_loader=0x92` | the crystal's tree; silicon revision V; ROM loader 9.2 | questions 14 and 16 (in part: not measured before the install) |
| `board.lcd_board=assumed levels=0x1e`, `board.panel=st7735 source=id id=7c89f0` | the board check passes; the panel answers its ID; the display works | question 10 |
| Keys | the twelve digit keys, \*, # and the side keys as drawn; the six function keys read one at a time (`keys_raw`, hardware page 9.4) and their order corrected | question 11's key half |
| `board.flash=EF4019 size=0x2000000 addr4=1`, `board.store=foreign` | the data flash answers; the board sector at 0x01E000 holds data that is not ours, the maker's records ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 2.4) | stage 6's first rows |
| `board.vbat_mv=8089`, `board.vdda_mv=3275`, `board.knob=on`, `board.hold=1`, `board.pow_det_mv=2080` | plausible on a charged pack; not compared with a meter | question 13 in part |
| `board.hrc=1 r10=0x80`, `board.gps_baud=9600` | the HR_C6000 answers on its port in FM receive; the GPS module is heard at 9600 | questions 23 and 32 (the sleep line) |
| `board.pll=locked r=2 vco_hz=383550000` on 433.500 MHz; unlocked with the LO side high (483.45 MHz) | the synthesizer programs and locks on the low side; the high side is out of the VCO's range there | question 2 |
| FM receive, a carrier on 433.500 MHz | **With the first image the receiver was 10.5 kHz high** (centered at 433.490 MHz tuned, `rssi_raw` about 2990 and `noise_raw` about 380 there against 1250 and 1300 on channel; idle 770 and 1100): the reference DAC was off. With the DAC on at mid-scale the carrier is centered on channel. The second image wrote the control word but left the DAC's level at 0, and the squelch still stayed shut. The hold is now 4:0x25 = 0x0C with the level at mid-scale plus the offset (hardware page 6, [hrc6000-registers.md](../spec/hrc6000-registers.md) 3.22) | question 1 except the fine offset; the squelch reading's direction and scale |
| PC8's duty 0, 512, 1023 at the passband's edge | `rssi_raw` 2019, 2380, 2711: about 2 kHz end to end | question 4's size |
| `rfoundry_debug.py reboot --rom-dfu` | refused by the first image (the entry was off); taken by the image that allows it, and the screen went gray: the ROM bootloader was reached from the running image | question 18's entry itself (not the pin questions) |
| The channel selector, 16 detents | `chsw_raw` `F D C 8 9 1 0 4 5 7 6 2 3 B A E`: a Gray code, now the table; with it the channel goes up when the selector is turned clockwise | question 11 |
| The volume knob | `vol_raw` 7 just above the power click, 3178 at full volume | `CS7000P_VOL_FULL_RAW` |
| `board.hrc` with the second image | `1 r10=0x80 r25=0x0c`: the receive hold is written and reads back | stage 8's hold row |
| `board.i2s` | `dir=swapped ioswp=0 edges=0/869`: the probe saw the chip's clock and gave a verdict | question 8 |
| The full backup and the claim | the web installer's Back Up, Everything read the data flash and claimed the settings region; settings then survive a power cycle and a codeplug can be written | stage 6: the claim-first order, storage on this flash (question 21 stays: what the maker keeps where is still unknown, the backup holds it) |
| The passband against another radio's carrier on 433.500 MHz, 500 Hz steps, wide filter | `noise_raw` under 800 from -6.0 to +6.5 kHz: the center 250 Hz high, inside the sweep's resolution and the other radio's own error. The reference offset stays 0 | question 1's level: mid-scale is on frequency to a few hundred Hz on this unit |
| DMR receive, a handheld's group call on 433.500 MHz, color code 1, simplex, 5 feet away | Synced at once: `dmr.rx_cc=1`, `call.src` and `call.dst` decoded, 222 voice bursts in 13.4 s (none missed), 666 voice frames, the call ended by its terminator. `voice.rx_bit_errors` 132 and `voice.rx_repeats` 44 over the call, at a signal strong enough to overload the receiver. `board.hrc` read `r10=0x6e`; `board.i2s` went to 1 for the call. No audio: no codec | question 2's I/Q order, question 9's IF level for DMR, the HR_C6000's three interrupt lines and its DMR set-up; the error rate at a normal signal level is not measured |
| A key press | beeps | PCM to the chip's line out, the amplifier and the speaker switch (stage 8's beep row) |
| The six function keys with the corrected order | each does what its label says | question 11's key half |
| FM receive with the level fix (the bias at mid-scale, written in one step) | the squelch opens on a signal on channel, also after minutes of idling; the audio is clear and at a sensible level. An image that wrote the level through zero at every receive entry opened the squelch after a start and later stopped | question 1 except the fine offset; question 9's audio |
| An update from the web installer | failed once with "not this radio's update mode (it reports ?)": the browser had no alternate setting names for the loader. The page now reads them from the device. An update with `rfoundry flash` from a loader entered by hand worked | the page's device check |

Not checked on it yet: the reference against a counter (only against another radio), sensitivity in FM and DMR, the
DMR error rate at a normal signal level, DMR data and text, CTCSS and DCS, the battery against a meter, and
everything of stage 11.

**One unexplained restart.** Once, while a script waited with the radio switched to the VFO, the radio left USB and
came back with `reset.cause=power`, `reset.flags=0x00fe0000` (power-on and brown-out, no software or watchdog
reset) and no crash record; its owner did not see it go off. The same steps run one at a time, with the other radio
keyed 5 feet away, did not repeat it (`vbat_mv` and `pow_det_mv` steady to 6 mV). Watch `reset.cause` and the uptime
in `view` during bench work: a second one with a cause is worth more than this one.

The board's host tests (`boards/cs7000p/tests`, run by `make -C tests/host`), each on the board's own source:

| Test | Covers |
|---|---|
| `cs7000pExpTest` (and `...Floor`) | the expander's one writer and the shared DAC's interlock, an interrupt racing the interlock's masked section, the reset word, the SK1 wait before a software reset |
| `cs7000pPowerTest` | `power_cs7000p.c` on fakes: the hold, the power-off order, the scan stopping, a sagging pack, the off state's LEDs, a `led()` call the power-off cuts into, the off watch idle once the off state ends |
| `cs7000pBatteryTest` | the battery model, the knob's switch-open threshold, the hold and the off watch rules |
| `cs7000pRadioTest` | `radio_cs7000p.c` over the real chain: the microphone in FM and DMR, the off state: no receiver restore, no amplifier (also behind a speaker call the power-off cuts into), no transmission in FM or DMR |
| `cs7000pRadioSeqTest` | the RF sequences on a model of the chain, partial calibrations, the bench drive at a measured floor, a safe request in the middle of a key-up at floor 0, the off state's refusal (`why=12`) |
| `cs7000pRfTest`, `cs7000pCalTest` | the frequency plan, the band edges, the calibration verdict |
| `cs7000pPinsTest`, `cs7000pPanelTest`, `cs7000pKeysTest`, `cs7000pSpeakerTest`, `cs7000pSubPwmTest`, `cs7000pStoreTest` | the pin set-up and its never-driven list, the LCD board check and the panel ID, the keys and the selector, the speaker lines, PC8's duty, the flash map |

## At a glance: what is assumed

Everything below is unverified. These are the assumptions a first power-on meets first; the stages hold the checks,
and the table at the end has one row per open question.

| Assumption | Stage | Question |
|---|---|---|
| The maker's recovery writes its default codeplug over the radio's: the codeplug and Radio Info are saved first (and the tuning data, if the maker's tuner software can export it) and the codeplug written back after it | 0, 2 | 20 |
| The processor is read protected as shipped (stage 1 reads the state; an unprotected one is read out before anything erases it) | 1 | 14 |
| A Read Unprotect is one-way: it erases the maker's bootloader and both programs, and only the maker's recovery brings them back | 2, 3 | 20 |
| The ROM loader's answers: a protected read, the strings and layout, the unprotect leaving USB, the Leave, a 1024-byte block written as block 2 at its own Set Address Pointer and read back the same way, no alternate setting selected. On loader V9.0 (0x90) a cold entry may need a second reset | 1, 3, 12 | 14, 18, 19, 30 |
| A normal power-on with the first sector erased starts nothing: this chip has no empty-flash fallback to the ROM loader | 3 | 19 |
| The radio stays on with the knob off only after a watchdog reset with SK1 held, or when RadioFoundry has not run since power-on (loader mode entered with SK1, an install cut short): the chain keeps its state. The pack must come off | 0, 3, 4, 12 | 29 |
| Polarities read from documents only: `DMR_SLEEP`, `A-PDN`, the AK2365A's select, `VCOVCC_SW`, the hold, `EXP_LAMP` | 4, 5, 8, 9 | 32, 12 |
| The expander's bit order on the hardware: the chain has no read-back, so only the outputs show it | 4 | 34 |
| The LCD board is the OEM's 2018 board, with an ST7735-class controller | 5 | 10 |
| The channel selector on another unit: its code table and direction were read on one radio | 5 | 11 |
| The I2S data direction: probed, never assumed; anything but `ok` or `swapped` stops the stage | 8 | 8 |
| The HR_C6000 keeps its reference hold (4:0x25 = 0x0C) through a DMR transmission; what the DAC does in a DMR TX slot is not measured | 8, 11 | 1 |
| The reference oscillator's bias: which register, the coding, the on-frequency level | 9, 11 | 1, 35 |
| The first LO is below the signal | 9 | 2 |
| The lock line reads unlocked at once after new synthesizer words; the DAC carries the tuning value about 100 us after a burst | 11 | 3, 25, 33 |
| No calibration: every transmission is refused until the PA and modulation tables are measured | 11 | 5 |
| The whole-chip backup is complete (no claim before it) | 6 | 21 |

## 0. Before anything

**The first install is one-way from RadioFoundry's side.** The radio's processor is expected to be read protected
(RDP level 1 in the maker's recovery procedure; stage 1 reads the state). On a protected chip the ROM bootloader
writes nothing until a Read Unprotect, and that erases the whole program flash: the maker's bootloader and both of
its programs. Nothing of them can be read out first. On an unprotected chip they can, and stage 1 says how.

1. **While the maker's firmware still runs, save the codeplug with the maker's programming software (CPS), and note
   the Radio Info screen** (firmware, bootloader, hardware, database, serial number). Keep both. This comes first:
   the maker's recovery (stage 2) writes its default codeplug (`ResetDefault_M17PLUS.rdbx`) over the radio's, and
   afterwards Radio Info shows the recovery's versions. RadioFoundry does not bring the channels over. **If the
   maker's tuner software** (on its software page for this radio) **can save the tuning data to a file, save that
   too**, and record whether it can: no document says. Fit the dummy load on the antenna connector first: the
   maker's service manual asks for a 50 ohm load before its test mode is entered. Read only: change no value, and
   if the tool has no export, close it and go on. The file is the only copy if stage 2 shows that the calibration
   was in the processor's flash (question 20).
2. **Get the maker's recovery files and instructions** for this radio (its "unbricking" procedure and the files it
   names).
3. Bench: a current-limited supply in place of the pack, or a pack you can pull; a dummy load on the antenna
   connector; an SDR or a signal generator. **Nothing transmits until stage 11.** Stage 5 presses PTT and the top
   key (emergency) only to read them, with the dummy load fitted, and the radio refuses (`board.cal=none`,
   `board.tx_refused` why=2). Stage 4 presses PTT twice, the same way: once for the red LED of a refused press
   (`why=2`), once in the power's off state (`why=12`). Stage 2's check of the maker's firmware is receive only, or
   a key-up into the dummy load. The maker's tuner software (step 1) is used to read only, with the dummy load
   fitted.
4. Know when the pack must come off: the radio stays on with the knob off, the PA off through R141, (a) after a
   watchdog reset with SK1 held, the one way into the loader the firmware can't guard, and (b) whenever RadioFoundry
   has not run since power-on: loader mode entered with SK1 (stages 1 to 3), or an install cut short. The chain then
   keeps its random power-up state, and the hold may be part of it (question 29). (c) A firmware fault that blocks
   the start task for good stops the knob poll the same way ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 5.3).

There is no way known to back up the data flash before RadioFoundry runs. RadioFoundry takes that backup itself,
before anything writes the chip (stage 6).

### Build

```
make BOARD=cs7000p
```

Nothing touches the radio here. The install is stage 3, after stages 1 and 2.

No debugger is needed. SWD is on two test points (TP304, TP301) with TP305 for reset; the firmware never
reconfigures those pins.

### Bench knobs

Build-time, through one make variable:
`make BOARD=cs7000p CS7000P_DEFS="-DCS7000P_LCD_FORCE=1 -DCS7000P_LO_HIGH=1"`. Each is an `#ifndef` default in the
board file that uses it (`grep -rn CS7000P_ boards/cs7000p` finds it). Batch them: every stage lists what to try, so
one rebuild can carry several.

| Knob | Default | Does |
|---|---|---|
| `CS7000P_LO_HIGH` | 0 | The first LO's side at start-up: 0 below the signal, 1 above. Sets the receive I/Q order with it. The run-time switch is `chip_reg` 3:3 |
| `CS7000P_REF_OFFSET` | 0 | The HR_C6000 reference offset written at every receive entry |
| `CS7000P_RX_TUNE_NOMINAL` | 2048 | The front-end tuning DAC code used while the table has none |
| `CS7000P_FE_GAIN_DB` | 0 | The front end's gain, for the RSSI figure in dBm |
| `CS7000P_E0_MIC` | 0x02 | The 4:0xE0 codec input bit forced after the DMR entry and the FM transmit set-up |
| `CS7000P_APC_FLOOR` | 0 | The largest DAC code that gives no RF output: the interlock's threshold (`exp_cs7000p.h`), and the calibration table's floor when the build gives it (`rfcal_cs7000p.c`): the two cannot differ |
| `CS7000P_RAIL_SETTLE_US` | 2000 | The wait after the synthesizer rail comes on at start-up (`cs7000p_rf_start`). The receive set-ups wait the sequencer's `rail_settle_us` instead, `CS7000P_RX_RAIL_US` (not a knob, below) |
| `CS7000P_REF_SETTLE_MS` | 30 | No transmission and no frequency measurement before this long after the synthesizer rail came on |
| `CS7000P_CTCSS_DEPTH` | 30 | PC8's swing at the generator's full scale, in percent of half the PWM period (`subpwm_cs7000p.h`). A first value: the deviation it gives is unmeasured, on 12.5 and on 25 kHz channels alike (the same swing on both) |
| `CS7000P_DCS_INVERT` | 0 | DCS polarity on PC8 |
| `CS7000P_DCS_RX_INVERT` | 0 | DCS polarity on the detector's input (PA2). Unverified; it may follow the first LO's side (`subpwm_cs7000p.h`) |
| `CS7000P_PANEL_TYPE` | not set | Forces a panel type of `drivers/display/st7735` (0 to 3, else the build fails) instead of the ID read's choice. Reported as `source=build`; `panel=` still names the controller the ID gives (`panel_cs7000p.h`) |
| `CS7000P_PANEL_MADCTL_XOR` | 0 | Flips the controller's orientation bits (0x40 columns, 0x80 rows, 0xC0 both) |
| `CS7000P_LCD_FORCE` | 0 | 1: skips the LCD board check and drives the connector. **Only after the board has been traced** |
| `CS7000P_CHSW_VERIFIED` | 1 | 1: the selector makes steps from the table read on a radio (`keys_cs7000p.h`). 0 turns it off |
| `CS7000P_BL_MIN`, `CS7000P_BL_MAX` | 20, 70 | The PWM duty in percent that 1 % and 100 % backlight map to |
| `CS7000P_VOL_FULL_RAW` | 3178 | The `VOL-DET` reading at full volume (`battery_cs7000p.h`), read on one radio; 7 just above the power click |
| `CS7000P_BAT_GAIN_PPK`, `CS7000P_BAT_OFFSET_MV` | 4006, 245 | The battery model: mV = (gain x `POW_DET` mV) / 1000 - offset (`battery_cs7000p.h`) |
| `CS7000P_BENCH_DMR_TX` | 0 | 1: the bench drive override may also key DMR bursts |
| `CS7000P_SOFT_ROM_ENTRY` | 1 | 1: allows the software entry to the ROM bootloader, and then only with loader version 0x91 or with PB10 reading high (`board.c`; the rule is `cs7000p_rom_entry_ok()` in `romentry_cs7000p.h`). Refused, or with 0, `rfoundry_debug.py reboot --rom-dfu` gets a refusal (reason 5) and the radio runs on |

Not knobs, but the places for measured values: `CS7000P_FN_KEYS` and the selector's table in `keys_cs7000p.h`; the
model tables in `rfcal_cs7000p.c`; the polarity of an expander bit in `exp_cs7000p.h`; the fixed waits in
`rfops_cs7000p.h` (`CS7000P_RX_RAIL_US` and the others of section 13), edited there.

Run-time, over the debug link (`tools/rfoundry_debug.py reg 3 N [VALUE]`,
[hardware-cs7000p.md](../spec/hardware-cs7000p.md) 14): 3:0 the receive tuning DAC, 3:1 the expander shadow (read),
3:2 PC8's idle duty, 3:3 the LO side, 3:4 the reference offset. Chip 0 is the HR_C6000, 1 the SKY72310 (write
only), 2 the AK2365A. **Never write DAC1 or GPIOE with `reg soc`**: such a write bypasses the interlock and can key
the PA, at any stage and whatever `board.cal` says (hardware page 4.3, 14). No step on this page needs a SoC write.

## 1. Loader mode, identified

Read only: nothing in this stage writes the radio. **Start a USB capture first** (usbmon with Wireshark on Linux,
USBPcap on Windows) and keep it running through the first `rfoundry detect`.

Loader mode: switch the radio off, hold **SK1** (the side key above PTT), switch it on with the volume knob, release
the key. The screen lights up gray and stays blank. Keep loader sessions short: the ROM bootloader drives several of
this board's nets and writes nothing to the output expander ([hardware-cs7000p.md](../spec/hardware-cs7000p.md)
2.5). If the knob does not switch the radio off afterwards, take the pack off (stage 0, step 4).

```
rfoundry detect --board cs7000p        # radio in loader mode: strings, layouts, protection; nothing written
```

| Look for | Proves | If not |
|---|---|---|
| With SK1 held at power-on the screen lights up gray and stays blank, and `0483:DF11` enumerates | BOOT0 on SK1, the ROM bootloader, USB on the accessory cable | Nothing enumerates: the cable (the maker's programming cable carries USB), the driver ([Installing](../user/02-installing.md#windows-the-bootloader-driver)), or ROM loader V9.0 (0x90), which may need a second reset after power-on before it connects (AN2606 Table 146; ST's workaround, a pull-up on PA11, is not on this board): hold SK1 and ground TP305 (NRST) briefly, or switch off and on again with SK1 held. Record which one worked |
| `rfoundry detect` prints string 1, the manufacturer and product strings and both alternate settings' names, then "the bootloader in a processor's ROM (plain DfuSe)" with this radio among the boards it can be. **Record them** | the device rfoundry will accept: with no `[flash.dfu] layout` in `board.toml`, the first layout must be the whole 2 MB program flash at 0x08000000 | "not a bootloader of a radio rfoundry knows (another chip's ROM loader?)" and no protection line: the first layout string is not the one expected. Record it; if it is this chip's (AN2606), it goes in `board.toml` as `[flash.dfu] layout`. Record `string1` and `product` there too: the Windows driver step can then name the device |
| `lsusb -v -d 0483:df11` (Linux, read only; another system's USB descriptor viewer, or the capture, shows the same): `wTransferSize` in the DFU functional descriptor. **Record it** | for the record only. AN3156 5.1 places a block at (block number - 2) x wTransferSize + pointer, and no document gives this loader's value. Both tools send every block as block 2 right after its own Set Address Pointer, so the value does not decide where a block lands | Any value is recorded; none stops the stage |
| "read protection: on". **Record it**: this is the state as shipped, before the maker's recovery | RDP level 1 as shipped (question 14): a protected read answers `dfuERROR` with status `errVENDOR` (0x0B) | **"read protection: off": record it and stop before stage 2.** The program flash can then be read through the loader (AN3156 Table 4 allows Read Memory with the protection off), and stage 2 erases it. Save all 2 MB (0x08000000, length 0x200000) with a DfuSe tool, for example STM32CubeProgrammer, which the maker's recovery uses: from alternate setting 0, the internal flash, never 1, the option bytes. Read only: write nothing, erase nothing, and leave the tool's "Read Unprotect (MCU)" box clear (the maker's recovery ticks it). Read it twice and compare the files; keep the loader session no longer than the two reads take. Keep the file private, never in the repository, and do not open or disassemble it: it is the only copy of what the unit holds before the maker's recovery erases it. A sector that refuses the read (PCROP): record its address. The row then counts as passed; stage 2's step 4 shows whether the recovery protected the chip, and so whether stage 3 needs `--unprotect`. The probe stops with "... neither the data nor "read protected" (status N, state S, ...)" and changes nothing: record N and S; `probe_protected` in `rfoundry/dfu.py` takes them |
| **The capture** of that first `rfoundry detect` shows no SET_INTERFACE request | rfoundry never selects the bootloader's second alternate setting, the option bytes. Its own code sends none (tested on the fake); that the USB library sends none by itself is what this checks | A SET_INTERFACE to alternate 1: **stop** and report it. Do not go on to stage 3: an install would send its writes to the option bytes |

## 2. The maker's recovery, proven on this unit

**Required before stage 3.** Stage 0, step 1 must be done: the recovery writes the maker's default codeplug over the
radio's. Its first step ("clear the radio" with STM32CubeProgrammer) removes the read protection itself and erases the
shipped bootloader and programs; the rest of it loads the maker's current ones. From here Radio Info shows the
recovery's versions, so stage 0's notes are the only record of what the unit shipped with.

1. Run the maker's recovery once on this unit, with its files and tools, as its instructions say (question 20).
2. Check that the radio works on the maker's firmware: it starts, Radio Info shows the recovery's versions, it
   receives. Receive only, or a key-up into the dummy load.
3. Write the saved codeplug back with the maker's CPS, and check the channels.
4. Put the radio in loader mode and run `rfoundry detect --board cs7000p` again. Record whether it says "read
   protection: on": whether the recovery protects the chip again is in no document. The install's probe decides
   either way. On Windows the maker's DFU tools may need another driver for `0483:DF11` than rfoundry's WinUSB
   ([Installing](../user/02-installing.md#windows-the-bootloader-driver) says how to switch back).

| Look for | Proves | If not |
|---|---|---|
| The radio works on the maker's firmware after its recovery, with the saved codeplug back | the way back works on this unit, and nothing the radio needs is in the processor's flash (question 20) | **Stop: do not install.** Report it with Radio Info before and after |

## 3. Install, and the debug link answers

**Only after** every stage 1 row passed, the capture showed no SET_INTERFACE, and stage 2 is done on this unit.

```
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --dry-run  # the plan, nothing sent
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --unprotect
```

Without `--unprotect`, rfoundry stops at a protected radio and changes nothing. A chip its probe finds unprotected
(stage 1, or stage 2's step 4 after the recovery) is written in that run, with or without the option: no Read
Unprotect is sent, and the one question is "Erase and write N sector(s) now?". With `--unprotect` at a protected
radio, rfoundry asks once ("Remove the read protection now? ..."), sends the Read Unprotect, and waits until the
radio has left USB (at most 120 s) and at least 60 s have passed since the command. Both figures come from
`mass_erase_max_s = 26` in `platform/stm32h7/platform.toml` (tME, DS12110 Table 43): the least wait is twice that
bound and never under 60 s (2 x 26 = 52, so the floor), the longest twice the least and never under 120 s
(2 x 60 = 120). Only then does it ask for the radio to be switched off and put in loader mode again; once the radio
is back it says to leave it on and connected while it is written, and writes with no second question. AN3156 lists
no system reset after the erase for this family, but should the bootloader be back on USB by itself (a reset with
SK1 still held), rfoundry writes to it at once and says so, with no switch-off line
([release.md](../dev/release.md#the-rom-bootloader-route)). If the knob does not switch the radio off in loader mode,
take the pack off (stage 0, step 4). Use rfoundry or the web flasher: both fill the image to whole 32-byte flash
words (question 30).

The plan erases the first sector first and writes the vector table's block last, after every other block is written
and read back; a vector table whose write fails or that does not read back has its sector erased again. An install cut
short therefore leaves no vector table. With SK1 released this chip boots from `BOOT_ADD0` (0x08000000, RM0433 Table
9) and has no empty-flash fallback to the ROM loader (AN2606 Pattern 10 enters it only with the boot pin high and
`BOOT_ADD1` on system memory): the core loads 0xFFFFFFFF as its stack pointer and reset vector and faults at once.
Nothing runs and nothing transmits (PA4 is not driven, R141 holds the PA off), the screen is probably lit and blank as
in loader mode (PC9 floats, [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 9.3), and the chain keeps its power-up
state, so the knob may not switch the radio off. Put it in loader mode and run the same install again. Nothing
protected is lost.

| Look for | Proves | If not |
|---|---|---|
| `--dry-run` prints "order: sector 0 erased first and written last (a cut install leaves no application to start)" | the vector-last plan (`check_plain` refuses any other) | Another order: the plan or the check is wrong; do not go on |
| The first `--unprotect` run: the first GETSTATUS after the command answers busy, the device leaves USB by itself within 120 s without restarting, and rfoundry asks for the switch-off no sooner than 60 s after the command | AN3156's description of Read Unprotect | "the bootloader did not take the Read Unprotect command (status N, state S)": record them. rfoundry and the web page say when to switch off: after the radio has left USB and at least 60 s (the least wait). With no disconnect within 120 s (the longest wait) the tools say the erase is over or was not taken (120 s is more than four times the 26 s tME bound of DS12110 Table 43), and the next probe decides. **Record** whether the log shows "the bootloader left the bus ... it may be erasing" (a lost GETSTATUS reply after 0x92, taken as the command taken), and whether the bootloader came back by itself with SK1 held through the erase |
| Every "erase" line of the first install passes | a 128 KB sector erases in up to 4 s (DS12110), inside rfoundry's 5 s per status request | "USB error while flashing" during an erase: run the same command again (the sector is then blank and erases quickly) and record it; the timeout is the `5000` in `get_status` of `rfoundry/dfu.py` |
| After the write: "verify: read back N blocks", then "write the vector table at 0x08000000" and "verify: read back the vector table", before Leave | the loader takes a 1024-byte UPLOAD of block 2 after Set Address Pointer and ABORT, for every block (the probe's 4-byte read is the only upload any document describes for a protected chip), and it put every block, written as block 2 after its own Set Address Pointer, at that pointer | A refused read or a mismatch stops the install with the image not started; the radio stays in the loader. A refused read on every block: record the status and state; the read-back is `Dfu.read_back` in `rfoundry/dfu.py` and `Dfu.readBack` in the web flasher's `dfu.js` |
| A mismatch at the same address on a second run of the same command | not a one-off. The expected cause is option-byte protection the regression left in place (a write-protected or PCROP sector; loader V9.0 does not clear PCROP, AN2606 Table 146). It is not the block numbering: both tools send every block as block 2 right after its own Set Address Pointer, so a block's address is the pointer whatever the loader multiplies the block number by (AN3156 5.1; stage 1 has the descriptor's `wTransferSize`) | **Record the address, and whether it is a sector's first block, and stop retrying.** Run the same install once with the other tool (rfoundry or the web flasher; both send the same requests): the same address again is the chip, another result is the tool or the computer's USB. No tool here writes option bytes (question 14) |
| After the second loader entry the write runs and the radio starts | the ROM's Leave into the image (question 19) | The radio shows nothing: switch it off and on (rfoundry says so). Still nothing: section "If it does not start" below |
| The USB serial port enumerates as "CS7000 M17 PLUS" | the clock, USB | No USB at all: the image did not start (USB has a clock on every clock tree) |
| `tools/rfoundry_debug.py --port PORT version` says `board=CS7000 M17 PLUS` | the debug link | |
| `rfoundry_debug.py view --section board`: `board.clock=hse` | the 25 MHz crystal and PLL1 from a 1 MHz input (question 16) | `hsi-pll`: the crystal did not start within 100 ms, or stopped while the radio ran and was still stopped at the restart (the clock security system's NMI: a crash record with cause 2 and a restart before it, `rfoundry_debug.py crash show`; `tools/crash_decode.py` prints it as "exception 2 (NMI: the clock security system, the external oscillator failed; pc is only where it was interrupted, not the cause)"); the radio runs at the same rates from the HSI, and the stages below work on it. Crash records with cause 2 (a rising count in `crash show`) while `board.clock=hse`: a crystal that stops and starts again. `hsi`: no PLL locked: USB and the debug link work, SPI1 to SPI3 have no kernel clock (no HR_C6000 voice port), so stop at stage 7. Then a long serial transfer, to check USB on that clock |
| `board.mcu_rev`, `board.rom_loader`, `board.rdp`: **record them** | the silicon revision (Y or V), the ROM loader's version, RDP level 0 after the install (question 14) | Revision Y: the USB PHY errata apply (ES0392 2.2.13, 2.25.1), and on the HSI fallback the 48 MHz clock is untrimmed |
| `rfoundry_debug.py chip`: family STM32H7, device ID 0x450, 2 MB | the chip info of the new family | |
| The watchdog runs: `rfoundry_debug.py watchdog show` | IWDG1 | |

If it does not start: the image's first word must be a stack pointer in 0x24000000 to 0x2407FFE0 and the second a
reset vector in bank 1. Loader mode is in the processor and cannot be erased (on loader 0x90 it may need the second
reset of stage 1), so install again.

Unverified in the platform itself, to check here once: the crash store (`rfoundry_debug.py crash test trap`, then
`crash show` after the restart: the record must be whole, CRC included, which is what the last-word flush is for);
the reset cause the crash log reports after a power-on, a `reboot` and a watchdog test
(`rfoundry_debug.py watchdog test starve`); `rfoundry_debug.py peek`
of the ID words; `tests/hil/test_chip_reg.py` (CPUID 0x411FC271) and `tests/hil/test_support_info.py`.

A known limit: reading a program flash word whose programming was cut short (an install that lost power) takes a
bus fault on this chip (RM0433 4.7.7). A `dump` or `peek` across such a word restarts the radio with a crash record
instead of answering. Install again; a whole image leaves no such word in the range it wrote.

## 4. Power, the knob, the expander

| Look for | Proves | If not |
|---|---|---|
| `board.knob=on`, `board.hold=1` | the ADC scan started and read the knob's switch closed three times (`POW_DET` at or above 0.7 V, `CS7000P_KNOB_OPEN_POW_DET_MV`) | `noadc`: the ADC did not start; the hold is never set and the radio goes off with the knob. `off` with the knob on: `POW_DET` read under 0.7 V, which a closed switch gives only far under any working battery: the ADC input or the wiring, not the battery model. **Record** `board.pow_det_mv` with the knob on at the lowest supply the radio runs at, and `POW_DET` on a scope as the knob goes off on a running radio (expected under 0.1 V within a millisecond) |
| `board.exp` is the reset word 0x2A0181 with the hold (bit 20, 0x100000) set, bit 7 (`EXP_DMR_SLEEP`) clear once the HR_C6000 is up, bit 17 (`EXP_SPK_INT_OFF`) clear once the speaker has played, bit 0 as the AK2365A's select last left it, and the rail, LED and lamp bits the radio has switched on since. No transmit bit (3, 9). `board.exp` is the driver's shadow, the word last shifted out: the chain has no read-back (IC621's QH' is open) | the firmware's writes: the reset word, then the hold, through the one writer, with the bits of `exp_cs7000p.h`. **Not the hardware's bit order**: a reversed or shifted chain shows the same value | A different value: a firmware fault, or a define in `exp_cs7000p.h` that differs from [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 4.1. The bit order is the next two rows |
| **Once, on a scope:** PE7, PE8, PE9 at power-on. Decode the first frame's 24 bits, bit 23 first: 0x2A0181 | the shift order and the reset word on the wire; the time from power-on to the reset word (expected: the reset temporization plus about 0.1 ms), the chain clock's half period there (0.5 us or more) and in normal running (0.16 us or more) (question 34) | Another word: the shift direction in `exp_cs7000p.c`. Half period under 100 ns: raise `EXP_DELAY_PASSES` in `exp_cs7000p.h` |
| The bit order at the outputs, dummy load fitted, receiving, `board.cal=none` (the firmware drives nothing into the PA): the green LED (bit 12) with Monitor held; the key lights (bit 16) with the backlight on and off; the red LED (bit 13) in the red and green flash of a refused PTT press; a meter on 5R (bit 15) and 5C (bit 11) while receiving, 5T (bit 9) at 0 V and no PA current. `board.exp` is the expected value for each | the chain's bit order in the hardware, bit by bit, before anything can transmit | **Stop before stage 11**: the chain's order or a bit of `exp_cs7000p.h` is wrong. Stages 5 (the lamp), 8 (the sleep bit) and 9 (the AK2365A's and the VCO's bits) check more bits the same way |
| Knob off: the radio is off within a second. Knob on again: it restarts | `switch_on()` on the switch-open threshold, the hold cleared at power-off | It stays on: pull the pack; the hold bit's polarity (`EXP_POWER_HOLD`), or the threshold |
| `rfoundry_debug.py reboot` with the knob on: the radio restarts | `board_reset_prepare()` writes the reset word and the knob still powers the radio | It goes off and stays off: the knob path is not what powers it; re-read the latch |
| Hold Monitor (SK1), knob on, and send `rfoundry_debug.py reboot`: nothing happens while SK1 is held (up to about 30 s; held longer, the reset goes on into the ROM loader with the hold clear). Release it within that: the image starts, with no gray screen | `board_reset_prepare()`'s wait for SK1's release (`cs7000p_sk1_wait_more()` in `romentry_cs7000p.h`) | A gray screen (the ROM loader): `PD15`'s level while SK1 is held (low expected), or the wait. Knob off, then on, leaves it |
| Knob off while the radio is busy (a menu open, receiving): off all the same | no busy state keeps the hold | |
| An SDR next to the radio at power-on and in loader mode, dummy load fitted: no carrier | the PA is off while the expander is undefined (question 6) | A carrier in loader mode is the maker's design too; note its level and length |
| At 6.0 V on the bench supply, the radio running normally (`board.hold=1`): knob off. The radio runs its power-off and does not drop at the knob: the debug link still answers for the 200 ms after the knob (`POWER_OFF_TX_MS`, longer during a deregistration). **Once, on a scope:** 3V3 (TP401) stays up after `POW_DET` falls, until the latch lets go | the hold's margin at low battery (question 13) | The radio drops at the knob: a hardware limit; note the voltage |
| `rfoundry_debug.py radio set battery_off_s 10`, the bench supply at 5.9 V (`board.vbat_mv` under 6,000), knob on: after 10 s the critical-battery power-off. The speaker is silent and the LEDs stay off (the off state). **Measure the current drawn**: the MCU and the receive rails stay powered until the knob goes off. Then raise the supply until `board.vbat_mv` reads 6,200 or more (about 6.3 V; the battery model is unmeasured until stage 7, and the core's own critical-battery refusal ends only at 6,200 mV by that reading) and press PTT, dummy load fitted: no transmission, and `board.tx_refused` counts up with `why=12` (not `why=2`: the off state is the first thing the board checks, before the calibration). Knob off: the radio goes off. Knob on (the setting is not stored before the claim, stage 6), send `radio set battery_off_s 10` again at 5.9 V, wait for the off state, then turn the knob off and on: the radio starts again. **Record `reset.cause`** (`rfoundry_debug.py view --section reset`): `software` is the off watch's restart, `power` (or `brownout`) the supply gone with the knob (expected for a turn by hand: the hold is clear, so the supply goes within milliseconds) | the off state (`cs7000p_power_off_state()`) and its refusal of a transmission; a knob off and on leaves it, by the off watch's restart or as a cold start | A beep, noise or a lit LED: the off state's gates (`radio_cs7000p.c`, `power_cs7000p.c`). A carrier, or another `why`: the off state's refusal (`cs7000p_rf_tx_admit`, `tx_request()` in `radio_cs7000p.c`). The count does not move: the core refused before the board was asked (its battery level is still critical: `board.vbat_mv`). The radio stays in the off state after the knob went off and on: the watch (`cs7000p_offwatch_restart()` in `battery_cs7000p.h`) |

## 5. LCD board, display, keys, selector

| Look for | Proves | If not |
|---|---|---|
| `board.lcd_board=assumed` with only PE0 low among the five levels | the LCD board is the assumed one (question 10) | **`unexpected`: stop.** The display and the key matrix stay off by design. Trace the board against [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 9.1 before building with `CS7000P_LCD_FORCE=1`. The radio still runs by the debug link, the side keys and PTT |
| `board.panel`: a controller name, `source=id`, six ID bytes that are not all 0x00 or 0xFF. **Record the bytes** | the read strobe, the bus timing, a controller `st7735` knows | `id` and `reg` all `00`: the panel did not answer the read (the bus drives the lines low before it turns them round), not an unknown controller. Check the reset timing, the RD strobe and the CS / D/C polarity first. `panel=unknown source=default` with other bytes: the ID is not in the driver's table. The standard init was sent. Note the bytes; try `CS7000P_PANEL_TYPE`, then `CS7000P_PANEL_MADCTL_XOR` |
| The Home screen, right side up, colors right, no band of garbage at an edge | 160 x 128, offsets and insets 0 | `rfoundry_debug.py screenshot` shows what the radio drew; the difference is the panel. `CONFIG_DISPLAY_INSET` in `config.mk`, the offsets in `display_cs7000p.c` |
| The backlight dims over the whole setting range | TIM8 CH4 on PC9, the 20 to 70 % duty window (question 12) | [backlight-curve.md](backlight-curve.md) measures the curve; then `CS7000P_BL_MIN`, `CS7000P_BL_MAX` |
| The key lights follow the backlight | `EXP_LAMP`'s polarity | Inverted: the bit in `exp_cs7000p.h` |
| `board.keys_raw`, one key at a time, all 18 front keys: each digit, \* and # at its crossing; OK, BACK, F1, F2 as `01`, `02`, `04`, `08` in the first byte, Up and Down as `01`, `02` in the last (read on one radio, 2026-11-22: [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 9.4) | the matrix, and the six function switches' order | A digit elsewhere: the matrix table in `keys_cs7000p.h`. A function key elsewhere: another LCD board revision; put its order in `CS7000P_FN_KEYS` and report it |
| SK1 = Monitor, SK2, SK3, the top key, PTT each show in the diagnostics page; the accessory PTT only after its line has read high. PTT and the top key only after `board.cal=none` has been seen, with the dummy load fitted: a refusal (`board.tx_refused` why=2) is expected | PD15, PE12, PE13, PE10, PA8, PE11 | The pulls and the mapping in `input_cs7000p.c` |
| `board.chsw_raw` over 16 detents: `F D C 8 9 1 0 4 5 7 6 2 3 B A E` (read on one radio); `board.chsw` counts 0 to 15 with it. **Turning clockwise takes the channel up** | the selector's code and its direction (question 11) | Other codes: another switch; fill the table in `keys_cs7000p.h`. The channel goes down: mirror the table |
| `board.phone_det`, `board.bt_strap` with and without an accessory plugged | what PHONE_DET does (question 24) | recorded only |

## 6. Data flash, the backup, the claim

| Look for | Proves | If not |
|---|---|---|
| `board.flash`: `EF4019`, 32 MB (0x2000000), `addr4` | the SPI4 bus, the 4-byte probe | The log has `flash: an erase or program is suspended or running: chip not used`: power cycle (question 22). No chip at all and no log line: the maker's firmware may have left it in power-down, which outlives an MCU reset and which the driver does not undo: power cycle. Another ID: `flash_cs7000p.c` accepts memory type 0x40 |
| `rfoundry_debug.py storage info`: `region   foreign` and the line `claim first: the data flash holds another firmware's data until the claim; back up all of it first (storage claim does)`. **This is the normal state of every unit**, also with `foreign_slots=0` on a blank ring | `STORAGE_CLAIM_FIRST`, and STORAGE INFO's flag that every client acts on | `region   ours` on a unit that never ran RadioFoundry: stop, the guard is not in the build. No `claim first` line at all: the tool or the build is older than the flag |
| Until the claim: a setting changed on the radio is gone after a power cycle, and a codeplug write is refused | nothing is written before the claim | |
| **The full backup: all 32 MB.** For the first radio with `rfoundry_debug.py storage claim --out DIR` (DIR outside any git tree: the tool refuses one inside), which reads each 32 KB block twice and compares; otherwise the web installer's **Back Up**, **Everything**. Keep the file, and keep it private. **Either client claims in the same step, at the end of a complete read**: there is no check between the backup and the claim | the backup the claim depends on (question 21) | The read stops early: neither client claims then. The installer says the settings area was not prepared; the tool says "nothing was claimed". Back up again |
| The claim has followed the complete backup by itself: `storage info` prints `region   ours` and `claim first: yes (region ours: nothing to claim)`, and settings survive a power cycle | the claim, the ring, the journal | Still `region   foreign` after a complete backup: the client's claim (the table below). A second `storage claim` on a region that is ours answers "nothing to claim" and exits 1: that is not a fault |
| After a restart, a second read of the upper 16 MB (the web installer's **Back Up**, **Everything** again; compare the memory chip's bytes of the two backups from 0x1000000 up: `spi-flash.bin` in the installer's bundle, the `dataflash_*.bin` of `storage claim`) matches the backup. This holds after the claim: nothing in RadioFoundry's map is at or above 0x1000000 ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.2), so nothing has written there | 4-byte reads, the extended address register put back | A mismatch above 16 MB only: the park after reads (`spi_nor_read_parked()`) |
| `board.store=foreign` | the reserved board sector at 0x01E000 still holds the maker's records ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 2.4): nothing of ours has written it, and it is not usable for the unit record | `blank`: the maker's records are gone from this unit (note it; compare the backup's 0x01E000-0x01FFFF). `ours`: something of ours wrote the sector: stop |

On this radio the claim opens the whole map to writes, so every client claims only after a whole-chip backup:

| Client | On this radio |
|---|---|
| Web installer, **Back Up**, **Everything** | reads all 32 MB, then claims; a read that stopped early claims nothing |
| `rfoundry_debug.py storage claim --out DIR` | reads all 32 MB through the CPS shim first, into one `dataflash_*.bin` (each 32 KB block twice and compared; minutes at USB speed), then dumps the ring, the journal and the calibration window and claims. A refused, short or differing block: "nothing was claimed", exit 1, no file left. `--skip-full-backup` leaves the whole-chip read out, and the tool says so |
| Codeplug Editor, **Write to Radio** | does not claim: it says to take the installer's backup of everything, opens the installer and writes nothing |
| Web installer, restore of channels from a bundle | does not claim, and says the same |
| Web installer's channel step, migration tools | claim nothing and write nothing on a claim-first board: they point to the backup of everything (this radio has no channel step anyway) |

Until the claim both whole-chip backups hold the chip's own bytes everywhere, the VFO A record at 0x7590 to 0x75C7
included: the firmware lays the live VFO over it only once the region is its own
([open-firmware-cps.md](../spec/open-firmware-cps.md) 2.2).

`test_storage_region` in `tests/hil/test_cps.py` skips on such a foreign region with its own text, which names the
whole-chip backup.

To map the maker's data (question 21): with the maker's firmware back on the radio, change one codeplug item with
the maker's software, return to RadioFoundry, back up again and compare the two files.

## 7. Battery, volume, VOX

| Look for | Proves | If not |
|---|---|---|
| `board.vbat_mv` against a meter at 6.0, 7.4 and 8.4 V, within 2 % | the battery model (question 13). Expected `board.pow_det_mv`: 1,560, 1,910, 2,160 | `CS7000P_BAT_GAIN_PPK`, `CS7000P_BAT_OFFSET_MV` |
| `board.vdda_mv` near 3,240 | VREFINT and the factory value | |
| `board.vol_raw` follows the knob; note its value at full volume | `VOL-DET` | `CS7000P_VOL_FULL_RAW` |
| `board.vox_raw` moves with speech into the microphone **only while keyed** (FM or DMR, into the dummy load, stage 11): the level detector's amplifier (`EXP_MIC_POWER`) and the microphone switch (`EXP_MIC_INT`) are on only during a transmission. While receiving it reads near 0. VOX itself is not written for this radio (`BOARD_NOT_WRITTEN`, `CONFIG_VOX` off) | the level detector on PC4 | |
| The LEDs: green, red, both for orange | `EXP_LED_GREEN`, `EXP_LED_RED` | |

## 8. HR_C6000 and I2S

| Look for | Proves | If not |
|---|---|---|
| `board.hrc`: init ok, 4:0x10 reads 0x80 in FM receive (0x6E in DMR) | the configuration port, the sleep line's polarity, the 29.4912 MHz clock (question 32) | 0xFF with init not ok is a silent bus: the sleep bit (`EXP_DMR_SLEEP`), the port's pins. The port's transfer always reports success, so the read-back is what finds a chip that does not answer. With init not ok there is no DMR and no FM transmit |
| 4:0x25 reads 0x0C while receiving (`board.hrc` `r25=0x0c`) | the reference hold was written; the register reads back (seen on one radio) | Another value: a set-up ran after the hold. With the hold missing the receiver sits about 10 kHz high at 433 MHz and the squelch never opens on channel |
| **Once, on a scope:** the configuration port's clock, 1 to 2 MHz | the bus delay on this core | |
| `board.i2s`: the two probe counts, and **`dir=ok` or `dir=swapped`** | the chip clocks its I2S port, and which pad the block drives (question 8) | **Anything else: stop.** `busy`: something drives PC11. `none`: neither sense drove the pad; the chip may not be the port's master, or the clock polarity is wrong (`ckpol` in `audio_cs7000p.c`). `unknown`: no clock seen yet. A scope on PC11 and PC12 confirms the verdict once |
| Then a beep from the speaker while receiving | PCM to the chip's line out, the amplifier and speaker switches | No beep with `dir=ok`: `EXP_AMP`, `EXP_SPK_INT_OFF`; underrun and overrun counts in `board.i2s`. Beeps are silent while the receive rail is off: a known limit |

DMR bursts go out with 4:0x25 = 0x0C (the hold stays through a DMR transmission) and FM transmit with 0x0E. In FM
receive the hold is measured, and 0x0E is measured on with analog transmit enabled; a DMR TX slot is not, and it
belongs to the reference measurements of question 1.

## 9. Receive

The signal generator on the antenna connector in place of the dummy load, through an attenuator that can take the
radio's full power. Nothing transmits (`board.cal=none`).

| Look for | Proves | If not |
|---|---|---|
| `board.pll`: locked on every channel across 400 to 527 MHz, the unlock count still | the plan, the bus, the lock pin, the VCO select (questions 3, 32) | Never locked: the bus (PD8 to PD10), `EXP_VCO_RX`'s polarity, `EXP_SYNTH_RAIL`. Locked on part of the band: note the edges and the tuning line at test point TP1 (0.5 to 3.9 V). Slow or flickering lock: the phase detector gain in `rfplan_cs7000p.c` |
| An SDR near the radio sees the receive LO at f - 49.95 MHz | the LO side of the plan | |
| `board.ak=ok` | only that the last set-up's frames were clocked out. The set-up writes and reads nothing back, so `ok` shows with the chip in power-down, in reset, deselected or missing | `fail`: a bus the driver could not use. Whatever it shows, the next row is the check |
| The AK2365A read-back: `rfoundry_debug.py reg 2 0x0B 0x5A`, then `reg 2 0x0B` read with `--page 0` and with `--page 1`; the same with 0xA5. Control 4 holds the AGC and a filter bit: a channel change writes the set-up again afterwards | the same value both ways, the one written: the chip is powered, out of reset and selected, and the read alignment holds (question 32) | Not the value, or different with the two pulls: `EXP_AK_RUN`, `EXP_AK_CSN`, the select level while A-RSTN (PC14) is released; or the read alignment (the open point in section 13). A rising `board.rssi_raw` with a generator (next row) is the second sign |
| A generator on channel: `board.rssi_raw` rises, `board.noise_raw` falls | **the first LO's side** (question 2), the receive rail, the front end | Nothing at f, with the read-back above passing: `rfoundry_debug.py reg 3 3 1` flips the side. If that is the answer, build with `CS7000P_LO_HIGH=1` |
| The carrier's leak or the receive LO on a counter while stepping 4:0x47, 0x48, 0x04 and 0x25 through `reg 0 ... --page 4` | **the reference oscillator's control** (question 1): which register moves it, the coding, the on-frequency level | Then `CS7000P_REF_OFFSET`, and the reference trim for the unit. Note the frequency against time after the synthesizer rail comes on (question 35): `CS7000P_REF_SETTLE_MS` |
| The same with `reg 3 2` at 0 %, 50 % and 100 % duty | PC8's pull on the reference (question 4): about 0.27 V of control voltage over the swing | The spectrum close in shows the PWM's ripple |
| `reg 3 0` swept at several frequencies for the best sensitivity. **Record the DAC code per frequency** | the front-end tuning voltage | The values go into the table in `rfcal_cs7000p.c`; meanwhile `CS7000P_RX_TUNE_NOMINAL` |
| FM audio from the speaker on a modulated carrier, wide and narrow | the chip's FM demodulation of the 450 kHz IF, the AK2365A's filter words, the IF level (question 9) | No audio with RSSI present: the I/Q order, `ctrl3` (the IF buffer gain), the 4:0x36 feed-through |
| RSSI and noise at -120, -110, -100, -80, -60 dBm. **Record them** | the mappings | `CS7000P_FE_GAIN_DB`, the squelch thresholds |
| CTCSS and DCS open the squelch; a wrong tone does not | the sub-audio filter on PA2, `EXP_AUDIO_RAIL` | CTCSS but not DCS: build with `CS7000P_DCS_RX_INVERT=1`, and check again after any change of `CS7000P_LO_HIGH`. `board.qt_raw` for the level. `CS7000P_DCS_INVERT` is for transmit |
| A sensitivity or noise floor scan across the band, CPU running | the 96 MHz bus clock's harmonic at 480 MHz, the 48 MHz harmonics (question 17) | note the channels |
| `board.gps_baud` outdoors | the GPS module's rate (question 23) | no answer at 9600 or 115200: the rate list in `gps_cs7000p.c` |

## 10. DMR receive

| Look for | Proves | If not |
|---|---|---|
| Sync on a DMR signal from the bench ([dmr-bench.md](dmr-bench.md)): the call's source and talk group show | the IF into the chip, the I/Q order, tones off | No sync with RSSI present: the LO side and I/Q order together (`reg 3 3`) |
| BER against level | the IF level, the reference's accuracy | `ctrl3`; question 1 |

DMR voice does not play: there is no codec. Data, text and the hotspot's receive side need none.

## 11. Transmit, only with the owner

Everything before this stage ran without RF. **Nothing transmits unless the test run passes `--allow-rf`**
([tests/hil/README.md](../../tests/hil/README.md)), and never without the dummy load.

The radio refuses every transmission while its tables are unset: `board.cal=none`, and `board.tx_refused` counts.
The one way the firmware keys it is the bench drive override, honored only under a test TX limit that lists the
frequency, in FM, for one transmission, within 60 s of arming
([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 13.2). A `reg soc` write to DAC1 or GPIOE is outside all of
that and is never made (Bench knobs, above).

| Step | Look for | If not |
|---|---|---|
| **The first key-up, at drive 1: `tools/power_cal.py --allow-rf MHZ key 1`** (dummy load, current-limited supply). **It transmits**: no receive-side step puts code 1 on the DAC or closes the loop. PA4 on a scope through the key-up, and the PA current with the loop closed and the transmit rail on (question 33). Not `reg 3 0 1`: that write is clamped to 250, which is also about 0.2 V on the pin and proves nothing about the floor | 0 V while the channel is off, before the loop line comes on; then about 0.2 V at code 1; no PA current at a zero reference | A current at code 1: that is the floor. `CS7000P_APC_FLOOR` |
| [power-calibration.md](power-calibration.md) with `tools/power_cal.py --allow-rf`, upward from the drive of the row above, on a current-limited supply into a dummy load, the PA current watched (question 5) | power rises with the drive; 1 W and 4 W inside the DAC's range | A cliff under some drive, or 4 W out of reach: note it. There is no reverse-power protection: never without a load |
| The carrier on a counter | the reference (question 1), with the receive offset | `CS7000P_REF_OFFSET`, the reference trim |
| A tone into the microphone, the deviation while stepping 4:0xE0 (question 7) | which codec input is the microphone; the modulation pair | No deviation at all: the microphone rail first (`MIC_5V` present while keyed: `EXP_MIC_POWER`, `EXP_MIC_INT`), then `CS7000P_E0_MIC`; the pairs in `rfcal_cs7000p.c` |
| CTCSS and DCS deviation and polarity | PC8's depth | `CS7000P_CTCSS_DEPTH`, `CS7000P_DCS_INVERT` |
| An FM key-up on a sagging pack (the bench supply's current limit low enough that the voltage dips under the key-up) | the key-up neither drops out as "knob off" nor restarts the radio: only the core's battery monitor or the time-out ends it. A closed switch reads far over the 0.7 V switch-open threshold | The radio goes off at the key-up: `board.pow_det_mv` during it, against `CS7000P_KNOB_OPEN_POW_DET_MV` |
| The measured values go into `rfcal_cs7000p.c`: the PA rows and the floor (`CS7000P_APC_FLOOR`) from `power_cal.py`, the FM and DMR modulation pairs, the reference offset (`ref_set`). All of them: `cs7000p_cal_tx_ok()` needs the PA row, the pair for the mode and the reference (hardware page 13.2). Rebuild with `CS7000P_BENCH_DMR_TX=1` and install | `board.cal=model`, a `tx_drive` in the core view that is not all 0, and the radio transmits FM without the override | `tx_drive` still 0: a PA point is still `CS7000P_CAL_UNSET` |
| DMR bursts: `tools/bench/pa_cal.py --radio PORT --bench HOST --freq MHZ --pad-db DB --max-w W --allow-rf` (question 25; `--pad-db` the pads between the radio and the SDR, `--max-w` the highest output measured above). It keys over the debug link, the bench gateway measures each burst's ramp; its open-loop keys use the drive override, which in DMR needs `CS7000P_BENCH_DMR_TX=1` | the burst runner and the sequencer on a radio, the key-up constants it reports | "the radio has no power calibration here": the step above is not done. `board.pll` unlock count, `board.dac` refusal count, `board.tx_refused` |
| Afterwards, as analysis only (neither keys the radio): `tools/bench/burstramp.py` on an I/Q capture of the bursts, `tools/bench/pa_shape.py` on `pa_cal.py`'s saved runs (`--out`) | the ramp shapes | |
| `pa_cal.py`'s constants into the board file; rebuild without `CS7000P_BENCH_DMR_TX`; the values on the hardware page | the radio transmits DMR without the override (no voice: no codec) | |

What the shared sequencer (`drivers/rf/chainseq`) assumes about this chain, each to check once:

| Assumption | Check | If not |
|---|---|---|
| Closing the APC loop on a 0 V reference gives no RF output | a spectrum analyzer on the dummy load while the loop line comes on with the DAC off: seen on the first key-up (stage 11's first row) | `CS7000P_APC_FLOOR` |
| The receive rail needs its settle time before the AK2365A's set-up on every receive set-up, a retune included | sensitivity right after a retune with the wait shortened | the board's `rail_settle_us`: `CS7000P_RX_RAIL_US` in `rfops_cs7000p.h` (a fixed define, edited there; `CS7000P_RAIL_SETTLE_US` is the start-up wait only) |
| After a DMR transmission the receiver comes back without an AK2365A set-up, although its LO was off during the bursts | sensitivity right after a DMR transmission against the same after a channel change | the board's `tx_end`: a full receive set-up |
| The lock line reads unlocked by the first read after new synthesizer words (three stale "locked" reads 100 us apart would pass a carrier that is still moving) | a scope on the lock line against the bus's last clock, at a receive to transmit change and at a retune | `CHAINSEQ_LOCK_READS`, `CHAINSEQ_LOCK_POLL_US`, or a fixed wait in the board's `synth` op |
| After a burst the tuning value is back on the shared DAC within about 100 us of the loop line going off. The transmit rail and the transmit VCO are off by then, but whether the APC amplifier's supply has fallen is open: until it has, the tuning value is a bias request to the PA | supply current and the PA gate voltage at the end of a DMR transmission | the board's `tx_end`: a full receive set-up, which waits |
| A key-up boost comes at or after the loop point; an earlier one is refused and the burst is dropped | `board.dac` and `board.tx_refused` after bursts with measured key-up constants | the key-up constants the board returns |

## 12. Later, each once on the bench

| Check | Question | Knob |
|---|---|---|
| The software entry to the ROM bootloader: the loader's version from `board.rom_loader`; whether the HR_C6000 releases its data output while deselected (PB14 against the loader's pin); PB10 with the selector in each position; whether the request survives in the backup register | 18 | On by default (`CS7000P_SOFT_ROM_ENTRY`). On a loader other than 0x91 with PB10 low (`board.chsw_raw` bit 2 clear), `reboot --rom-dfu` is refused (reason 5) before anything happens: turn the selector four detents either way. If the request is lost, the tool's after-OK hint says the request could not be stored and nothing is armed. SK1 at power-on stays available |
| The same entry through the tools, once the row above passed (untested: neither has restarted a radio yet). With RadioFoundry running and the cable in: `rfoundry flash IMAGE --board cs7000p` prints "restarting it into its bootloader over the debug link", finds 0483:DF11 within 20 s ("in its bootloader: leave it on and connected") and installs as from SK1. The web flasher: **Update**, connect, **Restart into Update Mode**, then **Connect** and the browser's device list. On a selector position that grounds PB10 both say the radio can't right now and show the hint (turn the channel selector four clicks either way and try again); rfoundry on a terminal asks "Try the restart again?", the page keeps its button, and SK1's steps follow. Record: the seconds until the loader is on USB, both outcomes on both tools, and that the install's lines after "found bootloader" are those of an SK1 entry. Without a radio: `rfoundry flash --board cs7000p --simulate rfoundry-nocodec` (taken), `--simulate rfoundry-refuses` (refused), and the flasher at `#sim=cs7000p&update` (refused once, then taken) | 18 | `restart` and `restart_hint` in `boards/cs7000p/board.toml` `[flash.bootloader]`. If the entry does not hold on a radio, take `restart` out: the tools then ask for SK1 as before |
| The VCAP capacitors' real value | 15 | none: the port stays at VOS3 |
| What the pack puts on BAT_DET; IC611; the unlabeled expander output | 24 | recorded only |
| A watchdog reset with SK1 held and the knob off; a power-on with SK1 held and the knob turned off in the loader, over several cold starts (does IC621 come up with the hold set?) | 29 | none: the pack comes off |

## 13. Unmeasured constants in the code

Not knobs: values the board's and the platform's files carry that no source gives, or that come from a data sheet and
have not been seen on this radio. Each has the symptom that says it is wrong.

| Constant | Value | Where | Symptom, or how to check |
|---|---|---|---|
| Display write strobe | 100 / 100 ns | `display_cs7000p.c` | a garbled picture: lengthen it, or raise the data lines' slope |
| Display read strobe | 1000 / 1000 ns | same | `id` and `reg` of `board.panel` differ or change between starts (all `00` is a panel that does not answer: stage 5) |
| Reset before the ID read | low 20 ms, then 120 ms | same | ID `000000` on a panel that then shows a picture |
| Keys-up wait before the ID read | 500 ms | same | a key held through power-on: the log line of the hardware page's 9.3 |
| A full flush's time (40,960 bytes, each with two cycle-counter waits) | not measured | `platform/stm32/lcd8080_gpio` | time a full redraw over the debug link; the UI must stay responsive |
| Key scan settle | 20 us | `input_cs7000p.c` | phantom keys in the row after a pressed key (`board.keys_raw`) |
| Selector settle | 30 ms | `input_cs7000p.c` | steps while turning slowly: a third code read between two detents |
| HR_C6000 receive gain 2 dB (4:0x37), safe DMR modulation pair 0x80 / 0x80, microphone gain 7 | as the tree's other HR_C6000 boards | `c6000_cs7000p.c` | stages 9 to 11 |
| SKY72310 phase detector gain | 31 | `rfplan_cs7000p.h` | another board's value for the same part: lock time, stage 9 |
| Lock time-out 5 ms, receive rail 1 ms (`CS7000P_RX_RAIL_US`), transmit rail lead 500 us | | `rfops_cs7000p.h` | unlock counts in `board.pll`; stage 11 on a spectrum analyzer |
| AK2365A: 1 ms out of power-down, 500 us from the mode to the calibration's start (`CS7000P_IF_MODE_US`, the data sheet's), 3 ms after the set-up | | `radio_cs7000p.c`, `rfplan_cs7000p.h` | a deaf receiver after a channel change, or a failed 0x0B read-back (stage 9) |
| I2S clock edges `ckpol = false` | | `audio_cs7000p.c` | distorted PCM audio with `dir=ok` |
| The sub-audio detector's smallest amplitude | the driver's default (8 counts) | `subtone_cs7000p.c` | a guess until the level on PA2 is measured (`board.qt_raw`) |
| The RF TX interrupt leads the first symbol by 2550 us; the PA drops 27.5 ms after the first symbol | | `rfops_cs7000p.h`, `platform/stm32/dmr_keyup` | SDR bench, `tools/bench/burstramp.py`: the carrier is up at the first symbol and off inside the guard time |
| The one-shot timer's handler latency; whether a 30 us wait is served | not measured | `platform/stm32h7/periph/oneshot_tim7.c` | the same capture over several bursts |
| ADC: 2,400 scans a second from TIM6; the calibration bounded at 50 ms | | `platform/stm32h7/periph/adc_scan.c` | a known CTCSS tone decodes; `board.adc_restarts` stays 0; all readings 0 on a running radio: the scan has stopped |
| VDDA from VREFINT | about 3,240 mV expected | same | `board.vdda_mv=0`: the measurement failed or was outside 1,600 to 3,700 mV, and the nominal figure is in use |
| PC8 PWM 93.75 kHz at 50 %; backlight PWM 40 kHz | | `board.c`, `display_cs7000p.c` | scope on PC8 and PC9. No backlight output at all: the timer's `MOE` |
| The mask time of a bus frame: about 0.12 ms for an AK2365A frame (six in a receive set-up), about 36 us for a synthesizer word | computed | `radio_cs7000p.c` | a modem interrupt in a DMR receive set-up is served that much late: measure against the slot engine's margins |
| The SK1 wait before a software reset: released for 10 ms in a row, at most 30,000 steps of about 1 ms (`CS7000P_SK1_UP_MS`, `CS7000P_SK1_WAIT_MS`) | computed; several ms a step without the cycle counter (a fault before the time base started) | `romentry_cs7000p.h`, `exp_cs7000p.c` | stage 4's reboot with SK1 held |

Open points found while the board was written, beside the hardware page's 36 questions:

| Point | Check |
|---|---|
| What ROM loader V9.2 (0x92) does with PA2 and PB10 before its USART is detected: AN2606 Table 145 names 0x91 only. Until it is known the gate treats 0x92 like the old loaders | stage 12, with `board.rom_loader` |
| The ROM loader's `wTransferSize` and how it places block N after one Set Address Pointer: in no document. Neither tool depends on it: every block is block 2 after its own Set Address Pointer | stage 1's descriptor record; stage 3's read-back |
| The AK2365A read alignment: `chip_reg` 2 returns bits 8 to 1 of ten samples | stage 9's 0x0B read-back |
| PCM playback in DMR mode: the board writes no codec register, and whether 4:0x22 bits 1 and 2 must be set again after the DMR entry is not known | a key beep on a DMR channel |
| The microphone in DMR: on from the arm (`EXP_MIC_POWER`, `EXP_MIC_INT`) to its end. Nothing uses it yet (no codec). Once something does, a silent DMR transmission: the microphone rail first (`MIC_5V` present while armed), then the codec input bit (`CS7000P_E0_MIC`) | `MIC_5V` on a meter while a DMR transmission is armed (stage 11) |
| The battery while keyed: the transmit check reads `POW_DET` once, before the key-up. Only the core's monitor ends a transmission when the battery sags | stage 11 |
| A safe request (`board_rf_safe()`) leaves the HR_C6000 in the mode it was in, the FM transmit set-up included, until the next `tx_off()` or status poll: the reference oscillator sits on the transmit set-up's control level with the PA off | a frequency counter on the reference after a forced safe request |
| `status()` is where the receiver is restored after a safe request and where the bench changes `chip_reg` 3:3 and 3:4 are applied. If the core stops polling it in some state, they wait for the next `rx()`. In the power's off state it does not restore | by reading the code paths once the radio runs |
| The burst runner's key-up under a bench drive uses the drive as its own top | no difference with the fixed key-up constant; matters once the constants are measured |
| After an underrun or overrun the two I2S streams may be a few samples out of step until the next audio stop and start (RM0433 50.9.12, 50.9.13; an overrun with an early frame in one frame, 50.9.14) | `board.i2s` `und=` / `ovr=` over a few minutes of audio: 0 expected |
| STM32H743 revision Y only: ADC and DAC conversions at the same time may cost ADC accuracy (ES0392 2.9.7), and 12 bits are not guaranteed with VDDA above 3.3 V (2.9.10) | `board.mcu_rev` |
| Port C's pin modes are set by a read-modify-write that is not atomic (`platform/stm32/gpio.c`), from three tasks: the audio task (PC11, PC12, the I2S pads), the GPS task (PC7) and the radio task (PC13, the AK2365A's data line, under the interrupt mask). A GPS or audio task cut by the radio task's masked write can write PC13's old mode back: PC13 an input in an IF chip write (a frame lost unseen: a wrong filter or a deaf receiver until the next receive set-up) or an output after a frame. The backlight's PC9 is set up before any task and is off this list | the RSSI and noise readings, or a 0x0B read-back, after many receive set-ups with GPS and audio running; open in the shared code (an owner's decision) |

## Every assumption, by question number

One row per question of [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 16: what the port assumes, where it is
checked above, and what to change.

| # | Assumption in the port | Stage | Change |
|---|---|---|---|
| 1 | Reference offset 0 (the hold value 4:0x25 = 0x0C is measured in FM receive); the DAC in a DMR TX slot | 9, 11 | `CS7000P_REF_OFFSET`, `rfcal_cs7000p.c`, the reference trim |
| 2 | First LO below the signal; I/Q order to match | 9 | `reg 3 3`, `CS7000P_LO_HIGH` |
| 3 | Phase detector gain 31, lock within 5 ms, three agreeing samples of the lock pin, lock over the whole band | 9 | `rfplan_cs7000p.c` |
| 4 | PC8 idles at 50 % duty, 93.75 kHz | 9 | `reg 3 2` |
| 5 | No PA value at all: tables unset, transmit refused | 11 | `rfcal_cs7000p.c` |
| 6 | The PA stays off while the expander is undefined, through R141 | 4 | none in firmware |
| 7 | The microphone is the codec input of bit 1 (0x02) | 11 | `CS7000P_E0_MIC` |
| 8 | Nothing: the I2S direction is probed. Clock polarity low; the chip is the master | 8 | `ckpol` in `audio_cs7000p.c` |
| 9 | The chip demodulates FM from the IF; IF buffer gain 6 dB | 9, 10 | the AK2365A words in `rfplan_cs7000p.c` |
| 10 | The LCD board is the 2018 OEM board; an ST7735-class controller; 160 x 128, offsets 0 | 5 | `CS7000P_LCD_FORCE`, `CS7000P_PANEL_TYPE`, `CS7000P_PANEL_MADCTL_XOR` |
| 11 | The selector's codes and direction and the six function keys' order, read on one radio (2026-11-22), hold on another | 5 | `keys_cs7000p.h`, `CS7000P_CHSW_VERIFIED` |
| 12 | Backlight window 20 to 70 % duty; key lights on with the bit high | 5 | `CS7000P_BL_MIN`, `CS7000P_BL_MAX` |
| 13 | The battery model; the knob reads off below `POW_DET` 0.7 V (`CS7000P_KNOB_OPEN_POW_DET_MV`); the hold works at low battery | 4, 7, 11 | `CS7000P_BAT_GAIN_PPK`, `CS7000P_BAT_OFFSET_MV`, `CS7000P_KNOB_OPEN_POW_DET_MV` |
| 14 | Either silicon revision; any ROM loader version (0x90 may need a second reset); read protected as shipped | 1, 3 | recorded. Unprotected as found: stage 1's readout before stage 2 |
| 15 | The core supply is good at VOS3 | 12 | none |
| 16 | The crystal starts without bypass; USB works from a PLL with a 1 MHz input | 3 | the clock table in `board.c`. A table with a 5 MHz input that stays in VOS3: `pll_m` 5, `pll_n` 144 (VCO 720 MHz, wide range), `pll_p` 4 (CPU 180 MHz), `pll_q` 15 (48 MHz), `ahb_div` 2 (HCLK 90 MHz), `apb_div` 2 (45 MHz, timers 90 MHz), `flash_latency` 1. With it `board.c`'s static asserts (the medium VCO, two wait states) change, PC8's period follows the timer clock (87.9 kHz: `subpwm_cs7000p.h`), and question 17 is checked again (5 x 90 = 450 MHz). The platform never changes the voltage scale, and question 15 is against a higher one |
| 17 | The bus clock's harmonics don't desense the receiver | 9 | the clock table |
| 18 | The software ROM entry reaches the loader and harms nothing: allowed only with loader 0x91 or PB10 high; not yet tried on a radio. rfoundry and the web flasher use it for an update (`[flash.bootloader] restart`) | 12 | `CS7000P_SOFT_ROM_ENTRY`; `restart` in `board.toml` |
| 19 | The ROM's Leave starts the image; an erased first sector starts nothing | 3 | the tools already ask for a power cycle |
| 20 | Nothing unit-specific is in the processor's flash | 0, 2 | none: the codeplug saved (and the tuning data, if the maker's tuner can export it), the maker's recovery, the codeplug written back, before the install |
| 21 | Nothing is known about the maker's data on the flash chip: nothing is written before the claim | 6 | `STORAGE_CLAIM_FIRST` stays |
| 22 | The flash chip may be in any address mode; no block protection; the write enable latch state after `C5h` does not matter (the driver sends `04h` either way) | 6 | `drivers/flash/spi_nor` |
| 23 | The GPS module talks NMEA at 9600 or 115200 | 9 | the rate list in `gps_cs7000p.c` |
| 24 | PHONE_DET, BAT_DET, IC611, the spare output, Bluetooth, the accelerometer and the motor are not used | 5, 12 | none |
| 25 | `chainseq` and `dmr_keyup` behave on a radio as in their host tests; key-up lead 800 us | 11 | `tools/bench/pa_cal.py`, `radio_cs7000p.c` |
| 26 | The shared STM32 code behaves on an STM32F4 radio as before the split | not on this radio | one STM32F4 radio through the HIL tests |
| 27 | `aprs_tocall = "C"`, `order = 8`, CPS radio type 7 are free | not on a radio | `board.toml` |
| 28 | The C-SKY builds, `reuse lint`, the HIL collection and the site build pass | not on a radio | CI |
| 29 | The radio stays on with the knob off only after a watchdog reset with SK1 held, or before RadioFoundry has run since power-on (loader mode, an install cut short); a start task blocked for good is a firmware fault (hardware page 5.3) | 0, 4, 12 | none: pull the pack |
| 30 | The ROM bootloader may not write a last partial flash word: the tools pad | 3 | use rfoundry or the web flasher |
| 31 | The second LO's error is small against the IF filter | 9 | none |
| 32 | `DMR_SLEEP` high = asleep; `A-PDN` high = running; the AK2365A's select high while its reset is released; `VCOVCC_SW` high = receive VCO | 8, 9 | one bit each in `exp_cs7000p.h` |
| 33 | "DAC 0" is the channel disabled; the floor is 0 | 11 | `CS7000P_APC_FLOOR` |
| 34 | The chain's and the buses' timing as computed; the chain's bit order as drawn | 4, 8 | `EXP_DELAY_PASSES`, the half periods in `radio_cs7000p.c`, `exp_cs7000p.h` |
| 35 | The reference has settled 30 ms after the synthesizer rail | 9 | `CS7000P_REF_SETTLE_MS`, `CS7000P_RAIL_SETTLE_US` |
| 36 | Two backup-domain pads as outputs do no harm at 250 kHz behind 1 k | 9 | none |
