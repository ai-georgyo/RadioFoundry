// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// CS7000 M17 PLUS HR_C6000: its pins, its values and its callbacks (docs/spec/hardware-cs7000p.md 6). The chip, its
// ports and its slot engine are the shared code: the driver and the order of its set-ups on a board that feeds it a
// 450 kHz IF (drivers/modem/hrc6000, drivers/modem/hrc6000/if_host), the two ports and the three interrupt lines
// (platform/stm32/hrc), the burst runner (platform/stm32/dmr_keyup). Nothing here has run on a radio.
//
// What is this board's own:
//
// - the AK2365A's 450 kHz output is the chip's only receive input, and FM receive audio is the chip's own, on its
//   line out: there is no analog path from the discriminator to the speaker;
// - the reference oscillator's control voltage comes from one of the chip's modulation DACs, so that DAC is held on
//   while receiving (ref_dac_hold), in the measured coding or, for a receive test, the maker's (ref_coding);
// - the microphone amplifier is on another codec input than the driver's set-ups choose (CS7000P_E0_MIC);
// - the chip's reset is the MCU's, so it starts from its reset state at every start, asleep (an expander bit).
#include <stddef.h>
#include "cs7000p.h"
#include "config.h"
#include "pins.h"
#include "exp_cs7000p.h"
#include "rfops_cs7000p.h"
#include "hal_time.h"
#include "modem/hrc6000/hrc6000.h"
#include "modem/hrc6000/if_host/hrc6000_if_host.h"
#include "hrc/hrc_bus.h"
#include "hrc/dmr_glue.h"
#include "hrc/i2s_pcm.h"
#include "dmr_keyup/dmr_keyup.h"

// The codec input the microphone amplifier feeds, as a bit of 4:0xE0: MIC_P, by elimination from the chip's pin
// list. Unverified: the first thing to try on the bench when the transmitted audio is silent
#ifndef CS7000P_E0_MIC
#define CS7000P_E0_MIC		0x02
#endif
#define E0_LINEIN1		0x40u	// the input the driver's set-ups select: nothing is connected to it here

#define C6000_PWD_WAIT_MS	12	// at least 10 ms after the sleep line goes low before the first access
#define DMR_MIC_GAIN		7	// the other HR_C6000 radios' value (unverified here)
#define FM_MIC_GAIN		7	// the same
#define RX_GAIN_DB		2	// 4:0x37, as the tree's other IF-fed HR_C6000 (unverified here)
#define SAFE_GAIN		0x80u	// the DMR modulation pair while the calibration has none
#define MAKER_ADC_CTL		0x7Du	// 4:0x26 as the maker's program leaves it (calibration-cs7000p.md 5.13)
#define VOICE_SPI_PINS		(PIN_MASK(PIN_DMR_VCLK) | PIN_MASK(PIN_DMR_VMISO) | PIN_MASK(PIN_DMR_VMOSI))

// The configuration port on PB12-PB15 and the voice port on SPI1 (PB3-PB5, chip select PB6) at 48 MHz / 16 = 3 MHz
static const stm32_hrc_bus_cfg_t modem_cfg = {
	.port = PIN_PORT(PIN_DMR_CS),
	.cs = PIN_MASK(PIN_DMR_CS), .clk = PIN_MASK(PIN_DMR_CLK), .mosi = PIN_MASK(PIN_DMR_MOSI),
	.miso = PIN_MASK(PIN_DMR_MISO),
	.voice_spi = SPI1, .voice_br = 3,
	.voice_cs_port = PIN_PORT(PIN_DMR_VCS), .voice_cs = PIN_MASK(PIN_DMR_VCS),
};
static hrc_bus_t modem_bus;
static hrc_t modem;
static hrc6000_if_t host;

// --- The IF host's questions (the answers are the RF chain's: rfops_cs7000p.h) ---

// One first LO side for the whole band: the bench's switch
static bool lo_high(void *ctx, uint32_t hz)
{
	(void)ctx;
	(void)hz;
	return cs7000p_rf.lo_high;
}

static bool mod_pair(void *ctx, uint32_t hz, bool dmr, uint8_t *mod2, uint8_t *mod1)
{
	(void)ctx;
	return cs7000p_rf_mod_pair(&cs7000p_rf, hz, dmr, mod2, mod1);
}

static int8_t ref_offset(void *ctx, uint32_t hz)
{
	(void)ctx;
	(void)hz;
	return cs7000p_rf.ref_offset;
}

// The bench's choice of coding (chip_reg 3:5): the measured hold unless a receive test asks for the maker's state
static uint8_t ref_coding(void *ctx, uint8_t *adc_ctl)
{
	(void)ctx;
	uint8_t k = cs7000p_rf.ref_coding;
	*adc_ctl = k == CS7000P_REF_MAKER_ADC ? MAKER_ADC_CTL : 0u;
	return k == CS7000P_REF_HOLD ? HRC6000_IF_REF_MID : HRC6000_IF_REF_SIGNED;
}

// 4:0x04 in receive: the reference offset, as before the option. Whether 4:0x04 moves the held level under 0x0C is
// not measured, so nothing else goes there while receiving
static int8_t rx_tx_offset(void *ctx, uint32_t hz, bool dmr)
{
	(void)ctx;
	(void)hz;
	(void)dmr;
	return cs7000p_rf.ref_offset;
}

