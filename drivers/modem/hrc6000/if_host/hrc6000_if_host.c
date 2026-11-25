// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000 call order of a board that feeds the chip a 450 kHz IF (hrc6000_if_host.h). The init, mode and retune
// order is the one of boards/md9600/c6000_md9600.c; the receive hold and the codec input rewrite are for a board
// whose reference oscillator and microphone hang on pins the driver's set-ups do not serve.
#include <stddef.h>
#include "config.h"
#include "hrc6000_if_host.h"
#include "modem/hrc_common/hrc6000_dmr.h"

// 4:0x25, the DAC control word (docs/spec/hrc6000-registers.md 3.22, 5.1): bits 3 and 2 hand the two modulation DACs
// to the chip's TX slot timing, bits 5 and 4 let the MCU switch them. Every set-up of the driver that writes it
// leaves 0x0E, and with that the DAC on pin 75 is on only while the chip transmits. The hold is that value with
// bit 1 clear, 0x0C: measured on a radio whose reference oscillator hangs on that pin (the registers page has the
// table), the DAC is on while receiving exactly with bit 3 set and bits 5 and 1 clear. The manual's own word for it
// (bit 5 set, bit 3 clear) does nothing on that pin. Written whole, not read and modified.
//
// Its level while held is the two-point bias, 4:0x47 and 0x48 bits 1-0: ten bits, unsigned, over the DAC's whole
// range (the manual's 0 to 3.3 V). With the bias at 0, which is what an offset of 0 writes there, the DAC is on at
// 0 V and the oscillator as far off as with the DAC off: seen on the same radio. While the chip transmits the same
// DAC idles at mid-scale and 4:0x04 moves it, in steps of the same size. So the held level is mid-scale plus the
// offset: receive and transmit then sit on one frequency, and the board's offset trims both
#define DAC_CTL_SETUP	0x0Eu
#define DAC_CTL_HOLD	((uint8_t)(DAC_CTL_SETUP & ~0x02u))
#define REF_DAC_MID	512

// The other coding a board may ask for (HRC6000_IF_REF_SIGNED, docs/spec/modem-hrc6000.md 8.2): the set-up's own
// 0x0E kept while receiving, the bias read as ten bits signed around mid-scale. Inferred, not measured: under 0x0E
// the DAC was seen off with the bias at 0x200, which is -512 signed, 0 V. The voltage of the hold's 0x200 + d is then
// d: the same low byte, bit 9 the other way
#define REF_BITS	0x3FFu

// hrc6000_fm_tx_t.uhf: the deviation table's band
#define UHF_FROM_HZ	300000000u

// 4:0x10 as hrc6000_set_mode_fm_rx leaves it (the modulator in FM)
#define MODE_FM		0x80u

static bool count(hrc6000_if_t *h, bool ok)
{
	if (!ok) {
		h->errors++;
	}
	return ok;
}

// ref_coding's answer now: the measured hold and the driver's 4:0x26 without the callback or without the hold, and
// for an answer the interface does not know (its 4:0x26 too: an answer out of range changes nothing)
static void coding_ask(const hrc6000_if_t *h, uint8_t *coding, uint8_t *adc_ctl)
{
	const hrc6000_if_cfg_t *c = h->cfg;
	*coding = HRC6000_IF_REF_MID;
	*adc_ctl = 0;
	if (!c->ref_dac_hold || c->ref_coding == NULL) {
		return;
	}
	uint8_t adc = 0;
	uint8_t k = c->ref_coding(c->ctx, &adc);
	if (k == HRC6000_IF_REF_MID || k == HRC6000_IF_REF_SIGNED) {
		*coding = k;
		*adc_ctl = adc;
	}
}

// At the start of a full receive set-up: the coding every level and control word write uses until the next one
static void coding_take(hrc6000_if_t *h)
{
	coding_ask(h, &h->ref_coding, &h->adc_ctl);
}

