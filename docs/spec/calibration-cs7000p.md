<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# CS7000 M17 PLUS: the maker's factory calibration

Status: format and use of the calibration data that the maker's factory tuning stores in each radio. **No
transmission has been made with any of it**, by RadioFoundry or under this analysis. Every result below comes from
static reading of the maker's DMR program, from running parts of it on a host emulator, or from one unit's data
(2026-11-22). The few facts measured on a radio are the port's bring-up readings, and the page names them.

How a fact here is known:

| Word | Means |
|---|---|
| read | read from the maker's program as a binary: what the code at an address does |
| emulated | the result of running the maker's own routines on a host emulator, with one unit's table in place and every write to the hardware recorded |
| seen | seen in one unit's table: a property of that data, not of the code. Other units may differ |
| inferred | a name, a meaning or a size given from how something is used, from the service manual's order or from arithmetic on the schematic. Not a measurement |
| measured | measured on a radio by the port's bring-up ([cs7000p-bringup.md](../testing/cs7000p-bringup.md), "First radio") |

Each table row or section says which it is.

This page holds no unit's values. Where an example helps, the numbers are **invented** and say so. The radio's
hardware is in [hardware-cs7000p.md](hardware-cs7000p.md). The HR_C6000 registers named here are in
[hrc6000-registers.md](hrc6000-registers.md).

## 1. Sources

| Source | Used for |
|---|---|
| The maker's DMR program "HOST" V9.00.96, file `M17_PLUS_HOST_V9.00.96_20260303_1.bin` (sha256 `1a4bdf38fed52dc50c738a85f8ce54f6384c070c6a08af21ca919a547b7e5a59`), from the maker's software page, decrypted and linked at 0x08020000 | every "read" and "emulated" fact |
| One unit's whole-chip backup of the data flash, taken by RadioFoundry before it wrote anything to the chip | every "seen" fact. The backup is the owner's and is not in the repository |
| The OEM's service manual, "DR7510-2 / DR7810-2 Service Manual, 2018-04 V01", page 8: the PC tuning items | the names of the items (SM numbers below) |
| [hrc6000-registers.md](hrc6000-registers.md), [hardware-cs7000p.md](hardware-cs7000p.md) | register meanings, the RF chain, the loop and DAC arithmetic |

The service manual's tuning items (its numbering, used as "SM n" below): 1 frequency stability; 2, 3, 4 TX power high,
middle, low; 5 maximum TX voice deviation; 6 VOX1; 7 VOX10; 8 2/5-tone deviation; 9 DTMF deviation; 10 MSK
deviation; 11 DCS/LTR balance; 12 DCS deviation; 13, 14 CTCSS 67.0 Hz and 254.1 Hz deviation; 15 battery warning
level; 16 RX sensitivity; 17, 18 squelch 9 open and close; 19, 20 squelch 1 open and close; 21, 22 RSSI at -120 dBm
and -70 dBm; 23 TX low voltage.

Other versions of the maker's program are not analyzed. Whether they read the table the same way is not established.

## 2. Where the table is

### 2.1 Location and form

| Fact | How known |
|---|---|
| The table is in the **data flash** (W25Q256), at **0x001000-0x0011FF**: 512 bytes, one copy | read; seen |
| It has no header, magic, version, length, checksum or second copy. The maker's program has no default table, and it never reads calibration from the MCU's flash | read |
| 0x000FF0-0x000FFF and 0x001200-0x001FFF are blank (0xFF). The table's runs of values occur once in the 32 MB | seen |
| The last byte, 0x0011FF, is read by nothing. It is no 8-bit sum, XOR, 16-bit sum or CRC-32 of the other bytes | read; seen |
| Multi-byte fields: frequencies are u32 big-endian, the four burst-timing arrays are u16 little-endian (section 3) | read; emulated |

### 2.2 How the maker's program uses it

| Fact | How known |
|---|---|
| At start-up the program reads the 512 bytes once into RAM (the flash's 4-byte-address read, 13h) and uses that copy from then on. It then applies the battery field (0x0DD), the VOX fields (0x042, 0x043) and the tone-input window (0x0DF) | read |
| **Nothing checks a value.** With an all-0xFF table every getter returns 255: the receive tune, both power levels, both modulation pairs; both trims write +127. With an all-zero table every getter returns 0 and both trims write -128 | emulated |
| Only the maker's tuner software writes the table, through the program's factory-test commands. Each such write rewrites the whole 4 KB sector 0x001000-0x001FFF. A further command reloads the RAM copy | read |

So a radio whose table was erased or overwritten runs the maker's program with nonsense values, and transmits on
them. A reader must check what the maker does not (section 7).

### 2.3 The bootloader

The maker's bootloader B7.06 holds no constant flash address inside 0x001000-0x0011FF (inferred: its 0x1000
constants are sizes and offsets; the arguments of all its flash calls were not traced). Whether it ever writes that
sector is open (section 8).

### 2.4 Against RadioFoundry's flash map