static const hrc6000_if_cfg_t host_cfg = {
	.dmr_mic_gain = DMR_MIC_GAIN,
	.rx_gain_db = RX_GAIN_DB,
	.fm_audio_chip = true,
	.ref_dac_hold = true,
	.e0_set = CS7000P_E0_MIC,
	.e0_clear = E0_LINEIN1,
	.lo_high = lo_high,
	.mod_pair = mod_pair,
	.ref_offset = ref_offset,
	.ref_coding = ref_coding,
	.rx_tx_offset = rx_tx_offset,
#if CONFIG_DMR
	.dmr_enter = stm32_hrc_dmr_enter,
	.dmr_leave = stm32_hrc_dmr_leave,
	.dmr_rx_restart = stm32_hrc_dmr_rx_restart,
#endif
	.safe_gain = SAFE_GAIN,
};

#if CONFIG_DMR
// The slot engine's RF side is the burst runner; the RF RX interrupt is not wired to the MCU and nothing uses it
static const stm32_hrc_dmr_board_t dmr_board = {
	.int_port = PIN_PORT(PIN_DMR_INT_TS),
	.rf_switch = dmr_keyup_rf_switch,
	.rf_counters = dmr_keyup_rf_counters,
	.tx_prepare = dmr_keyup_tx_prepare,
	.tx_wait_lock = dmr_keyup_tx_wait_lock,
	.tx_done = dmr_keyup_tx_done,
	.tx_end = cs7000p_radio_dmr_tx_end,
	.listen_split = false,
};
#endif

// The PCM stream is told which chip plays a beep through its prompt FIFO: this one while it is in FM receive, none
// otherwise (platform/stm32/hrc/i2s_pcm.h)
static void pcm_chip(void)
{
	stm32_i2s_pcm_fm_chip(cs7000p_c6000_fm_rx() ? &modem : NULL);
}

bool cs7000p_c6000_init(void)
{
	// Port B also has a line of the key matrix, which the key scan drives for a moment at a time: its lock around
	// this port's set-up, so neither writes the other's pin mode back
	cs7000p_lcdbus_lock();
	stm32_pin_af(PIN_PORT(PIN_DMR_VCLK), VOICE_SPI_PINS, GPIO_AF5_SPI1, false, GPIO_NOPULL);
	stm32_hrc_bus_init(&modem_cfg, &modem_bus);
	cs7000p_lcdbus_unlock();
	(void)cs7000p_exp_set(0, EXP_DMR_SLEEP);
	hal_delay_ms(C6000_PWD_WAIT_MS);
	hrc6000_attach(&modem, &modem_bus);
	bool up = hrc6000_if_init(&host, &modem, &host_cfg, 0);
#if CONFIG_DMR
	stm32_hrc_dmr_init(&modem, &dmr_board);
#endif
	pcm_chip();
	cs7000p_radio_speaker();
	return up;
}

void cs7000p_c6000_mode(bool dmr, uint32_t hz)
{
	if (dmr) {
		stm32_i2s_pcm_fm_chip(NULL);	// before the DMR set-up takes the chip out of FM mode
	}
	hrc6000_if_mode(&host, dmr, hz);
	pcm_chip();
	cs7000p_radio_speaker();	// the set-up opened the FM audio's feed-through: the routing's state again
}

void cs7000p_c6000_retune(uint32_t hz)
{
	if (hrc6000_if_retune(&host, hz)) {
		// The coding changed and the mode's whole set-up ran: its feed-through is open as after a mode change
		pcm_chip();
		cs7000p_radio_speaker();
	}
}

bool cs7000p_c6000_fm_tx(uint32_t hz, bool wide)
{
	uint32_t errors = host.errors;
	stm32_i2s_pcm_fm_chip(NULL);
	bool ok = hrc6000_if_fm_tx(&host, hz, wide, FM_MIC_GAIN);
	if (ok && host.errors != errors) {
		// A write of the set-up failed: not a modulator to key a carrier with
		hrc6000_if_fm_tx_end(&host);
		ok = false;
	}
	pcm_chip();
	cs7000p_radio_speaker();
	return ok;
}

void cs7000p_c6000_fm_tx_end(void)
{
	hrc6000_if_fm_tx_end(&host);
	pcm_chip();
	cs7000p_radio_speaker();
}

void cs7000p_c6000_rx_hold(void)
{
	hrc6000_if_rx_hold(&host);
}

bool cs7000p_c6000_fm_rx(void)
{
	return host.up && !host.dmr && !host.fm_tx;
}

void cs7000p_c6000_fm_audio(bool on)
{
	if (cs7000p_c6000_fm_rx()) {
		(void)hrc6000_fm_audio(&modem, on);
	}
}

void cs7000p_c6000_state(bool *up, uint8_t *r10, uint8_t *r25)
{
	*up = host.up;
	*r10 = host.r10;
	*r25 = host.r25;
}

// The debug link's CHIP_REG, chip 0: pages 1 and 4 (2 and 3 are the data and vocoder FIFOs, which a read would
// drain). Straight on the bus, each access one transaction; the driver's next set-up overwrites a write, and every
// receive entry writes 4:0x25, 0x47, 0x48 and 0x04 again (and 0x26 with chip_reg 3:5 = 2)
bool cs7000p_c6000_reg(uint8_t page, uint8_t reg, uint32_t *val, bool write)
{
	if (modem.bus == NULL || (page != HRC_PAGE_AUX && page != HRC_PAGE_SYS) || (write && *val > 0xFFu)) {
		return false;
	}
	uint8_t v = (uint8_t)*val;
	bool ok = (!write || hrc_write(&modem, page, reg, v)) && hrc_read(&modem, page, reg, &v);
	*val = v;
	return ok;
}