static void ref_offset(hrc6000_if_t *h, uint32_t hz)
{
	const hrc6000_if_cfg_t *c = h->cfg;
	int8_t off = c->ref_offset != NULL ? c->ref_offset(c->ctx, hz) : 0;
	if (!c->ref_dac_hold) {
		count(h, hrc6000_set_ref_offset(h->dev, off, false));
		return;
	}
	int8_t r04 = c->rx_tx_offset != NULL ? c->rx_tx_offset(c->ctx, hz, h->dmr) : off;
	count(h, hrc_write(h->dev, 4, 0x04, (uint8_t)r04));
	if (h->ref_coding == HRC6000_IF_REF_SIGNED) {
		// The signed level and its control word back to back: the low byte (the same in both codings), 0x0E,
		// then the high bits. When the coding changes, the one write in between (0x25 before 0x48) is
		// unavoidable: bit 9 differs for the same voltage. Leaving this coding, the hold's 0x48 lands under 0x0E
		// in the same way, and the hold's 0x0C follows it (rx_setup)
		uint16_t level = (uint16_t)((uint16_t)off & REF_BITS);
		count(h, hrc_write(h->dev, 4, 0x47, (uint8_t)level));
		count(h, hrc_write(h->dev, 4, 0x25, DAC_CTL_SETUP));
		count(h, hrc_write(h->dev, 4, 0x48, (uint8_t)(level >> 8)));
	} else {
		// The held DAC's level, written here and not through the driver's call: that one puts the offset alone in
		// the bias first, which is the DAC near 0 V and a kick of several kHz to the reference at every receive
		// entry (a retune, each wake of the receiver's battery saving) until the level follows. The low byte and
		// the high bits change in two writes; the high bits last, so the level passes through values at most one
		// byte's range away
		uint16_t level = (uint16_t)(REF_DAC_MID + off);
		count(h, hrc_write(h->dev, 4, 0x47, (uint8_t)level));
		count(h, hrc_write(h->dev, 4, 0x48, (uint8_t)(level >> 8)));
	}
	if (h->adc_ctl != 0u) {
		// After the rows that write the driver's value (the FM receive rows, the DMR entry), at every entry
		count(h, hrc_write(h->dev, 4, 0x26, h->adc_ctl));
	}
}

// The receive I/Q order for hz. write: also on the chip, if the order changes (4:0x01 holds the same value in every
// mode, so the write is the one the DMR entry makes). Without it the next write of 4:0x01 carries the order
static void iq_follow(hrc6000_if_t *h, uint32_t hz, bool write)
{
	const hrc6000_if_cfg_t *c = h->cfg;
	if (c->lo_high == NULL) {
		return;
	}
	// The driver's flag and not the last frequency's answer: a board may change its LO side on the same frequency
	bool swap = c->lo_high(c->ctx, hz);
	if (swap == !h->dev->rx_iq_straight) {
		return;
	}
	count(h, hrc6000_rx_iq_swap(h->dev, swap, write));
}

// The control word of the coding the last full set-up took
static void hold(hrc6000_if_t *h)
{
	if (h->cfg->ref_dac_hold) {
		count(h, hrc_write(h->dev, 4, 0x25, h->ref_coding == HRC6000_IF_REF_SIGNED ? DAC_CTL_SETUP : DAC_CTL_HOLD));
	}
}

// The driver's set-ups choose the codec input of the boards it was written on. Read and modified: the other bits are
// the driver's, and the register reads back (hrc6000_tx_pair relies on it)
static void codec_inputs(hrc6000_if_t *h)
{
	const hrc6000_if_cfg_t *c = h->cfg;
	uint8_t e0;
	if ((c->e0_set | c->e0_clear) == 0) {
		return;
	}
	if (!count(h, hrc_read(h->dev, 4, 0xE0, &e0))) {
		return;
	}
	count(h, hrc_write(h->dev, 4, 0xE0, (uint8_t)((e0 & ~c->e0_clear) | c->e0_set)));
}

// What the chip holds after a set-up, for the board's diagnostics
static void read_back(hrc6000_if_t *h)
{
	count(h, hrc_read(h->dev, 4, 0x10, &h->r10));
	count(h, hrc_read(h->dev, 4, 0x25, &h->r25));
}