The map is in [hardware-cs7000p.md](hardware-cs7000p.md) 10.2.

| Fact | How known |
|---|---|
| The table lies inside RadioFoundry's open-firmware "EEPROM" image (data flash 0x000000-0x00FFFF, [open-firmware-cps.md](open-firmware-cps.md) 7.1). That image's layout puts no field at 0x1000-0x13FF, but its DTMF settings (0x1400) and APRS configurations (0x1588) share the 4 KB sector | read (the map and the CPS layout) |
| A CPS write of EEPROM part 1 (0x0080-0x6060) sends whole sectors from the PC's image. The table survives it only if the PC sends back the bytes it read from this radio. A tool that loads an image made on another radio, or builds one fresh, replaces it | read (the CPS protocol) |
| RadioFoundry's own writes into that sector (a DTMF or APRS edit) read, erase and program the whole sector. They keep the table's bytes, and the journal finishes a rewrite cut by a power loss at the next start ([settings-store.md](settings-store.md) 4), but each such write erases the table and programs it again | read (the port's storage layer) |
| The maker's device information at 0x000080-0x00021F (sector 0) is exposed the same way | seen; read |
| Before the owner's claim RadioFoundry writes nothing there ([hardware-cs7000p.md](hardware-cs7000p.md) 10.3, `STORAGE_CLAIM_FIRST`). After the claim nothing in the firmware protects the table | read (the port) |
| **The board sector 0x01E000 is not free.** It holds two 0x33-byte records of the maker's, at 0x01E000 and 0x01E400, and was not blank on the unit seen. The maker's program reads, writes and erases that sector. Sector 0x01F000 holds a 0x80-byte record of the maker's, also not blank on the unit seen | seen; read |
| 0x006000-0x01DFFF was blank on the unit seen, and no constant-address flash call of the maker's program falls in it. Flash calls with computed addresses were not traced | seen; read |

## 3. Layout

Offsets are from 0x001000. A **TX row** or an **RX row** is 8 bytes, one per frequency point of section 4,
interpolated as section 4 says. Codes are unsigned 8-bit unless the row says otherwise. The SM column pairs a field
with the service manual's item by the order in which the maker's tuner numbers its items: that pairing is inferred.

| Offset | Size | SM | Content | Coding | Used in the maker's program | How known |
|---|---|---|---|---|---|---|
| 0x000 | 8 x u32 BE | | TX frequency points | Hz | the lookup of every TX row | read; emulated |
| 0x020 | 8 x u32 BE | | RX frequency points | Hz | the lookup of every RX row | read; emulated |
| 0x040 | 1 | 21 | RSSI code at the low reference level | 8-bit ADC code | RSSI bars | read; dBm inferred |
| 0x041 | 1 | 22 | RSSI code at the high reference level | 8-bit ADC code | RSSI bars | read; dBm inferred |
| 0x042 | 1 | 6 | VOX threshold at setting 9 | code | VOX | read; name inferred |
| 0x043 | 1 | 7 | VOX threshold at setting 0 | code | VOX | read; name inferred |
| 0x044 | RX row | 16 | Receive front-end tune | PA4 code | PA4 in receive | emulated |
| 0x04C | RX row | 17 | Squelch level 9, open | noise ADC code | squelch | emulated |
| 0x054 | RX row | 18 | Squelch level 9, close | noise ADC code | squelch | emulated |
| 0x05C | RX row | 19 | Squelch level 1, open | noise ADC code | squelch | emulated |
| 0x064 | RX row | 20 | Squelch level 1, close | noise ADC code | squelch | emulated |
| 0x06C | TX row | 2 | Power **high** | PA4 code | PA4 in transmit | emulated |
| 0x074 | TX row | 4 | Power **low** | PA4 code | PA4 in transmit | emulated |
| 0x07C | TX row | 3 | Power middle | PA4 code | **nothing**: only the tuner reads it | read |
| 0x084 | TX row | 13 | Sub-audio level at 67.0 Hz | level L | PC8 | read |
| 0x08C | TX row | 14 | Sub-audio level at 254.1 Hz | level L | PC8 | read |
| 0x094 | TX row | 12 | DCS level | level L | PC8 | read |
| 0x09C | TX row | 8 | Single-tone (2/5-tone) level | code | PA5 tone DAC | read; name inferred |
| 0x0A4 | TX row | 9 | DTMF level | code | PA5 tone DAC | read; name inferred |
| 0x0AC | TX row | 10 | MSK level | code | PA5 tone DAC | read; name inferred |
| 0x0B4 | TX row | | **DMR** modulation pair, first | register value | 4:0x45 in DMR | emulated |
| 0x0BC | TX row | | **DMR** modulation pair, second | register value | 4:0x46 in DMR | emulated |
| 0x0C4 | TX row | 5 | **FM** modulation pair, first | register value | 4:0x45 in FM | emulated |
| 0x0CC | TX row | 11 | **FM** modulation pair, second | register value | 4:0x46 in FM | emulated |
| 0x0D4 | 8 | | unknown | | nothing (only the tuner) | read |
| 0x0DC | 1 | 1 | **Trim A**, the reference oscillator | offset binary: v - 128 | the bias in receive; 4:0x04 in FM transmit | emulated |
| 0x0DD | 1 | 15 | Battery warning point | 8-bit POW_DET code | battery thresholds | read; voltage inferred |
| 0x0DE | 1 | 23 (inferred) | unknown | | nothing reachable (only the tuner) | read |
| 0x0DF | 1 | | DC level of the tone input (PC1) | 8-bit ADC code | an ADC watchdog window | read |
| 0x0E0-0x0E7, 0x0EB | 9 | | unused | | | seen (zero) |
| 0x0E8-0x0EA | 3 x 1 | | unknown | | nothing (the tuner fills them from a live reading) | read |
| 0x0EC | TX row | | DMR, high power: slot advance | 4:0x12 bits 5-0 | 4:0x12 | emulated |
| 0x0F4 | TX row | | DMR, high power: ramp start | in 2-tick units | burst shape | emulated |
| 0x0FC | TX row | | DMR, high power: rise end | in 2-tick units | burst shape | emulated |
| 0x104 | 8 x u16 LE | | DMR, high power: hold end | in 2-tick units | burst shape | emulated |
| 0x114 | 8 x u16 LE | | DMR, high power: PA off | in 2-tick units | burst shape | emulated |
| 0x124 | TX row | | DMR, low power: slot advance | as 0x0EC | 4:0x12 | emulated |
| 0x12C | TX row | | DMR, low power: ramp start | as 0x0F4 | burst shape | emulated |
| 0x134 | TX row | | DMR, low power: rise end | as 0x0FC | burst shape | emulated |
| 0x13C | 8 x u16 LE | | DMR, low power: hold end | as 0x104 | burst shape | emulated |
| 0x14C | 8 x u16 LE | | DMR, low power: PA off | as 0x114 | burst shape | emulated |
| 0x15C | RX row | 21 | RSSI code per RX point, low reference level | 8-bit ADC code | only the tuner; element 4 is copied to 0x040 | read |
| 0x164 | RX row | 22 | RSSI code per RX point, high reference level | 8-bit ADC code | only the tuner; element 4 is copied to 0x041 | read |
| 0x16C-0x173 | 8 | | unused | | | seen (zero) |
| 0x174 | 1 | | **Trim B**, the reference oscillator | offset binary: v - 128 | 4:0x04 in DMR transmit; the bias in one DMR receive path | emulated |
| 0x175-0x1FE | | | unused | | | seen (zero) |
| 0x1FF | 1 | | unknown | | nothing | read |

The table has **no bandwidth dimension**. There are two modes (FM and DMR) and two power levels the program uses
(high and low). Bandwidth is set by fixed register values (section 5.13). The SM item numbers of 0x0B4/0x0BC and
0x0EC-0x14C are not on the service manual's list; those fields belong to this program, not to the older model.

An example of one TX row, **invented**, in the form the table holds it:

| Point | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| TX point (MHz), invented | 400.2 | 429.8 | 430.2 | 445.0 | 458.0 | 472.0 | 492.0 | 511.8 |
| power high (0x06C), invented | 190 | 194 | 195 | 210 | 213 | 215 | 212 | 209 |
| power low (0x074), invented | 100 | 99 | 99 | 108 | 115 | 122 | 126 | 127 |

## 4. Frequency points and interpolation

### 4.1 The points

| Fact | How known |
|---|---|
| Two sets of eight points: TX points for every TX row, RX points for every RX row | read |
| On the unit seen both sets rise, the first point lies just above 400 MHz and the last just below 512 MHz | seen |
| On the unit seen two TX points sit about 0.1 MHz on either side of 430 MHz, and two RX points within about 0.05 MHz on either side of 479 MHz. Those are the program's VCO switch points for transmit and for receive (section 5.12). So each VCO has its own end values | seen; read (the switch points) |
| Whether every unit has the same points is not established | |

### 4.2 Which frequency looks up which row

| Fact | How known |
|---|---|
| RX rows are looked up at the receive frequency f_rx | emulated |
| **TX rows are looked up at the transmit frequency f_tx, the two modulation pairs included, even inside a receive set-up.** A split channel's pair follows its TX frequency | emulated (with f_rx and f_tx different) |
| Frequencies are compared in **10 Hz units**: the channel's frequency in 10 Hz against the point / 10 (integer division) | read |

### 4.3 The segment

For a frequency f (10 Hz units) and points P[0..7] (each / 10), the segment is the first k in 0..5 with
f <= P[k+1], else k = 6. The two points used are k and k + 1. So a frequency below P[1] uses points 0 and 1, and a
frequency above P[6] uses points 6 and 7. Past the ends the line is extended (read).

### 4.4 The arithmetic

With x0 = P[k], x1 = P[k+1], y0 and y1 the row's values there (read; a model of it matched the program on 3000
random inputs, emulated):

