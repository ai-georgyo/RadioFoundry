<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# CS7000 M17 PLUS bring-up checklist

The CS7000 M17 PLUS port (`make BOARD=cs7000p`) builds and passes its host tests. **One radio has run it** (from
2026-11-22): it starts, receives, plays received DMR voice through the codec (2026-11-23), and has its maker's table
back in use (2026-11-24). What was read on it is in [First radio](#first-radio) below. No key-up has been made on any
radio. Everything else is from the maker's
schematic and the chip makers' documents ([hardware-cs7000p.md](../spec/hardware-cs7000p.md)). This page is the
order to try it in, every assumption the port makes with the check that settles it, and the knob to change when a
check fails. A row is untested unless First radio lists it: each says what a radio should show, not what one did.
The question numbers are those of the hardware page's section 16.

The stages are in order, and nothing that cannot be undone comes before the checks that gate it: the codeplug saved
and the build (0), the loader identified with a USB capture (1), the maker's recovery proven on this unit (2; it
erases and reloads the maker's programs itself), and only then the one-way Read Unprotect install (3). Nothing
transmits before stage 11. Its bench route (11.2) never runs without a dummy load; its route on the maker's table
(11.1) transmits into whatever is on the antenna connector, and running it without a load is the owner's decision.

The feature checklist, with every feature's required HIL tests and the last recorded result, is
`tools/bringup.py list --radio cs7000p`. `tools/bringup.py run --radio cs7000p` records a run
([tests/hil/README.md](../../tests/hil/README.md#bring-up-mode)). `boards/cs7000p/hil/test_cs7000p.py` checks the
view keys below on a radio and never transmits.

## Status

| Item | State |
|---|---|
| Build, host tests, stack check | pass |
| On a radio | one unit: starts; display, keys, the channel selector, beeps, USB, the debug link and settings storage work; FM receive with clear audio; DMR calls are received, decoded and played with the merged codec. The maker's table, lost to an earlier build, restored from the owner's backup with the tool (stage 6), in use for receive (9.1 step 1) and kept through one Codeplug Editor write; the codec still in place after two updates with the web installer ([First radio](#first-radio), 2026-11-24). Every transmission: not checked |
| Transmit | written, never run on a radio: no key-up has been made on any radio, and RADIO op 44 (`radio tx-arm`) has not run on one. On the maker's table (restored in stage 6): refused until the owner arms it over the debug link, in stages (stage 11.1). On the model tables: refused until the PA and the modulation are measured (stage 11.2) |
| DMR voice | needs the codec merged (stage 10.1). Receive works on the first radio. Transmit: a DMR transmission as any other, so on the maker's table only at the arm's top stage (`dmr`, stage 11.1); never run, and the codec's encoder has not run on a radio |
| VOX | not written (`BOARD_NOT_WRITTEN`): the level detector has no supply while receiving |
| Records | `docs/testing/results/cs7000p/seed.json`, with no feature row: no HIL test has run on the radio, and the readings below are by hand |

## First radio

One unit, from 2026-11-22 (the maker's table from 2026-11-24), by its owner; readings through `rfoundry_debug.py view
--section board` unless said otherwise. This radio went straight to the install: stages 0 to 2 were not run on it as
written. The unit's own values (CRCs, trims, tune codes, thresholds) stay on its debug link and out of this page.

| What | Seen | Settles |
|---|---|---|
| The first install | Through the ROM bootloader with the web installer; the image starts. `board.rdp=0xaa` afterwards | the route as a whole; the reset path, the power hold, the clock |
| `board.clock=hse`, `board.mcu_rev=V`, `board.rom_loader=0x92` | the crystal's tree; silicon revision V; ROM loader 9.2 | questions 14 and 16 (in part: not measured before the install) |
| `board.lcd_board=assumed levels=0x1e`, `board.panel=st7735 source=id id=7c89f0` | the board check passes; the panel answers its ID; the display works | question 10 |
| Keys | the twelve digit keys, \*, # and the side keys as drawn; the six function keys read one at a time (`keys_raw`, hardware page 9.4) and their order corrected | question 11's key half |
| `board.flash=EF4019 size=0x2000000 addr4=1`, `board.store=foreign` | the data flash answers; the board sector, then at 0x01E000, holds data that is not ours, the maker's records ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 2.4) | stage 6's first rows |
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
| The full backup and the claim | the web installer's Back Up, Everything read the data flash and claimed the settings region; settings then survive a power cycle and a codeplug can be written | stage 6: the claim-first order, storage on this flash (question 21 stays in part: the maker's codeplug, fonts and voice data are not located, the backup holds them) |
| The passband against another radio's carrier on 433.500 MHz, 500 Hz steps, wide filter | `noise_raw` under 800 from -6.0 to +6.5 kHz: the center 250 Hz high, inside the sweep's resolution and the other radio's own error. The reference offset stays 0 | question 1's level: mid-scale is on frequency to a few hundred Hz on this unit |
| DMR receive, a handheld's group call on 433.500 MHz, color code 1, simplex, 5 feet away | Synced at once: `dmr.rx_cc=1`, `call.src` and `call.dst` decoded, 222 voice bursts in 13.4 s (none missed), 666 voice frames, the call ended by its terminator. `voice.rx_bit_errors` 132 and `voice.rx_repeats` 44 over the call, at a signal strong enough to overload the receiver. `board.hrc` read `r10=0x6e`; `board.i2s` went to 1 for the call. No audio: no codec | question 2's I/Q order, question 9's IF level for DMR, the HR_C6000's three interrupt lines and its DMR set-up; the error rate at a normal signal level is not measured |
| A key press | beeps | PCM to the chip's line out, the amplifier and the speaker switch (stage 8's beep row) |
| The six function keys with the corrected order | each does what its label says | question 11's key half |
| FM receive with the level fix (the bias at mid-scale, written in one step) | the squelch opens on a signal on channel, also after minutes of idling; the audio is clear and at a sensible level. An image that wrote the level through zero at every receive entry opened the squelch after a start and later stopped | question 1 except the fine offset; question 9's audio |
| An update from the web installer | failed once with "not this radio's update mode (it reports ?)": the browser had no alternate setting names for the loader. The page now reads them from the device. An update with `rfoundry flash` from a loader entered by hand worked | the page's device check |
| The data flash's 0x001000-0x0011FF, read on 2026-11-23 against the whole-chip backup taken before the claim | all 0xFF on the radio; the backup holds the maker's factory table there ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 2). A codeplug write made with an earlier build, which wrote whole sectors of the EEPROM image, replaced it. The radio is uncalibrated as before (nothing used the table), but the maker's firmware would transmit on the blank bytes | why the table's bytes are kept now and why the restore exists ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.3; stage 6's `storage factory` rows) |
| The codec, installed by the web installer with the maker's file V9.00.96 (2026-11-23, image e54fc89a) | `rfoundry probe`: `codec cs7000p-v90096-s7` and "the codec can be kept"; `dmr.codec=1`, `voice.enc_supported=0x7`. `peek 0xE000ED14` gave 0x00060200 straight after the install, with no power cycle (both trap bits clear after the ROM loader's jump). No `tasks.free.audio` line 137 s after the start, before any sound; 4936 after a key beep | stage 10.1: the web installer's codec step (the file recognized, the merge in the browser, sector 7 written through the ROM bootloader) and the boot check. `rfoundry flash --stock` has not written a codec to a radio |
| DMR voice receive with the codec: three group calls from a handheld on 433.500 MHz, about a minute | **Good audio the whole time.** 1650 voice bursts, `voice.rx_frames=4950`, `rx_lost`, `rx_concealed`, `rx_bad` and `rx_underruns` 0, `dmr.underruns=0`, `voice.rx_queue_max=3`, `voice.rx_bit_errors=960`, `voice.rx_repeats=73`, `voice.rx_peak=651`. `tasks.free.audio=2560` afterwards, `tasks.free.dmr=1564`, `tasks.free.ui=604`; `crash.valid=0` | stage 10.2 except the silence frame: decode from the placed address, no fault, the stack, decode in time |
| Image 1 (2026-11-24): the port's state at `be543708` (the kept bytes, the reader, the copy, the restore), built on this branch (the port with the codec route), installed with the web installer over the running RadioFoundry. `view` sections `board`, `board2`, `storage` | `board.flash ... refused=0`, `board.store=blank`, `board.cal_table=blank src=none why=blank` with the `crc=` of 512 bytes of 0xFF, `board.cal_rec=blank wrote=0 restore=none`, `storage.region=ours`, `storage.factory=0,0,0,0`: stage 6's prediction for a lost table | the reader refusing a blank window and nothing copied from it; the board sector blank; no write refused by the driver |
| `storage factory --backup BACKUP` on image 1, with the first backup (the whole chip) | `window blank`, `window / file   differs`, no record; the maker's records at 0x01E000-0x01FFFF the same on the radio and in the backup (the identity check passed); exit 1 | stage 6's compare and its identity evidence on a real backup |
| `storage factory --backup BACKUP --restore` on image 1 | accepted; `result ok`: the window holds the file's bytes and the record was written; exit 0. The compare after it: the window `ok`, `in use: window`, `window / file   same`, `record / file   same` (CRC-32); exit 0 | the one restore path end to end: ops 4 and 5, the kept rewrite of sector 1 with the backup's bytes, the read back, the record at 0x01B000 |
| Image 2 (2026-11-24): the port's state at `6153f422` (receive on the table, the arm's handshake), with the same codec route; installing it restarted the radio. `view --section board2` | `board.cal_table=ok src=window why=none fields=0x1fff`; `board.cal_rec=ok wrote=0 restore=none` with the window's CRC-32 | the reader accepting a real maker's table with every field group; the window and the record kept through an update and a restart; the source order (the window first) |
| The receive side on image 2 | `board.ref=hold` with `bias=` 0x200 + `d_a` (trim A inside the receive bound), `r04=0x00`, `src=maker why=none`; `board.rx_cal=maker rssi=maker vco=low vco_rule=freq ifw=port`; `board.rx_tune` the table's row, not 2048; `board.ref_offset=0`; `board.sq_maker=ok` with its four thresholds; `board.hrc=1 r25=0x0c` (`r10=0x6e`, read on the DMR channel); `board.tx_arm=off id=0x00000000 list=0`, `board.tx_refused=0 why=0`; `storage.region=ours`, `storage.factory=0,0,0,0` | 9.1 step 1's "after": receive takes the tune row, trim A and the RSSI points from the table, the hold stays 0x0C, the VCO line follows the frequency rule at 433.5 MHz, the maker's thresholds are read and shown, the arm is off after a start |
| FM receive on 433.500 MHz on the table (image 2) | clear audio, as good as before, by ear | the tune row and trim A no worse by ear on one frequency; the A/B with readings (9.1 step 2) was not made |
| A DMR group call on image 2 | decoded, and played with good audio through the codec, still in place after both updates. The signal strength of the same strong signal read a few dB lower than on the data sheet's line, as the table's scale predicts | DMR receive on the tune row and trim A; the table's RSSI points in use; the codec through two updates with the web installer |
| A codeplug write with the site's Codeplug Editor (**Write to Radio**) on image 2: the kind of write that lost the table under the earlier build | `storage factory --backup BACKUP` `same` before and after it: the table stayed | the table through the editor's write. The editor sends no byte of the kept range itself (`[[flash.protect]]`), and which of its two protocols carried the write is not recorded: the firmware dropping a tool's bytes for the range is not shown by it |

Not checked on it yet: the reference against a counter (only against another radio), sensitivity in FM and DMR, the
DMR error rate at a normal signal level, DMR data and text, CTCSS and DCS, the battery against a meter, `CCR` after a
power cycle, and everything of stage 11: no key-up of any kind has been made on any radio, DMR voice and the codec's
encoder included, and RADIO op 44 (`radio tx-arm`) has not run on one. Of stages 6 and 9.1: the web installer's
factory step (the table was restored with the tool), any refusal of the restore, the record written by the firmware
itself at a first claimed start (the restore wrote this radio's), a codeplug write by `tools/cps_client.py`, by TFTP
or known to have gone through the fast codeplug protocol, the resume of either at a start, a DTMF or APRS edit after
the restore, the journal's replay after a power cut, 9.1's A/B of today's values against the table's with readings
(steps 2 and 3: `reg 3 0`, `reg 3 4`), the reference coding (step 4, `reg 3 5`), the maker's IF words (step 5,
`reg 3 7`), the RSSI line knob (`reg 3 8`), a `CS7000P_RX_MAKER=0` build, and the HIL tests.

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
| `cs7000pRadioTest` | `radio_cs7000p.c` over the real chain: the microphone in FM and DMR, the off state: no receiver restore, no amplifier (also behind a speaker call the power-off cuts into), no transmission in FM or DMR; `chip_reg` 3:5: RAM only, a receive set-up, every key-up refused; 3:6's rule in `rx_cal`; the owner's transmit arm: its handshake against each source of the table, every refusal (a bench PC8 idle and a stored reference trim among them, and `why=16` for a trim stored while armed), the bench's writes to the modem, the synthesizer and PC8 refused while armed, the bench drive and the arm refusing each other (`why=18`, the unarmed bench key-up's writes pinned), the restore refused while armed and the arm while a restore is pending; armed and listed, FM key-ups on the table's drive, pair, trim A and registers, tone channels at the depth of the table's level (a tone without one refused), DMR key-ups at the top stage on a split channel (the DMR pair at f_tx, trim B in 4:0x04, in DMR receive on the top stage only); a DMR voice key-up as the core asks it with the codec in place: refused unarmed, at both FM stages and without a list, keyed only at the top stage with one |
| `cs7000pRadioSeqTest` | the RF sequences on a model of the chain, partial calibrations, the bench drive at a measured floor, a safe request in the middle of a key-up at floor 0, the off state's refusal (`why=12`), the reference coding's (`why=14`), the transmit arm's (`why=13`, `why=15`) at every level, mode and stage with nothing admitted without both, and `why=16` (a trim stored after the admission included), `why=17` and `why=18` (a bench drive at every armed stage, the key-up constants) at the top stage; armed and listed, FM on the table's drive at f_tx (pinned), tone channels at their level's depth (pinned), a tone without a level refused, DMR at the top stage on the table's drive, DMR pair at f_tx and trim B with bursts under the chain's rules, the switch window and extension refusals, the arm lowered before the key-up or the bursts, a lower stage ending a covered transmission |
| `cs7000pRfTest`, `cs7000pCalTest` | the frequency plan, the band edges, the calibration verdict, which field groups a key-up on the maker's table needs, a tone's level from it |
| `cs7000pPinsTest`, `cs7000pPanelTest`, `cs7000pKeysTest`, `cs7000pSpeakerTest`, `cs7000pSubPwmTest`, `cs7000pStoreTest` | the pin set-up and its never-driven list, the LCD board check and the panel ID, the keys and the selector, the speaker lines, PC8's duty in per mille and the maker's level as a depth (and the old depth option's build failure), the flash map |
| `cs7000pCodecTest` | the codec's boot check with the board's own profile (`vocoder_cs7000p.c`) on a sector 7 that holds a synthetic codec in the real layout: the descriptor, both CRC-32s, the first word, the entry offsets; a byte changed in the code, in a blanked span, in the block or in a gap fails it. It takes sector 7 alone or a whole merged image (its last 128 KB). With `pinned` as its second argument the file is merged from the maker's file, outside the repository, and the test also asks for the two pinned CRC-32s |

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
| No transmit calibration in use: every transmission is refused until the PA and modulation values are measured or taken from the maker's table (read and shown, not used for transmit) | 11 | 5 |
| The whole-chip backup is complete (no claim before it) | 6 | 21 |
| The radio's own codec, merged into sector 7 from the maker's file V9.00.96, passes the boot check and runs from its placed address; the audio task's stack holds it | 3, 10.1, 10.2, 11 | [codec-cs7000p.md](../spec/codec-cs7000p.md), "Only a radio can check" |
| A flash word that a power cut left half written reads as a bus fault. Never tried, and not to be tried: the installers' rule stands in for the test | 3 | hardware page 2.3 |

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

For DMR voice the install needs the maker's DMR program file, `M17_PLUS_HOST_V9.00.96_20260303_1.bin` (V9.00.96, SHA-256
`1a4bdf38fed52dc50c738a85f8ce54f6384c070c6a08af21ca919a547b7e5a59`; the maker's software page for this radio). Two
ways, with the same bytes on the radio:

| Way | How |
|---|---|
| The build merges | With the file in `codecs/`, `make BOARD=cs7000p` also writes `build/cs7000p/rfoundry_cs7000p_codec.bin`, 1048576 bytes: the image, 0xFF, the codec in sector 7 and its descriptor ([codecs/README.md](../../codecs/README.md)). Flash that file |
| rfoundry merges | `rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --stock M17_PLUS_HOST_V9.00.96_20260303_1.bin`: merged in memory and written |

The file and the merged image hold the maker's codec: keep them out of the repository and to yourself. V9.00.95,
V9.00.09 and V9.00.05 are recognized and refused.

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
| `CS7000P_RX_MAKER` | 1 | 1: receive takes the tune, the reference level and the RSSI scale from a valid maker's table. 0: today's values whatever the table holds (`board.ref ... why=build`, `rx_cal=default rssi=sheet`); the table is still read, shown and copied. The lasting way back from stage 9.1, where the knobs are RAM only |
| `CS7000P_FE_GAIN_DB` | 0 | The front end's gain, for the RSSI figure in dBm |
| `CS7000P_E0_MIC` | 0x02 | The 4:0xE0 codec input bit forced after the DMR entry and the FM transmit set-up |
| `CS7000P_APC_FLOOR` | 0 | The largest DAC code that gives no RF output: the interlock's threshold (`exp_cs7000p.h`), and the calibration table's floor when the build gives it (`rfcal_cs7000p.c`): the two cannot differ |
| `CS7000P_RAIL_SETTLE_US` | 2000 | The wait after the synthesizer rail comes on at start-up (`cs7000p_rf_start`). The receive set-ups wait the sequencer's `rail_settle_us` instead, `CS7000P_RX_RAIL_US` (not a knob, below) |
| `CS7000P_REF_SETTLE_MS` | 30 | No transmission and no frequency measurement before this long after the synthesizer rail came on |
| `CS7000P_CTCSS_DEPTH_PM` | 300 | PC8's swing at the generator's full scale, in per mille of half the PWM period (`subpwm_cs7000p.h`), on the model's rows and the bench drive. A first value: the deviation it gives is unmeasured, on 12.5 and on 25 kHz channels alike (the same swing on both). Armed on the maker's table a tone channel takes the table's level instead (hardware page 13.2). The old `CS7000P_CTCSS_DEPTH` (a percent) fails the build |
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
3:2 PC8's idle duty, 3:3 the LO side, 3:4 the reference offset (0x100 releases it), 3:5 the reference DAC's coding
(receive only), 3:6 the receive VCO rule, 3:7 the IF chip's words (receive only), 3:8 the RSSI scale (the table's
points or the data sheet's line). None of them is stored: a restart
puts each back to its default. Chip 0 is the HR_C6000, 1 the SKY72310 (write
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
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --stock FILE --dry-run  # the plan, nothing sent
rfoundry flash build/cs7000p/rfoundry_cs7000p.bin --board cs7000p --stock FILE --unprotect
```

FILE is the maker's `M17_PLUS_HOST_V9.00.96_20260303_1.bin` (Build, above). The Read Unprotect erases the codec with
everything else and a protected radio cannot be read, so the first codec can only come from the file. Without
`--stock`, rfoundry refuses a radio on the maker's firmware before it asks the loader anything, and names the two ways
on: `--stock FILE`, or `--codec none`, which installs without the codec (sector 7 erased and left blank,
`codec=absent`, FM only; stage 10.1 then installs it). Either is a valid first install. With `--codec none` a start
that fails in this stage has one suspect less, the codec's boot check, which reads all of sector 7 at every start.

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
| `--dry-run` with `--stock FILE` prints "codec mode: merge", the image's sectors as "ERASE + WRITE app", and sector 7 (0x080E0000..0x080FFFFF) as "ERASE + WRITE codec: blocks 2..129 (128 x 1024 bytes, up to 0x080FFFFF)": the plan numbers a sector's blocks from 2. Sectors 8 to 15 read "not touched", and the "erase N sector(s)" line names the image's sectors and 7, no other. With `--codec none`: "codec mode: blank" and sector 7 as "ERASE, left blank (...)" | the merge plan: sector 7 written whole, from its start to the descriptor in its last block; bank 2 untouched | Sector 7 "not touched" or "keep" on a first install, or any sector of bank 2 erased or written: the plan is wrong; do not go on |
| "stock ... verified (SHA-256), codec windows decrypted and checked" before anything is sent | the file is the analyzed one; its key, the carve from both copies, the placement and the three CRC-32 gates held on this computer | A named version that is refused: get V9.00.96. Any other refusal: record the message; nothing was erased |
| The first `--unprotect` run: the first GETSTATUS after the command answers busy, the device leaves USB by itself within 120 s without restarting, and rfoundry asks for the switch-off no sooner than 60 s after the command | AN3156's description of Read Unprotect | "the bootloader did not take the Read Unprotect command (status N, state S)": record them. rfoundry and the web page say when to switch off: after the radio has left USB and at least 60 s (the least wait). With no disconnect within 120 s (the longest wait) the tools say the erase is over or was not taken (120 s is more than four times the 26 s tME bound of DS12110 Table 43), and the next probe decides. **Record** whether the log shows "the bootloader left the bus ... it may be erasing" (a lost GETSTATUS reply after 0x92, taken as the command taken), and whether the bootloader came back by itself with SK1 held through the erase |
| Every "erase" line of the first install passes | a 128 KB sector erases in up to 4 s (DS12110), inside rfoundry's 5 s per status request | "USB error while flashing" during an erase: run the same command again (the sector is then blank and erases quickly) and record it; the timeout is the `5000` in `get_status` of `rfoundry/dfu.py` |
| After the write: "verify: read back N blocks" (N includes sector 7's 128 blocks in a merge), then "write the vector table at 0x08000000" and "verify: read back the vector table", before Leave | the loader takes a 1024-byte UPLOAD of block 2 after Set Address Pointer and ABORT, for every block (the probe's 4-byte read is the only upload any document describes for a protected chip), and it put every block, written as block 2 after its own Set Address Pointer, at that pointer | A refused read or a mismatch stops the install with the image not started; the radio stays in the loader. A refused read on every block: record the status and state; the read-back is `Dfu.read_back` in `rfoundry/dfu.py` and `Dfu.readBack` in the web flasher's `dfu.js` |
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

The same limit and the codec ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 2.3): the boot check reads the
descriptor and all 105243 placed bytes of sector 7 at every start. It first reads every flash word of the sector
under a guard, so a torn word there is meant to cost DMR voice ("No codec"), not the start-up. Untested: no torn
word is made on purpose.

| Look for | Proves | If not |
|---|---|---|
| Nothing: **nobody cuts the power during an install to see this.** What stands in for the test is the installers' rule, run against the fake loader with a cut at every block of sector 7: sector 7 is left as it is only at a radio that is not read protected and holds a vector table, which every install writes last. Otherwise it is written whole or erased whole | (host tests only) | |
| After an install that was cut by accident (USB or power): the same command again. A merge runs whole. A keep is void ("the radio holds no vector table ..."): rfoundry merges from its saved copy of the codec or from `--stock FILE`, or refuses and names `--stock FILE` and `--codec none`. **Record what it printed** | the rule on a radio, without provoking it | The radio then faults at every start (a crash record with a bus fault, a restart loop, or nothing on USB), or starts with no codec after a merge: loader mode and `--codec none` erase sector 7; if it starts after that, the codec's sector was the cause. A restart loop means the boot check's guard did not hold: **record the crash record** |
| **The limit that stays.** A sector 7 left torn by another program (a generic DFU tool) under a vector table that program wrote: the boot check's guard should start RadioFoundry without the codec, until an install through the ROM loader with `--stock FILE` or `--codec none`; a codec read-back or a `dump` there still faults. Use rfoundry or the web flasher only | not checked on purpose | |

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
| `board.store=blank`, `board.flash` `refused=0`; `board.cal_table`'s `crc=` against the backup's 0x1000-0x11FF (`cal_table=blank src=none` on the first radio, whose table was lost) | the board sector at 0x01B000 ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.2) is blank, as it was on the unit seen: the factory copy writes it only from a window that passes the table's reader (10.3). The driver refused no write of the maker's bytes | `foreign`: someone else's bytes there (note it; compare the backup's 0x01B000-0x01BFFF); the sector is not usable for the table's copy. `ours` with `board.cal_rec=ok` and `cal_table=ok src=window`: the copy, written at the first claimed start from a valid window; check that its CRC is the backup's. `ours` otherwise: stop. `refused` above 0: a writer missed the kept-bytes rule: stop, keep the log |
| `rfoundry_debug.py storage factory --backup BACKUP.zip` (the first backup, the whole chip). Read only | the window's, the record's and the backup's CRC-32 (and the window's and the backup's sha256), `window / file` and `record / file`, and the maker's records at 0x01E000-0x01FFFF `same` as the backup's; exit 0 when all are the same, 1 otherwise. On the first radio (2026-11-24): `window blank`, `window / file   differs`, no record, the records `same`, exit 1 | the compare and the identity evidence the restore needs ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.3) | The records `differ`: not this radio's backup, or something wrote the maker's sectors (stop, keep the log). "is not a [[flash.protect]] entry": the tool's registry and the firmware disagree, rebuild both from one tree |
| **Only on a radio whose window lost its table**, the owner's call: `rfoundry_debug.py storage factory --backup BACKUP.zip --restore`, nothing transmitting, the region ours | `restore accepted`, then `result          ok`, `window now ... same as the file`, `record now      ok` with the backup's CRC-32 and `in use: window`; exit 0. Then the compare again (exit 0), `board.cal_table=ok src=window` with the backup's `crc=`, `board.cal_rec=ok` with the same `crc=` and `restore=ok`; after a restart the same with `restore=none`. Seen on the first radio (2026-11-24, [First radio](#first-radio)) | the one restore path: ops 4 and 5, the kept rewrite of sector 1 with the backup's 512 bytes, the read back, the record at 0x01B000 (settings-store.md 4.2) | A refusal prints its reason and writes nothing ([troubleshooting](../user/20-troubleshooting.md#the-factory-calibration-differs-from-the-backup)): `invalid` with the reader's reason number (`CS7000P_MK_WHY_*` in `makercal_cs7000p.h`) means the backup's bytes fail the reader, so keep them for the record and stop; `window_differs` is a valid table already there. `verify` or `flash`: keep the log, compare again before anything else |
| Or the web installer's route (untested): **Restore** of the same backup, **Channels and settings only**, then its factory calibration step | the step shows the radio's, the record's and the backup's CRC-32 and, when the backup is this radio's (the maker's records the same), one button, "Write the Backup's Factory Calibration"; after it the same `cal_table` and `cal_rec` as the tool's row above | no button (`other_radio`, `unchecked`): the tool's row above. A busy answer right after the channels' write: **Check Again** ([web-flasher.md](../dev/web-flasher.md#the-factory-calibration-step)) |
| After a codeplug write through the CPS (part 1 included) or a DTMF or APRS edit: `rfoundry_debug.py storage factory --backup BACKUP.zip` gives the window's CRC-32 and sha256 as before the write (or the CPS read of the codeplug regions, `tools/cps_client.py --regions codeplug read FILE`, and the sha256 of its 0x1000-0x11FF). Once, a whole-chip read (the web installer's **Back Up**, **Everything**): its 0x01E000-0x01FFFF equal to the first backup's | the table's bytes and the maker's records kept ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.3) | The sha256 changed: stop, keep the log and the view's `storage.factory`. On the first radio, until the restore above, the window is blank (an earlier build replaced it), so its sha256 is that of 512 bytes of 0xFF, before and after. After the restore one Codeplug Editor write left the compare `same` ([First radio](#first-radio)); the CPS tool, TFTP, a DTMF or APRS edit and a whole-chip read after it are not checked |
| Only before going back to the maker's firmware: (1) the table, the compare above until it exits 0, `--restore` first where it differs; (2) sector 0, the maker's device information beside RadioFoundry's callsign and DMR ID: `tools/cps_client.py --regions codeplug write spi-flash.bin` (unzipped from the backup); (3) only then the maker's recovery | the compare exits 0 before step 2; the CPS write ends without error | Not in this order: the maker's firmware checks nothing in the table and transmits on what it holds (blank: codes of 255). Steps 1 and 2 need RadioFoundry running. No tool writes the rest of the chip back ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 10.3) |
| `python3 -m pytest tests/hil/test_factory_copy.py boards/cs7000p/hil/test_cs7000p.py -k factory -v -rs` (add `--data-flash-backup BACKUP.zip` while the window is blank) | FACTORY_INFO equal to `cal_table` and `cal_rec`, the refusals by reason, the window's CRC the same after; no test writes the flash | the copy's ops on the radio, read only |

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
| Turning the knob shows the volume bar, 16 at full volume and 1 at the bottom; `volume.knob` (features view) follows it. Untested | the position through the core's pot mapping (hardware-cs7000p.md 5.6) | `CS7000P_VOL_FULL_RAW` |
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
| `board.pll`: locked on every channel across 400 to 527 MHz, the unlock count still | the plan, the bus, the lock pin, the VCO select (questions 3, 32) | Never locked: the bus (PD8 to PD10), `EXP_VCO_LOW`'s polarity, `EXP_SYNTH_RAIL`. Locked on part of the band: note the edges (one at 479 MHz is the VCO switch: repeat with `chip_reg` 3:6 = 1) and the tuning line at test point TP1 (0.5 to 3.9 V). Slow or flickering lock: the phase detector gain in `rfplan_cs7000p.c` |
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

### 9.1 Receive on the maker's table

For a radio with its table in use: restored in stage 6, or never lost. Nothing here transmits, and nothing is stored:
each `reg 3 N` knob is back at its default after a restart, so a restart undoes every step. A steady signal on one
FM channel is enough (the generator at about -110 dBm, or a station heard weakly); steps 4 and 5 want one at a known
frequency. Write each reading into the bring-up record. All commands are `tools/rfoundry_debug.py`.

On the first radio (2026-11-24, [First radio](#first-radio)): step 1's "after" read as written here (of its
"before", only `cal_table=blank src=none`, on an image without the receive work); FM receive on the table sounded as good
as before, by ear, and a DMR group call was decoded. Steps 2 to 5 have not been run.

1. **The table's state, before and after the restore.** `view --section board2`, once before stage 6's restore and
   once after it. Before, on a lost table: `board.cal_table=blank src=none`, `board.ref=hold bias=0x200 r04=0x00 d_a=-
   d_b=- src=default why=table`, `board.rx_cal=default rssi=sheet ...`, `board.sq_maker=none why=table`. After:
   `cal_table=ok src=window` with the backup's `crc=`, `ref=hold bias=0x...` with `bias` = 0x200 + `d_a`,
   `src=maker why=none`, `rx_cal=maker rssi=maker`, `sq_maker=ok o1= c1= o9= c9=`. The restore needs no restart:
   the radio takes the table at its next status poll. If not: `ref ... src=default why=rx_bound` is a trim A over 32
   steps, not used (note `d_a`, receive keeps today's level); `why=group` a field group the reader refused (the
   `fields=` of `cal_table`); `src=none` after `restore=ok`: stage 6's "If not" column.
2. **FM, today's values against the table's.** On the channel with the signal, one knob at a time, each taking effect
   at the next status poll (under a second): the tune, `reg 3 0 2048` (today's) and `reg 3 0 0` (the table's row);
   the level, `reg 3 4 0` (today's) and `reg 3 4 0x100` (trim A). For each, `view --section board` (`board.rssi_raw`,
   `board.noise_raw`, `board.rx_tune`) and `view --section board2` (`board.ref`, `board.rx_cal`), and the audio by
   ear: clearer, the same or noisier. Expect the table's no worse. If worse: keep today's value for the session with
   the same command (it is RAM only, so set it again after each start) and note the frequency, the readings and
   `board.ref`. For good: build with `CS7000P_DEFS="-DCS7000P_RX_MAKER=0"` (Bench knobs) and install it. It
   receives on today's values whatever the table holds, the table kept and shown (`board.ref ... why=build`).
3. **DMR, the same.** On a DMR channel with a station or the bench repeater: `reset` before each half, then a few
   calls, `view --section dmr_stats` (`call.rx_calls`, `call.rx_lost_frames`) and `view --section dmr_call` (the
   source and talk group shown) with today's values (`reg 3 0 2048`, `reg 3 4 0`), then with the table's (`reg 3 0
   0`, `reg 3 4 0x100`). Expect the same calls decoded each way. The RSSI (`call.rssi_dbm`, the S-meter) is on the
   table's scale now (`rx_cal ... rssi=maker`), so its figures differ from before: compare the decode, not the dBm.
   The scale itself is the third knob: `reg 3 8 1` puts the dBm back on the data sheet's line (`rssi=bench`), `reg 3
   8 0` on the table's points, at the next status poll. The DMR scan's strength test and roaming's leave threshold
   read the dBm: if a scan stops or skips, or roaming changes, after the restore, try the same with `reg 3 8 1`, and
   note which. For good: `CS7000P_RX_MAKER=0` (Bench knobs), which also gives back today's tune and level.
4. **The maker's coding of the reference DAC** ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 8 item 1),
   FM only. Note what `reg 3 4` reads (0x0100: no bench offset). Then `reg 3 5 1` and `reg 3 4 0` (bias 0, 4:0x04
   0): `board.hrc` reads `r25=0x0e`, `board.ref` `ref=maker bias=0x000`. Listen: on channel, the same audio as
   before, means the inferred coding holds; about 10 kHz off (no audio on a narrow channel, distorted on a wide one,
   `noise_raw` high) means it does not. Then `reg 3 4 0x05` and `reg 3 4 0xFB` (+5 and -5 steps; about 18-21 Hz
   each, so the audio does not change, but a counter or an SDR on the carrier's beat shows it) and back to `reg 3 4
   0`. Then `reg 3 5 2`: `reg 0 0x26 --page 4` reads 0x7D; note whether anything changes. Last `reg 3 5 0`:
   `r25=0x0c`, `reg 0 0x26 --page 4` reads 0xFD, and `reg 3 4 0x100` (or the value noted). Each 3:5 write reaches the
   modem at the next status poll: until then `board.ref` ends in `ask=` with the coding asked for. A key-up meanwhile,
   and until the way back is taken, is refused with `board.tx_refused` `why=14`. Record: on channel or not under 3:5 = 1, the step size if measured,
   whether 3:5 = 2 changed anything. Off channel: the hold stays the default, nothing else to do.
   `test_reference_coding_knob` checks the register values without a signal.
5. **The maker's IF words and squelch thresholds.** With `board.sq_maker` `ok`: the signal stepped down from about
   -110 dBm into the noise (or a weak station and no signal), at each step `board.noise_raw` and `board.rssi_raw`
   (`view --section board`), with `noise_raw` / 16 set against `o1` / `c1` (squelch level 1, open / close) and `o9`
   / `c9` (level 9). Then `reg 3 7 1` (`board.rx_cal ... ifw=maker`), the same steps, the audio by ear, and `reg 3 7
   0`. Nothing is applied: the squelch stays the port's either way. Record both columns. If the audio is worse
   under the maker's words: `reg 3 7 0` and note it.

`python3 -m pytest boards/cs7000p/hil/test_cs7000p.py -k "receive_calibration or reference_coding" -v -rs` checks
the keys, the held level against `board.ref` and the knob's register values, without a signal and without keying.

## 10. DMR receive

| Look for | Proves | If not |
|---|---|---|
| Sync on a DMR signal from the bench ([dmr-bench.md](dmr-bench.md)): the call's source and talk group show | the IF into the chip, the I/Q order, tones off | No sync with RSSI present: the LO side and I/Q order together (`reg 3 3`) |
| BER against level | the IF level, the reference's accuracy | `ctrl3`; question 1 |

Data, text and the hotspot's receive side need no codec. DMR voice needs the radio's own codec in sector 7: the two
subsections below. Everything in them is untested.

### 10.1 The codec: merge, install, the boot check

No RF. The codec is the one in the maker's `M17_PLUS_HOST_V9.00.96_20260303_1.bin`, merged on this computer into
sector 7 ([codec-cs7000p.md](../spec/codec-cs7000p.md), "Hosting"). If stage 3 installed with `--stock FILE`, the
install rows below are already done: go on at the `probe` row.

| Look for | Proves | If not |
|---|---|---|
| `python3 tools/codec_extract.py --stock M17_PLUS_HOST_V9.00.96_20260303_1.bin --board cs7000p` ends with "... verified" and "wrote build/cs7000p/rfoundry_cs7000p_codec.bin (1048576 bytes; ...)" | the file is the analyzed one; the key, the carve, the placement, the three gates | Another version is named and refused: get V9.00.96. "the rewritten code's CRC-32 ... is not the ... that follows from the map", or the like: the tool or the map is wrong; stop and report it |
| `rfoundry flash IMAGE --board cs7000p --stock FILE --dry-run`: sector 7 "ERASE + WRITE codec", "order: sector 0 erased first and written last" | the plan | |
| The install's "verify: read back" passes | the ROM loader wrote sector 7 | The radio starts nothing until the same command has run again |
| `rfoundry probe`: `codec    cs7000p-v90096-s7`; `rfoundry_debug.py view --section dmr`: `dmr.codec=1` | the descriptor (tag 0x00090096), both CRC-32s over 105243 bytes (code 0xA6BAF942, block 0x1DD4537C), the first word, as flashed | `absent` after a merge: rfoundry says the radio has no usable codec and to flash again with `--stock`. Do that once. `absent` again: **record** the install's read-back lines, and run the host check below on the merged image: it tells a wrong image from a wrong write |
| `rfoundry_debug.py peek 0xE000ED14` (the SCB's `CCR`, 4 bytes): bits 3 and 4 clear. Once after the install's own start (the ROM loader's jump) and once after a power cycle. **Record both values** | the start-up's restore of `CCR` and that nothing set a trap bit since | A set bit is a finding, not a harmless state: the start-up clears both, so something set it later. Record the value and which start it was, and run no DMR voice until it is found |
| Press a key so the radio beeps, then `rfoundry_debug.py view --section tasks`: `tasks.free.audio`, before any DMR voice. **Record it** | the baseline of the audio task's stack | No `tasks.free.audio` line: the radio has made no sound since it started. The audio task is created at the first one |
| An update without `--stock`: the plan shows sector 7 as "keep (codec in the radio stays: ...)", and afterwards `codec=` is as before | keep: the update erased only the image's sectors (0 to 4 for today's default image) | The update says the keep is void: the radio was in its loader without a vector table, or the probe was not remembered. Give `--stock`. `codec=absent` after a keep: the loader erased more than asked; report it |
| `tests/hil/test_codec_info.py` and `tests/hil/test_codec_cpu.py` | the same checks as tests (`codec_check`) | |

To check a merged image without a radio, run the boot check's steps on its sector 7 (its last 131072 bytes, which
the test takes itself from a whole merged image) with the pinned CRC-32s:

```
make -C tests/host cs7000pCodecTest
tests/host/cs7000pCodecTest build/cs7000p/rfoundry_cs7000p_codec.bin pinned
```

It ends `# cs7000pCodecTest pinned PASSED (0 failures)`. A file that is neither 131072 bytes (sector 7 alone) nor
1048576 (a merged image) is refused by its size. The file stays outside the repository.

### 10.2 DMR voice, receive

No RF from the radio. A voice call from the bench ([dmr-bench.md](dmr-bench.md)).

| Look for | Proves | If not |
|---|---|---|
| The call plays from the speaker, intelligible | decode from the codec's placed address; the tables where the merge put them; the PCM path of stage 8 | No audio with the call shown: `dmr.codec`, then `voice.rx_frames` in `dmr_stats`. A restart with a crash record: **record it whole** (`crash show`) and stop DMR voice: a fault inside 0x080E0004..0x080F9B23 is the codec at its placed address |
| No crash record and no restart over a minute of received voice | no fault in the codec: alignment on the real core (the LDRD on T3 and T5), no divide by zero | Record the crash's `pc` and status words. A usage fault with a trap bit set in `CCR`: stage 10.1's row |
| `view --section tasks` after that minute: `tasks.free.audio`, expected above 128. **Record it** | the audio task's stack under decode (2192 bytes inside the codec, less than encode's 4048) | Under 128: stop and report it |
| No gaps: `dmr.underruns` in the `dmr` section and `voice.rx_underruns` in `dmr_stats` stay | decode within its 20 ms at this clock | Underruns that rise with voice only: record the counts |
| The AMBE silence frame from the bench decodes to silence | the page's silence frame check, on the radio | |
| The receive side of the codec page's "Only a radio can check" list: voice quality and level through this radio's audio path, bit order from the HR_C6000, the FEC results | | recorded |

Transmitted voice is in stage 11: on the maker's table at the arm's top stage (11.1, stage 3), else on the bench route (11.2).

## 11. Transmit, only with the owner

Everything before this stage ran without RF. **Nothing transmits unless the test run passes `--allow-rf`**
([tests/hil/README.md](../../tests/hil/README.md)), and arming is no exception: `radio tx-arm` needs it too.
**Every step below is the first time on any radio**: nothing of the transmit path has run on one.

The radio refuses every transmission while its tables are unset: `board.cal=none`, and `board.tx_refused` counts.
With the maker's table in use (`board.cal=maker`) it still refuses every key-up until the owner arms transmission
over the debug link (RADIO op 44, [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 13.2), on the frequencies a
test TX limit lists. Two routes:

| Route | Needs | Values |
|---|---|---|
| 11.1, on the maker's table behind the arm | stage 6's table in use (`cal_table=ok`), stage 9.1 done, an antenna or a dummy load on the connector, a second receiver | the factory's: the drive, the pairs, trim A and B, the sub-audio levels (hardware page 13.2) |
| 11.2, the bench route | a dummy load, a current-limited supply, a power meter or the SDR bench | measured here, written into `rfcal_cs7000p.c` |

The two routes exclude each other (hardware page 13.2, the bench's PA): while the arm is not off the radio takes no
bench drive (`radio tx-drive N` is refused) and keys no bench carrier, and the arm is refused while a bench drive is
set, a bench carrier is up or the DMR key-up constants are the bench's (`pa-keyup`, `pa-keyrun` with constants). From
11.1 to 11.2: `radio tx-arm off` (or a restart). From 11.2 to 11.1: the carrier ended, `radio tx-drive off` and
`radio pa-keyup default`, or a restart.

### 11.1 On the maker's table, behind the arm

The commands are `tools/rfoundry_debug.py`. BACKUP is the whole-chip backup of stage 6 (the zip, or its
`spi-flash.bin`). Choose one simplex FM frequency your license allows, not a repeater input, with a second receiver
tuned to it; every key-up names it with `--mhz` and is refused anywhere else (`why=15`; `txlim.refused` in the core
view). Without a dummy load the carrier goes to the antenna: the antenna is on before any key-up (the PA has no
reverse-power protection), and the station identifies as its license asks. What can be judged without a power meter,
a counter or a deviation meter is in the last table.

| Step | Look for | If not |
|---|---|---|
| **Preconditions**, after a restart (no bench knob of stages 8 to 10 left set). `view --section board2`; `storage factory --backup BACKUP`; `view --section board`; `view --section storage` | `cal_table=ok src=window` (or `src=record` with `cal_rec=ok`) with the backup's `crc=`; the compare exits 0 (`window / file   same`); `ref=hold ... src=maker why=none` (9.1 step 1), `board.cal=maker`; 9.1's receive checks recorded, and its step 4 (the reference coding) with `reg 3 5 0` again: `board.hrc ... r25=0x0c`, no `ask=` in `ref`; `board.pc8_duty=512`; `reg 3 4` reads 0x0100 (no bench offset); `tx_arm=off ...` without `chips=1` or `soc=1` (no `reg 0`, `reg 1` or `reg soc` write since the restart), `bench_drive=` or `bench_keyup=1` (no `radio tx-drive`, `pa-keyup` or `pa-keyrun` left set); the storage view's `ref_trim=0` (**Settings** > **Freq trim** at 0): trim A is the reference's whole correction | `cal_table` not `ok`: stage 6. `ref ... src=default why=rx_bound`: trim A is over the receive bound, receive kept today's level, and FM transmit takes trim A anyway: note it and stop until the owner decides. `r25=0x0e` or `ask=`: `reg 3 5 0` and the next status poll; until then the arm is refused (`state`) and every key-up is `why=14`. `pc8_duty` not 512 (section 9's PC8 steps) or `ref_trim` not 0: a restart, **Freq trim** to 0; until then the arm is refused (`state`), and a trim set while armed refuses every key-up, `why=16`. `reg 3 4` not 0x0100, or `src=bench`: `reg 3 4 0x100` (or a restart) and the next status poll; until then the arm is refused (`state`), and while armed `reg 3 4` refuses every offset. `chips=1` or `soc=1`: a restart, and no `reg 0`, `reg 1` or `reg soc` write after it (reads are fine; while armed every `reg soc` write is refused). `bench_drive=`: `radio tx-drive off`; `bench_keyup=1`: `radio pa-keyup default` (or a restart for either) |
| `radio tx-arm` | `transmit arm    off`, `highest stage   dmr` (`fm` in a build without DMR), `calibration     0x001000-0x0011FF, 512 bytes: in use, CRC-32 0x...` equal to the backup's | `none in use that the arm covers`: the table is not in use, back to stage 6. `no transmit arm`: an older firmware |
| **What the table would key, before arming.** The channel of the first key-up set as the row after Stage 1 says (the VFO on MHZ, 12.5 kHz, no CTCSS or DCS, `radio power lowest`), the arm still off, no key-up. `view --section board2` | `tx_maker=ok at=rx hz=` MHZ in Hz, `mode=fm lvl=0` (hardware page 15); `lo=` and `hi=` both numbers, each a multiple of 16 (16 × the table's code) and `lo` below `hi`: stage 1 keys on `lo=`, stage 2 at 4W on `hi=`; `pair=` two values; `d_a=` the same as `ref`'s `d_a=`. `at=tx` is as good when its `hz=` is MHZ and `lvl=0` (a key-up asked since the last channel change). Neither value says what power comes out: only the second receiver judges that, from stage 1 on. Note the line in the record: the values are this unit's and stay out of the tree | `none`: `no=` names the lookup that refuses and `why=` its reason. `band`: another frequency. `table`: stage 6. `drive` or `pair` with `window` or `switch`: MHZ is inside or past the table's VCO switch segment; choose another frequency. Anything else, or `lo` not below `hi`: stop and report the line; do not arm. Another `hz=`: the VFO is not on MHZ yet |
| **Stage 1.** `radio tx-arm fm-low --backup BACKUP --mhz MHZ --allow-rf` | the compare's lines with `in use / file   same`, `test TX limit   MHZ MHz`, `ARMED           fm-low`; `view --section board2`: `tx_arm=fm-low id=0x...` (the backup's CRC) `list=1`; `log`: `radio: transmit arm fm-low id=0x...`; the core view's `txlim=list`, `tx_drive=N/0` (the low level's drive only) | `refused: the calibration in use is not ...`: the backup is not this table's; nothing was armed. `state`: a bench knob (`reg 3 5`, `reg 3 2`, `reg 3 4`), a `reg 0`, `reg 1` or `reg soc` write since the start (`chips=1`, `soc=1`), a bench drive or the bench's DMR key-up constants (`bench_drive=`, `bench_keyup=1`), a stored **Freq trim**, the power's off state or a restore pending. `id`: never retry with another file |
| On an FM channel or the VFO (`radio vfo on`, `radio freq MHZ`) at 12.5 kHz (`radio bw 12.5`), no CTCSS or DCS, `radio power lowest` (the core view's `power=0 power_name=1W`), the TX timeout short (**Settings** > **TX timeout**) | `view --section core`: `tx_hz` the listed frequency, `mode=fm`, `txlim=list` | Another `tx_hz`: the key-up is refused `why=15`. Fix the channel first |
| **The first key-up**: `ptt --seconds 1`, the second receiver listening, the operator at the radio's knob | `PTT held 1 s, released`; the second receiver hears a carrier for about a second; afterwards `board.tx_refused` unchanged, `board.exp` with neither transmit bit, `board.dac` back on the tuning value, `board.pll=locked`, `state=rx` | No carrier and `tx_refused` up: its `why=` (hardware page 13.2) says which rule held it. A carrier that does not end with the release: turn the knob off (the hold is released and the supply cut, hardware page 5.4), else take the battery off; report it with the log. `the debug link dropped during the key-up` (or `the PTT down not confirmed ... and the debug link dropped`, exit 2) instead of the release: the USB link dropped (below the table); the tool sends the release again when the port is back and says `PTT released` once the view shows nothing injected held, else what to do without the link |
| Then 2 s with speech: the physical **PTT**, speaking at a normal distance | on the second receiver: on channel (no need to retune it; its fine tuning or an SDR shows any offset), clean speech, no hum or whistle; on the neighboring channels (12.5 and 25 kHz either side) nothing, or no more than from another radio's carrier at the same distance. `ptt` releases; the TX timeout ends a held key-up | Off channel or distorted: lower the arm (`radio tx-arm off`) and record what was heard. Splatter on the neighbors: the same. A carrier with no speech: the microphone's codec input (question 7, `CS7000P_E0_MIC`) |
| **Stopping**, each once: release PTT; let the TX timeout end a key-up; the knob off. The debug link's stops (`radio tx-arm off`, `radio tx-limit block`) need the link up during the key-up, and on the first radio it was not (below the table): `radio tx-arm off` after the key-up | each ends the carrier; after `tx-arm off` a key-up is refused `why=13`; after the next start `tx_arm=off` | A carrier that outlives any of them: knob off, battery off, report it |
| **Stage 2.** `radio tx-arm fm --backup BACKUP --mhz MHZ --allow-rf`, then `radio power +1` (`power=1 power_name=4W`, `tx_drive=N/M`) and the same key-ups | `ARMED           fm`; on the second receiver a stronger signal than at 1W (a step of the S-meter or more), the same clean speech, the neighbors as before; the supply current (if the supply shows it) higher | No difference from 1W, or a weaker one: record it and go back to 1W. Distortion only at 4W: record it, stay at stage 1 |
| Tone channels, at stage 1 or 2: a CTCSS tone on the channel (on the radio, or `radio tone tx ctcss 1000`: 100.0 Hz, in 0.1 Hz), then a DCS code; the second receiver with the same tone set | the second receiver opens on it with the tone, and not with another tone set; afterwards `tx_maker ... tone_pm=` the depth it took, in per mille. A tone outside 67.0-254.1 Hz is refused (`why=2`; `tx_maker=none ... no=tone why=range`) | It doesn't open: the depth rule (question 4) or the DCS polarity (`CS7000P_DCS_INVERT`); record it, keep tones off |
| A 25 kHz channel (`radio bw 25`), at stage 1 | speech as loud as at 12.5 kHz on a wide receiver, not quieter | quieter: question 38 |
| **Stage 3, last.** DMR receive first, the same signal, with stage 2 and then after `radio tx-arm dmr --backup BACKUP --mhz MHZ --allow-rf`: `view --section dmr_stats` | the same calls decoded either way (on the top stage 4:0x04 in DMR receive is trim B: question 39) | Worse with `dmr`: `radio tx-arm off` and record it |
| A DMR transmission on a DMR channel at 1W, with the second radio's color code: data first, the smaller step (it needs no codec and nothing of the encoder): `data sms ID TEXT` to the second radio's DMR ID (or `data check ID`) | the second DMR radio shows the message (or answers the check) from this radio's ID. `board.tx_refused` unchanged | No call shown: record `tx_refused`, `board.pll`, `board.dac`; `radio tx-arm fm`. Nothing of the burst path has run on a radio (question 25), and the burst timing is the port's (question 37). The reference in a burst: the bias holds trim A (`ref`'s `bias=`, 0x200 + d_A under the hold) and 4:0x04 trim B (`r04=`), the same two values the maker's DMR transmission carries ([calibration-cs7000p.md](../spec/calibration-cs7000p.md) 5.2, 6.4); only the control word differs, the port's 0x0C in the slot against the maker's 0x0E, and what the DAC does under 0x0C in a slot is not measured (question 1). A call not decoded after FM was on channel at stages 1 and 2 points there first |
| **DMR voice**, after the data row passed, the same channel at 1W. Needs the codec (`dmr.codec=1`, stage 10.1) and received voice passed (stage 10.2). First the test signal in place of the microphone: `radio voice-test on` (view key `voice.tx_test`), then `ptt --seconds 2` | the second DMR radio shows a call from this radio's ID to the channel's talk group and plays the stepped tone; `board.tx_refused` unchanged. Below this stage the same PTT is refused `why=13` and nothing keys | `tx_deny=no_codec` (the core view): no codec, stage 10.1. A call shown without the tone: the encoder, then the rows below. Nothing of the encoder has run on a radio: the codec has only decoded on one (First radio) |
| `view --section tasks` after it: `tasks.free.audio`. **Record it** beside stage 10.1's and 10.2's values | the audio task's stack under encode, the deepest call (4048 bytes inside the codec, 4104 with the veneer; `make stack-check` gives `deepest 4456 free 328` of 5120 and does not see into the codec) | Under 128 bytes free, or a crash record with a stack overflow: `radio tx-arm fm` and report it |
| `view --section dmr`: `voice.enc_flags=0x1840` with the default encode options; `dmr.underruns` and `call.tx_underruns` (`dmr_stats`) unchanged over the call | the flags word the host passes; encode within its 20 ms at this clock | Another flags value: `drivers/vocoder/ambe2_flags.h` against `voice.enc_options`. Underruns that rise only with voice: record the counts and `board.clock` |
| Then `radio voice-test off` and the physical **PTT**, speaking at a normal distance, 2 s | the second radio plays clear speech | Silence with the test signal working: `MIC_5V` while armed, then `CS7000P_E0_MIC`. Distorted or broken speech: `radio tx-arm fm` and record what was heard |

**The USB link while keyed.** On the first radio (2026-11-25) the USB link, which runs through the accessory
connector (hardware page 11), dropped for the length of each key-up, injected or physical PTT alike, and came back
after it. The radio ran on: no restart, no crash record. The cause is not established (hardware page 16, question
43). So the stops to rely on while keyed are release PTT, the TX timeout, the knob and the battery; the arm is lowered
after the key-up. `ptt --seconds` then reports the dropped link instead of the release: the radio ends the injected
PTT by its own time-out, set just past the hold. On 2026-11-25 the port also went deaf after several key-ups: the host
saw a device and got no answer from it, a replug changed nothing (the radio can't see one: no VBUS, hardware page 11),
and only switching the radio off and on brought it back. The firmware now resets the USB controller by itself when a
transmission left the port without a host for 2 s, and tries again every 4 s for about a minute
([cps-protocol.md](../spec/cps-protocol.md) 1.1; `usb.reattach_tx` in the `hotspot` section counts the asks): untested
on a radio. `ptt --seconds` waits long enough for that.

After each stage, before the next: `radio tx-arm off`, `radio tx-limit off`, a restart, and the record written. A
start always begins at `off`. To step back after a check fails, use `radio tx-arm off`: the tool takes a stage over
`off` only with `--backup`, `--mhz` and `--allow-rf`, and without them it sends nothing, so a bare `radio tx-arm fm`
leaves the radio at the stage it was.

What the owner's ear and a second receiver can and cannot judge:

| Can | Cannot |
|---|---|
| Whether a carrier comes and goes with the key-up; whether it is near the channel (a narrow receiver with no audio, or distorted audio, means far off) | the frequency error in Hz, the reference trims' effect (questions 1, 31) |
| Clean speech, hum, a whistle, clipping on loud speech | the deviation in kHz, narrow or wide (question 38) |
| A tone that opens a receiver set to it | the tone's deviation, so too much of it (question 4) |
| A signal heard on a neighboring channel | splatter under the other receiver's own selectivity, harmonics, spurious emissions (question 6) |
| 4W stronger than 1W on an S-meter | the power in watts, the PA current's margin (question 5) |
| A DMR call decoded by another radio | the burst's shape and place in the slot (questions 25, 37) |

### 11.2 The bench route

The one way the firmware keys a radio without a table is the bench drive override, honored only under a test TX
limit that lists the frequency, in FM, for one transmission, within 60 s of arming, and only while the owner's
transmit arm is off ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 13.2): on a radio armed in 11.1,
`radio tx-arm off` first. A `reg soc` write to DAC1 or GPIOE is outside all of
that and is never made (Bench knobs, above).

| Step | Look for | If not |
|---|---|---|
| **The first key-up, at drive 1: `tools/power_cal.py --allow-rf MHZ key 1`** (dummy load, current-limited supply). **It transmits**: no receive-side step puts code 1 on the DAC or closes the loop. PA4 on a scope through the key-up, and the PA current with the loop closed and the transmit rail on (question 33). Not `reg 3 0 1`: that write is clamped to 250, which is also about 0.2 V on the pin and proves nothing about the floor | 0 V while the channel is off, before the loop line comes on; then about 0.2 V at code 1; no PA current at a zero reference | A current at code 1: that is the floor. `CS7000P_APC_FLOOR` |
| [power-calibration.md](power-calibration.md) with `tools/power_cal.py --allow-rf`, upward from the drive of the row above, on a current-limited supply into a dummy load, the PA current watched (question 5) | power rises with the drive; 1 W and 4 W inside the DAC's range | A cliff under some drive, or 4 W out of reach: note it. There is no reverse-power protection: never without a load |
| The carrier on a counter | the reference (question 1), with the receive offset | `CS7000P_REF_OFFSET`, the reference trim |
| A tone into the microphone, the deviation while stepping 4:0xE0 (question 7) | which codec input is the microphone; the modulation pair | No deviation at all: the microphone rail first (`MIC_5V` present while keyed: `EXP_MIC_POWER`, `EXP_MIC_INT`), then `CS7000P_E0_MIC`; the pairs in `rfcal_cs7000p.c` |
| CTCSS and DCS deviation and polarity | PC8's depth: on the maker's table its level's (hardware page 13.2), else the build's | `CS7000P_CTCSS_DEPTH_PM`, `CS7000P_DCS_INVERT`; on the table, the level-to-depth rule in `subpwm_cs7000p.h` |
| An FM key-up on a sagging pack (the bench supply's current limit low enough that the voltage dips under the key-up) | the key-up neither drops out as "knob off" nor restarts the radio: only the core's battery monitor or the time-out ends it. A closed switch reads far over the 0.7 V switch-open threshold | The radio goes off at the key-up: `board.pow_det_mv` during it, against `CS7000P_KNOB_OPEN_POW_DET_MV` |
| The measured values go into `rfcal_cs7000p.c`: the PA rows and the floor (`CS7000P_APC_FLOOR`) from `power_cal.py`, the FM and DMR modulation pairs, the reference offset (`ref_set`). All of them: `cs7000p_cal_tx_ok()` needs the PA row, the pair for the mode and the reference (hardware page 13.2). Rebuild with `CS7000P_BENCH_DMR_TX=1` and install | `board.cal=model`, a `tx_drive` in the core view that is not all 0, and the radio transmits FM without the override | `tx_drive` still 0: a PA point is still `CS7000P_CAL_UNSET` |
| DMR bursts: `tools/bench/pa_cal.py --radio PORT --bench HOST --freq MHZ --pad-db DB --max-w W --allow-rf` (question 25; `--pad-db` the pads between the radio and the SDR, `--max-w` the highest output measured above). It keys over the debug link, the bench gateway measures each burst's ramp; its open-loop keys use the drive override, which in DMR needs `CS7000P_BENCH_DMR_TX=1` | the burst runner and the sequencer on a radio, the key-up constants it reports | "the radio has no power calibration here": the step above is not done. `board.pll` unlock count, `board.dac` refusal count, `board.tx_refused` |
| Afterwards, as analysis only (neither keys the radio): `tools/bench/burstramp.py` on an I/Q capture of the bursts, `tools/bench/pa_shape.py` on `pa_cal.py`'s saved runs (`--out`) | the ramp shapes | |
| `pa_cal.py`'s constants into the board file; rebuild without `CS7000P_BENCH_DMR_TX`; the values on the hardware page | the radio transmits DMR without the override (voice needs the codec of stage 10.1) | |
| **DMR voice, transmit**, on the model tables (`board.cal=model`; on the maker's table it is 11.1's stage 3). Only when the codec is in place (`dmr.codec=1`, stage 10.1) and received voice passed (stage 10.2); under `--allow-rf`, on the dummy load. First the test signal in place of the microphone (`rfoundry_debug.py radio voice-test on`, view key `voice.tx_test`): the bench decodes the call and hears the stepped tone | the encoder from its placed address, the frames on air | No call on the bench with DMR data working: `dmr.codec`, then the rows below |
| `rfoundry_debug.py view --section tasks` after a transmission: `tasks.free.audio`. **Record it** beside stage 10.1's and 10.2's values | the audio task's stack under encode, the deepest call: 4048 bytes inside the codec, 4104 with the veneer. `make stack-check` computes `deepest 4456 free 328` of 5120 and does not see into the codec | Under 128 bytes free, or a crash record with a stack overflow: stop DMR voice and report it. `AUDIO_TASK_STACK` is shared by both STM32 families (`platform/stm32/hrc/i2s_pcm.c`): raising it is the owner's decision |
| No late blocks in a minute of transmitted voice: `dmr.underruns` in the `dmr` section and `call.tx_underruns` in `dmr_stats` stay | encode within its 20 ms at this clock | Underruns that rise only with voice: record the counts and the clock (`board.clock`) |
| `rfoundry_debug.py view --section dmr`: `voice.enc_flags=0x1840` with the default encode options | the flags word the host passes | Another value with default options: `drivers/vocoder/ambe2_flags.h` against `voice.enc_options` |
| Then the microphone: a voice call from the radio decodes on the bench | the microphone path in DMR (the open point on `EXP_MIC_POWER` in section 13) | Silence with the test signal working: `MIC_5V` while armed, then `CS7000P_E0_MIC` |
| The transmit rules do not change with the codec: without calibration the radio refuses every transmission, voice included (`board.tx_refused`), and on the maker's table voice keys only at the arm's top stage | the calibration verdict and the arm are ahead of the vocoder | A key-up with `board.cal=none`, or a DMR key-up below the `dmr` stage: stop, report it |

What the shared sequencer (`drivers/rf/chainseq`) assumes about this chain, each to check once:

| Assumption | Check | If not |
|---|---|---|
| Closing the APC loop on a 0 V reference gives no RF output | a spectrum analyzer on the dummy load while the loop line comes on with the DAC off, at the first bench key-up (11.2's first row) | `CS7000P_APC_FLOOR` |
| The receive rail needs its settle time before the AK2365A's set-up on every receive set-up, a retune included | sensitivity right after a retune with the wait shortened | the board's `rail_settle_us`: `CS7000P_RX_RAIL_US` in `rfops_cs7000p.h` (a fixed define, edited there; `CS7000P_RAIL_SETTLE_US` is the start-up wait only) |
| After a DMR transmission the receiver comes back without an AK2365A set-up, although its LO was off during the bursts | sensitivity right after a DMR transmission against the same after a channel change | the board's `tx_end`: a full receive set-up |
| The lock line reads unlocked by the first read after new synthesizer words (three stale "locked" reads 100 us apart would pass a carrier that is still moving) | a scope on the lock line against the bus's last clock, at a receive to transmit change and at a retune | `CHAINSEQ_LOCK_READS`, `CHAINSEQ_LOCK_POLL_US`, or a fixed wait in the board's `synth` op |
| After a burst the tuning value is back on the shared DAC within about 100 us of the loop line going off. The transmit rail is off by then, but whether the APC amplifier's supply has fallen is open: until it has, the tuning value is a bias request to the PA | supply current and the PA gate voltage at the end of a DMR transmission | the board's `tx_end`: a full receive set-up, which waits |
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
| The microphone in DMR: on from the arm (`EXP_MIC_POWER`, `EXP_MIC_INT`) to its end. The codec's encoder uses it once the codec is merged (stage 10.1). A DMR voice transmission that is silent with the test signal working (stage 11.1): the microphone rail first (`MIC_5V` present while armed), then the codec input bit (`CS7000P_E0_MIC`) | `MIC_5V` on a meter while a DMR transmission is armed (stage 11) |
| The battery while keyed: the transmit check reads `POW_DET` once, before the key-up. Only the core's monitor ends a transmission when the battery sags | stage 11 |
| A safe request (`board_rf_safe()`) leaves the HR_C6000 in the mode it was in, the FM transmit set-up included, until the next `tx_off()` or status poll: the reference oscillator sits on the transmit set-up's control level with the PA off | a frequency counter on the reference after a forced safe request |
| `status()` is where the receiver is restored after a safe request and where the bench changes `chip_reg` 3:3 to 3:6 are applied. If the core stops polling it in some state, they wait for the next `rx()`. In the power's off state it does not restore | by reading the code paths once the radio runs |
| The burst runner's key-up under a bench drive uses the drive as its own top | no difference with the fixed key-up constant; matters once the constants are measured |
| After an underrun or overrun the two I2S streams may be a few samples out of step until the next audio stop and start (RM0433 50.9.12, 50.9.13; an overrun with an early frame in one frame, 50.9.14) | `board.i2s` `und=` / `ovr=` over a few minutes of audio: 0 expected |
| STM32H743 revision Y only: ADC and DAC conversions at the same time may cost ADC accuracy (ES0392 2.9.7), and 12 bits are not guaranteed with VDDA above 3.3 V (2.9.10) | `board.mcu_rev` |
| The codec from its placed address: decode runs on the first radio; encode has run only in an emulator ([codec-cs7000p.md](../spec/codec-cs7000p.md) "On a radio") | stage 11 |
| `CCR.UNALIGN_TRP` and `CCR.DIV_0_TRP` at run time: the start-up clears both and the host checks once before its first codec call; nothing checks later | stage 10.1's `peek`, after the loader's jump and after a power cycle |
| The audio task's stack with the codec: `make stack-check` gives `deepest 4456 free 328` of 5120 bytes and does not see into the codec; the codec page's 4048 bytes for encode are an emulator's figure | `tasks.free.audio` before voice, after received voice and after transmitted voice (stages 10.1, 10.2, 11) |
| Encode and decode within 20 ms at this clock: the emulator gives instruction counts only | the underrun counts of stages 10.2 and 11 |
| Whether a read of a torn flash word faults as the HAL says ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 2.3) | not to be tried on purpose; stage 3's rows |
| The keep across an update: the ROM loader erases only the sectors asked | stage 10.1's update row |
| Port C's pin modes are set by a read-modify-write that is not atomic (`platform/stm32/gpio.c`), from three tasks: the audio task (PC11, PC12, the I2S pads), the GPS task (PC7) and the radio task (PC13, the AK2365A's data line, under the interrupt mask). A GPS or audio task cut by the radio task's masked write can write PC13's old mode back: PC13 an input in an IF chip write (a frame lost unseen: a wrong filter or a deaf receiver until the next receive set-up) or an output after a frame. The backlight's PC9 is set up before any task and is off this list | the RSSI and noise readings, or a 0x0B read-back, after many receive set-ups with GPS and audio running; open in the shared code (an owner's decision) |

## Every assumption, by question number

One row per question of [hardware-cs7000p.md](../spec/hardware-cs7000p.md) 16: what the port assumes, where it is
checked above, and what to change.

| # | Assumption in the port | Stage | Change |
|---|---|---|---|
| 1 | Reference offset 0 (the hold value 4:0x25 = 0x0C is measured in FM receive); the DAC in a DMR TX slot. On the maker's table the receive level 0x200 + d_A under the hold: in use on the first radio with FM as clear by ear (no counter, no A/B); the coding under 0x0E not tried | 9, 9.1, 11 | `CS7000P_REF_OFFSET`, `rfcal_cs7000p.c`, the reference trim; `reg 3 4`, `reg 3 5`, `CS7000P_RX_MAKER` |
| 2 | First LO below the signal; I/Q order to match | 9 | `reg 3 3`, `CS7000P_LO_HIGH` |
| 3 | Phase detector gain 31, lock within 5 ms, three agreeing samples of the lock pin, lock over the whole band | 9 | `rfplan_cs7000p.c` |
| 4 | PC8 idles at 50 % duty, 93.75 kHz; on the maker's table a tone's depth is (4000 L + 525) / 1050 per mille | 9, 11.1 | `reg 3 2`; the rule in `subpwm_cs7000p.h` |
| 5 | No PA value of the tree's own: the model tables unset. On the maker's table its drive codes, behind the arm, give the rated power under the port's ramp | 11 | `rfcal_cs7000p.c`; the arm stays at its stage |
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
| 18 | The software ROM entry reaches the loader and harms nothing: allowed only with loader 0x91 or PB10 high; `reboot --rom-dfu` reached the loader on the first radio (First radio), whether it harms nothing is not checked. rfoundry and the web flasher use it for an update (`[flash.bootloader] restart`) | 12 | `CS7000P_SOFT_ROM_ENTRY`; `restart` in `board.toml` |
| 19 | The ROM's Leave starts the image; an erased first sector starts nothing | 3 | the tools already ask for a power cycle |
| 20 | Nothing unit-specific is in the processor's flash | 0, 2 | none: the codeplug saved (and the tuning data, if the maker's tuner can export it), the maker's recovery, the codeplug written back, before the install |
| 21 | The maker's table, device information and records on the flash chip are located (hardware page 10.3), its codeplug, fonts and voice data are not: nothing is written before the claim. On the first radio the backup's 0x001000-0x0011FF, restored, passed every field group of the reader | 6 | `STORAGE_CLAIM_FIRST` stays |
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
| 32 | `DMR_SLEEP` high = asleep; `A-PDN` high = running; the AK2365A's select high while its reset is released; `VCOVCC_SW` high = the VCO below about 430 MHz, chosen by frequency ([hardware-cs7000p.md](../spec/hardware-cs7000p.md) 7.2) | 8, 9 | one bit each in `exp_cs7000p.h` |
| 33 | "DAC 0" is the channel disabled; the floor is 0 | 11 | `CS7000P_APC_FLOOR` |
| 34 | The chain's and the buses' timing as computed; the chain's bit order as drawn | 4, 8 | `EXP_DELAY_PASSES`, the half periods in `radio_cs7000p.c`, `exp_cs7000p.h` |
| 35 | The reference has settled 30 ms after the synthesizer rail | 9 | `CS7000P_REF_SETTLE_MS`, `CS7000P_RAIL_SETTLE_US` |
| 36 | Two backup-domain pads as outputs do no harm at 250 kHz behind 1 k | 9 | none |
| 37 | The port's DMR key-up constants time bursts on the maker's table too; its burst fields are checked, not applied | 11.1 | `radio_cs7000p.c`; `tools/bench/pa_cal.py` once a bench exists |
| 38 | Wide FM deviates enough under 4:0x35 = 0x20 | 11.1 | `maker_fm_rows` in `rfops_cs7000p.c` |
| 39 | Trim B in 4:0x04 harms no DMR receive on the arm's top stage | 11.1 | the receive branch of `cs7000p_rf_r04` |
| 40 | No radio holds another radio's valid table | 6 | none written: report it |
| 41 | Nobody goes back to the maker's firmware without the order of stage 6 | 6 | none: the order (hardware page 10.3) |
| 42 | The arm stays required until every stage has passed | 11.1 | `CS7000P_TX_ARM_REQUIRED` |