// FM receive. With the chip's own FM audio it stays in FM mode with the feed-through to the line out open. Without,
// the modulator goes back to DMR, where the codec plays PCM (beeps, prompts) on the line out
static void fm_rx_setup(hrc6000_if_t *h)
{
	count(h, hrc6000_set_mode_fm_rx(h->dev));
	count(h, hrc6000_fm_audio(h->dev, h->cfg->fm_audio_chip));
	if (!h->cfg->fm_audio_chip) {
		count(h, hrc6000_fm_rx_pcm(h->dev));
	}
}

// The chip's part of the DMR entry on hz
static void dmr_setup(hrc6000_if_t *h, uint32_t hz)
{
#if CONFIG_DMR
	const hrc6000_if_cfg_t *c = h->cfg;
	uint8_t mod2 = c->safe_gain, mod1 = c->safe_gain;
	if (c->mod_pair == NULL || !c->mod_pair(c->ctx, hz, true, &mod2, &mod1)) {
		mod2 = mod1 = c->safe_gain;
	}
	h->fm_tx = false;	// the entry ends an FM transmit set-up (4:0x60)
	if (c->fm_audio_chip) {
		count(h, hrc6000_fm_audio(h->dev, false));	// the line out is the codec's from here
	}
	iq_follow(h, hz, false);	// the entry writes 4:0x01
	count(h, hrc6000_dmr_enter(h->dev, mod2, mod1, c->dmr_mic_gain));
	codec_inputs(h);
#else
	(void)h;
	(void)hz;
#endif
}

// The whole receive set-up on hz of the mode h->dmr names, the reference's coding asked first: the mode's rows, then
// the level and the control word in that coding
static void rx_setup(hrc6000_if_t *h, uint32_t hz)
{
	coding_take(h);
	if (h->dmr) {
		dmr_setup(h, hz);
	} else {
		fm_rx_setup(h);
		iq_follow(h, hz, true);
	}
	// hz is the receive frequency from here, also for the end of an FM transmission
	ref_offset(h, hz);
	h->tuned_hz = hz;
	hold(h);
	read_back(h);
}

bool hrc6000_if_init(hrc6000_if_t *h, hrc_t *dev, const hrc6000_if_cfg_t *cfg, uint32_t hz)
{
	*h = (hrc6000_if_t){ .dev = dev, .cfg = cfg, .tuned_hz = hz };
	coding_take(h);
	count(h, hrc6000_power_on_init(dev, cfg->dmr_mic_gain));
	fm_rx_setup(h);
	count(h, hrc6000_set_rx_gain(dev, cfg->rx_gain_db));
	count(h, hrc6000_tones_off(dev));	// with the init's 0x80 an IF-fed chip never syncs on DMR
	if (cfg->lo_high != NULL) {
		// Unlike iq_follow, written whatever the order was: the first time the board's answer reaches the chip
		count(h, hrc6000_rx_iq_swap(dev, cfg->lo_high(cfg->ctx, hz), true));
	}
	// The level before the hold opens the DAC, so the oscillator's control voltage starts where it will stay
	ref_offset(h, hz);
	hold(h);
	read_back(h);
	// A chip with any step of its set-up missing is not one to enter DMR or transmit FM with. Nor is one that does not
	// give back the mode the set-up wrote: a board's bus may have no way to report a failure, and a chip that is
	// not there then reads as the idle level of its data line
	uint8_t mode = cfg->fm_audio_chip ? MODE_FM : dev->chip->dmr_mode(false);
	h->up = h->errors == 0 && h->r10 == mode;
	return h->up;
}

void hrc6000_if_mode(hrc6000_if_t *h, bool dmr, uint32_t hz)
{
#if !CONFIG_DMR
	dmr = false;
#endif
	if (!h->up || dmr == h->dmr) {
		return;
	}
	const hrc6000_if_cfg_t *c = h->cfg;
	h->dmr = dmr;
	if (dmr) {
		rx_setup(h, hz);
		if (c->dmr_enter != NULL) {
			c->dmr_enter();	// the engine on and the receiver restarted, on a chip that is ready
		}
	} else {
		if (c->dmr_leave != NULL) {
			c->dmr_leave();	// the engine off while the chip is still in DMR
		}
		rx_setup(h, hz);
	}
}