| Case | Result |
|---|---|
| x0 = x1 | (y0 + y1) / 2, integer division |
| otherwise | t = 4 y0 + (4 (y1 - y0) (f - x0)) / (x1 - x0), in signed 32-bit, the division truncating toward zero |
| t >= 1020 | 255 |
| t < 0 | 0 |
| otherwise | (t / 4, rounded down) + 1 when bit 1 of t is set: t / 4 rounded half up |

The product 4 (y1 - y0) (f - x0) is formed in 32 bits without widening. Inside 400-527 MHz it overflows only for a
large difference between two values far from both points; a reader should compute it in 64 bits and refuse such
inputs anyway (section 7). The burst-timing arrays (u16) use a 16-bit form of the same interpolation (read); with all
eight values equal, as on the unit seen, it returns that value.

### 4.5 Interpolation over a setting

The same arithmetic runs with a setting in place of the frequency (read):

| Use | x | y at x0 / x1 |
|---|---|---|
| Squelch open threshold | squelch level n, 1 to 9 | open at level 1 (0x05C at f_rx) / open at level 9 (0x04C at f_rx) |
| Squelch close threshold | n, 1 to 9 | 0x064 / 0x054 at f_rx |
| VOX threshold | VOX setting, 0 to 9 | 0x043 / 0x042 |
| CTCSS level | the tone in 0.1 Hz, 670 to 2541 | 0x084 / 0x08C, each first looked up at f_tx |

