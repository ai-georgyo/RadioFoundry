<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Connect Systems CS7000 M17 PLUS hardware

The CS7000 M17 PLUS is a UHF handheld: STM32H743VI, HR_C6000, a SKY72310 synthesizer with discrete VCOs, an AK2365A
IF chip, a discrete PA with a current loop, a 32 MB SPI NOR flash, a GPS module, and a 24-bit output expander that
carries every rail enable. The port is `boards/cs7000p` on `platform/stm32h7`.

Status: **nothing in this file has been run on a radio: all of it is untested.** The image builds, its host tests
pass and its static stack check passes. The order to check things on a radio, and the knob to change when a check
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
| Any transmission: FM, DMR data, the hotspot, Tier III registration | the calibration verdict in `tx()` and in the burst key-up (section 13) | the model tables are filled from bench measurements |
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

Not known from any of them, and so not in the code: the first LO's side, the HR_C6000 registers that set the
reference oscillator's control voltage, every calibration value, the display controller, the channel selector's
code, and where the maker's firmware keeps anything on the data flash (the order of the six function keys was in
none of them either; it has since been read on a radio, 9.4). Section
16 lists all of it.

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
| ROM bootloader from the running image | the debug link's `REBOOT` to the ROM (`rfoundry_debug.py reboot --rom-dfu`) | allowed (`CS7000P_SOFT_ROM_ENTRY`, 1 by default) only with ROM loader version 0x91 (V9.1) or with the selector's PB10 contact open (below); the one radio read so far has loader 0x92 (V9.2), so there a selector position that grounds PB10 is refused and the user turns the selector four detents either way. The entry itself has not been tried on a radio. The rule is `cs7000p_rom_entry_ok()` in `romentry_cs7000p.h` (`cs7000pPinsTest`). The platform asks it before the debug link answers, so a refusal is the refused status with reason 5 ([debuglink.md](debuglink.md) 3.11), not an OK followed by nothing. `rfoundry flash` and the web flasher send the same request to a radio that runs RadioFoundry, so an update needs no key (`[flash.bootloader] restart` in `board.toml`; [release.md](../dev/release.md#the-restart-into-the-bootloader)). On a refusal they show the board's hint (`restart_hint`: turn the channel selector four clicks either way and try again) and go on with SK1 at power-on. Untested on a radio, through the tools as well |

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
| 8 | `EXP_VCO_RX` | VCOVCC_SW | the receive VCO is powered; 0: the transmit VCO (the drawing's note "T:0V R:3V") |
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
level while its reset is released (open question 32). Each is one bit to flip in `exp_cs7000p.h`.

### 4.2 Reset words and the order at start-up

| Word | Value | Meaning |
|---|---|---|
| `EXP_RESET_WORD` | 0x2A0181 | bits 0, 7, 8, 17, 19, 21: both chips parked, the receive VCO selected, both speakers disconnected, everything else off. **No transmit bit, no rail, and the hold clear** |
| `EXP_TX_BITS` | bits 3 and 9 | the two bits that can make RF |
| `exp_rf_safe_word(shadow)` | shadow without the transmit bits, with `EXP_VCO_RX` | what `board_rf_safe()` writes |

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
| VCOs | Q3 (receive), Q4 (transmit), one tuning line | `EXP_VCO_RX` powers one or the other, never both. Which is which rests on the drawing's note "T:0V R:3V" |
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
| First LO side | **low**: LO = f - 49.95 MHz, receive VCO 350.05 to 477.05 MHz. `CS7000P_LO_HIGH` 0 | **open**: only the VCO tanks' part values point to it. A bench switch flips it together with the I/Q order (section 14, 3:3). Open question 2 |
| Transmit | the VCO on the carrier | read: no multiplier |
| Reference divider | 2 for the whole band (`sky72310_pick_ref_div()`): 8.4 MHz at the phase detector, N 41.7 to 61.0 (receive VCO 350.05 to 477.05 MHz, transmit 400 to 512 MHz; up to 68.7 with the LO above the signal, 449.95 to 576.95 MHz), step 32.04 Hz | datasheet |
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
- the transmit rail is off whenever the receive VCO is selected;
- the DAC never carries a tuning value while the loop is closed;
- every error path, a refused write included, ends in the safe state.

The hardware bound: at DAC full scale the loop's reference is 0.58 V, so the loop limits the PA current. Nothing
limits reverse power. A first transmission needs a dummy load, a current-limited supply and the lowest drive first.

### 7.6 PC8: sub-audio out, and part of the reference

PC8 is TIM3 CH3 behind a 2-pole RC filter, summed into the reference oscillator's control amplifier with gain
0.082: its whole swing moves the control voltage by 0.27 V. The net has no pull resistor.

- From `board_early_init()` on, PC8 carries a PWM at **50 % duty, 93.75 kHz** (96 MHz / 1024), and is never
  released. The radio's frequency calibration holds for that level only (open question 4).
- The sub-audio generator (`subtone_cs7000p.c`) sets one duty value per ADC scan tick, 2,400 a second:
  512 / 1024 plus the sample scaled by `CS7000P_CTCSS_DEPTH`, clamped so it never reaches 0 or the period
  (`subpwm_cs7000p.h`, `cs7000pSubPwmTest`).
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
| 2 | Safe state: DAC 0, `EXP_APC_LOOP` off, `EXP_TX_RAIL` off | the DAC is about to become a tuning voltage: the loop must be open first |
| 3 | `EXP_SYNTH_RAIL` on (settle if it was off), `EXP_VCO_RX` on | |
| 4 | Synthesizer to f - IF; wait for lock, at most 5 ms. Unlocked is counted and receive goes on | |
| 5 | `EXP_RX_RAIL` on; 1 ms | the tripler is the AK2365A's LO |
| 6 | AK2365A set-up: software reset, control 1 (the operating mode), 500 us, control 2 (the AGC setting and the calibration's start), control 3 to 5; then 3 ms (calibration 1.3 ms, settle 1.5 ms). In DMR the narrow filter and level are used whatever the channel's width says | the data sheet's calibration procedure: the circuits it runs on are up 500 us after the mode is set |
| 7 | DAC: the front-end tuning value, from the calibration table or `CS7000P_RX_TUNE_NOMINAL` (2048), clamped to codes 250 to 3850 | the buffer's range: 0.2 V from either rail |
| 8 | `EXP_AUDIO_RAIL` on | the sub-audio filter |
| 9 | The HR_C6000's mode and retune. FM: the chip plays its own FM audio. DMR: the pair, the engine, a receiver restart on a new channel. The reference hold in both | |
| 10 | The sub-audio detector reset, unless the channel keeps it | |

`rx()` during a transmission ends its parts first (the tone, the burst runner), and the sequencer brings the RF down
in order inside the receive set-up.

### 7.9 FM transmit

| Step | Action | Reason |
|---|---|---|
| 0 | Refuse unless calibrated or a bench drive override is in force (section 13); refuse outside the two transmit ranges; refuse below the critical battery voltage; refuse until the reference has settled | never key without calibration |
| 1 | DAC 0 | it held the tuning voltage |
| 2 | `EXP_RX_RAIL` off, sub-audio detector paused | receiver and tripler off |
| 3 | The HR_C6000's FM transmit set-up with the pair and reference offset, then the codec input bits. Then `EXP_MIC_POWER` and `EXP_MIC_INT` on | modulation ready before RF. Under a bench override with no pair known the carrier is unmodulated. A failed write of the transmit set-up counts as a refusal (`why=9`) |
| 4 | `EXP_VCO_RX` off; synthesizer to the carrier; wait for lock. **Unlocked: back to receive, refused** | |
| 5 | `EXP_TX_RAIL` on; 500 us | pre-drivers, LO, antenna switch |
| 6 | DAC at the floor, `EXP_APC_LOOP` on, then a ramp of 8 steps of 250 us to the drive | the loop closes on a zero reference |
| 7 | Sub-audio generator on | |
| End | DCS turn-off, ramp down, `EXP_APC_LOOP` off, `EXP_TX_RAIL` off, the whole receive sequence of the RF chain, then the microphone off and the HR_C6000 back to FM receive, its retune and the sub-audio detector | the modem leaves its transmit set-up only after the PA is down: its DAC is the reference oscillator's control |

### 7.10 DMR transmit

`tx()` arms the shared burst runner (`platform/stm32/dmr_keyup`) with the same refusal. Per burst, at interrupt
level: receiver off, transmit VCO, synthesizer words, `EXP_TX_RAIL` with the DAC off; the loop closed with the DAC
still off; then the drive points; the PA drop; back to the receive LO. A drive point before the loop point would be
refused by the interlock. The PA point of a burst is refused when the lock pin does not read locked three times in a
row. After each burst sequence the reference hold is written again. `tx_off()` after bursts runs the whole receive
set-up (the last switch back has none for the AK2365A); the modem glue's `dmr_tx_end` keeps the receiver as that
switch left it. A refused re-arm behind an armed transmission sets the receiver up again at once.

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
| RSSI | AK2365A RSSI on PB0 | the datasheet's two typical points at the chip's input (0.3 V at -100 dBm, 2.0 V at -30 dBm) and `CS7000P_FE_GAIN_DB` 0. Not an absolute level until calibrated |
| Noise | AK2365A noise rectifier (net BUSY) on PB1, raw >> 4 | larger = more noise; 255 while not receiving. There is no digital "busy" line |
| Tone | the sub-audio detector | |
| `carrier` | RSSI at or over -118 dBm while receiving | a placeholder (the figure of `boards/md380/radio_md380.c`): on this board's unmeasured RSSI scale every reading is over it, so it is true whenever the receiver is on |

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
| Open-firmware "EEPROM" image | 0x000000 | 64 KB | |
| Settings ring | 0x011000 | 8 sectors | foreign-guarded |
| Journal | 0x019000 | 2 sectors | foreign-guarded |
| **Board sector** | 0x01E000 | 1 sector | reserved for a per-unit calibration record (section 13.3). In the hole no CPS address maps to. Written by nothing yet |
| Open-firmware "flash" image | 0x020000 to 0xE00000 | | |
| Native codeplug | 0xE00000 | 1 MB | |
| Staging | 0xF00000 | 256 KB | |
| 0x1000000 to 0x1FFFFFF | not in the map | 16 MB | read for the backup only |
| CPS calibration window | none | | no CPS calibration format is known for this radio |

### 10.3 The maker's data, and the claim

The maker's firmware has used this chip. **No region of the data flash is known to hold factory data**: nothing in
any allowed source says where the maker's firmware keeps its codeplug, its calibration, its fonts or its voice
data. So the registry lists no protected region (`[[flash.protect]]` has no entry), and the port protects the chip
another way:

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
- After the claim the core may write anywhere in the map. Whatever the maker's firmware kept there is then at risk,
  and the backup is the only copy.
- A valid record of RadioFoundry's proves one claim. A radio that went back to the maker's firmware and returns is
  "ours" at the first start if a record survived: the backup is then the owner's job again.
- The board sector is written only when it is blank or carries the tree's own magic. The maker's calibration,
  wherever it is, is never read, interpreted or written.

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

None is known and none is guessed. The maker's alignment data is somewhere on the data flash in a format no
allowed source describes. The port has model tables in source (`rfcal_cs7000p.c`), all unset:

| Value | Now | Supplied by |
|---|---|---|
| PA drive per power level and frequency | 9 points, 400 to 520 MHz in 15 MHz steps, all `CS7000P_CAL_UNSET` | [power-calibration.md](../testing/power-calibration.md) with `tools/power_cal.py --allow-rf`, a wattmeter and a current-limited supply |
| PA floor (the largest drive with no output) | 0 | the same procedure's zero point |
| FM and DMR modulation pairs (4:0x45 / 0x46) | unset | deviation on a service monitor or the SDR bench ([dmr-bench.md](../testing/dmr-bench.md)) |
| Reference offset and bias | 0, unverified | a frequency counter on the carrier, then the reference trim for the unit |
| Front-end tuning DAC per frequency | mid scale | a sensitivity sweep with `chip_reg` 3:0 |
| Squelch thresholds, RSSI offset | the plain mappings | a signal generator |
| DMR key-up constants | a fixed lead | `tools/bench/pa_cal.py`, after the PA rows, the DMR modulation pair and the reference offset are built in (it stops while the view's `tx_drive` is 0), with `CS7000P_BENCH_DMR_TX=1` |

### 13.2 What is refused, and the bench override

`cs7000p_cal_tx_ok()` is true only when the PA row for the level, the modulation pair for the mode and the
reference offset are all set. With the tables as shipped it is false everywhere: `tx()` returns false in FM and DMR,
the hotspot cannot key, and `board.cal=none` says why. `board.tx_refused` counts the refusals and gives the last
reason as `why=`:

| `why` | Reason |
|---|---|
| 0 | nothing refused yet |
| 1 | outside the transmit ranges: outside 400.0-405.9875 and 406.1125-512.0 MHz |
| 2 | no calibration and no bench drive armed |
| 3 | a bench drive armed, but no debug link in the build |
| 4 | a bench drive armed, but no test TX limit lists this frequency |
| 5 | a bench drive armed, but DMR without `CS7000P_BENCH_DMR_TX` |
| 6 | a bench drive armed more than 60 s ago |
| 7 | the battery under its critical level, or without a reading (the ADC scan not running) |
| 8 | the reference not settled |
| 9 | the modem: not up, its FM transmit set-up refused, or the burst runner refused the arm |
| 10 | the synthesizer not locked on the carrier |
| 11 | a line or DAC write refused or failed (the interlock, a safe request from outside) |
| 12 | the power's off state (5.4): the radio let go of the supply and the knob still powers it. Checked before everything else: an armed bench drive is neither used nor dropped |

The only way the firmware keys an uncalibrated radio is the debug link's drive override (`tx_drive`, 1 to 4095).
(A raw SoC `CHIP_REG` write to DAC1 or GPIOE can key the PA outside every check here, 4.3: the bench must not make
one.) Arming the override is not enough: the next `tx()` honors it only when all of these hold.

| Condition | Why |
|---|---|
| A test TX limit is set as a frequency list, and the frequency is on it | the bench harness named this frequency. A PTT press outside a test run meets no list |
| The mode is FM, or the build has `CS7000P_BENCH_DMR_TX=1` | the burst key-up has not run on a radio |
| At most 60 s since the override was armed | an armed override does not wait for a transmission that never came |
| The debug link is built | without it there is neither a limit nor an override |

The override is dropped at the transmission's end, at a receive set-up that ends a transmission, and when it
expires. A key-up refused in the off state does not look at it: it expires at the first key-up after that state, if
60 s have passed since it was armed. On a calibrated radio it is used under the same four conditions; without them
the level's own drive is used. A drive at or under the floor, or of 0, is refused. `tx_drive_top` is 0 while the
tables are unset, so the PA drive sweep is refused on an uncalibrated radio: only the plain override keys it. With
no modulation pair known the carrier is unmodulated.
Everything in section 7.5 about the first transmission applies.

### 13.3 The planned unit record

Per-unit values will go in a record of the tree's own, not in a vendor format: the magic `RFC1`, a version, a
length, a CRC-16, then byte rows read with `drivers/cal/calblock`, in the board sector at 0x01E000
(`CS7000P_STORE_CAL_ADDR`). A unit record will win over the model table. This change reserves the sector and
states the form. The reader and the writer (a bench tool over a debug link board-record command) come with the
first measured radio; the field widths are fixed then.

---

## 14. `chip_reg` numbering

Bench access through the debug link ([debuglink.md](debuglink.md) 3.24):

| Chip | Page / register | Access |
|---|---|---|
| 0 | HR_C6000, pages 1 and 4 (2 and 3 are FIFOs a read would drain). Every receive entry writes 4:0x25, 0x47, 0x48 and 0x04 again | read / write; a write is followed by the register read back |
| 1 | SKY72310, page 0, registers 0 to 14, values to 0xFFF. The driver's next tune overwrites the divider registers | write only (the value written is returned) |
| 2 | AK2365A, 0x01 to 0x04, 0x0B and 0x0C (the registers the data sheet names; it forbids access to the others). Page 0 reads with the data line pulled down, page 1 pulled up: a value that is the same both ways came from the chip. Where the read bits fall is from the data sheet's drawing and unverified | read / write; a write is followed by a read back |
| 3 | The board, page 0. 0: the receive tuning DAC, through the interlock: a write of 1 to 4095 puts the value (clamped to 250 to 3850) on the DAC now, if the radio is receiving, and keeps it for later receive set-ups; 0 gives the table's value back at the next one; refused while a transmit bit is set or an FM transmission is up; a read gives the DAC's code now. 1: the expander shadow, read only. 2: PC8's idle duty, 0 to 1024 (0 and 1024 hold the line low or high: bench only); refused while a tone is sent. 3: the LO side (0 low, 1 high: retunes and sets the I/Q order). 4: the reference offset, a signed byte (two's complement in the low byte). 3 and 4 take effect at the radio task's next status poll (a receive set-up) | read / write |
| 0xFF (`soc`) | The SoC's own registers ([debuglink.md](debuglink.md) 3.24). **Never DAC1 (0x40007400-0x400077FF) or GPIOE (0x58021000-0x580213FF)**: a write there bypasses the interlock (4.3) and can key the PA | read / write; a write is refused only while transmitting |

---

## 15. Debug link view keys

`view_cs7000p.c`; each is `board.<key>` in view section 28 ([debuglink.md](debuglink.md) 3.7). The diagnostics page shows the same
readings.

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
| `flash` | JEDEC ID, size, `addr4`: `EF4019 size=0x2000000 addr4=1` |
| `store` | the board sector: `blank` / `ours` / `foreign` / `unread` (no chip, or the read failed; asked again at the next request). Read once, at the first request |
| `cal` | `none` / `model` |
| `tx_refused` | the count and `why=`, the last reason (13.2) |
| `dac` | the shared DAC's code now (0 = off) and `refused=`, the refused DAC writes and refused expander writes together |
| `lo_side` | `low` / `high` |
| `pll` | `locked` / `unlocked` (the lock pin read when the view is asked, not the last tune's result), `r=` the reference divider, `vco_hz=`, `unlocks=` |
| `ak` | `ok` / `fail`: whether the IF receiver's last set-up frames were sent. No read-back: `ok` does not show that the chip answered (the 0x0B read-back through `chip_reg` 2 does, bring-up stage 9) |
| `rssi_raw`, `noise_raw`, `qt_raw` | the three receiver ranks |
| `rx_tune`, `ref_offset`, `pc8_duty` | the tuning DAC value, the reference offset, PC8's duty |
| `hrc` | init ok (`0` / `1`), `r10=` and `r25=`: 4:0x10 and 4:0x25 as read back |
| `i2s` | running, `dir=` (`ok` / `swapped` / `busy` / `none` / `unknown`), `ioswp=`, `edges=` the two probe counts, `und=`, `ovr=`. Only in a build with DMR |
| `gps_baud` | the rate the module answered at; 0: not heard, or built without GPS |
| `adc_restarts` | overrun recoveries of the ADC scan; 0 is expected |

`boards/cs7000p/hil/test_cs7000p.py` reads them on a radio. It never transmits.

---

## 16. Open questions

What no allowed source settles, the risks, and what resolves each. The numbers are the ones the bring-up page and
the source comments use.

| # | Question or risk | Where it bites | Resolved by |
|---|---|---|---|
| 1 | **Reference oscillator control.** Which HR_C6000 registers set pin 75's level in receive and in transmit, the bias's coding, the level that is on frequency. The chip's manual contradicts itself on which DAC is which | every mode: the receive and transmit frequency, and the offset between them | The carrier on a frequency counter (bench override, dummy load) and the receive LO's leak while stepping 4:0x47 / 0x48, 0x04, 0x25 through `chip_reg` chip 0; the voltage at the oscillator's control pin if a test point is found |
| 2 | **First LO side**: low, seen on one radio (it locks and receives there; the high side is out of the VCO's range at 433 MHz). With it the I/Q order of 4:0x01 bit 6 is right: the same radio syncs to DMR and decodes calls | answered for the low side | A signal generator on channel; flip `chip_reg` 3:3. DMR sync needs the matching I/Q order |
| 3 | Synthesizer: the phase detector gain for this loop filter, lock time, the lock pin's behavior in fractional mode, the lock range over 350 to 512 MHz (450 to 577 MHz with the LO above the signal; charge pump 0.5 to 3.9 V) | lock failures, slow key-up, the band edges | `board.pll` over a frequency sweep; the tuning line at test point TP1 |
| 4 | PC8's idle level (50 % chosen) and PWM ripple on the reference | carrier offset, hum on the carrier | The carrier with PC8 at 0 %, 50 %, 100 % (`chip_reg` 3:2); the spectrum close in |
| 5 | PA: drive against power, the floor, a cliff under it, whether 4 W is reachable inside the DAC's range, no reverse-power protection | the first transmission | [power-calibration.md](../testing/power-calibration.md) from a low drive, on a current-limited supply, into a dummy load, the PA current watched |
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
| 18 | Software entry to the ROM loader from a running image: the supply is already configured; the loader's USART pins are PA2 (an op-amp output here) and PB10 (a selector contact to ground) on every loader but 0x91 (0x92: not stated), PA9 and PB14 (the chip's data output) on 0x91. Whether the HR_C6000 releases its data output while deselected. Whether the backup register keeps the request with the RTC's kernel clock off. (A plain software reset with SK1 held would reach the same loader on the same pins: every one waits for SK1's release first, 2.5) | updates without the key | On by default under the rule (`board_rom_entry_ok()`, `romentry_cs7000p.h`): loader 0x91, or PB10 high. To check on a radio: that the entry reaches the loader at all, PB10 and PB12 as section 4.2 step 5. SK1 at power-on stays available |
| 19 | The ROM's Leave into the image (the stack pointer rule, peripherals as the loader left them); a normal power-on with the first sector erased starts nothing (2.5) | the radio needs a power cycle after flashing; an install cut short needs the loader and the same install | Observe. The tools already say to switch the radio off and on, and write the vector table last |
| 20 | **Read Unprotect erases the maker's bootloader and programs.** Whether anything unit-specific is in the MCU's flash is not stated (the maker's recovery reloads a bootloader, programs, a default codeplug and a language file, which suggests not) | a radio that cannot go back to stock, or lost calibration, before RadioFoundry is of use | All before the first RadioFoundry install: the owner saves the codeplug with the maker's CPS and notes Radio Info while the stock firmware runs (and saves the tuning data, if the maker's tuner software can export it: no document says), runs the maker's documented recovery once (it loads the maker's default codeplug), confirms the radio works on the maker's firmware, and writes the saved codeplug back |
| 21 | The maker's data in the 32 MB flash (codeplug, calibration, fonts): places unknown | a claim of the settings region can overwrite it | A full backup first: on every unit the region is foreign until claimed (section 10.3). Compare two backups across a codeplug write by the maker's software to map it |
| 22 | The state the maker's firmware leaves the flash chip in (`ADP`, block protection, a suspended operation); whether the write enable latch stays set after `C5h` | `ADP` set: the part starts in 4-byte mode (handled). Block protection: writes fail. A suspended operation: the part is not used until a power cycle | `board.flash`; the status registers through the debug link's flash info |
| 23 | The GPS module's rate and sentences; the antenna shared with the radio | no position | `board.gps_baud` |
| 24 | PHONE_DET, BAT_DET, IC611, the unlabeled expander output, Bluetooth, the accelerometer, the motor | none: not used | recorded for later work |
| 25 | `chainseq` and `dmr_keyup` are new shared code no radio has run | DMR burst timing, the key-up shape | `tools/bench/pa_cal.py` on this radio once it is calibrated, with the SDR bench; `burstramp.py` and `pa_shape.py` analyze what it captured |
| 26 | The split of the STM32 platform code is proven by images, not on a radio | none expected | One STM32F4 radio through the HIL tests |
| 27 | `aprs_tocall = "C"`, `order = 8`, CPS radio type 7 | registry conflicts with open pull requests | The maintainer checks the open pull requests |
| 28 | Not run where this was written: the C-SKY builds, `reuse lint`, the HIL test collection, the site build | CI may fail there first | CI |
| 29 | **The radio on with the knob off.** (a) A watchdog reset with SK1 held: no code of RadioFoundry's runs before the ROM bootloader, and the chain keeps the hold it had. The only path into the loader the firmware cannot guard: every software reset waits for SK1 (2.5). (b) A power-on with SK1 held, or with the first sector erased, and the knob turned off before RadioFoundry has run: the chain keeps its random power-up state. (c) A start task blocked for good with the knob off, a firmware fault (5.3) | the radio stays on until the pack is taken off (the PA stays off through R141) | Stated here (5.3), on the bring-up page and on the user page. Every reset the firmware causes itself clears the hold first. How often IC621 powers up with the hold set: several cold starts with SK1, knob off in the loader |
| 30 | Whether the ROM bootloader forces the last partial 32-byte flash word of a download | the last bytes of the image missing after an install | rfoundry and the web flasher fill the image to 32 bytes. A third-party DfuSe tool is not covered: the user page says to use ours |
| 31 | The second LO (3 x the reference) is not corrected by the plan's trim | 50 Hz of IF offset per ppm of reference error | Small against the IF filter; noted with the measurement of 1 |
| 32 | Polarities taken from documents: `DMR_SLEEP`, `A-PDN`, the AK2365A's select level while its reset is released (held high here), `VCOVCC_SW` (a drawing note only) | a chip that stays asleep; the wrong VCO | `board.hrc` (its read-back), the AK2365A's 0x0B read-back (bring-up stage 9; `board.ak` sends frames only and proves none of it), `board.pll` at the first start. Each is one expander bit in `exp_cs7000p.h` |
| 33 | DAC code 0 with the buffer on is up to 0.2 V: 36 mV at the APC reference. The sequences disable the channel for "0"; a ramp's first codes pass through the buffer's floor | a PA floor that is not where the table expects it | PA4 with the channel off and at code 1, and the PA current with the loop closed and the transmit rail on: all at the first key-up, at drive 1 into the dummy load (bring-up stage 11). No receive-side write puts code 1 on the DAC |
| 34 | The timing of the expander and the bit-banged buses is computed, not measured; the time from power-on to the reset word; the chain's bit order as drawn (no read-back exists) | a wrong expander bit (it can be the APC loop); a marginal bus | A scope on PE7 to PE9 at power-on (the first frame decoded) and in normal running, and on the HR_C6000's bus clock; the outputs bit by bit (bring-up stage 4) |
| 35 | The reference's settle time after the synthesizer rail comes on (30 ms assumed) | a first transmission or measurement off frequency | The carrier against time after the rail, with 1 |
| 36 | PC13 and PC14 both outputs at times (2.7) | none expected: CMOS loads behind 1 k, 250 kHz | The maker's wiring; PC13 is an output only inside a frame |