bool hrc6000_if_retune(hrc6000_if_t *h, uint32_t hz)
{
	if (!h->up) {
		return false;
	}
	if (h->fm_tx) {
		// The transmit set-up holds the reference offset in its transmit form: hrc6000_if_fm_tx_end applies hz
		h->tuned_hz = hz;
		return false;
	}
	const hrc6000_if_cfg_t *c = h->cfg;
	uint8_t coding, adc_ctl;
	coding_ask(h, &coding, &adc_ctl);
	if (coding != h->ref_coding || adc_ctl != h->adc_ctl) {
		// One coding per receive state: never this state's level under the other's control word. The whole set-up
		// also gives 4:0x26 the driver's value back. In DMR the engine goes off and on around it, as at an entry
		if (h->dmr && c->dmr_leave != NULL) {
			c->dmr_leave();
		}
		rx_setup(h, hz);
		if (h->dmr && c->dmr_enter != NULL) {
			c->dmr_enter();
		}
		return true;
	}
	ref_offset(h, hz);
	iq_follow(h, hz, true);
	if (h->dmr && hz != h->tuned_hz) {
		// New channel: its modulation pair, and the receiver searches again
		uint8_t mod2 = c->safe_gain, mod1 = c->safe_gain;
		if (c->mod_pair == NULL || !c->mod_pair(c->ctx, hz, true, &mod2, &mod1)) {
			mod2 = mod1 = c->safe_gain;
		}
		count(h, hrc_set_mod_gain(h->dev, mod2, mod1));
		if (c->dmr_rx_restart != NULL) {
			c->dmr_rx_restart();
		}
	}
	h->tuned_hz = hz;
	hold(h);	// every receive entry passes here: one write keeps the reference where a set-up elsewhere left it
	return false;
}

bool hrc6000_if_fm_tx(hrc6000_if_t *h, uint32_t hz, bool wide, uint8_t mic_gain)
{
	if (!h->up || h->dmr) {
		return false;
	}
	const hrc6000_if_cfg_t *c = h->cfg;
	uint8_t mod2 = 0, mod1 = 0;
	if (c->mod_pair == NULL || !c->mod_pair(c->ctx, hz, false, &mod2, &mod1)) {
		return false;	// a guessed pair is a guessed deviation
	}
	hrc6000_fm_tx_t tx = {
		.uhf = hz > UHF_FROM_HZ,
		.wide = wide,
		.tone = HRC6000_TONE_NONE,
		.mod2 = mod2,
		.mod1 = mod1,
		.ref_offset = c->ref_offset != NULL ? c->ref_offset(c->ctx, hz) : 0,
		.mic_gain = mic_gain,
	};
	h->fm_tx = true;
	if (c->fm_audio_chip) {
		count(h, hrc6000_fm_audio(h->dev, false));	// open only in FM receive
	}
	count(h, hrc6000_set_mode_fm_tx(h->dev, &tx));	// leaves 4:0x25 as the driver's transmit set-up wants it
	codec_inputs(h);
	read_back(h);
	return true;
}

void hrc6000_if_fm_tx_end(hrc6000_if_t *h)
{
	if (!h->up || !h->fm_tx) {
		return;
	}
	h->fm_tx = false;
	coding_take(h);
	fm_rx_setup(h);
	count(h, hrc6000_tones_off(h->dev));
	// The level goes in under the transmit set-up's 0x0E: the hold right after it, before the I/Q order's write,
	// so the level and its control word disagree for no more than the one write between
	ref_offset(h, h->tuned_hz);
	hold(h);
	iq_follow(h, h->tuned_hz, true);
	read_back(h);
}

// The one function a burst sequence may call from interrupt level: a single register write (atomic on the bus), in
// the coding the last set-up took (no callback here), and no state but the error count
void hrc6000_if_rx_hold(hrc6000_if_t *h)
{
	if (!h->up || h->fm_tx) {
		return;
	}
	hold(h);
}
