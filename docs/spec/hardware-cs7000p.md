<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Connect Systems CS7000 M17 PLUS hardware

The CS7000 M17 PLUS is a UHF handheld: STM32H743VI, HR_C6000, a SKY72310 synthesizer with discrete VCOs, an AK2365A
IF chip, a discrete PA with a current loop, a 32 MB SPI NOR flash, a GPS module, and a 24-bit output expander that
carries every rail enable. The port is `boards/cs7000p` on `platform/stm32h7`.

Status: **one radio runs the port; what was seen on it is in the bring-up page's
[First radio](../testing/cs7000p-bringup.md#first-radio), and everything else in this file is untested. On it the
maker's table was restored from the owner's backup and receive runs on it (2026-11-24). Nothing of the transmit path
has run on a radio: no key-up of any kind has been made on one.** The image builds, its host tests pass and its static stack check passes. The order to check things on a radio, and the knob to change when a check
fails, is [cs7000p-bringup.md](../testing/cs7000p-bringup.md).

Words used here:

| Word | Meaning |
|---|---|
| unverified | Written from the schematic or a datasheet, never seen on a radio. Everything in this file is, unless it says otherwise |
| open | In no allowed source. The code carries no value for it, or a value behind a gate that keeps it out of use |
| assumed LCD board | The display and keypad board is not in the maker's schematic. The OEM's 2018 board is the working assumption (section 9) |

What the radio refuses until it is calibrated or checked:

| Refused | By | Lifted when |
|---|---|---|
| Any transmission on the model tables (all unset): FM, DMR data, the hotspot, Tier III registration | the calibration verdict in `tx()` and in the burst key-up (section 13) | the model tables are filled from bench measurements |
| Any transmission on the maker's table (`board.cal=maker`) | the owner's transmit arm (13.2): off at every start, never stored | the owner arms it over the debug link until power-off, in stages (FM at the low level, FM, then DMR), with a test TX limit list naming the frequency (`radio tx-arm`, 13.2) |
| DMR voice transmit, received voice audio | the core: no vocoder (`board.vocoder` is NULL) | a codec route exists |
| Settings and codeplug writes, on every unit, blank flash or not | the foreign-region guard with `STORAGE_CLAIM_FIRST` (section 10) | the owner claims the region after a backup of the whole chip |
| The power hold | `power_cs7000p.c` (section 5) | the knob has been read on, some tens of milliseconds after reset |
| Display and key matrix | the LCD board check (section 9) | the connector reads as the assumed board, or the bench knob `CS7000P_LCD_FORCE` |
| Beeps and PCM audio | the I2S direction probe (section 6) | the transmit pad is seen driven in one of the two senses |
| Software entry to the ROM bootloader | `board_rom_entry_ok()`, the rule in `romentry_cs7000p.h` (section 2.5): the debug link answers such a `REBOOT` as refused | allowed under that rule by default (`CS7000P_SOFT_ROM_ENTRY`); 0 refuses it always |
| Channel selector | `CS7000P_CHSW_VERIFIED` (section 9.4): on, with the table read on one radio | 0 turns it off |

---

## 1. Sources and what is unverified

| Source | Used for |
|---|---|
| The maker's schematic, "CS7000M17plus schematic_V01" (main board only) | every net, pin and part value below, read twice |
| The OEM's service manual for the older model, "DR7510-2 / DR7810-2 Service Manual, 2018-04 V01" | the LCD and keypad board (its page 14), part names where the schematic prints none, the list of alignment items |
| The maker's user documents: the M17 manual ("Using the CS7000 M17 Radio"), "Clearing Flash Memory", "Using the DfuSe Demo program on a bricked radio", "Unbricking a CS7000 M17 PLUS", "Loading the GPS Database" and the others on its software page | controls, bands, the bootloader key, the recovery procedure |
| FCC filing Y4GDR7810S-2 (2022, the OEM's model): grant, Part 90 report | transmit ranges, power levels |
| ST: RM0433 (reference manual), DS12110 (datasheet), ES0392 (errata), AN2606 (ROM bootloader), AN3156 (USB DFU protocol) | the MCU |
| Skyworks SKY72310 datasheet, AKM AK2365A datasheet, Winbond W25Q256JV datasheet, TI SN74HC595 datasheet, the HR_C6000 notes in [hrc6000-registers.md](hrc6000-registers.md) | the chips |
| [calibration-cs7000p.md](calibration-cs7000p.md), the maker's factory calibration, written up from a separate analysis under [CLEANROOM.md](../CLEANROOM.md) | where the factory table and the maker's other records are on the data flash, the table's layout and coding, what each field drives, and what a reader must refuse |

Not known from the schematic, the manuals, the filing or the datasheets, and so not in the code: the first LO's
side, the HR_C6000 registers that set the reference oscillator's control voltage, every calibration value, the display
controller, the channel selector's code, and where the maker's firmware keeps anything on the data flash (the order
of the six function keys was in none of them either; it has since been read on a radio, 9.4). Section 16 lists all
of it.

The calibration and the maker's data on the flash are now known in part, from
[calibration-cs7000p.md](calibration-cs7000p.md): the maker's factory table is at 0x001000-0x0011FF, one per unit;
the maker's device information is at 0x000080-0x00021F, and its records at 0x01E000, 0x01E400 and 0x01F000 (that
page's 2.1 and 2.4). The rest of the maker's data (its codeplug, fonts, voice prompts) is not mapped. Every write
of the firmware keeps the table's bytes and the records; the device information is not kept (10.3). The firmware
reads the table at start, keeps a verified copy of it and shows both; receive uses three of its values (13.1), and
transmit uses its values only once the owner arms it (13.2). On the first radio the table, restored from the owner's
backup, passed the reader with every field group and receive runs on it (bring-up, First radio). No transmission
has been made on them.

The OEM model is Shenzhen Covalue's DR7810S-2. The service manual and the FCC filing describe that model and its
STM32F405 predecessor; where they disagree with the schematic, the schematic wins.

---

## 2. MCU, clock, supply, reset, boot

### 2.1 The chip

IC614, STM32H743VI in LQFP100: Cortex-M7, 2 MB of program flash in two banks of eight 128 KB sectors, 128 KB DTCM,
512 KB AXI SRAM, 4 KB backup SRAM. The pin table is section 3.

| Item | On this board | Consequence |
|---|---|---|
| VDD, VDDA, VREF+ | all on the 3V3 rail (a buck converter set to about 3.24 V), no bead, no reference part | ADC full scale is the rail. The port measures it once from VREFINT (`board.vdda_mv`) |
| VBAT (pin 6) | tied to 3V3: no backup cell | the backup domain loses power with the radio; see 2.6 |
| VCAP (pins 48, 73) | drawn 0.1 uF each; ST asks for 2.2 uF with the regulator on (DS12110) | open question 15. The port stays at VOS3, the reset scale, which asks the least of them |
| Core regulator | the internal LDO: this part has no SMPS and the package no VDDLDO pin | `PWR_CR3` `LDOEN` is the only valid supply configuration |
| 32.768 kHz crystal | none: PC14 and PC15 are signals | no RTC that keeps time. The time comes from GPS |
| Debug | no connector. Test points: TP304 SWDIO (PA13), TP301 SWCLK (PA14), TP305 NRST, TP303 BOOT0, TP401 3V3 | PA13 and PA14 are never reconfigured; their second nets are read through `GPIOA->IDR` (section 3) |

### 2.2 Clock

X603 is a 25 MHz crystal (a 4-pad part drawn with an oscillator symbol; wired as a crystal, 10 pF each side, 1 k in
series with OSC_OUT). `HSEBYP` is 0. The board's table (`board.c`, `stm32_clock_t`):

| Field | Value | Gives |
|---|---|---|
| `hse_hz`, `pll_m` | 25,000,000, 25 | PLL1 input 1 MHz (medium VCO range) |
| `pll_n` | 384 | VCO 384 MHz |
| `pll_p` | 2 | `sys_ck` and the CPU 192 MHz |
| `pll_q` | 8 | 48 MHz for USB and for the SPI1 to SPI3 kernel clock |
| `ahb_div` | 2 | HCLK 96 MHz, timer clocks 96 MHz |
| `apb_div` | 2 | every APB 48 MHz |
| `flash_latency` | 2 | wait states at VOS3 |

Why these: everything fits VOS3, so no code changes the voltage scale. The CPU clock's harmonics (384, 576 MHz) fall
outside the 400-527 MHz receive band. A bus clock under 127 MHz always has a harmonic in that band (here 5 x 96 =
480 MHz): open question 17.

The start-up tries three trees, in this order (`platform/stm32h7/clock.c`). `board.clock` says which one it ended on:

| `board.clock` | Tree | USB clock | What works |
|---|---|---|---|
| `hse` | the table above from the crystal | `pll1_q` | everything |
| `hsi-pll` | the same VCO and dividers from the 64 MHz HSI (input divider 20, a 3.2 MHz PLL input, wide VCO range, N 120): the same rates, about 1 % accurate. A 2 MHz input is for the medium VCO only (RM0433 8.5.5), and the platform refuses it on the wide one | HSI48, trimmed from the USB start of frame by the CRS (not on silicon revision Y, ES0392 2.2.3) | everything. The radio's frequency doesn't come from this clock |
| `hsi` | the bare HSI behind the bus dividers: HCLK 32 MHz, PCLK 16 MHz | HSI48 | USB and the debug link. SPI1 to SPI3 masters have no kernel clock |

The crystal is given 100 ms and a PLL 10 ms to start. The bus dividers and the flash latency are written before any
switch, so no bus is ever over its limit.

On the crystal tree the platform turns on the HSE clock security system. If the crystal stops while the radio runs,
the hardware moves `sys_ck` to the HSI, stops PLL1 and raises the NMI. The NMI takes the fault path:
`board_rf_safe()` first, a crash record with cause 2 (the NMI), then a restart. The next start tries the crystal
again like any start: `hse` if it starts within 100 ms, `hsi-pll` if not. The crash record with cause 2 is the trace
either way.

The ADCs use their synchronous clock mode, `CKMODE` 11 (RM0433 25.4.3). On silicon revision Y that is the bus clock /
4 (24 MHz here). On revision V it is `sys_ck` (HPRE 1) or `sys_ck` / 2 (any other HPRE), then / 4, and / 2 again at
the analog block (12 MHz with this board's dividers); there the platform sets `BOOST` from that clock, since ST's
module works it out from the bus clock. Either way the ADCs have a clock on every tree, which the power switch
reading depends on (section 5).

### 2.3 Memory

`boards/cs7000p/memory.ld` with the shared `platform/stm32/common.ld`:

| Region | Memory | Holds |
|---|---|---|
| `FLASH` | bank 1, 0x08000000 to 0x08100000 | the image. Bank 2 is unused: a ROM bootloader version with a bank 2 erase fault (AN2606 Table 146, V9.1) is never asked to erase it |
| `RAM` | AXI SRAM, 0x24000000, 512 KB less 32 bytes | `.data`, `.bss` (DMA buffers are here: DMA1 and DMA2 reach it), the main stack at the top. The ROM bootloader's Leave wants the initial stack pointer more than 16 bytes below the end of the RAM |
| `CCMRAM` | DTCM, 0x20000000, 128 KB | `.ccmdata`, `.ccmbss`, `.ccmram`, the RTOS heap with every task stack. CPU only: no DMA reaches it |
| not used | SRAM1 to SRAM3 (their clocks stay off), SRAM4, ITCM | |
| backup SRAM | 0x38800000, 4 KB | the crash record. Kept through a reset, lost at power-off (VBAT is on 3V3), erased by a Read Unprotect |

The instruction cache is on. The data cache and the MPU are off: no DMA buffer needs cache maintenance. The task
stacks (the RTOS heap) and the hot statics are in the DTCM, which is never cached; the main stack (start-up and every
interrupt), `.data` and `.bss` are in the AXI SRAM, which a data cache would cover.

### 2.4 Reset

IC615 (PST9124) drives NRST, with a 10 k pull-up and 0.01 uF. **The same net is the HR_C6000's RESETN.** Every MCU
reset (power-on, the supervisor, a software reset, the watchdog: the MCU drives NRST low itself) resets the HR_C6000
too, and the port always loads the chip in full after a reset. There is no way to reset the HR_C6000 alone except its
own software reset register.

The platform's reset handler (`platform/stm32h7/reset.S`) runs in this order: interrupts masked; the supply
configuration (`PWR_CR3` `LDOEN`) and a bounded wait for `ACTVOSRDY`, because the chip takes no RAM write before it
(RM0433 6.4.1); ST's `SystemInit`; the backup domain check (2.6); the board's `board_reset_safe()` (section 4.2);
then `.data`, `.bss` and `main`.

### 2.5 Boot modes, BOOT0 and the ROM bootloader

BOOT0 (pin 94) has 10 k to ground and a P-channel MOSFET (Q9) to 3V3 whose gate is the SK1 key's net. **SK1 held
at power-on or at any reset pulls BOOT0 high, and the STM32's ROM bootloader starts instead of the image.** SK1 is
the side key above PTT, and also `KEY_SK1` on PD15: held at power-on it never reaches the firmware.

| Start | How | What runs |
|---|---|---|
| Normal | no key | the image at 0x08000000 |
| ROM bootloader | SK1 held while the radio is switched on. The maker: "the screen will appear gray" | ST's system memory: USB DfuSe on 0483:DF11. Unverified on a radio with RadioFoundry installed |
| ROM bootloader from the running image | the debug link's `REBOOT` to the ROM (`rfoundry_debug.py reboot --rom-dfu`) | allowed (`CS7000P_SOFT_ROM_ENTRY`, 1 by default) only with ROM loader version 0x91 (V9.1) or with the selector's PB10 contact open (below); the one radio read so far has loader 0x92 (V9.2), so there a selector position that grounds PB10 is refused and the user turns the selector four detents either way. On the first radio `reboot --rom-dfu` reached the loader (the screen went gray). The rule is `cs7000p_rom_entry_ok()` in `romentry_cs7000p.h` (`cs7000pPinsTest`). The platform asks it before the debug link answers, so a refusal is the refused status with reason 5 ([debuglink.md](debuglink.md) 3.11), not an OK followed by nothing. `rfoundry flash` and the web flasher send the same request to a radio that runs RadioFoundry, so an update needs no key (`[flash.bootloader] restart` in `board.toml`; [release.md](../dev/release.md#the-restart-into-the-bootloader)). On a refusal they show the board's hint (`restart_hint`: turn the channel selector four clicks either way and try again) and go on with SK1 at power-on. Through the tools (their restart, their refusal hint) untested on a radio |

The maker's firmware has its own bootloader with its own modes (PTT and the top key for its update mode; SK2 or SK3
to pick one of its two programs). A RadioFoundry install erases that bootloader with the rest of the program flash,
so those modes are gone until the maker's recovery procedure puts it back.

The route depends on the option bytes as shipped: `BOOT_ADD1` on system memory and RDP below level 2. **No code in
the image writes option bytes or programs the internal flash**: the HAL's flash modules are not built, and nothing
writes `FLASH_OPTKEYR`, `FLASH_OPTCR` or `FLASH_KEYR1/2`
([gotchas.md](gotchas.md) 18.8). `board.rdp` shows the RDP byte, `board.rom_loader` the loader's version byte (the
byte at 0x1FF1E7FE), `board.mcu_rev` the silicon revision (Y or V).

Radios leave the maker read protected (RDP level 1 in the maker's own recovery procedure). In that state the ROM
bootloader reads, writes and erases nothing. A Read Unprotect sets RDP level 0 and leaves the USB bus without
restarting (AN3156). The chip erases the program flash, except sectors in a PCROP or secure-only area its option
bytes keep (a partial erase, RM0433 4.5.3), and the backup SRAM and the RTC backup registers. AN3156 says the
bootloader then resets the option bytes to their defaults, but loader V9.0 (0x90) leaves PCROP in place (AN2606 Table
146, fixed in V9.1). The loader version and the option bytes as shipped are open question 14. The Read Unprotect is
the first step of every RadioFoundry install, and it removes the maker's bootloader and programs for good
([release.md](../dev/release.md#the-rom-bootloader-route)).

**A normal power-on with the first sector erased** (an install cut short, or the Read Unprotect done and nothing
written yet): with the boot pin low the chip boots from `BOOT_ADD0` (default 0x0800, that is 0x08000000; RM0433
Table 9). This family has no empty-flash fallback to the ROM bootloader: AN2606's Pattern 10 enters it only with the
boot pin high and `BOOT_ADD1` = 0x1FF0. The core loads 0xFFFFFFFF as its stack pointer and reset vector and faults at
once. Nothing of RadioFoundry's runs: the expander keeps its power-up state, PA4 is not driven, so R141 holds the PA
off and nothing transmits, and PC9 floats, so the screen is probably lit and blank as in loader mode (9.3). It stays
so until the radio is put in its bootloader (SK1 held at power-on) and the same install has run again. The tools
write the vector table's block last for this reason ([release.md](../dev/release.md#the-rom-bootloader-route)):
nothing protected is lost, and no half image ever starts.

**The ROM bootloader sets up every interface it supports, on this board's nets** (AN2606 Table 145). It writes
nothing to the output expander. Keep loader sessions short: connect, write, leave.

| ROM interface | Pin and what the ROM does with it | Net here | Risk |
|---|---|---|---|
| USB DFU | PA11, PA12 | USB_D-, USB_D+ | the route in use. Clocked from HSI48 on loaders from V13.3 and on V9.x: no crystal needed |
| USART1 | PA10 in, PA9 **out** | the Bluetooth module's TX and RX | the module is unpowered when the expander's bit 4 is 0 |
| USART1, second pair | PB15 in with pull-up, PB14 **out** | the HR_C6000's configuration port: its data input, and **its data output** | two outputs on one line while the chip drives its output. The chip's select has no pull-up (not fitted). Open question 18 |
| USART2 | PA3 in, PA2 **out** | biased receive audio; **the sub-audio filter's op-amp output** | an op-amp output against a pin |
| USART3 | PB11 in, PB10 **out** | two of the channel selector's contacts, which the switch can **ground with no series resistor** | a pin driven high into a grounded contact |
| I2C1 | PB6, PB9, open drain | the HR_C6000 voice port's select; IC611's data line | none expected |
| I2C3 | PA8, PC9, open drain | PTT; the backlight's control line | the backlight floats to "lit": the gray screen |
| SPI1 slave | PA4, PA5, PA7 in; PA6 **out** | the PA's power reference (an input: the PA stays off), the tone DAC line, mic audio; the battery sense behind 100 k | none |
| SPI3 slave | PA15, PC10, PC12 in; PC11 **out** | the HR_C6000's I2S port; PC11 is the chip's data input | none |
| SPI4 slave | PE11, PE12, PE14 in; PE13 **out** | external PTT, SK2, a selector contact; SK3 behind 1 k | none |

The loader's version byte is not in order of age: V13.2 (0xD2) and V13.3 (0xD3) are the oldest, then V9.0 (0x90),
V9.1 (0x91), V9.2 (0x92) (AN2606 Table 146). Table 145 names one version, 0x91, on which the USART transmit pins
PA9, PA2 and PB10 stay inputs until that USART is used; on the others they are driven from the start, and for 0x92
the table says nothing (open). PB14 is driven on every version. After the loader's Leave command the image
starts with some of these pins still driven, the loader's USB interrupt and SysTick enabled and its own vector table:
the reset handler and `board_reset_safe()` take all of that back before anything else
([gotchas.md](gotchas.md) 18.10).

This is why the software entry is allowed only under the rule above. With SK1 at power-on the same pins are driven,
but that is the maker's own route and its own procedure.

**Every software reset waits for SK1.** SK1 is the Monitor key, and any reset while it is down starts the same
loader. So every software reset the firmware makes (the crash restart, the hang check, the knob watch's restart, a
reset after a CPS or UI storage job, the debug link's `REBOOT`) goes through `board_reset_prepare()` in
`exp_cs7000p.c`, which first writes the reset word (RF off, the hold clear, 4.2 step 5) and then waits for SK1 to be
released. The wait ends after 10 ms released in a row (`CS7000P_SK1_UP_MS`), checked about once a millisecond, or
after at most 30,000 steps (`CS7000P_SK1_WAIT_MS`, about 30 s). If the cycle counter was not running yet (a fault
before the time base started), each step takes several milliseconds and the bound several minutes (estimated from the
code, not measured). The independent watchdog is fed during the wait. At the bound the reset goes ahead into the ROM
loader, which stays up until the knob is turned off. With the knob off the radio goes dark during the wait, since the
hold is already clear. The rule is `cs7000p_sk1_wait_more()` in `romentry_cs7000p.h` (`cs7000pExpTest`). A watchdog
reset is the one path into the loader this cannot guard (5.3).

### 2.6 VBAT on VDD and the backup domain

With VBAT tied to VDD, a fast power cycle can skip the backup domain's reset and leave `RCC_BDCR` and the RTC
backup registers with random content (ES0392 2.2.32). A random `LSEON` would give the oscillator control of PC14
and PC15 (here the AK2365A's reset line and the accelerometer's data line); a random backup register would read as a
request to enter the ROM bootloader. `stm32h7_reset_early()` resets the backup domain by software on the first
start after power came up, and whenever `RCC_BDCR` shows `LSEON` or `LSEBYP`.

### 2.7 PC13, PC14 and PC15: a known deviation

PC13 to PC15 are supplied through the backup domain's power switch. RM0433 6.4.4 allows only one of them as an
output at a time, at 2 MHz into 30 pF at most. The maker wired two as outputs: PC13 is the AK2365A's data line and
PC14 its reset line, each behind 1 k. The port keeps to the intent of the limit:

- PC14 is static while PC13 moves.
- PC13 is an output only inside a frame of the AK2365A's bus, and an input with a pull-down otherwise.
- The bus runs at 250 kHz, and the loads are CMOS inputs behind 1 k.

PC15 is an input (the accelerometer is not used). Open question 36.

---

## 3. Pin table

Every signal pin of the LQFP100, by package pin. `boards/cs7000p/pins.h` has the same nets as `PIN_*` defines.
"Never driven" means the pin is never a push-pull output, because a chip's output or a contact to ground is on the
net; `cs7000pPinsTest` holds that list.

| Pin | Port | Net | Goes to | Use in the port |
|---|---|---|---|---|
| 1 | PE2 | FLASH_SCK | W25Q256JV CLK | SPI4 AF5, medium slope |
| 2 | PE3 | A-SCLK | AK2365A SCLK, 1 k | bit-banged clock |
| 3 | PE4 | FLASH_CS | W25Q256JV /CS, no pull-up on the board | GPIO select, driven high in the reset hook |
| 4 | PE5 | FLASH_SDO | W25Q256JV DO | SPI4 MISO. Never driven |
| 5 | PE6 | FLASH_SDI | W25Q256JV DI | SPI4 MOSI, medium slope |
| 7 | PC13 | A-SDATA | AK2365A SDATA, 1 k, both ways | an output only inside a frame (2.7) |
| 8 | PC14 | A-RSTN | AK2365A RSTN, 1 k | output, low from the pin set-up until the chip is started |
| 9 | PC15 | MD-SDA | accelerometer SDA, 4.7 k pull-up | input, not used. Never driven |
| 12, 13 | PH0, PH1 | | X603, 25 MHz | HSE |
| 14 | NRST | NRST | PST9124, HR_C6000 RESETN, TP305 | |
| 15 | PC0 | TIME_SLOT_INTER | HR_C6000 | EXTI0. Never driven |
| 16 | PC1 | 2T/5T | voice band receive audio after the tone filters | analog, ADC INP11, not scanned yet. Never driven |
| 17 | PC2 | RF_TX_INTER | HR_C6000 | EXTI2 (the PC2_C pad; `PC2SO` stays 0, below). Never driven |
| 18 | PC3 | SYS_INTER | HR_C6000 | EXTI3 (the PC3_C pad; `PC3SO` stays 0). Never driven |
| 22 | PA0 | PHONE-TXD | accessory contact C-12 | analog mode, not used (UART4) |
| 23 | PA1 | PHONE-RXD | accessory contact C-2 | analog mode, not used. Never driven |
| 24 | PA2 | QT_DQT_IN | the sub-audio low-pass filter's op-amp output, direct | analog, ADC INP14. Never driven |
| 25 | PA3 | RX-A | discriminator audio x3.34, AC coupled, biased at half the rail | analog, INP15, not scanned yet |
| 28 | PA4 | APC/TV | the PA loop's reference (180 k / 39 k) and the front end's tuning line (100 k) | DAC1 channel 1 (section 7.5) |
| 29 | PA5 | BEEP | the speaker line before the volume pot; HR_C6000 pin 8 through 750 k | analog mode, DAC channel 2 off, not used |
| 30 | PA6 | POW_DET | the knob-switched battery through a divider | analog, INP3 (section 5) |
| 31 | PA7 | TX-A | microphone amplifier output, AC coupled | analog, INP7, not scanned yet |
| 32 | PC4 | VOX | rectified microphone level | analog, INP4: the view only (VOX not written, 5.6) |
| 33 | PC5 | VOL-DET | the volume pot's wiper through 100 k | analog, INP8 |
| 34 | PB0 | RSSI | AK2365A RSSI | analog, INP9. Never driven |
| 35 | PB1 | BUSY | AK2365A noise rectifier output | analog, INP5. Never driven |
| 36 | PB2 | BAT_DET | the battery connector's middle contact, 4.7 k | analog mode, not read: the pad has no ADC input, and what the pack puts on it is open |
| 37 | PE7 | CLK | SRCLK of the three 74HC595 | expander clock (section 4) |
| 38 | PE8 | CS | RCLK of the three | expander latch |
| 39 | PE9 | DS | SER of the first | expander data |
| 40 | PE10 | ALARM_KEY | the top key to ground, 47 k pull-up | input, low = pressed. Never driven |
| 41 | PE11 | EXT_PTT | accessory contact C-9, 100 k pull-up | input, low = pressed. Never driven |
| 42 | PE12 | PF2 | SK2 through 1 k, 47 k pull-up | input, low = pressed |
| 43 | PE13 | PF3 | SK3 through 1 k, 47 k pull-up | input, low = pressed |
| 44 | PE14 | ECN0 | channel selector, 47 k pull-up | input, a closed contact reads 0. Never driven |
| 45 | PE15 | ECN1 | channel selector | the same |
| 46 | PB10 | ECN2 | channel selector | the same |
| 47 | PB11 | ECN3 | channel selector | the same |
| 51 | PB12 | DMR_CS | HR_C6000 U_CS, pull-up not fitted | configuration port select, bit-banged |
| 52 | PB13 | DMR_SCLK | HR_C6000 U_SCLK | bit-banged clock |
| 53 | PB14 | DMR_SDO | HR_C6000 U_SDO | input. Never driven |
| 54 | PB15 | DMR_SDI | HR_C6000 U_SDI | bit-banged data out |
| 55 | PD8 | PLL_DAT | SKY72310 DATA, 100 R | bit-banged |
| 56 | PD9 | PLL_CS | SKY72310 /CS, 100 R | bit-banged |
| 57 | PD10 | PLL_CLK | SKY72310 CLK, 100 R | bit-banged, idle low |
| 58 | PD11 | PLL_LD | SKY72310 lock detect, 1 k, 10 k pull-up | input, high = locked |
| 59 | PD12 | LCD_RES | LCD connector "LCD-3" | assumed LCD board: display D/C |
| 60 | PD13 | LCD_A0 | "LCD-2" | assumed: write strobe |
| 61 | PD14 | LCD_CS1 | "LCD-1" | assumed: read strobe |
| 62 | PD15 | PF1 | SK1 through 1 k, 47 k pull-up; the same key drives BOOT0 | input, low = pressed |
| 63 | PC6 | GPS-RXD | GPS module RXD, 47 k pull-up | input (nothing is sent to the module) |
| 64 | PC7 | GPS-TXD | GPS module TXD, 100 R | USART6 RX, AF7. Never driven |
| 65 | PC8 | CTC/DCS_OUT | a 2-pole RC filter into the reference oscillator's control amplifier | TIM3 CH3 PWM, AF2, never released (section 7.6) |
| 66 | PC9 | LCD-LED | two RC stages to the LCD board's backlight transistor | TIM8 CH4 PWM, AF3 |
| 67 | PA8 | PTT_KEY | PTT through 1 k, 47 k pull-up | input, low = pressed |
| 68 | PA9 | BLUE-RXD | Bluetooth module UART RX | analog mode, not used |
| 69 | PA10 | BLUE-TXD | Bluetooth module UART TX | analog mode, not used. Never driven |
| 70, 71 | PA11, PA12 | USB_D-, USB_D+ | accessory contacts C-5, C-4 | OTG_FS, AF10 |
| 72 | PA13 | PHONE_DET behind 10 k | accessory contact C-1; TP304 | SWDIO, never reconfigured; read from `GPIOA->IDR` |
| 76 | PA14 | BLUE-DET | the Bluetooth module's supply behind 10 k; TP301 | SWCLK, never reconfigured; read from `GPIOA->IDR` |
| 77 | PA15 | I2S_FS | HR_C6000 C_CS | SPI3 WS, AF6, an input (slave). Never driven until the chip is known to be its port's master |
| 78 | PC10 | I2S_CK | HR_C6000 C_SCLK | SPI3 CK, AF6, the same |
| 79 | PC11 | I2S_RX | HR_C6000 C_SDI, **the chip's data input** | SPI3's SDI pad by default (section 6.3) |
| 80 | PC12 | I2S_TX | HR_C6000 C_SDO, **the chip's data output** | SPI3's SDO pad by default. Never driven |
| 81-88 | PD0-PD7 | K1-K8 | LCD connector | assumed LCD board: display data D0-D7 and the key matrix |
| 89 | PB3 | V_SCLK | HR_C6000 voice port clock | SPI1 AF5 |
| 90 | PB4 | V_SDO | HR_C6000 voice port data out | SPI1 MISO. Never driven |
| 91 | PB5 | V_SDI | HR_C6000 voice port data in | SPI1 MOSI |
| 92 | PB6 | V_CS | HR_C6000 voice port select | GPIO |
| 93 | PB7 | K9 | LCD connector | assumed LCD board: the matrix's fifth driven line |
| 94 | BOOT0 | | 10 k to ground; Q9 to 3V3, gate on SK1; TP303 | 2.5 |
| 95 | PB8 | SCL | IC611, 4.7 k pull-up | input, not used. Never driven |
| 96 | PB9 | SDA | IC611, 4.7 k pull-up | input, not used. Never driven |
| 97 | PE0 | LCD_DB7 | "LCD-4" | assumed LCD board: display reset (10 k pull-down on that board) |
| 98 | PE1 | LCD_DB6 | "LCD-5" | assumed: display chip select |

Pins 6 (VBAT), 20 (VREF+), 21 (VDDA) and the VDD pins are on 3V3; 48 and 73 are VCAP. The supply hold is not a
pin: it is expander bit 20.

Rules the pin set-up keeps (`pinsetup_cs7000p.c`, `cs7000p_reset_levels()`):

- A pin nobody configured is in analog mode on this chip and reads 0.
- The GPIO slope is the family's low one (12 MHz). Only the flash bus's clock and data out are raised to medium.
- PE7 to PE9 are set up only in the reset hook.
- Every MCU line of the LCD connector except LCD-LED (PD0-PD7, PB7, PD12-PD14, PE0, PE1) is an input with a pull-up
  until the board check has passed (section 9.2). LCD-LED (PC9) is a low output, which is off.
- The two I2S data pads are GPIO inputs until the direction probe has a verdict (section 6.3).
- `PC2SO` and `PC3SO` (`SYSCFG_PMCR`) are 0, the analog switches to the `_C` pads closed: the reset value on this
  package. The platform closes them again at start-up on every package except the one coded LQFP208 / TFBGA240 in
  `SYSCFG_PKGR`, where it leaves the reset value.

---

## 4. The output expander

Three 74HC595 in a chain (IC619, IC620, IC621) on PE9 (data), PE7 (clock) and PE8 (latch). They have no reset
(`SRCLR` on 3V3) and their outputs are always enabled (`OE` on ground): **all 24 outputs are random from power-up
until the first frame, and they keep their state through every MCU reset.** No rail enable and no LED is on an MCU
pin.

### 4.1 Bits

`exp_cs7000p.h`. Bit n of the 24-bit word; bit 23 is shifted first. Bits 0-7 are IC619's QA-QH, 8-15 IC620's, 16-23
IC621's.

| Bit | Name | Net | 1 means |
|---|---|---|---|
| 0 | `EXP_AK_CSN` | A-CSN | AK2365A deselected |
| 1 | `EXP_SPARE` | no label on the drawing | kept 0 |
| 2 | `EXP_MOTOR` | MOT-C | vibration motor on (kept 0) |
| 3 | `EXP_APC_LOOP` | RF_APC_SW | the APC amplifier is powered: the PA can be biased |
| 4 | `EXP_BT_ON` | BLPOWER_ON | Bluetooth module's regulator on (kept 0) |
| 5 | `EXP_MIC_EXT` | EXT-MIC-C | accessory microphone connected |
| 6 | `EXP_AK_RUN` | A-PDN | AK2365A out of power-down |
| 7 | `EXP_DMR_SLEEP` | DMR_SLEEP | HR_C6000 asleep (its PWD pin) |
| 8 | `EXP_VCO_LOW` | VCOVCC_SW | the VCO below about 430 MHz is powered (Q3); 0: the one above (Q4). Chosen by frequency in receive and transmit alike (7.2). The drawing's note "T:0V R:3V" reads it as a direction, which agrees with it only where receive is under 479 MHz and transmit over 430 MHz |
| 9 | `EXP_TX_RAIL` | 5TC | 5T on: pre-drivers, the LO into the transmit chain, the antenna switch to transmit |
| 10 | `EXP_MIC_INT` | MIC-C | internal microphone connected |
| 11 | `EXP_SYNTH_RAIL` | SAVE | 5C on: charge pump, VCOs, the modulation amplifiers, and with them the reference oscillator's control voltage |
| 12 | `EXP_LED_GREEN` | RX_LED | green LEDs on |
| 13 | `EXP_LED_RED` | TX_LED | red LED on |
| 14 | `EXP_AMP` | AFCO | speaker amplifier powered |
| 15 | `EXP_RX_RAIL` | 5RC | 5R on: LNA, mixer, IF amplifier, the second LO's tripler, and the line-out amplifier |
| 16 | `EXP_LAMP` | LAMP | key lights on |
| 17 | `EXP_SPK_INT_OFF` | PHONE_C | internal speaker disconnected |
| 18 | `EXP_MIC_POWER` | MICPOW_C | microphone amplifier powered |
| 19 | `EXP_SPK_EXT_OFF` | SPK_C | accessory speaker lines disconnected |
| 20 | `EXP_POWER_HOLD` | POW_C | the supply is held with the knob off (section 5) |
| 21 | `EXP_ACC_SCL` | MD-SCL | accelerometer clock high (its idle level) |
| 22 | `EXP_AUDIO_RAIL` | FM_SW | 5C_A on: the receive audio filters behind the ADC taps |
| 23 | `EXP_AF_MUTE` | AF_MUTE | no effect as drawn (the part it switches is not fitted); kept 0 |

Polarities read from documents and not seen on a radio: `DMR_SLEEP`, `A-PDN`, `VCOVCC_SW`, and the AK2365A's select
level while its reset is released (open question 32). Each is one bit to flip in `exp_cs7000p.h`. `VCOVCC_SW`'s
meaning is the maker's program's ([calibration-cs7000p.md](calibration-cs7000p.md) 5.12) and agrees with the lock
seen at 433.5 MHz with the bit set; its switch points have not been seen on a radio.

### 4.2 Reset words and the order at start-up

| Word | Value | Meaning |
|---|---|---|
| `EXP_RESET_WORD` | 0x2A0181 | bits 0, 7, 8, 17, 19, 21: both chips parked, the lower VCO selected (the line parked), both speakers disconnected, everything else off. **No transmit bit, no rail, and the hold clear** |
| `EXP_TX_BITS` | bits 3 and 9 | the two bits that can make RF |
| `exp_rf_safe_word(shadow)` | shadow without the transmit bits, with `EXP_VCO_LOW` (the line parked) | what `board_rf_safe()` writes |

The header asserts at build time that the reset word is 0x2A0181 and holds neither the hold nor a transmit bit.

What is undefined at reset, and what each step does about it:

| # | When | Step | Why |
|---|---|---|---|
| 0 | hardware | PA4 is not driven at reset (analog mode) and is an input in the ROM bootloader. R141 (39 k) holds the APC reference at ground, so the PA gates are at 0 V **whatever the expander holds**. The PA drains are on the battery at all times; only the gate bias keeps the PA off | the schematic's own protection. The ROM bootloader relies on nothing else |
| 1 | `board_reset_safe()`, from the reset handler, before `.data` and `.bss` exist | GPIO clocks on; every pin back to analog mode except PA13 and PA14 | the ROM bootloader's Leave enters the image with some of its pins still driven (2.5) |
| 2 | same | PE7, PE8, PE9 low outputs; PE4, the flash's select, high | the flash's select has no pull-up |
| 3 | same | `hc595_shift(&chain_ops, EXP_RESET_WORD, 24)`: one latch edge sets all 24 outputs | a random bit here can be the APC loop |
| 4 | `main()`: `board_rf_safe()` | DAC channel 1 off if its clock is on; then the expander: the safe word of the shadow (initialized data: the reset word until the first update). These are its only writes. Inside the same interrupt-masked section it tells the radio's sequencer (`cs7000p_radio_rf_safe`, `cs7000p_rf_safe`, `chainseq_safed`): flags and state only, nothing while the radio is not up. From then on no transmit bit comes on and the DAC takes no value but 0 until the radio task's next request | the rule of [gotchas.md](gotchas.md) 3.13. The shadow is changed before it is shifted, so code that runs on after a fault can't bring a transmit bit back. The sequencer is told under the mask because a burst point from the timer or the modem's interrupt between the unmask and the radio's safe flag would key the PA again (`cs7000pExpTest`). No second pass through the sequencer's own writes: this is the deepest path on every task's stack (an assert into `reboot()`), and it fits the stack check only without one |
| 5 | before a jump to the ROM bootloader, and before every software reset | `board_sysboot_prepare()` / `board_reset_prepare()`: the reset word, then the HR_C6000's configuration select PB12 driven high. Both are stateless: no shadow is read or written. `board_reset_prepare()` then waits for SK1's release (2.5) | the ROM bootloader never writes the chain. With the chip deselected its data output is expected to be released, the only lever on the PB14 conflict of 2.5 |
| 6 | `board_early_init()` | No init call for the chain: its shadow is initialized data holding `EXP_RESET_WORD`, the word the reset hook shifted before `.data` was copied, so there is one shadow and one writer from the first statement of `main()`. `cs7000p_reset_levels()`. The clock. The DAC set up and left off | a defined reference before any rail comes up |
| 7 | same | PC8: TIM3 CH3 PWM, 50 % duty, 93.75 kHz (period and idle duty from `subpwm_cs7000p.h`) | section 7.6 |
| 8 | same | PC9: TIM8 CH4 PWM at duty 0 (`cs7000p_backlight_init()`, `display_cs7000p.c`): the pin stays low. The display's init only sets the level, after the LCD board check | port C's pin modes have writers in the GPS and audio tasks later; before any task exists nothing can write them back |
| 9 | radio task | section 7.7 | |

In steps 1 to 3 nothing may touch `.data`, `.bss`, the heap or the cycle counter. The chain's half period there, and
everywhere, is a loop of `EXP_DELAY_PASSES` (32) empty passes: at least 0.5 us at the 64 MHz the reset hook runs at
and at least 0.16 us at 192 MHz with one cycle a pass (SN74HC595: 100 ns pulses and 125 ns set-up at 2 V, 20 and
25 ns at 4.5 V, nothing given at 3.3 V). One chain update takes 10 to 25 us.

How long the outputs are undefined after power-on: the chip's reset temporization (377 us typical, DS12110) plus
about 0.1 ms of start-up. A bring-up measurement (open questions 6 and 34), not a promise. For as long as the ROM
bootloader runs, they stay as they were.

### 4.3 One writer, and the shared DAC's interlock

`exp_cs7000p.c` is the only firmware writer of the chain and of DAC channel 1. A bench host can still reach both: a
SoC `CHIP_REG` write (chip 0xFF, [debuglink.md](debuglink.md) 3.24) is allowed anywhere in the peripheral window,
which holds DAC1 (0x40007400) and GPIOE (0x58021000, the chain's PE7 to PE9). Such a write bypasses the interlock
and can key the PA; it is refused only while the radio state is transmit. No tool or test here makes one, and the
bench must not (section 14).

```c
bool cs7000p_exp_set(uint32_t set, uint32_t clear);  // false, nothing written: a transmit bit with the DAC above the floor
uint32_t cs7000p_exp_get(void);
bool cs7000p_apc_dac(uint16_t value);                 // 0 = off, a true 0 V. False, nothing written: the interlock
uint16_t cs7000p_apc_dac_get(void);
```

Both run their check and their write inside one interrupt-masked section, so the burst timer, the radio task and a
bench write over the debug link share them. The rule (`exp_dac_ok`, `exp_set_ok` in the header, `cs7000pExpTest`):

- a DAC value above the floor may be written only while both transmit bits are set (a drive) or both are clear (the
  receive tuning voltage);
- a transmit bit may be set only while the DAC is at or below the floor.

A clear is never refused. A clear that would leave exactly one transmit bit under a reference above the floor
switches the DAC off first (data 0, 20 us, channel off) in the same masked section, so the rule holds after every
write, whoever calls. `cs7000p_apc_dac()` also refuses a value above 4095, and any non-zero value before the DAC is
set up (`board_early_init()`).

The floor is `CS7000P_APC_FLOOR`, the largest DAC code that gives no RF output: 0 until a PA floor is measured.
`cs7000p_apc_dac(0)` zeroes the data register with the buffer still on, waits 20 us, then disables the channel, so
R141 takes the reference the rest of the way to 0 V. With its buffer on the DAC reaches 0.2 V at the lowest
(DS12110), which is 36 mV at the APC reference: "DAC 0" in the sequences of section 7 always means the channel off
(open question 33).

---

## 5. Power

### 5.1 The supply path

```
battery + --F1 2A-- BAT+ ---------------- PA drains (0.13 ohm shunt), speaker amplifier switch
                     |
                     +--F2 0.5A-- BATF+ -- the knob's switch (VR401) -> POW_SW
                                    |      LEDs, the APC amplifier's switch
                                 Q606 (PNP pass transistor), on when POW_SW or POW_C is high
                                    |
                                 BAT7V4 -- buck converter -> 3V3 (about 3.24 V): MCU, flash, HR_C6000, expander, GPS
                                        -- LDO -> 5M: sources of 5R, 5C, the microphone rail, the AK2365A's 3.3 V LDO
                                        -- LDO -> 5T through its switch
                                        -- the LCD board's supply
```

`BAT+` and `BATF+` are not switched: the PA drains, the LED anodes and the knob's switch are on the battery whenever
a pack is fitted. Nothing below Q606 has power with the radio off, the expander included. There is no charger and
no USB VBUS on the board; the pack is charged outside the radio.

### 5.2 The latch

The volume knob's switch (VR401) connects `BATF+` to `POW_SW`, which turns Q606 on through a diode and Q607.
`POW_C`, expander bit 20, does the same through a second diode: the hold.

| State | Radio |
|---|---|
| knob on | on. The hold changes nothing (its diode is reverse biased) |
| knob off, hold set | on |
| knob off, hold clear | off within about 5 to 15 ms |

The hold drives Q607's input below the level its maker guarantees (about 2.7 V against 3.0 V): the latch works by
typical values. Whether it holds at the lowest battery voltage is open question 13.

### 5.3 Why the hold is clear at reset

The chain keeps its outputs through an MCU reset, and BOOT0 is the SK1 key, sampled at every reset. A reset word
with the hold set would keep the radio on for good in three cases:

- a fault or a hang before the first knob reading, with the knob off: every reset sets the hold again;
- an ADC that does not start: the knob is never read;
- a reset with SK1 down: the ROM bootloader runs and never writes the chain. SK1 is the monitor key, one people
  hold. Every software reset first writes the reset word, then waits for SK1's release (2.5).

With the knob on the hold has no effect at all. So:

- The reset word has the hold **clear**. A reset with the knob off ends with the radio off.
- `cs7000p_power_start()`, the first call of `board_init()`, starts the ADC scan and waits for three averages 10 ms
  apart. All three at or above the switch-open threshold, `POW_DET` 0.7 V (`CS7000P_KNOB_OPEN_POW_DET_MV`), which is
  2,559 mV as a battery reading by the model (`CS7000P_KNOB_OPEN_MV`): the hold is set, `board.knob=on`. Any of them
  below: the knob is off and this start is a reset under a decaying supply: the hold stays clear, `board.knob=off`.
- If the ADC does not start, the state is `noadc`: **the hold is never set**, and the radio runs for as long as the
  knob powers it. A scan that stops for good later (two overruns in a row, a DMA error) is treated the same from the
  next poll: the hold is cleared and `board.knob=noadc`.
- Every software reset writes the reset word first (section 4.2, step 5).

From reset to the hold is some tens of milliseconds. A bounce of the knob's contact in that window switches the
radio off, and the knob restarts it.

**When the pack must be taken off.** In each of these the radio stays on with the knob off until the pack is
removed, and the PA stays off through R141 (open question 29):

- A watchdog reset with SK1 held: no code of RadioFoundry's runs before the ROM bootloader, and the chain keeps the
  hold it had. Every software reset waits for SK1 (2.5), so this is the only path into the loader the firmware
  cannot guard.
- A power-on with SK1 held (the install route, or a session that left the loader without installing), or a power-on
  with the first sector erased (2.5), with the knob turned off before RadioFoundry has run: the chain still holds
  its random power-up state, and the hold may be part of it. How often IC621 comes up with the hold set is a bench
  measurement.
- A start task that stays blocked for good (a lock never given back) with the knob off: the knob is no longer polled,
  so the hold is never cleared. The watchdog does not see a task that waits forever while the others still idle
  ([crash-log.md](crash-log.md) 1.1.1). This is the same on every radio whose supply the firmware holds; a fix would be
  the core's, a stall check on the start task, not this board's.

SK1 held for more than about 30 s through a software reset is not one of them: the reset then goes into the loader
with the hold clear (2.5), and the knob switches the radio off.

### 5.4 Switching off

`switch_on()` decides "knob off" on its own threshold, `POW_DET` under 0.7 V (`CS7000P_KNOB_OPEN_POW_DET_MV`). With
the switch open the pin reads 0 V within a millisecond, whatever the hold and the battery. Closed, it reads about a
quarter of the battery: 1.56 V at 6.0 V, over 1 V down to 4.0 V. Three readings 10 ms apart go through the shared
`pwrsw` debounce (`cs7000p_knob_reading()`). A pack that sags under a transmission is no switch-off: a flat battery
is the core's battery monitor's business.

`latch(false)` first sets the board's off state (`cs7000p_power_off_state()`), then: the data flash quiesced,
`board_rf_safe()`, the amplifier, display and LEDs off, and the hold cleared. If the supply is still there after
500 ms, the knob is on (a power-off the radio asked for itself: a critical battery, the auto power off). While the
off state lasts:

- the radio task takes no receiver restore at the status poll, the speaker amplifier stays off for the squelch and
  for beeps, and no LED comes on (the core keeps driving them). A `led()` or speaker call that the power-off cuts
  into leaves nothing on either: both test the off state and write their expander bit in one masked section;
- `tx()` is refused, FM and DMR, with `why=12` (13.2), whatever the pack reads by then; a bench drive that is armed
  is neither used nor dropped. `rx()` and the backlight are not gated: a UI key press still lights the backlight and
  the key lights;
- the MCU and the receive rails, which `board_rf_safe()` does not cut, keep drawing current, so the pack goes on
  discharging until the knob is turned off.

`latch(true)`, the core's override when the radio still runs 2 s after an auto power off, ends the off state: it
stops the watch and reads the knob again (three readings), and the hold is set if it reads on. The next status poll
sets the receiver and the speaker up again (section 7.11). A critical-battery power-off with the knob on stays in the
off state until the knob is turned off (the supply then goes, since the hold is clear), or until the knob watch sees
the switch open and then closed again: a restart. It never loops through resets by itself.

The watch runs on a 10 ms timer. A restart needs a reading of the switch open (under 2,559 mV by the model,
`CS7000P_KNOB_OPEN_MV`) since (or right before) the power-off, then three readings in a row of it closed, and at
least 500 ms since the hold was cleared. Without a reading (no ADC) there is no restart. `cs7000pPowerTest` runs
`power_cs7000p.c` on fakes: the hold, the power-off order, the scan stopping, a sagging pack, the off state's LEDs,
also for a `led()` call the power-off cuts into, and the watch idle once the off state ends. `cs7000pRadioTest` and
`cs7000pRadioSeqTest` check that nothing transmits in the off state.

### 5.5 Battery reading

`POW_DET` (PA6, ADC INP3) is the only analog path from the battery, and it is behind the knob's switch: knob off
reads 0 V. It is not a plain divider: the latch's diode and Q607's input resistor load the node.

| Battery | `POW_DET` (model) |
|---|---|
| 8.4 V | 2.16 V |
| 7.4 V | 1.91 V |
| 6.0 V | 1.56 V |

`battery_cs7000p.h`: `cs7000p_battery_mv(pow_det_mv) = (4006 x pow_det_mv - 245000) / 1000`, and 0 below 62 mV.
The constants are `CS7000P_BAT_GAIN_PPK` (4006) and `CS7000P_BAT_OFFSET_MV` (245). Q607's input resistor alone
moves the result by +/-4 %: an accurate reading needs a per-unit value (open question 13). `battery_low_mv` is
6200 and `battery_critical_mv` 6000, for a 2-cell pack. `battery_mv()` is 0 (no reading) only when the unfiltered
figure is under the switch-open level (2,559 mV by the model, `POW_DET` 0.7 V), so the moments between knob-off and
power-off are not shown as a flat battery. Any other reading, a flat pack under 5,500 mV included, is smoothed
(`drivers/power/battery`) and goes to the core, whose `battery_low_mv` and `battery_critical_mv` judge it.
`battery_pct()` is linear from 6.4 V to 8.2 V.

`BAT_DET` (PB2) is the battery connector's third contact, not a voltage divider. It is not read.

### 5.6 The ADC scan, volume, LEDs

ADC1 scans six ranks at 2,400 scans a second (TIM6), 16 scans deep: `POW_DET`, `VOL-DET`, `VOX`, `RSSI`, `BUSY`,
`QT_DQT_IN`. The sub-audio rank is handed to the detector in batches of 8 scans.

| Function | How |
|---|---|
| Volume | The pot is in the analog audio path: the firmware cannot set the volume. `VOL-DET` gives the knob's position as a percent of `CS7000P_VOL_FULL_RAW`. It is 3178, read on one radio at full volume (7 just above the power click); no source has the pot's resistance. `volume()` is -1 while the scan does not run. The core draws the volume bar only for a radio whose `hal_radio_t.volume` is set, and this one's is NULL (section 8) |
| LEDs | red: bit 13; green: bit 12; both: orange. On or off only: the green LEDs' resistor is drawn 22 R, so no PWM |
| Microphone level | the `VOX` rank, read for the view (`board.vox_raw`) only. VOX is not written for this radio (`BOARD_NOT_WRITTEN`): the level detector's amplifier (`EXP_MIC_POWER`) and the microphone switch (`EXP_MIC_INT`) are on only during a transmission, so it has no supply while receiving |
| Vibration | none (`vibrate` is NULL) |

---

## 6. HR_C6000

IC609, with its own 29.4912 MHz oscillator (X602, always running), reset by NRST (2.4) and put to sleep by expander
bit 7. Shared behavior is in [modem-hrc6000.md](modem-hrc6000.md); section 8.2 there is the order a board like this
one needs.

### 6.1 Ports and lines

| Port | Pins | In the port |
|---|---|---|
| Configuration (U_SPI) | PB12 select, PB13 clock, PB15 data out, PB14 data in | bit-banged on port B (`stm32_hrc_bus_cfg_t`) at 1 to 2 MHz |
| Voice (V_SPI) | PB3 clock, PB4 data in, PB5 data out, PB6 select | SPI1, 3 MHz |
| PCM (C_ port, as I2S) | PA15 WS, PC10 CK, PC11 to the chip, PC12 from the chip | SPI3 as a full-duplex I2S slave (6.3) |
| Interrupts | PC0 timeslot, PC3 system, PC2 RF TX | EXTI0, EXTI3, EXTI2. The board passes `-DSTM32_HRC_LINE_SYS=3 -DSTM32_HRC_LINE_RFTX=2` |
| RF RX interrupt | not wired to the MCU | not used |

### 6.2 What the chip does here

- **Receive.** The AK2365A's 450 kHz IF output goes to the chip's I ADC; the Q ADC is unconnected. The chip
  demodulates DMR and FM from the IF itself. The I/Q order (4:0x01 bit 6) follows the first LO's side.
- **FM audio.** There is **no analog path from the discriminator to the speaker.** FM receive audio is the chip's
  own, on its line out (4:0x10 = 0x80, feed-through 4:0x36 = 0x02).
- **Transmit.** Two-point modulation from the chip's two DACs: pin 74 (net MOD1) to the VCO's modulation varactors,
  AC coupled; pin 75 (net MOD2) to the reference oscillator's control pin, DC coupled.
- **Tones.** 4:0xA1 is written 0x00 after the power-on init: with the init's 0x80 a chip fed a 450 kHz IF never
  finds DMR sync.

### 6.3 The I2S crossing

The chip's data input (C_SDI) is on PC11, which is SPI3's **SDI** pad by default, and its data output (C_SDO) is on
PC12, the SDO pad. The pair is crossed for a master; for a slave the reference manual says two things. RM0433
Table 426 (50.9.6): in full duplex with `IOSWP` 0, SDI is the input and SDO the output, for master and slave.
Section 50.9.2: a slave receives on SDO and transmits on SDI. By the table this board needs `IOSWP` 1, by 50.9.2
it needs 0, and the wrong one makes PC12 a push-pull output against the chip's output for as long as audio runs.

The port assumes neither. The board says which pad is which (`-DSTM32_I2S_TX_ON_SDI=1`), and the STM32H7 half of
the PCM stream proves the direction before it drives anything that could conflict:

1. Both data pads stay GPIO inputs: PC11 with a pull-down, PC12 without a pull.
2. With the chip's clock running, PC11 alone is given to the block, first with the `IOSWP` value Table 426 gives,
   then with the other. Two samples are written and the half counts the pad's level changes for 4 ms.
3. The first sense drove the pad: `dir=ok`. Only the second: `dir=swapped` (it works, and the log says so).
   Something else drives the pad: `busy`. Neither sense: `none`, and the stream stays off with no pad left in
   alternate function mode.
4. Only on `ok` or `swapped` does PC12 leave GPIO input.

`board.i2s` shows the verdict, the `IOSWP` in use, both edge counts and the stream's error counts. Until there is a
verdict the radio has no beeps and no PCM audio. Open question 8.

### 6.4 Codec inputs

The microphone amplifier feeds the chip's pin 5. The driver's set-ups select another input (4:0xE0 = 0xC9). After
the DMR entry and after the FM transmit set-up the IF host rewrites 4:0xE0 as `(value & ~0x40) | CS7000P_E0_MIC`,
with `CS7000P_E0_MIC` 0x02: the input on pin 5 by elimination from the pin list, unverified, the first thing to
try on the bench (open question 7). Bit 5, the input the MCU's tone DAC reaches, stays clear.

### 6.5 The reference oscillator depends on the chip

The 16.8 MHz reference (X601) takes its control voltage from an amplifier with two inputs:

```
VC = 2.975 V - 0.82 x MOD2 - 0.082 x CTC/DCS_OUT        (for a 5.0 V synthesizer rail)
```

MOD2 is the HR_C6000's DAC on pin 75. So **the radio's frequency, in receive too, depends on the chip being awake
with that DAC on**, on the synthesizer rail (the amplifier's supply and reference), and on PC8's mean level
(section 7.6).

The driver's set-ups leave 4:0x25 = 0x0E: with it the pin 75 DAC is on only while the chip transmits. With
`ref_dac_hold` the IF host writes 4:0x25 = 0x0C after every mode set-up and at every receive entry, so the DAC stays
on while receiving, and its level, the two-point bias 4:0x47 / 0x48, as 512 (mid-scale) plus the reference offset,
`CS7000P_REF_OFFSET`, which is 0. While the chip transmits the DAC idles at mid-scale plus the same offset in
4:0x04.

**Measured on one radio (2026-11-22).** With the DAC off (the first image wrote 0x26, the manual's recipe, which does
nothing on this pin) MOD2 sits at 0 V, the control voltage near 2.84 V, and the receiver is about 10.5 kHz high at
433.5 MHz: a 433.500 MHz carrier is centered with the radio tuned to 433.490 MHz, and the squelch never opens on
channel. With the DAC on at mid-scale the same carrier is centered on 433.500 MHz (signal and noise readings equal to
the passband's center). The second image wrote the right control word but left the bias at 0: the DAC was on at 0 V
and the squelch still did not open on channel. The bias is now mid-scale plus the offset. PC8's duty moves the passband by about 2 kHz end to end, which gives about 8 kHz per volt of
control voltage at this frequency. The table of control words is in
[hrc6000-registers.md](hrc6000-registers.md) 3.22.

**The maker's coding, for a receive test.** The maker's program keeps 4:0x25 = 0x0E and 4:0x26 = 0x7D and puts a
signed trim in the bias ([calibration-cs7000p.md](calibration-cs7000p.md) 5.2, 5.13, 8 item 1). Which coding the pin 75
DAC has under 0x0E in receive is open. The measured hold stays the default. `chip_reg` 3:5 (section 14) selects the maker's
state through the IF host's `ref_coding` option ([modem-hrc6000.md](modem-hrc6000.md) 8.2): 1 is 0x0E with the
level the signed offset, 2 the same with 4:0x26 = 0x7D. RAM only, 0 after every start, taken at the next receive set-up
in full, and while it is not 0 every transmission is refused (`why=14`, 13.2). 4:0x04 in receive stays the reference
offset (the IF host's `rx_tx_offset`): whether 4:0x04 moves the held level under 0x0C is not measured. The one
exception is DMR on the arm's top stage (13.2): there 4:0x04 is trim B at every DMR receive entry, the value the
bursts carry.

**Trim A in the receive level.** With the maker's table in use (13.1) the held level in receive is 512 + d_A, d_A the
table's trim A less 128 (that page's 5.2, the inferred equivalent under the hold), only while |d_A| is at most 32
steps (`CS7000P_MAKER_RX_TRIM_MAX`, about 650 Hz at 433 MHz): mid-scale was on frequency on the first radio to a few
hundred Hz, so a trim farther than that says the coding or the table is not this unit's, and the level stays 512 +
`CS7000P_REF_OFFSET` (`board.ref` `why=rx_bound`, 15). On the first radio (2026-11-24) trim A was inside the bound
and in use (`src=maker`), and FM receive sounded as clear as on mid-scale; no counter or A/B reading was made. Only the level: 4:0x04 keeps the reference offset in receive,
and the FM transmit set-up's 4:0x04 is the same as without the table. `chip_reg` 3:4 sets both over trim A, and 0x100
gives them back (14); never while transmission is armed (13.2). Under 3:5's maker coding the same offset is written as the signed level. A new level is
written low byte first, so a step across a byte of it (512 to 507) passes one value a byte away for one bus write,
as a 3:4 change always did; with a table that happens only at the receive set-up after a restore, as a start writes
the level before the hold opens the DAC.

**Still open** (open question 1): the offset in 4:0x04 that puts this unit exactly on frequency (the passband is
flat near its center, so it needs a frequency counter on the transmitter or a narrower measurement), the same for
other units, and what the DAC does in a DMR TX slot. Until the offset is measured, receive and transmit may sit a
few ppm off. The control amplifier settles through 1 uF into about 6 k: nothing transmits and no frequency
is measured until `CS7000P_REF_SETTLE_MS` (30 ms) after the synthesizer rail came on (open question 35).

---

## 7. RF

### 7.1 The chain

One fractional-N synthesizer with two discrete VCOs, a double-conversion receiver, a transmitter with the VCO on the
carrier, and a PA whose **supply current** is regulated. There is no RF power detector and no reverse-power
protection.

| Block | Parts | Notes |
|---|---|---|
| Synthesizer | SKY72310 (IC607), 16.8 MHz VC-TCXO (X601), fixed loop filter | lock detect on PD11; Mux_out is not wired |
| VCOs | Q3 (below about 430 MHz), Q4 (above it), one tuning line | `EXP_VCO_LOW` powers one or the other, never both, by the frequency the VCO runs at (7.2). The drawing's note "T:0V R:3V" names them by direction |
| Receive | tracking band-pass filters, LNA, mixer, 49.95 MHz crystal filter (XF201), IF amplifier, AK2365A (IC200) | second LO 50.4 MHz = 3 x 16.8 MHz from a tripler on the receive rail; second IF 450 kHz to the HR_C6000 |
| Transmit | two pre-drivers, a driver and a final FET | their drains are on the battery at all times |
| T/R switching | the 5T and 5R rails | the LO routing and the antenna switch follow the rails through PIN diodes. No control line of their own |
| GPS | tapped from the main antenna | |

Rails:

| Rail | Expander bit | Feeds |
|---|---|---|
| 5C | `EXP_SYNTH_RAIL` | charge pump, VCO supply, LO buffers, the modulation amplifiers and their reference |
| 5R | `EXP_RX_RAIL` | LNA, mixer, IF amplifier, the tripler, **and the amplifier after the HR_C6000's line out** |
| 5T | `EXP_TX_RAIL` | pre-drivers, the LO into the transmit chain, the antenna switch |
| APC amplifier supply | `EXP_APC_LOOP` | the loop's op-amp: without it the PA gates are at 0 V |
| 5C_A | `EXP_AUDIO_RAIL` (needs 5C) | the receive audio filters behind the ADC taps |
| always on | | the synthesizer's 3.3 V, the reference oscillator, the AK2365A's 3.3 V regulator |

### 7.2 Frequency plan

`rfplan_cs7000p.c` (pure, `cs7000pRfTest`):

| Item | Value | Status |
|---|---|---|
| Reference | 16.8 MHz | read (printed at X601) |
| First IF | 49.95 MHz | read (printed at XF201) |
| Second LO, second IF | 50.4 MHz, 450 kHz | derived |
| First LO side | **low**: LO = f - 49.95 MHz, 350.05 to 477.05 MHz. `CS7000P_LO_HIGH` 0 | **open**: only the VCO tanks' part values point to it. A bench switch flips it together with the I/Q order (section 14, 3:3). Open question 2 |
| Transmit | the VCO on the carrier | read: no multiplier |
| VCO select | `EXP_VCO_LOW` set for 400.0 MHz <= f_rx < 479.00001 MHz in receive and for 400.0 MHz <= f_tx < 430.00001 MHz in transmit, else clear (`cs7000p_vco_low`, compared in 10 Hz units). Both switch points put the VCO itself at about 429-430 MHz (the receive LO at 479 MHz is 429.05 MHz). Through the sequencer's `vco_line` op: the line moves only with the transmit rail off and the loop open. With the LO above the signal (3:3), and with `chip_reg` 3:6 = 1, receive keeps the line set (the rule this port ran before); transmit always follows f_tx | read: the maker's rule ([calibration-cs7000p.md](calibration-cs7000p.md) 5.12). Agrees with the lock at 433.5 MHz (line set, LO 383.55 MHz); 433.5 MHz receive and transmit write exactly what they wrote before (`cs7000pRadioSeqTest`). Seen under the rule: 433.5 MHz receive with the line set (`rx_cal ... vco=low vco_rule=freq`, bring-up First radio). Not seen: either switch point, receive over 479 MHz, any transmission (bring-up) |
| Reference divider | 2 for the whole band (`sky72310_pick_ref_div()`): 8.4 MHz at the phase detector, N 41.7 to 61.0 (receive LO 350.05 to 477.05 MHz, transmit 400 to 512 MHz; up to 68.7 with the LO above the signal, 449.95 to 576.95 MHz), step 32.04 Hz | datasheet |
| Phase detector gain, Mux_out | 31 and tri-stated (`SKY72310_MUX_OFF`: the pin is not wired) | open: the gain that suits this loop filter is unknown. Open question 3 |
| Lock | PD11 high. Three samples 100 us apart must agree; timeout 5 ms | datasheet, derived |
| Reference trim | applied to the frequencies in the plan (`ref_trim_tune_hz`): exact for the first LO and the carrier. The second LO is three times the same oscillator and is not corrected: about 50 Hz of IF offset per ppm (open question 31) | design |

Bands (`board.c`, with the edges of `rfplan_cs7000p.h`, the same constants the transmit check uses):

| Range | Receive | Transmit | Source |
|---|---|---|---|
| 400.0-405.9875 MHz | yes | yes | the FCC grant's first row, 400.0-406.0 MHz |
| 405.9875-406.1125 MHz | yes | **no** | the gap between the grant's rows, 406.0-406.1 MHz (the distress beacon band), plus half a 25 kHz channel on each side |
| 406.1125-512.0 MHz | yes | yes | the grant's second row, 406.1-512.0 MHz |
| 512.0-527.0 MHz | yes | no | receive to 527 MHz is the maker's manual ("the frequency of this radio is 400-527 MHz") |

The grant's rows bound the emission, while the board checks the carrier: hence the 12.5 kHz in from each inner edge,
so a 25 kHz channel's emission stays out of 406.0-406.1 MHz.

Two things follow from edges that are not on a whole MHz. A VFO band scan in the 406.1125-512 MHz row restarts at
the row's first frequency on the VFO step's raster: 406.1125 MHz at 12.5 kHz, 406.125 MHz at 25 kHz
([radio-features.md](radio-features.md) 2). A refused transmission shows "Bands 400-406 406-512": the edges are
shown to the nearest MHz.

Power levels: `1W` and `4W`, the FCC Part 90 report's low and high. The maker's M17 manual says 1 W or 5 W; the
names follow the certified figures. `power_default` is the low one. Whether the synthesizer locks over the whole
range is a bring-up measurement.

### 7.3 AK2365A

| Register | Value | Meaning |
|---|---|---|
| control 1, wide | 0xF1 | mode 7 (IF buffer, discriminator, noise squelch), wide level, +/-6 kHz filter, 50.4 MHz LO |
| control 1, narrow | 0xE9 | the same with the narrow level (BAND 0) and the +/-3 kHz filter |
| control 2 | 0x1D | the datasheet's required AGC setting, calibration start |
| control 3 | 0x00 | IF buffer gain 6 dB. The level the HR_C6000 wants is open (open question 9) |
| control 4, control 5 | 0x01, 0x80 | |

Frame: 16 clocks, the driver's `reg << 9 | data` with the empty slot after A0. The chip's 3.3 V regulator is always
on; `EXP_AK_RUN` is its power-down pin and PC14 its reset. Its filter calibration needs the 50.4 MHz LO, so the
receive rail comes on before the set-up. The set-up's words and their order are the shared driver's
(`ak2365a_setup_words`); the board writes them, with the 500 us ahead of control 2 (`rfplan_cs7000p.h`,
`cs7000pRfTest`).

For a receive test of the maker's squelch and RSSI points under the words they were set under, `chip_reg` 3:7 = 1
(14) writes the maker's words instead ([calibration-cs7000p.md](calibration-cs7000p.md) 5.13), one configuration per
mode (`cs7000p_if_cfg_maker_fm`, `_dmr`):

| Register | FM wide | FM narrow | DMR |
|---|---|---|---|
| control 1 | 0xF5 (filter F2) | 0xE9 | 0xE5 (narrow level, filter F2) |
| control 2 | 0x1F | 0x1F | 0x1F |
| control 3 to 5 | the port's | the port's | the port's |

Control 2 is the maker's AGC setting, 0x1E, with the calibration start (0x01): this port's set-up begins with a
software reset, after which the discriminator needs its calibration. The maker's control 3 to 5 are not known. The
chip only receives, so the knob neither refuses nor changes a transmission; a mode change with it on asks for a
receive set-up (the DMR word). `cs7000pRadioTest` reads the words off the chip's pins for each.

### 7.4 Buses

| Chip | Lines | Half period | Notes |
|---|---|---|---|
| SKY72310 | data PD8, clock PD10 (idle low), select PD9, each behind 100 R | 1 us | `wire3_gpio` wrapped as the driver's bus |
| AK2365A | clock PE3, data PC13, each behind 1 k; **select = expander bit 0** | 2 us (500 ns minimum, slow pads) | `wire3_gpio`, with a read for the bench. PC13 is released after every frame (2.7) |

Half periods are `stm32_wait_ns()`. Both buses are used from the radio task, except the synthesizer write inside a
DMR burst, which runs at interrupt level, and the bench's `chip_reg` writes and reads, from the debug link's task.
Every frame runs with the RTOS interrupt mask held (a synthesizer word about 36 us, an AK2365A frame about 0.12 ms
with the two expander updates of its select), so frames never mix and PC13's change of direction never interleaves
with another task's set-up of a port C pin. The synthesizer's data line is parked low between frames.

### 7.5 The PA loop and the shared DAC

`APC/TV` (PA4, DAC1 channel 1) goes to two places, and nothing switches it:

- the PA loop's reference, divided to 0.178 x the DAC voltage. The loop compares it with 0.26 V per ampere of
  driver and final current;
- the tuning line of the receive front end's four varactor filters, through 100 k.

In receive the loop's amplifier is unpowered; in transmit the front end is. So **a tuning value left on the DAC is
a power request the moment the loop and the transmit rail come on.** The interlock of section 4.3 holds the rule at
the sink, and `drivers/rf/chainseq` keeps the order on every path:

- the DAC is 0 (or at the floor) and the loop is open whenever the transmit rail changes;
- the VCO select line changes only while the transmit rail is off and the loop is open (it follows the frequency,
  7.2, not the direction);
- the DAC never carries a tuning value while the loop is closed;
- every error path, a refused write included, ends in the safe state.

The hardware bound: at DAC full scale the loop's reference is 0.58 V, so the loop limits the PA current. Nothing
limits reverse power. A first transmission needs a dummy load, a current-limited supply and the lowest drive first.
On the maker's table the arm's first stage keys the low level only, on the drive the factory set (13.2); without a
dummy load that carrier goes to the antenna. An antenna (or a load) must be on the connector before any key-up, and
a first transmission without a load or a power meter is the owner's decision (bring-up stage 11.1).

### 7.6 PC8: sub-audio out, and part of the reference

PC8 is TIM3 CH3 behind a 2-pole RC filter, summed into the reference oscillator's control amplifier with gain
0.082: its whole swing moves the control voltage by 0.27 V. The net has no pull resistor.

- From `board_early_init()` on, PC8 carries a PWM at **50 % duty, 93.75 kHz** (96 MHz / 1024), and is never
  released. The radio's frequency calibration holds for that level only (open question 4).
- The sub-audio generator (`subtone_cs7000p.c`) sets one duty value per ADC scan tick, 2,400 a second:
  512 / 1024 plus the sample scaled by the transmission's depth, in per mille of half the period, clamped so it never
  reaches 0 or the period (`subpwm_cs7000p.h`, `cs7000pSubPwmTest`). The depth: on the maker's values the table's
  level for the tone at f_tx (13.2); otherwise the build's `CS7000P_CTCSS_DEPTH_PM` (300), the same on both
  bandwidths. The old option `CS7000P_CTCSS_DEPTH` (a percent) fails the build.
- DCS polarity: `CS7000P_DCS_INVERT` 0 on transmit, `CS7000P_DCS_RX_INVERT` 0 on the detector's input; both
  unverified, and the receive one may follow the first LO's side.
- Receive: the sub-audio filter's output on PA2, scanned at 2,400 Hz, into the shared detector (CTCSS Goertzel, DCS
  slicer). It needs `EXP_AUDIO_RAIL`.

### 7.7 Start-up

`hal_radio_t.init`, at task level, in this order:

1. The sequencer's safe state. The calibration verdict is computed once.
2. `EXP_SYNTH_RAIL` on; wait `CS7000P_RAIL_SETTLE_US` (2 ms). The time is noted for the reference settle (6.5).
   Then the synthesizer driver's power-up words (`sky72310_init`), and the burst runner and the key-up rule
   (`pa_keyup_init` with 2020 us as the earliest point, `dmr_keyup_init`).
3. The HR_C6000: its pins, `EXP_DMR_SLEEP` off, 12 ms, then `hrc6000_if_init()` with this board's values
   ([modem-hrc6000.md](modem-hrc6000.md) 8.2): the chip's FM audio, the reference hold, the codec input bits, the
   I/Q order for the LO side, the modulation pair and reference offset from the calibration tables.
4. The AK2365A: `EXP_AK_RUN` on, 1 ms, reset released (PC14 high; it was low since the pin set-up), select high
   throughout.
5. The sub-audio detector and generator.

### 7.8 Receive

| Step | Action | Reason |
|---|---|---|
| 1 | Refuse outside 400-527 MHz, nothing written | |
| 2 | Safe state: DAC 0, `EXP_APC_LOOP` off, `EXP_TX_RAIL` off, `EXP_VCO_LOW` set | the DAC is about to become a tuning voltage: the loop must be open first |
| 3 | `EXP_VCO_LOW` (set in step 2, the safe state's park) cleared when f_rx is over the receive switch; `EXP_SYNTH_RAIL` on (settle if it was off) | the VCO for the LO, before the words (7.2) |
| 4 | Synthesizer to f - IF; wait for lock, at most 5 ms. Unlocked is counted and receive goes on | |
| 5 | `EXP_RX_RAIL` on; 1 ms | the tripler is the AK2365A's LO |
| 6 | AK2365A set-up: software reset, control 1 (the operating mode), 500 us, control 2 (the AGC setting and the calibration's start), control 3 to 5; then 3 ms (calibration 1.3 ms, settle 1.5 ms). In DMR the narrow filter and level are used whatever the channel's width says | the data sheet's calibration procedure: the circuits it runs on are up 500 us after the mode is set |
| 7 | DAC: the front-end tuning value: `chip_reg` 3:0's, else the maker's tune row at f_rx (13.1), else the model table's or `CS7000P_RX_TUNE_NOMINAL` (2048); clamped to codes 250 to 3850 | the buffer's range: 0.2 V from either rail |
| 8 | `EXP_AUDIO_RAIL` on | the sub-audio filter |
| 9 | The HR_C6000's mode and retune. FM: the chip plays its own FM audio. DMR: the pair, the engine, a receiver restart on a new channel. The reference hold in both | |
| 10 | The sub-audio detector reset, unless the channel keeps it | |

`rx()` during a transmission ends its parts first (the tone, the burst runner), and the sequencer brings the RF down
in order inside the receive set-up.

### 7.9 FM transmit

| Step | Action | Reason |
|---|---|---|
| 0 | Refuse unless calibrated, a bench drive override is in force, or the owner's arm covers a key-up on the maker's table (section 13); refuse outside the two transmit ranges; refuse below the critical battery voltage; refuse until the reference has settled | never key without calibration |
| 1 | DAC 0 | it held the tuning voltage |
| 2 | `EXP_RX_RAIL` off, sub-audio detector paused | receiver and tripler off |
| 3 | The HR_C6000's FM transmit set-up with the pair and reference offset, then the codec input bits; on the maker's table the maker's fixed registers after them (13.2). Then `EXP_MIC_POWER` and `EXP_MIC_INT` on | modulation ready before RF. Under a bench override with no pair known the carrier is unmodulated. A failed write of the transmit set-up counts as a refusal (`why=9`) |
| 4 | `EXP_VCO_LOW` for f_tx (set under 430.00001 MHz); synthesizer to the carrier; wait for lock. **Unlocked: back to receive, refused** | the transmit rail is still off (7.2) |
| 5 | `EXP_TX_RAIL` on; 500 us | pre-drivers, LO, antenna switch |
| 6 | DAC at the floor, `EXP_APC_LOOP` on, then a ramp of 8 steps of 250 us to the drive | the loop closes on a zero reference |
| 7 | Sub-audio generator on, at the depth chosen at step 0 (on the maker's table its level for the tone at f_tx, 13.2) | the tone's depth is chosen before anything is written, so a tone channel without a level writes nothing |
| End | DCS turn-off, ramp down, `EXP_APC_LOOP` off, `EXP_TX_RAIL` off, the whole receive sequence of the RF chain, then the microphone off and the HR_C6000 back to FM receive (the maker's fixed registers written back first), its retune and the sub-audio detector | the modem leaves its transmit set-up only after the PA is down: its DAC is the reference oscillator's control |

### 7.10 DMR transmit

`tx()` arms the shared burst runner (`platform/stm32/dmr_keyup`) with the same refusal. The runner's plan (task level,
once per transmission, where f_tx is first known) then gives the modem the modulation pair for f_tx and the 4:0x04
the bursts carry (`hrc6000_if_tx_freq`, [modem-hrc6000.md](modem-hrc6000.md) 8.2): the receive entries wrote the
pair for f_rx, which a split channel's differs from. Without a pair the safe gain 0x80 / 0x80, as before; on the
maker's values its pair or a refusal (`why=9`). A failed write is a refusal too. The plan takes its drives before
these writes, so a plan refused for its drive (0 once the arm no longer covers the transmission) writes nothing. 4:0x25 in a DMR slot stays the
port's hold, rewritten after each burst sequence: the maker keeps 0x0E there, and what the pin 75 DAC does in a DMR
TX slot under either word is not measured (open question 1). Per burst, at interrupt
level: receiver off, the VCO line for f_tx, synthesizer words, `EXP_TX_RAIL` with the DAC off; the loop closed with
the DAC still off; then the drive points; the PA drop; back to the receive LO with the line for f_rx. A drive point
before the loop point would be refused by the interlock. The PA point of a burst is refused when the lock pin does
not read locked three times in a row. After each burst sequence the reference hold is written again. `tx_off()`
after bursts runs the whole receive set-up (the last switch back has none for the AK2365A); the modem glue's
`dmr_tx_end` keeps the receiver as that switch left it. A refused re-arm behind an armed transmission sets the
receiver up again at once.

At the arm `EXP_MIC_POWER` and `EXP_MIC_INT` come on, as in FM's step 3: the microphone's bias settles before the
first burst. They go off when the arm ends: `tx_off()`, `dmr_tx_end`, a refused arm or re-arm, a retune or a mode
change. The external microphone (`EXP_MIC_EXT`) is never selected, in FM or DMR: nothing tells when one is plugged
in. `cs7000pRadioTest` runs `radio_cs7000p.c` over the real chain for the microphone in both modes.

The key-up constants are a fixed 800 us lead, unmeasured. `tools/bench/pa_cal.py` measures them on a radio, but only
once the power calibration is built in: the PA rows, the DMR modulation pair and the reference offset (13.2), with
`CS7000P_BENCH_DMR_TX=1` for its open-loop keys. Before that it stops while the view's `tx_drive` is 0. `chainseq`
and `dmr_keyup` are new shared code no radio has run (open question 25).

### 7.11 Status readings

| Reading | From | Notes |
|---|---|---|
| RSSI | AK2365A RSSI on PB0 | with the maker's table in use, its two points on the 12-bit reading (0x040 / 0x041 at -120 / -70 dBm, inferred: [calibration-cs7000p.md](calibration-cs7000p.md) 5.8); else the datasheet's two typical points at the chip's input (0.3 V at -100 dBm, 2.0 V at -30 dBm) and `CS7000P_FE_GAIN_DB` 0, not an absolute level. The S-meter and the DMR scan's strength test read it; the FM squelch reads the noise |
| Noise | AK2365A noise rectifier (net BUSY) on PB1, raw >> 4 | larger = more noise; 255 while not receiving. There is no digital "busy" line |
| Tone | the sub-audio detector | |
| `carrier` | RSSI at or over -118 dBm while receiving | a placeholder (the figure of `boards/md380/radio_md380.c`): on the datasheet's scale every reading is over it, so it is true whenever the receiver is on; on the maker's table's scale it is a real threshold |

After a `board_rf_safe()` that returned, the next status poll outside a transmission, the receiver's sleep and the
power's off state sets the receiver up again and re-applies the speaker bits, whatever the radio was asked to do in
between (a key-up that was refused does not lose it). After a power-off the knob overrode, the off state (5.4) holds
that restore back: the receiver stays idle, the speaker amplifier off for the squelch and for beeps, and no LED
comes on, until `latch(true)` ends the off state or the knob watch restarts the radio. `tx()` is refused in it
(`why=12`, 13.2); `rx()` and the backlight are not gated by it.

Receive sleep turns the receive rail off; the synthesizer rail and the AK2365A stay on, so the reference and the
filter calibration hold.

---

## 8. Audio

| Path | Circuit | In the port |
|---|---|---|
| Speaker | HR_C6000 line out, a buffer on the **receive rail**, the volume pot, a bridge amplifier (TDA2822) | `audio(on)`: `EXP_SPK_INT_OFF` clear, `EXP_AMP` set. Off: `EXP_AMP` clear (the amplifier's supply is the only mute that works). The same for the PCM route (a beep). The HR_C6000's FM audio feed-through (4:0x36) is open only while the squelch is open and no PCM plays: the IF host leaves it open in FM receive whatever the squelch does, and a beep with the squelch shut would otherwise play over the receiver's noise (`speaker_cs7000p.h`, `cs7000pSpeakerTest`) |
| FM receive audio | the chip demodulates the 450 kHz IF | no analog path from the discriminator to the speaker |
| Beeps, DMR audio | PCM over I2S to the chip, out of its line out | **silent while the receive rail is off** (transmit, sleep): the line-out buffer has no supply. A known limit; the MCU's tone DAC on PA5 is the later answer |
| Accessory speaker | switched by `EXP_SPK_EXT_OFF` | stays disconnected: what PHONE_DET means is open |
| Microphone | internal (`EXP_MIC_INT`) or accessory (`EXP_MIC_EXT`), an amplifier on its own rail (`EXP_MIC_POWER`), to the chip's pin 5 and to PA7 | internal only. `hal_radio_t.mic_gain` is not offered: the chip's microphone gain steps (4:0xE4, 0 to 31) are in no document, so FM transmit uses one value, 7 |
| Volume | an analog pot before the amplifier | not settable (`volume` is NULL) |
| MCU taps | discriminator audio on PA3, voice band on PC1, microphone on PA7 | not scanned yet |

The PCM stream is DMR-only code (`audio_cs7000p.c` builds under `CONFIG_DMR`): a build without DMR plays FM through
the chip's line out and has no beeps. With the I2S direction probe at no verdict, the same (section 6.3).

Not written for this radio: the HR_C6000's transmit tone path (roger beep, 1750 Hz, DTMF, AFSK), APRS receive, the
audio probe.

---

## 9. Display, keys, selector

### 9.1 The assumed LCD board

The maker's schematic stops at the 18-pin connector J801, "TO LCD BOARD". The display, the key matrix and the
backlight driver are on a second board that it does not show. The OEM's 2018 service manual draws that board for
the older model ("lcd V03"), with a connector of the same 18 names in the same order. Everything in this section
assumes it.

| Connector | MCU pin | Main-board net | On the assumed board |
|---|---|---|---|
| VCC | | the switched battery rail | the display's 3.3 V regulator, the key lights' 5 V regulator |
| LAMP | expander bit 16 | LAMP | the key lights' regulator enable |
| LCD-LED | PC9 behind two RC stages | LCD-LED | the base of the backlight transistor |
| LCD-5 | PE1 | LCD_DB6 | display **chip select**, 1 k |
| LCD-4 | PE0 | LCD_DB7 | display **reset**, 1 k, with 10 k to ground |
| LCD-3 | PD12 | LCD_RES | display **D/C**, 1 k |
| LCD-2 | PD13 | LCD_A0 | **write strobe**, 1 k |
| LCD-1 | PD14 | LCD_CS1 | **read strobe**, 1 k |
| K9 | PB7 | K9 | key column 5, 1 k |
| K8-K5 | PD7-PD4 | K8-K5 | key columns 1-4 and display data D7-D4, 1 k each way |
| K4-K1 | PD3-PD0 | K4-K1 | key rows D-A and display data D3-D0, 1 k each way |

The main board's net names for the five control lines come from an older serial display and do not say what a line
does on this board.

### 9.2 The board check

The 1 k series resistors that make a pressed key or a wrong pin function harmless are parts of the assumed board.
On another board two push-pull outputs could meet. So **every MCU line of the connector except LCD-LED (PD0-PD7,
PB7, PD12-PD14, PE0, PE1) is an input with a pull-up until this check has passed.** LCD-LED (PC9, behind the main
board's RC stages) and LAMP (expander bit 16) are held low, which is off, from reset, and stay low on a failed
check:

1. PD12, PD13, PD14, PE0 and PE1 are set to inputs with pull-ups and read twice, 1 ms apart. On the assumed board
   exactly PE0 reads low (its 10 k pull-down against the pin's 40 k pull-up), in both readings: a line that moves
   between them is `unexpected`. `cs7000p_lcd_board_ok()` in `panel_cs7000p.h` is the rule;
   `cs7000pPanelTest` runs all 32 patterns.
2. That pattern: `board.lcd_board=assumed levels=0x1e`. The display bus and the key scan start. `levels` has one
   bit a line, 1 = high: bit 0 PE0 (reset), bit 1 PE1 (chip select), bit 2 PD12 (D/C), bit 3 PD13 (write strobe),
   bit 4 PD14 (read strobe). Nothing on the connector reads `0x1f`; on a failed check it is the reading that failed.
3. **Any other pattern: `unexpected`.** No line of the connector ever becomes an output: no display bus, no ID read,
   no key matrix scan. The display functions do nothing and the backlight stays off. The radio runs by the debug
   link, the side keys, the top key and PTT. The log has `display: LCD board unexpected (levels xx), not driven`.
4. The bench knob `CS7000P_LCD_FORCE=1` skips the check (`forced`). Use it only after the board has been traced.

### 9.3 Display

- Bus: 8-bit 8080 on GPIO (`platform/stm32/lcd8080_gpio`): data on PD0-PD7, write strobe 100 / 100 ns, read strobe
  1000 / 1000 ns. The data lines go back to inputs with pull-ups between sessions, because they are also the key
  matrix. One recursive mutex covers the bus and the key scan.
- Controller: **open**. Nothing names it. The port reads its ID with no key down (reset low 20 ms, then 120 ms;
  0x04 and 0xDA to 0xDC, each after its dummy byte). The matrix is scanned first and a held key is waited for, up to
  500 ms; then the ID is read anyway and the log has `display: a key is held, the panel ID may read wrong`. The
  port picks a type of `drivers/display/st7735` from the ID (`st7735_panel_from_id()`). A panel that does not answer
  the ID read (still in reset, no RD strobe, a select or D/C of the other polarity) reads all zeros: the bus drives
  the data lines low before it turns them round, so a line nobody drives keeps 0, not the command byte. A recognized
  ID gets its type. An unknown one, all zeros or all ones included, gets type 0, shown as `panel=unknown
  source=default`:
  the standard init is sent, and the radio works by the debug link whatever the panel shows. `CS7000P_PANEL_TYPE`
  (reported as `source=build`) and `CS7000P_PANEL_MADCTL_XOR` override. The color order is the driver's own for the
  type (type 3 is BGR); for this panel it is as open as the offsets.
- Size: 160 x 128, 16 colors (4 bits a pixel), offsets and insets 0. The only number in a document is the maker's
  PC tool: "Welcome Image: maximum support 160 * 128 pixel size". Open question 10.
- Backlight: TIM8 CH4 on PC9 at 40 kHz: the period is the timer clock / 40,000 (2,400 clocks at 96 MHz), computed,
  so the fallback clock trees give 40 kHz too. Set up dark in `board_early_init()`, before any task (4.2, step 8); a
  timer clock that gives no 40 kHz period leaves the pin a low output and the backlight off. The assumed board turns
  the filtered level into a base current: dark below roughly 15 to 20 % duty, saturated well before 100 %
  (estimates). `set_backlight(percent)` maps 1 to 100 % onto `CS7000P_BL_MIN` to `CS7000P_BL_MAX` (20 to 70 % duty),
  and 0 is the pin low. With the pin floating (reset, the ROM bootloader) the transistor biases itself on: the lit,
  blank "gray" screen. Open question 12.
- Key lights: `EXP_LAMP` follows the backlight being on. Two tasks set the level, the UI and the power-off, so the
  backlight level, the key-light state and `EXP_LAMP` change in one interrupt-masked section: the last caller wins
  for all three, and the key lights never stay on under a dark panel.
- The panel loses power with the radio: a full init at every start.

### 9.4 Keys

Matrix (`platform/stm32/gpio_keypad`): K5 to K9 are driven low one at a time and released otherwise; K1 to K4 are sensed
with pull-ups; settle 20 us. The scan runs only while no display session is open, and not at all on an `unexpected`
board. A scan that finds a session open is skipped: the matrix keys keep their state, and the side keys and PTT are
still read. After three skipped scans (30 ms) the next one waits for the bus. Two scans that agree make a change (matrix
and direct keys apart). So a matrix key held at power-on behind a busy display is first reported at the 8th scan at the
latest (3 skipped, a read, 3 skipped, the settling read), inside the core's start-up key guard of 10 scans
([information-architecture.md](../ux/information-architecture.md) 2). `board.keys_raw` is five bytes in scan order, one
a driven line (K5, K6, K7, K8, K9); bit n of a byte is sense line K(n+1) low. Key 1 (K8 x K1) reads `0000000100`.

| Sense \ drive | K8 (PD7) | K7 (PD6) | K6 (PD5) | K5 (PD4) | K9 (PB7) |
|---|---|---|---|---|---|
| K1 (PD0) | 1 | 2 | 3 | function switch 1 | function switch 5 |
| K2 (PD1) | 4 | 5 | 6 | function switch 2 | function switch 6 |
| K3 (PD2) | 7 | 8 | 9 | function switch 3 | - |
| K4 (PD3) | * | 0 | # | function switch 4 | - |

The six function switches carry no legend on the drawing. The radio's six are F1, Up, F2 above OK, Down, BACK.
Their order was read on one radio, each key held alone (`board.keys_raw`, 2026-11-22):

| Function switch | Crossing | `keys_raw` | Key |
|---|---|---|---|
| 1 | K5 x K1 | `0100000000` | OK |
| 2 | K5 x K2 | `0200000000` | BACK |
| 3 | K5 x K3 | `0400000000` | F1 |
| 4 | K5 x K4 | `0800000000` | F2 |
| 5 | K9 x K1 | `0000000001` | Up |
| 6 | K9 x K2 | `0000000002` | Down |

`CS7000P_FN_KEYS` = `{ KEY_OK, KEY_BACK, KEY_F1, KEY_F2, KEY_UP, KEY_DOWN }`. The twelve digit keys were reported
working at the drawing's crossings on the same radio. No diodes: three keys on the corners of a rectangle read as
four.

Direct keys, all low when pressed, each with a pull-up on the board:

| Key | Pin | Short press | Long press |
|---|---|---|---|
| SK1 (side, above PTT) | PD15 | Monitor | Monitor |
| SK2 | PE12 | A/B | user key 2 |
| SK3 | PE13 | user key 3 | user key 3 |
| Top key (TK) | PE10 | none | emergency |
| PTT | PA8 | PTT | |
| External PTT | PE11 | PTT, through `plugptt` | counted only after the line has read high for 100 ms: the accessory jack may be empty or hold a plug. A line that is low for 2 s without ever having counted is reported (`ptt_line_stuck`): the core flashes the LEDs red and green until the line is released once. An empty jack reads high (100 k pull-up) and is never reported |

Front keys: OK select, BACK back, Up and Down navigate with repeat, \* quick settings, # actions, digits, F1 user
key 1, F2 user key 4. Settable keys: SK1, SK2, SK3, F1, F2, TK. What the core does with the user keys by default:
1 and 4 step the zone on Home, 3 is the next power level, 2 held turns dual watch on or off.

**Channel selector.** A 16-position coded switch on four lines (endless, by the OEM's part name), not a quadrature
encoder. **Its code is in no document.** `board.chsw_raw` shows the four lines: bit n = ECNn's level (PE14, PE15,
PB10, PB11), an open contact 1. Read on one radio, one detent at a time over a whole turn (2026-11-22):

`F D C 8 9 1 0 4 5 7 6 2 3 B A E`

a Gray code: one line changes per detent, the last back to the first too. `keys_cs7000p.h` holds it as position by
code, `CS7000P_CHSW_VERIFIED` is 1 and `CAP_ENCODER` is set: `drivers/input/chswitch` gives the position, and the
difference modulo 16 becomes steps, the shorter way round, half a turn in one reading counted as 8 forward. A code
settles after 30 ms. The list runs clockwise, the core's forward: on the radio the channel goes up when the selector
is turned clockwise. `CS7000P_CHSW_VERIFIED=0` turns the selector
off; a table that is not a full set of 16 positions makes no steps (and `cs7000pKeysTest` fails).

PB10 (ECN2) is low on eight of the sixteen detents, in two runs of four (`8 9 1 0` and `2 3 B A`): there the software
entry to the ROM bootloader is refused on a loader other than 9.1 (section 2.5). Four detents either way from a
refused position always reach an allowed one, which is what the tools' hint says.

---

## 10. Data flash

IC613, W25Q256JV (JEDEC ID EF 40 19), 32 MB, on SPI4: clock PE2, data PE6 and PE5, select PE4 as a GPIO. 12 MHz
(48 MHz / 4), mode 0. /WP and /HOLD are strapped high; the part has no reset pin. PE2 and PE6 get the medium slope
after the driver's probe (`stm32_pin_speed()`): the probe itself runs on the family's default, the low slope.

### 10.1 4-byte addresses

The board builds with `-DSPI_NOR_ADDR4=1` ([build-config.md](../build-config.md#board-defines),
[gotchas.md](gotchas.md) 10.7 and 10.8):

- The chip keeps its address mode, its extended address register and a suspended operation through an MCU reset.
  The driver resets it by command at every start (`66h`, `99h`), unless it reports a suspended operation: then it
  is not used until a power cycle, and the log says so.
- Only the opcodes that always take four address bytes (`13h`, `12h`, `21h`). The mode commands are never sent.
- The extended address register is written back to 0 after every program and erase and after a read past 16 MB, and
  once more before power-off or a jump to the ROM bootloader.
- No status register is ever written.

### 10.2 Map

`store_cs7000p.c`: the layout the STM32 radios with a 16 MB flash use, in the low half of this part, so every
codeplug tool's addresses hold.

| Area | Address | Size | Note |
|---|---|---|---|
| Open-firmware "EEPROM" image | 0x000000 | 64 KB | sector 1 holds the maker's table: kept (below) |
| Settings ring | 0x011000 | 8 sectors | foreign-guarded |
| Journal | 0x019000 | 2 sectors | foreign-guarded |
| **Board sector** | 0x01B000 | 1 sector | the factory copy's record: the tree's own verified copy of the maker's table (magic `RFFC`, [settings-store.md](settings-store.md) 4.2), in the hole no CPS address maps to. Blank on the unit seen; no constant-address flash call of the maker's program falls there ([calibration-cs7000p.md](calibration-cs7000p.md) 2.4). Written once, by the firmware (10.3) |
| Unit record | 0x01C000 | 1 sector | reserved for the record of values the tree measures itself (13.3). Nothing reads or writes it yet |
| The maker's records | 0x01E000 | 2 sectors | kept whole (10.3). Never erased or programmed |
| Open-firmware "flash" image | 0x020000 to 0xE00000 | | |
| Native codeplug | 0xE00000 | 1 MB | |
| Staging | 0xF00000 | 256 KB | |
| 0x1000000 to 0x1FFFFFF | not in the map | 16 MB | read for the backup only |
| CPS calibration window | none | | `calibration_size` 0. That window is a copy an open-firmware CPS may write ([cps-protocol.md](cps-protocol.md) 2.3), so the maker's factory table, inside the EEPROM image (10.3), is not one |

The map's factory ranges (`hal_flash_map_t.factory`, [settings-store.md](settings-store.md) 4.1; the board builds
with `-DSTORAGE_FACTORY_KEEP=1`):

| Range | Holds | Kind |
|---|---|---|
| 0x001000-0x0011FF | the maker's factory table | 512 bytes of sector 1. The rest of the sector, the open-firmware DTMF settings (0x1400-0x1477) and APRS configurations (0x1588-0x1787), stays writable |
| 0x01E000-0x01FFFF | the maker's records at 0x01E000, 0x01E400 and 0x01F000 | two whole kept sectors |

`board.toml` has the same two ranges as `[[flash.protect]] mem = "data"` entries; `tools/boards.py check` compares
the two lists.

### 10.3 The maker's data, and the claim

The maker's firmware has used this chip. What it keeps there is known in part
([calibration-cs7000p.md](calibration-cs7000p.md) 2):

| Range | The maker's | Against the map | Kept |
|---|---|---|---|
| 0x000080-0x00021F | device information | sector 0 of the open-firmware EEPROM image, with that image's device info, callsign and DMR ID | **no** (below) |
| 0x001000-0x0011FF | the **factory calibration table**, 512 bytes, one copy, no checksum (section 13.1) | sector 1 of the EEPROM image, with its DTMF settings (0x1400) and APRS configurations (0x1588); inside CPS EEPROM part 1 | yes: its bytes |
| 0x01E000, 0x01E400 | two 0x33-byte records; the maker's program reads, writes and erases the sector | the hole no CPS address maps to | yes: the whole sector |
| 0x01F000 | a 0x80-byte record | the same hole | yes: the whole sector |

Where it keeps its codeplug (that page's 5.12 names one record of it), fonts and voice data is not mapped.

In short (each row is explained below). On the first radio (2026-11-24, bring-up First radio): the blank window
read and refused, the compare against the owner's backup, the restore through ops 4 and 5 (the kept rewrite of
sector 1 and the record), the window and the record after an update and a restart, and one codeplug write by the
site's Codeplug Editor with the table unchanged. Not yet on a radio: every other writer of sector 1 (the CPS tool,
TFTP, a DTMF or APRS edit, "start a new codeplug", staging), a tool's bytes for the range dropped, the journal's
replay after a power cut, the record written by the firmware itself, a refusal of the restore, the web flasher's
step:

| What | The rule |
|---|---|
| The table's 512 bytes | kept by every write of the firmware to sector 1: a tool's bytes for them are dropped and the write succeeds; the rest of the sector stays writable |
| The maker's records, 0x01E000-0x01FFFF | never erased or programmed |
| The tree's own copy | the record at 0x01B000 (magic, version, length, CRC), written once by the firmware: on a claimed region, from a window that passes the reader, into a blank sector |
| The table in use | the window when it passes the reader, else the record (`src=record`); none when both pass and differ (`why=conflict`) |
| The restore | one path: debug link STORAGE ops 4 and 5, from the owner's backup, refused over a valid table that differs; the tool's `storage factory --backup FILE --restore`, or the web flasher's step after a codeplug Restore |
| Not protected | the maker's device information in sector 0 (0x000080-0x00021F) |
| Going back to the maker's firmware | (1) the table restored with RadioFoundry still installed, (2) sector 0 through the CPS, (3) only then the maker's recovery (the end of this section) |

**The table's bytes are kept.** Every writer of the core reaches sector 1 through the kept rewrite of
[settings-store.md](settings-store.md) 4.1: the table's 512 bytes are carried over from the flash, the rest of the
sector comes from the writer, and the write succeeds. This holds for a DTMF or APRS edit (a whole-sector rewrite,
never a patch transaction: neither record leaves sector 1), a CPS write of EEPROM part 1 through the open-firmware
shim, the fast codeplug protocol and its resume, a TFTP codeplug commit and its resume, "start a new codeplug" and the
journal's replay at a start, an entry an earlier build left for sector 1 included (its bytes for the table never
land). Bytes a tool sends for the range are dropped: a CPS that writes part 1 from another radio's image, or a fresh
one, sees success, and the table stays (the view's `storage.factory` counts the drop, [debuglink.md](debuglink.md)
3.7). A write that differs from the flash only inside the range erases nothing. A CPS that verifies its write reads
the radio's own bytes there. A power cut anywhere in such a rewrite leaves the table in the journal as well as in the
sector, and the next start finishes it. The two sectors of the maker's records are never erased or programmed: a
write to them succeeds and changes nothing. The STM32 flash driver refuses an erase or a program in any of these
sectors that the storage layer did not open for one kept rewrite (`board.flash` `refused=`, section 15). The in-tree
tools agree through the `[[flash.protect]]` entries: the CPS tools send no byte of a range, and the web flasher's
codeplug editor and Restore keep them ([board-registry.md](../dev/board-registry.md), "Protected regions").

**The copy.** The board gives the core's factory copy ([settings-store.md](settings-store.md) 4.2) the table's range
(the map's first) and the board sector, 0x01B000, as its record; its checker is the table's reader (13.1): the reader's
state ok and every integrity rule. At each start, after the codeplug and before the radio, the copy reads the window
and the record twice each. The table in use is the window when it passes the reader, else the record when it holds an
ok copy (shown `src=record`), and none when both pass and differ (`why=conflict`: compare both CRCs with the backup).
The record is written once, by the firmware, when the region is claimed, the window passes and the sector is blank;
nothing else writes it but the restore (below), the one path that writes the window. The radio takes the table in use
at its init and parses it; a later change of it (only a restore makes one) is a request its task takes at its next
status poll, never
under a transmission, followed by a receive set-up. The view's `cal_table` and `cal_rec` (15) show the outcome, and the
log a line, `cal: maker table ok src=window crc=0x... rec=... fields=0x...`, or why none is used. A build without
`CONFIG_FACTORY_COPY` reads the window alone (`cal_rec=off`). If the window held another radio's valid table at the
first claimed start, that table is in the record now: compare `cal_table`'s CRC with the backup's 0x1000-0x11FF before
the first start of such a firmware. Receive uses the table in use (13.1) from the receive set-up that follows its take: after a restore, the one at that status poll (a receiver asleep in FM battery saving: its wake writes the new level, the poll after it the new tune). Nothing transmits on it unless the owner arms transmission (13.2).

**Another radio's table has no way out yet.** A CPS read through the shim of an earlier build includes 0x1000-0x11FF,
so a codeplug file saved from radio A and written to radio B with such a build puts A's valid table into B's window,
and B's first start with the copy writes it into B's record. The restore then refuses B's own backup
(`window_differs`, and `record_differs` after it), as policy asks: a valid table is never swapped by accident. The
identity gate passes (the maker's records at 0x01E000 are B's), so `storage factory --backup` prints a `diagnosis`
line when the backup's other ranges match the radio but a valid window or record differs from the file. No path
writes B's table back in that state; the arm's handshake (the backup's CRC against the table in use) keeps such a
radio from transmitting. A way back (an override of the two refusals behind the identity gate and the chip's UID) is
the owner's decision and not written.

**A table already lost stays lost until it is restored.** An earlier build of the port kept nothing: a CPS write of
part 1 after the claim replaced the table with the PC's bytes, and installing such a build again ends the protection.
Builds before the factory copy keep nothing, so a codeplug is never written with one; after one was, compare with
`storage factory --backup` once this build runs again. The record at 0x01B000 outlives such a build, and a blank or
damaged window comes back from it or from the backup, but not a window that then holds another valid table (above).
The first radio's window was found blank after such a write (its sha256 is that of 512 bytes of 0xFF): the reader
refuses it, nothing is copied from it (`cal_table=blank src=none`), and the record stays blank until the restore
below (bring-up stage 6, run on 2026-11-24: the window and the record now hold the backup's table). The owner's
whole-chip backup still holds the table. The firmware's one path back is the factory copy's restore, debug link
STORAGE ops 4 and 5 ([settings-store.md](settings-store.md) 4.2): the backup's 512 bytes loaded, checked against the
reader (13.1) and against the window and the record (a valid table there that differs is refused), then the window
written through the kept rewrite, read back, and the record written. The board refuses it while a transmission is up
or a table a restore wrote is not taken yet, and shows the result as `cal_rec`'s `restore=` (15). The owner's tool
is `tools/rfoundry_debug.py storage factory --backup FILE` ([04-programming.md](../user/04-programming.md#the-makers-calibration-inside-the-codeplugs-memory)):
it reads the window twice through STORAGE READ and prints the CRC-32 and sha256 of the window, the record's CRC-32
(op 3; the record is in no area STORAGE READ opens) and the file's 0x1000-0x11FF, and compares the maker's records
at 0x01E000-0x01FFFF with the file's. `--restore` runs ops 4 and 5 only when those records equal the file's (the
identity gate: nothing of ours writes them, so they are this radio's backup's and almost surely not another radio's;
`--no-identity` overrides it, and a file of the 512 bytes alone needs it), with the window CRC of its own read as the
handshake, then polls op 3 and reads the window back. Until a restore runs, the kept rewrite keeps whatever the
window holds, blank bytes included. A codeplug restore of the backup does not bring the table back: the CPS tools and
the web flasher's Restore send none of its bytes (`cps_client.py` names `storage factory` on stderr, the web flasher
names the range in its log). The web flasher's Restore then runs the same compare and identity gate itself and offers one button that runs ops 4 and 5 ([web-flasher.md](../dev/web-flasher.md#the-factory-calibration-step)). Under a firmware without the command, the CPS read of the codeplug regions
(`tools/cps_client.py --regions codeplug read FILE`) holds 0x1000-0x11FF, and its sha256 against the backup's says
whether the table is still there.

**Not kept: the maker's device information** at 0x000080-0x00021F. It shares sector 0 with the open-firmware layout's
own fields (device info 0x0080, callsign 0x00E0, DMR ID 0x00E8), so the owner can change the callsign and the DMR ID,
and "start a new codeplug" clears them. Going back to the maker's firmware therefore needs the whole backup written
back first: the maker's program checks nothing in the table (that page's 2.2) and would transmit on whatever it holds,
and a radio whose window is blank would transmit on codes of 255. The order: the table restored while RadioFoundry
still runs (`storage factory --backup FILE --restore`, above; the maker's program has no command an owner can use for
it), then sector 0 through the CPS (a codeplug
write of the backup's EEPROM image), then the maker's recovery. No tool in the tree writes the whole chip back yet;
the site offers no stock target for this radio.

The port also protects the chip another way:

- The board builds with `-DSTORAGE_CLAIM_FIRST=1` ([settings-store.md](settings-store.md) 5). The settings region is
  **foreign on every unit, at every start, whether or not it is blank**, until the owner claims it over the debug
  link. Until then nothing in the map is erased or programmed: no ownership mark, no settings, no codeplug or CPS
  write, no staging. The radio keeps its settings in RAM and refuses codeplug writes.
- `storage.region=foreign` with `storage.foreign_slots=0` is the normal state of a unit whose ring is blank, not a
  fault.
- The backup to take before the claim is of the **whole 32 MB** (`backup.spi_size = 0x2000000` in `board.toml`). It
  cannot be taken before RadioFoundry runs: the ROM bootloader reaches the program flash only, and no way to read
  the data flash under the maker's firmware is known. `STORAGE INFO` carries the claim-first flag
  ([debuglink.md](debuglink.md) 3.13), and the clients act on it ([settings-store.md](settings-store.md) 5): the web
  installer offers the whole-chip backup right after the install (it can be skipped) and claims only when it read
  all of it;
  `rfoundry_debug.py storage claim --out DIR` first reads all 32 MB through the CPS shim into one `dataflash_*.bin`,
  each 32 KB block twice and compared (minutes at USB speed), and claims only if all of it came back
  (`--skip-full-backup` leaves that out); the Codeplug Editor does not claim and sends the user to the installer's
  backup; the installer's restore of channels from a bundle does not claim either, nor do its channel step and the
  migration tools (`prepareStorage`, `writeConverted`), which write nothing and point to the backup of everything.
- Both whole-chip backups read the chip through CPS area 1. The firmware lays the live VFO A record over 0x7590 to
  0x75C7 only while the region is its own ([open-firmware-cps.md](open-firmware-cps.md) 2.2), so a backup before the
  claim holds the chip's own bytes there too.
- After the claim the core may write anywhere in the map but the kept ranges. Whatever else the maker's firmware kept
  there (its device information, and the data not mapped) is then at risk, and the backup is the only copy.
- A valid record of RadioFoundry's proves one claim. A radio that went back to the maker's firmware and returns is
  "ours" at the first start if a record survived: the backup is then the owner's job again.
- Only the factory copy writes the board sector, once, and only over a blank sector: a record of another kind there
  (`cal_rec=other`, `store=foreign`) is shown and never overwritten. The firmware reads the maker's table and shows
  it (13.1); receive uses three of its values, transmit its values only behind the owner's arm (13.2).

One thing the maker's documents do show: its programming software loads a repeater database to 0x780000 through
0xFB3FFF of this chip ("Loading the GPS Database"). 0x780000 to 0xF3FFFF of that range is inside the map's
open-firmware image, native codeplug and staging areas, so writes after a claim can overwrite it; 0xF40000 to
0xFB3FFF is outside the map and never written. The maker publishes the file to load it again.
Mapping the rest needs two backups taken across a codeplug write by the maker's software (open question 21).

What the maker's recovery procedure does not reload, and so what is lost for a return to stock if it was
overwritten: tuning data, fonts, voice prompts, pictures, the model and serial number.

---

## 11. GPS, USB, accessory connector

| Part | Wiring | In the port |
|---|---|---|
| GPS | IC805, a TAU1113 module, always powered. Its TXD to PC7 (USART6 RX); its RXD on PC6. No power switch, reset or 1PPS is wired. Antenna shared with the radio | `stm32_gps_cfg_t`: USART6, rates 9600 then 115200, 3 s of listening. The module's own default rate is open; the rate hunt is for that. `board.gps_baud`. Nothing is sent to the module |
| USB | PA11, PA12 to accessory contacts C-5, C-4. **On the accessory connector**: there is no USB socket, and the maker's programming cable carries it. No VBUS reaches the MCU; PA9 is the Bluetooth module's line | full-speed device, VBUS sensing off. Clocked from `pll1_q`, or from HSI48 without the crystal. Product "CS7000 M17 PLUS", serial from the chip's unique ID |
| Accessory contact C-3 | the radio's own 5 V rail, an output | whether the maker's cable puts the host's VBUS on it is open |
| Accessory UART | PA0, PA1 (UART4) on contacts C-12, C-2, at 3.3 V with protection diodes only | not used. Could take the fault trace: no driver yet |
| PHONE_DET | contact C-1 behind 10 k, on PA13 (SWDIO) | read from `GPIOA->IDR` for the view only (`board.phone_det`). What pulls it low is open |
| External PTT, microphone, speaker | contacts C-9, C-10, C-7 / C-8 | external PTT only |

---

## 12. On the board and not used

| Part | Wiring | State |
|---|---|---|
| Bluetooth module (IC804, BTM869) | UART on PA9 / PA10; its regulator enable is expander bit 4; audio to and from the analog path | off (`EXP_BT_ON` 0), the two pins in analog mode. The module's protocol is unknown. `board.bt_strap` is PA14: the module's supply behind 10 k, a "fitted" strap, not a connection status |
| Accelerometer (IC612, KXTJ3-1057) | data on PC15, **clock on expander bit 21**, interrupt not wired to the MCU | not used. Polling only would be possible, at two chain updates a bit |
| Vibration motor | a driver on expander bit 2 and a 2-pin socket | whether a motor is fitted is in no document. `CONFIG_VIBRATE` is unconfirmed in the registry and off |
| IC611 (HR_V3000S) | I2C1 on PB8 / PB9 with pull-ups | function unknown from any allowed source. The pins are inputs. Nothing else depends on it |
| Accessory UART | UART4, section 11 | not used |
| Tone DAC | PA5, to the speaker line and to the HR_C6000's pin 8 | off |
| Expander bit 1 | no label on the drawing | kept 0 |
| M17 | no M17 mode in the core | out of scope |

---

## 13. Calibration

### 13.1 What exists

Each unit carries the maker's factory table, 512 bytes at 0x001000-0x0011FF of the data flash (10.3). Its layout,
coding, interpolation and the checks a reader must make are in [calibration-cs7000p.md](calibration-cs7000p.md). The
maker's program checks none of its values (that page's 2.2), and no transmission has been made with any of them.

**Receive uses the table; transmit (FM, a tone channel's sub-audio level, DMR) uses it only behind the owner's arm
(13.2).** The radio takes the table in use
at its init (the window's or the copy's record, 10.3), parses it and shows it (`board.cal_table`, 15). Receive takes
three values from it on the radio task, each from its own field group, with the value without the table as its
fallback (receive side below). The port has model tables in source (`rfcal_cs7000p.c`), all unset. Unarmed, every
transmit value below is the model's, as without the table; armed, an FM transmission takes the drive, the FM pair
and trim A from the table, a tone channel also its sub-audio level, and a DMR transmission (the top stage) the
drive, the DMR pair and trim B (13.2). The last column is the maker's field for the same value:

| Value | Now | Supplied by | In the maker's table ([calibration-cs7000p.md](calibration-cs7000p.md)) |
|---|---|---|---|
| PA drive per power level and frequency | 9 points, 400 to 520 MHz in 15 MHz steps, all `CS7000P_CAL_UNSET`; armed, FM and DMR on the table's row for the level at f_tx (DMR: P, not the maker's P + step) | [power-calibration.md](../testing/power-calibration.md) with `tools/power_cal.py --allow-rf`, a wattmeter and a current-limited supply | 0x074 (low) and 0x06C (high) at f_tx, 8-bit codes (16 c in the 12-bit DAC): 5.1 |
| PA floor (the largest drive with no output) | 0 | the same procedure's zero point | none: the maker ramps from 0 |
| FM and DMR modulation pairs (4:0x45 / 0x46) | unset; armed, FM on the table's FM pair at f_tx, DMR on its DMR pair at f_tx | deviation on a service monitor or the SDR bench ([dmr-bench.md](../testing/dmr-bench.md)) | FM 0x0C4 / 0x0CC, DMR 0x0B4 / 0x0BC, at f_tx, one pair for both bandwidths: 5.3 |
| Reference offset and bias | 0, unverified; the receive level trim A within 32 steps (6.5); armed, 4:0x04 in FM transmit trim A, and on the top stage 4:0x04 in DMR (receive entries and the key-up) trim B | a frequency counter on the carrier, then the reference trim for the unit | trim A 0x0DC (receive, FM transmit) and trim B 0x174 (DMR transmit, and the bias in two DMR receive paths), offset binary. Under the port's 4:0x25 hold the bias equivalent is inferred, and the coding under the maker's 0x0E is open: 5.2 |
| Front-end tuning DAC per frequency | the maker's row, else mid scale | a sensitivity sweep with `chip_reg` 3:0 | 0x044 at f_rx: 5.1 |
| Squelch thresholds, RSSI offset | the plain mappings, the maker's thresholds shown (`board.sq_maker`); the RSSI the maker's points | a signal generator | squelch 0x04C-0x064 (levels 1 and 9, open and close), RSSI 0x040 / 0x041: 5.7, 5.8 |
| Sub-audio depth | `CS7000P_CTCSS_DEPTH_PM`, one value in per mille of half the PWM period; armed, the table's level for the tone at f_tx as a depth (13.2) | deviation on a service monitor | CTCSS 0x084 / 0x08C over the tone, DCS 0x094, at f_tx, doubled on 25 kHz: 5.5 |
| DMR key-up constants | a fixed lead | `tools/bench/pa_cal.py`, after the PA rows, the DMR modulation pair and the reference offset are built in (it stops while the view's `tx_drive` is 0), with `CS7000P_BENCH_DMR_TX=1` | burst timing 0x0EC-0x14C, from the RF TX interrupt; the time base and the place of the interrupt are not established: 5.4 |

The maker's sequences around these values are not copied, only the values
([calibration-cs7000p.md](calibration-cs7000p.md) 6.5): the port's order of section 7.5 stands.

The table's reader: `makercal_cs7000p.{h,c}`, pure, host-tested on invented tables (`cs7000pMakerCalTest`); the
radio's intake of it on the board's map, the core's storage and the copy (`cs7000pRadioTest`, and without the copy
`cs7000pRadioNoCopyTest`). It refuses what [calibration-cs7000p.md](calibration-cs7000p.md) 7 refuses, in two
kinds of rule. An **integrity** rule (that page's section 7, with a trim bound of 64 steps) makes the table not
valid: it is never used or copied, and a restore refuses such bytes. A **use** rule (the port's own: an RSSI point at 0x00 or 0xFF, a
sub-audio level of 0, burst hold or PA-off values that differ across the points, the battery byte at 0x00 or 0xFF)
only drops that field group, so what depends on it keeps today's value. Lookups follow that page's 4.3-4.4 to the
bit and flag every case where the maker's 32-bit product or sum would wrap. On top of them, the port's rules:

| Rule | Transmit (f_tx, TX points) | Receive (f_rx, RX points) |
|---|---|---|
| Inside the segment holding the VCO switch (that page's 5.12: 430.00001 MHz transmit, 479.00001 MHz receive) | the end value on the frequency's VCO side if the segment is at most 1 MHz wide, else refused | the end value on the frequency's VCO side |
| Past the end points | extended inside the transmit ranges only; refused when the extension crosses the switch | extended inside the receive range and clamped (the tune into 250-3850); today's value when the extension crosses the switch |
| Past the end points, the switch's segment the first or the last | refused: its line mixes the two VCOs' values | today's value |
| A result that wrapped, was held at 0 or 255, or is 0 or 255 | refused | today's value only when it wrapped |
| Trim A | d = v - 128 (4:0x04 in FM, d mod 256) | the bias 0x200 + d only within 32 steps, else today's |
| Trim B | d = v - 128 (4:0x04 in DMR, d mod 256), within the reader's 64 steps | not used: the bias keeps trim A's rule in DMR too (that page's two DMR receive paths with trim B on the bias are not traced to a user action, its section 8 item 5) |
| DMR burst fields | refused at a frequency where the looked-up fields fail that page's burst rules (an extension can) | |

**The receive side** (`cs7000p_cal_*_m` in `rfcal_cs7000p.h`, the bench knobs over them):

| Value | Source order | Falls back when |
|---|---|---|
| Front-end tune | `chip_reg` 3:0; the tune row 0x044 at f_rx (16 c); the model's row or `CS7000P_RX_TUNE_NOMINAL` | `CS7000P_RX_MAKER=0`, no valid table, TUNE refused, or the lookup refused at that frequency (out of band, wrapped, extended across the VCO switch) |
| Receive reference level | `chip_reg` 3:4; trim A (6.5); `CS7000P_REF_OFFSET` | `CS7000P_RX_MAKER=0`, no valid table, TRIM_A refused, or \|d_A\| over 32 (`why=rx_bound`) |
| RSSI dBm | `chip_reg` 3:8 (the datasheet line); the points 0x040 / 0x041 on raw >> 4; the datasheet line | `CS7000P_RX_MAKER=0`, no valid table, or RSSI refused (a point at 0x00 or 0xFF is a use rule: that group alone) |

A table that is not valid, or none, leaves every receive write and the dBm exactly as without it, and a valid table
changes only the tune and the level's bias (4:0x04 and every line as before): `cs7000pRadioTest` checks both write
for write, `cs7000pRadioSeqTest` the chain's (a bench key-up's own writes are today's, and nothing is admitted on
the table unarmed), `cs7000pCalTest` the getters on invented tables. The modem's callbacks are one call each into the chain
(`cs7000p_rf_ref_for`, `cs7000p_rf_r04`, `cs7000p_ref_signed` in `rfops_cs7000p.h`), which the tests call the
same way: unarmed, the FM transmit set-up's 4:0x04 and bias stay today's value on a valid table, and so does 4:0x04
at every receive entry below the arm's top stage. On the radio, 3:0 and 3:4 are the owner's A/B: `reg 3 4 0`
is today's level (`CS7000P_REF_OFFSET` 0, the default), `reg 3 4 0x100` the table's again; `reg 3 0 2048` today's
tune, `reg 3 0 0` the table's (both at the next status poll); `reg 3 8 1` the data sheet's RSSI line, `reg 3 8 0` the
table's points, for the DMR scan and roaming, which test the dBm. On the first radio (2026-11-24) the three were in
use (`rx_tune` the table's row, `ref ... src=maker`, `rx_cal=maker rssi=maker`), FM receive sounded as good as
before, DMR calls were decoded, and a strong DMR signal read a few dB lower than on the data sheet's line; the A/B with readings, the
scan and roaming on the new scale were not tried. The knobs are RAM only; the build knob
`CS7000P_RX_MAKER=0` is the lasting way back: receive then takes none of the three values from a valid table (the
reference's `why=build`), and writes and reads what it does without one (`cs7000pRadioRxOffTest`). Squelch stays the port's (the noise reading); the
maker's thresholds are read, checked (a damaged squelch row makes the table not valid) and shown at the receive
frequency (`board.sq_maker`, 15), not applied: how the radio's squelch levels map to the maker's 1 to 9 is not
decided. They were set under the maker's IF words, which `chip_reg` 3:7 gives for the comparison (7.3).

### 13.2 What is refused, and the bench override

`cs7000p_cal_tx_ok()` is true only when the PA row for the level, the modulation pair for the mode and the
reference offset are all set. With the tables as shipped it is false everywhere: `tx()` returns false in FM and DMR,
the hotspot cannot key, and `board.cal=none` says why (`maker` with the maker's table in use: still refused until
the owner arms transmission, below). `board.tx_refused` counts the refusals and gives the last
reason as `why=`:

| `why` | Reason |
|---|---|
| 0 | nothing refused yet |
| 1 | outside the transmit ranges: outside 400.0-405.9875 and 406.1125-512.0 MHz |
| 2 | no calibration and no bench drive armed; on the maker's table also a tone channel whose tone has no level in the table at f_tx (a CTCSS tone outside 67.0-254.1 Hz, a refused CTCSS or DCS group, a lookup refused there), and a lookup the reader refuses at f_tx (a switch segment over 1 MHz, an extension across the switch, a clamped or 0 / 255 result, DMR burst fields that fail the burst rules there) |
| 3 | a bench drive armed, but no debug link in the build |
| 4 | a bench drive armed, but no test TX limit lists this frequency |
| 5 | a bench drive armed, but DMR without `CS7000P_BENCH_DMR_TX` |
| 6 | a bench drive armed more than 60 s ago |
| 7 | the battery under its critical level, or without a reading (the ADC scan not running) |
| 8 | the reference not settled |
| 9 | the modem: not up, its FM transmit set-up refused, a write of the DMR key-up's pair or 4:0x04 failed, no pair for a DMR key-up on the maker's values (the arm lowered since the check), or the burst runner refused the arm |
| 10 | the synthesizer not locked on the carrier |
| 11 | a line or DAC write refused or failed (the interlock, a safe request from outside) |
| 12 | the power's off state (5.4): the radio let go of the supply and the knob still powers it. Checked before everything else: an armed bench drive is neither used nor dropped |
| 13 | the maker's table in use holds this transmission's values, but the owner's transmit arm does not cover its mode and level (below). Only when no bench drive is armed: a bench key-up keeps its own reasons, 3 to 6 (18 while armed) |
| 14 | the reference DAC in the maker's coding for a receive test (`chip_reg` 3:5 not 0, 6.5), or still in it: the modem takes 3:5's way back to 0 only at its next full receive set-up (the next status poll), and until then a burst would keep the maker's 0x0E and 0x26. Checked after the off state and before everything else: an armed bench drive is neither used nor dropped |
| 15 | ... the arm covers it, but no test TX limit list names the frequency |
| 16 | ... the arm covers it and a list names the frequency, but the reference carries a correction the table was not set with: PC8's idle duty is the bench's (`chip_reg` 3:2 not 512), a reference trim is stored (**Settings** > **Freq trim**, the storage view's `ref_trim` not 0), or the bench's reference offset is set (`chip_reg` 3:4 not 0x100) or still held by the modem (`board2.ref ... src=bench`: a release reaches the modem at the next receive set-up, and a DMR burst keeps the level it holds). Trim A is the table's whole correction of the reference; any of them on top of it moves the carrier by an amount nobody checked with the table (PC8 alone about 2 kHz end to end, a stored trim up to 10 ppm, the offset about 20 Hz a step). Raising the arm is refused while any is set, and neither 3:2 nor 3:4 can be set while armed, so for them this is the rule's second line. A trim can still be stored while armed (the menu, the debug link's SETTING): every key-up the arm covers is refused here from then on, and a transmission admitted before it does not tune on it (the sequencer's plan refuses the tune: FM's key-up `why=2`, a DMR transmission keys no burst) |
| 17 | ... the arm covers it and a list names the frequency, but the bench wrote the modem or the synthesizer (`chip_reg` chips 0 and 1) or a SoC register (`chip_reg` 0xFF, the debug link's note) since the start: a register their set-ups do not write again (the synthesizer's modulation, gain and delta-sigma words, most of the modem's, a timer, DAC1 or a pin of the SoC) keeps the bench's value until a restart. Checked after 16. As with 16, the arm is refused while it holds and none of them can be written while armed (14), so this is the second line |
| 18 | the bench's PA met the owner's arm: a bench drive armed while the arm is not off, or while a transmission on it has not been followed by a receive set-up (checked before the bench drive's own conditions, 3 to 6; the drive is neither used nor dropped: `tx_arm ... bench_drive=`), or a DMR key-up the arm covers while the PA key-up constants are the debug link's (RADIO op 37, or a key run's: `bench_keyup=1`; checked after 17, and again at the burst plan, before any write, for constants set after the admission). The two exclude each other (the arm, below): a drive is not taken while armed and the arm is refused while one is set or the constants hold, so the drive's case is the rule's second line. Op 37 itself is taken while armed: then every armed DMR key-up is refused here until `radio pa-keyup default` |

**The owner's transmit arm** ([radio-features.md](radio-features.md) 6.3, RADIO op 44). A key-up the maker's table
would serve (`cs7000p_cal_maker_tx`: FM the level's drive row, the FM pair and trim A at f_tx; DMR also the DMR pair,
trim B and the level's burst fields, every one passing the reader's transmit rules there) is refused until the owner
arms transmission. With no bench drive armed the table wins over the model wherever it holds the values. Armed for
the mode and level, and listed, an FM key-up and a DMR key-up go ahead on the table's values, a tone channel only
where the table gives its tone's level at f_tx (never on the model's rows, nor at the build's depth). What an armed
FM key-up takes, all at f_tx, all from one getter (`cs7000p_rf_maker_fm`, which asks the arm anew at every call; the
tone's level from `cs7000p_rf_tone_depth`, which asks it too):

| Value | From the table | Through |
|---|---|---|
| Drive | level 0 (`1W`) the low row 0x074, level 1 (`4W`) the high row 0x06C, 16 c, over the floor; the reader's transmit rules (13.1) | the admission (`admit_drive`) and the sequencer's plan: the port's ramp of 7.9, never the maker's (that page's 6.5) |
| FM pair | 0x0C4 to 4:0x45, 0x0CC to 4:0x46 | the modem's pair callback, for the admitted frequency only |
| 4:0x04 and the bias | trim A, never the bench's 3:4 (refused while armed; a key-up with one set or held is refused, 16): 4:0x04 d mod 256, the bias d mod 1024 (4:0x48 bits 9-8), under the set-up's 0x0E: the maker's bias in FM transmit, which its key-up leaves from receive (that page's 5.2, 6.3). Not the driver's FM transmit form (4:0x48 = 0), which puts a negative d 256 steps up | the modem's FM transmit set-up (`ref_dac_hold`: [modem-hrc6000.md](modem-hrc6000.md) 8.2) |
| Sub-audio depth (a tone channel) | L: CTCSS the line between 0x084 (67.0 Hz) and 0x08C (254.1 Hz), each at f_tx, over the tone in 0.1 Hz (that page's 4.5); DCS 0x094 at f_tx, either polarity; doubled on a 25 kHz channel, to at most 254. The depth in per mille of half the period: (4000 L + 525) / 1050, the maker's swing of about 4 L counts of its 1050-count half period taken as the same share of this PWM's (L = 40: 152; 254: 968). A CTCSS tone outside 67.0-254.1 Hz has no level: refused | the admission (the level must exist) and the depth at the key-up, before anything is written (`cs7000p_rf_tone_depth`; refused, `why=2`, should the arm drop in between); the generator after the PA is up (7.9 step 7). Inferred, not measured: the maker set its levels at a 48.76 % idle on its own PWM period, this one idles at 50 % (open question 4) |
| Fixed registers (that page's 5.13) | 4:0x3F 0x04 narrow / 0x07 wide, 4:0x35 0x20, 4:0x34 0xBC, 4:0x3E 0x06, 4:0xE4 0x20 narrow / 0x22 wide (the microphone gain) | the IF host's FM transmit rows ([modem-hrc6000.md](modem-hrc6000.md) 8.2), written after the set-up and written back at its end: receive after the transmission writes what it writes after any other. Wide 4:0x35 is the one value recorded, 0x20: if the maker's differs on wide, this under-deviates. The receive registers (4:0x37, the AK2365A words) stay the port's |

What an armed DMR key-up takes (the top stage only), all at f_tx, all from one getter (`cs7000p_rf_maker_dmr`,
which asks the arm anew at every call). The least measured transmission of all: nothing of the burst path has run on
a radio (open question 25), and a first one without a dummy load and a power meter is the owner's decision (7.5).

| Value | From the table | Through |
|---|---|---|
| Drive | the same rows as FM's for the level: P, not the maker's P + step (that page's 5.4: a by-product of its rise rule; the factory set P) | the admission and the burst plan (`cs7000p_rf_burst_drives`): the level's drive and the top level's at f_tx, both from the getter, never the model's; 0 once the arm no longer covers it, and the plan refuses |
| DMR pair | 0x0B4 to 4:0x45, 0x0BC to 4:0x46 | `hrc6000_if_tx_freq` at the burst plan (7.10), for the admitted frequency only: a split channel's f_tx, not the receive frequency the entry looked it up at. The maker writes its pair at the channel set-up, looked up at f_tx (that page's 5.3); the radio knows f_tx only at the key-up, and nothing writes the pair per burst |
| 4:0x04 | trim B, d mod 256, never the bench's 3:4 (as in FM: 16) | `rx_tx_offset` at every DMR receive entry while the arm covers DMR, and at the burst plan for the transmission on the table: the value the bursts carry (nothing else writes 4:0x04 before them). The maker writes trim B at its DMR key-up and leaves it there in receive (that page's 6.4). What 4:0x04 does under the port's 0x0C hold in a DMR slot is not measured; below the top stage, and in FM at every stage, 4:0x04 is today's |
| Burst timing | read and checked (a damaged field is a damaged table), not applied | the port's key-up constants stay (7.10) until that page's section 8 items 3 and 4 are measured |
| 4:0x25 in a slot | not the table's | the port's hold, as before: the maker's 0x0E in a DMR slot is open, so it is not copied |

The arm:

| Rule | Detail |
|---|---|
| Stages | 0 off; 1 FM at the low level (`1W`); 2 FM at both levels; 3 FM and DMR (`CONFIG_DMR` builds; the top stage is 2 without) |
| Handshake | stage 1 to 3 needs the CRC-32 of the table in use (the window's, or the copy's record's when that is the source: 10.3), which the owner's tool computes from its backup: a window restored from the backup and a record copied from the original window both have it. Refused with reason 1 when no table is in use, 2 for another CRC, 3 over the top stage, 4 before the radio's init, while 3:5 or the modem's coding is not the hold (14 above), while PC8's idle duty is the bench's (3:2 not 512), a reference trim is stored, or the bench's reference offset is set (3:4 not 0x100) or still held by the modem until the next status poll after its release (`src=bench`; 16 above), after a bench write to the modem or the synthesizer since the start (`tx_arm ... chips=1`, until a restart; 17 above) or to a SoC register (`soc=1`: the debug link's note, [debuglink.md](debuglink.md) 3.24, which the board asks; until a restart), while a bench drive is armed or a bench carrier is up (`bench_drive=`), or while the DMR key-up constants are the debug link's (`bench_keyup=1`; 18 above), in the power's off state, or while a restore's job is pending or running or its outcome is not yet taken (the table is about to change). Off, and a stage below the one armed, are always taken (the id kept) |
| The list | a key-up on the maker's values also needs a test TX limit list that names the frequency (`why=15`): the frequencies the owner named are the radio's rule, and clearing the limit makes it refuse again |
| RAM only | off at every start; off at every safe request (a fault path, the power's off state); never stored |
| Lower or off | a transmission on the table's values the new stage no longer covers ends at once, by the safe request (`board_rf_safe`); no bench carrier can be up while armed (the bench's PA, below). Should the arm drop between the check and the key-up, the pair, the trim and the rows are no longer given, the burst plan's drives are 0 and the sequencer's plan refuses: nothing keys. A change of stage sets receive up again at the next status poll, so 4:0x04 in DMR follows it |
| The levels | stage 1 keys the low row only: the high row is no level's drive (`tx_drive_level` 0 for level 1, `tx_drive_top` 0, so the drive sweep is refused), and the bench's `tx_drive_now` (the sweep's writes) stays at or under the admitted drive on the transmission on the table and is refused on any other while the arm holds |
| The restore | refused while the arm is not off (`busy`, [settings-store.md](settings-store.md) 4.2), and the arm while a restore is pending |
| The bench's PA | the two routes exclude each other ([radio-features.md](radio-features.md) 6.3), so an armed radio puts nothing on the air but its stage's transmissions on the table's values. While the arm is not off, or a transmission on it has not been followed by a receive set-up (`cs7000p_rf_arm_holds`): `tx_drive` takes no drive but 0 (`radio tx-drive N` answers bad argument, and the tool says the arm is on), so neither `radio tx-drive` nor `pa-keyrun` (which stops at its first drive; constants it carries are set before that and then refuse every armed DMR key-up, 18) keys anything; `tx_drive_now` as in the row above; a bench admission is refused (18). The arm is refused while a drive is armed (expired ones included, until a key-up drops them or `radio tx-drive off`), while a bench carrier is up (its drive cleared since included), and while the DMR key-up constants are op 37's. From the bench route to the arm: the carrier ended, `radio tx-drive off`, `radio pa-keyup default` (or a restart); back: `radio tx-arm off` (or a restart) |
| The bench drive | as before, unarmed: its conditions (below), no pair. With a valid table its key-up's writes are those of a radio without one, after a restart or after `radio tx-arm off` from any stage alike; the receive bias it inherits from the receive entry before it carries trim A (13.1) |
| Lifting it | `CS7000P_TX_ARM_REQUIRED` in `rfops_cs7000p.h`, 1 and not a build option: 0 makes every stage cover every mode, level and frequency |

What each stage lets go out on the maker's table, on the listed frequencies only (whatever is on the antenna
connector receives it):

| Stage (`radio tx-arm`) | Keys on the table | Still refused |
|---|---|---|
| `off` (every start) | nothing | every key-up the table would serve (`why=13`) |
| `fm-low` | FM at 1W (a PTT on an FM channel); a tone channel only where the table gives its tone a level | FM at 4W, every DMR transmission (voice, data, APRS over DMR, the hotspot, Tier III) |
| `fm` | FM at both levels | every DMR transmission |
| `dmr` | FM and DMR at both levels, what the radio sends by itself included (APRS over DMR, data replies, the hotspot, Tier III registration) | nothing the table serves |

**The owner's tool** ([radio-features.md](radio-features.md) 6.3), never run against a radio yet:

| Command | Does |
|---|---|
| `tools/rfoundry_debug.py radio tx-arm` | prints the stage, the highest, and the table's range and CRC-32 as the radio computes it. Reads only |
| `... radio tx-arm fm-low --backup BACKUP.zip --mhz MHZ --allow-rf` | says what the stage lets out, compares the table in use with the backup's 0x1000-0x11FF (the window read twice, or the record by op 3's CRC; a difference refuses and sends nothing), sets the test TX limit to the `--mhz` list, arms with the CRC-32 of the backup's bytes (never the radio's), reads the stage back. `fm` and `dmr` the same |
| `... radio tx-arm off` | stage 0; the test TX limit stays as set (`radio tx-limit off` clears it, and then even an armed radio refuses, `why=15`) |

The order is the stages': `fm-low` until its checks pass, then `fm`, then `dmr`, each with the steps of bring-up
stage 11.1. Each start begins at `off`. The first key-up of each is the first on any radio.

`cs7000pRadioSeqTest` checks 13, 14 and 15 at every level, mode and stage, and 16 (each of its four causes, and a trim stored after the admission: no tune) and 17 at the top stage in FM and DMR, with nothing admitted, written or given; 18 for a bench drive at every armed stage, level and mode and after the arm went off under a transmission on it, the drive kept, and for the key-up constants at the top stage; no drive taken and no drive now while the arm holds;
armed and listed, FM admitted on the table's drive at f_tx (pinned values on both VCOs, inside the switch segment,
past the last point), its pair, trim A and rows, the whole key-up's writes under the chain's rules; DMR at the top
stage on the table's drive with its DMR pair at f_tx (a split channel: never the receive frequency's), trim B and
the top level's drive for the burst plan, two bursts under the chain's rules; 4:0x04 trim B in DMR on the top stage
only, today's in FM; a wide switch segment and an extension across the switch refused, in FM and DMR alike, and
outside the transmit ranges in both; tone channels at their level's depth (pinned, narrow and wide, at f_tx on split
channels, for the admitted frequency only and while armed), a tone without a level refused; the table over a model
with set rows; a drive at or under the floor, in both modes; the arm lowered before the key-up or the bursts.
`cs7000pRadioTest` the handshake against each source, every refusal, armed FM key-ups from the window and from the
record, tone channels with the depth the generator gets (and the arm cleared during one), armed DMR key-ups on a
split channel (the pair the modem gets at f_tx, trim B at the key-up and in DMR receive on the top stage only, back
to today's at a lower stage, a DMR transmission ended by a lower stage, no pair and a failed write refused, the arm
lowered inside the burst plan refused with nothing written), the restore and the arm excluding each other, and the
SoC write the debug link noted (the arm refused, `soc=1`, 17 for a covered key-up, the bench key-up's writes as before), the bench drive and the arm refusing each other (a drive set, a bench carrier up, the key-up constants; 18 for a DMR key-up under constants set before the admission or after it), and the unarmed bench key-up's writes pinned (their hash as before the exclusion), after `radio tx-arm off` from every stage too; `tx_maker` (15) read before the arm against what the armed key-up
then gives the chain and the modem (FM at both levels and widths, CTCSS and DCS channels, a split channel, DMR at the
top stage on both levels), and the line moving nothing before the radio is up, asleep, keyed or during a take;
`cs7000pCalTest` the tone's level from the table, `cs7000pSubPwmTest` the duty
and the level's depth; `hrc6000IfHostTest` the rows and their write-back, and `hrc6000_if_tx_freq`.

The only way the firmware keys an uncalibrated radio is the debug link's drive override (`tx_drive`, 1 to 4095).
(A raw SoC `CHIP_REG` write to DAC1 or GPIOE can key the PA outside every check here, 4.3: the bench must not make
one. The debug link refuses one while the owner's arm is not off, and one since the start refuses the arm, 14.) Arming the override is not enough: the next `tx()` honors it only when all of these hold.

| Condition | Why |
|---|---|
| The owner's arm is off, and no transmission on it is up | the two routes exclude each other (13.2, the bench's PA): while the arm holds no drive is taken, and one met all the same is refused (`why=18`) |
| A test TX limit is set as a frequency list, and the frequency is on it | the bench harness named this frequency. A PTT press outside a test run meets no list |
| The mode is FM, or the build has `CS7000P_BENCH_DMR_TX=1` | the burst key-up has not run on a radio |
| At most 60 s since the override was armed | an armed override does not wait for a transmission that never came |
| The debug link is built | without it there is neither a limit nor an override |

The override is dropped at the transmission's end, at a receive set-up that ends a transmission, and when it
expires. A key-up refused in the off state does not look at it: it expires at the first key-up after that state, if
60 s have passed since it was armed. On a calibrated radio it is used under the same conditions; without them
the level's own drive is used. A drive at or under the floor, or of 0, is refused. `tx_drive_top` is 0 while the
tables are unset, so the PA drive sweep is refused on an uncalibrated radio: only the plain override keys it. With
no modulation pair known the carrier is unmodulated.
Everything in section 7.5 about the first transmission applies.

### 13.3 The planned unit record

Per-unit values the tree measures itself (a power meter's corrections, for one) will go in a record of the tree's own,
not in a vendor format: the magic `RFC1`, a version, a length, a CRC-16, then byte rows read with
`drivers/cal/calblock`, in its own sector of the hole, 0x01C000 (10.2). A unit record will win over the model table.
The board sector, 0x01B000 (`CS7000P_STORE_CAL_ADDR`), is for the copy of the maker's table instead; the maker's
records at 0x01E000 are kept and never written (10.3). The reader and the writer (a bench tool over a debug link
board-record command) come with the first measured radio; the field widths are fixed then.

---

## 14. `chip_reg` numbering

Bench access through the debug link ([debuglink.md](debuglink.md) 3.24):

| Chip | Page / register | Access |
|---|---|---|
| 0 | HR_C6000, pages 1 and 4 (2 and 3 are FIFOs a read would drain). Every receive entry writes 4:0x25, 0x47, 0x48 and 0x04 again (and 0x26 with 3:5 = 2). A write is refused while the owner's transmit arm (13.2) is not off, and while a transmission admitted on it has not been followed by a receive set-up: an armed key-up runs on the table's values, and a write would change them on air or linger into the next one (4:0x25 until a receive entry). A write tried since the start (one the bus refused included) refuses the arm until a restart (`why=17`, 13.2): a register no set-up writes again keeps it. Under a bench carrier the modem stays writable | read / write; a write is followed by the register read back |
| 1 | SKY72310, page 0, registers 0 to 14, values to 0xFFF. The driver's next tune overwrites the divider registers; the others (the modulation words, the gain, the delta-sigma control) only a restart. Refused as chip 0 is, a write since the start refuses the arm as chip 0's does, and refused also while any transmission is up, a bench carrier's included: the PA is off before the VCO moves (7.5) | write only (the value written is returned) |
| 2 | AK2365A, 0x01 to 0x04, 0x0B and 0x0C (the registers the data sheet names; it forbids access to the others). Page 0 reads with the data line pulled down, page 1 pulled up: a value that is the same both ways came from the chip. Where the read bits fall is from the data sheet's drawing and unverified | read / write; a write is followed by a read back |
| 3 | The board, page 0. 0: the receive tuning DAC, through the interlock: a write of 1 to 4095 puts the value (clamped to 250 to 3850) on the DAC now, if the radio is receiving, and keeps it for later receive set-ups; 0 gives the table's value back (the maker's row, else the model's) at the next status poll; refused while a transmit bit is set or an FM transmission is up; a read gives the DAC's code now. 1: the expander shadow, read only. 2: PC8's idle duty, 0 to 1024 (0 and 1024 hold the line low or high: bench only); refused while a tone is sent and while the owner's transmit arm is not off (13.2; a duty other than 512 also refuses the arm). 3: the LO side (0 low, 1 high: retunes and sets the I/Q order). 4: the reference offset, a signed byte (two's complement in the low byte), for the receive level over trim A and for 4:0x04 (6.5); 0x100 releases it (the level back to trim A or the default, 4:0x04 to the default); a read gives 0x100 while none is set. An offset is refused while the owner's transmit arm is not off and while a transmission admitted on it has not been followed by a receive set-up (13.2; one set, or held by the modem until the receive set-up after its release, also refuses the arm). The release is always taken: while armed no offset can be set, so it writes the values already there and asks for a receive set-up, which waits for the end of any transmission. 5: the reference DAC's coding in receive (6.5), 0 the measured hold (4:0x25 = 0x0C, the default), 1 the maker's state (0x0E, the level the signed offset), 2 the same with 4:0x26 = 0x7D; not 0 refuses every transmission (`why=14`); nothing keeps it over a restart. 6: the receive VCO rule, 0 by frequency (the maker's, the default), 1 by direction (the line set in every receive set-up, as before the rule: for a receive test across 479 MHz); transmit follows f_tx either way, and nothing keeps it over a restart. 7: the IF chip's words in receive (7.3), 0 the port's (the default), 1 the maker's for the mode (for a receive test of the table's squelch and RSSI points); transmit is neither refused nor changed, and nothing keeps it over a restart. 8: the RSSI scale, 0 the table's two points when they passed (the default), 1 the data sheet's line (13.1), for an A/B of what the dBm scale does to the DMR scan's strength test, roaming's and trunking's leave threshold, the S-meter and the hotspot's report; it writes nothing, and nothing keeps it over a restart. 3, 4, 5, 6, 7 and 0's release take effect at the radio task's next status poll (a receive set-up); 8 at the next status poll, which reads the RSSI | read / write |
| 0xFF (`soc`) | The SoC's own registers ([debuglink.md](debuglink.md) 3.24). **Never DAC1 (0x40007400-0x400077FF) or GPIOE (0x58021000-0x580213FF)**: a write there bypasses the interlock (4.3) and can key the PA | read / write; a write is refused while transmitting and while the owner's transmit arm (13.2) is not off (the debug link's own check, through `tx_arm_info`), and one tried since the start (one the platform refuses included) refuses the arm until a restart (`tx_arm ... soc=1`; `why=17` should the two meet) |

---

## 15. Debug link view keys

`view_cs7000p.c`; each is `board.<key>`, in view section 28 (`board`) but the last seven, which are in section 39
(`board2`, `board_t.view_split`: `board` is near the size of one reply; [debuglink.md](debuglink.md) 3.7). The
diagnostics page shows the bring-up readings.

| Key | Values |
|---|---|
| `clock` | `hse`, `hsi-pll`, `hsi` (2.2) |
| `mcu_rev`, `rom_loader`, `rdp` | silicon revision (`Y` / `X` / `V`, or `0x....` for one the port does not know), the ROM bootloader's version byte (`0x91`), the RDP byte (`0xaa`, read only) |
| `exp` | the expander shadow, hex (`0x2a0181`): the word last shifted out, not a reading of the outputs (the chain has no read-back) |
| `knob`, `hold` | `unknown` / `on` / `off` / `noadc`; whether the hold bit is set (`0` / `1`) |
| `pow_det_mv`, `vbat_mv`, `vdda_mv` | the battery sense, the battery by the model (the smoothed display figure; 0 with the knob off), the rail from VREFINT (0: not measured, the nominal 3,240 mV is in use) |
| `vol_raw`, `vox_raw` | the volume pot and microphone level ranks (the second moves only while keyed: 5.6) |
| `lcd_board` | `assumed` / `unexpected` / `forced`, and `levels=0x..`, the five levels read (9.2) |
| `panel` | controller, `source=`, `id=` and `reg=` (three bytes each): `st7735 source=id id=7c89f0 reg=7c89f0`. `none source=none` on an `unexpected` board, `unknown source=default id=000000 reg=000000` when the panel does not answer the read (9.3) |
| `keys_raw` | the five scan bytes, ten hex digits (9.4) |
| `chsw_raw`, `chsw` | the selector's four lines (`0xf` all open); `unverified` or the position |
| `phone_det`, `bt_strap` | PA13, PA14 (`0` / `1`, through `GPIOA->IDR`) |
| `flash` | JEDEC ID, size, `addr4`, and `refused=`, the erases and programs the driver refused in a sector holding the maker's bytes outside the storage layer's grant (10.3; 0 unless a writer misses the rule): `EF4019 size=0x2000000 addr4=1 refused=0` |
| `store` | the board sector at 0x01B000 (10.2): `blank` / `ours` (the magic `RFFC`) / `foreign` / `unread` (no chip, or the read failed; asked again at the next request). Read once, at the first request; the record's own state is `cal_rec`'s |
| `cal` | `none` / `model` / `maker`: the table in use holds FM transmit values somewhere in the transmit ranges, or DMR's in a build with DMR, served only behind the owner's arm (`tx_arm`, 13.2) |
| `tx_refused` | the count and `why=`, the last reason (13.2) |
| `dac` | the shared DAC's code now (0 = off) and `refused=`, the refused DAC writes and refused expander writes together |
| `lo_side` | `low` / `high` |
| `pll` | `locked` / `unlocked` (the lock pin read when the view is asked, not the last tune's result), `r=` the reference divider, `vco_hz=`, `unlocks=` |
| `ak` | `ok` / `fail`: whether the IF receiver's last set-up frames were sent. No read-back: `ok` does not show that the chip answered (the 0x0B read-back through `chip_reg` 2 does, bring-up stage 9) |
| `rssi_raw`, `noise_raw`, `qt_raw` | the three receiver ranks |
| `rx_tune`, `ref_offset`, `pc8_duty` | the tuning DAC value, the reference offset (4:0x04's but in DMR on the arm's top stage, where `ref`'s `r04=` is trim B; the receive level's is `ref`'s), PC8's duty |
| `hrc` | init ok (`0` / `1`), `r10=` and `r25=`: 4:0x10 and 4:0x25 as read back |
| `i2s` | running, `dir=` (`ok` / `swapped` / `busy` / `none` / `unknown`), `ioswp=`, `edges=` the two probe counts, `und=`, `ovr=`. Only in a build with DMR |
| `gps_baud` | the rate the module answered at; 0: not heard, or built without GPS |
| `adc_restarts` | overrun recoveries of the ADC scan; 0 is expected |
| `cal_table` (`board2`) | the maker's table as taken at start (10.3, 13.1): the window's state `unread` / `blank` / `zero` / `refused` / `ok`; `src=` the table in use, `window` / `record` / `none`; `crc=` the CRC-32 of the window's 512 bytes (Python's `zlib.crc32`; 0 when unread), the figure to compare with the backup's 0x1000-0x11FF; `why=` the window's first failed rule (the reader's names), `read` / `unstable` when unread, `conflict` when window and record are both valid and differ; `off=` the byte it failed at; `fields=` the field groups of the table in use that passed (`CS7000P_MK_F_*`, 0 with none in use); `bat=` its battery byte. `cal_table=blank src=none crc=0x... why=blank off=0x000 fields=0x0000 bat=0x00` on a radio whose table was lost |
| `cal_rec` (`board2`) | the copy's record at 0x01B000: `unread` / `blank` / `ok` / `bad` / `newer` / `other` ([settings-store.md](settings-store.md) 4.2), `off` in a build without `CONFIG_FACTORY_COPY`; `crc=` of its 512 bytes (`-` when it holds none of ours); `wrote=` 1 when this start wrote it, 2 when it tried and failed (tried again at the next start), else 0; `restore=` the last restore's result now: `none` (no restore at this start), `pending` while its job runs, `ok` or the reason's name (`window_differs`, `flash`, ...; settings-store.md 4.2). The record's state and CRC are the table in use's, taken at the status poll after a restore's job |
| `ref` (`board2`) | the receive reference (6.5): the coding the modem holds, taken at its last full receive set-up, `hold` / `maker` / `maker7d`; `bias=` the level the last receive entry wrote in it (0x200 + the offset under the hold); `r04=` 4:0x04 as the last receive entry or DMR key-up wrote it (trim B in DMR on the arm's top stage, else the reference offset); `d_a=`, `d_b=` the table's trims as d (`-` with none); `src=` the level's offset, `maker` (trim A) / `default` / `bench` (3:4); `why=` when it is not trim A, `table` (none valid) / `group` / `rx_bound` (over 32 steps) / `build` (`CS7000P_RX_MAKER=0`), else `none`; last, `ask=` the coding 3:5 asks for, only while the modem has not taken it (until the next status poll; every key-up is refused meanwhile, `why=14`). `ref=hold bias=0x1fb r04=0x00 d_a=-5 d_b=-3 src=maker why=none` |
| `rx_cal` (`board2`) | the tune's source, `maker` / `default` / `bench` (3:0); `rssi=` the last status poll's scale, `maker` (the table's points) / `sheet` (the data sheet's line) / `bench` (the data sheet's line by 3:8); `vco=` the VCO line now, `low` / `high`; `vco_rule=` `freq` / `dir` (3:6); `ifw=` the IF chip's words, `port` / `maker` (3:7) |
| `sq_maker` (`board2`) | the maker's squelch thresholds at the receive frequency, shown and never applied (13.1): `ok o1= c1= o9= c9=`, open and close at levels 1 and 9 as 8-bit noise codes (the scale of `noise_raw` >> 4; [calibration-cs7000p.md](calibration-cs7000p.md) 5.7), by the receive rows' lookup (inside the 479 MHz switch segment the end value on the frequency's VCO side; past the end points extended, where their order need not hold); `none why=` the lookup's reason, `table` (none valid) / `group` / `band` (out of the receive band, or no receive yet) / `switch` / `math`. `sq_maker=ok o1=120 c1=130 o9=40 c9=50` |
| `tx_arm` (`board2`) | the owner's transmit arm (13.2): `off` / `fm-low` / `fm` / `dmr`; `id=` the CRC-32 it was armed with (0 when off); `list=` 1 while a test TX limit list is set (an armed key-up on the maker's values needs one naming its frequency, `why=15`); `chips=1` only after a bench write to the modem or the synthesizer since the start, `soc=1` after one to a SoC register (either: the arm refused until a restart, `why=17`); `bench_drive=` the bench drive armed or in use and `bench_keyup=1` while the DMR key-up constants are op 37's, each only while it holds (the arm refused meanwhile, `why=18`). `tx_arm=off id=0x00000000 list=0` after every start |
| `tx_maker` (`board2`) | what a key-up on the table in use would take (13.2), read without arming and never a key-up itself: the same lookups the admission and the key-up's getters make, nothing written. `ok` when the table serves it, `none` when it does not. `ok` is the table's part of the admission only: an armed key-up whose stage covers it, on a listed frequency, takes these values if it keys, and the other rules of 13.2 (the battery, the reference's settle time, the bench states) can still refuse it. `at=` which transmission: `tx` the one the board was last asked for (`tx()`, refused ones included: a key-up while the arm is off is refused `why=13` and noted), until the core's next receive set-up (a channel change, also to another channel on the same receive frequency, a scan step, a dual watch switch, a beacon's retune), a mode change or a restart. The radio's own receive set-ups (a transmission's end, the status poll's after a bench change or an arm) keep it. A key-up the core refuses itself (TX inhibit, a test TX limit that does not name the frequency, a critical battery, a channel that cannot transmit) never reaches the board and is not noted. `rx` without one: the receive frequency as a simplex channel's at the lowest level with no tone. The board learns a channel's transmit frequency, power level and tone only from the key-up request, so a power level or tone changed since shows at the next one. `hz=` the transmit frequency, `mode=` `fm` / `dmr`, `lvl=` the power level (0 the low row); `lo=`, `hi=` the drive at the two levels at that frequency (12-bit DAC codes, 16 c: FM keys on `lvl=`'s, DMR's burst plan on `lvl=`'s with `hi=` as its top), `pair=` the mode's modulation pair at that frequency (4:0x45, 4:0x46), `d_a=` trim A as d (FM: 4:0x04 and the bias in the transmit set-up; in DMR checked only, the bursts' bias being the receive level, `ref`'s `bias=`), in DMR `d_b=` trim B as d (4:0x04 at the key-up), on an FM tone channel `tone_pm=` the tone's depth in per mille at the channel's width (the tone generator's); `-` where that lookup gives none. With `none`, `no=` the first lookup that refuses in the admission's order, `band` (outside the transmit band) / `table` (none valid) / `drive` / `pair` / `trim_a` / `trim_b` / `burst` (DMR, the level's burst fields) / `tone`, and `why=` its reason: the reader's (`band`, `group`, `row`, `switch`, `window`, `math`, `code`, `floor`, `burst`, `table`) or a tone's own, `range` (a CTCSS tone outside 67.0 to 254.1 Hz). `tx_maker=ok at=rx hz=446000000 mode=fm lvl=0 lo=1744 hi=3360 pair=0x78,0x87 d_a=-5` on the host tests' invented table; on a radio the unit's own values, which stay on its debug link |

The seven `board2` keys of one reply describe one state: the table in use, the receive set-up on it and the last
key-up request. A restore's take changes the table and sets receive up on it in one step of the radio task; the reply
copies the state at its first `board2` line, never during such a step, and its other lines format that copy. A reply
asked during the step waits for its end; one that waited 250 ms gives `<key>=busy` for each of the seven instead (not
expected: the step is one receive set-up). The other keys, in `board`, are read one by one. The longest `board2`
reply, every line at its longest, is about 600 bytes of the 1,002 a reply has for them.

`boards/cs7000p/hil/test_cs7000p.py` reads them on a radio, and compares `cal_table`'s CRC with the window read
through STORAGE READ (and an ok record's bytes with `cal_rec`'s), `tx_arm` with RADIO op 44's info (the stage, the
table's range and CRC, the top stage), and checks `tx_maker`'s form and the order of its two drives. It never
transmits, writes or arms; `tests/hil/test_tx_arm.py` is the arm's.

---

## 16. Open questions

What no allowed source settles, the risks, and what resolves each. The numbers are the ones the bring-up page and
the source comments use.

| # | Question or risk | Where it bites | Resolved by |
|---|---|---|---|
| 1 | **Reference oscillator control.** Which HR_C6000 registers set pin 75's level in receive and in transmit, the bias's coding, the level that is on frequency. The chip's manual contradicts itself on which DAC is which | every mode: the receive and transmit frequency, and the offset between them. On the maker's table: the coding under the maker's 0x0E against the port's 0x0C hold ([calibration-cs7000p.md](calibration-cs7000p.md) 8 item 1), and what trim A and trim B in 4:0x04 do under the hold in FM and DMR transmit (that page's item 2) | The carrier on a frequency counter (bench override, dummy load) and the receive LO's leak while stepping 4:0x47 / 0x48, 0x04, 0x25 through `chip_reg` chip 0; the voltage at the oscillator's control pin if a test point is found. The coding in receive: bring-up 9.1 step 4 (`chip_reg` 3:5), not run yet. Seen on one radio: the receive level 0x200 + d_A under the hold in use, FM as clear by ear (no counter). On the maker's table without a counter: a second receiver's tuning step or an SDR (bring-up 11.1) |
| 2 | **First LO side**: low, seen on one radio (it locks and receives there; the high side is out of the VCO's range at 433 MHz). With it the I/Q order of 4:0x01 bit 6 is right: the same radio syncs to DMR and decodes calls | answered for the low side | A signal generator on channel; flip `chip_reg` 3:3. DMR sync needs the matching I/Q order |
| 3 | Synthesizer: the phase detector gain for this loop filter, lock time, the lock pin's behavior in fractional mode, the lock range over 350 to 512 MHz (450 to 577 MHz with the LO above the signal; charge pump 0.5 to 3.9 V) | lock failures, slow key-up, the band edges | `board.pll` over a frequency sweep; the tuning line at test point TP1 |
| 4 | PC8's idle level (50 % chosen; the maker's levels were set at 48.76 %) and PWM ripple on the reference; on the maker's table the sub-audio depth rule, (4000 L + 525) / 1050 per mille, inferred from the maker's PWM period (13.2) | carrier offset, hum on the carrier; a tone too weak or too strong to open a receiver | The carrier with PC8 at 0 %, 50 %, 100 % (`chip_reg` 3:2); the spectrum close in. The tone's deviation on a service monitor, or a receiver with the tone set opening on it (bring-up 11.1) |
| 5 | PA: drive against power, the floor, a cliff under it, whether 4 W is reachable inside the DAC's range, no reverse-power protection. On the maker's table: whether its drive codes give 1 W and 4 W under the port's ramp and a floor of 0 | the first transmission | [power-calibration.md](../testing/power-calibration.md) from a low drive, on a current-limited supply, into a dummy load, the PA current watched. Without a meter, only a second receiver's S-meter and the supply's current: no power figure (bring-up 11.1) |
| 6 | A carrier leak while the expander is undefined (the reset temporization plus about 0.1 ms of start-up; **for as long as the ROM bootloader runs**), and with the transmit rail on but the loop open | a spurious emission at power-on and in loader mode | An SDR next to the radio at power-on and in loader mode, with a dummy load. The loader case is the maker's own design and procedure |
| 7 | HR_C6000 codec inputs: which 4:0xE0 bit is the microphone on pin 5; microphone and line-out gains | no FM modulation | A tone into the microphone, deviation while stepping 4:0xE0 |
| 8 | **The I2S data direction of a slave.** RM0433 Table 426 against 50.9.2. Also the clock polarity, and whether the chip is the master | no beeps; with the wrong sense assumed, two outputs on one line | Not assumed: probed (section 6.3). `board.i2s` shows the verdict and both counts; a scope on PC11 / PC12 confirms it once |
| 9 | FM receive audio through the chip from a 450 kHz IF; the IF level (AK2365A `IFOG`); the 0.16 V bias at the chip's input | no FM audio, poor DMR sensitivity | Audio at the speaker against generator level; DMR BER ([dmr-bench.md](../testing/dmr-bench.md)) |
| 10 | **The LCD board**: on one radio the board check passes, the panel answers ID 7C 89 F0 (ST7735) and the display and keys work. Still open: offsets and insets by eye, other board revisions | a blank or garbled display; the keys | `board.lcd_board` (only PE0 low), the `board.panel` ID bytes, `board.keys_raw` |
| 11 | Answered on one radio: the function keys' positions, the selector's code and its direction (9.4). Open only for another LCD board revision or selector part | wrong keys, channels stepping the wrong way | `board.keys_raw` and `board.chsw_raw`, one key and one detent at a time |
| 12 | The backlight curve; the key lights' polarity | too dark, no dimming | [backlight-curve.md](../testing/backlight-curve.md) |
| 13 | The battery scale (a model, +/-4 %), the knob's switch-open threshold (`POW_DET` 0.7 V), the hold's margin at low battery | a wrong battery display; the radio switches off when the knob's contact bounces at low battery | A bench supply sweep against `board.vbat_mv`; `POW_DET` with the knob on at the lowest battery and as the knob goes off; knob off at 6.0 V from normal running (the hold set) |
| 14 | Silicon revision, the ROM loader's version, the RDP level and option bytes as shipped. One radio: revision V, loader 0x92; RDP read 0xAA only after the install (as shipped: not read) | USB PHY errata, loader quirks (on 0x90: whether a cold SK1 entry connects without a second reset, AN2606 Table 146; PCROP left by the Read Unprotect), whether the first install always needs the unprotect | `board.mcu_rev`, `board.rom_loader`, `board.rdp`; `rfoundry detect` in loader mode, before and after the maker's recovery |
| 15 | VCAP capacitors drawn 0.1 uF (ST asks 2.2 uF) | core supply ripple at higher scales | Look at the board. The port stays at VOS3 |
| 16 | HSE start (a crystal, no bypass); a PLL from a 1 MHz input; USB on that clock. On the fallback: USB on HSI48 with the CRS (untrimmed on revision Y) | USB errors. Without the crystal the radio still starts, at the same rates, and says `board.clock=hsi-pll` | `board.clock`; enumerate; a long CDC transfer |
| 17 | The 96 MHz bus clock's fifth harmonic at 480 MHz; 48 MHz harmonics | receiver desense on some channels | A sensitivity or noise floor scan across the band with the CPU running |
| 18 | Software entry to the ROM loader from a running image: the supply is already configured; the loader's USART pins are PA2 (an op-amp output here) and PB10 (a selector contact to ground) on every loader but 0x91 (0x92: not stated), PA9 and PB14 (the chip's data output) on 0x91. Whether the HR_C6000 releases its data output while deselected. Whether the backup register keeps the request with the RTC's kernel clock off. (A plain software reset with SK1 held would reach the same loader on the same pins: every one waits for SK1's release first, 2.5) | updates without the key | On by default under the rule (`board_rom_entry_ok()`, `romentry_cs7000p.h`): loader 0x91, or PB10 high. The entry reached the loader on the first radio (loader 0x92); still to check: PB10 and PB12 as section 4.2 step 5. SK1 at power-on stays available |
| 19 | The ROM's Leave into the image (the stack pointer rule, peripherals as the loader left them); a normal power-on with the first sector erased starts nothing (2.5) | the radio needs a power cycle after flashing; an install cut short needs the loader and the same install | Observe. The tools already say to switch the radio off and on, and write the vector table last |
| 20 | **Read Unprotect erases the maker's bootloader and programs.** Whether anything unit-specific is in the MCU's flash is not stated (the maker's recovery reloads a bootloader, programs, a default codeplug and a language file, which suggests not). The factory calibration is not: the maker's program reads it from the data flash only ([calibration-cs7000p.md](calibration-cs7000p.md) 2.1) | a radio that cannot go back to stock, or lost calibration, before RadioFoundry is of use | All before the first RadioFoundry install: the owner saves the codeplug with the maker's CPS and notes Radio Info while the stock firmware runs (and saves the tuning data, if the maker's tuner software can export it: no document says), runs the maker's documented recovery once (it loads the maker's default codeplug), confirms the radio works on the maker's firmware, and writes the saved codeplug back |
| 21 | The maker's data in the 32 MB flash: the calibration table, the device information and three records are located (10.3); the codeplug, fonts and voice data are not. On one radio the backup's 0x001000-0x0011FF, restored, passed every field group of the reader | a claim of the settings region can overwrite what the map does not keep: the device information and the data not mapped (the table and the records are kept, 10.3) | A full backup first: on every unit the region is foreign until claimed (section 10.3). Compare two backups across a codeplug write by the maker's software to map it |
| 22 | The state the maker's firmware leaves the flash chip in (`ADP`, block protection, a suspended operation); whether the write enable latch stays set after `C5h` | `ADP` set: the part starts in 4-byte mode (handled). Block protection: writes fail. A suspended operation: the part is not used until a power cycle | `board.flash`; the status registers through the debug link's flash info |
| 23 | The GPS module's rate and sentences; the antenna shared with the radio | no position | `board.gps_baud` |
| 24 | PHONE_DET, BAT_DET, IC611, the unlabeled expander output, Bluetooth, the accelerometer, the motor | none: not used | recorded for later work |
| 25 | `chainseq` and `dmr_keyup` are new shared code no radio has run; on the maker's table the arm's top stage keys through them (13.2) | DMR burst timing, the key-up shape | `tools/bench/pa_cal.py` on this radio once it is calibrated, with the SDR bench; `burstramp.py` and `pa_shape.py` analyze what it captured. Without the bench: another DMR radio decoding the bursts (bring-up 11.1), which shows that a burst decodes, not its shape |
| 26 | The split of the STM32 platform code is proven by images, not on a radio | none expected | One STM32F4 radio through the HIL tests |
| 27 | `aprs_tocall = "C"`, `order = 8`, CPS radio type 7 | registry conflicts with open pull requests | The maintainer checks the open pull requests |
| 28 | Not run where this was written: the C-SKY builds, `reuse lint`, the HIL test collection, the site build | CI may fail there first | CI |
| 29 | **The radio on with the knob off.** (a) A watchdog reset with SK1 held: no code of RadioFoundry's runs before the ROM bootloader, and the chain keeps the hold it had. The only path into the loader the firmware cannot guard: every software reset waits for SK1 (2.5). (b) A power-on with SK1 held, or with the first sector erased, and the knob turned off before RadioFoundry has run: the chain keeps its random power-up state. (c) A start task blocked for good with the knob off, a firmware fault (5.3) | the radio stays on until the pack is taken off (the PA stays off through R141) | Stated here (5.3), on the bring-up page and on the user page. Every reset the firmware causes itself clears the hold first. How often IC621 powers up with the hold set: several cold starts with SK1, knob off in the loader |
| 30 | Whether the ROM bootloader forces the last partial 32-byte flash word of a download | the last bytes of the image missing after an install | rfoundry and the web flasher fill the image to 32 bytes. A third-party DfuSe tool is not covered: the user page says to use ours |
| 31 | The second LO (3 x the reference) is not corrected by the plan's trim | 50 Hz of IF offset per ppm of reference error | Small against the IF filter; noted with the measurement of 1 |
| 32 | Polarities taken from documents: `DMR_SLEEP`, `A-PDN`, the AK2365A's select level while its reset is released (held high here), `VCOVCC_SW` (a drawing note, and the maker's rule of 7.2; its switch points not seen) | a chip that stays asleep; the wrong VCO | `board.hrc` (its read-back), the AK2365A's 0x0B read-back (bring-up stage 9; `board.ak` sends frames only and proves none of it), `board.pll` at the first start; for the VCO's switch points `board.pll` locked on receive channels on both sides of 479 MHz and, once a transmission is allowed, on carriers on both sides of 430 MHz. Each is one expander bit in `exp_cs7000p.h` |
| 33 | DAC code 0 with the buffer on is up to 0.2 V: 36 mV at the APC reference. The sequences disable the channel for "0"; a ramp's first codes pass through the buffer's floor | a PA floor that is not where the table expects it | PA4 with the channel off and at code 1, and the PA current with the loop closed and the transmit rail on: all at the first key-up, at drive 1 into the dummy load (bring-up stage 11). No receive-side write puts code 1 on the DAC |
| 34 | The timing of the expander and the bit-banged buses is computed, not measured; the time from power-on to the reset word; the chain's bit order as drawn (no read-back exists) | a wrong expander bit (it can be the APC loop); a marginal bus | A scope on PE7 to PE9 at power-on (the first frame decoded) and in normal running, and on the HR_C6000's bus clock; the outputs bit by bit (bring-up stage 4) |
| 35 | The reference's settle time after the synthesizer rail comes on (30 ms assumed) | a first transmission or measurement off frequency | The carrier against time after the rail, with 1 |
| 36 | PC13 and PC14 both outputs at times (2.7) | none expected: CMOS loads behind 1 k, 250 kHz | The maker's wiring; PC13 is an output only inside a frame |
| 37 | The maker's DMR burst timing fields (0x0EC-0x14C) are checked, not applied: the port's unmeasured key-up constants time the bursts on the maker's table too ([calibration-cs7000p.md](calibration-cs7000p.md) 8 items 3 and 4) | a burst early or late in its slot, splatter at its edges | An SDR capture of a burst against the slot |
| 38 | Wide FM under the maker's 4:0x35 = 0x20, the one value recorded (13.2; that page's 8 item 15) | under-deviation on a 25 kHz channel | Deviation on an SDR or by ear against another radio, 12.5 against 25 kHz |
| 39 | Trim B in 4:0x04 at every DMR receive entry on the arm's top stage: its effect under the port's 0x0C hold is not measured | DMR receive after arming `dmr` | DMR receive before and after arming `dmr`, the same signal (bring-up 11.1) |
| 40 | Another radio's valid table in the window or the record: no path replaces it (10.3). An override behind the identity gate is designed, not written | a radio that cannot get its own table back | The owner's decision |
| 41 | Going back to the maker's firmware needs the whole backup written back; no tool in the tree writes the whole chip (10.3) | the maker's data RadioFoundry overwrote after the claim | The owner's decision whether to build one |
| 42 | The arm itself: lifting it (`CS7000P_TX_ARM_REQUIRED` 0, 13.2) also lifts the test TX limit list's requirement | a radio that transmits on the maker's values without the owner | Not before every stage's checks have passed on the radio; the owner's decision |