## 5. What each field drives

### 5.1 PA4: the PA reference and the front-end tune

PA4 is DAC1 channel 1, net APC/TV ([hardware-cs7000p.md](hardware-cs7000p.md) 7.5).

| Fact | How known |
|---|---|
| The program writes PA4 as an **8-bit right-aligned** value (the DAC's 8-bit data register), output buffer on, software trigger. A table code c is therefore the 12-bit code 16 c, and c / 256 of VREF+ | read; emulated |
| At its DAC set-up the program writes 255 to PA4 | emulated |
| **Receive:** PA4 = the 0x044 row at f_rx, written after the synthesizer and before the HR_C6000's pair. The PA loop is already open | emulated |
| **Transmit:** the drive P = the 0x06C row at f_tx for a channel set to any nonzero power level, the 0x074 row for level 0. Middle power is never used: every nonzero level is high. P is not range-checked | emulated |
| In FM, PA4 ramps from 0 to P and holds P (section 6.3). In DMR each burst holds **P + step**, about 5 % above P (section 5.4) | emulated |
| The program builds an 11-step power profile at start-up that no ramp reads | emulated |
| Arithmetic (inferred): a code c gives 0.178 x c / 256 x VREF+ at the loop's reference, and the loop holds 0.26 V per ampere of PA current. With VREF+ = 3.275 V, c = 100 asks for about 0.88 A and c = 200 for about 1.75 A | inferred |

### 5.2 The reference trims

The 16.8 MHz VC-TCXO takes its control voltage from the HR_C6000's DAC on pin 75, which 4:0x47/0x48 (the "bias")
and 4:0x04 set ([hrc6000-registers.md](hrc6000-registers.md) 3.22, 5.1; [hardware-cs7000p.md](hardware-cs7000p.md)
6.5).

| Fact | How known |
|---|---|
| A trim byte v is a signed offset d = v - 128 | read; emulated |
| Written as the **bias**: 4:0x47 = d mod 1024, low 8 bits; 4:0x48 = bits 9-8 of d mod 1024. A 10-bit two's complement | read; emulated |
| Written to **4:0x04**: d mod 256, an 8-bit two's complement (the same as v XOR 0x80) | read; emulated |
| The program writes 4:0x25 = 0x0E, and only once, at the HR_C6000's power-on set-up. **It is 0x0E in receive, in FM transmit and in DMR transmit.** No other write reaches 0x25-0x29 | read |
| The program writes 4:0x26 = 0x7D, also only at power-on | read |

Which trim goes where (emulated unless said):

| Path | Bias (0x47/0x48) | 4:0x04 |
|---|---|---|
| FM receive, at every entry (also after an FM transmission) | trim A | |
| DMR channel set-up | trim A | |
| A DMR return to receive after a transmission | trim A | |
| A second DMR return to receive: every transmit line off, 5R on, then the synthesizer, the tune and the DMR AK2365A words. It is taken instead of the one above when a state flag is set; which transmissions set it is not established | trim B | |
| One further DMR receive set-up, which also writes 4:0xA1 = 0x00. Which user action reaches it is not established | trim B | |
| FM key-up, before the synthesizer goes to the carrier | | trim A |
| DMR key-up, once per transmission, before the synthesizer goes to the carrier | | trim B |
| The FM branch of the channel set-up | none | |

So trim A is "receive, and FM transmit" (SM 1, frequency stability). Trim B is "DMR transmit", and the bias in two of
the DMR receive paths.

The trim's size (inferred): 4:0x04 has steps of about 3.3 mV at pin 75
([hrc6000-registers.md](hrc6000-registers.md) 5.1). Through the control amplifier (VC = 2.975 V - 0.82 x MOD2,
[hardware-cs7000p.md](hardware-cs7000p.md) 6.5) and the oscillator's slope, one step moves the carrier by about
18-21 Hz at 433 MHz. The slope, about 7-8 kHz per volt, rests on two bring-up readings, each turned into volts with
the schematic's factors: the receiver 10.5 kHz high with the DAC off, against on frequency at mid-scale (measured;
taken as 1.35 V of control voltage, inferred), and about 2 kHz across PC8's range (measured from the RSSI at the
passband's edge; 0.27 V of control voltage, inferred). A positive d lowers the frequency **if** the oscillator's
frequency rises with its control voltage. Whether a step of the bias equals a step of 4:0x04 is not established.

**The coding depends on 4:0x25.** The maker never leaves 0x0E. The port holds 0x0C in receive. Under 0x0C the
receiver was on frequency with the bias at 0x200 and off channel with it at 0 (measured,
[hrc6000-registers.md](hrc6000-registers.md) 3.22); the manual calls the held level unsigned over the DAC's range.
The maker's signed word under 0x0C would put pin 75 near 0 V: a small positive d written that way is about 10 kHz
off frequency (inferred). Under 0x0C the equivalent of the maker's d is 0x200 + d (inferred). How the bias codes
under 0x0E in receive is open (section 8, item 1).

### 5.3 The modulation pairs

| Fact | How known |
|---|---|
| The pair is chosen **by mode**: FM writes 0x0C4 to 4:0x45 and 0x0CC to 4:0x46. DMR writes 0x0B4 to 4:0x45 and 0x0BC to 4:0x46. Bandwidth does not change the pair | emulated |
| Both are looked up at **f_tx** and written at the channel or mode set-up, **never at key-up** and never per DMR burst | emulated |
| 4:0x45 sets the level on pin 74, the VCO's modulation path; 4:0x46 the level on pin 75, the reference oscillator's (the net the hardware page calls MOD2). Three things agree: the register reference's figure 4.21 by pin number, the service manual's order (SM 5 "maximum deviation" for 0x0C4, SM 11 "DCS/LTR balance" for 0x0CC), and the data seen (0x045's row steps at the 430 MHz VCO switch and falls faster than 1/f; 0x046's follows 1/f) | inferred |
| On the unit seen the FM values are above the DMR values at every point | seen |

### 5.4 DMR burst timing and shape

The program writes **4:0x12** = (0x0EC at f_tx for high power, 0x124 for low) AND 0x3F, then **4:0x13** = 0, at the
DMR channel set-up (emulated). In the register reference, 4:0x12 bits 5-0 are the RF_TX_INTER advance before the
30 ms boundary in about 100 us steps ([hrc6000-registers.md](hrc6000-registers.md) 3.4); that meaning is inferred
here, not measured.

Each burst is timed in **ticks from the HR_C6000's RF TX interrupt** (PC2), which restarts the shape (read,
emulated). One tick is **50 us** (inferred, strongly: the timer's set-up gives it with the timer clock prescaler
assumed at its reset value, and only 50 us makes the factory hold-end field span about one slot). Let S, R, H and F be the ramp
start, rise end, hold end and PA-off fields at f_tx for the power level, and P the drive of 5.1:

| Tick | What the program does | How known |
|---|---|---|
| 0 | PA4 = 0, the timer starts | emulated |
| 2 S | the PA loop closes (`EXP_APC_LOOP` on), PA4 = 0 | emulated |
| 2 S + 1 to 2 S + N - 1 | PA4 rises, one code per tick, N = 2 (R - S) codes in all with the 0 above | emulated |
| during the rise | the transmit rail (5T) comes on: 6 ticks after the loop at high power, 12-13 at low, seen. Rule not derived | emulated |
| to 2 H | PA4 holds the last rise code, P + step | emulated |
| 2 H + 1 to 2 H + 4 | PA4 falls 4 times by D1 = hold div 8, to M = hold - 4 D1 | emulated |
| 2 H + 5 to 2 H + 12 | PA4 falls to 0 in 8 steps of M div 8, the last (M mod 8) steps one code larger | emulated |
| 2 F | the loop opens and 5T goes off (high power: exactly 2 F; low power: 2 F - 1 to 2 F seen) | emulated |

The rise codes, with h = P div 2, D = P - h, step = D div (N - 4) and rem = D mod (N - 4) (emulated; this rule fits
all ten runs, five frequencies across the band at both levels):

| Index i | Code |
|---|---|
| 0 | 0 |
| 1 | h - (h div 4) + rem |
| 2 | h + rem |
| 3 to N - 1 | h + rem + step x (i - 2), each capped at 250 |

So the hold, code N - 1, is P + step. N must be at least 5; the program does not check it.

An example with **invented** fields, S = 12, R = 19, H = 300, F = 310: the loop closes 1.20 ms after the interrupt,
the drive rises from 1.25 ms to 1.85 ms (N = 14), holds to 30.0 ms, reaches 0 at 30.6 ms, and the loop opens at
31.0 ms (the times rest on the inferred tick). Where a unit's fields put the drive against the burst's first and
last symbols depends on the inferred meaning of 4:0x12 and on where the RF TX interrupt falls (section 8, items 3
and 4).

The program re-tunes the synthesizer to the carrier once per transmission, not per burst (section 6.4).

### 5.5 Sub-audio on PC8

The program drives PC8 from TIM8 channel 3 (read). [hardware-cs7000p.md](hardware-cs7000p.md) 7.6 has the filter
and the summing into the reference's control voltage.

| Fact | How known |
|---|---|
| Period 2100 counts (prescaler 0); idle compare 1024, so the idle duty is **48.76 %** | read |
| Each sample: compare = 1024 + (L x s) / 32 - 4 L, with s from a 0-254 sine. The swing is about plus or minus 4 L counts of 2100, that is plus or minus 0.381 L percent of half the period | read; arithmetic |
| CTCSS: L = the CTCSS level over the tone (4.5) between 0x084 and 0x08C, each at f_tx | read |
| DCS: L = 0x094 at f_tx, the same form | read |
| A 25 kHz channel doubles L, to at most 254 | read |
| The trims were set at the maker's 48.76 % idle. 50 % instead is +41 mV at PC8, about +3.4 mV of control voltage, about 25 Hz | inferred |

### 5.6 Tones on PA5

PA5 (DAC1 channel 2) carries the tones the MCU makes. 0x0A4 scales the DTMF pair (row and column summed); 0x09C a
24-sample tone with a rate timer; 0x0AC a 24-step tone generator (read). Their absolute deviation is not established.
The port has no tone DAC output ([hardware-cs7000p.md](hardware-cs7000p.md) 12).

### 5.7 Squelch

| Fact | How known |
|---|---|
| Thresholds: open and close over the squelch level (4.5), with the rows at f_rx. Level 0, or 10 and above, sets both to 255: always open | emulated |
| The input is the AK2365A's noise output (net BUSY, PB1, ADC2) as an 8-bit code, averaged over 12 samples. Larger is more noise | read (not re-checked) |
| The squelch opens after 2 samples with noise <= open, or with the 8-bit RSSI >= 150. It closes after 6 samples with noise >= close | read (not re-checked) |
| On the unit seen open < close at every point (the hysteresis), and level 1's thresholds are above level 9's (level 9 is the tight one) | seen |

The thresholds were set under the maker's AK2365A words (5.13).

### 5.8 RSSI

| Fact | How known |
|---|---|
| The input is the AK2365A's RSSI output (PB0, ADC2) as an 8-bit code | read |
| Bars: q = (0x041 - 0x040) / 4; bar n starts at 0x040 + n q | read |
| 0x040 and 0x041 are the readings at -120 dBm and -70 dBm (from the service manual's item order) | inferred |
| The per-point rows 0x15C and 0x164 are only the tuner's; element 4 of each is what 0x040 and 0x041 hold | read |

### 5.9 Battery

0x0DD is an 8-bit POW_DET code (the battery divider on the ADC, [hardware-cs7000p.md](hardware-cs7000p.md) 5.5). The
program derives its thresholds from it: B = 0x0DD x 34 / 31, rounded, in all three battery-type branches; a further
set uses 0x0DD x 57 / 62 and x 96 / 62 (read). Which threshold is the warning, and its voltage, are not established
(inferred near 6.2 V).

### 5.10 VOX

The VOX threshold is interpolated over the setting between 0x043 (setting 0) and 0x042 (setting 9) (read). The input
it is compared with is not established here. The port has no VOX (`BOARD_NOT_WRITTEN`).

### 5.11 The tone input window

0x0DF is an 8-bit code. At start-up the program sets an ADC1 analog-watchdog window of 0x0DF + 2 to 0x0DF - 2 on
channel 11, PC1, the TONEIN (2T/5T) input (read). What the window serves is not established. RadioFoundry does not
use PC1.

### 5.12 VCO select and the first LO

The VCO-select line is the expander's bit 8 (`EXP_VCO_RX` in the port). For the model variant seen (variant 0):

| Fact | How known |
|---|---|
| Receive: bit 8 = 1 for 400.0 MHz <= f_rx < 479.00001 MHz, else 0 | read; emulated |
| Transmit: bit 8 = 1 for 400.0 MHz <= f_tx < 430.00001 MHz, else 0, at the FM key-up and at the DMR key-up | read; emulated (420 MHz gives 1, 433.5 MHz gives 0) |
| Both rules switch at a VCO output of about 429-430 MHz: the receive LO at 479 MHz is 429.05 MHz. So bit 8 = 1 selects the VCO below about 430 MHz, in receive and in transmit alike | read; inferred |
| The first LO is f_rx - 49.95 MHz (below the signal) | read |
| The power, pair and tune values at each point were set on the program's choice of VCO | inferred |

This agrees with the port's measurement: lock at 433.5 MHz with the LO at 383.55 MHz and bit 8 = 1, no lock with the
LO at 483.45 MHz and bit 8 = 1 (measured).

The program reads a model-variant byte from its own codeplug (at 0x58 into the record the codeplug directory points
to; 0x029258 on the unit seen, where it is 0). Variant 1 forces bit 8 = 1. Variant 2 puts the LO above the signal,
sets bit 8 = 1 in receive and 0 in transmit, and writes 4:0x01 = 0xF0 (read). A unit of another variant needs its
own check.

### 5.13 Values the program writes around the calibration

Not calibration, but the conditions under which the factory values were set (emulated, at 433.5 MHz, narrow, unless
said):

| Register | The maker's value |
|---|---|
| 4:0x25 / 4:0x26 | 0x0E / 0x7D, at power-on only (read) |
| 4:0x3F | 0x04 narrow, 0x07 wide (0x08 with variant 2) |
| 4:0x34, 4:0x35, 4:0x3E | 0xBC, 0x20, 0x06 in the FM receive set-up. Whether 0x35 changes with bandwidth was not recorded |
| 4:0x37 / 4:0xE4 | FM narrow 0x32 / 0x20, FM wide 0x2A / 0x22, DMR 0xC1 / 0xE0 |
| 4:0xE0 | 0x8B at the channel set-up in both modes, at the DMR receive set-up and at the FM key-up (read; emulated for the first and the last). 0xC9 only in three other paths, one of which also turns analog transmit on; what they serve is not established (read). That 0x8B selects the microphone input is the port's inference |
| AK2365A control 1 | 0xE9 FM narrow, 0xF5 FM wide, 0xE5 DMR |
| AK2365A control 2 | 0x1E |

## 6. The maker's sequences, as far as the values depend on them

Emulated, unless said. "Wait" is a delay whose length in time is not established.

### 6.1 Start-up

The table is read into RAM; the battery, VOX and tone-input fields are applied; the HR_C6000's power-on set-up writes
4:0x25 = 0x0E and 4:0x26 = 0x7D; PA4 is set to 255 (read, emulated).

### 6.2 Receive set-up

| Order | FM receive entry | DMR channel set-up |
|---|---|---|
| 1 | 4:0x10 = 0x80, 4:0x60 = 0x00 | 5R on; codec, AK2365A words |
| 2 | bias = trim A | bias = trim A |
| 3 | synthesizer to f_rx - 49.95 MHz | synthesizer to f_rx - 49.95 MHz |
| 4 | PA4 = tune (0x044 at f_rx) | PA4 = tune |
| 5 | VCO bit (receive rule) | DMR mode registers |
| 6 | FM pair (at f_tx) to 4:0x45 / 0x46 | DMR pair (at f_tx); 4:0x12 and 4:0x13; the drive P and the burst shape are computed |
| 7 | FM registers, AK2365A words | |

### 6.3 FM key-up and key-down

| Order | Key-up |
|---|---|
| 1 | 5R off; 4:0x04 = trim A; synthesizer to f_tx (transmit); VCO bit by the transmit rule; wait |
| 2 | the lock line (PD11) must read high on 6 polls in a row |
| 3 | two waits |
| 4 | 5T on, then the PA loop on, then a short wait **with the receive tune code still on PA4** |
| 5 | PA4 from 0 in equal steps of P / 32 rounded (high power) or P / 16 rounded (low), a wait after each; the last step can overshoot P by up to one step less one code; then P |
| 6 | the sub-audio generator; a wait |
| 7 | (the tone word, if any) 4:0xE0 = 0x8B; the microphone's power and input lines; then **4:0x60 = 0x80**, analog transmit on, after the PA is up |

No write reaches 4:0x45, 4:0x46 or 4:0x25 during the key-up.

Key-down: PA4 falls to 0 in about 32 (high) or 16 (low) steps of P div 32 or P div 16 (truncated here, where the
key-up rounds), a wait after each; the loop opens; three long waits; 5T off; the microphone lines off; 4:0x60 = 0x00;
then the whole FM receive entry of 6.2, with trim A on the bias.

### 6.4 DMR key-up and bursts

| Order | DMR transmission |
|---|---|
| 1 | At the first write that tells the HR_C6000 to transmit (4:0x41 bit 7), before that write: 5R off; **4:0x04 = trim B**; synthesizer to f_tx; VCO bit by the transmit rule. Once per transmission: it runs again only after a receive set-up |
| 2 | Each burst: the shape of 5.4, from the RF TX interrupt |
| 3 | 4:0x25 stays 0x0E; 4:0x45 / 0x46 are not written per burst (read, emulated) |
| 4 | End: 5T off, the loop off, 5R on, the receive set-up with trim A on the bias; or the second return path of 5.2, with trim B |

**The HR_C6000's DAC control word during a DMR transmission is 0x0E** (read).

### 6.5 What not to copy

These are the maker's habits, not calibration. Each breaks a rule of the port's sequencer
([hardware-cs7000p.md](hardware-cs7000p.md) 7.5):

- 255 on PA4 at the DAC's set-up;
- the PA loop closed while PA4 still holds the receive tune code (FM key-up, step 4);
- the last ramp step above P;
- 5T switched on with the loop closed and drive applied (DMR bursts).

Only the steady values (P, the pair, the trims, the timing fields) come from the table. The port's own order (the
DAC at the floor while 5T changes, the loop closed at the floor, then a ramp) uses them unchanged.

## 7. What a reader must check and refuse

The maker checks nothing (2.2). A reader must, and must refuse the whole table, or the field, rather than use a value
that fails. The rules below are this page's, not the maker's.

| Check | Refuse when |
|---|---|
| Blank or cleared | all 512 bytes 0xFF, or all 0x00; any used row with a point at 0xFF; a power, pair or tune row with a point at 0 |
| Points | either set not strictly rising; any point outside 400.0-527.0 MHz |
| Power | high not above low at every TX point; any code outside 1-254; a code at or under the board's floor |
| Pairs | any value outside 1-254 |
| Tune | any code outside 1-254 (the port also clamps the 12-bit code, [hardware-cs7000p.md](hardware-cs7000p.md) 7.8) |
| Trims | either trim farther from 128 than the reader allows. An oscillator within a few ppm puts it within a few tens of steps (inferred) |
| Squelch | open not below close at a point; level 1 not above level 9 |
| RSSI | 0x041 not above 0x040 |
| Burst timing | the ramp start S not below the rise end R; N = 2 (R - S) under 5; the hold end H not after R; the PA-off tick F before H |
| Frequency | a lookup that extrapolates past the end points by more than the band's edge; a lookup inside a VCO switch window (below) |
| Identity | a table read from the radio that differs from the copy kept for it |

Values inside the two windows between the VCO switch points (on the unit seen, the two points on either side of 430 MHz for TX and of 479 MHz for
RX) mix the two VCOs' values: the maker uses them as interpolated, a reader may refuse them or take the nearer
point on the same VCO. Extrapolation past the last RX point can reach codes the DAC buffer cannot follow (the port
clamps).

Never use: 0x07C, 0x0D4, 0x0DE, 0x0E8-0x0EA, 0x1FF.

The table names no unit: nothing in it ties it to a radio's serial number. A copy kept elsewhere should carry the
sha256 of the 512 bytes and be checked against what the radio holds.

## 8. Not established

| # | Question | Settles it |
|---|---|---|
| 1 | How the pin 75 DAC codes the bias under 4:0x25 = 0x0E in receive. Inferred: mid-scale plus the signed bias, and with analog transmit on mid-scale plus the signed 4:0x04. The port's "off under 0x0E" was seen with the bias at 0x200, which is -512 signed, so 0 V. The maker's 4:0x26 = 0x7D against the port's 0xFD is a second difference | In FM receive, nothing keyed: 4:0x25 = 0x0E, 4:0x60 = 0x00, bias 0x000; expect the receiver on frequency. Then a bias of d steps should move the passband about 18-21 Hz per step, and 0x200 about 10 kHz. Repeat with 4:0x26 = 0x7D. Or a voltmeter on MOD2 |
| 2 | The absolute effect of the transmit trims (4:0x04 in FM and in DMR) | the carrier on a counter or an SDR |
| 3 | The time bases: the 50 us burst tick (the timer clock's prescaler), and the length of the FM ramp's waits | a scope on PA4 or the PA current; or the clock set-up |
| 4 | Where the RF TX interrupt falls against the slot, and whether 4:0x12 is an advance in 100 us steps | an SDR capture of a burst |
| 5 | Which user action reaches the two DMR receive paths that put trim B on the bias (the receive set-up that writes 4:0xA1, and the second return to receive), and so which trim the bias holds in DMR receive after a transmission | tracing those paths' callers and the flag that selects the second return |
| 16 | What the three paths that write 4:0xE0 = 0xC9 serve | tracing their callers |
| 6 | Whether anything writes PA4 between PTT and the FM ramp | the PTT path |
| 7 | The names of 0x09C and 0x0AC, and the meaning of 0x0D4, 0x0DE, 0x0E8-0x0EA and 0x1FF | the tuner's item handler, or the tuner software's protocol |
| 8 | The squelch sample counts and the ADC rank map, read but not re-checked | a second reading of the squelch path |
| 9 | The battery voltage behind 0x0DD, and which threshold is the warning | a bench-supply sweep against the maker's warning |
| 10 | Whether the maker's bootloader writes 0x001000-0x001FFF | its flash calls with their arguments |
| 11 | The dBm figures of 0x040 / 0x041 | a signal generator |
| 12 | The VOX input the thresholds compare with | the VOX path |
| 13 | The 5T switch-on point within the DMR rise | the runner's condition for it |
| 14 | Whether other units share the frequency points, and whether other program versions read the table the same way | a second unit; the other versions |
| 15 | The deviation the pairs give under register values other than the maker's (5.13) | deviation on an SDR |
